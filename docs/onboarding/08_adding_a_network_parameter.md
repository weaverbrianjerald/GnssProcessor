# 8 · Adding a network station parameter

**Goal:** a recipe for a new per-station state of the network producer. The horizontal
troposphere gradient (north and east components, `network.estimate_tropo_gradients`) is the
worked example: it was added this way (card NOM5 step N6h), and each step names the file and the
function or table row it took. The per-station zenith wet delay (ZWD) is the template for a
carried parameter. Page 7 (network tour) comes first.

Marks: **REQUIRED** (every new station parameter needs the step); **WHEN CARRIED** (needed
once the parameter is carried from one epoch to the next, or adds rows of its own; a first
version has neither); **ZWD-SPECIFIC** (history the wet delay accumulated; skip unless the
same mechanism is needed).

In both golden configurations the ZWD is not carried: it is one column of the station's
local block, estimated afresh every epoch (`network.carry_zwd` is off, ledger verdict
KEEP_OFF). The gradient follows that posture (two uncarried columns): the REQUIRED steps
added it, and one step of rows of its own bounds it, a zero-mean prior row per column and
epoch (section 3).

| # | Step | Where (the gradient's edits) | Mark |
| --- | --- | --- | --- |
| 1 | Identity, state, observation partial | `ParamRef` kind and forms; the `StationParams` field; the equation's term (already there) | REQUIRED |
| 2 | Declared treatment: registry, inventory, contribution sets, labels | `ParamRegistry.cpp`, `ParamInventory.cpp`, `Contributions.cpp`, `StationBlock.cpp`, `RedundancyCensus.cpp` | REQUIRED |
| 3 | Station split, local block column | `StationSplit.cpp` | REQUIRED, no edit by default |
| 4 | Carry: inject, harvest, kind, responses | `ZwdCarry.cpp`, `LocalCarryHarvest.cpp` | WHEN CARRIED |
| 5 | Fingerprint; cross-epoch state and snapshot | `SnapshotFingerprint.cpp`; `Entities.*`, snapshot files | fingerprint REQUIRED with a knob; the rest WHEN CARRIED |
| 6 | Configuration and ledger | `GnssConfigTypes.hpp`, `GnssConfigLoadNetwork.cpp`, `NetworkKnobs`, ledger JSON | REQUIRED |
| 7 | Run-end report | the subsystem's report | REQUIRED with a knob |
| 8 | Checks before a commit | scripts, tests | REQUIRED |

---

## 1. Identity, state and observation partial (REQUIRED)

`buildStation` (`src\core\network\driver\ApplicationNetworkSetup.cpp`) gives each station a
`StationParams` set (`startStationParams`), a `PppModel` and the row inputs (`StationRowInputs`,
`src\core\network\assembly\StationRows.cpp`); the producer takes the column layout and the
linearization point from the set. It runs no station Kalman filter and writes no solution back,
so `filter.process_noise.*` does not apply; time behaviour comes from the network's carry.

**Identity.** A parameter is named by a `ParamRef` (`include\common\ParamRef.hpp`): a
`NetworkParamKind`, appended at the end of the enumeration with `kNetworkParamKindCount` raised
(at most 32 kinds), and a `ParamForm` when the kind has components. The gradient is
`NetworkParamKind::TropoGradient` with the forms `North` and `East`. In
`src\core\common\ParamRef.cpp` it has its factory (`gradientRef(bool north)`), its names
(`paramKindName` gives "tropo_gradient", `paramFormName` "north" and "east") and a case in
`paramInstanceOf` (no constellation, as the ZWD). The identity is shared with the
single-receiver filter: `UserParamView::find` (`src\core\measurement\UserParamView.cpp`) answers
for it from the `StateManager`, so one `ParamRef` names the producer's column and the consumer's
state.

