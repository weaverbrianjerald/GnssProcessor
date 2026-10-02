# PRM: the parameter registry and the carry contract (one declared property set per parameter kind, one traversal over every carried reference)

**STATUS**

- **id:** PRM
- **topic:** network-vehicle
- **status:** open
- **summary:** PRE-REGISTERED 2026-09-29 (the author's question after cards CTU and CTU2: does the network need parameter objects whose properties a human can read, with a clear estimator hierarchy). The PPP path has such a registry (`StateManager` owns the layout, `StateKeySchema` every key format); the network path does not: a border parameter is a (kind, satellite, signal) tuple whose properties are re-derived at every use (its process noise at four sites plus card CTU's mirror, its variance floor at two, the response to a slip, a gap or a handover on four paths), and the ten carried maps that reference it are enumerated by hand in six functions. Five defects of the last month are that shape: the coupling rows' time update at the legacy clock noise while the fold anchored the clock (card CTU's root), the ephemeris re-level that moves two of ten carries (CLV section 6.4 item 6), the receiver code-bias carry folded as an absolute prior (CTU2 section 2), the drop that did not decouple (CTU2 section 6), and the hand-built re-base of CLV2 section 6. This card specifies consolidation, not abstraction: a declarative descriptor per parameter kind read by every mechanism (section 4), one traversal over every carried reference (section 5), a run-end "model as armed" table (section 6) and a declarative knob table (section 7), in five byte-identity-gated phases (section 8), with the estimator class hierarchy and the unification of the epoch-skip payloads excluded (section 9). Build card for the implementing session, sequenced after card CTU2's code lands.
- **source:** [CTU](CTU_coupling_time_update_2026-09-28.md) sections 9.1 and 13.1 (the mirror of the fold); [CTU2](CTU2_level_recursion_followup_2026-09-29.md) sections 2 and 6; [CLV](CLV_clock_level_2026-09-22.md) section 6.4 item 6; [CLV2](CLV2_clock_level_batch2_2026-09-25.md) section 6.4; [RF-1 design](RF-1_float_ambiguities_design.md) sections 1.3 to 1.5; `docs/network_decomposition_plan.md` sections 10 and 11 (the run-state entity table, the seam constraints); the 2026-06 architecture audit (memory `project_arch_opportunity_backlog`: consolidation over abstraction, the YAGNI list)
- **knobs / posture:** phases 1, 2 and 4 are pure refactors with no knob, gated by same-build byte identity on both goldens; phase 3a adds counters only; phase 3b (unified event responses) is behaviour-changing and gets one default-off knob per response it replaces; the "model as armed" table is trace class (always written); no golden, ledger or posture change
- **dates in the text:** 2026-09-29
- **hashes / run dirs:** the read is against main `001e3da` plus the uncommitted CTU and CTU2 working tree; the counts of section 2 are of that tree (`include/app/NetworkRunContext.hpp`, `include/filter/NetworkRunTypes.hpp`, `src/core/config/GnssConfigLoadNetwork.cpp`); no run

## 1. Why: one defect class, five instances

