# 8 · Adding a network station parameter

**Goal:** a recipe for a new per-station state of the network producer, for example a
horizontal tropospheric gradient (north and east components). The per-station zenith wet
delay (ZWD) is the worked example: each step names the file and the function or table row
to copy. Page 7 (network tour) comes first.

Marks: **REQUIRED** (every new station parameter needs the step); **WHEN CARRIED** (needed
once the parameter is carried from one epoch to the next, or adds rows of its own; a first
version has neither); **ZWD-SPECIFIC** (history the wet delay accumulated; skip unless the
same mechanism is needed).

In both golden configurations the ZWD is not carried: it is one column of the station's
local block, estimated afresh every epoch (`network.carry_zwd` is off, ledger verdict
KEEP_OFF). A first version of a new parameter follows that posture and needs the REQUIRED
steps only.

| # | Step | Where | Mark |
| --- | --- | --- | --- |
| 1 | State, allocation, observation partial | `StateManager`, `MeasurementModel.cpp` | REQUIRED |
| 2 | Declared treatment: kind, inventory class | `NetworkParamRegistry`, `NetworkParamInventory` | REQUIRED |
| 3 | Station split, local block column | `StationSplit.cpp`, `StationBuild.cpp` | REQUIRED, no edit by default |
| 4 | Carry: inject, harvest, kind, responses | `ZwdCarry.cpp`, `LocalCarryHarvest.cpp` | WHEN CARRIED |
| 5 | Cross-epoch state, snapshot, fingerprint | `Entities.*`, snapshot files | WHEN CARRIED |
| 6 | Configuration and ledger | `GnssConfigLoadNetwork.cpp`, ledger JSON | REQUIRED |
| 7 | Run-end report | `RunEnd.cpp` | REQUIRED with a knob |
| 8 | Checks before a commit | scripts, tests | REQUIRED |

---

## 1. The model borrowed from the user side (REQUIRED)

`buildStation` (`src\core\network\driver\ApplicationNetworkSetup.cpp`) gives each station a
`StateManager`, a `PppModel` and a `MeasurementModel`; the producer takes the column layout
and the linearization point from them. It runs no station Kalman filter and writes no
solution back, so `filter.process_noise.*` does not apply; time behaviour comes from the
network's carry.

**State.** The ZWD is a fixed core column: the `StateManager` constructor assigns
`tropo_idx_`, `getTropoIndex()` returns it, and it has no string key. A fixed column needs
the constructor assignment, an accessor and a `getStateLabels` entry, behind a config flag.
A keyed state (ambiguity, per-satellite ionosphere) instead needs a `make`/`parse` pair in
`include\filter\StateKeySchema.hpp`, an `add...State` function in `StateManager`
(`addIonosphereState`) and a creation site in `MeasurementModel`.

**Partial.** The observation equation (`observationEquation()` in
`src\core\measurement\ObservationEquation.cpp`) gives the ZWD term the partial `mapping_wet`
(`PppObservation::mf_w`) at `getTropoIndex()` and the product `mapping_wet * zwd` in the
predicted observable. A new column needs the same two things: a term in that function,
named by a `ParamRef` (`include\common\ParamRef.hpp`) and found in the state by
`UserParamView::find()`.
The gradient has both already: `estimation.estimate_tropo_gradients` (default false)
allocates `getTropoGradientNorthIndex()` and `getTropoGradientEastIndex()`, with partials
`mf_grad * cos_az` and `mf_grad * sin_az`. For it the recipe starts at step 2.

**Default off.** Standing invariant 6 (`CLAUDE.md`): with the knob off the output is
byte-identical, so each step tests its config value at its first line, as every row of
`kStationBuildSteps` does.

## 2. The declared treatment (REQUIRED)

Three run-start tables describe a parameter; each needs a row.

- **Registry kind** ("Network (param model)"): an enumerator of `NetworkParamKind` and
  `kNetworkParamKindCount` in `include\common\ParamRef.hpp` (at most 32 kinds);
  a case in `paramKindName` (`src\core\common\ParamRef.cpp`) and a block in
  `NetworkParamRegistry::build` in
  `src\core\network\assembly\ParamRegistry.cpp`, copied from the `Zwd` block (`estimated`,
  `carried`, `q_rule`, `gap_horizon_s`, `route`, `source`). Uncarried: `carried` false,
  route `NotCarried`.
