# NOM3 step EP4: pre-registration (setup becomes builders in their concerns' files)

Card: `docs/refound/NOM3_network_giants_2026-10-09.md` section 3, EP4. Base: `nom/wave1`
`8f4f1db` (main `7141240` plus EP2, SG1 to SG3, PB1a). Frozen text:
`git show 8f4f1db:src/core/app/ApplicationNetwork.cpp` (847 lines; `Application::runNetwork` is
lines 193 to 845, 653 lines). Branch `nom/ep4`. Pure relocation: output byte-identical (gate
G-EP, run by the orchestrator). No printed text moves, so no `reverse.sed` is owed.

Measured at the base: the lane layer is deleted (card RKD) and the deweight gate, the
carry-driver configuration and the emit engines' construction already left setup in EP1
(`networkEngineSpec`, `NetworkRun`'s constructor). Of those four items of the card's list only
three alias lines remain (`carry_cfg`, `carry_driver`, `emit_engines`); they become bindings of
the builders that read them. No lane builder exists.

## 1. The block table (frozen lines -> builder, file)

Every range moves as text. A builder has the run form `void networkX(NetworkRun& run[, kept])`,
opens with one binding per frozen name its body reads (EP1/EP2 markers), and fills the run
member it names. B1 runs before the run exists and has the explicit form.

| # | frozen lines | what | builder | file |
| --- | --- | --- | --- | --- |
| B1 | 254-264 | marker -> ARP census copied into the counters | `networkMarkerArpCensusCopy(ctx, config_, setup)` | `app/ApplicationNetworkSetup.cpp` (decl. `app/NetworkSetupResult.hpp`) |
| B2 | 280-344 | slip-QC parameters, history sized | `networkSlipQcConfigure(run)` | `slip/NetworkSlipQc.{hpp,cpp}` |
| B3 | 345-357 | slip oracle load (simulation) | `networkSlipOracleArm(run, log_manager_)` | `slip/NetworkSlipOracle.{hpp,cpp}` |
| B4 | 358-375 | oracle pin load (simulation) | `networkOraclePinArm(run, log_manager_)` | `filter/NetworkOraclePinSetup.{hpp,cpp}` (new; PB1b owns `NetworkOraclePin.*`) |
| -- | 376 | `traces.openAll(...)` | stays in the driver (already one call) | |
| B5 | 377-384 | raw-slip wavelength provider | `networkSlipQcArmWavelengthProvider(run, log_manager_)` | `slip/NetworkSlipQc.{hpp,cpp}` |
| B6 | 389-403 | carry-VCE apply step (census arm) | `networkCarryVceApplyArm(run)` | `filter/NetworkCarryVceApply.cpp` (new; header exists) |
| B7 | 404-482 | prior-residual census corroborator (census arm) | `networkPriorResidualArm(run)` | `filter/NetworkPriorResidual.{hpp,cpp}` |
| B8 | 483-524 | block-Baarda census seat and resolver (census arm) | `networkBlockBaardaArm(run)` | `filter/NetworkBlockBaarda.{hpp,cpp}` |
| B9 | 525-533 | station congruence seat (census arm) | `networkCongruenceArm(run)` | `filter/NetworkCongruenceSetup.{hpp,cpp}` (new; PB1b owns `NetworkCongruence.*`) |
| B10 | 536-548 | run manifest startup record, parameter-model tables | `networkRunManifestStartup(run, config_path_)` | `logging/NetworkRunTraces.{hpp,cpp}` |
| -- | 575-593 | Eigen pin, `timing.loop_begin` | stay in the driver (they bracket the epoch source) | |
| B11 | 595-646 | estimator configuration | `networkEstimatorConfigure(run, engine_spec)` | `filter/NetworkEstimator.{hpp,cpp}` |
| B12 | 648-668 | D0 section timers armed (estimator, carry driver) | `networkSectionTimersArm(run)` | `app/NetworkRun.{hpp,cpp}` (beside the EP2 timer seams) |
| B13 | 672-717 | run arming: snapshot path, fingerprint, warm resume, the two restores, injector, CPA, signal roster, ledger emit | `networkRunArm(run, log_manager_)` | `app/NetworkRun.{hpp,cpp}` |
| B14 | 719-807 | code-bias admission census and its table | `networkCbAdmissionCensus(run)` | `filter/NetworkBiasAdmission.{hpp,cpp}` |

The driver keeps: the replay branch, the banner, validation, `resolveData`, `networkSetup_`, the
alias lines still read (`sync`, `synced`, `closed_loop_on`, `sh_model`, `shc`, `parallel_on`,
`consumer`), the context, the traces, the engine spec, the run, `openAll`, the Eigen pin, the loop
timing stamp, the builder calls, the epoch source and the teardown.

## 2. Order constraints between builders

The calls keep the frozen order exactly; the constraints that make it load-bearing:

1. B1 follows the context's construction and reads only the setup result.
2. B2 precedes B5 (`anyConsumer` reads the parameters B2 fills).
3. B3 and B4 precede `openAll`: a missing or mis-pointed fixture throws before any trace file
   opens, and their log lines precede the trace-open lines.
4. `openAll` precedes B5 (B5 tests `slip_raw_trace`, which `openAll` opens).
5. B6 to B9 seat independent `ctx.carry` members (no reads between them; the resolvers read
   `slip_qc` per epoch, not at arming). B10 follows them (frozen log and manifest order).
6. The Eigen pin and `timing.loop_begin` precede B11 (the loop wall's start is unchanged).
7. B11 precedes B12; B12 precedes B13 (the timers are reset before the resume restores the carry
   driver).
8. Inside B13: snapshot path, fingerprint, resume, the canonical-signal and code-bias
   restores (staged by the resume, before the first epoch locks the latch), then the injector
   (its banner after the resume), CPA, signal roster, ledger emit.
9. B14 follows B13 (census per process, after the resume; its log table after the arms' lines).

## 3. Declared substitutions and drops (closed list)

Anything else the verifier finds is a failure.

- **D1 bindings.** A frozen name a builder reads binds once, before
  `// ---- End of the bindings ----`, by its declared text: a frozen declarator whose root is the
  run's member keeps its text verbatim (`slip_qc`, `prires_est`, `carry_driver`,
  `emit_engines`, `est`, `slip_raw_trace`, `synced`, `start_offset_s`, `carry_cfg`); a setup
  alias spells the root `setup.X` as `run.setup.X` (`sync`, `stations`, `n_threads`,
  `seam_threads`, `carry_par`, `emitter`); `auto& stations` spells its deduced type; the frozen
  object locals `ctx`, `traces` bind by reference to the same object; `nc`, `log` and the
  Application members `config_`, `log_dir_` bind as in EP1 (`run.nc`, `run.log`, `run.config`,
  `run.log_dir`).
- **D2 kept parameters.** `LogManager& log_manager_` (B3, B4, B5, B13),
  `const std::filesystem::path& config_path_` (B10), `const NetworkEngineSpec& engine_spec`
  (B11, root of `carry_cfg`); B1's parameters `ctx`, `config_`, `setup` (const added).
- **D3 const tightening.** `slip_qc` binds const in B7 and B8 (read only; compile-proven).
- **D4 unwrap.** A frozen `{ ... }` block that is a builder's whole body loses its outer brace
  lines (B2: 286, 344; B4: 367, 375); the content is dedented one level.
- **D5 dropped driver lines.** Aliases with no remaining driver reader: 236 (`stations`), 238 to
  241 (`n_threads`, `seam_threads`, `carry_par`, `emitter`); frozen declarators that became
  bindings: 279, 285, 411, 534, 558, 559, 573. Comment-only lines (385 to 388, the stale
  `=== Epoch loop ===` heading; 550 to 557 and 560 to 572 move above the binding they document)
  are not statements.
- **D6 calls.** Each moved range is replaced in the driver by its one call.

## 4. The verifier, `scripts/nom_ep4_verify.py` (reduced G-R, authorized)

Lexing and statements from `scripts/nom_cpp.py`; a statement line is a source line's code
tokens. The tool holds its own manifest and imports nothing from the cut script.

- **V1 coverage and order.** The new driver, each builder call expanded into the builder's body
  (below the end marker), reproduces the frozen function's statement lines exactly once each, in
  order, up to D4 to D6. Each builder's body holds exactly its own range's lines.
- **V2 brace balance.** Every builder and every moved body alone is bracket-balanced.
- **V3 bindings.** Every frozen-scope name (and `config_`, `log_dir_`, `log_manager_`,
  `config_path_`) a body reads is bound once, by the declared D1 text or a D2 parameter of the
  declared type; a binding's type equals the frozen declarator's up to D1's declared spellings
  and a leading const (D3, rejected where the body writes the name); every root is the run, a
  kept parameter or an earlier binding.
- **V4 effects.** Per builder, the ordered logger calls, `traces.` member calls, throws and
  returns equal the frozen range's.
- **V5 declarations.** Each builder is declared once in its header, preceded by a Doxygen block
  with `@brief` and one `@param` per parameter.
- **Negative controls** (each must fail first): a swapped pair of lines in B2, a dropped line in
  B14, a const added to a written binding (B2's `slip_qc`), a reordered log call in B14, two
  builder calls swapped in the driver, a missing `@param` in a header.

`python scripts/nom_epoch_lint.py` passes. No epoch writer changes (the builders write no
`NetworkEpoch` group), so `tests/test_network_epoch_contract.cpp` gets no row.

## 5. Commits

(1) this file; (2) the cut, by `scripts/nom_ep4_cut.py` (text moves from the frozen file and
the templates of section 1) with `scripts/nom_ep4_verify.py`, and CLAUDE.md's routing text (the
driver's line count, the rule: new setup goes in a builder in its concern's file, called from
`runNetwork` in its order).