**State.** The set holds the slot. A fixed column is a field of `StationParams`
(`include\network\state\StationParams.hpp`; the gradient's `std::array<int, 2> gradient`, as
`zwd` is) with a case in `addParam` and in `findParam` and a label in `stateLabel`
(`src\core\network\state\StationParams.cpp`; "TGRAD_N", "TGRAD_E"). `startStationParams` lays
it out behind its key (the gradient after the wet delay), `isCore` keeps it from removal and
`removeSlots` renumbers it. A keyed state (ambiguity, per-satellite ionosphere) instead needs a
typed map in the set with the same cases, a seed function in `include\measurement\Seeds.hpp`
when it is born from data, and a birth: a local column in `networkBirthLocalStates`
(`src\core\network\assembly\LocalBirth.cpp`), a shared one in `networkAdmitSharedStates`
(`SharedAdmission.cpp`). The births fix the order of the columns, which the local block
factorizes in (page 7).

**Partial.** The observation equation (`observationEquation()` in
`src\core\measurement\ObservationEquation.cpp`) has one function per effect, and each asks the
caller's lookup for its parameters (`NetView::find`, `network\assembly\StationRows.hpp`, on the
network). The gradient's function, `addGradients`, writes the term
`mf_grad * (cos(az) * g_n + sin(az) * g_e)`, so a row's entries at the two slots are
`mf_grad * cos(az)` and `mf_grad * sin(az)`. A term whose parameter the lookup does not hold is
left out, so the term needed no edit: it reads whichever set holds the parameter. A new effect
is an `ObsEffect` enumerator and a function in that file, added in the order of the sum.

**Default off.** Standing invariant 6 (`CLAUDE.md`): with the knob off the output is
byte-identical. The gradient's slots exist only behind its key, so with the key off no column,
row entry or label changes. A new kind adds one row to the run-start "Network (param model)"
table, which lists every kind; that is a declared text change.

## 2. The declared treatment (REQUIRED)

Three run-start tables describe a parameter; each needs a row.

- **Registry kind** ("Network (param model)"): a block in `NetworkParamRegistry::build`
  (`src\core\network\assembly\ParamRegistry.cpp`). The gradient's block: `estimated` from its
  knob, `carried` false, `q_rule` None, route `NotCarried`, and the `source` text naming the key.
  A carried kind copies the `Zwd` block (`q_rule`, `gap_horizon_s`, route).
- **Inventory class** ("Network (parameters as armed)"): `paramInstanceOf` reads the class from
  the `ParamRef` (kind and form), and `paramClassLabel` (`src\core\network\assembly\ParamInventory.cpp`)
  prints it ("troposphere gradient, north"). A column that no field or typed map of the set takes
  keeps its slot and is counted in `StationParams::untyped`; a withheld satellite parameter whose
  identity cannot be typed lands on the "unclassified" integrity line, which must read 0.
- **Contribution sets** ("Network (contributions as armed)"): the kind in `kCodeRow` and
  `kPhaseRow` of `src\core\network\assembly\Contributions.cpp` (the ZWD and the gradient are in
  both).

Trace labels: `blockLabel` (`StationBlock.cpp`) and `censusLabel` (`RedundancyCensus.cpp`), a case
per kind ("tgrad:n", "tgrad:e"); an unlabelled column prints positional names.

## 3. The station split and the local block column (REQUIRED, no edit by default)

`networkStationRows` keeps each row's Jacobian entries as (slot, value) pairs
(`StationDataRows`), and `networkSplitStation` (`src\core\network\assembly\StationSplit.cpp`)
gives each slot one disposition from them: position `Pinned`, shared satellite columns
`EstimatedBorder`, a slot no row reads with a nonzero entry `Unobserved`, every other slot
`EstimatedLocal` (entered in `h2local`, slot to local column). It then writes every entry
straight into the shared border or the local block. A new core column becomes a local column
with no code, as the gradient did. Another disposition (held at its seed, dropped by a declared
rule) is an edit; `RxIfbRule` is the template.

A parameter with rows or priors of its own needs a step: a function in the family's file
and a row in `kStationBuildSteps` or `kStationCarrySteps` (`StationBuild.cpp`) with
the `ContributionKind` set it creates (`kindBit`), the matching count in
`StationBuild.hpp`, and the step named in its kinds' builder text. The gradient's prior is
the example: `networkTropoGradientPriorRows` (`src\core\network\assembly\StationAnchors.cpp`),
row `tropo_gradient_prior` after `sim_zero_iono`, kind `ContributionKind::TropoGradientPrior`
(its declaration in `contributionSpecs()`, and a case in `rowClassOfKind`, `RowClassify.cpp`,
since it is a row and not a diagonal prior). One row per gradient column, +1 on the column,
weight `1 / network.tropo_gradient_prior_sigma_m^2`, and the target minus the slot's
linearization value, so the row holds the gradient toward absolute zero.

