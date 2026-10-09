# NOM: an object model for the network producer (systems, signals, channels and their parameters)

**STATUS**

- **id:** NOM
- **topic:** network-vehicle
- **status:** open
- **summary:** The network producer gains explicit, typed objects for every constellation, band, signal, satellite, station, line of sight and arc, a declared disposition for every parameter instance, a declared kind for every row, and run state owned by those objects; plain types, clarity over byte identity (the author, section 10). N0a, N1, N1b, N0b and N0c LANDED, byte-identical (sections 12, 14, 18; `c7e0a9a`, `029b06c`); a network code tour for newcomers started (`docs/onboarding/07_network_tour.md`); the row-classification read found one solve-affecting defect, card RPA (section 16). N2 is specified here (section 17); its results go to a continuation card, as this file nears the size bar.
- **source:** the author's request of 2026-10-08 ("explicit objects for each system, signal, channel, etc. and their network parameters ... much easier for me to follow"); the read-only organization map of 2026-10-08 (five investigators over the user filter, the network parameters, the network rows, the identity layer and the run state; session files, every count below re-checked where it drives a decision); [PRM](PRM_parameter_registry_2026-09-29.md) and [PRM3](PRM3_responses_2026-10-02.md) (the registry, the traversal, the principles this card keeps); [SID](SID_signal_identity_2026-09-22.md) (the signal roster and arc ledger, built and not armed); [L5R](L5R_l5_return_on_the_exact_update_2026-10-08.md) and [GAL2](GAL2_galileo_on_the_exact_update_2026-10-08.md) (the defects that triggered the request); `docs/network_decomposition_plan.md` sections 10 and 11
- **knobs / posture:** steps N0 to N5 are refactors with no knob, gated by same-build byte identity on both goldens (card PRM's harness `scripts/prm_ab.py`); N1 and N2 add trace-class tables and counters only; N6 is behaviour-changing and gets its own card
- **dates in the text:** 2026-10-08
- **hashes / run dirs:** the map was read on main `a4cda77`; branches `nom/n0a-n1` (`1752737`, `deca912`), `nom/n1b` (`d6f93cf`), `nom/n0b` (from `2b7e804`) in a session worktree; exes `gnss_processor_gal2.exe` (`df936f39`, the clean pre-step build), `gnss_processor_nom1` to `nom8`; G0 arms `runs/_rap_nom{1..8}_*`, the combined-tree check `runs/_rap_main1_{f2,s2}`
- **gates (frozen at pre-registration):** section 6

**Outcome.** Question: can the network producer be organized around explicit objects a human can follow? The author chose plain types, N0 and N1 first, and clarity over byte identity (section 10). N0a (typed identities, one constellation traits table) and N1 (every station column classified, with a declared disposition, and the "parameters as armed" table) are built, reviewed and byte-identical on four arms (section 12); N1b accounts for every withheld instance (section 14); N0b and N0c put the network's identity handling through the typed layer (satellite first-character reads 173 to 3, section 18). The table shows the receiver L5 delay held at its seed with no reading of code. Next: N2 (section 17).

## 1. Why: the defects of the last three weeks share one shape

A newcomer who wants to follow one Galileo satellite clock (E05) from its first observation to the emitted C record visits about 28 files and meets five spellings of its key (`E05|SC`, `E05|CLK`, `CLK:E05`, `CLK|E05`, `CarryPrior{Clock,"E05"}`). Following one phase cycle slip touches every carry container a response might reach, found by reading the responses, not by asking the slip's arc. The network's run state is organized by the card that added each piece: `NetworkCarryState` holds about 188 containers (139 with a string in the key, 40 packing a composite identity into one string with six separator conventions), `NetworkKnobs` 334 members, `NetworkServiceCounters` about 750; no section of it is named after a station, a satellite, a signal or a line of sight.

The recorded defects are mostly two places deriving one identity differently, or an identity that was never stored:

| card | what happened | the missing object |
| --- | --- | --- |
| L5R | the receiver's band-5 code delay is dropped from the network solve by a substring test (`find("_IFB")`, `ApplicationNetworkAssembly.cpp` around 161 to 176) whose justification (GIM weighting) the goldens do not satisfy; the registry still states the receiver code bias `estimated = true`; station-constant band-5 offsets up to 29 m | a parameter instance with a declared treatment (estimated, held at its seed, dropped and why) |
| GRK | the arc tracker named band 1 by a lexical pick (L1C) while the carry was keyed by the station's latch (L1W): every band-1 gap re-entry at L1W stations missed | one per-station signal plan that both read |
| CSL, CPA | each station's signal per band was a side effect of the first epoch's satellite count; an emulation's gain belonged to a latch move nobody could see | the plan, printed and diffed between arms |
| WUP | the wind-up restart response walked the live ambiguity map, which a re-entering arc no longer has: dead wiring for a month | one arc object owning both the live column and its carry |
| GAL | the geometry-free pin gave the broadcast target only to a (band 1, band 2) pair, so Galileo was pinned at zero | constellation traits naming each navigation message's group-delay pair |
| PRM | card LGM's `pc_carry` was in none of the hand-written lists of references a re-base must move | per-line-of-sight parameter sets, members by construction |
| CTU | the coupling rows and the fold read two variances of one clock decision | rows that reference the parameter, not a cached copy of its property |
| RXP | blank-padded satellite ids dropped satellites below G10 in 16 files | one satellite-id type with one parser |
| OWD O5 | post-fit rows written without a signal | a row tag carrying its observation identity |
| SID phase 2 | the datum seam keyed a constellation by letter, the roster by name: no datum cell written | one constellation type |

The organization map also found suspected defects of the same shape that no card records (section 7). They are owed reads, not findings.

## 2. What the user filter does well, and what it does not

The user-side filter reads well for four reasons: one owner of the state (`StateManager`, about 110 documented methods), one header for every key format (`StateKeySchema`), one configuration block per property (`filter.process_noise`, `filter.initial_std`), and a straight-line pipeline (`Application.cpp` 941 to 987). It is not an object model, and it has its own scatter: no per-category descriptor (one category's key, birth gate, Jacobian gate, variance, q, reset, prune, label and snapshot tag sit in about eight places), nine categories enumerated by hand in at least seven lists that no longer agree, the observation equation written three times (`buildJacobianRow`, `computeRawResidual`, `computeAmbiguitySeed`, the last with its own frequency table), 102 source lines that build an ambiguity key by concatenation, and `makeIfbKey` / `makeDcbKey` with no caller anywhere in `src/`.

The network inherits part of this directly: every station carries a full user-side `StateManager`, `PppModel` and `MeasurementModel` (`NetworkRunTypes.hpp`, `NetStation`), whose H is re-mapped by key into the border or the local block (`networkSplitStation_`); after that the station's state manager is a column registrar and an `x_lin` store, and its covariance and q are never read. Copying the user filter's organization would import its scatter. The target is what makes it readable (one owner, one schema, a linear story) applied to entities, which the user side lacks too. What this card builds can later move to the user side; it does not start there.

## 3. The model

Five layers, each a set of plain value types and tables. No base class, no virtual dispatch, no parameter or estimator hierarchy (card PRM principle 1 kept). Strings remain the serialization (keys in snapshots, the wire, traces, fingerprints); in memory, identities are typed.

### 3.1 Identity (one header, one traits table)

| type | holds | replaces |
| --- | --- | --- |
| `Constellation` | enum (IRNSS included) plus traits: letter, name, ISB name, FDMA, network role, the BIA clock-reference pair, the broadcast group-delay pairs per navigation message, the band roles (primary pair, third band, lanes) | four representations ('E', "E", "Galileo", the `System` enum), ten private converters with four default policies, 38 letter comparisons and 30 `case 'G'` arms in the network code |
| `Band` | constellation, RINEX digit, name, nominal frequency, slot dependence (GLONASS) | four band representations, about six frequency sources that disagree on GLONASS FDMA and BeiDou digit 1, 46 `sig[1]` reads |
| `Signal` | band, attribute, tracking family (one definition) | the bare RINEX string; the two contradictory "channel family" definitions (`NetworkSbasis.cpp` 278 to 298 against `NetworkDatumGraph.cpp` 229 to 234) |
| `SignalPair` | an ionosphere-free or pin pair as its own type | the "C1C_C2W" string that collides with the key separator |
| `SatId`, `StationId` | normalized once (the GLONASS slot on the satellite) | per-reader normalization (card RXP) |
| `LosId`, `ArcId` | (station, satellite); (station, satellite, signal, generation) | `std::pair<int,std::string>` with five meanings; the arc ledger's tuple key |

Comparators reproduce today's string order exactly (a `SatId` compared as letter then zero-padded PRN does), so every ordered container keeps its iteration order and every rank-1 update its order.

### 3.2 The plan (what is modelled, per station)

`StationSignalPlan`: per (station, constellation, band) the modelled phase, the modelled code, the companions (the second code channel), the datum role (pin-pair member), and every excluded signal with its reason. `ConstellationPlan`: the pin pair, the clock pivot, the band roles in force. This is card SID's `SignalRoster` made authoritative: the latch, the tracker, the carry keys, the pin rules and the second channel read it instead of deciding again (twelve independent selection rules today). Elections that legitimately pick an element (the pivot, the anchor) stay elections and record their result here.

### 3.3 Parameters (every instance, with a declared treatment)

Each parameter instance is (kind, subject identity) with a reference to its `ParamSpec` (card PRM's registry, extended with the sub-kinds the registry merges today: the receiver code bias splits into DCB, IFB per band, ISC per signal and the GLONASS slot) and one declared `Treatment`:

- `Estimated{route}`: a column of the border or of the station's local block, with the carry route PRM declares;
- `HeldAtSeed{source}`: in the model at a fixed value (today's GPS_IFB5);
- `Absorbed{by, valid_when}`: deliberately left out because another parameter takes it up, with the condition under which that holds (the IFB drop's GIM premise, stated where a loader can check it);
- `Pinned{datum}` and `Dropped{reason}`.

An inventory pass after the station split compares what each station allocated with what reached its block and the border, and counts instances per (kind, constellation, band, treatment). The run-start and run-end tables "parameters as armed" print one line per instance class: "receiver IFB, GPS band 5: held at seed on 58 stations (dropped: estimate_sat_code_bias)" is the line card L5R had to find with a residual census.

### 3.4 Contributions (every row of the normal equations has a kind)

A static `ContributionSpec` table, one record per family (about 32 today: data code and phase rows, the second channel's companions, the IF and geometry-free pins, the zero inter-signal assertions, the sum-to-zero levels, the S-basis rows, the station priors before and after the PD test, the dense and diagonal border folds, the local carry rows, the whitened local block, the receiver code bias in block, the coupling rows, the anchor rows, the leveling rows, the completion rows): kind, subject type, parameter kinds touched, channel, stage relative to the rank gate, weight source (knob or named constant), replay policy, attribution tag, counter. Every builder that appends rows appends a plain tag per row (kind and subject ids) to a side vector; whitened blocks carry one tag per block. Classifiers (the post-fit RAIM pool, the post-fit row trace, the frame diagnostics, the RHS attribution, the within-epoch replay) read the tag instead of inferring the kind from coefficient magnitudes or label prefixes (seven inference schemes today). A run-start table "rows as armed" lists every family with its builder, weight and live count, and a family armed with zero instances is printed (standing invariant 4 made structural).

### 3.5 Run state owned by entities

| entity | key | takes over (examples from `NetworkCarryState`) |
| --- | --- | --- |
| `ConstellationState` | `Constellation` | pivots (`prev_pivot`, `last_pivot`, the election result), the frame hold's per-constellation median, the multi-GNSS counters; about 14 containers |
| `SatelliteState` | `SatId` | the clock carry entry and its border index, the Q3 anchor, the clock level, the oscillator, the re-admission seeds, `prev_clk`, the IF-pin latches, the discontinuity queues (one cause bitmask); about 49 containers |
| `SatSignalState` | (`SatId`, `Signal`) | the phase-bias and code-bias carry entries, the hold, the anchor incumbent and pin target, the held basis, `pin_bridge`; about 20 containers |
| `StationState` | `StationId` | the station bundle (`NetStation`), the receiver clock, ZWD, receiver code-bias and receiver phase-bias carries; about 14 containers |
| `LosState` | `LosId` | the slant ionosphere carries, the phase colour carry, the gross holds, the prefit flags; about 20 containers |
| `ArcState` | `ArcId` | the ambiguity carry, the response flags (re-float, re-level, free epoch), the arc's lifecycle; about 16 containers; card SID's arc ledger promoted from observatory to owner |

Each field declares its persistence (snapshot record, fingerprinted, cold-start by design) and the entity its lifecycle (absent, present, departed with history, returned), which makes invariant 7's two edges visible: a return is never read as a first sighting. Responses address the entity: a slip on (WTZZ, G05, L2W) opens one `ArcState` and its line of sight, and every carry the response must move is a field there, by construction (the traversal card PRM built becomes iteration over fields). The dense covariance stays where it is, contiguous Eigen blocks indexed by column inside the estimator; an entity holds ids, scalars and the index of its column, never a slice of a covariance.

A per-epoch `EpochEvents` record, built fresh every epoch, replaces about 50 `this_epoch_*` / `ep_*` fields that live in the cross-epoch struct and are cleared by hand, and the 18 "pending" fields that carry post-emission responses into the next epoch.

### 3.6 The reader's view

The tables a human reads at run start and run end: signals as planned (per station), parameters as armed (per instance class and treatment), rows as armed (per family), and PRM's model as armed (per kind). Following E05 means one `SatelliteState`, its `ConstellationState` and its `SatSignalState` entries; following a slip means one `ArcState`. The driver's seam calls (75 calls with 807 positional arguments today) pass entity tables instead of lists of aliases.

## 4. Principles (kept from card PRM, plus three)

1. Consolidation, not abstraction: plain value types and tables; no virtual dispatch, no base classes; no merge of the user `StateManager` with the network's model.
2. Declare, then check, then replace: each new declaration first runs beside the code it describes with a mismatch counter, and replaces it only at zero on both beds.
3. Byte identity for every step that claims no behaviour change (PRM principle 3), with the PRM harness and, for pure moves, the object-code identity method.
4. JSON knob names, snapshot record formats, the wire and trace column names do not change.
5. The registry and the tables are read, never written, inside the epoch loop; per-instance dispositions stay state on the instance.
6. (new) Strings are a serialization, never an in-memory identity; every new container takes a typed key.
7. (new) Dense numerical blocks stay index-based; objects resolve to index vectors before PASS B, and the parallel produce phase writes only per-station slots (decomposition plan section 2).
8. (new) No long-lived branch: each step lands on main within days, coordinated with the concurrent sessions that edit the same files.

## 5. Steps

| step | what | behaviour | size (estimate) |
| --- | --- | --- | --- |
| N0 | identity types and the traits table; one frequency function; the ten converters and the ad-hoc letter and digit tests in the network code read the types; comparators proven order-preserving by unit test | byte-identical | one to two sessions |
| N1 | parameter instances with treatments; the inventory after the split; "parameters as armed"; counters at every drop site (`drop_cols` first) | trace and counters only | one session |
| N2 | contribution specs and row tags; "rows as armed"; then each classifier switched to the tags, one per step, with a disagreement counter run first | tables byte-identical; a classifier switch that changes a QC decision is a behaviour change, graded on its own | two to three sessions |
| N3 | the signal plan authoritative: the readers (the latch, the tracker, the carry keys, the pin pair, the second channel) switched one at a time with a mismatch counter at zero | byte-identical per reader | two sessions |
| N4 | entity tables own the run state, one entity per step (constellation, satellite, satellite-signal, station, line of sight, arc), with persistence declared per field; the snapshot writer and reader iterate the declared fields and write the same records | byte-identical per entity | four to six sessions |
| N5 | `EpochEvents`; the seam signatures take entity tables | byte-identical | one to two sessions |
| N6 | the network owns its local parameter set (no borrowed user `StateManager`) and one observation-equation function, returning its term list, serves the producer's H, the consumer's H and the seeds | behaviour-changing: its own card, graded on both beds | later |

Interleaving with the science program: N0 and N1 come first, because both open work depend on them. Card L5R's next step R-1b (estimating the receiver band-5 delay with its datum) is a treatment change from `HeldAtSeed` to `Estimated`: it is built on N1 as the first parameter added the new way. Card GAL2's owed per-family geometry-free pin is a declared property of the Galileo signal families and the pin pair: built on N0 and N3. N2, N4 and N5 continue beside the science cards.

## 6. Gates (frozen)

- **G0, byte identity (every step N0 to N5).** `scripts/prm_ab.py`, same build flags: products (comment lines stripped), `closed_loop_trace.csv` and `network_epoch_status.csv` of 2 h arms from both goldens identical to the pre-step build; plus the golden postures `program`, `eq34`, `handover` of the PRM harness where the step touches their code.
- **G1, coverage.** N1: every allocated local instance on both goldens and on one three-band and one GPS+Galileo arm appears in the inventory with a treatment; zero instances without one. N2: every row of every block carries a tag on the same four arms; zero untagged rows. N4: every field of `NetworkCarryState` that a step moves is listed in exactly one entity with a persistence declaration.
- **G2, the reader test.** After N1 and N2: the run-start tables of the GPS+Galileo field arm and of the three-band field arm, printed beside this card, must show the IFB5 treatment, the C5X zero assertions, the second channel's companions and the deweighted rows without reading code. After N4: the E05 and slip traces of section 1 are re-walked and their file count reported.
- **G3, cost.** Wall time within +3 % on the field golden at each step.

## 7. Suspected defects found by the map (owed reads, not findings)

1. The post-fit RAIM pool may admit carried-prior coupling rows and whitened block rows as phase data (row identity inferred from |A_r| above 0.5 on an ambiguity column; a scalar coupling row carries exactly 1); the MDB rider would then label them as phase observations and the prior-residual rider count them on the data side. Read before N2's classifier switch.
2. The RHS attribution reads the carry channel and not the pre-PD station prior channel that `network.station_prior_before_pd` routes the receiver code-bias datum rows into; on the golden its closure should report a violation every epoch. Not run.
3. Two row classifiers disagree on code rows of a satellite with no code-bias column (the classifier says other, the post-fit row trace says code).
4. The GIM ionosphere rows are called every epoch on the golden and return without a counter (inert), while the call-site comment states they apply.
5. The IF pin pair is chosen by a lexical sort independent of the station's latch; on the golden it falls on the companion C1C, whose rows are deweighted by 10 (card SCC lists the pair's misfit as open).
6. The user-side slant ionosphere of a PPP run with uncombined observations gets zero process noise (the q gate tests the processing mode, not the creation predicate); two test-suite configs hit it.
7. BeiDou digit 1 maps to B1I (1561.098 MHz) in one frequency source and to B1C (1575.42 MHz) in three others; BeiDou is off on the network.

## 8. Risks and non-goals

- **Byte identity is the binding constraint.** Iteration order of string-keyed maps is numerically load-bearing (the inject order of rank-1 updates, the fold order, the lexical reference choices). Typed keys keep it only with order-preserving comparators; a re-key that cannot preserve order (station name to station index, an IF pair to a tuple) is named in its step and either kept as is or re-baselined by its own card.
- **Migration size and concurrency.** About 80 network files and 75 000 lines; the concurrent sessions edit `NetworkRunContext`, the estimator and the loader daily. Small steps on main, announced, with the files they touch listed in the step's section.
- **The model must not become a second registry that drifts.** Each declaration replaces the code it describes (principle 2); a declaration that cannot be wired is deleted, not kept.
- **Producer and consumer agreement.** The local block reuses the user-side H, keys and seeds on purpose; N6 keeps one observation equation shared by both, never two.
- Non-goals: no change of what the solve estimates in N0 to N5; no knob renames; no snapshot format change; no unification of the eight epoch-skip payloads or of lifecycles that differ on purpose (decomposition plan section 11); no merge with the user-side state manager.

## 9. Decisions owed by the author

1. The form: plain value types, traits tables and entity tables with no virtual dispatch (this card), against classes with behaviour (a `Parameter` with `onSlip()`), which card PRM's principle 1 ruled out. This card assumes the former.
2. The order: N0 and N1 now, with L5R's R-1b and GAL2's per-family pin built on them, and the remaining steps beside the science cards; or a pause of the science until N4.
3. The bar: byte identity at every step (this card), or a single declared re-baseline where an order cannot be preserved.
4. Coordination with the concurrent sessions that edit the network files: a short quiet window per step, or a file lock list per step.

<!-- Results are appended below this line under dated headings. -->

## 10. 2026-10-08: the author's decisions

The author: "Plain types, N0 and N1 first, byte identity is not as important to me as the clear, well-structured and organized project."

1. **Form:** plain value types, traits tables and entity tables; no behaviour classes, no virtual dispatch.
2. **Order:** N0 and N1 first.
3. **The bar:** clarity and structure take precedence over byte identity. Gate G0 changes from a pass condition to a measurement: every step still runs the same-build golden comparison and reports whether the output moved; a step that moves it states why (an iteration order, a corrected inconsistency) and shows on both beds and the 16-site census that nothing got worse beyond the run-to-run noise of card HLT section 4.4. A step whose move cannot be explained is not landed.
4. **Coordination:** not decided. Until it is, N0 lands in two parts: N0a adds the new types as new files (no existing file changes except the build list), and N0b migrates the call sites after the concurrent sessions' uncommitted work in the network files is committed.

## 11. 2026-10-08: N0a and N1 specified (pre-registered before the build)

Built in a clean worktree from main `a4cda77`, so that no session's uncommitted hunk rides along; the pre-step build is `gnss_processor_gal2.exe` (`df936f396109e367`, the same commit, clean). The patch is brought into the main tree hunk by hunk.

### 11.1 N0a: the identity types (`include/common/GnssIdentity.hpp`, `src/core/common/GnssIdentity.cpp`)

The existing `System` enum (`common/GnssTypes.hpp`) is the constellation and `FrequencyBand` (`common/SignalBand.hpp`) the physical band: N0a adds no second enum for either (a fifth representation is the defect, not the cure). It adds:

- `ConstellationTraits` and `const ConstellationTraits& traitsOf(System)`: the RINEX letter, the name used by `PppUtils::getSystemString` and the configuration ("GPS", "Galileo", ...), the system-time-offset key name of `StateKeySchema`, the FDMA flag, the physical bands of the constellation in RINEX digit order. One documented letter policy, `System systemFromLetter(char)`: unknown letters (IRNSS 'I' until N0b adds it to the enum) map to `System::Unknown`, never to GPS; `systemFromName(std::string_view)` accepts the names and the letters.
- `SatId {System sys; int prn;}`: `parse` normalizes "G05", "G 5" and "G5" (card RXP) and refuses anything else; `str()` renders "G05"; ordering is (letter, prn), which reproduces the lexical order of the strings it replaces.
- `SignalId {System sys; char digit; char attribute;}`: a tracking signal such as GPS "1C"; `fromObsCode(System, "C1C" or "L1C")`; `band()` through `classifySignal`; `obsCode(ObsType)`; `frequencyHz(glo_slot)`; ordering (letter, digit, attribute).
- `ObservableId {SignalId signal; ObsType type;}` ("L1C"); `SignalPair {SignalId a, b;}` with the canonical rendering "C1C_C2W" that the ionosphere-free ambiguity keys already use.
- `StationId {int index;}`, `LosId {StationId, SatId}`, `ArcId {StationId, SatId, SignalId}` with the same ordering rule (the arc generation joins in N4).
- One frequency function, `double carrierFrequencyHz(FrequencyBand, int glo_slot = 0)`, and `wavelengthM`. N0a does not migrate callers; it tests the function against `PppUtils::getFrequency` for every classified code and records the disagreements it finds (BeiDou digit 1, GLONASS slot 0) as N0b's decisions.

Tests (`tests/test_gnss_identity.cpp`, suite `GnssIdentityTest`): the comparators against `std::string` order over every satellite of five constellations and every signal of the classifier's table; parse and render round trips, the blank-padded forms included; the letter, name and key maps against the existing helpers; the frequency function against `PppUtils`.

### 11.2 N1: the parameter inventory (`include/filter/NetworkParamInventory.hpp`, `src/core/filter/NetworkParamInventory.cpp`)

- `ParamTreatment {Estimated, HeldAtSeed, Absorbed, Pinned, Dropped, Unobserved}`. `ParamInstanceId`: the registry kind, the receiver code-bias form where the kind is `RxCodeBias` (`Dcb`, `Ifb`, `Isc`, `GloSlotIfb`), the constellation, the satellite, the signal or band digit and the station, built by ONE classifier from a `StateKeySchema` key (every family the station's state manager can hold: position, receiver clock, ZWD, inter-system bias, slant ionosphere, phase colour, ambiguity, receiver phase bias, DCB, IFB, ISC, GLONASS slot IFB, satellite clock, satellite phase and code bias). A key the classifier does not know is counted as `unclassified` and printed, never dropped.
- The receiver IFB drop becomes a declared rule, `RxIfbRule networkRxIfbRule(const GnssConfig&)`: whether non-primary receiver IFB columns are dropped (today: whenever the satellite code biases are estimated), the treatment the dropped instance gets (`HeldAtSeed`, source "first-epoch seed, StateInitializer"), the premise the drop rests on, stated as text ("GIM weighting leaves the station ionosphere level free to absorb the band's receiver delay"), and whether the premise holds on this run (ionosphere weighting enabled and the W6 ionosphere constraint not `none`). `networkSplitStation_` reads the rule instead of testing `estimateNetworkSatCodeBias()` itself; the drop's columns are unchanged.
- After each station's split, the inventory records every instance of the station's state manager with its treatment: a border column `Estimated` (route border), a local column `Estimated` (route local), a coordinate `Pinned` (station coordinates held), a dropped IFB per the rule, a column with an all-zero H row this epoch `Unobserved`. Records are written to the station's own slot inside the parallel PASS B and drained serially.
- Tables: "parameters as armed" written once after the first emitted epoch and again at run end, one line per instance class (kind, form, constellation, signal or band) with the instance count per treatment, and one WARNING line per class whose rule's premise does not hold on this run with instances present, e.g. "receiver IFB, GPS band 5: held at its first-epoch seed at 58 stations; dropped because the satellite code biases are estimated; premise (GIM weighting) does not hold on this run". The same content as a `param_inventory` record in the run manifest.

Gates (section 10's bar): G0 measured on 2 h arms of both goldens against `gnss_processor_gal2.exe` (N0a and N1 change no estimate, so the records, the consumer trace and the epoch status are expected identical; a move is a defect of the step); G1 every instance on the two goldens, a three-band arm and a GPS+Galileo arm has a treatment and zero are unclassified; G2 the three-band arm prints the IFB warning without anyone reading code; the full unit suite passes.

## 12. 2026-10-08: N0a and N1 built and landed in the working tree

Built by a worker on branch `nom/n0a-n1` (worktree from `a4cda77`), reviewed by an independent reviewer (verdict: merge after four fixes), fixed in a second round, read by the manager, and applied to the main working tree as a patch beside the concurrent sessions' uncommitted hunks (all 16 files applied cleanly). Commits on the branch: `1752737` (the build), `deca912` (the review fixes). Not committed on main.

**What a reader gets.** `include/common/GnssIdentity.hpp`: one traits table per constellation (letter, name, FDMA, physical bands by RINEX digit), `SatId`, `SignalId`, `ObservableId`, `SignalPair`, `StationId`, `LosId`, `ArcId`, one frequency function, and an ordering contract that keeps every future re-keyed container in today's order. `include/filter/NetworkParamInventory.hpp`: every column of every station's state manager classified by one classifier into a parameter class, paired with what the station split did with it (one `ParamDisposition`: estimated on the border, estimated in the station block, held at its seed, absorbed, pinned, dropped, unobserved), the receiver IFB drop as one declared rule with its premise, and the "parameters as armed" table after the first emitted epoch and at run end, with a `param_inventory` record in the run manifest.

| gate | result |
| --- | --- |
| G0 | IDENTICAL against `gnss_processor_gal2.exe` on both 2 h goldens, on the three-band arm and on the GPS+Galileo arm (the two arms where the new drop rule runs), 120 to 240 epochs, `ident2h.py`; re-checked by the reviewer and by the manager |
| G1 | 0 unclassified columns and 0 columns without a disposition, at the first epoch and over the run, on the four arms |
| G2 | the three-band arm prints, at run start: "WARNING: receiver IFB, GPS L5: held at its first-epoch seed at 58 station(s); dropped because the satellite code biases are estimated (network.estimate_sat_code_bias); premise (GIM weighting ...) does not hold on this run" |
| unit suite | Debug, worktree: 2 879 tests, 2 858 passed, 21 skipped (fixture data absent in a fresh tree), 0 failed; 22 new cases (`GnssIdentityTest`, `NetworkParamInventoryTest`) |
| G3 cost | idle box, alternating sequential 2 h field arms: 100.85 s against 101.6 s (+0.7 %), 314.5 against 314.8 ms per epoch; run-to-run spread about 2 % |

**Recorded for N0b** (the identity disagreements N0a found and did not fix): BeiDou C1I / C1Q read as B1I (1561.098 MHz) by `classifySignal` and the new function, as B1C (1575.42 MHz) by `PppUtils::getFrequency`; a GLONASS FDMA satellite with no known slot gives 0.0 from `getFrequency` and slot 0 from the new function (now an explicit choice, `nominalCarrierFrequencyHz`); `StateInitializer` still builds the IFB and DCB keys by concatenation.

**Owed reads the inventory surfaced.** More ambiguity columns than satellite phase-bias columns on some families (field L1W 148 against 143; three-band L5Q 102 against 101; Galileo L1C 157 against 153, L5Q 150 against 147): most likely the allocation gates of `MeasurementModel.cpp` around 1266 to 1293 (unmatched antenna, coverage, allowlist), whose comments say the float ambiguity absorbs the withheld bias. That is the `Absorbed` disposition, and the next step (N1b) counts it at those sites. One field station-epoch in 13 920 never reached the split.

## 13. 2026-10-08: the author's directive and what it adds to the plan

The author: "The codebase should be so incredibly organized that I can read it and follow easily, and anyone new can immediately begin contributing." Recorded in `CLAUDE.md` (Conventions, "Organization").

Today the onboarding sends newcomers past the network: `docs/onboarding/README.md` calls about two thirds of the repository out of scope, every `Network*` file included. The directive makes that the target. Every NOM step from here lands with the reader's page it affects, and the network gets its own code tour (`docs/onboarding/`), built on the objects as they land: one satellite, one signal, one parameter, one row and one event, each followed through the tables and the entities instead of through the files. Next, in order: N1b (the `Absorbed` count at the allocation gates; small), N0b (the call sites read the identity types, after the concurrent sessions commit their network files), N2 (row kinds), then the network code tour's first page.

## 14. 2026-10-08: N1b, the absorbed instances (built, applied to the working tree)

Branch `nom/n1b` (`d6f93cf`, from `deca912`), reviewed by the manager, applied as a patch. Every shared state a gate withholds from a station is noted with the gate's reason (`AbsorbedReason`) on the station's `MeasurementModel` (network mode only; cleared at the start of the shared-state allocation, `ensureNetworkSharedStates`, which runs in PASS A) and counted after the split as `Absorbed`, once the station is confirmed to hold the absorbing column (the ambiguity for a phase bias, the satellite clock for a code bias; a record without it goes on its own integrity line).

The finding: none of the five allocation gates of section 11's owed read withholds anything on the four arms. Every gap comes from a later rule, the anchored-union coverage gate of `networkBuildPhaseDatumCandidates` (`network.phase_datum_min_stations`, 2 on the goldens), which prunes the phase bias of a key fewer than two stations hold; the eq34 hygiene prune (`networkAnchorHygiene`, not armed) is the same mechanism and is counted too. With those two reasons, border plus absorbed equals the ambiguity count for every satellite phase-bias signal on all four arms, at the first epoch and over the run (field L1W 35 081 + 1 039 = 36 120; GPS+Galileo: Galileo L1C 18 212 + 484 = 18 696, L5Q 17 666 + 333 = 17 999; three-band L5Q 12 364 + 118 = 12 482). The one field station-epoch that never reached the split had no usable observations (now its own line, by cause).

Also checked and corrected in comments: the GLONASS code-bias branch cannot run in a loaded configuration (the loader refuses `estimate_sat_code_bias` with `estimate_glonass`); a station excluded for an unmatched antenna does not feed "its own position" (the network holds coordinates). G0 IDENTICAL against `gnss_processor_gal2.exe` on both 2 h goldens and the 1 h three-band and GPS+Galileo arms; unit suite 2 884 run, 2 863 passed, 21 skipped, 0 failed. Open: the five allocation gates are exercised by unit tests only.

## 15. 2026-10-08: N0b specified (pre-registered before the build)

N0b moves the network code's identity handling onto the N0a types. It changes no container key (N4) and no rule that chooses a signal or a band role (N3): every decision stays as it is, expressed through typed identities, and every site whose logic is a band ROLE or a signal CHOICE is listed for N3.

What moves, in the network files (`src/core/filter/Network*.cpp`, `src/core/app/*Network*.cpp`, `src/core/app/SignalRoster.cpp`, `src/core/slip/Network*.cpp`, `src/core/products/Network*.cpp`, `src/core/logging/NetworkRunTraces.cpp`, their headers) and `src/core/measurement/StateInitializer.cpp`:

1. The ten private constellation converters (organization map table T2) are deleted; callers read `systemFromLetter`, `systemFromName` and `traitsOf`. One letter policy (an unknown or SBAS letter is never GPS).
2. A satellite string's constellation is read in one place, `systemOfSat(std::string_view)` (added to `common/GnssIdentity.hpp`), until N4 re-keys the containers by `SatId`; letter comparisons and `case 'G':` switches become `System` comparisons.
3. Band-digit reads of an observation code become `SignalId` / `ObservableId` (and `FrequencyBand` where the intent is the physical band).
4. Carrier frequencies and wavelengths in these files come from `carrierFrequencyHz` / `wavelengthM` (the FDMA slot explicit); the GPS-only frequencies of the ambiguity trace (`network.trace_amb_if`) are replaced, so the trace also writes Galileo lines (a trace change, not a solve change).
5. Keys of `StateKeySchema` families are built and parsed through its helpers (the receiver IFB and DCB keys of `StateInitializer` included).
6. Estimator column labels are read through `NetworkEstimator::sharedColumnIdentity`, not by `rfind("PB:")`-style prefix parses (27 sites in 9 files).

Out of scope: the composite string keys of `NetworkRunContext` containers (N4), IRNSS in the `System` enum and the BeiDou digit-1 frequency of `PppUtils` (user-side decisions, separate), the uncommitted lines of the concurrent sessions (migrated after they commit).

Gates (section 10's bar): G0 against `gnss_processor_gal2.exe` on the two 2 h goldens and the 1 h three-band and GPS+Galileo arms, expected IDENTICAL (a move is explained or the step does not land); the unit suite; the pattern counts of the organization map's table T5 before and after, by file; the list of band-role and signal-choice sites for N3; an independent review.

## 16. 2026-10-08: N2 groundwork, the suspected row-classification defects read

Two read-only investigators checked section 7's suspects 1 to 4 against the code and the runs `_rap_nom6_new_f2`, `_rap_nom6_new_s2` (2 h goldens) and `_rap_gal2_ctl_f6` (6 h field); scripts in the session scratchpad (`n2check`). The manager re-read the pool loop (`NetworkRaim.cpp` 329 to 333) and re-ran the T4 replay on the field run.

| suspect | verdict | consequence on the golden |
| --- | --- | --- |
| 1, the post-fit RAIM pool admits carried-prior rows as phase data | CONFIRMED. The only admission test is \|A_r(row, ambiguity)\| > 0.5 ("not a phase row of this ambiguity"); the float vehicle's whitened ambiguity block rows and the receiver code-bias in-block rows clear it; the within-epoch tier (`NetworkSolveGlue.cpp` ~1601) repeats the rule. | Solve-affecting. A replay of the T4 isolated-arc criterion on the producer's own table reproduces every emitted `T4_attrib` event (12 of 12 on the 2 h field, 26 of 26 on the 6 h field, 0 on the sim); on a data-only table the same criterion keeps 1 of 12 and 2 of 26. Example: LCK400IND G04 refloated at r = 9.45 against a data maximum of 0.14. Real data spikes at KIT300UZB (TOW 263670 to 263730, G21 132, G08 41) are partly masked by one prior row absorbed into the station effect. The prior-residual census counts block rows on both sides (statistic only). sigma0, URA, T3 and A4 unchanged on these runs (post-fit sigma0 at its 1.0 floor), latent otherwise. |
| 2, the RHS attribution omits the pre-PD station prior channel | CONFIRMED in code; the closure has never executed on this posture (0 era trips over 425 armed run directories) | diagnostic only |
| 3, two row classifiers disagree on rows without a bias column | CONFIRMED (documented as an open gap in `NetworkFrameDiag.hpp`): 2 code rows and 30 phase rows (the first 6 epochs, before the carry is warm) filed differently | statistics only; the census and trace pools, the H-record sigma0 by one order statistic at about 8 epochs |
| 4, the GIM ionosphere rows called and silently inert | CONFIRMED: about 13 900 calls per 2 h field run, 0 rows, no counter; the call-site comment is misleading | none |

**What this sets for N2.** N2 tags every contribution where it is created (rows of the station blocks, the diagonal prior channels, pins, coupling and birth rows, the shared rows and priors), and every reader takes the kind from the tag. Where today's behaviour depends on the misclassification, N2 keeps the behaviour and states it explicitly: the RAIM pool admits "data phase rows plus prior rows that clear 0.5 on the column" as a declared rule with a counter per kind, so the step changes no decision. Classification by declared observation type moves the statistics of suspect 3 (census pool skips 20 to 0, 30 trace rows from code to phase) and is reported as such.

**A separate card, not part of the reorganization:** admitting data rows only to the RAIM pool changes T4 decisions on the golden (11 of 12 refloats gone, masked spikes flagged). It is a science change: a default-off knob, a field and sim A/B with the census, and the two ready-made events (BSHM00ISR TOW 259470 to 259590; KIT300UZB TOW 263670 to 263730). Arming is the author's decision.

## 17. 2026-10-08: N2 specified (pre-registered before the build)

N2 gives every contribution to the normal equations a declared kind, in two parts.

**N2a, declare and tag (no reader changes; byte-identical, traces included).**
- `include/filter/NetworkContributions.hpp`: `ContributionKind`, one value per family of section 16's inventory (station data rows: code, companion code, phase, second phase; station constraint rows: ambiguity pins, eq34 pins and rank floor and time link, GLONASS anchors, SH ties, GIM ionosphere rows, SD-EWL rows; station block rows: scalar coupling rows, the whitened local block per family (ambiguity, wet delay, ionosphere, receiver clock, receiver code bias, phase colour), LGM birth and backstop rows; station priors: the post-PD diagonal priors per family and the pre-PD receiver code-bias datum; shared rows: each satellite code-bias datum row type, the clock constraint rows, the S-basis completion rows; shared priors: Kaula, phase-bias heal, SH prior, phase hold, pin bridge, the border carry in its diagonal, gauge-free and dense forms, the Q3 refold, clones and walks and shared history, the clock oscillator, the cold clock anchor; pins: the clock pivots; gauges: the ensemble phase gauges; the DD prior blocks), and one `ContributionSpec` per kind (name, channel, stage relative to the rank gate, subject, parameter kinds touched, weight source, whether it is observation data).
- Every site that appends rows or folds a prior records the kind: a plain tag per row of each station block (kind, satellite, signal), aligned with `A_r` / `A_s` and kept aligned through the within-epoch re-whiten; a kind on every shared fold (replacing the free-text attribution tags).
- A "contributions as armed" table after the first emitted epoch and at run end: per kind, the rows or folds this epoch and over the run, the builder, the weight source; a declared kind with zero instances is printed (the GIM rows: 0 rows, no GIM loaded).

**N2b, the readers take the kind (explained trace moves; the solve unchanged).** The row classifier, the post-fit row trace, the RHS attribution (iterating exactly the channels the kernel folds, the pre-PD prior included), the MDB rider and the RAIM pool admission read the tag. The RAIM pool keeps today's admission population as a declared rule ("data phase rows, and the ambiguity and receiver code-bias block rows whose coefficient on the column exceeds 0.5"), with a counter per admitted kind and the same visit multiplicity, so no QC decision moves; changing that rule is the separate science card of section 16. Expected trace moves, each explained: 30 early phase rows filed as phase instead of code, the census pool skips 20 to 0 and 3 to 0 on the field, the code and phase post-fit sigma0 at 6 epochs, the H-record sigma0 at about 8 epochs.

Gates: G0 on the four arms (products, consumer trace, epoch status identical; N2a also every trace); the table lists every kind with 0 untagged rows on the four arms; a unit closure test of the RHS attribution with a non-empty pre-PD prior and one forced attribution dump on a 2 h golden copy (`network.rhs_attrib_tow`); the independent review. The user-side measurement header stops including network headers (the absorbed-instance record moves to its own small header), the one should-fix the commit review left.

## 18. 2026-10-09: N0b and N0c landed (`029b06c`)

N0b was built on branch `nom/n0b` (stage-1 helpers `2b7e804`; four workers on disjoint file sets, merged at `d0ed4a5`; cleanup `d86faa7`), reviewed (merge after eight fixes), fixed (`b3eb57f`), then applied onto main `c7e0a9a` together with N0c (the same six items for the PFQ and BCL clone code N0b never saw) and fast-forwarded as one commit, `029b06c`, reviewed again (land; this section and the tour were its two should-fix items).

**What changed for a reader.** A satellite string's constellation is read in one place (`systemOfSat`, `letterOfSat`, `constellationKey`); the ten private converters, `NetworkSbasis::constellationOf` and every `case 'G':` arm are gone; band digits come from `SignalId`; the network files read no frequency constant (every carrier frequency and wavelength from the traits table); the receiver IFB and DCB keys go through `StateKeySchema`; estimator columns are classified by one function, `sharedColumnFamily` (`NetworkRowClassify`), with its own test (`NetworkColumnFamilyTest`). Pattern counts over the network files, main `c7e0a9a` against `029b06c`: satellite first-character reads 173 to 3, letter comparisons 50 to 3, `case 'G':` arms 30 to 0, band-digit reads 59 to 11, label-prefix parses 33 to 18, frequency constants 22 to 0, `StateKeySchema` calls 115 to 161.

**Gates.** G0 IDENTICAL against `gnss_processor_gal2.exe` on the four arms (products, consumer trace, epoch status; every log file but the timing ones), and against `gnss_processor_main1.exe` (`c7e0a9a`) on a clock-clone arm with PFQ and on a PFQ + BCL-a + BCL-d arm; a new run-end census of satellite ids with an unknown constellation or SBAS reads 0 on all six; unit suite 2 917 tests, 2 893 passed, 24 skipped (fixture data), 0 failed. Lines no G0 arm exercises (equivalence by reading only): the conflict-resolved `uRow` of `NetworkClockClone.cpp`, two clone-key lookups in `NetworkEstimator.cpp`, the warm-resume fill of `pfq_pivot`. Wall time (G3) not readable on a loaded machine; owed on an idle box.

**Corrected inconsistencies (trace or observatory only, explained):** the signal roster's tracker cell is keyed by (station, constellation, band), so a Galileo band-1 row no longer lands in the GPS cell (mismatched rows on the GPS+Galileo arm 24 393 to 49); the ambiguity trace (`network.trace_amb_if`) now writes Galileo lines (GPS lines byte-identical), with its common mode removed per constellation and BeiDou excluded (B1C and B1I are 14 MHz apart); `NetworkOraclePin` splits a family key at the first underscore. **Recorded deviations:** local maps re-keyed from a letter to `System` iterate in enum order (their bodies are per group); about 30 sites now refuse a malformed observation code that `sig[1]` read. **Owed decisions:** BeiDou digit 1 (B1C at every network site, as before; `classifySignal` says B1I); IRNSS in the `System` enum; `NetworkEstimator::isClockClone` now means any clone (rename); five label parses remain (`NetworkDatumGraph`, `NetworkFrameDiag`, `NetworkGrossLos`, `NetworkSbasis`, `NetworkSolveGlue`).

**The N3 list.** The sites whose logic is a band ROLE or a signal CHOICE, collected by the four workers and the reviews (line numbers of `nom/n0b`); each is a decision the per-station signal plan of step N3 takes over.

**A. Pin pair, primary pair and the group-delay pair**
1. `NetworkSbasis.cpp:27-53` `selectIfPinPair`: the IF pin pair is ordered by band digit then name; the first later signal on a distinct frequency is taken.
2. `NetworkSbasis.cpp:56-72`: the pair the latched group delay describes is Galileo (1,5) and (1,2) for every other constellation.
3. `NetworkSbasis.cpp:864-~900`: the per-satellite IF pin choice and the `clock_signal_basis_hold` pair hold.
4. `NetworkSbasis.cpp:1439-1445`: the secondary family's own pin pair.
5. `NetworkExtraBandScope.cpp:13-46`: the BIA clock-reference pin pair (`networkPinBandsFor`), the extra-band test and the primary MW-pair scope.
6. `NetworkClockLevel.cpp:530-543`: the heaviest code row per band digit, matched against the BIA pin bands.
7. `NetworkClockLevel.cpp:243-248` `clockLevelIsFreeSameBand`: a code counts as free if it sits on the pin pair's first band digit.
8. `StateInitializer.cpp:215`: DCB versus IFB role, decided by the BIA clock-reference pair bands.
9. `StateInitializer.cpp:774`: the first clock-reference band is the code-bias seed reference.
10. `NetworkCanonicalPairAdmission.cpp:33-46` `latchedPairs`: the canonical pair is digits 1 and 2, so Galileo is never screened.
11. `NetworkBiasAdmission.cpp:102-119`: tier 2 admits a satellite only if its candidates span 2 or more distinct bands (the IF-pin requirement).
12. `ApplicationNetworkAssembly.cpp:1640` and `:2169`: the MW pair must lie inside the primary pin pair.
13. `NetworkRunContext.cpp:5307`: the AIL trace's IF pair is the first two traits bands.

**B. Extra band and band-2 companion**
1. `NetworkSbasis.cpp:1176`: extras on the reference band (mu_k = 1) stay free.
2. `NetworkSbasis.cpp:1197-1205`: the band-2 companion is the signal with the same digit as sig2.
3. `NetworkSbasis.cpp:1239-1250`: the extra band (digit outside the pin pair) and its roster key.
4. `NetworkSolveGlue.cpp:373-383`: the ISC band-mate roster.
5. `NetworkSolveGlue.cpp:409-432`: the extra-band roster over the candidate pin pairs.
6. `NetworkSolveGlue.cpp:550-560`: the band-2 companion verdict on the wire (`isc=obs` or pinned).
7. `ApplicationNetworkAssembly.cpp:381`: the extra-band code deweight.
8. `NetworkChannelAll.cpp:22-28` and `:30-53`: canonical versus extra channel per band digit.
9. `NetworkChannelAll.cpp:98,105`: the census groups rows by band digit.

**C. Second channel restricted to band 1**
1. `NetworkSecondCodeScope.cpp:24` and `NetworkSecondPhaseScope.cpp:31`.
2. `ApplicationNetworkAssembly.cpp:1928`.
3. `SignalRoster.cpp:132-171`: companion codes and second phases.
4. `StateInitializer.cpp:134`: the second code channel, which also reads the canonical latch.
5. `NetworkMwWlGlue.cpp:141` and `:300`: `is_primary_sig1` when no primary pair exists.
6. `SignalRoster.cpp:688`: the CSL band-1 phase.

**D. EWL, UWL and lane band roles**
1. `NetworkSdEwlAr.cpp:38-53` `sdEwlBands`: GPS (2,5), Galileo (7,5). This is one of three copies of the rule; its caller is `ApplicationNetworkAssembly.cpp:542`.
2. `ApplicationNetwork.cpp:776-790` `isUncarriedThirdBand`: GPS '5', Galileo '7', or the UWL tier.
3. `NetworkDdPriorBuild.cpp:563`: the EWL third band is kept out of the carry.
4. `NetworkDdPriorBuild.cpp:482-525`: the EWL top, mid and fallback bands (digit '5' for every constellation without UWL, otherwise the tier table).
5. `ApplicationNetwork.cpp:849-853` and `ApplicationNetworkSetup.cpp:1204`: the UWL lane-tier constellations are GPS and Galileo.
6. `NetworkDdPriorBuild.cpp:332-380`: the UWL-1 joint pair census (two lowest carried bands, majority pair).
7. `NetworkDdPriorBuild.cpp:448-481`: `lane_pref_pair`, including the `"L"+digit` probe of `isUncarriedThirdBand`.
8. `NetworkDdPriorBuild.cpp:426-446`: admission of minority-family pairs.
9. `NetworkDdPriorBuild.cpp:621-649`: routing into a pair group versus a per-band group.
10. `NetworkTeardown.cpp:4949`: the run-end label reads the lane's top-band digit ('8' is E5, otherwise E5b).

**E. Which signal stands for a band**
1. `NetworkClockAnchor.cpp:126-150`: one phase bias per band, the one with the smallest folded variance.
2. `NetworkPbHoldClone.cpp:261-289` `pbHoldClonePickPair`: the smallest stashed variance per band.
3. `NetworkDdPriorBuild.cpp:300-319`: the preferred channel per (constellation, band), by majority census with a lexical tie-break.
4. `NetworkDdPriorBuild.cpp:382-414`: each arc's primary pair (two lowest digits, lexical tie, then `preferChan`).
5. `NetworkFastClock.cpp:237-260`: the fast tier's two bands (lowest FrequencyBand ordinal, tie broken by code).
6. `NetworkSdEwlAr.cpp:296-306`: every phase signal per band, with all (s1, s2) pairs tried.
7. `ApplicationNetworkAssembly.cpp:633-700`: candidate channels in ascending order; the first mature pair wins.
8. `NetworkRunContext.cpp:5474` (`resolve`) and `:5654` (`cap_of`): the first carried signal on the same band digit.
9. `NetworkRawObservables.cpp:105-112`: the slip tracker takes the two lowest digits after an unstable sort, then `front()`.
10. `NetworkRawObservables.cpp:137`: the latch overrides the band's signal.
11. `SignalRoster.cpp:255`: the lexically lowest phase per band.
12. `StateInitializer.cpp:366`: the ionosphere seed takes the first code pair on distinct bands.
13. `ApplicationNetworkSetup.cpp:1095-1110`: the receiver ATX report uses the two lowest priority bands, with attribute 'C' standing for each band.
14. `NetworkMeasurementAdmission.cpp:67-75`: the digit used as the band key when withholding a channel.
15. `SignalRoster.cpp:1427`: the band '1'/'2' mismatch counters.
16. `SignalRoster.cpp:1066-1070`: the tracker cell looks the band up in the first constellation that has it, not the satellite's own. Worker 4 flags this as a suspected defect: a Galileo band-1 row is matched to the GPS cell.

**F. Channel family (two contradictory definitions)**
1. `NetworkSbasis.cpp:286-310` `channelFamilyOf`: 'X' is secondary; Galileo, GPS and BeiDou only.
2. `NetworkDatumGraph.cpp:228-233`: family is the attribute character.

**G. Constellation roles and reference carriers**
1. `NetworkProducerAr.cpp:144`: GPS and Galileo are the integer-capable constellations (a `ConstellationPlan` candidate).
2. `NetworkChainDatum.cpp:250-259`: only GPS, Galileo, BeiDou and QZSS groups are rounded, and the wavelength comes from the band digit alone. BeiDou digit 1 stays B1C here, which belongs with the BeiDou digit-1 decision.
3. `NetworkSbasis.cpp:186-199`: which Galileo bands' health bits count, read from the configured priorities.
4. The ionosphere is referenced to the GPS L1 carrier for every constellation at `NetworkEmitStore.cpp:707` and `NetworkIonoDwRelease.cpp:205` (listed by worker 2). The same convention is at `NetworkShDevVtec.cpp:83`, `NetworkShHealth.cpp:198`, `ApplicationNetworkAssembly.cpp:1096,1236,3392` and `StateInitializer.cpp:286,374,473,754`.
5. GLONASS-exclusion tests (`== System::GLONASS`, 21 in the network files, for example `NetworkEstimator.cpp:1262`) could read `traitsOf(sys).fdma`.

Added by the reviews: GPS as the inter-system-bias and time reference (`StateInitializer.cpp` 57 and 536, `NetworkMultiGnss.cpp` 70, `ApplicationNetworkSetup.cpp` 530); same-band equivalence in gap re-entry (`NetworkSlipQc.cpp` around 2242 and 2546); `NetworkProductEmitter.cpp` 107 `discThresholdM` still classifies through `classifySignalForSat`.