- **Inventory class** ("Network (parameters as armed)"): a `StateContainer` value
  (`include\measurement\AbsorbedSharedStates.hpp`; the ZWD's is `TropoIndex`), then in
  `src\core\network\assembly\ParamInventory.cpp` a `put` line in `listStateColumns`, a case in
  `classifyStateColumn` (`case SC::TropoIndex: c.kind = PK::Zwd;`) and lines in
  `paramClassLabel` and `stateContainerName`. Otherwise the column is `Unlisted` (the enum
  comment names gradients) and lands on the "unclassified" integrity line, which must read
  0. Two components can be two `ParamForm` values of one kind.
- **Contribution sets** ("Network (contributions as armed)"): the kind in `kCodeRow` and
  `kPhaseRow` of `src\core\network\assembly\Contributions.cpp`, where the ZWD is in both.

Trace labels: `buildLocalLabels` (`StationBlock.cpp`) and `localColumnLabels`
(`RedundancyCensus.cpp`); an unlabelled column prints positional names.

## 3. The station split and the local block column (REQUIRED, no edit by default)

`networkSplitStation` (`src\core\network\assembly\StationSplit.cpp`) gives each column one
disposition: position `Pinned`, shared satellite columns `EstimatedBorder`, an all-zero H
column `Unobserved`, every other column `EstimatedLocal` (entered in `h2local`, state index
to local column). A new core column becomes a local column with no code. Another
disposition (held at its seed, dropped by a declared rule) is an edit; `RxIfbRule` is the
template.

A parameter with rows or priors of its own needs a step: a function in the family's file
and a row in `kStationBuildSteps` or `kStationCarrySteps` (`StationBuild.cpp`) with
the `ContributionKind` set it creates (`kindBit`), the matching count in
`StationBuild.hpp`, and the step named in its kinds' builder text.

The parallel contract (header of `StationBuild.hpp`): one OpenMP iteration per
station; a step gets its own station and slot writable (`StationBuild::st`, `::R`) and
everything else const. Entity tables are read in a step and written by serial code only
(`nom_n4_verify.py`, check V5). The normal equations sum in a fixed order, local-block
columns as ambiguity, wet delay, slant ionosphere, receiver clock, receiver code bias, phase
colour; `tests\network\test_network_station_build.cpp` pins it.

## 4. The carry (WHEN CARRIED)

Template: the diagonal route of `src\core\network\carry\ZwdCarry.cpp`.

**Inject** (PASS B, parallel): `networkZwdCarryPrior`, row `zwd` of `kStationCarrySteps`.
Copy the gate at the first line; the column through `h2local.find(tropo_idx)`; the record
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

## 5. Cross-epoch state (WHEN CARRIED)

A value that survives an epoch is a field of an entity-table row
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

Pattern of `CLAUDE.md` ("Config toggles"); network keys load in
`src\core\config\GnssConfigLoadNetwork.cpp`:
`network_carry_zwd_ = jval<bool>(json, "network.carry_zwd", "carry_zwd", false);`

1. Member `network_carry_zwd_{false}` and getter `networkCarryZwd()` with Doxygen in
   `include\config\GnssConfig.hpp`; the loader line, default off.
2. A member of `NetworkKnobs` (`include\network\state\RunContext.hpp`) set in
   `NetworkKnobs::snapshot` (`src\core\network\driver\Knobs.cpp`); steps and registry read it.
3. A row in `docs\CONFIG_promotion_ledger.json` (`knob`, `verdict`, `area`, `default`,
   `why`; `UNVALIDATED` for a new default-off knob; `fingerprinted` when step 5 added a
   term), then `python scripts\ledger_render.py`.
4. The goldens stay untouched: they are generated, and a knob enters them only by the
   author's decision (ledger, then `scripts\make_golden_config.py`).

ZWD-SPECIFIC, skip: the `carry_zwd_in_block` interlocks and `network.trace_zwd`.

## 7. The run-end report (REQUIRED with a knob)

An armed knob that acted on nothing must show at run end (standing invariant 4). A report
is `int networkXxxRunEnd(const NetworkRunEndView& v)` in the subsystem's file, gated on the
knob; `networkZwdSeamRunEnd` (`ZwdCarry.cpp`) is a short model that warns on an armed
knob with a zero counter. Add one row to `kNetworkRunEndReports`
(`src\core\network\runend\RunEnd.cpp`) and raise `kNetworkRunEndReportCount`
(`RunEnd.hpp` states the order rules). A parameter without a knob needs no row: the
tables of step 2 print it.

## 8. Checks before a commit

1. `scripts\vs2026_build.ps1 -Config x64-Debug`, then `build\x64-Debug\gnss_tests.exe`,
   with tests for steps 2 to 5 (`tests\network\test_network_param_registry.cpp` and
   `tests\network\test_network_param_inventory.cpp` as templates).
2. `python scripts\nom_epoch_lint.py` after a change to a seam signature or an entity writer.
3. `python scripts\nom_n4_verify.py --whole` (each field declared once; Record rows complete).
4. A row in `tests\network\test_network_epoch_contract.cpp` for a new epoch writer.
5. `python scripts\make_golden_config.py --verify`,
   `python scripts\config_posture_check.py --verify-loader`,
   `python scripts\ledger_render.py --check`.
6. Byte identity with the knob off: a 2 h golden arm (`scripts\rapid_arm.py`, page 7) on
   the pre-change executable (kept as a renamed copy) and on the candidate, both Release,
   then `python scripts\nom_td_gate.py <run A> <run B> --whole`; exit 0 means identical.
7. With the knob armed in an arm copy, the three run-start tables show the parameter
   (`estimated_local` counts, integrity lines at 0), and the run end shows non-zero counters.

## Known rough edges

- **The network borrows the user-side `StateManager` and `MeasurementModel`.** A parameter
  begins as a user-side state; the network classifies and labels it afterwards. A user-side
  flag changes the network layout with no network key and no ledger row
  (`estimation.estimate_tropo_gradients` has neither). A separate future step, in which the
  network owns its parameter set, removes this; the set exists as a shadow of the state
  manager (`StationParams`, page 7), checked on every run, and nothing reads it yet.
- **The ZWD is a noisy template.** Its carry, in-block route, seam window, capture and
  census hooks are history the golden posture does not use. `NetworkParamKind::PhaseColour`,
  the last estimated kind in the enum, is a second template for steps 2 to 4.
- **A gradient needs a graded decision this recipe does not make:** whether an uncarried
  column keeps the local block well conditioned, or needs a prior.

Next: [7 · Network tour](07_network_tour.md), or the network task at hand.