The parallel contract (header of `StationBuild.hpp`): one OpenMP iteration per
station; a step gets its own station and slot writable (`StationBuild::st`, `::R`) and
everything else const. Entity tables are read in a step and written by serial code only
(`nom_n4_verify.py`, check V5). The normal equations sum in a fixed order, local-block
columns as ambiguity, wet delay, slant ionosphere, receiver clock, receiver code bias, phase
colour; `tests\network\test_network_station_build.cpp` pins it.

## 4. The carry (WHEN CARRIED)

Template: the diagonal route of `src\core\network\carry\ZwdCarry.cpp`. The gradient is not
carried; a carried gradient would take these steps with its own kind.

**Inject** (PASS B, parallel): `networkZwdCarryPrior`, row `zwd` of `kStationCarrySteps`.
Copy the gate at the first line; the column's slot from the station's set (`st.params.zwd`, a
typed map or a field of `StationParams` for another kind), then `h2local.find(tropo_idx)`; the record
`R.pl_zwd = PlZwd{zcol, x_lin}`; the entry from `net::get(sta_rows, ...)`; the gap test
`carryPastHorizon`; and the prior pushed to `inj` with
`RowTag{ContributionKind::PriorZwdCarry, {}, {}}`, `lambda = 1 / (var + q * gap)`. Trap:
`networkStationCarriedPriors` returns before its walk unless
`legacy_local_inject || rx_cb_on || rx_cb_datum || opin_on`; a carry that must act outside
the leveling carry's warm-up needs its knob in that predicate, or it is armed and inert there.

**Harvest** (serial, after the solve): `networkHarvestZwd`, one call in
`networkHarvestLocalCarries` (`src\core\network\carry\LocalCarryHarvest.cpp`; the call order is
load-bearing, see its header and `scripts\nom_hv1_verify.py`). Copy
`est.reconstructStation(est_idx, x_s)`, `value_abs = x_lin + xr(col)`,
`est.conditionalLocalVarianceDiag(...)`, the stash `net::put(...) = LocalCarry{...}` and
`carryGapPrune(...)` against `ctx.params.spec(kind).gap_horizon_s`.

**Contribution kind.** An enumerator in `ContributionKind`
(`include\network\assembly\ContributionKind.hpp`, before `Unset`) and a row of
`contributionSpecs()` (copy `PriorZwdCarry`); the order is checked at compile time. A row
kind, not a diagonal prior, also needs a case in `rowClassOfKind` (`RowClassify.cpp`).

**Responses** (standing invariants 1 and 7): rows of the `ResponseSite` table
(`Responses.hpp`, `.cpp`; `ZwdGapSkip`, `ZwdGapPrune`) and, for a rate, a
`NetworkQClass` row (`StationZwd`, `include\config\GnssConfigTypes.hpp`). The gap gate is
needed by any carried parameter; the ZWD borrows the ambiguity horizon
(`network.phase_leveling_max_gap_s`), and a new kind declares its own `gap_horizon_s`.

**ZWD-SPECIFIC, skip:** the seam re-seed (`zwd_seam_ep_left`, `networkZwdSeamWindowStep`);
the snapshot capture (`networkZwdRowCapture`); the coupling-row route (`buildCplRow`,
`CouplingScalar`); the in-block route (`BlockSegment::Zwd`, `BlockZwdCarry`, `is_zwd` in
`Coupling.cpp`); the prior-residual hook (`prires_zwd`); `n_raim_pool_zwd_rows`;
the shared-history note; `zwdQEff`.

## 5. Fingerprint; cross-epoch state (fingerprint REQUIRED with a knob; the rest WHEN CARRIED)

**Fingerprint.** A knob that changes what the solve estimates appends a token to the warm-resume
fingerprint when armed (`networkSnapshotFingerprint`, `src\core\network\state\SnapshotFingerprint.cpp`),
so a resumed run never crosses it and an unarmed run keeps its fingerprint. The gradient appends
`tgrad:1`.