A parameter of the network estimator has properties: a process noise, a variance floor, a
birth variance, a route through the carry (a dense border entry, a conditional row, a row of
the station's local block, a diagonal station prior), a datum role (pinned, held, free), a
response to each event (a slip, a gap, a handover, an anchor decouple, a re-admission), and a
place in the warm-resume snapshot. None of these is declared once. Each is decided where a
mechanism happens to need it, and a mechanism that needs a property it does not decide reads
a different answer from the one another mechanism applied.

| defect | the property applied in one place and not another | record |
| --- | --- | --- |
| the frozen clock level | the coupling rows shrank a clock term at q = 1e-3 m2/s (`couplingQ`) while the dense fold anchored the same clock at 2.9e-8 with a 1e-5 m2 floor | CTU sections 1 and 2 |
| the mirror of the fold | to read the fold's own Q the rows needed a side-effect-free twin of the fold's per-key decision, checked by counters every epoch | CTU section 13.1 |
| the ephemeris re-level | moves the clock references of `prev_carry`, `local_carry` and `zwd_carry`; the ionosphere, hold, receiver clock, receiver phase-bias and receiver code-bias carries hold the same references and are not moved | CLV section 6.4 item 6; `NetworkRelevel.cpp` 106 to 119 |
| the receiver code-bias carry | a conditional variance folded as an absolute diagonal prior, so the border information doubled every epoch; the same parameter has an in-block route with the right semantics | CTU2 section 2 |
| the drop that did not decouple | the response to a slip cleared the border regression only when the variance moved; an arc at birth strength kept folding | CTU2 section 6 |
| the phase-exact re-base | to move the level consistently a mechanism had to enumerate the ambiguities, the held datum's pins, the sticky targets and the handover ledger by hand | CLV2 section 6.4 |
| four reset paths | pending-reseed, the free-epoch latch, the T4 and T3 erase, the gap cliff: one parameter, four responses, none restoring the birth variance | RF-1 design section 1.5 |

The cost is paid twice: at build time (each mechanism re-derives the properties and gets a
subset), and at reading time (a human who wants the satellite phase bias's process noise as
armed reads the config, the knob snapshot, two fold branches, the coupling builder, the
anchor's override, the floor and the VCE scale, in that order).

## 2. What exists today

**The PPP path has a registry.** `StateManager` owns the state layout and the dynamic key
maps; `StateKeySchema` is the single source of truth for every key format, with a documented
format, a `make` builder and a `parse` helper per category, and the rule that no call site
walks a key string on its own. The process noise and the initial standard deviation of each
category are one block each of the config (`filter.process_noise`, `filter.initial_std`).

**The network path has identities without properties.** The border parameter kinds are
`SharedParamKind` (Clock, PhaseBias, CodeBias, VtecSh); an instance is `CarryPrior` (kind,
satellite, signal, value, variance, the reinit flag, the park time) keyed by `sharedCarryKey`.
A station-local carried instance is `LocalCarry` (value, variance, last seen, the regression
`coupling` on the border, the harvest block and index, the birth stamps, the re-level stamp,
the VCE factor). The properties live elsewhere:

| property | where it is decided today |
| --- | --- |
| process noise per kind | the diagonal D2 fold (`NetworkRunContext.cpp` 3706), the dense fold (3818), the coupling builder (`NetworkCoupling.cpp` 62), the SH prior inject (3388), the anchor's override for a clock (`NetworkClockAnchor.cpp` 112 and 132), and card CTU's mirror `networkCouplingExactPrepare` with `clockAnchorFoldPeek` |
| variance floors | the dense and diagonal folds (`pb_var_floor`, `cb_var_floor`), the anchor's floor, the leveling floor (default 0), the float vehicle's birth floor |
| carry route | chosen per mechanism: the dense block registers every stashed clock, phase-bias and code-bias entry; a station local is a conditional row, a row of the F3 block, or a diagonal station prior, by knob and by kind |
| datum role | the S-basis (`NetworkSbasis`), the held phase datum (`NetworkPhaseDatum`), the pinned pivot; a coupling term on a pinned column carries `p_prev = 0` (card 1e) |
| event responses | four reset paths for an ambiguity (RF-1 design 1.5); the anchor decouple (`NetworkClockAnchor`); the phase-anchor re-reference erase; the handover S-transform on the border only (`NetworkHandoverStransform.cpp` 107); the eq34 shift on the locals (`NetworkChainDatum.cpp` 419, 831) |
| snapshot membership | `NetworkStateSnapshot` serializes the border carry (D rows), the ambiguity and ZWD carries (L, Z), the pivots, the clock oscillators, the phase anchor, the Q3 anchor, the CLK-R frame, and since card CTU the level information (LB); the decomposition plan's entity table names about thirteen carries that are deliberately cold-started under a two-place rule |
| the references a re-base must move | ten `LocalCarry` maps (`local_carry`, `zwd_carry`, `iono_carry`, `iono_blk_carry`, `gl_hold_amb`, `gl_hold_iono`, `gl_hold_iono_blk`, `rx_clk_carry`, `rpb_carry`, `rx_cb_carry`), the border vector, and the baselines (`prev_clk`, `cb_exc_prev_clk`, `pb_hold_prev_clock_m`, `pb_level_prev`, `pin_bridge`, `code_bias_prev_`, the hold's held values, the anchor's pin targets); enumerated by hand in six functions |

The sizes: `NetworkCarryState` holds 171 containers and about 391 data members over 1 740
lines; `NetworkKnobs` about 310 members; `NetworkServiceCounters` about 668; the network
loader reads 655 knobs in 13 500 lines; one knob (`network.carry_coupling_time_update`) is
declared or listed in seven files (the config header, the loader, the posture log, two
consumers, the ledger JSON and the ledger register), plus the fingerprint tokens in
`NetworkRunContext.cpp`.

Two recorded hazards bound this card. The Tier-1 carries that the snapshot does not restore
are cold-started by design, gated by a fingerprint conditional-append and never restored
without their coupled substate (decomposition plan section 10): the registry records that
membership; it does not change it. The eight epoch-skip payloads differ by design and are not
unified (section 11 of the same plan).

## 3. Principles

1. Consolidation, not abstraction. The 2026-06 audit's finding stands: the wins are declared
   properties and one implementation of each rule, not new class hierarchies. No estimator
   base class: the PPP Kalman filter and the network's information-form solve with a factorized
   carry are different algorithms, and one type over both is the YAGNI item the audit killed.
2. Declare, then check, then replace. Every migration first adds the declared property beside
   the existing site, counts disagreements on both beds for one build (the pattern card CTU
   used for the fold's mirror), and replaces the site only at zero. Nothing is deleted before
   its replacement has been measured equal.
3. Byte identity for every phase that claims no behaviour change: the products (comments
   stripped), the closed-loop trace and the epoch status of 2 h arms from both goldens
   identical to the pre-phase build on the same build flags; the pure moves additionally proven
   by the object-code identity method (`dumpbin /disasm` of the Release objects, memory
   `project_object_code_identity_proof`).
4. The JSON knob names do not change. Configs in the wild, the ledger and the goldens name
   them; a rename is a migration card.
5. The registry is read, never written, inside the epoch loop. Per-instance runtime
   dispositions (anchored, loose, reinit, re-referenced, departed, parked) are state on the
   instance, set by the mechanism that decides them, read by everyone else.

## 4. The registry

**4.1 Kinds.** One enumeration of parameter kinds for the network path, in a new header
`include/filter/NetworkParamKind.hpp`: the border kinds (the existing `SharedParamKind`
values SatClock, SatPhaseBias, SatCodeBias, VtecSh) and the station-local kinds (Ambiguity,
Zwd, SlantIono, RxClock, RxPhaseBias, RxCodeBias). The gross-screen holds (`gl_hold_*`) are
instances of Ambiguity and SlantIono with a hold state, not kinds. `SharedParamKind` stays as
the border subset (no rename; the new enumeration maps onto it).

**4.2 The descriptor.** One `ParamSpec` per kind, built once at run start from the config by
`NetworkParamRegistry::build(const GnssConfig&)` and stored on `NetworkRunContext` (a new
member beside `knobs`, read-only after the build):

| field | meaning | today's source |
| --- | --- | --- |
| `q_m2s` | the process noise per second of the kind's random walk (0 for a constant) | `filter.process_noise.*` through `NetworkKnobs::q_clock`, `q_pb`, `q_cb`; the SH per-degree ladder |
| `q_rule` | how the noise scales with the gap: linear in the gap, per-degree (SH), or the anchor's per-satellite elapsed time | the three forms in the folds and the anchor |
| `var_floor_m2` | the floor on the carried variance | `pb_var_floor`, `cb_var_floor`, `clock_anchor_var_floor_m2`, the leveling floor |
| `birth_var_m2` and `birth_rule` | the variance an instance is born with, and whether a re-birth restores it | the float vehicle's `amb_float_birth_var_m2`, the drop's restore rule |
| `route` | DenseBorder, DiagonalBorder, ConditionalRow, LocalBlock, DiagonalStationPrior, InBlock | the knob-and-kind branches of the fold and the inject |
| `datum_role` | Pinned, Held, Free, GaugeProjected | the S-basis, the held datum, the pivot |
| `on_slip`, `on_gap`, `on_handover`, `on_decouple`, `on_readmit` | named response policies (section 4.5) | the four reset paths, the anchor, the re-reference erase, the S-transform, the READMIT seed |
| `snapshot` | Restored (with the record letter), ColdStartedByDesign (with the coupled substate it must be cold-started with), Derived | `NetworkStateSnapshot`, the decomposition plan's entity table |
| `fingerprint_tokens` | the knob tokens whose change alters this kind's meaning | the conditional-append list in `NetworkRunContext.cpp` |
| `reference_sites` | the containers that hold a value, a regression term, a baseline or a held value of this kind (section 5) | the six hand-written enumerations |

The descriptor carries what the config says; the per-instance disposition of section 4.4
carries what this epoch decided.

**4.3 The single Q source.** One function, `double paramProcessNoise(const ParamSpec&,
const CarryDisposition&, double gap_s)`, returns the inflation every consumer applies to an
instance this epoch: the kind's `q_m2s` under its `q_rule`, the anchor's elapsed time for an
anchored clock, the loose rate for a loose one, the reinit variance for a flagged one,
infinity for a decoupled, departed or skipped column, plus the floor raise. The four sites of
section 2 and card CTU's mirror call it; `couplingQ` keeps only the SH ladder until the SH
prior inject also reads the registry.

**4.4 The disposition on the instance.** `CarryPrior` gains `CarryDisposition disposition`
(Anchored, Loose, Reinit, Reref, Departed, Parked, Skipped) and `double q_applied_m2`, both
set by the fold when it decides them and read by the coupling builder, the exact update, the
snapshot writer and the teardown. Card CTU's mirror (`networkCouplingExactPrepare`,
`clockAnchorFoldPeek`) is kept for one phase as the check (its mismatch counter must read 0
with the disposition in place) and removed in the phase's second commit.

**4.5 Response policies.** The responses are named values, one implementation each, chosen
by the descriptor: `EraseAndRebirth` (the T4 and T3 path), `FreeEpoch` (the latch), `Drop`
(the float vehicle's drop, with the decouple of CTU2 section 6 as its only form),
`GapPrune`, `Marginalize` (the departed and decoupled route of the border), `Stransform`
(the declared datum move), `SeedAtVarPlusQGap` (READMIT). Phase 3a declares which policy each
kind uses today, counts every response by (kind, policy, site) and changes nothing. Phase 3b
routes each kind's response through the one implementation of its policy, one kind per arm,
behind a default-off knob per replaced path.

**4.6 What reads the registry.** The folds (diagonal and dense), the anchor, the coupling
builder (scalar and exact), the SH prior inject, the local-block builder, the harvest (the
birth rule), the snapshot writer and reader (membership), the re-base and re-level seams
(the reference sites of section 5), the teardown (section 6) and the loader's posture log
(section 7).

## 5. The traversal of carried references

A static table of reference sites, `include/filter/NetworkCarrySites.hpp`, one row per
container of `NetworkCarryState` that holds a value, a regression term (`CoupledShared`), a
baseline, a held value or a pin target of a border parameter: the container, its key form,
what it holds and how a shift applies (add to the value, add to `s_prev`, add to the baseline,
accumulate the emit-side transfer). Today's rows are the ten `LocalCarry` maps, `prev_carry`
(parked entries included), `prev_clk`, `cb_exc_prev_clk`, `pb_hold_prev_clock_m`,
`pb_level_prev`, `pin_bridge`, `code_bias_prev_`, the hold's `xfer` and `held_m`, the phase
anchor's `anchor_pin_target`, and the READMIT parked entries.

One function, `long long forEachCarriedReference(NetworkCarryState&, const ReferenceFilter&,
const ReferenceVisitor&)`, walks the table and returns the count of references visited, per
site in a counter array (invariant 4). The mechanisms that today enumerate the maps by hand
become callers: `clockLevelShiftCarry` (card CLV), the ephemeris re-level (`NetworkRelevel`),
`applyClockStransform` (the handover), the eq34 D4 shift (`NetworkChainDatum`), the READMIT
park and seed, and the snapshot's serialize and restore (which iterate the sites whose
`snapshot` property is Restored).

The migration recipe, per caller: run the traversal beside the hand-written list, count the
references each visits, assert equality on both beds for one build, then replace. The unit
test that would have caught CLV section 6.4 item 6: a `NetworkCarryState` with every site
populated for one satellite, a shift of 1 m applied through the traversal, and an assertion
that every site moved by 1 m (a new site added to the state without a table row fails the
test's "every LocalCarry map is a site" check, which enumerates the maps by type through the
table).

## 6. The "model as armed" table

At the first kept epoch and in the run-end tail (`NetworkTeardown`), one line per kind:
kind, `q_m2s` and rule, floor, birth variance, route, datum role, the five response policies,
snapshot membership, and the count of instances at that moment; the same rows into
`network_run_manifest.json` under `param_model`. A second table at run end, from the
traversal's counters: site, references held, references moved by each mechanism this run.
The table is trace class: always written, no knob, no solve effect. With it, the mismatch of
card CTU (1e-3 against 2.9e-8 on the same clock) is a visible pair of numbers on the first
run, and a card's "posture" section can be generated from the manifest by a script
(`scripts/param_model_table.py`, read-only).

## 7. The knob table

`config/network_knobs.json`, one object per network knob: JSON path, type, default, class
(fingerprinted, trace, posture, diagnostic), the parameter kinds it affects, `requires`,
`refuses`, the fingerprint token, the ledger status, the posture row text. Phase 4a generates
it from the sources that exist (`scripts/knob_table_extract.py`: the loader's `jval` calls,
the posture rows of `GnssConfigNetworkPosture.cpp`, the fingerprint tokens, the ledger JSON,
the golden manifest) and extends `config_posture_check.py --verify-loader` to verify the table
against all of them; the table is then the checked description of the knob surface a human
reads. Phase 4b, optional, makes the loader read defaults, `requires` and `refuses` from the
table, which retires the hand-written interlock code one section at a time; it changes no
knob name and no default.

## 8. Phases and gates

Every phase is one commit with a compound subject (the project's convention), one thing at a
time in the main tree, after card CTU2's code lands (both touch `NetworkCoupling`,
`NetworkRunContext` and `NetworkClockAnchor`; the registry's Q source is where CTU's mirror
should end up).

| phase | content | gate |
| --- | --- | --- |
| P0 harness | the same-build A/B script for this card: 2 h arms from `config/golden/golden_{sim,field}_refound.json` on the pre-phase and post-phase exes built with the same flags, compared on `products.txt` (comments stripped), `closed_loop_trace.csv` and `network_epoch_status.csv` (the `hash3.py` and `ident2h.py` rules), plus the object-code identity proof for pure moves; the script under `scripts/` with the exe as a parameter (the decomposition's gate hard-coded a stale exe) | the harness reproduces itself: two builds of the same source are identical; `_rap_prm_g0_{s,f}` recorded as the references |
| P1 registry | `NetworkParamKind.hpp`, `ParamSpec`, `NetworkParamRegistry::build`, the single Q source, the disposition on `CarryPrior`; the four q sites and card CTU's mirror call the registry (declare, check, replace); the "model as armed" table | byte-identical on both beds; the mirror's mismatch counters 0 before its removal; the full suite green; a unit test that builds the registry from each golden and asserts the armed values (clock q 2.9e-8 anchored, 1e-3 loose, phase bias 1e-12, code bias 1e-10, floors as pinned) |
| P2 traversal | `NetworkCarrySites.hpp`, `forEachCarriedReference`, the six callers migrated one per commit with the count check, the snapshot's serialize and restore driven by the table's membership | byte-identical on both beds after every caller; the every-site unit test of section 5; a warm-resume seam arm (`event_leg.py`, a control snapshot at 1 h) identical to the pre-phase resume |
| P3a responses declared | the policy field per kind, the response counters by (kind, policy, site), nothing re-routed | byte-identical; the run-end response table matches RF-1 design section 1.5's paths and counts (the free-epoch latches and inject-site skips that section names as un-instrumented become rows) |
| P3b responses unified | one implementation per policy; one kind per arm behind a default-off knob; the drop's decouple (CTU2 section 6) is the first | not byte-identical by design: 2 h arms on both beds, the level and consumer bars of card CTU section 10, the response counters equal before and after per (kind, policy) |
| P4a knob table | `config/network_knobs.json` generated and verified; `config_posture_check.py --verify-loader` extended | the table verifies against the loader, the posture rows, the fingerprint list, the ledger and the manifest with 0 gaps; no code change to the loader |
| P4b loader reads the table | defaults, `requires`, `refuses` from the table, one loader section per commit | byte-identical; `make_golden_config.py --verify` and the posture check unchanged; every archived config in `config/` loads to the same knob snapshot (the fingerprint hex of each) |

Each gate also runs `python scripts/docs_index_check.py` and
`python scripts/make_golden_config.py --verify`. A phase that fails its gate is not merged; the
failing reading is recorded here.

## 9. Non-goals

- No estimator base class, no "Estimator" or "Parameter" class hierarchy over the PPP and
  network paths.
- No unification of the eight epoch-skip payloads (decomposition plan section 11).
- No change to which Tier-1 carries the snapshot restores (the two-place rule); the registry
  records the membership as it is.
- No renaming of JSON knobs and no default flips.
- No numerical change in phases 1, 2, 3a, 4a and 4b; phase 3b is the only behaviour change and
  is armed one kind at a time.
- No merge of `StateManager` and the network registry; the PPP path keeps its own.

## 10. Owed reads before phase 3b

- The birth variance rule of a re-born arc (RF-1 design section 1.4): which policy the
  descriptor should name for Ambiguity, read on the float vehicle with the exact update.
- Which of the ten `LocalCarry` maps are live on the RF-0hv posture (the gross-screen holds and
  the ionosphere block carry may be empty there); the traversal's per-site counters answer it
  on the P2 arms.
- Whether the SH prior inject (inert on this posture, `estimate_iono_sh: false`) should read
  the registry in P1 or stay on `couplingQ`'s ladder until an SH card re-arms it.

## 11. Pointers (the tree of 2026-09-29)

- Identity and instances: `include/filter/NetworkRunTypes.hpp` (`SharedParamKind`,
  `sharedCarryKey` 384, `CarryPrior` 344, `CoupledShared` 425, `LocalCarry` 510);
  `include/app/NetworkRunContext.hpp` (`NetworkKnobs`, `NetworkCarryState`,
  `NetworkServiceCounters`).
- The q sites: `src/core/app/NetworkRunContext.cpp` 50 to 52 (the knob snapshot), 3706 and
  3818 (the folds), 3388 (the SH inject); `src/core/filter/NetworkCoupling.cpp` 62
  (`couplingQ`); `src/core/filter/NetworkClockAnchor.cpp` 112 and 132; the CTU mirror in
  `NetworkClockLevel.cpp` and `NetworkClockAnchor.cpp` (`networkCouplingExactPrepare`,
  `clockAnchorFoldPeek`).
- The hand-written enumerations: `src/core/filter/NetworkClockLevel.cpp` (`clockLevelShiftCarry`,
  the ten maps and the baselines), `src/core/filter/NetworkRelevel.cpp` 106 to 119,
  `src/core/filter/NetworkHandoverStransform.cpp` 107, `src/core/filter/NetworkChainDatum.cpp`
  419 and 831, the READMIT park and seed in `NetworkRunContext.cpp`, and
  `src/core/products/NetworkStateSnapshot.cpp`.
- The knob surface: `include/config/GnssConfig.hpp`, `include/config/GnssConfigTypes.hpp`,
  `src/core/config/GnssConfigLoadNetwork.cpp` (655 `jval` reads),
  `src/core/config/GnssConfigNetworkPosture.cpp`, `docs/CONFIG_promotion_ledger.json`,
  `config/golden/golden_manifest.json`, `scripts/config_posture_check.py`,
  `scripts/make_golden_config.py`.
- The gates' tools: the object-code identity proof (memory `project_object_code_identity_proof`),
  `scripts/event_leg.py` (seam arms), `scripts/rapid_arm.py` (2 h arms), the session graders
  `hash3.py` and `ident2h.py` (to be promoted into the P0 script).

## 12. The first graded case of the q rule and the birth variance (2026-09-29)

Card [SMH](SMH_smoothed_hold_clone_2026-09-29.md) (sections 7 to 9) put a fixed-point smoother on
the satellite clock, and a smoother reads q literally as motion. Card Q3's anchor folds every
satellite's clock at the legacy rate (1e-3 m2/s, 0.03 m2 per 30 s epoch) until its guard
re-validates it (4 epochs at the run start, and at every re-admission and withdrawal). That q is
a datum device, the clock re-solved nearly fresh while it finds the a priori's frame, not a
statement about the clock's motion. The smoother passed the estimator's settling across those
epochs as motion: 5 cm p50 of phase-visible motion on the sim (clock truth without jumps), the
consumer at 1.41 m against the phase-clock hold's 0.34 m in hour 0 (SMH section 7). The
difference is visible on the wire, so the registry has to be able to say it:

- `q_rule` distinguishes a motion model from a datum device. The anchor's LOOSE fold is the
  second; a consumer of the q (a smoother, a clone, an instrument that reads the passed motion
  against the stated walk) must not read it as motion. SMH section 9 does this by hand (the
  clone lives only while the anchor holds its clock, and the phase-clock transfer bridges the
  rest); with the registry it is a property of the kind and the disposition.
- The principled replacement for the LOOSE rate is a `birth_var_m2` with the physical q from the
  first epoch (the clock's own walk, card Q3's 2.9e-8 m2/s), with the engagement test kept as
  the event it is (`on_readmit`, `on_decouple`), not expressed as a q.
- SMH section 9's G3 adds a second case: with a known walk injected into the a priori clock, the
  hold passes the walk only when the anchor's q states it (97 % passed with q matched, 46 % hidden
  with the golden q). Card Q3's q is therefore a statement about the a priori product's error,
  and it belongs in the registry as such, with the product it was derived for.

## 13. Phase 0, the harness (2026-10-01)

`scripts/prm_ab.py` (`make`, `run`, `cmp`): four 2 h arms per tag, all from the committed goldens
(`config/golden/golden_{field,sim}_refound.json`) as generated (`_gf`, `_gs`) and with the nine
knobs of the clock-level program posture (`_pf`, `_ps`: the exact coupling update, the smoothed
block hold and the monitors, as listed in the script), the sim from 00:30:15. `cmp` compares
`products.txt` without its comment lines, `logs/closed_loop_trace.csv` and
`logs/network_epoch_status.csv` byte for byte, and with `--all-csv` every other CSV under `logs/`
that both arms wrote. The executable is a parameter; a Debug path is refused.

Self-identity (tags `g0a` and `g0b`, both run with exe `gnss_processor_lgm0.exe`, main `17fbb0c`,
sha256[:16] `0601c7d08d4f0ed6`): the three gate files identical on all four arms, and 132 of 135
other CSV comparisons identical. The three that differ do so only in wall-clock columns
(`network_datum_graph.csv` column `ms`, `network_run_summary.csv` columns `loop_ms` and
`ms_per_epoch`, `network_service_health.csv` column `ms_per_epoch_window`); the script drops those
columns by header name (`VOLATILE_COLS`), after which all 145 comparisons are identical. Gate passed.
The references for phase 1 are `runs/_rap_prm_g0a_{gf,gs,pf,ps}`.

## 14. Phase 1a, the registry (2026-10-01)

Built: `include/filter/NetworkParamRegistry.hpp` and `src/core/filter/NetworkParamRegistry.cpp`.
The parameter kinds (`NetworkParamKind`: the four border kinds, the existing `SharedParamKind`
values, and six station-local kinds), one `ParamSpec` per kind (estimated, carried, q with its
units and rule, the anchored rate, the variance floor, the birth floor, the route, the knob
sources), `NetworkParamRegistry::build(knobs, config)` once per run, held as the const member
`NetworkRunContext::params` (declared after `knobs`). The registry reads every number from the knob
snapshot, so its values are the doubles the folds used before it.

The single process-noise source: `NetworkParamRegistry::q(SharedParamKind)` replaces the per-kind
selection at every solve-path and instrument site that read the three border rates from the knob
snapshot: the diagonal and dense D2 folds and the CTU mirror (`NetworkRunContext.cpp`), the coupling
builder's rates (`ApplicationNetworkAssembly.cpp`, read by `CouplingRowBuilder::couplingQ`), the Q3
anchor's four loose-rate terms (`NetworkClockAnchor.cpp`), the clock-level monitor
(`NetworkClockLevel.cpp`), the prior-residual census (`NetworkStashStatus.cpp`) and a dead alias in
`NetworkSolveGlue.cpp`. Section 11 listed the first four; the last three were found by a search of
every read of `knobs.q_clock`, `q_pb` and `q_cb`. The fingerprint and the run manifest's knob dump
keep reading the snapshot, because they record the configuration rather than use it. The SH ladder
stays in `couplingQ` (section 10, third item). Both fold loops skip `VtecSh` entries before the
selection and the dense mirror never sees one, so the registry read is exact for the three kinds
that reach it.

The "model as armed" table (section 6): one line per kind at run start and in the run-end tail
(`Network (param model)` and `Network (param model, run end)`), the run-end tail followed by the
carried instances of each kind counted from their carry maps, and the same rows as a `param_model`
record in `network_run_manifest.json` when the manifest is written. On the program posture the
table reads, among others: the satellite clock loose at 1e-3 and anchored at 2.9e-8 m2/s with the
1e-5 floor (the pair of numbers behind card CTU's root, now on one line), the ambiguities in the
local block with no process noise, the receiver code bias in the local block, and the wet delay,
the slant ionosphere and the receiver clock not carried.

Gate: byte-identical. `prm_ab.py cmp g0a p1 --all-csv` (exe `gnss_processor_prm1.exe` against the
phase 0 references): all 145 comparisons identical on the four arms. Unit test
`tests/test_network_param_registry.cpp` (5 cases: both goldens' armed values, the border q equal to
the knob snapshot's double, the table and the JSON naming every kind, the border mapping); the full
suite 2747 passed, 1 fixture-gated skip.

Owed (phase 1b): the disposition on `CarryPrior` (section 4.4) set by the fold and read by the
coupling builder, the exact update and the snapshot writer, with card CTU's mirror
(`networkCouplingExactPrepare`, `clockAnchorFoldPeek`) kept as the check until its mismatch counter
reads 0 and then removed. Card LGM's nuisance kind does not need it and is sequenced first.

## 15. Phase 1b, the border's fold decided once (2026-10-02)

**15.1 The order of the epoch decides where the disposition is set.** Section 4.4 has the
disposition set by the fold and read by the coupling builder. The fold
(`networkInjectD2CarryPrior`, in `networkSolveGlue`) runs after PASS B, and PASS B builds the
coupling rows, so a disposition the fold sets is one epoch late for the rows; the snapshot writer
and the teardown run after the stash, which rebuilds every live entry. Card CTU solved the first
half with a mirror of the fold before PASS B. The decision therefore has to be made where the
mirror ran: after the shared columns are registered and the switch-on seam's level noise is set,
before PASS B. Between that point and the fold, four things happen to the carry (a read of every
writer in the window): the clock datum election and the handover test (`buildSharedDatum`,
`pivotHandoverSignalled`, pure), the ephemeris cut-over re-level (values only, `brdc_rts` only,
inert on the goldens), the clock-pivot handover S-transform (values and variances of the changed
constellation's clocks; `prev_carry_cov` untouched), and, inside the fold, the anchor's begin-epoch
reset and the READMIT erase of parked entries on a datum change. No writer of the reference sets,
the anchor state, the seam noise or the VCE factors runs in the window.

**15.2 The build.** `networkCarryDecide` (new seam `filter/NetworkCarryDisposition`) decides
every carried border entry once per epoch before PASS B and records the decision on the entry
(`CarryPrior::fold`, a `CarryFoldDecision`). The record holds the route and the process-noise
increment, not the folded variance: `carryFoldVarEff` forms the variance from the carried
variance its caller holds (the fold's chain in its order: the reinit variance, or the variance
plus the increment through the anchor's floor, the B4 floor, the VCE factor, and on the dense
fold the seam noise). The coupling rows read the record on the stashed variance; the fold reads
it on the variance it folds. The dispositions: Folded, Anchored, Loose, Reinit, Seeded, Reref,
Held, OnOscillator, Parked, Screened, Retired, plus a departed flag. The card's Skipped is split
into Held, OnOscillator, Screened and Retired; its Departed is the flag (a foldable entry whose
column left the layout, which the estimator marginalizes); Folded and Seeded were not listed.
The fold applies the one clause no earlier point can know: a clock-pivot handover retires the
changed constellations' datum unless the S-transform re-expressed their carry (Retired). It
records Screened when the variance it forms is not positive, commits the anchor's bookkeeping and
tallies what it applied per (kind, disposition).

The Q3 anchor's `clockAnchorFoldVariance` is split into `clockAnchorFoldDecide` (pure, with the
increment), `clockAnchorFoldAt` (the floor on a given variance, the one implementation of it) and
`clockAnchorFoldCommit` (the per-satellite state and counters, called by the fold only, so an epoch
that ends before the fold advances nothing). `clockAnchorFoldVariance` remains as the two halves in
one call. Card CTU's mirror (`networkCouplingExactPrepare`'s clause chain and
`clockAnchorFoldPeek`) is removed; the prepare forms the rows' epoch from the record. The fold's
own clause chain is removed too: both branches read the record through one per-key function. The
column-presence test of the decision and of READMIT's park is one function, `carryColumnPresent`.

**15.3 The mirror's handover mismatches were the S-transform.** Card EXL disclosed 31 inflation
mismatches between the mirror and the fold, at one epoch per 6 h bed. They are the 31 GPS clocks
the S-transform re-expressed at the clock-pivot handover (TOW 276150, incoming pivot G02, on both
beds), and the largest |dQ| equals the transform's `var_q` exactly (field 1.124e-03 m², sim
4.473e-04 m²): the transform adds `v_i² var_q` to each clock's variance after the rows were built
on the variance before it. The mirror was not wrong; the rows and the fold see two variances of
one decision. The run-end line now counts these keys as re-expressed after the rows were built (31
on each 6 h golden, with the largest variance change), and the CTU line counts the keys in S that
a handover re-expressed or retired. Those rows are still not conditioned on the re-expressed
variance at that epoch. Making them so is a re-expression of the rows' border references at the
handover, which is phase 2's traversal (the S-transform is one of its callers), not a phase 1
change.

**15.4 The gates.** Same build flags; the references are the HEAD executable
`gnss_processor_prm1b0.exe` (`45183c6e51e9c729`, main `d241f27`, tags `b0h6` and `b0`). The
harness's 2 h arms contain no clock-pivot handover, so the goldens run 6 h (`--hours 6`, the
handover at 4.7 h); the program posture (with card LAH's phase-clock hold off, which the loader
refuses with the smoothed hold), the scalar coupling update and the diagonal border fold (reached
only on the level vehicle) run 2 h, the three postures the goldens no longer exercise.

| step | executable | result |
| --- | --- | --- |
| 1, the decision beside the mirror (`dec3101`) | `gnss_processor_prm1b1.exe` `733c20fc106b44bc` | byte-identical on all ten arms (284 comparisons, every CSV); the decision against the mirror bit for bit, 0 mismatches (153 437 / 113 194 keys on the 6 h field / sim goldens, 51 229 / 37 860 on the program posture; the mirror does not run on the scalar and diagonal postures); against the fold bit for bit, 0 variance and 0 disposition mismatches (151 725 / 111 777 variances on the 6 h goldens, 37 399 to 50 897 on the 2 h arms); 31 keys re-expressed in 1 epoch on each 6 h golden; 0 entries undecided at the fold |
| 2, the mirror removed, the fold reading the decision | `gnss_processor_prm1b2.exe` `77ab7c95d714bc4c` | byte-identical on all ten arms (284 comparisons); the CTU line counts 1 handover epoch on each 6 h golden with 31 keys of S folded on the re-expressed variance and 0 retired; 0 entries undecided at the fold |

The check of step 1 is bitwise (`==` on the doubles), not the mirror's relative 1e-12 tolerance,
and it covers the leftovers' inflations and the departed flag as well as the dense keys. Unit
suite 2759 passed, 1 fixture-gated skip, after each step; suite `CarryDisposition` (5 cases: the clauses in the fold's order, the
variance against the rule written out, a re-expressed variance keeping the decision, the anchor's
decide, at and commit against the rule in the six states, the pass overwriting a stale record and
writing nothing else).

**15.5 The tally.** The run-end table now states what the fold applied. On the 6 h field golden:
the satellite clocks 21 579 key-epochs anchored, 256 loose, 4 seeded, 341 parked; the code biases
67 660 folded, 16 seeded, 1 348 parked; the phase biases 62 210 folded, 23 re-referenced; departed
4, 16 and 12. Nothing is held (no phase hold, no SH prior on this posture) and nothing retired (the
S-transform released the one handover). The same counts go to the run manifest as a
`param_dispositions` record.

**15.6 What phase 1b does not change.** Five sites still re-derive part of the fold's decision,
each with a formula that differs from the fold by design or by omission; moving them onto the
record changes outputs, so they are owed to phase 3 or to their own cards:

- the VCE innovation feed and the prior-residual census (`NetworkStashStatus`): run after the stash
  (the record is gone), omit the seam noise, and the feed needs the variance before the VCE factor;
- the clock-level monitor's walk (`NetworkClockLevel`): its own dt, no anchor floor, no reinit, no
  seam noise;
- the scalar coupling route (card 1d's kappa, `couplingQ`): the loose clock rate over the row's own
  gap, for rows off the exact route (167 per 6 h on the field) and every row with the exact update
  off: card CTU's root in scalar form;
- the card 1d reinit history (`tu_reinit_hist`): a cross-epoch history the stale rows read.

The snapshot writer reads no per-epoch decision: the record does not survive the stash, and the
writer's membership rule is the park stamp (`t_park_s`), a persistent instance property set by
the mechanism that decides it (principle 5 already holds); the registry's snapshot field is phase
2. The election and the handover test could move before PASS B without a byte change (both are
pure), which would let the decision know the retired-datum clause; the S-transform cannot move
without changing the handover epoch's rows. Neither is done.
