# PRM: the parameter registry and the carry contract

**STATUS**

- **id:** PRM
- **topic:** network-vehicle
- **status:** open
- **summary:** Phases 0 to 2 BUILT, byte-identical on both goldens: the parameter registry, the border fold decided once per epoch and the carried-reference traversal; the responses continue in card PRM3.
- **source:** [CTU](CTU_coupling_time_update_2026-09-28.md) sections 9.1 and 13.1 (the mirror of the fold); [CTU2](CTU2_level_recursion_followup_2026-09-29.md) sections 2 and 6; [CLV](CLV_clock_level_2026-09-22.md) section 6.4 item 6; [CLV2](CLV2_clock_level_batch2_2026-09-25.md) section 6.4; [RF-1 design](RF-1_float_ambiguities_design.md) sections 1.3 to 1.5; `docs/network_decomposition_plan.md` sections 10 and 11 (the run-state entity table, the seam constraints); the 2026-06 architecture audit (memory `project_arch_opportunity_backlog`: consolidation over abstraction, the YAGNI list)
- **knobs / posture:** phases 1, 2 and 4 are pure refactors with no knob, gated by same-build byte identity on both goldens; phase 3a adds counters only; phase 3b (unified event responses) is behaviour-changing and gets one default-off knob per response it replaces; the "model as armed" table is trace class (always written); no golden, ledger or posture change; phase 2 step 3 added `network.handover_stransform_before_rows` (default off, not recommended, section 16.6)
- **dates in the text:** 2026-09-29; phases 0 and 1a 2026-10-01, 1b and 2 2026-10-02
- **hashes / run dirs:** the read is against main `001e3da` plus the uncommitted CTU and CTU2 working tree; the counts of section 2 are of that tree (`include/app/NetworkRunContext.hpp`, `include/filter/NetworkRunTypes.hpp`, `src/core/config/GnssConfigLoadNetwork.cpp`); no run at pre-registration; phases 0 to 2: harness `scripts/prm_ab.py` (self-identity on `gnss_processor_lgm0.exe`), exes `gnss_processor_prm1.exe` to `gnss_processor_prm2d.exe`, arms `runs/_rap_prm_*` (sections 13 to 16)

**Outcome.** The author's question after cards CTU and CTU2 (readable parameter objects, an estimator hierarchy) is answered by consolidation, not a class hierarchy: one declared property set per parameter kind, one traversal over the carried references. Phases 0 to 2 pass every gate: one process-noise source, the fold decided once per epoch (card CTU's mirror retired at 0 mismatches), a traversal over 26 declared sites replacing five hand-written walks. The one output-changing option, the handover's S-transform before the rows, reads neutral to slightly worse and is not recommended. Phase 3 continues in [PRM3](PRM3_responses_2026-10-02.md); phase 4, the knob table, is not built.

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

## 16. Phase 2, the traversal of carried references (2026-10-02)

**16.1 The sites.** `include/filter/NetworkCarrySites.hpp` declares 26 sites, one per container
and reference role of `NetworkCarryState` that holds a reference of a border parameter: the
carried value and variance (`prev_carry`, READMIT's parked entries included), the stash's pinned
pivot (`cxu_pivot`), the oscillator's offset (`clock_osc`), five baselines (`prev_clk`,
`cb_exc_prev_clk`, `pb_hold_prev_clock_m`, `pb_level_prev`, `code_bias_prev_`), two held values
(`pin_bridge`, `pb_hold`), and the eleven local carry maps, each as its regression terms and, where
its value is expressed in a border frame, as that value (the ambiguity, the gross screen's held
ambiguity, the receiver clock, the receiver phase bias, the receiver code bias). Each row states
the key form, what the reference is, how a shift applies, and the snapshot record that restores it
or why it is cold-started. `forEachCarriedReference` walks a site mask in a fixed order, hands each
reference to a visitor and counts per site what the visitor moved.

The read corrected section 5's list in four places. The local carry maps are eleven, not ten: card
LGM's `pc_carry` holds regression terms and is in no hand-written list. `clock_osc` is a site (the
ephemeris re-level moves it). `anchor_pin_target` is an ambiguity's pin target, not a border
reference; it is listed with the non-sites in the header's file comment, beside `phase_basis`, the
eq34 targets and the handover ledger, each rebuilt by its own election. Card CLV's `xfer` is a
declaration derived from the moved references and written after the walk, not a site.