**Cross-epoch state.** A value that survives an epoch is a field of an entity-table row
(`include\network\state\Entities.hpp`, whose header is the guide): the ZWD carry is
`std::optional<LocalCarry> zwd` in `net::StationState`, with a `FieldDecl` row in
`kStationFields` (`src\core\network\state\Entities.cpp`) naming its persistence.

- `Persist::ColdStart` (the receiver clock carry): the field and its row. The simplest start.
- `Persist::Record` (the ZWD, record `Z`): also a member of `NetworkStateSnapshot`
  (`include\products\NetworkStateSnapshot.hpp`) with serialize and parse in
  `src\core\products\NetworkStateSnapshot.cpp`; `zwdToSnapshot` and `zwdFromSnapshot`; the
  tag in `kRestoreOrder`; the calls in `StateSnapshotWrite.cpp` and
  `WarmResume.cpp`; a term in `networkSnapshotFingerprint`; a round-trip test naming
  the tag (`tests\network\test_network_entities_records.cpp`).
- A function that binds an entity table non-const has a `kEntityWriters` row (lint check
  V12): `networkHarvestZwd`, `zwdFromSnapshot`.

A per-epoch value a later stage reads is a member of the writing stage's group of
`EpochEvents` (`include\network\driver\Epoch.hpp`, its writer list updated), never an
undeclared `NetworkCarryState` member (which needs a `kRunFields` row); counters go in
the counter struct of the writing subsystem (its own header), a base of
`NetworkServiceCounters`.

## 6. Configuration (REQUIRED)

Network keys load in `src\core\config\GnssConfigLoadNetwork.cpp`, each in the section helper of
its kind. The gradient's key is a member of `NetworkConfig` (`estimate_tropo_gradients`,
`include\config\GnssConfigTypes.hpp`), read in `loadNetworkSharedStateForcings_` beside
`network.estimate_rx_extra_band_code_delay`:
`network_config_.estimate_tropo_gradients = jval<bool>(json, "network.estimate_tropo_gradients", "", false);`
The empty second argument means no top-level legacy spelling: a root `estimate_tropo_gradients`
is the single-receiver flag's legacy key, and two keys must not share one spelling.

1. The member with its Doxygen and the loader line, default off.
2. A member of `NetworkKnobs` (`include\network\state\RunContext.hpp`) set in
   `NetworkKnobs::snapshot` (`src\core\network\driver\Knobs.cpp`; the gradient's `tgrad_on`);
   steps and the registry read it.
3. A row in `docs\CONFIG_promotion_ledger.json` (`knob`, `verdict`, `area`, `default`,
   `why`; `UNVALIDATED` for a new default-off knob; `fingerprinted` when step 5 added a
   token), then `python scripts\ledger_render.py`.
4. The goldens stay untouched: they are generated, and a knob enters them only by the
   author's decision (ledger, then `scripts\make_golden_config.py`).

**A parameter with a single-receiver flag.** In a network run the single-receiver layout flags
(`estimation.estimate_tropo_gradients`, `estimate_velocity`, `estimate_phase_clock`,
`imu.enabled`, `state_ambiguity_basis` nl_wl, `between_satellite_sd.enabled`) set only the
closed-loop consumer's layout: the consumer is configured by a copy of the network configuration
(`GnssConfig::deriveClosedLoopConsumerConfig`). With no consumer a flag reaches no parameter, and
the loader refuses the configuration (`GnssConfig::validateNetworkUserLayoutFlags_`), naming the
network key where one exists. A new parameter with such a flag puts its network key in that
table.

## 7. The run-end report (REQUIRED with a knob)

An armed knob that acted on nothing must show at run end (standing invariant 4). The gradient's
line is a section of the parameter inventory's report (`networkTropoGradientRunEnd`,
`src\core\network\assembly\ParamInventory.cpp`, listed in `networkParamInventoryRunEnd`): it
reads the inventory's own tally (the station-epochs that estimated each component and that left
it out as unobserved) and warns INERT when the key is armed and no column was estimated. A
parameter with counters of its own writes a report `int networkXxxRunEnd(const NetworkRunEndView& v)`
in its subsystem's file, gated on the knob (`networkZwdSeamRunEnd`, `ZwdCarry.cpp`, is a short
model), with one row in `kNetworkRunEndReports` (`src\core\network\runend\RunEnd.cpp`) and
`kNetworkRunEndReportCount` raised (`RunEnd.hpp` states the order rules).

