# UOM: the user-side edits of card NOM step N6 (notices moved from the UOM card)

Companion to [UOM_user_object_model_2026-10-13.md](UOM_user_object_model_2026-10-13.md), which holds the card. The notices below were appended to it on branch `nom/n6` and moved here at the merge with main, which had appended its own sections 16 and 17 (step UB2) and left the card at the size bar (rule 4). The text is unchanged except the section numbers: sections 18 and 19 of the card became 1 and 2 here, and the reference to the observation equation of "section 16" in the N6b notice names section 1 here.

## 1. 2026-10-17: card NOM step N6a edits these user-side files (branch `nom/n6`)

Card NOM step N6 (`scripts/nom3_designs/N6_design.md`) gives the network its own local parameter set and one observation equation that the producer, the consumer and the seeds read. Its decision D4 takes the "later one observation equation" item of this card into N6. Step N6a is the first of the user-side edits, and the list below is the announcement the design asks for. All of N6a is identical output with the check key off; with `processing.obs_equation_check` on, every code and phase row is built twice and compared bit for bit.

| file | change |
| --- | --- |
| `src/core/measurement/MeasurementModelRow.cpp` | `buildJacobianRow` builds the term list of the equation (`observationEquation`), sums it (`evaluate`) and scatters the partials (`scatterDense`); the Doppler branch is the member `buildDopplerRow`, the decomposition traces are the member `traceRowDecomposition`; the file banner |
| `include/measurement/MeasurementModel.hpp` | declarations of `obsInput`, `buildDopplerRow`, `wantsRowDecomposition`, `traceRowDecomposition`, `buildJacobianRowReference`, `checkRowAgainstReference`; the `buildJacobianRow` Doxygen; the file banner |
| `src/core/measurement/MeasurementModel.cpp` | the banner only |
| `include/measurement/StateInitializer.hpp`, `src/core/measurement/StateInitializer.cpp` | `ReceiverBiasEntry` also carries the receiver code-bias state as a typed `RxCbKey` (`key`, `has_key`); the text key is rendered from it by `userCodeBiasKey` and is unchanged |
| `include/filter/StateManager.hpp`, `src/core/filter/StateManager.cpp` | the read-only static `ambiguitySeedFrequencyHz`, the seed's own frequency table, for the check's frequency report |
| `include/config/GnssConfig.hpp`, `src/core/config/GnssConfigLoad.cpp` | the key `processing.obs_equation_check` (default off; a TRACE row of the ledger) |
| `include/common/ParamRef.hpp`, `src/core/common/ParamRef.cpp` | `ParamRef`, `UserCore` and the factory functions that name a parameter without its station |

New files: `include/measurement/ObservationEquation.hpp` and `src/core/measurement/ObservationEquation.cpp` (the term list, its sum and its Jacobian row), `include/measurement/UserParamView.hpp` and `src/core/measurement/UserParamView.cpp` (where a parameter sits in a `StateManager`, and the one function that renders a typed identity as a user-side key), `include/measurement/ObservationEquationCheck.hpp`, `src/core/measurement/ObservationEquationCheck.cpp` and `src/core/measurement/MeasurementModelRowReference.cpp` (the comparator and the row builder it compares against; both are deleted by step N6e), the run-end row `src/core/network/runend/ObsEquationCheckRunEnd.cpp`, and `tests/test_observation_equation.cpp`.

Rules the edits keep. The summation order of the predicted observable is unchanged: the terms are added left to right in the order `modeled`, range correction, receiver clock, phase clock, wet troposphere, gradients, inter-system bias, slant ionosphere, receiver code bias, satellite code bias, satellite phase bias, satellite clock, SSR bias, ambiguity, phase colour, and a term with a factor keeps the factor outside the sum of its parts. A satellite or signal text that is not a canonical RINEX id (`SatId::str` and `SignalId::obsCode` would not give the text back) makes a row that needs a state key from it unusable; every parser and decoder of the tree produces canonical ids, and the check counts any such row. N6b (the seeds: `StateInitializer.*`, `StateManager.cpp`, `MeasurementModelStates.cpp`, the 14 seed call sites) edits further files of this list; edits to them by this card should merge with `nom/n6`.

## 2. 2026-10-17: card NOM step N6b edits these user-side files (branch `nom/n6`)

Step N6b puts the seeds on the observation equation of section 1. `StateInitializer::computeRawResidual` and `StateManager::computeAmbiguitySeed` are deleted: both seeds read the term list, built with the effects of one of two masks (`kLegacySeedMask`, `include/measurement/Seeds.hpp`), and subtract in the order the copies did, so the numbers are the same bit for bit (`tests/test_seeds.cpp` keeps the old code as the reference). The first-epoch seeds (inter-system bias, cold clock, phase clock, receiver code bias, slant ionosphere) are pure functions that return values; `MeasurementModel` applies them in the order they have always run in.

| file | change |
| --- | --- |
| `StateInitializer.hpp/.cpp` | keeps the latches, `resolveReceiverBias`, the code-bias restore and `frequencyHelper`; loses its `StateManager` reference and the seed functions |
| `StateManager.hpp/.cpp` | `computeAmbiguitySeed`, `ambiguitySeedFrequencyHz` and the file-local tables deleted |
| `MeasurementModel.hpp/.cpp`, `MeasurementModelStates.cpp` | `ambiguitySeed`, `ambiguitySeedInput`, `residualSeedInput`; `initializeInterSystemBiases`, `initializePhaseClockOffset`, `initializeCodeBiases` (bodies from `StateInitializer`); the 7 seed sites; the table rows |
| `KalmanOutlierResponse.cpp` (3 sites), `CycleSlipRepair.cpp` (2) | call `MeasurementModel::ambiguitySeed` |
| `PivotManager.hpp/.cpp`, `CycleSlipPivot.cpp` | `handlePivotHandover` takes `ambiguitySeedInput()` for its 2 sites |
| `ObservationEquation.hpp/.cpp`, `ObservationEquationCheck.hpp/.cpp` | the set of effects to build; `findTerm`, `termValue`; `ObsInput::fdma`; the frequency audit loses the seed's own table |

New: `Seeds.hpp/.cpp`, `MeasurementModelSeeds.cpp`, `tests/test_seeds.cpp`, `tests/support/ModelStack.hpp`.

The slant ionosphere's carrier frequency is the one place the copies disagreed, on GLONASS FDMA bands (G1, G2) only: the row builder takes each satellite's channel, the ambiguity seed took channel 0, and the other seeds the default channel table of the initializer's own helper. Every other band is one number in all three, bit for bit (tested over every classified signal). N6b keeps each seed's number, so no output moves (`ObsInput::fdma` is `Zero` for the ambiguity seed; the residual seeds take the initializer's helper). One rule for the three is decided with the full-equation seeds (step N6g).

The seeds read no process-wide cache. The .BIA clock-reference observables are `ObsInput::clock_refs` and a parameter of `StateInitializer::resolveReceiverBias`; the presence of a .BIA entry is a predicate argument of `seedIonosphere`; whether a present entry counts is `biaEntryApplies` (`RowRules.hpp`), the half of `biaCoversSignal` that reads the configuration. `MeasurementModel` supplies the product cache's map (`setClockRefs` replaces it in a unit test). Further files edited: `RowRules.hpp`, `MeasurementModelRow.cpp`, `MeasurementModelRowReference.cpp`.