**16.2 The mechanisms.** Six callers: card CLV's level re-base, the ephemeris re-level, the
clock-pivot handover S-transform after PASS B, the same transform before the rows (step 3), the
eq34 D4 shift, and the float vehicle's declared anchor handover (`networkAmbFloatDeclaredHandover`,
a seventh hand-written list the card did not name). Two of section 5's six are not callers. The
READMIT park, seed and datum drop touch one container (`prev_carry`) and enumerate no other site;
their rule is the park stamp, which each mechanism's visitor reads as the reference's `parked`
attribute. The snapshot's serialize and restore stay per record: each record has its own capture
knob, screens and encoding, the file format is fixed, and the I/O module may not include filter
types (`NetworkStateSnapshot.hpp` 183), so a table-driven writer would re-encode the same per-record
code. The table records each site's record instead (section 9: the membership as it is).

Each mechanism declares the sites it moves and, for every other site its direction reaches, the
reason it does not move it (`carryMechanismSite`). The exemptions are the hand lists' gaps, named:

| mechanism | not moved, with the reason |
| --- | --- |
| card CLV's re-base | `pc_carry` (card LGM postdates card CLV's list; never armed together); `cxu_pivot`; `clock_osc` (CLV is resolved off with the clock model); `pb_hold` (by design: the re-base accumulates the emit-side transfer instead) |
| the ephemeris re-level | eight of the ten coupling maps, `cb_exc_prev_clk`, `pb_hold_prev_clock_m` (card CLV section 6.4 item 6), `cxu_pivot` |
| the handover after PASS B | every coupling map and `cxu_pivot` (the rows of the handover epoch are built before it); the baselines and `clock_osc` (by design: erased, suppressed or stepping by the datum delta); the receiver clock carry (off the transform's direction) |
| the eq34 D4 shift | seven coupling maps, `pb_level_prev`, `pb_hold`, the gross screen's held ambiguity; `pin_bridge` (by design: a bridged satellite is a zero-shift root) |
| the declared anchor handover | every coupling map but the ambiguities', `pb_level_prev`, `pin_bridge`, `pb_hold`, the gross screen's held ambiguity |

Phase 2 keeps every mechanism's set as it was; each owed exemption is a one-line change behind its
own arm (phase 3b). The section 10 question of which local carry maps are live on the RF-0hv
posture is answered by the run-end table: at the end of the 6 h goldens the ambiguities hold
192 209 (field) and 146 629 (sim) regression terms on 894 and 946 carried values, and the receiver
code bias's in-block route 21 113 and 9 299 terms on 98 and 60 values; the wet delay's carry is
filled only when a snapshot path is set (none on the harness arms); the ionosphere, gross-screen
hold, receiver clock, receiver phase-bias and phase-colour maps are empty, and so are `prev_clk`,
`cb_exc_prev_clk`, `clock_osc` and `pin_bridge` (their mechanisms are off on the goldens).

**16.3 Step 1, the traversal beside the hand-written lists.** Each mechanism's walk through the
traversal was written beside its hand-written walk (`clockLevelShiftThroughSites`,
`ephCutoverShiftCarry`, `applyClockStransformThroughSites`, `eq34ShiftCarry`,
`declaredHandoverShiftCarry`), run dry (nothing written) before the hand walk, and its count
compared with the hand walk's (the S-transform's also on c0 and var_q, bit for bit). The run end
writes one line per site (references held, moved by each mechanism, the snapshot record) and one
per mechanism (calls, the check, the sites not moved), and the same rows to the run manifest as a
`param_sites` record. The goldens leave four of the mechanisms inert, so the harness gained four
postures that run them (`scripts/prm_ab.py`, from the probes of 2026-10-02): card CLV with its
code-bias follow (`clv`; card LAH's hold off, which the loader refuses with the level carry), the
ephemeris re-level (`relevel`; broadcast a priori from the brdc-only data root, the satellite code
bias off, 2.1 h so the field reaches the 02:00 navigation batch), the eq34 D4 shift (`eq34`) and the
declared anchor handover (`dho`). `make` now refuses a knob the loader does not read (an unknown
key is ignored without a word, so such an arm is silently its base).

| gate | executable | result |
| --- | --- | --- |
| references | `gnss_processor_prm1b2.exe` `77ab7c95d714bc4c` (HEAD code) | tags `b0h6` and `b0` (phase 1b); new tags `x0` (clv, eq34, dho, 1 h) and `x0r` (relevel, 2.1 h) |
| step 1, the check beside the lists | `gnss_processor_prm2a.exe` `0e2bca9d4ee1d2f6` | byte-identical on all 16 arms, every CSV (556 comparisons); the count check 0 mismatches on every call: handover 1 per 6 h golden, card CLV 121 per bed, the re-level 30 (field) and 29 (sim), the eq34 shift 46 (field: 2 095 values and 88 228 coupling terms, the probe's run-end counters), the declared handover 1 per bed |
| step 2, the lists replaced | `gnss_processor_prm2b.exe` `a1dd6aa00ce2e51f` | byte-identical on all 16 arms, every CSV (556 comparisons); each mechanism ran on its own arms through the traversal: handover 1 call per 6 h golden, card CLV 121 per bed, the re-level 30 / 29, the eq34 shift 46 / 41, the declared handover 1 per bed |
| step 3, knob off | `gnss_processor_prm2c.exe` `788c46b2fde5165e` | byte-identical on all 16 arms, every CSV (558 comparisons) |
| step 3 after the review (the undo, the receiver-clock refusal, the re-detection's bookkeeping) | `gnss_processor_prm2d.exe` `67a9f988dd141dc0` | knob off: byte-identical on all 16 arms (558 comparisons); knob on: byte-identical to the `prm2c` knob-on arms (0 re-detections, 0 undos on the goldens), so the read of section 16.6 stands |
| step 3, the warm-resume seam (section 8's P2 gate) | `gnss_processor_prm2c.exe` and `gnss_processor_prm2d.exe` against `gnss_processor_prm1b2.exe` | `event_leg.py` seeds of 2 h on both goldens (snapshots every 120 epochs) and null legs of 118 epochs resumed from the 1 h snapshot (`.ep0122`; `resume_rxcb_lin_restore` off, which the loader refuses with `rx_code_bias_datum`): byte-identical, products, every CSV and every snapshot file with the M line's wall-clock field masked (156 comparisons); both legs resumed (209 / 154 border entries, 930 / 982 ambiguities, 58 / 60 wet delays) |
| step 3, `network.handover_stransform_before_rows` on (6 h goldens, `cmp b0h6 p2ch --map g=h`) | the same | every file identical up to TOW 276150 (the handover) and different from that epoch on; one edge per bed, re-expressed before PASS B: 31 GPS clocks, the stash's pivot, 27 264 (field) / 31 168 (sim) ambiguity coupling terms and 3 136 / 1 920 receiver code-bias terms; the CTU line counts 0 keys of S folded on a variance re-expressed after the rows (31 in phase 1b); the handover's own counters unchanged (1 edge, 31 rows, 214 retained) |

Unit suite 2769 passed, 1 fixture-gated skip; suite `CarrySites` (10 cases: the table, every site
moved by 1 m through the walk, the site mask, the scan of `NetworkCarryState`'s declaration for
local carry maps without a site and of the run types for any other holder of regression terms,
the mechanism table, each mechanism's walk against its hand walk or its rule written out).

**16.4 Step 2, the hand-written lists replaced.** The five hand walks are deleted; each mechanism
calls its walk through the traversal (`clockLevelShiftCarry`, `ephCutoverShiftCarry`,
`applyClockStransform` on the run's carried state, `eq34ShiftCarry`, `declaredHandoverShiftCarry`).
The two phase-bias shifts share one rule, `phaseBiasShiftOf`, which replaces
`rebaseCouplingPhaseBiasSprev` (it had no other caller; its tests now run through the
traversal). The dry mode and the count check are retired with the lists; the run end keeps each
mechanism's calls and the moved counts per site. The card's recipe migrates one caller per commit;
the five are one commit here, because the gate already separates them: each mechanism runs on its
own posture's arms (card CLV on `clv`, the re-level on `relevel`, the eq34 shift on `eq34`, the
declared handover on `dho`, the S-transform on the 6 h goldens), and every arm is compared byte for
byte with its reference. Unit suite 2769 passed, 1 fixture-gated skip.

**16.5 Step 3, the handover's S-transform before the rows.** `network.handover_stransform_before_rows`
(default off, requires `handover_stransform`, refused with the broadcast a priori, fingerprinted
when armed). `networkHandoverStransformBeforeRows` previews the clock datum election and the
handover test before the fold decision and PASS B (both pure, section 15.6; one declaration,
`networkClockDatumDeclaration`, now shared with the solve glue's election) and applies the
S-transform through the traversal on the mechanism `handover_before_rows`: every clock reference
of the changed constellation the rows read (the carried clocks, an absent or parked satellite of
the constellation at the structural direction's common coefficient 1, the coupling terms' s_prev
and, for a free term, p_prev, and the stash's pivot value). The fold decision, the exact update's
prepare and the rows then read the re-expressed carry, and the solve glue's transform keeps the
bookkeeping (the counters, the STRANSFORM row, now with `before_rows=1`, the fold release).

The preview runs before two skip sites the transform after PASS B never preceded (PASS B's own
skip and the null-space gate's withhold), so the transform is recorded (`sts_carry_pivot`: the
pivot, c0, var_q) until the next stash re-expresses the carry in the solve's datum. When an epoch
is skipped after the preview, the next preview keeps the carry if it elects the same pivot (the
handover is detected again and released without a second transform, counted as an edge with
c0 = 0, as the transform after PASS B counts a re-detection), and undoes the transform if it
elects another: the outgoing pivot again (the sticky incumbent's return) or a third one, which is
then transformed to from the outgoing datum, so var_q is never added twice (`undoClockStransform`,
a STRANSFORM `sts_undone` row and a run-end count). The review of the first build found the three
cases (2026-10-02); the unit cases pin them with dyadic values, so the undo is exact. The
transform after PASS B keeps its own gap: a skip after it (`solve_failed`, `we_restore_failed`,
`sigma0_withheld`) re-detects the handover and runs the kernel again, which moves nothing (c0 = 0)
and adds var_q to every clock once more, and a re-election of the outgoing pivot after such a skip
leaves the carry in the incoming datum (recorded here, not changed: knob-off byte identity).

The knob is refused with `network.carry_rx_clock_in_block`: the receiver clock is on the clock
datum's direction, the transform moves its coupling terms and not its carried value, and its
block row would be off by c0 at the handover epoch (the ambiguity, wet-delay, ionosphere and
receiver code-bias rows have a family sum of 0 on the direction and are unaffected). Unit suite
2774 passed, 1 fixture-gated skip (the knob's loader rules; the preview's transform, its
re-detection, its undo on the outgoing pivot's return and before a third pivot).

**16.6 The read: neutral to slightly worse, because the rows were already exact.** With the knob on
the 6 h goldens, the emitted clock and phase bias move together after the handover by 0.4 mm rms
(field) and 2.3 mm rms (sim), at most 8 mm, and the code bias by less than 0.03 mm, for the rest of
the run. The consumer is unchanged on the field to 0.1 mm and changes by at most 0.1 mm on the sim.
Against the sim's truth (clock_level/clklevel.py's datum-free rule) the GPS clock error is 0.39 mm
worse in the half hour after the handover and 0.15 to 0.17 mm worse in the hour after that, on an
error of 19 to 24 mm.

The read of the rows (2026-10-02) explains the sign. A coupling row is gauge-invariant: its clock
coefficients keep the harvest's family sum, and when the outgoing pivot has left the layout its
deficit is spread over the present clocks of the constellation, so the row built before the
transform, in the outgoing datum, states the same conditional relation as the row of the exactly
transformed prior. The diagonal re-expression the fold uses (var_i + v_i^2 var_q with the cross
term dropped, SPEC_pivot_visibility_gauge_defect section 8.1) is the approximation. Moving the rows
onto it adds var_q to every changed clock's time update in the rows, which loosens their hold on
the clocks at the handover epoch by the measured amount. The algebra is derived, not proven; the
measurement agrees with it.

The knob stays default off and is not recommended. The consistent cure runs the other way: the
fold should match the rows, by re-expressing the dense border covariance itself
(S P S^T, deferred by SPEC_pivot_visibility_gauge_defect section 8.8), which the traversal now makes
a single-site change (`prev_carry` with `prev_carry_cov`). It is owed to phase 3b or its own card.

**16.7 What phase 2 leaves open.**

- The mechanism table's owed exemptions (section 16.2), each a one-line change behind its own arm
  in phase 3b: the ephemeris re-level's eight coupling maps and two baselines (card CLV section 6.4
  item 6), card CLV's `pc_carry`, the phase-bias shifts' remaining coupling maps, `pb_level_prev`,
  `pb_hold` and the gross screen's held ambiguity, and the receiver clock carry under the handover.
- The parked clocks of an absent satellite are not re-expressed by the transform after PASS B
  (its membership is this epoch's registered set); a seed after a handover reads them in the
  outgoing datum. The transform before the rows reaches them. Not exercised on the 6 h goldens.
- The snapshot's records are declared per site (`record`, `snapshot_note`), not generated from the
  table; the warm-resume seam arm is the check that nothing in the phase moved them.
- The run end's site table answers section 10's second owed read (which local carry maps are live
  on the RF-0hv posture, section 16.2).

## 17. Phase 3a, the responses declared and counted (2026-10-02)

Continued in [PRM3](PRM3_responses_2026-10-02.md): 69 response sites declared with their arming
conditions and counted, the registry's policies per event, the free-epoch latch per cause;
byte-identical on all 16 harness arms, 0 declaration errors.