An instrument is optional: the gradient writes `network_tropo_gradient.csv` (the estimates and
their datum-conditioned formal sigma, per station and epoch), a stream of `NetworkRunTraces`
(`network\diag\RunTraces`) open exactly when the key is on, written by `networkIonoDiag` beside
the ZWD trace.

## 8. Checks before a commit

1. `scripts\vs2026_build.ps1 -Config x64-Debug`, then `build\x64-Debug\gnss_tests.exe`,
   with tests for the steps taken (the gradient's: `tests\network\test_network_layout_flags.cpp`,
   the layout, the rows' partials, the prune and the loader rules;
   `tests\network\test_network_param_registry.cpp` and
   `tests\network\test_network_param_inventory.cpp` are templates for steps 2 to 5).
2. `python scripts\nom_epoch_lint.py` after a change to a seam signature or an entity writer.
3. `python scripts\nom_n4_verify.py --whole` (each field declared once; Record rows complete).
4. A row in `tests\network\test_network_epoch_contract.cpp` for a new epoch writer.
5. `python scripts\make_golden_config.py --verify`,
   `python scripts\config_posture_check.py --verify-loader`,
   `python scripts\ledger_render.py --check`.
6. Byte identity with the knob off: a 2 h golden arm (`scripts\rapid_arm.py`, page 7) on
   the pre-change executable (kept as a renamed copy) and on the candidate, both Release,
   then `python scripts\nom_td_gate.py <run A> <run B> --whole`; exit 0 means identical (a new
   kind's run-start row is the declared exception).
7. With the knob armed in an arm copy, the three run-start tables show the parameter
   (`estimated_local` counts, integrity lines at 0), and the run end shows non-zero counters.

## Known rough edges

- **The set shares its vocabulary with the user side on purpose.** The observation equation, the
  seeds and the receiver-bias rule are one code for the producer and the single-receiver filter,
  so a term added for one serves the other. A parameter is still printed in the traces and the
  snapshot as the user-side state key (`userKey()`), a naming contract that the trace columns and
  the snapshot records keep.
- **The ZWD is a noisy template.** Its carry, in-block route, seam window, capture and
  census hooks are history the golden posture does not use. `NetworkParamKind::PhaseColour`
  is a second template for steps 2 to 4, and `NetworkParamKind::TropoGradient` the template for
  an uncarried column.
- **Whether an uncarried column keeps the station blocks well conditioned** is a graded
  question, not a recipe step: for the gradient it is card NOM5's gate G4, and its verdict is
  recorded there. Conditioning is not the whole test. The gradient's blocks stayed well
  conditioned, but one epoch determines a gradient to only 5 to 7 mm, and the free columns fit
  noise and took up a station-level step; the gradient therefore takes the zero-mean prior of
  section 3, graded in card NOM5 section 6. A carried gradient would instead need the in-block
  vehicle: a diagonal carry of a station-common column is the double count that
  `network.carry_zwd` (KEEP_OFF) showed.

The gradient took edits in 5 files for step 1 (`ParamRef` and `StationParams`, header and source
each, and `UserParamView.cpp` because the identity is shared; the equation already had the
term), 5 for step 2, none for step 3, 1 for the fingerprint, 5 for the configuration
(`GnssConfigTypes.hpp`, `GnssConfigLoadNetwork.cpp`, `RunContext.hpp`, `Knobs.cpp`, the ledger
JSON) and a section of `ParamInventory.cpp` for the run-end line, plus the optional trace
(`RunTraces` header and source, `IonoDiag.cpp`) and one test file. Its prior took the step and
its row (`StationAnchors` header and source, `StationBuild` header and source), the kind
(`ContributionKind.hpp`, `Contributions.cpp`, `RowClassify.cpp`), the key in the same five
configuration files, the fingerprint token, a clause of the run-end line, and tests in the same
file plus the step-table and row-class pins.

Next: [7 · Network tour](07_network_tour.md), or the network task at hand.
