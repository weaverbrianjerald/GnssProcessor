# NOM3 step EP1: the buildable specification

Read-only design for card NOM3 step EP1 (`docs/refound/NOM3_network_giants_2026-10-09.md`, section 3 "EP" and the frozen gates of section 5). No repository file was changed. It starts from the judges' winner (`design_reloc.md`: one canonical table, a generated character-exact cut, an independent relocation verifier with a binding check), grafts the improvements both judges agreed on, and fixes every defect they found after checking each one against the code (section 10 lists each defect, what the check found and what this specification does about it).

**Base.** Step EP0 exists: branch `nom/ep0`, commit `a453448` (worktree `scratchpad/wt_ep0`), built on the HV0 and WCF content that main `366385b` carries (`git diff --stat main nom/ep0` touches only EP0's 21 files and the card). Every line number below is a line of that commit unless another tree is named, so the EP0 knob names used here are the landed ones, not provisional. The frozen copy used for every measurement is `scratchpad/ep1design/base/` (`git show a453448:` and `git archive a453448`). The full call table at this base was regenerated with the inventory's scripts (`spec_tools/calltable.py`, output `base/calltable_ep0.md`), and the conversion of section 3 is generated from it (`spec_tools/convtable.py`, outputs `base/conv_table.md` and `base/conv_compact.md`, the latter reproduced as Appendix A).

**Shape at the base.** `runNetwork` is `ApplicationNetwork.cpp` 198-2937 (2 939-line file); setup 198-1228; the per-epoch lambda `processSyncedEpoch` 1229-2905 (1 676 lines), capturing 62 driver names plus `LoopSignal`; 99 calls (79 `network*` seams with 753 arguments; EP0 removed the `emitEpochStatus` forwarder and 16 configuration-derived arguments).

---

## 0. The design in one page

1. **Two objects.** `NetworkRun` (new `include/app/NetworkRun.hpp`): the run's seven references (context, configuration, network block, logger, traces, setup, log directory) and the five TD1a groups it owns by value (`engines`, `emit`, `lanes`, `timing`, `closed_loop`). `NetworkRunEngines` becomes the owner of the seven engines, built in place, never copied or moved. `NetworkEpoch` (new `include/app/NetworkEpoch.hpp`): the admitted epoch and twelve members grouped by the stage that writes them, built fresh each epoch after the admission.
2. **Engines owned in EP1, without moving a setup statement that logs.** `NetworkRun` is built right after `NetworkRunTraces traces(log_dir_);` (285) from a pure `NetworkEngineSpec`, the engines' constructor arguments, cut character-exact out of setup into `networkEngineSpec`. Every configuring statement keeps its place and reads the owned engine under its old name through a reference (`SlipQcChannel& slip_qc = run.engines.slip_qc;`). The one log line inside a cut range (the `debug_r_scale` warning, 762-765) stays in place.
3. **One seam rule on both sides of the solve.** A converted seam takes `(NetworkRun& run, const NetworkEpoch& ep, <outputs>&...)`: the epoch is always const, and each epoch group a seam writes arrives as a named non-const parameter that the spine line passes (`networkSignalIntegrity(run, ep, ep.pass_a, ep.funnel)`). After the solve the only outputs are `ep.solve.handoff` (RAIM, clock QC) and `ep.prep.sol` (the emitter); the driver stores the four returned values (`ep.ar_fixes`, `ep.fixed_cycles`, `ep.prep`, `ep.emitted`). The compiler rejects any other epoch write in a seam; a permanent lint checks the driver.
4. **Bodies stay character-exact.** Each converted definition opens with a generated preamble, one line per frozen parameter, its frozen declarator followed by `= <binding>;` (so a reference stays a reference and a copy stays a copy); writes bind through the output parameter and are marked `// written`. Seams whose bodies declare `run` or `ep` keep `run` / `ep` in the header and use `nrun` / `nep` in the definition (six seams; section 3.2).
5. **The solve's two `std::function` callbacks go.** The T2 replay calls `networkBuildEnsembleGauges(run, ep)` and `networkBuildCarryDdPriors(run, ep, dd_build, funnel)` directly, with card WCF's writer-end counter kept between them; the solve's signature now declares what the replay writes (`ws`, `est_index_of`, `dd_build`, `funnel`). The solve's internal `networkRhsAttribTrip` call (A4 withhold exit) is untouched: that seam keeps its explicit form, with an epoch-form adapter for the driver.
6. **Generated and verified like step TD.** One canonical table (four per-worker rows files), a generator (`scripts/nom_ep1_cut.py`), an independent verifier (`scripts/nom_ep1_verify.py`) whose binding check derives every binding from the frozen and the new source separately and accepts only eleven declared substitution classes, sixteen pre-registered negative controls, and named witness arms for bindings no golden distinguishes.
7. **Delivery.** EP1a-0 (tooling, no code change), EP1a-1 (types, run construction, epoch function; every call still explicit; gated alone), EP1b (four workers on disjoint files: W1 app 10 seams, W2 pre-solve and solve 13, W3 emit and post-fit 15, W4 diagnostics 24; the two shared generated files regenerated, never hand-merged), then EP1c (the compile-proven const tightenings) and the docs.

---

## 1. What EP1 assumes and what it leaves alone

**Lands first.** Card WCF (main `ee18652`..`e63b498`): `EpochWorkspace::we_culprits` (`NetworkRunTypes.hpp` 2714 region), no culprit pointers; the driver's rebuild closure passes `ws.we_culprits` (2454). Step HV0 (main `2fdc636`..`5334513`): `networkCouplingExactPrepare` in `NetworkBorderPrior.cpp`, `networkWriteStateSnapshot` in `NetworkStateSnapshotWrite.cpp`, `networkRxCbDemeanPrepass` in `NetworkLocalCarryHarvest.cpp`. Step EP0 (`a453448`): the 22 dead names gone; `NetworkKnobs` gains `fc_on`, `fc_shadow`, `sec_timers_on`, `clk_anchor_sigma`, `sh_dev_*`, `vce_wants_pb/cb`, `uwl_gal_top`, `streaming_on`, `within_epoch_armed`, `pd_*`, `max_duration_s`, `epoch_decimation_n`, `rhs_attrib_*`, `gap_census_on`, `trace_*`, `pl_warmup_on_sh_conv`, `amb_birth_floor_on`; `last_vce_hour` and `first_vce_s` live in `ctx.carry`. **EP0 must be merged to main before EP1a-1 starts**; the frozen source `<B>` is that merge commit (its `ApplicationNetwork.cpp` equals `a453448`'s; the generator's survey mode re-derives every line number from `<B>`).

**Untouched by EP1.** The inline blocks EP2 moves (the phase-datum election 1620-2125, the eq34 RPB allocation 2137-2154, the bounded drop 2226-2238, the gap-resume census 2326-2433, the funnel drains 1446-1525, the broadcast feed, the D0/D0b readout); the run end (`networkTeardown`, `NetworkRunEndView`, `NetworkRunEndInputs`, the 116-row table and its reports); setup order; every seam body below its preamble; the explicit forms of the adapter seams and every test that calls them; the snapshot format and fingerprint; every printed line.

---

## 2. The types

### 2.1 `NetworkRun` (`include/app/NetworkRun.hpp`, new; `src/core/app/NetworkRun.cpp`, new)

```cpp
#pragma once
/**
 * @file NetworkRun.hpp
 * @brief The network run as the epoch loop sees it: the run's references and the groups it
 *        owns (card NOM3 step EP1).
 *
 * Application::runNetwork builds one NetworkRun right after the trace owner, in place, and
 * passes it to Application::networkProcessEpoch_ for every epoch.  It holds:
 *   - seven references to objects runNetwork declares before it and keeps alive to the run
 *     end: the run context, the configuration and its network block, the main logger, the
 *     trace owner, the setup result and the log directory;
 *   - the five driver-owned groups of filter/NetworkRunGroups.hpp, by value: the estimation
 *     engines (NetworkRunEngines, the owner since EP1), the emit engines, the lane run, the
 *     run timing and the closed-loop tally.
 *
 * Passed as NetworkRun&, never const: C++ does not carry const through a reference member,
 * so a const run would still hand out a writable context, traces and setup, and most seams
 * write run state by design (counters, carry, traces, engines).  What a seam may not write
 * is stated by the epoch (app/NetworkEpoch.hpp) and by each seam's preamble, whose lines keep
 * the frozen declarators (`const NetworkEstimator& est = run.engines.est;`).
 *
 * Neither copyable nor movable: two corroborators capture the address of engines.slip_qc
 * (the prior-residual and block-Baarda resolvers, ApplicationNetwork.cpp 519 and 577), and
 * the run end binds const references to the groups.
 */
#include "filter/NetworkRunGroups.hpp"   // NetworkRunEngines, NetworkEngineSpec, the TD1a groups
#include "filter/NetworkWlRunTypes.hpp"  // SubBlock
#include <filesystem>
#include <memory>

namespace spdlog { class logger; }

namespace GnssProcessor {

class GnssConfig;
struct NetworkConfig;
struct NetworkKnobs;
struct NetworkRunContext;
struct NetworkRunTraces;
struct NetworkSetupResult;

struct NetworkRun {
    /// Binds the references and builds the owned groups from @p spec, each member at its
    /// final address (no engine is copied or moved).
    NetworkRun(NetworkRunContext& ctx, const GnssConfig& config, const NetworkConfig& nc,
               const std::shared_ptr<spdlog::logger>& log, NetworkRunTraces& traces,
               NetworkSetupResult& setup, const std::filesystem::path& log_dir,
               const NetworkEngineSpec& spec)
        : ctx(ctx), config(config), nc(nc), log(log), traces(traces), setup(setup),
          log_dir(log_dir), engines(spec),
          emit{ NetworkEmitRebase{}, NetworkPbEmitFilter(spec.pb_filter_cfg),
                NetworkClockEmitRebase(spec.clk_rebase_cfg) },
          lanes(spec.mw_wl_cfg, spec.ewl_cfg) {}
    NetworkRun(const NetworkRun&) = delete;
    NetworkRun& operator=(const NetworkRun&) = delete;
    NetworkRun(NetworkRun&&) = delete;
    NetworkRun& operator=(NetworkRun&&) = delete;

    // --- The run's references ----------------------------------------------------------
    NetworkRunContext&                     ctx;      ///< Knobs (const), cross-epoch carry state, service counters.
    const GnssConfig&                      config;   ///< The configuration (Application::config_); never written during the run.
    const NetworkConfig&                   nc;       ///< Its network block, config.getNetworkConfig().
    const std::shared_ptr<spdlog::logger>& log;      ///< "main_log" (runNetwork's local `log`).
    NetworkRunTraces&                      traces;   ///< Every trace stream and the run manifest.
    NetworkSetupResult&                    setup;    ///< Stations, sync, emitter, product writer, consumer; one address for the run.
    const std::filesystem::path&           log_dir;  ///< Application::log_dir_ (the producer AR's export root).
    // --- The groups the run owns (filter/NetworkRunGroups.hpp) ---------------------------
    NetworkRunEngines  engines;      ///< The estimation engines (est, carry_driver, carry_vce, prires, slip_qc, iono_deweight, fc_ppp_utils).
    NetworkEmitEngines emit;         ///< Emit re-base, phase-bias emission filter, clock emission re-base.
    NetworkLaneRun     lanes;        ///< The WL / EWL / UWL lane machinery and its tallies.
    NetworkRunTiming   timing;       ///< Loop start, D0b totals, the bracket scratch, the assembly wall.
    ClosedLoopTally    closed_loop;  ///< The held-out consumer's integer-recovery tallies.
};

static_assert(!std::is_copy_constructible_v<NetworkRun> && !std::is_move_constructible_v<NetworkRun>);

/// The engines' constructor arguments, a pure function of the configuration and the knobs
/// (no log line, no trace, no setup read).  Its body is the setup statements that built
/// them, cut character-exact from runNetwork (card NOM3 step EP1); step EP4 splits it into
/// its concerns' builders.  Defined in src/core/app/NetworkRun.cpp.
NetworkEngineSpec networkEngineSpec(const GnssConfig& config_, const NetworkConfig& nc,
                                    const NetworkRunContext& ctx);

/// Opens D0b sub-block @p b: records the wall clock and the estimator's section timers.
/// No-op unless network.trace_section_timers (NetworkKnobs::sec_timers_on).  Formerly the
/// driver closure subBegin (ApplicationNetwork.cpp 1063-1067); the body is unchanged.
void networkSubBegin(NetworkRun& run, SubBlock b);
/// Closes D0b sub-block @p b: adds its wall and timed deltas to NetworkRunTiming::sub_blocks.
/// Formerly the closure subEnd (1068-1080).
void networkSubEnd(NetworkRun& run, SubBlock b);

} // namespace GnssProcessor
```

| member | type | owns / refers | who writes it during the loop |
|---|---|---|---|
| `ctx` | `NetworkRunContext&` | runNetwork's `ctx` (266) | the seams (carry, counters, traces of state); knobs are const |
| `config`, `nc` | `const GnssConfig&`, `const NetworkConfig&` | `Application::config_`, its network block | nobody |
| `log` | `const std::shared_ptr<spdlog::logger>&` | runNetwork's `log` (199) | nobody (the object is the same one the lambda captured) |
| `traces` | `NetworkRunTraces&` | runNetwork's `traces` (285) | the seams |
| `setup` | `NetworkSetupResult&` | runNetwork's `setup` (233) | a few seams (stations' state managers, emitter, writer) |
| `log_dir` | `const std::filesystem::path&` | `Application::log_dir_` | nobody |
| `engines` | `NetworkRunEngines` | owned | the seams through the preamble declarators; setup configures it in place |
| `emit` | `NetworkEmitEngines` | owned | `networkEmitStore`, the warm resume and the snapshot |
| `lanes` | `NetworkLaneRun` | owned | the DD build and the install; setup fills `ewl_lanes` |
| `timing` | `NetworkRunTiming` | owned | `networkSubBegin`/`End`, `networkPassB_` (`asm_wall_ms`), the readout |
| `closed_loop` | `ClosedLoopTally` | owned | `networkEmitStore` |

### 2.2 `NetworkRunEngines` as the owner, `NetworkEngineSpec`, `NetworkRunTiming` (`include/filter/NetworkRunGroups.hpp`)

`NetworkRunEngines` stays in `NetworkRunGroups.hpp`, its one home since TD1a (the types design gave it two homes; the reader design moved it; both rejected). The header now includes the six engine headers (`NetworkEstimator.hpp`, `NetworkInvariantCarryDriver.hpp`, `NetworkCarryVce.hpp`, `NetworkPriorResidual.hpp`, `NetworkIonoDeweightGate.hpp`, `slip/NetworkRawObservables.hpp`, `common/PppUtils.hpp`); no cycle (checked: none of them includes `NetworkRunGroups.hpp`; its includers are three seam headers, `NetworkDdPriorBuild.hpp`, `NetworkEmitStore.hpp`, `NetworkInstallCarry.hpp`, and about two dozen sources). The 14 run-end files that read `v.run.engines.X` already see the header (they need `NetworkRunEndInputs` complete) and change in no character: through `const NetworkRunEndInputs&` a value member is const exactly as the old `const T&` member was.

```cpp
/// The constructor arguments of the run's engines and groups (card NOM3 step EP1), computed
/// once by networkEngineSpec (app/NetworkRun.hpp) from the configuration and the knobs.  Each
/// field is the setup local it replaced, with that local's name and type.
struct NetworkEngineSpec {
    std::vector<std::string>            vce_families;          ///< carry_vce ? parseComponents(networkCarryVceComponents()) : {} (was 445-447).
    double                              vce_window_epochs{0};  ///< static_cast<double>(config_.networkCarryVceWindowEpochs()) (448).
    double                              vce_seed_floor{0};     ///< pl_var_floor (434, 448).
    double                              vce_seed_rate{0};      ///< pl_q_amb (431, 448).
    std::vector<std::string>            prires_families;       ///< knobs.prires_on ? parseComponents(nc.prior_residual_families) : {} (473-484).
    double                              prires_window_epochs{0};
    double                              prires_min_rows{0};
    double                              prires_report_sigma{0};
    bool                                prires_corrob{false};
    bool                                prires_s0g{false};
    NetworkInvariantCarryDriver::Config carry_cfg;             ///< The DD-carry driver's configuration (was 665-713).
    NetworkMwWlLayer::Config            mw_wl_cfg;             ///< The WL-1 layer's configuration (756-761).
    NetworkMwWlLayer::Config            ewl_cfg;               ///< The EWL lane's configuration (773-782).
    NetworkPbEmitFilter::Config         pb_filter_cfg;         ///< (846-852)
    NetworkClockEmitRebase::Config      clk_rebase_cfg;        ///< (853-858)
    IonoConstraintDeweightConfig        idw;                   ///< A copy of config_.getIonoConstraintDeweightConfig() (896).
};

/// The run's estimation engines, owned (card NOM3 step EP1; card NOM2 step TD1a made this name
/// a const view, so every run-end report's `engines.X` reads unchanged).  Built in place inside
/// NetworkRun from a NetworkEngineSpec; setup then configures the members through references
/// (slip-QC parameters, corroborators, release tracking, estimator settings, warm resume).
struct NetworkRunEngines {
    explicit NetworkRunEngines(const NetworkEngineSpec& s)
        : slip_qc(), fc_ppp_utils(),
          carry_vce(s.vce_families, s.vce_window_epochs, /*split_by_const=*/true,
                    /*seed_floor=*/s.vce_seed_floor, /*seed_rate=*/s.vce_seed_rate),
          prires(s.prires_families, s.prires_window_epochs, s.prires_min_rows,
                 s.prires_report_sigma, s.prires_corrob, s.prires_s0g),
          carry_driver(s.carry_cfg),
          iono_deweight(std::set<std::string>(s.idw.stations.begin(), s.idw.stations.end()),
                        s.idw.factor, s.idw.adaptive, s.idw.window_epochs, s.idw.sigma_hi,
                        s.idw.sigma_lo, s.idw.floor, s.idw.rate_limit, s.idw.min_epochs),
          est() {}
    NetworkRunEngines(const NetworkRunEngines&) = delete;
    NetworkRunEngines& operator=(const NetworkRunEngines&) = delete;

    /// The slip-QC channel.  Declared first, so it is destroyed last: two corroborators
    /// capture its address.
    SlipQcChannel               slip_qc;
    std::unique_ptr<PppUtils>   fc_ppp_utils;  ///< The fast tier's wavelength provider; null unless NetworkKnobs::fc_on (assigned in setup, 417).
    NetworkCarryVce             carry_vce;     ///< The LS-VCE carry-row monitor (M2); was carry_vce_est.
    NetworkPriorResidual        prires;        ///< The prior-residual census; was prires_est.
    NetworkInvariantCarryDriver carry_driver;  ///< The DD cycle-basis invariant carry (card 4).
    NetworkIonoDeweightGate     iono_deweight; ///< The SH-VTEC-constraint deweight gate.
    NetworkEstimator            est;           ///< The joint estimator: one instance, reset and reused every epoch (its station-block pool).
};
static_assert(!std::is_copy_constructible_v<NetworkRunEngines> && !std::is_move_constructible_v<NetworkRunEngines>);
```

Member order is today's construction order (292, 416, 448, 473, 714, 897, 955), so construction and destruction order among the engines are unchanged. No engine constructor logs or touches a trace (checked: `NetworkEstimator() = default`, `NetworkInvariantCarryDriver(Config)` allocates its inflation monitor only when calibration is armed, the others store their arguments; `PppUtils`, the one constructor that logs, is still created by `make_unique` at its setup line, 403 and 417); no engine has a user-declared destructor.

`NetworkRunTiming` gains the bracket scratch the closures and the readout used (a declared field move; same initializers, same lifetime: they lived in runNetwork's frame for the run):

```cpp
    std::vector<SubBlockTimer> sec_sub_ep0 = std::vector<SubBlockTimer>(SB_COUNT);  ///< D0b: the epoch-top copy of sub_blocks the readout differences against (was runNetwork 1060).
    std::vector<std::chrono::steady_clock::time_point> sub_wall0 =
        std::vector<std::chrono::steady_clock::time_point>(SB_COUNT);              ///< networkSubBegin's wall stamp per sub-block (1061).
    std::vector<NetworkNormalEquations::SectionTimers> sub_tmr0 =
        std::vector<NetworkNormalEquations::SectionTimers>(SB_COUNT);              ///< networkSubBegin's section-timer stamp per sub-block (1062).
```

`loop_begin`'s comment changes from "just before the estimator is built" to "just before the estimator is configured" (the default-constructed estimator now exists from the run's construction; the masked D0 wall loses an empty construction). `NetworkRunEndInputs` is unchanged; the driver binds it as `NetworkRunEndInputs{ run.engines, run.emit, run.lanes, run.timing, run.closed_loop }` (was 2933-2936).

### 2.3 `NetworkEpoch` (`include/app/NetworkEpoch.hpp`, new)

```cpp
#pragma once
/**
 * @file NetworkEpoch.hpp
 * @brief One epoch of the network producer: everything the stages of
 *        Application::networkProcessEpoch_ hand each other (card NOM3 step EP1).
 *
 * Built once per processed epoch, right after the admission (the event injector and the
 * canonical-pair screen) fixes the observations the producer sees, and destroyed at the
 * epoch's end.  Construction is the per-epoch reset; nothing here survives to the next
 * epoch.  A value that must survive is run state (NetworkRunContext), never a member here.
 *
 * Members in the order the stages fill them.  "Written by" is the authoritative list: a
 * writer receives its group as a non-const parameter on its spine line, so
 * `grep -n "ep\.pass_a)" src/core/app/ApplicationNetworkEpoch.cpp` lists every writer of
 * the layout and `grep -rn "= ep\.pass_a\.present;" src` every reader of `present`.
 *
 *   member        group type           available from            written by
 *   se            const SyncedEpoch&   construction              the admission (before construction)
 *   clock         EpochClock           the epoch top             the driver, once
 *   funnel        NetworkSlipFunnel    the funnel's opening      the driver's drains (inline until EP2), networkPassASlipScan_ (raw subset),
 *                                                                networkSignalIntegrity, networkMeasurementAdmission;
 *                                                                declared writable, read only (EP1c makes them const): networkBuildCarryDdPriors,
 *                                                                networkSolveGlue (its replay's DD rebuild)
 *   pass_a        EpochPassA           PASS A's sizing           networkPassAHead_, networkPassASlipScan_, networkAssemblyPrefilter_,
 *                                                                networkSignalIntegrity, networkMeasurementAdmission, the phase-datum election
 *                                                                (inline until EP2: shared_phase_keys), networkPassB_ (modeled, present)
 *   columns       EpochColumns         networkAssemblyRegisterColumns_   that seam only
 *   assembly      EpochAssembly        networkPassB_             networkPassB_; est_index_of also networkSolveGlue (each T2 replay rebuilds it)
 *   dd_build      EpochDdBuild         the DD build              networkBuildCarryDdPriors (the driver's primary call and each replay's)
 *   ws            EpochWorkspace       construction (empty)      networkSolveGlue (the primary DD build reads it empty, card WCF)
 *   solve         NetworkSolveGlueResult  the solve              the driver stores the solve's return; solve.handoff also networkPostFitRaim, networkClockQc
 *   ar_fixes      ArEpochFixes         the producer AR           the driver stores networkProducerAr's return
 *   fixed_cycles  FixedCycleEmission   the fixed-cycle fold      the driver stores networkFixedCycles's return
 *   prep          NetworkSolPrepResult solution prep             the driver stores networkSolPrep's return; prep.sol also networkEmitStore (the re-base)
 *   emitted       bool                 after networkEmitStore    the driver, once
 *
 * How a seam reads and writes it:
 *   - every converted seam takes `const NetworkEpoch& ep`; the groups it writes arrive as
 *     separate non-const parameters named after the member (`EpochPassA& pass_a`);
 *   - its preamble binds each name it reads through `ep`, with the frozen declarator
 *     (`const std::vector<bool>& present = ep.pass_a.present;`), and each name it writes
 *     through its output, marked `// written`; below the preamble the body never spells `ep.`;
 *   - a seam reads only groups available at its spine line (scripts/nom_epoch_lint.py);
 *   - no seam keeps a pointer or a copy of an epoch member across a call (card WCF's defect
 *     was a by-value pointer copy that stayed null while the original moved on).
 * After the solve the seams' outputs are solve.handoff and prep.sol; the driver's own writes
 * are the four stores above (the lint checks the driver, the compiler checks the seams).
 *
 * Const is deep: no member, here or in a struct held by value inside it, is a non-const
 * reference, a pointer to non-const, a smart pointer or a std::function
 * (scripts/nom_ep1_verify.py check V5).  Not covered: epoch-scoped state a run-owned engine
 * holds (the estimator's per-epoch columns and blocks, SlipQcChannel::epoch); its writers are
 * the engines' own callers.
 *
 * Neither copyable nor movable (se is a reference); it cannot bind a temporary epoch.
 */
#include "filter/NetworkRunTypes.hpp"      // StationAssembly, EpochWorkspace, EpochAction
#include "filter/NetworkSolveGlue.hpp"     // NetworkSolveGlueResult, NetworkPostFitHandoff
#include "filter/NetworkSolPrep.hpp"       // NetworkSolPrepResult
#include "filter/NetworkArCycleBasis.hpp"  // ArEpochFixes
#include "filter/NetworkFixedCycles.hpp"   // FixedCycleEmission
#include "filter/NetworkZStore.hpp"        // ArcKey
#include "common/PppObservation.hpp"
#include <Eigen/Core>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace GnssProcessor {

struct SyncedEpoch;
struct NetworkRun;

/// The epoch's time base and the phase-leveling carry's warmth, stored once at the epoch top
/// from the driver's three const locals (dt, pl_now_s, carry_warm), which keep their names
/// for the inline blocks.
struct EpochClock {
    double dt{0.0};           ///< Seconds since the previous processed epoch, week-aware; 0 on the first.  (The DD-carry predict uses its own TOW-only step, card NOM3 section 2 finding 4.)
    double pl_now_s{0.0};     ///< This epoch in continuous GPS seconds.
    bool   carry_warm{false}; ///< The phase-leveling carry is past its warm-up (the SH convergence latch, or the emitted-epoch count).
};

/// The slip funnel (card C3c): every (station roster index, ambiguity key) this epoch treats
/// as slipped, the object standing invariants 1 and 7 turn on.  Opened at the epoch top with
/// the previous epoch's deferred flags, grown by PASS A and the two admission screens, read
/// from the phase datum on.  Step EP2 moves the type and its drains to filter/NetworkSlipFunnel.
struct NetworkSlipFunnel {
    std::set<std::pair<int, std::string>> carry_slipped_ambs; ///< The funnel.
    std::set<std::pair<int, std::string>> carry_slipped_raw;  ///< Its raw-detector subset (Iono-2 v3k): what a raw observable saw.
};

/// The layout after PASS A.
struct EpochPassA {
    std::vector<std::vector<PppObservation>> modeled;   ///< Per station: this epoch's modelled observations.
    std::vector<bool>                        present;   ///< Per station: modelled this epoch and still in the solve.
    std::set<std::string>                    shared_phase_keys;  ///< Legacy: the pivot's keys; prioritized: the anchored union.
    int                                      n_dropped_sats{0};  ///< Satellites the observability prefilter dropped.
};

/// The shared columns registered for this epoch (written once, by networkAssemblyRegisterColumns_).
struct EpochColumns {
    std::vector<std::string>                         registered_clock_sats;
    std::vector<std::pair<std::string, std::string>> registered_code_keys;
    std::vector<std::string>                         col_desc;   ///< One readable descriptor per shared column.
    int n_clock{0};
    int n_pb{0};
    int n_cb{0};
    int n_present{0};
};

/// The station blocks PASS B assembled.
struct EpochAssembly {
    std::vector<StationAssembly> asm_res;            ///< Per station: the assembled block.
    std::vector<int>             est_index_of;       ///< Per station: its estimator slot, -1 when not added (each T2 replay rebuilds it).
    int                          total_pins{0};      ///< Ambiguity pins folded this epoch.
    double                       sec_asm_ms_epoch{0.0}; ///< This epoch's assembly wall (D0; wall clock only).
};

/// The DD build's reverse maps (carry group -> chord -> ambiguity key); the last build
/// before the accepted solve wins; read by networkInstallCarry.
struct EpochDdBuild {
    std::map<int, std::map<ArcKey, std::string>> carry_arc_amb_key;   ///< Band 1.
    std::map<int, std::map<ArcKey, std::string>> carry_arc_amb_key2;  ///< Band 2 of a lane pair group.
};

struct NetworkEpoch {
    /// @param se The admitted epoch: the source's epoch, or a scratch copy the epoch function
    ///           owns and declares before this object (the injector's, the screen's).
    explicit NetworkEpoch(const SyncedEpoch& se) : se(se) {}
    NetworkEpoch(const SyncedEpoch&&) = delete;
    NetworkEpoch(const NetworkEpoch&) = delete;
    NetworkEpoch& operator=(const NetworkEpoch&) = delete;
    NetworkEpoch(NetworkEpoch&&) = delete;
    NetworkEpoch& operator=(NetworkEpoch&&) = delete;

    const SyncedEpoch&     se;            ///< The admitted observations.
    EpochClock             clock;         ///< dt, the epoch's seconds, the carry's warmth.
    NetworkSlipFunnel      funnel;        ///< The slip funnel.
    EpochPassA             pass_a;        ///< The layout.
    EpochColumns           columns;       ///< The shared columns.
    EpochAssembly          assembly;      ///< The station blocks.
    EpochDdBuild           dd_build;      ///< The DD build's reverse maps.
    EpochWorkspace         ws;            ///< The replay capture channel, the clock maps, the within-epoch culprit sets (card WCF).
    NetworkSolveGlueResult solve;         ///< The shared solution and its datum; solve.handoff is the post-fit hand-off.
    ArEpochFixes           ar_fixes;      ///< The producer AR search's fixes.
    FixedCycleEmission     fixed_cycles;  ///< The fixed-cycle fold's held solve.
    NetworkSolPrepResult   prep;          ///< The solution prepared for the wire.
    bool                   emitted{false};///< This epoch's solution reached the wire (networkEmitStore counted an emission).

    /// The solution the wire carries: the held vector when the fixed-cycle fold emits, else
    /// the float solve (card FCF).  Read by solution prep and the emitter; every other seam
    /// reads solve.x_s.
    const Eigen::VectorXd& x_emit() const { return fixed_cycles.emit ? fixed_cycles.x_held : solve.x_s; }
};

/// A measure-only per-epoch instrument's form (the row type of step EP3's
/// kNetworkEpochObservers).  Every converted seam that returns void and has no kept or
/// output parameter has it.
using NetworkEpochObserver = void (*)(NetworkRun& run, const NetworkEpoch& ep);

} // namespace GnssProcessor
```

**Amended at the EP1 review round (2026-10-14).** Two sentences of the banner above did not hold
on the generated tree, and the generated header differs from them (the text above stays as
pre-registered). The grep recipes missed four of the six writer spine lines (the pattern matched
`pass_a` only as the last argument) and three readers of `present` (the fallback name `nep`, the
two adapters' forwarding arguments); the header now names `scripts/nom_epoch_lint.py` (V11) as
the authority and gives `grep -nE "ep\.pass_a[,)]"` and `grep -rnE "n?ep\.pass_a\.present" src`.
`EpochClock::carry_warm`'s documentation named the T1d SH-convergence latch, which the REMOVE-key
deletion removed; at a base without it the header reads "the emitted-epoch count reached
network.phase_leveling_warmup_epochs" (`templates.RETIRED_TEXT`).

Decisions inside the type, each weighed:

- **Groups by writing stage**, with today's local names as member names (the precedent of `PhaseDatumCandidates`), so a body's name greps to its member. `funnel` is the card's "slips" group under EP2's name; `dd_build` is the card's "carry" group; `ar_fixes`, `fixed_cycles` and `emitted` are stages card FCF added after the card was written.
- **Per-station vectors are sized where the frozen locals were declared** (PASS A 1528-1529, PASS B 2262-2268), not in the constructor: epochs that end before a stage allocate nothing for it, as today.
- **`x_emit()` is an accessor**, not a pointer member: the epoch holds no pointer into itself (reloc's `const Eigen::VectorXd* x_emit` is gone).
- **The scratch `SyncedEpoch`s stay locals** of the epoch function, declared before `ep`, so they outlive the reference.
- **`EpochWorkspace` keeps its name and type** (five test files construct it).

### 2.4 The post-fit hand-off (`include/filter/NetworkSolveGlue.hpp`)

```cpp
/// What the post-fit tiers hand the epoch stash: seeded by the solve (the within-epoch tier's
/// refloats), extended by networkPostFitRaim and networkClockQc, read by networkStashStatus.
/// With prep.sol, the only epoch members a seam writes after the solve.
struct NetworkPostFitHandoff {
    std::set<std::string> clock_refloat_sats;      ///< Satellites the stash refloats: the T2 and RAIM culprits, then the clock NIS and monitor culprits.
    std::set<std::string> retro_flag_sats;         ///< The retro-bump feed (networkRetroUraBookkeeping, inside networkClockQc).
    int                   n_arc_refloat_epoch{0};  ///< Isolated-arc refloats this epoch (the epoch-status row).
};
```

`NetworkSolveGlueResult` replaces its fields `clock_refloat_sats`, `retro_flag_sats`, `n_arc_refloat_epoch` by `NetworkPostFitHandoff handoff;`. `retro_feed` stays a field of the result: only the solve writes it, RAIM and clock QC take it by value (judge 2's graft; it keeps the hand-off exactly the writers' write set). The three epilogue lines `NetworkSolveGlue.cpp` 2853, 2854, 2856 become `result.handoff.X = ...` (declared FIELD_MOVE).

### 2.5 Free functions EP1 adds

| function | file | body |
|---|---|---|
| `networkEngineSpec(config_, nc, ctx)` | `src/core/app/NetworkRun.cpp` | the cut setup statements of section 4.1 table, character-exact, under a generated preamble that repeats the frozen alias declarations they read (`const bool carry_invariant = ctx.knobs.carry_invariant;` and so on, identical text) and binds each built local to its field (`NetworkInvariantCarryDriver::Config& carry_cfg = spec.carry_cfg;` replaces the frozen `NetworkInvariantCarryDriver::Config carry_cfg;`) |
| `networkSubBegin(run, b)`, `networkSubEnd(run, b)` | `src/core/app/NetworkRun.cpp` | the closures' bodies (1063-1080) verbatim under a preamble `NetworkRunContext& ctx = run.ctx; NetworkEstimator& est = run.engines.est; NetworkRunTiming& timing = run.timing; auto& sub_wall0 = timing.sub_wall0; auto& sub_tmr0 = timing.sub_tmr0;` |
| `networkIsUncarriedThirdBand(knobs, cons, signal)` | `include/filter/NetworkDdPriorBuild.hpp`, `src/core/filter/NetworkDdPriorBuild.cpp` (its only consumer) | the closure's body (738-755) verbatim under `const auto uwl_excl_on = knobs.uwl_on; const auto uwl_gal_top = knobs.uwl_gal_top; const auto uwl_dual = knobs.uwl_dual_on;` (the init-captures, which a non-mutable lambda makes const). The knob `uwl_gal_top`'s comment (`NetworkRunContext.hpp` 1113) names the function |

The predicate moves because a run built at line 285 cannot refer to a closure declared at line 738; after EP0 its three captures are knobs, so it is a pure function of the knobs and belongs beside the DD build.

### 2.6 `Application.hpp`

```cpp
    /// What one epoch tells the epoch source (the lambda's former local enum, 1229).
    enum class LoopSignal { Next, Stop };

    /**
     * @brief One synced epoch through the network producer (card NOM3 step EP1).
     *
     * The admission (start offset, event injector, canonical-pair screen), the NetworkEpoch,
     * then the stages in spine order: roster, admission tail (broadcast feed, maximum
     * duration, decimation), epoch top, the slip funnel, PASS A and the screens, the phase
     * datum, the columns, pre-assembly, PASS B, the gauges and the DD build, the solve, the
     * install, AR and fixed cycles, solution prep, emit, the post-fit diagnostics, RAIM,
     * clock QC, the measure-only instruments, the stash, the D0 readout and the snapshot.
     * Four stages end the epoch early (networkPassAHead_, networkAssemblyRegisterColumns_,
     * networkPassB_, networkSolveGlue), the admission three ways.
     * Implemented in ApplicationNetworkEpoch.cpp.
     *
     * @param run    The run, built once by runNetwork after the traces.
     * @param se_raw The synced epoch as the source delivered it.
     * @return LoopSignal::Stop at the maximum-duration cutoff, else LoopSignal::Next.
     */
    LoopSignal networkProcessEpoch_(NetworkRun& run, const SyncedEpoch& se_raw);

    friend struct NetworkEpochContractProbe;   // tests/test_network_epoch_contract.cpp reads the five private seam types
```

plus forward declarations of `NetworkRun`, `NetworkEpoch`, `EpochPassA`, `NetworkSlipFunnel`, `EpochColumns`, `EpochAssembly`, and the five member seams' new declarations (section 3.3).

### 2.7 What the compiler checks

`static_assert`s beside the definitions: `NetworkRun`, `NetworkRunEngines`, `NetworkEpoch` are neither copy- nor move-constructible nor assignable; `!std::is_constructible_v<NetworkEpoch, SyncedEpoch&&>`. Every post-solve seam takes `const NetworkEpoch&`, so writing a member there does not compile; pre-solve seams can write only through their output parameters. The contract test (section 5.9) pins every converted seam's parameter list.

---

## 3. The seam conversion

### 3.1 The rule

Every seam the epoch function calls at top level converts to one of four forms; everything else keeps its text:

| form | signature | used by |
|---|---|---|
| reader | `R networkX(NetworkRun& run, const NetworkEpoch& ep [, kept...])` | 40 direct seams and 9 adapters' epoch forms (the observers among them) |
| writer | `R networkX(NetworkRun& run, const NetworkEpoch& ep [, kept...], Out1& o1 [, Out2& o2...])` | 12 seams (5 members, 5 direct, 2 adapters): 9 before the solve, 3 after |
| run only | `void networkX(NetworkRun& run)` | `networkClockLevelRestoreRaise` (reads no epoch member, is not an observer) |
| adapter | the explicit form unchanged, plus an epoch form of one of the above whose body is one forwarding call | 11 seams with another caller (section 3.6) |
| unchanged | today's text, arguments read the epoch function's aliases | the input stage (`networkInjectEpoch`, `networkCanonicalPairAdmission`: they produce the epoch `ep` is built on), the calls inside the EP2 blocks (they take block locals), the method calls (`est.*`, `traces.*`, `carry_driver.*`, `ctx.carry.bb/cg.beginEpoch`, `LogManager::setCurrentEpochTime`, `feedStore`) |

Outputs: a writer receives the whole group it is listed as a writer of (2.3), or the single member when it writes one member of a group another stage owns (`networkSolveGlue`: `std::vector<int>& est_index_of`; `networkEmitStore`: `NetworkSolutionEpoch& sol`; single members keep their frozen declarator and name, so they need no preamble line). Kept parameters: only the two whose argument differs between call sites (`networkFastClockTick`'s `shadow_pass`, `false` at 1364 and `true` at 1404; `networkGrossLosScreen`'s `phase`, `GrossLosPhase::Decide` at 2523 and the `gl_emit_exclude` conditional at 2635). Return types are unchanged.

### 3.2 The preamble, and the name pre-flight

A converted definition opens with:

```cpp
    // ---- From the run and the epoch (card NOM3 step EP1; generated by scripts/nom_ep1_cut.py,
    //      each line checked by scripts/nom_ep1_verify.py).  Reads bind through `ep`; writes
    //      bind through the output parameters and are marked `// written`. ----
    <frozen parameter declaration, default argument removed> = <binding>;   [// written]
    ...
    // ---- End of the bindings ----
```

then the frozen body, byte for byte. Because each line is the frozen declarator, a reference stays a reference and a copy stays a copy taken at entry, where the call took it (nothing runs between argument evaluation and the body). Folded argument expressions are restricted to a fold grammar (`&`/`*` prefix, member chains, `.get()` on a smart pointer, `ep.x_emit()`, and side-effect-free conditionals over member reads such as `run.engines.slip_qc.epoch.computed ? &run.engines.slip_qc.epoch : nullptr`), so folding moves no side effect. A comment inside a rewritten argument list moves to the preamble line of the parameter it annotated (the four in `networkWriteStateSnapshot`'s call, 2892-2899).

**Name pre-flight (fixes the collision defect all three designs had).** The header always reads `(NetworkRun& run, const NetworkEpoch& ep, ...)`. If the frozen body declares or uses as a free identifier, at any depth, `run`, `ep` or an output's name, the definition uses the fallback names `nrun`, `nep`, `<output>_out` (parameter names may differ between declaration and definition), and its preamble's first comment line says why. At `a453448` this applies to six seams: `networkCouplingExactPrepare` (`CouplingExactEpoch& ep`, `NetworkBorderPrior.cpp` 780), `networkFixedCycles` (`const long long ep`, `NetworkFixedCycles.cpp` 741), `networkRedundancyCensus` (`const long long ep`, `NetworkRedundancyCensus.cpp` 241), `networkContributionsEpochTail` (`const ContributionTally run`, `NetworkContributions.cpp` 746), and the nested cases `networkPassB_` (`ep`, `ApplicationNetworkAssembly.cpp` 3574) and `networkSignalRosterAudit` (`ep` 1750, `run` 1797, `SignalRoster.cpp`). The adapters' explicit bodies are untouched, so `networkLedgerEmitFold` (240) and `networkPbHoldCloneUpdate` / `Harvest` (548, 1109) need nothing. No output name (`pass_a`, `funnel`, `columns`, `assembly`, `dd_build`, `handoff`) occurs in its writers' bodies (scanned). Verifier check V13 re-runs the scan on the final tree.

### 3.3 Every call of the per-epoch lambda, in order

Line numbers are `ApplicationNetwork.cpp` at `a453448`. "Writes" lists the epoch outputs (all other arguments fold into the preamble; Appendix A gives every parameter's binding and frozen kind).

| # | line | call | args | form | EP1 call (inside `networkProcessEpoch_`) | epoch writes |
|---|---|---|---|---|---|---|
| 1 | 1251 | `LogManager::setCurrentEpochTime` | 1 | unchanged | same text | |
| 2 | 1261 | `networkInjectEpoch` | 5 | unchanged (input) | same text | (its scratch) |
| 3 | 1274 | `networkCanonicalPairAdmission` | 6 | unchanged (input) | same text, then `NetworkEpoch ep(se);` | (its scratch) |
| 4 | 1289 | `networkBuildSignalRoster` | 6 | reader | `networkBuildSignalRoster(run, ep)` | |
| 5 | 1300 | `est.attribArm` | 1 | unchanged | same text | |
| 6 | 1337 | `setup.brdc_apriori->feedStore` | 2 | unchanged | same text | |
| 7 | 1364 | `networkFastClockTick` | 7 | reader, kept `shadow_pass` | `networkFastClockTick(run, ep, false)` | |
| 8 | 1371 | `networkParamInventoryNoteSatellites` | 2 | reader (observer) | `networkParamInventoryNoteSatellites(run, ep)` | |
| 9 | 1382 | `est.sectionTimers` | 0 | unchanged | same text | |
| 10 | 1389 | `traces.rotateServiceCsvs` | 2 | unchanged | same text | |
| 11 | 1393 | `traces.writeHealthRow` | 10 | unchanged | same text | |
| 12 | 1404 | `networkFastClockTick` | 7 | reader, kept | `networkFastClockTick(run, ep, true)` | |
| 13 | 1414 | `traces.emitEvent` (the `emitEvent` forwarder) | 9 | unchanged | same text | |
| 14 | 1539 | `networkPassAHead_` | 7 | writer (member) | `networkPassAHead_(run, ep, ep.pass_a)`; SkipEpoch ends the epoch | pass_a |
| 15 | 1545 | `networkPassASlipScan_` | 12 | writer (member) | `networkPassASlipScan_(run, ep, ep.pass_a, ep.funnel)` | pass_a, funnel (raw subset) |
| 16 | 1559 | `networkAssemblyPrefilter_` | 8 | writer (member) | `networkAssemblyPrefilter_(run, ep, ep.pass_a)` | pass_a |
| 17 | 1579 | `networkSignalIntegrity` | 13 | writer, adapter | `networkSignalIntegrity(run, ep, ep.pass_a, ep.funnel)` | pass_a, funnel |
| 18 | 1606 | `networkMeasurementAdmission` | 13 | writer, adapter | `networkMeasurementAdmission(run, ep, ep.pass_a, ep.funnel)` | pass_a, funnel |
| 19-35 | 1633-2115 | the election's calls (`networkBuildPhaseDatumCandidates` ... `prmRespond`, three `traces.emitEvent`) | | unchanged (EP2 block) | same text | (shared_phase_keys, through the alias) |
| 36 | 2166 | `networkLevelAuthorityCensus` | 8 | reader (observer) | `networkLevelAuthorityCensus(run, ep)` | |
| 37 | 2173 | `networkAssemblyRegisterColumns_` | 14 | writer (member) | `networkAssemblyRegisterColumns_(run, ep, ep.columns)`; SkipEpoch ends the epoch | columns |
| 38 | 2191 | `networkSdEwlAr` | 9 | reader | `networkSdEwlAr(run, ep)` | |
| 39 | 2203 | `networkRxCbDemeanPrepass` | 4 | reader | `networkRxCbDemeanPrepass(run, ep)` | |
| 40 | 2217 | `networkOraclePinEpoch` | 6 | reader | `networkOraclePinEpoch(run, ep)` | |
| 41-42 | 2235-2236 | `networkResponseLatchTally`, `networkAmbFloatApplyDrops` | | unchanged (EP2 block, the bounded drop) | same text | |
| 43 | 2245 | `networkClockLevelRestoreRaise` | 2 | run only | `networkClockLevelRestoreRaise(run)` | |
| 44 | 2252 | `networkHandoverStransformBeforeRows` | 8 | reader, adapter | `networkHandoverStransformBeforeRows(run, ep)` | |
| 45 | 2254 | `networkCarryDecide` | 4 | reader, adapter | `networkCarryDecide(run, ep)` | |
| 46 | 2255 | `networkCouplingExactPrepare` | 5 | reader (fallback names) | `networkCouplingExactPrepare(run, ep)` | |
| 47 | 2270 | `networkPassB_` | 23 | writer (member, fallback names) | `networkPassB_(run, ep, ep.pass_a, ep.assembly)`; SkipEpoch ends the epoch | pass_a, assembly |
| 48 | 2289 | `networkBuildEnsembleGauges` | 8 | reader | `networkBuildEnsembleGauges(run, ep)` (the closure goes) | |
| 49 | 2387 | `carry_driver.carriedChordValue` | 4 | unchanged (EP2 block, the gap census) | same text | |
| 50 | 2451 | `networkBuildCarryDdPriors` | 20 | writer | `networkBuildCarryDdPriors(run, ep, ep.dd_build, ep.funnel)` inside `{ CarryEigenScope es(carry_par, n_threads); ... }` (the closure goes) | dd_build, funnel (read only, declared writable) |
| 51 | 2466 | `carry_driver.beginEpoch` | 3 | unchanged | same text (reads the block's shadowing TOW-only `dt`, finding 4) | |
| 52 | 2471 | `networkSolveGlue` | 25 | writer | `ep.solve = networkSolveGlue(run, ep, ep.ws, ep.assembly.est_index_of, ep.dd_build, ep.funnel)`; `ep.solve.action == SkipEpoch` ends the epoch | ws, est_index_of, dd_build, funnel |
| 53 | 2505 | `networkInstallCarry` | 17 | reader | `networkInstallCarry(run, ep)` (gate `if (carry_invariant)` unchanged) | |
| 54 | 2511 | `networkProducerAr` | 12 | reader | `ep.ar_fixes = networkProducerAr(run, ep)` | (driver store) |
| 55 | 2523 | `networkGrossLosScreen` | 12 | reader, kept `phase` | `networkGrossLosScreen(run, ep, GrossLosPhase::Decide)` | |
| 56 | 2534 | `networkFixedCycles` | 11 | reader (fallback names) | `ep.fixed_cycles = networkFixedCycles(run, ep)` | (driver store) |
| 57 | 2538 | `networkSolPrep` | 17 | reader, adapter | `ep.prep = networkSolPrep(run, ep)` | (driver store) |
| 58 | 2558 | `networkPbHoldCloneUpdate` | 6 | reader, adapter | `networkPbHoldCloneUpdate(run, ep)` | |
| 59 | 2560 | `networkEmitStore` | 25 | writer | `networkEmitStore(run, ep, ep.prep.sol)`, then `ep.emitted = ctx.counters.n_emitted != fc_n_emitted_before;` | prep.sol |
| 60 | 2572 | `networkFastClockHarvest` | 9 | reader | `networkFastClockHarvest(run, ep)` (gate `ctx.knobs.fc_on && ep.emitted`) | |
| 61 | 2577 | `networkIonoDiag` | 14 | reader | `networkIonoDiag(run, ep)` | |
| 62 | 2581 | `networkDdFrac` | 11 | reader | `networkDdFrac(run, ep)` | |
| 63 | 2585 | `networkShDevVtec` | 13 | reader | `networkShDevVtec(run, ep)` | |
| 64 | 2589 | `networkShHealth` | 16 | reader | `networkShHealth(run, ep)` | |
| 65 | 2600 | `networkIonoDwRelease` | 11 | reader | `networkIonoDwRelease(run, ep)` | |
| 66 | 2604 | `networkLeveling` | 18 | reader | `networkLeveling(run, ep)` | |
| 67 | 2615 | `networkPostFitRaim` | 18 | writer | `networkPostFitRaim(run, ep, ep.solve.handoff)` (gate unchanged) | solve.handoff |
| 68 | 2635 | `networkGrossLosScreen` | 12 | reader, kept `phase` | `networkGrossLosScreen(run, ep, ctx.knobs.gl_emit_exclude ? GrossLosPhase::Act : GrossLosPhase::Full)` | |
| 69 | 2641 | `networkClockQc` | 18 | writer | `networkClockQc(run, ep, ep.solve.handoff)` | solve.handoff |
| 70 | 2648 | `networkPhaseVisibleMonitor` | 6 | reader (observer), adapter | `networkPhaseVisibleMonitor(run, ep)` | |
| 71 | 2662 | `networkSignalRosterAudit` | 12 | reader (fallback names) | `networkSignalRosterAudit(run, ep)` | |
| 72 | 2671 | `networkLedgerEmitFold` | 3 | reader (observer), adapter | `networkLedgerEmitFold(run, ep)` | |
| 73 | 2679 | `networkSecondPhaseEpochTail` | 3 | reader (observer) | `networkSecondPhaseEpochTail(run, ep)` | |
| 74 | 2684 | `networkChannelAllEpoch` | 5 | reader (observer) | `networkChannelAllEpoch(run, ep)` | |
| 75 | 2689 | `networkMultiGnssEpoch` | 5 | reader (observer) | `networkMultiGnssEpoch(run, ep)` | |
| 76 | 2693 | `networkParamInventoryEpochTail` | 5 | reader (observer) | `networkParamInventoryEpochTail(run, ep)` (`emitted` folds to `ep.emitted`) | |
| 77 | 2698 | `networkContributionsEpochTail` | 6 | reader (observer, fallback names) | `networkContributionsEpochTail(run, ep)` (likewise) | |
| 78-79 | 2713, 2717 | `ctx.carry.bb.beginEpoch`, `ctx.carry.cg.beginEpoch` | 2 | unchanged (EP3 wraps them) | same text | |
| 80 | 2719 | `networkPostfitSigma0` | 11 | reader (observer) | `networkPostfitSigma0(run, ep)` (gate unchanged) | |
| 81 | 2722 | `networkShSpectrum` | 6 | reader (observer) | `networkShSpectrum(run, ep)` | |
| 82 | 2729 | `networkStationBlock` | 10 | reader (observer) | `networkStationBlock(run, ep)` | |
| 83 | 2738 | `networkPrefitCensus` | 10 | reader (observer), adapter | `networkPrefitCensus(run, ep)` | |
| 84 | 2747 | `networkRedundancyCensus` | 11 | reader (observer, fallback names) | `networkRedundancyCensus(run, ep)` | |
| 85 | 2756 | `networkBlockBaardaEmit` | 6 | reader (observer) | `networkBlockBaardaEmit(run, ep)` | |
| 86 | 2765 | `networkCongruenceEmit` | 6 | reader (observer) | `networkCongruenceEmit(run, ep)` | |
| 87 | 2775 | `networkMdbFlush` | 5 | reader (observer) | `networkMdbFlush(run, ep)` | |
| 88 | 2785 | `networkDatumGraphCensus` | 13 | reader (observer) | `networkDatumGraphCensus(run, ep)` | |
| 89 | 2789 | `est.attribArmed` | 0 | unchanged | same text | |
| 90 | 2790 | `networkRhsAttribution` | 11 | reader (observer) | `networkRhsAttribution(run, ep)` | |
| 91 | 2797 | `networkRhsAttribTrip` | 11 | reader (observer), adapter | `networkRhsAttribTrip(run, ep)` | |
| 92 | 2801 | `networkStashStatus` | 28 | reader | `networkStashStatus(run, ep)` | |
| 93 | 2809 | `networkPbHoldCloneHarvest` | 4 | reader, adapter | `networkPbHoldCloneHarvest(run, ep)` | |
| 94 | 2813 | `networkClockLevelRebase` | 8 | reader | `networkClockLevelRebase(run, ep)` | |
| 95 | 2818 | `networkClockLevelMonitor` | 8 | reader | `networkClockLevelMonitor(run, ep)` | |
| 96 | 2825 | `networkAmbStateTrace` | 3 | reader (observer; reads no epoch member) | `networkAmbStateTrace(run, ep)` | |
| 97-98 | 2852, 2871 | `est.sectionTimers`, `est.numShared` | 0 | unchanged | same text | |
| 99 | 2892 | `networkWriteStateSnapshot` | 12 | reader | `networkWriteStateSnapshot(run, ep)` | |

Counts: 62 distinct seams at 64 call sites convert (5 members, 45 direct free seams, 11 adapters, 1 run-only); 12 of them write the epoch (9 before the solve, 3 after). The 64 converted call sites carry 659 positional arguments today and 151 after; the eleven spine calls of the card's table go from 238 arguments at `6e39dc4` (225 at `a453448`) to 33. 35 of the 659 arguments bind under a parameter name that differs from the argument's leaf name (`spec_tools/renames.py`, list in `base/renames.txt`; the dangerous ones are in 5.4). `networkSignalRosterAudit` is a reader but not an observer: it advances the arc ledger's admitted machine, which armed SID readers feed back into the solve.

### 3.4 The solve, its replay and its exits

```cpp
/**
 * @brief The datum build, the solve and the within-epoch RAIM adaptation of this epoch.
 * @param run          The run: the estimator is folded and solved (and rebuilt by each T2
 *                     replay); ctx carry and counters, the traces, the setup's writer and
 *                     consumer are written as before.
 * @param ep           The epoch, read: se, clock, pass_a, columns, assembly (asm_res, total_pins).
 * @param ws           [in/out] the capture channel the replay re-folds, the clock maps, and the
 *                     within-epoch culprit sets the T2 block adapts and clears (card WCF).
 * @param est_index_of [in/out] ep.assembly.est_index_of, rebuilt by each replay.
 * @param dd_build     [in/out] ep.dd_build, rewritten by each replay's DD rebuild.
 * @param funnel       [in/out] ep.funnel, which the DD rebuild declares writable (it only
 *                     reads it; step EP1c makes both const and removes this parameter).
 * @return The solve's result; action is SkipEpoch at the five exits (bookkeeping done here).
 */
NetworkSolveGlueResult networkSolveGlue(NetworkRun& run, const NetworkEpoch& ep,
                                        EpochWorkspace& ws, std::vector<int>& est_index_of,
                                        EpochDdBuild& dd_build, NetworkSlipFunnel& funnel);
```

Body edits beyond the preamble, each declared: the replay's two callback lines (`NetworkSolveGlue.cpp` 2131 and 2141 at `a453448`) become direct calls, with WCF's writer-end counter kept between them:

```cpp
        // Rebuild the ensemble phase-bias gauges on the fresh arrowhead (fix A) ...  [comment unchanged]
        networkBuildEnsembleGauges(run, ep);
        // Rebuild the DD-prior blocks on the fresh arrowhead ...                     [comment unchanged]
        if (ctx.knobs.carry_invariant && ws.we_culprits.namesAmbiguities())
            ++ctx.counters.we_culprit_withhold.resolves;
        networkBuildCarryDdPriors(run, ep, dd_build, funnel);
```

(`resolve_dropping` at 1819 captures by reference, so `run`, `ep`, `dd_build`, `funnel` are the solve's parameters; inside the DD build `ep.ws.we_culprits` is the same object as the solve's `ws`, and `ep.assembly.est_index_of` the same object as the solve's `est_index_of`.) Three epilogue lines move to `result.handoff` (2.4). The solve's own alias preamble (its "byte-copies of the run-TU alias block", 126-200) stays below the generated preamble, unchanged. Nothing else in the 2 686-line body changes: the result type, the five `result.action = EpochAction::SkipEpoch` exits (762 datum failure, 1327 observability-gate withhold, 1461 solve failure, 2506 T2 restore failure, 2736 A4 sigma0 withhold) and the internal `networkRhsAttribTrip(ctx, config_, log, traces, se, setup.stations, est, plan.shared_datum, asm_res, est_index_of)` call (2711) keep their text; that call still reaches the explicit form, with the solve's local `plan` and no `x_s`. The banner of `NetworkSolveGlue.hpp` (lines 26-28, "std::function so the within-epoch re-solve re-invokes them ...") is rewritten to say the replay calls the two builders directly.

### 3.5 Two worked examples

**Before the solve, with the binding the old signature hid** (`networkPassASlipScan_`, `Application.hpp` 813):

```cpp
    void networkPassASlipScan_(NetworkRun& run, const NetworkEpoch& ep,
                               EpochPassA& pass_a, NetworkSlipFunnel& funnel);

void Application::networkPassASlipScan_(NetworkRun& run, const NetworkEpoch& ep,
                                         EpochPassA& pass_a, NetworkSlipFunnel& funnel) {
    // ---- From the run and the epoch (card NOM3 step EP1; ...) ----
    const SyncedEpoch& se = ep.se;
    NetworkRunContext& ctx = run.ctx;
    NetworkSetupResult& setup = run.setup;
    NetworkRunTraces& traces = run.traces;
    double dt = ep.clock.dt;
    double pl_now_s = ep.clock.pl_now_s;
    bool carry_warm = ep.clock.carry_warm;
    std::vector<std::vector<PppObservation>>& modeled = pass_a.modeled;                // written
    std::vector<bool>& present = pass_a.present;                                        // written
    const std::set<std::string>& shared_phase_keys = ep.pass_a.shared_phase_keys;
    std::set<std::pair<int, std::string>>& carry_slipped_ambs = funnel.carry_slipped_raw;  // written: the raw subset; the driver unions it into the funnel
    SlipQcChannel& qc = run.engines.slip_qc;
    // ---- End of the bindings ----
    ...frozen body...
```

The parameter `carry_slipped_ambs` binds the raw subset, as the frozen call bound it (1545-1547); a by-name binding would send PASS A's drains to the funnel and starve the raw-subset readers (PASS B's d5_keep, the DD build's retap-only, the WL-3 skip). This is negative control N2.

**After the solve** (`networkClockQc`, `NetworkClockQc.hpp` 98):

```cpp
void networkClockQc(NetworkRun& run, const NetworkEpoch& ep, NetworkPostFitHandoff& handoff);
    // preamble (frozen declarators):
    NetworkRunContext& ctx = run.ctx;
    const GnssConfig& config_ = run.config;
    const std::shared_ptr<spdlog::logger>& log = run.log;
    NetworkRunTraces& traces = run.traces;
    const EpochWorkspace& ws = ep.ws;
    const SyncedEpoch& se = ep.se;
    const NetworkEstimator& est = run.engines.est;
    const Eigen::VectorXd& x_s = ep.solve.x_s;
    const std::vector<std::string>& registered_clock_sats = ep.columns.registered_clock_sats;
    bool datum_changed = ep.solve.datum_changed;
    const std::set<char>& changed_consts = ep.solve.changed_consts;
    bool clk_model_warm = ep.solve.clk_model_warm;
    bool emit_rate_warm = ep.solve.emit_rate_warm;
    double pl_now_s = ep.clock.pl_now_s;
    double dt = ep.clock.dt;
    std::set<std::string>& clock_refloat_sats = handoff.clock_refloat_sats;   // written
    std::set<std::string>& retro_flag_sats = handoff.retro_flag_sats;         // written
    bool retro_feed = ep.solve.retro_feed;
```

Eighteen positional arguments, three adjacent bools and two adjacent doubles among them, become eighteen named bindings that cannot be swapped silently; the signature says that clock QC writes the hand-off and cannot rewrite the solution it judges.

### 3.6 Adapters and explicit exceptions

An adapter keeps the explicit declaration and definition byte-identical and adds the epoch form beside it (declaration after the explicit one; definition in a generated region at the end of the `.cpp`), whose body is one call forwarding the same bindings as Appendix A. Eleven seams, each with its other caller (found by `spec_tools2/callers.py` over the extracted base tree, comments and strings blanked):

| seam | other callers |
|---|---|
| `networkSignalIntegrity` | `tests/test_signal_integrity.cpp` |
| `networkMeasurementAdmission` | `tests/test_measurement_admission.cpp` |
| `networkHandoverStransformBeforeRows` | `tests/test_handover_stransform.cpp` (483) |
| `networkCarryDecide` | `tests/test_carry_disposition.cpp`, `tests/test_code_datum_family.cpp` (6 calls, four estimators) |
| `networkSolPrep` | `tests/test_network_solprep_disc.cpp`, `tests/test_code_datum_family.cpp` |
| `networkPbHoldCloneUpdate` | `tests/test_pb_hold_block.cpp` (445), `tests/test_pb_hold_smoothed.cpp` (753) |
| `networkPbHoldCloneHarvest` | the same two files |
| `networkRhsAttribTrip` | `src/core/filter/NetworkSolveGlue.cpp` 2711 (production: the solve's local plan, no `x_s`) |
| `networkPhaseVisibleMonitor` | `tests/test_phase_visible_monitor.cpp` (272, 276) |
| `networkPrefitCensus` | `tests/test_prefit_census.cpp` (3 calls) |
| `networkLedgerEmitFold` | `tests/test_ledger_emit.cpp` |

No test changes in EP1. Step EP2 retires the ten test-only adapters by moving those tests onto a `tests/support/NetworkTestRun.hpp` fixture (a configuration, a context, traces, a setup with N stations, a run with default engines, and a `NetworkEpoch` over a test epoch), so each seam has one form; `networkRhsAttribTrip`'s explicit form lives until step SG rewrites the solve's exit. Explicit by exception: the input stage (2 seams) and the calls inside the EP2 blocks (16 seams and helpers), each listed in the rows with its reason.

---

## 4. The driver after EP1

### 4.1 `runNetwork` (`ApplicationNetwork.cpp`, about 1 180 lines after EP1, from 2 939)

Setup keeps its order. The edits, all declared:

| frozen (`a453448`) | after EP1 | class |
|---|---|---|
| 257 `ClosedLoopTally cl_tally;` and its comment | removed (the run owns `closed_loop`; the comment moves to the member) | OWNER_MOVE |
| after 285 `NetworkRunTraces traces(log_dir_);` | `const NetworkEngineSpec engine_spec = networkEngineSpec(config_, nc, ctx);` `NetworkRun run(ctx, config_, nc, log, traces, setup, log_dir_, engine_spec);` | new lines |
| 292 `SlipQcChannel slip_qc;` | `SlipQcChannel& slip_qc = run.engines.slip_qc;` (297-362 param fill, 403 `utils`, 519 and 577 corroborators unchanged) | OWNER_MOVE |
| 416 `std::unique_ptr<PppUtils> fc_ppp_utils;` | `std::unique_ptr<PppUtils>& fc_ppp_utils = run.engines.fc_ppp_utils;` (417 unchanged) | OWNER_MOVE |
| 431, 434, 444-452 `pl_q_amb`, `pl_var_floor`, `carry_vce`, `vce_families`, `NetworkCarryVce carry_vce_est(...)` | removed: their only reader was the constructor; the statements are in `networkEngineSpec` | SPEC_CUT |
| 473-484 `NetworkPriorResidual prires_est(...)` | `NetworkPriorResidual& prires_est = run.engines.prires;` (its arguments are spec fields) | SPEC_CUT, OWNER_MOVE |
| 665-713 the `carry_cfg` statements (the interleaved `carry_invariant` 657 and `carry_innov_gate` 670 stay; `lane_cfg`, `lane_on`, `lane_joint` 697-699 move, their only reader being the cut) | `const NetworkInvariantCarryDriver::Config& carry_cfg = engine_spec.carry_cfg;` (read at 1016) | SPEC_CUT |
| 714 `NetworkInvariantCarryDriver carry_driver(carry_cfg);` | `NetworkInvariantCarryDriver& carry_driver = run.engines.carry_driver;` | OWNER_MOVE |
| 738-755 the `isUncarriedThirdBand` closure | removed (`networkIsUncarriedThirdBand`, 2.5) | CALLABLE |
| 756-761 `mw_wl_cfg` statements | `const NetworkMwWlLayer::Config& mw_wl_cfg = engine_spec.mw_wl_cfg;` (the warning 762-765 stays and reads it) | SPEC_CUT |
| 773-782 `ewl_cfg` statements | `const NetworkMwWlLayer::Config& ewl_cfg = engine_spec.ewl_cfg;` (the EWL fill 790-816 reads it) | SPEC_CUT |
| 787 `NetworkLaneRun lane_run(mw_wl_cfg, ewl_cfg);` | `NetworkLaneRun& lane_run = run.lanes;` | OWNER_MOVE |
| 846-858 `pb_filter_cfg`, `clk_rebase_cfg` | removed (spec fields) | SPEC_CUT |
| 859-860 `NetworkEmitEngines emit_engines{...}` | `NetworkEmitEngines& emit_engines = run.emit;` | OWNER_MOVE |
| 896-900 `idw`, `NetworkIonoDeweightGate iono_deweight(...)` | `NetworkIonoDeweightGate& iono_deweight = run.engines.iono_deweight;` (908 `enableReleaseTracking` and the two warnings unchanged) | SPEC_CUT, OWNER_MOVE |
| 943 `NetworkRunTiming timing;` | `NetworkRunTiming& timing = run.timing;` (944 `loop_begin` unchanged) | OWNER_MOVE |
| 955 `NetworkEstimator est;` | `NetworkEstimator& est = run.engines.est;` (973-1037 setters unchanged) | OWNER_MOVE |
| 1060-1080 the three timer vectors and the `subBegin` / `subEnd` closures | removed (`NetworkRunTiming` fields; `networkSubBegin` / `networkSubEnd`) | FIELD_MOVE, BRACKET |
| 1229-2905 `enum class LoopSignal` and the lambda | removed (`Application::LoopSignal`; `networkProcessEpoch_`) | TYPE_MOVE, relocation |
| 2921, 2927 `processSyncedEpoch(se_live)` / `(se)` | `networkProcessEpoch_(run, se_live)` / `(run, se)` | call rewrite |
| 2932-2936 the `engines` view and the teardown call | `return networkTeardown(ctx, config_, log, traces, setup, NetworkRunEndInputs{ run.engines, run.emit, run.lanes, run.timing, run.closed_loop });` | binding |

The spec cut moves no log line (the one in range, 762-765, stays), no trace call, no setup read: every statement reads `config_`, `nc` or `ctx.knobs` (V12 proves it). The cut statements' text is unchanged because `networkEngineSpec` takes `(config_, nc, ctx)` under those names and repeats, character for character, the alias declarations they read. Statements that are constructor arguments inline in a frozen constructor call (`static_cast<double>(config_.networkCarryVceWindowEpochs())`, the prires arguments) become field initializers with the same expression text (`spec.vce_window_epochs = static_cast<double>(config_.networkCarryVceWindowEpochs());`), and the engine constructor passes the field at the same position.

**Amended at the EP1 review round (2026-10-14).** The table above did not list the aliases whose
only readers were inside the lambda; moving the lambda out leaves them unread (33 at main
`926dafa`: `brdc_rts`, `brdc_nav_staged`, `brdc_nav_cursor`, the carry and knob aliases of the
loop head, the counters, the phase-datum and invariant-carry switches, the carry's chord maps
and the loop counters), the dead-alias defect INC-12 and EP0 removed before. They go, as the
twelfth declared class UNREAD_ALIAS (5.6), with the comment blocks that only introduced them
(`scripts/nom_ep1/docs.py`); what those blocks said that is still true moves to the declaration
it describes. A generation that would leave any declaration of `runNetwork`'s outermost scope
unread is refused by name, and V1d fails one (control R53).

### 4.2 `Application::networkProcessEpoch_` (`src/core/app/ApplicationNetworkEpoch.cpp`, new, about 1 400 lines)

The lambda's body moved four columns left, with the transforms of section 5.3. The EP2 blocks are shown collapsed; their text is the frozen text.

```cpp
Application::LoopSignal Application::networkProcessEpoch_(NetworkRun& run, const SyncedEpoch& se_raw) {
    // ---- The run under the names the inline blocks still read (card NOM3 step EP1; generated,
    //      each line the frozen runNetwork declaration with the run's objects re-pointed).
    //      Step EP2 removes these lines with the blocks that read them. ----
    const std::shared_ptr<spdlog::logger>& log = run.log;
    const auto& nc = config_.getNetworkConfig();
    NetworkSetupResult& setup = run.setup;
    const bool brdc_rts = setup.brdc_rts;
    auto& brdc_nav_staged = setup.brdc_nav_staged;
    auto& brdc_nav_cursor = setup.brdc_nav_cursor;
    auto& stations = setup.stations;
    const int n_threads = setup.n_threads;
    const bool carry_par = setup.carry_par;
    NetworkRunContext& ctx = run.ctx;
    NetworkRunTraces& traces = run.traces;
    SlipQcChannel& slip_qc = run.engines.slip_qc;
    NetworkInvariantCarryDriver& carry_driver = run.engines.carry_driver;
    NetworkEstimator& est = run.engines.est;
    NetworkRunTiming& timing = run.timing;
    std::vector<SubBlockTimer>& sec_sub_ep0 = timing.sec_sub_ep0;
    auto& last_pivot = ctx.carry.last_pivot;          // ... and the other 19 ctx.carry / ctx.counters
    auto& anchor_incumbent = ctx.carry.anchor_incumbent;   //     aliases the blocks read, frozen text
    /* this_epoch_anchor, start_anchor_s, local_carry, ura_sigma0_ewma, grade_metric_s, prev_tow,
       prev_week, n_emitted, n_gated_epochs, n_gated_key_epochs, max_dur_anchor_s, max_dur_anchored,
       carry_sat_vertex, carry_group_id, carry_prev_tow, carry_innov_pending, n_loop_epochs,
       decim_counter, sh_converged_latch */
    const bool pl_on = ctx.knobs.pl_on;               // ... and the 10 other knob aliases, frozen text
    /* pl_warmup, pl_max_gap_s, ura_on, raim_on, start_offset_s, phase_datum_ensemble,
       phase_datum_eq34, carry_invariant, carry_innov_gate, phase_datum_prioritized */
    // ---- End of the run's names ----

    // ---- Admission: start offset, event injector, canonical-pair screen ----
    if (start_offset_s > 0.0) {                       // [frozen 1242-1246]
        const double now_s = TimeUtils::continuousGpsSeconds(se_raw.time);
        if (start_anchor_s < 0.0) start_anchor_s = now_s;
        if (now_s - start_anchor_s < start_offset_s) return LoopSignal::Next;
    }
    LogManager::setCurrentEpochTime(se_raw.time);
    SyncedEpoch se_inject_scratch;
    const SyncedEpoch& se_admit = networkInjectEpoch(ctx, log, traces, se_raw, se_inject_scratch);
    SyncedEpoch se_cpa_scratch;
    const SyncedEpoch& se = networkCanonicalPairAdmission(ctx, log, traces, stations,
                                                          se_admit, se_cpa_scratch);
    // The epoch: everything the stages below hand each other (app/NetworkEpoch.hpp).
    NetworkEpoch ep(se);

    networkSubBegin(run, SB_ROSTER);
    networkBuildSignalRoster(run, ep);
    networkSubEnd(run, SB_ROSTER);

    est.attribArm(ctx.knobs.rec_on || (...));         // [frozen 1300-1303]
    ctx.carry.rhs_trip_pending.clear();
    if (ctx.knobs.dg_on) ctx.carry.dg_spec.clear();
    if (brdc_rts) { /* broadcast feed, frozen 1324-1332; EP2: filter/NetworkBrdcWire */ }
    if (setup.brdc_apriori) setup.brdc_apriori->feedStore(...);
    if (ctx.knobs.max_duration_s > 0.0) { /* frozen 1342-1350 */ return LoopSignal::Stop; }
    if ((decim_counter++ % ctx.knobs.epoch_decimation_n) != 0) {
        if (ctx.knobs.fc_on) networkFastClockTick(run, ep, false);
        return LoopSignal::Next;
    }
    ++n_loop_epochs;
    networkParamInventoryNoteSatellites(run, ep);

    // ---- Epoch top: the D0 baseline, the CSV rotation, the heartbeat, the fast-clock shadow ----
    NetworkNormalEquations::SectionTimers sec_t0;
    std::chrono::steady_clock::time_point sec_epoch_t0;
    double& sec_asm_ms_epoch = ep.assembly.sec_asm_ms_epoch;    // was: double sec_asm_ms_epoch = 0.0;
    if (ctx.knobs.sec_timers_on) { /* frozen 1381-1385 */ }
    traces.rotateServiceCsvs(nc, log);
    traces.writeHealthRow(se.time, nc, ctx.counters, timing.loop_begin,
                          ura_on, raim_on, pl_on, ura_sigma0_ewma, last_pivot, grade_metric_s);
    if (ctx.knobs.fc_shadow) {
        networkSubBegin(run, SB_FASTCLOCK);
        networkFastClockTick(run, ep, true);
        networkSubEnd(run, SB_FASTCLOCK);
    }
    auto emitEvent = [&](/* frozen 1410-1416 */) { traces.emitEvent(se.time, ...); };

    // ---- The clock ----
    const double dt = (prev_tow < 0.0) ? 0.0 : (...);            // [frozen 1418-1419]
    const double pl_now_s = TimeUtils::continuousGpsSeconds(se.time);
    const bool carry_warm = pl_on && (...);                       // [frozen 1435-1437]
    ep.clock = EpochClock{ dt, pl_now_s, carry_warm };

    // ---- The slip funnel, opened with last epoch's deferred flags ----
    auto& carry_slipped_ambs = ep.funnel.carry_slipped_ambs;     // was: std::set<...> carry_slipped_ambs;
    /* D5 drain, retap-only swap: frozen 1452-1469 */
    auto& carry_slipped_raw = ep.funnel.carry_slipped_raw;       // was: std::set<...> carry_slipped_raw;
    /* deweight-release drain, RAIM drain: frozen 1483-1525; EP2: filter/NetworkSlipFunnel */

    // ---- PASS A ----
    ep.pass_a.modeled = std::vector<std::vector<PppObservation>>(stations.size());
    auto& modeled = ep.pass_a.modeled;
    ep.pass_a.present = std::vector<bool>(stations.size(), false);
    auto& present = ep.pass_a.present;
    auto& shared_phase_keys = ep.pass_a.shared_phase_keys;
    if (networkPassAHead_(run, ep, ep.pass_a) == EpochAction::SkipEpoch)
        return LoopSignal::Next;
    networkSubBegin(run, SB_SLIP);
    ++slip_qc.epoch_idx;
    networkPassASlipScan_(run, ep, ep.pass_a, ep.funnel);
    carry_slipped_ambs.insert(carry_slipped_raw.begin(), carry_slipped_raw.end());
    networkSubEnd(run, SB_SLIP);

    // ---- Pre-assembly ----
    networkSubBegin(run, SB_PREASM);
    networkAssemblyPrefilter_(run, ep, ep.pass_a);
    networkSignalIntegrity(run, ep, ep.pass_a, ep.funnel);
    networkMeasurementAdmission(run, ep, ep.pass_a, ep.funnel);
    /* the phase-datum election, frozen 1620-2125 (506 lines); EP2: filter/NetworkPhaseAnchorEpoch */
    /* the eq34 RPB allocation, frozen 2137-2154 */
    networkLevelAuthorityCensus(run, ep);
    if (networkAssemblyRegisterColumns_(run, ep, ep.columns) == EpochAction::SkipEpoch)
        return LoopSignal::Next;                                  // leaves SB_PREASM open, as today
    networkSdEwlAr(run, ep);
    networkRxCbDemeanPrepass(run, ep);
    if (ctx.carry.oracle_pin)
        networkOraclePinEpoch(run, ep);
    { /* the bounded drop, frozen 2226-2238; EP2 */ }
    networkClockLevelRestoreRaise(run);
    networkHandoverStransformBeforeRows(run, ep);
    networkCarryDecide(run, ep);
    networkCouplingExactPrepare(run, ep);
    networkSubEnd(run, SB_PREASM);

    // ---- PASS B ----
    ep.assembly.asm_res = std::vector<StationAssembly>(stations.size());
    ep.assembly.est_index_of = std::vector<int>(stations.size(), -1);
    if (networkPassB_(run, ep, ep.pass_a, ep.assembly) == EpochAction::SkipEpoch)
        return LoopSignal::Next;

    // ---- The carried priors ----
    networkSubBegin(run, SB_GAUGEBUILD);
    networkBuildEnsembleGauges(run, ep);
    /* the gap-resume census, frozen 2326-2433; EP2: slip/NetworkGapResumeCensus */
    networkSubEnd(run, SB_GAUGEBUILD);
    networkSubBegin(run, SB_DDBUILD);
    if (carry_invariant) { /* frozen 2458-2467: the TOW-only dt and carry_driver.beginEpoch */ }
    { CarryEigenScope es(carry_par, n_threads); networkBuildCarryDdPriors(run, ep, ep.dd_build, ep.funnel); }
    networkSubEnd(run, SB_DDBUILD);

    // ---- The solve ----
    networkSubBegin(run, SB_SOLVEGLUE);
    ep.solve = networkSolveGlue(run, ep, ep.ws, ep.assembly.est_index_of, ep.dd_build, ep.funnel);
    if (ep.solve.action == EpochAction::SkipEpoch) return LoopSignal::Next;   // leaves SB_SOLVEGLUE open, as today
    networkSubEnd(run, SB_SOLVEGLUE);

    // ---- After the solve.  Seams read a const epoch; their writes are ep.solve.handoff (RAIM,
    //      clock QC) and ep.prep.sol (the emitter).  The driver stores four returned values:
    //      ep.ar_fixes, ep.fixed_cycles, ep.prep, ep.emitted (scripts/nom_epoch_lint.py). ----
    networkSubBegin(run, SB_INSTALL);
    if (carry_invariant)
        networkInstallCarry(run, ep);
    networkSubEnd(run, SB_INSTALL);
    networkSubBegin(run, SB_NETAR);
    ep.ar_fixes = networkProducerAr(run, ep);
    networkSubEnd(run, SB_NETAR);
    networkSubBegin(run, SB_SOLPREP);
    if (ctx.knobs.gl_emit_exclude)
        networkGrossLosScreen(run, ep, GrossLosPhase::Decide);
    ep.fixed_cycles = networkFixedCycles(run, ep);
    ep.prep = networkSolPrep(run, ep);
    networkSubEnd(run, SB_SOLPREP);
    const std::size_t fc_n_emitted_before = ctx.counters.n_emitted;
    networkPbHoldCloneUpdate(run, ep);
    networkSubBegin(run, SB_EMITSTORE);
    networkEmitStore(run, ep, ep.prep.sol);
    networkSubEnd(run, SB_EMITSTORE);
    ep.emitted = ctx.counters.n_emitted != fc_n_emitted_before;
    if (ctx.knobs.fc_on && ep.emitted) {
        networkSubBegin(run, SB_FASTCLOCK);
        networkFastClockHarvest(run, ep);
        networkSubEnd(run, SB_FASTCLOCK);
    }
    networkSubBegin(run, SB_IONODIAG);   networkIonoDiag(run, ep);    networkSubEnd(run, SB_IONODIAG);
    networkSubBegin(run, SB_DDFRAC);     networkDdFrac(run, ep);      networkSubEnd(run, SB_DDFRAC);
    networkSubBegin(run, SB_SHDEV_VTEC); networkShDevVtec(run, ep);   networkSubEnd(run, SB_SHDEV_VTEC);
    networkSubBegin(run, SB_T1A);
    networkShHealth(run, ep);
    networkIonoDwRelease(run, ep);
    networkSubEnd(run, SB_T1A);
    networkSubBegin(run, SB_LEVELING);   networkLeveling(run, ep);    networkSubEnd(run, SB_LEVELING);
    networkSubBegin(run, SB_RAIM);
    if (raim_on && (carry_warm || ctx.knobs.raim_ci_on))
        networkPostFitRaim(run, ep, ep.solve.handoff);
    networkGrossLosScreen(run, ep, ctx.knobs.gl_emit_exclude ? GrossLosPhase::Act
                                                             : GrossLosPhase::Full);
    networkSubEnd(run, SB_RAIM);
    networkSubBegin(run, SB_CLOCKUPD);
    networkClockQc(run, ep, ep.solve.handoff);
    networkSubEnd(run, SB_CLOCKUPD);
    networkPhaseVisibleMonitor(run, ep);

    // ---- The roster and census tail ----
    networkSubBegin(run, SB_ROSTER);
    networkSignalRosterAudit(run, ep);
    networkLedgerEmitFold(run, ep);
    networkSecondPhaseEpochTail(run, ep);
    networkChannelAllEpoch(run, ep);
    networkMultiGnssEpoch(run, ep);
    networkParamInventoryEpochTail(run, ep);
    networkContributionsEpochTail(run, ep);
    networkSubEnd(run, SB_ROSTER);

    // ---- The measure-only instruments (EP3: kNetworkEpochObservers) ----
    networkSubBegin(run, SB_FRAMEDIAG);
    if (ctx.knobs.bb_on) ctx.carry.bb.beginEpoch(pl_now_s, se.time.tow);
    if (ctx.knobs.cg_on) ctx.carry.cg.beginEpoch(pl_now_s, se.time.tow);
    if (ctx.knobs.trace_postfit_on)    networkPostfitSigma0(run, ep);
    if (ctx.knobs.trace_sh_spectrum)   networkShSpectrum(run, ep);
    if (ctx.knobs.trace_station_block) networkStationBlock(run, ep);
    if (ctx.knobs.pc_walk_on)          networkPrefitCensus(run, ep);
    if (ctx.knobs.redun_census_on)     networkRedundancyCensus(run, ep);
    if (ctx.knobs.bb_on)               networkBlockBaardaEmit(run, ep);
    if (ctx.knobs.cg_on)               networkCongruenceEmit(run, ep);
    if (ctx.knobs.mdb_on)              networkMdbFlush(run, ep);
    if (ctx.knobs.dg_on)               networkDatumGraphCensus(run, ep);
    if (est.attribArmed())             networkRhsAttribution(run, ep);
    networkRhsAttribTrip(run, ep);
    networkSubEnd(run, SB_FRAMEDIAG);

    // ---- The stash: the carry the next epoch folds ----
    networkSubBegin(run, SB_STASH_STATUS);
    networkStashStatus(run, ep);
    networkPbHoldCloneHarvest(run, ep);
    networkClockLevelRebase(run, ep);
    networkClockLevelMonitor(run, ep);
    networkAmbStateTrace(run, ep);
    /* the LGM and AIL trace drains, frozen 2827-2839 */
    networkSubEnd(run, SB_STASH_STATUS);

    if (ctx.knobs.sec_timers_on) { /* the D0 / D0b readout, frozen 2851-2887 (reads n_present below) */ }
    networkWriteStateSnapshot(run, ep);
    return LoopSignal::Next;
}
```

(The readout's `n_present` binds `const int& n_present = ep.columns.n_present;`, placed where the frozen `int n_clock = 0, n_pb = 0, n_cb = 0, n_present = 0;` stood; the other three are dropped, their members' default initializers being the frozen initializers.) The forty `subBegin` / `subEnd` lines and the five same-line bracket groups shown condensed above keep one statement per line in the generated file; only the text `run, ` is added to each.

---

## 5. Generator and relocation verifier (gate G-R)

### 5.1 Files

| file | role | size (estimate) |
|---|---|---|
| `scripts/nom_cpp.py` | token lexer, statement splitter, scope walker, declaration parser: the inventory's `lex.py`, `stmts.py`, `sigs.py`, `blocklocals.py` (proven on this file), with a self-test that reproduces the inventory counts on `<B>` (62 captured names, 99 calls, 753 `network*` arguments; `base/calltable_ep0.md`) | 450 |
| `scripts/nom_ep1/model.py` | row types, the fold grammar, the epoch-member and run-member realization tables (generator input only) | 250 |
| `scripts/nom_ep1/declared.py` | the declared substitutions (5.6), the spec-cut ranges, the setup edits of 4.1, the doc and include hunks, each with reason and frozen file:line | 300 |
| `scripts/nom_ep1/rows_{app,solve,emit,diag}.py` | one row per converted seam, one file per worker | 4 x 80 |
| `scripts/nom_ep1/manifest_*.json` | written by the generator (frozen ranges resolved, preamble lines emitted); read by the verifier for pairing only | generated |
| `scripts/nom_ep1_cut.py` | the generator (`--survey`, `--rows`, `--shared-only`, `--presolve-writer {outputs,epoch}`) | 1 000 |
| `scripts/nom_ep1_verify.py` | the verifier (V1-V13, `--rows`, `--controls`) reusing `rt_seam_verify.strip_comments` and `nom_td_verify`'s coverage and print-order machinery | 900 |
| `scripts/nom_epoch_lint.py` | the permanent check (V4 driver writes, V10 stage order, V11 writer banners), run on every later tree | 200 |

### 5.2 The canonical table

```python
Seam("networkPostFitRaim",
     hpp="include/filter/NetworkRaim.hpp", cpp="src/core/filter/NetworkRaim.cpp",
     mech=DIRECT,              # DIRECT | MEMBER | ADAPTER | RUN | EXPLICIT(reason=...)
     outs=("handoff",),        # output groups or single members, in parameter order
     kept=(),                  # frozen parameter names that stay explicit (differ between call sites)
     observer=False,           # measure-only: its epoch form must be NetworkEpochObserver
     worker="W3")
```

No row carries bindings: the generator derives each from the frozen call site's argument through the realization tables (`run.*` for the run's names, `ep.<group>.<member>` for the lambda's locals, the output parameter for a written member), refuses an argument outside the fold grammar unless the parameter is `kept`, and refuses a parameter whose derived binding differs between two call sites. Rows name call sites by callee and occurrence, never by line, so they survive the line shifts of a moved base.

### 5.3 The transforms the generator applies

| id | what | example |
|---|---|---|
| T-A | the lambda body becomes `networkProcessEpoch_`'s body, shifted four columns, comments carried | |
| T-B1 | CTOR: `NetworkEpoch ep(se);` inserted right after the canonical-pair statement | 1274 |
| T-B2 | WS: `EpochWorkspace ws;` (1235, before the start-offset gate) dropped; `ep.ws` is built with `ep` | |
| T-B3 | ALIAS: a default-constructed local becomes a reference to its member at the same line (only if the remaining text reads it) | `auto& carry_slipped_ambs = ep.funnel.carry_slipped_ambs;` |
| T-B4 | SIZED: a sized local becomes a member assignment of the identical constructor expression, plus an alias if read | `ep.pass_a.present = std::vector<bool>(stations.size(), false);` |
| T-B5 | SCALAR: a scalar local whose initializer equals the member's default initializer is dropped; an alias if read | `int n_dropped_sats = 0;` |
| T-B6 | CLOCK: `ep.clock = EpochClock{ dt, pl_now_s, carry_warm };` inserted after the three const locals | 1437 |
| T-B7 | RESULT: `T x = f(args);` becomes `ep.<m> = f(...);` plus a reference alias `T& x = ep.<m>;` (and the frozen member aliases) while any text reads them; in EP1a-1 they all stay, because the explicit calls read them; once EP1b has converted the readers they drop, and the one read left (`sg_.action`) becomes `ep.solve.action` | 2471-2494, 2511, 2534, 2538-2546 |
| T-B8 | ACCESSOR: the `x_emit` local (2537) becomes `const Eigen::VectorXd& x_emit = ep.x_emit();` while an explicit call reads it (EP1a-1), then drops; converted readers bind `ep.x_emit()` | |
| T-B9 | EMITTED: `ep.emitted = ...;` inserted after the emit bracket; the four comparisons become `ep.emitted` | 2572, 2665, 2694, 2699 |
| T-C | CALL: a converted call's argument list becomes `(run, ep, kept..., outs...)`; the statement's other tokens are kept | |
| T-D | SEAM: signature and preamble per 3.2; Doxygen per V7; adapters per 3.6 | |
| T-E | BRACKET: `subBegin(SB_X)` becomes `networkSubBegin(run, SB_X)` | 45 lines |
| T-F | CALLBACK: the two driver closures drop, their primary invocations become direct calls; the solve's two parameters drop and its replay calls the builders | 2289-2295, 2450-2468; solve 2131, 2141 |
| T-G | SETUP: the edits of the 4.1 table; the spec cut into `networkEngineSpec` | |

Each worker's run of the generator rewrites its own seam files and, as scratch, the two shared files (`ApplicationNetworkEpoch.cpp`, the contract test) from EP1a-1's rows plus its own. Re-running the generator on its own output changes nothing.

### 5.4 The binding model

A **root** names a storage object: `D:x` (declared in runNetwork's outermost scope, frozen), `L:x` (declared at the frozen lambda's top level), `A:x` (an `Application` member), `B:f.x` (a block local), `copy(R)` (a by-value copy of R taken at a known point). A **binding** is a root, a member chain and at most one `&` or `*`. Resolution of an expression, in the frozen and in the new source independently: the head identifier's declaration by a scope walk (innermost block first); a reference declaration is replaced by its initializer and resolved again; a parameter is replaced by the argument at the call site under examination; `run.<m>` resolves through the `NetworkRun` constructor call in the new `runNetwork` (references to the bound object; owned members to `R:run.<path>`, mapped to the frozen `D:` object by the OWNER_MOVE table); `ep.<group>.<m>` resolves through the epoch function's `NetworkEpoch ep` to the `L:` local whose realization V3c proves; an output parameter resolves through the argument its call site passes; a by-value declaration gives `copy(...)`; `copy(copy(X))` normalizes to `copy(X)` when X is const or not written between the two copy points. The scope walk respects the two shadowing facts in the moved code: the block-local `dt` at 2465 (the DD-carry predict's TOW-only step) and the nested `ep` / `run` declarations of 3.2.

The bindings that make the check necessary (35 of the 659 converted arguments bind under a parameter name that differs from the argument's leaf name; `base/renames.txt` lists them):

| seam | parameter | binds | what a by-name binding would do |
|---|---|---|---|
| `networkPassASlipScan_` | `carry_slipped_ambs` (writable) | `funnel.carry_slipped_raw` | PASS A's drains would skip the raw subset (N2) |
| `networkSolPrep`, `networkEmitStore` | `x_s` | `ep.x_emit()` | the products built from the float vector under card FCF's fold emit (N3) |
| `networkFixedCycles` | `we`, `datum`, `fixes` | `ep.solve.we_landed`, `ep.solve.plan.shared_datum`, `ep.ar_fixes` | `we` against `ep.ws.we_culprits` is the same type and compiles |
| `networkPassB_` | `sec_asm_wall_ms` | `run.timing.asm_wall_ms` | the run total written into the epoch's own wall |
| `networkPostFitRaim` | `prires`, `slip_ep` | `run.engines.prires`, `&run.engines.slip_qc.epoch` | |
| `networkSignalRosterAudit` | `raw`, `freed`, `emitted` | the `computed` conditional, `ep.funnel.carry_slipped_ambs`, `ep.emitted` | |
| `networkFixedCycles`, `networkPbHoldCloneUpdate`, `networkRedundancyCensus`, `networkRhsAttribution`, `networkRhsAttribTrip` | `datum` / `plan_datum` | `ep.solve.plan.shared_datum` | the trip's other caller, the solve, binds its local plan instead (N4) |
| `networkSdEwlAr`, `networkGrossLosScreen` (both sites), `networkPbHoldCloneHarvest` | `now_s` | `ep.clock.pl_now_s`; the harvest's `TimeUtils::continuousGpsSeconds(ep.se.time)` | |
| nine seams | `t` | `ep.se.time` | |
| `networkWriteStateSnapshot` | `emit_rebase`, `consumer` | `&run.emit.rebase`, `run.setup.consumer.get()` | |

Finding 1's defect reads, in this model: name `carry_we_drop_sats`, frozen root `L:carry_we_drop_sats` (the inline T2 block wrote the driver's pointer), new root `copy(L:carry_we_drop_sats)`; V3a fails it. Its WCF-landed form is negative control N1.

### 5.5 The checks (the verifier exits 0 only when all pass)

- **V1 Bodies.** (a) Every frozen code line of the lambda is carried exactly once by `networkProcessEpoch_` (shift 4) or is the old side of a declared transform of 5.3 whose new side is checked by rule. (b) Each converted seam's body below its end-of-bindings marker equals the frozen body code line for code line; the only exceptions are the declared lines (the solve's two replay lines and three epilogue lines). (c) Each adapter's explicit declaration and definition are byte-identical to frozen. (d) `runNetwork` equals frozen apart from the 4.1 edits. (e) Every hunk of `git diff <B>` lies in a generated region, a converted signature, a preamble, a generated Doxygen block, an include or forward-declaration line, or a hand edit listed in `declared.py`. Comments and blank lines are ignored, as in `rt_seam_verify.py` and `nom_td_verify.py`.
- **V2 Signatures.** Each converted seam has exactly one epoch-form declaration and one definition; its parameter list is `(NetworkRun&, const NetworkEpoch&, kept..., outs...)` with `outs` equal to the frozen writable epoch-rooted parameters mapped to their groups (or `NetworkEpoch&` under `--presolve-writer epoch`); a seam called after the frozen solve call has no output other than `ep.solve.handoff` or `ep.prep.sol`; kept parameters keep their frozen declaration text and order.
- **V3 Bindings.** (a) For every converted seam, at every frozen call site (driver, closures, other seams, tests) paired with its new site, and for every frozen parameter p: `resolve_frozen(argument)` equals `resolve_new(p's preamble initializer or the adapter's forwarding argument)` modulo 5.6; the preamble declarator equals the frozen parameter's declaration text. Adapter seams' other callers are byte-identical. (b) Every name the epoch function reads that the frozen lambda captured resolves to the frozen root (a reference, `const` allowed to be added, or a const copy of a const root). (c) Each `ep.` path is realized at the frozen declaration position of exactly one `L:` local by one transform of 5.3; its type text equals the local's; its default initializer equals the frozen initializer (T-B5) or is empty; no binding of it occurs before that position. (d) Each `NetworkRun` constructor argument is a named lvalue declared in runNetwork's outermost scope before the construction, member i receiving the object of role i. (e) Fresh names: no body reads a free `run`, `ep`, `nrun`, `nep` or output name other than the parameters (with the 3.2 pre-flight). (f) Every folded argument parses in the fold grammar.
- **V4 Driver writes after the solve.** After `ep.solve = networkSolveGlue(...)`, the only writes into `ep` in `networkProcessEpoch_` (assignments, non-const reference bindings, non-const member calls, output arguments) are `ep.ar_fixes =`, `ep.fixed_cycles =`, `ep.prep =`, `ep.emitted =` and the three output arguments; `ep.clock` is assigned exactly once, before the first seam that reads it.
- **V5 Deep const.** No member of `NetworkEpoch`, nor of any project struct held by value inside it (parsed recursively), is a non-const reference, a pointer to non-const, a smart pointer, a `std::function` or a `std::reference_wrapper`; `se` is the one reference (to const). A first-level scan of `StationAssembly`, `PppObservation`, `NetworkSbasis::Plan`, `NetworkSolPrepResult`, `NetworkSolutionEpoch`, `FixedCycleEmission`, `ArEpochFixes`, `EpochWorkspace`, `WithinEpochCulprits`, `RowTag` found none.
- **V6 Order.** The ordered event list of the epoch function (every call by callee, every log call with level and format literal, every bracket with its `SB_*`, every `return LoopSignal::X`) equals the frozen lambda's, with `subBegin`/`subEnd` read as `networkSubBegin`/`networkSubEnd` and the inserted stores (CTOR, CLOCK, EMITTED) placed as T-B fixes them; likewise `runNetwork` against its frozen self minus the lambda, and `networkEngineSpec`'s statements in their frozen order.
- **V7 Documentation.** Each converted declaration's generated Doxygen names the run state and the epoch members its preamble binds, split into read and written by the declarator, and carries each folded parameter's former `@param` text under its member name.
- **V8 Completeness.** Every top-level call of the frozen lambda is in the table once (converted, adapter, run-only, or explicit with its reason); the contract test's seam list equals the rows.
- **V9 No new overload.** For every translation unit that gains `app/NetworkRun.hpp` or `app/NetworkEpoch.hpp` (directly or through a converted seam header), the function names newly visible do not intersect the names its frozen code calls; the two new headers declare only their types, `networkEngineSpec`, `networkSubBegin`/`End` and the observer alias.
- **V10 Stage order.** Every `ep.<group>` a seam's preamble binds names a group available (2.3 banner) at the seam's spine line; for the builders the solve calls, at the solve's spine line. Machine-read from the banner table.
- **V11 Writer banners.** The set of spine lines that pass `ep.<group>` (or a member of it) as an output equals the group's "written by" list in the banner, plus the listed driver writes.
- **V12 Spec cut.** The statements of `networkEngineSpec` are the frozen statements of the declared ranges, character-exact; they contain no call outside an allowlist (`GnssConfig` getters, `getNetworkConfig`, `NetworkCarryVce::parseComponents`, `static_cast`, `std::vector` and `std::set` construction), no `log`, `traces` or `setup` token; each name they read resolves to `config_`, `nc`, `ctx.knobs` or a preamble alias whose text equals its frozen declaration; each engine constructor argument in `NetworkRunEngines` and `NetworkRun` is the spec field that holds the frozen argument expression, at the frozen position.
- **V13 Names.** The pre-flight of 3.2 re-run on the final tree: the six fallback seams use `nrun`/`nep`, no other seam needs them.

### 5.6 Declared substitutions (a closed list; anything else fails)

| class | what is accepted, and the proof obligation | entries |
|---|---|---|
| EPOCH_MEMBER | a lambda local's storage is a `NetworkEpoch` member (or, for the three clock values, a copy of the const local stored once); proved per member by V3c | 24 locals and the 3 clock values |
| OWNER_MOVE | a driver-local engine or group is a run-owned member, the frozen name now a reference to it; the object is constructed once, at the run's construction, with arguments equal by SPEC_CUT; nothing between the old and the new construction point reads it | `est`, `carry_driver`, `carry_vce_est`, `prires_est`, `slip_qc`, `iono_deweight`, `fc_ppp_utils`, `emit_engines`, `lane_run`, `timing`, `cl_tally` |
| SPEC_CUT | a setup statement moved into `networkEngineSpec`, proved by V12 | the ranges of 4.1 |
| FIELD_MOVE | a field moved to a new owner path, type text unchanged, no frozen use of the old path left | the three hand-off fields; `sec_sub_ep0`, `sub_wall0`, `sub_tmr0` |
| COPY_SOURCE | a const driver copy of a setup field is read from the field; proof: no write to the field after `networkSetup_` returns (lexer write scan over `src/`) | `n_threads`, `seam_threads`, `carry_par`, `brdc_rts` |
| VALUE_EQUAL | an expression replaced by an epoch value equal to it at every reader; proof: the single writer of `ctx.counters.n_emitted` inside the loop is `NetworkEmitStore.cpp` 670 (`++n_emitted`; the other write, `NetworkWarmResume.cpp` 1239, runs before the loop) and no call between the store and the last reader reaches it | `ep.emitted` at 4 sites |
| ACCESSOR | `x_emit` is `ep.x_emit()`, whose expression resolves to the frozen initializer (2537) under EPOCH_MEMBER | 2 bindings |
| CALLABLE | the third-band closure is `networkIsUncarriedThirdBand`: body character-exact, init-captures as const preamble lines, the DD build's `std::function` parameter replaced by a preamble lambda forwarding to it | 1 |
| CALLBACK_DIRECT | the two solve callbacks are direct calls whose arguments resolve, at the replay, to the objects the closures captured | 2 |
| BRACKET | the two timing closures are functions, bodies character-exact; each call site gains `run` | 2 + 45 sites |
| TYPE_MOVE | `LoopSignal` is `Application::LoopSignal` | 1 |

Not substitutions, and therefore failures: a change of declarator kind (reference to copy, copy to reference, adding non-const), a change of root, a change of a kept argument's text, a member realized at a different position, an unlisted body edit.

**Amended at the EP1 review round (2026-10-14): the twelfth class.**

| class | what is accepted, and the proof obligation | entries |
|---|---|---|
| UNREAD_ALIAS | a `runNetwork` alias whose every reader moved out with the lambda (or with a removed closure or cut statement) loses its line; proof: one declarator, a reference or a const builtin copy of a member path (no call, so the line had no effect but the binding), and no token of the remaining `runNetwork` names it; and no declaration of `runNetwork`'s outermost scope is left unread | 33 at main `926dafa` |

### 5.7 Negative controls (pre-registered; each must fail its named check before a pass is believed)

| id | mutation of the candidate tree | expected failure |
|---|---|---|
| N1 | WCF shape: `WithinEpochCulprits we_culprits = ep.ws.we_culprits;` (a copy) in the DD build's preamble | V3a (declarator) |
| N2 | `networkPassASlipScan_` binds `funnel.carry_slipped_ambs` | V3a (root) |
| N3 | `networkSolPrep`'s adapter forwards `ep.solve.x_s` | V3a (frozen: the x_emit conditional) |
| N4 | `networkRhsAttribTrip` converted directly, the solve's internal call bound to `ep.solve.plan` | V3a at `NetworkSolveGlue.cpp` 2711, and V10 |
| N5 | `auto stations = run.setup.stations;` in the epoch function's preamble | V3b (copy of a captured reference) |
| N6 | `networkIonoDiag` and `networkDdFrac` calls swapped | V6 |
| N7 | `ep.pass_a.present.clear();` after the solve in the driver | V4 (and the lint) |
| N8 | `est_index_of` sized with `0` instead of `-1` | V1a / V3c |
| N9 | a one-token edit inside a converted body | V1b |
| N10 | the shadowing `dt` (2465) rewritten to `ep.clock.dt` | V1a |
| N11 | the `debug_r_scale` warning (762-765) cut into `networkEngineSpec` | V12 |
| N12 | `networkFixedCycles` defined with `ep` as its parameter name | V13 (and the compiler) |
| N13 | `networkClockQc` given `EpochPassA&` as a second output | V2 and the contract test |
| N14 | the replay's WCF `resolves` counter line dropped | V1b |
| N15 | `networkHandoverStransformBeforeRows` binds `ep.solve.plan` | V10 |
| N16 | `ep.emitted` stored before `networkEmitStore` | V6 / V4 |

A positive control runs too: the verifier on EP1a-1 alone (no seam converted) passes with only the setup, field, type and storage substitutions.

### 5.8 What G-R proves, what it cannot, and who covers each gap

Proved for every path: the text and order of every moved statement; the object every name in every moved body denotes; the reference or copy semantics of every parameter; the initial state of every epoch member where its local was declared; that no seam after the solve can write the epoch except through its two named outputs, and the driver only through its four stores; that the engines are built from the arguments they had; that no stray edit entered the tree.

| gap | covered by |
|---|---|
| default construction then move assignment equals direct initialization (T-B4, T-B7) | G-EP, every arm |
| construction order: the engines are built at 285 instead of 292-955; heap layout differs | G-EP, every arm (a pointer-keyed container would show) |
| destruction order and exception unwinding (no engine has a user destructor; an exception leaves the run) | G-EP |
| the move of about 1 400 lines to a new translation unit (code generation, inlining; `/fp:precise`) | G-EP; the cost pair |
| bindings that differ from their by-name alternative only on some postures | the witness arms of section 6 |
| reachability | section 6 names what each arm reaches and what none reaches |

### 5.9 The permanent checks

- `tests/test_network_epoch_contract.cpp` (generated from the rows): the static asserts of 2.7; for every converted free seam `static_assert(std::is_same_v<decltype(static_cast<R (*)(NetworkRun&, const NetworkEpoch&, ...)>(&networkX)), R (*)(NetworkRun&, const NetworkEpoch&, ...)>)` with exactly the outputs of its row (the explicit overload of an adapter is not selected by the cast); the five private member seams through `NetworkEpochContractProbe`; every observer row checked against `NetworkEpochObserver`. A seam that starts writing the epoch must edit this list, which is the point.
- `tests/test_network_engine_spec.cpp` (suite `NetworkEngineSpec`, in the style of EP0's `NetworkKnobsEpochDriver`): on the defaults, both goldens and the retired lane-carry posture, every spec field equals the frozen expression evaluated directly; `networkIsUncarriedThirdBand` equals the frozen closure on a table of (constellation, signal) cases under `uwl_on` off, on, and with `uwl_dual_on`.
- `scripts/nom_epoch_lint.py`: V4, V10, V11 on any tree, run before a commit that touches a seam signature or the epoch function (written into CLAUDE.md's seam contract).

---

## 6. Gate arms (G-EP: `scripts/nom_td_gate.py --whole`, same-build Release, renamed exes)

Reference executable: `<B>`'s build; candidate: the step's. Every arm compares the whole `stdout.txt` and `main_log`, every CSV, `products.txt`, the manifest, `rt_engine.log` and the exit code. "Evidence" is what the arm's own output must show for the reach to count.

| arm | posture | reaches | evidence |
|---|---|---|---|
| `f2` | field golden 2 h (GPS + Galileo since GAD) | the normal path; the max-duration `Stop` | run-end counts |
| `s2` | sim golden 2 h from 00:30:15 | the start-offset exit on the first epochs (no `ep` built), then the normal path | skipped-epoch count |
| `zero` | field golden, start offset 30 h | the start-offset exit on every epoch; the run end's zero-emit branches, exit 1 | exit code 1 |
| `cil` | retired `golden_field_gps_dual` with the lane carry, 1 h | `carry_invariant`: the DD-carry predict (shadowing `dt`), the DD build, the install, the lane run and the EWL fill, eq34 and the RPB allocation, the T2 tier armed | WCF and lane run-end lines |
| `joint` | T2 adapts, 3 isolated-arc refloats | the T2 replay (the two direct builder calls), `est_index_of`'s second writer, an isolated-arc culprit, the hand-off seeded by the solve | T2 run-end line |
| `wcff` | retired golden, T2 floor and trigger forced down | the WCF writer-end counter path under `carry_invariant` | WCF row (quiet verdict D1) |
| `bcl` | border clone 1 h, phase-visible and clock-level monitors | `networkPhaseVisibleMonitor`, `networkClockLevelMonitor`, the PB-hold clones | their trace files |
| `snapw` | field golden 0.6 h, snapshot with every capture layer | `networkWriteStateSnapshot`'s engine pointers, the warm-resume restore into owned engines at setup | snapshot bytes equal (UTC masked) |
| `snapr` | warm resume from the reference's snapshot | the resume writing `run.engines.carry_driver` and `run.emit.*` through the setup aliases | resume run-end lines |
| NOM2's other eight smokes | GLONASS, replay with injection, pin bridge, SD-EWL, LGM, second phase channel, diagonal carries, gauge-free carry (`joint` is the joint-coupling smoke) | their families' seams and the inject path | as in NOM2 section 8 |
| NOM2's `tri`, `gal`, `cold`, `shx` | three-band 1 h, GPS + Galileo 1 h, card ICF's cold bound, shared history with clock clones | the third-band predicate (`networkIsUncarriedThirdBand`), the gross-LOS cold bound, the clone replay folds inside the solve | |
| **A4** (new) | field golden, `network.raim_withhold` below the settled sigma0 (calibrated on a 0.25 h smoke), `network.rhs_attrib_on_trip` true | the solve's exit 2736 and its internal `networkRhsAttribTrip` (explicit form, no `x_s`), `wh_free_erase` if armed | `n_sigma0_withheld` > 0; `logs/rhs_attrib/` compared |
| **gate** (new) | field golden, `network.rank_gate_tol` raised until the rank gate withholds (1e-3, then 1e-2) | the solve's exit 1327 | `n_gate_withheld` > 0 |
| **fc** (new) | field golden, `network.fast_clock.enable` and `shadow`, `epoch_decimation_n` 2 | both `networkFastClockTick` sites (kept `false` / `true`), the decimation exit, `networkFastClockHarvest` under `ep.emitted` | fast-clock trace rows |
| **fcf** (new) | card FCF's graded fold-emit arm | `ep.x_emit()` selecting `fixed_cycles.x_held` in solution prep and the emitter | fold run-end line, `fce.emit` epochs > 0 |
| **raw** (new) | retired golden with the lane carry and `carry_invariant_innov_retap_only` | the raw-subset binding of `networkPassASlipScan_` read by the DD build | D5 v4 run-end line |
| **hand** (new) | field golden with `clk_mon_couple` and `ura_retro` | the hand-off's sets non-empty through RAIM and clock QC into the stash | refloat counts |
| **legacy** (new) | a legacy-datum posture (`phase_datum_prioritized` off) with an injected 10-minute outage of the clock-datum station | `networkPassAHead_`'s skip | "PIVOT ... skipping epoch" lines |
| **cul** (new) | `joint` plus an injected `sat_clock_step` and an `rx_clock_step` | T2 with a satellite and a station culprit | T2 culprit counts by kind |
| **census** (new) | field golden with every measure-only instrument armed (trace-postfit group, SH spectrum, station block, prefit walk, redundancy census, block-Baarda, congruence, MDB, datum graph, RHS attribution window) | every observer of rows 80-91 | their CSVs |
| **roll** (new) | a 2 h sim fixture whose span crosses a Saturday/Sunday 00:00 GPST boundary | the week-aware `dt` and the TOW-only DD-carry step at a rollover (finding 4) | the epoch at the boundary |

**What no arm reaches, named.** The solve's datum-failure exit (762: `buildSharedDatum` throws only on an invalid broadcast anchor sigma, which the loader refuses), the solve-failure exit (1461: `est.solve` throws only on a logic error), and the T2 restore failure (2506), unless a candidate posture found while building the arms shows a non-zero count; `networkAssemblyRegisterColumns_`'s empty-system skip unless the `legacy` or `zero` arm shows one; the streaming epoch source (`networkProcessEpoch_(run, se_live)`, one call-site rewrite, proved by V1d and V3d only); `roll` if the simulator cannot be pointed at a week boundary within the step (then finding 4 stays covered statically). Each of these is listed in the card's result section with its static coverage.

**Cost.** An idle-machine ABBA pair on both 2 h goldens, within 1 %. Expected change is nil: references add no instruction; the per-call `std::function` temporaries of the predicate and the two callbacks disappear; `ep.solve` and `ep.prep` are filled by one move each; 45 bracket calls cross a translation unit (wall clock only, a few nanoseconds against seconds per epoch); a decimated epoch now builds an empty `NetworkEpoch` (today it built an empty `EpochWorkspace`).

---

## 7. Work split, base commit, integration, merge risks

**EP1a-0, the tooling (one worker, starts as soon as EP0 is on main; changes no source file).** `nom_cpp.py` with its inventory self-test, `nom_ep1/`, the generator in `--survey` mode, the verifier, the lint, the negative-control harness. Acceptance: the self-test reproduces `base/calltable_ep0.md`; the verifier run frozen-against-frozen passes; N5-N12 fire on synthetic mutations of `<B>`. Separating it takes the tooling (about 3 100 lines) off the critical path of the code change and lets it be reviewed alone.

**EP1a-1, the base (one worker, serial, gated alone).** Files: `include/app/NetworkRun.hpp` (new), `src/core/app/NetworkRun.cpp` (new: `networkEngineSpec`, `networkSubBegin`/`End`), `include/app/NetworkEpoch.hpp` (new), `include/filter/NetworkRunGroups.hpp`, `include/filter/NetworkSolveGlue.hpp` and `src/core/filter/NetworkSolveGlue.cpp` (the hand-off only), `include/filter/NetworkDdPriorBuild.hpp` and `.cpp` (adds `networkIsUncarriedThirdBand` only), `include/app/NetworkRunContext.hpp` (one comment), `include/app/Application.hpp` (`LoopSignal`, `networkProcessEpoch_`, the friend), `src/core/app/ApplicationNetwork.cpp`, `src/core/app/ApplicationNetworkEpoch.cpp` (new, generated: T-A, T-B, T-E, the predicate lambda in its preamble; every call still explicit, reading the preamble's names), `CMakeLists.txt` (two sources, two tests), `tests/test_network_epoch_contract.cpp` (types only), `tests/test_network_engine_spec.cpp`. Gate: Debug compile, Release build, suite, the verifier (V1a, d, e; V3b-d; V4; V5; V6; V8; V12; positive control; N5, N6, N7, N8, N10, N11), G-EP on `f2`, `s2`, `zero`, `cil`, `snapw`, `snapr`, `fc`.

**EP1b, four workers on disjoint files**, each branched from EP1a-1, owning its seam files and its rows file:

| worker | owns | seams | witness arms |
|---|---|---|---|
| W1 app | `Application.hpp` (the five member declarations), `ApplicationNetworkAssembly.cpp`, `SignalRoster.{hpp,cpp}`, `NetworkRunContext.hpp` (three declarations), `NetworkBorderPrior.cpp`, `NetworkStateSnapshotWrite.cpp`, `NetworkLocalCarryHarvest.cpp` | 10: `networkPassAHead_`, `networkPassASlipScan_`, `networkAssemblyPrefilter_`, `networkAssemblyRegisterColumns_`, `networkPassB_`, `networkBuildSignalRoster`, `networkSignalRosterAudit`, `networkCouplingExactPrepare`, `networkRxCbDemeanPrepass`, `networkWriteStateSnapshot` | `raw`, `legacy`, `snapw`, `snapr` |
| W2 pre-solve and solve | `NetworkFastClock`, `NetworkSignalIntegrity`, `NetworkMeasurementAdmission`, `NetworkLevelAuthority`, `NetworkSdEwlAr`, `NetworkOraclePin`, `NetworkHandoverStransform`, `NetworkCarryDisposition`, `NetworkDdPriorBuild`, `NetworkSolveGlue`, `NetworkInstallCarry` (headers and sources) | 13 (4 adapters), and T-F | `joint`, `cul`, `wcff`, `A4`, `gate`, `fc`, `cil` |
| W3 emit and post-fit | `NetworkProducerAr`, `NetworkGrossLos`, `NetworkFixedCycles`, `NetworkSolPrep`, `NetworkPbHoldClone`, `NetworkEmitStore`, `NetworkRaim`, `NetworkClockQc`, `NetworkStashStatus`, `NetworkClockLevel`, `NetworkRhsAttrib` | 15 (4 adapters, 1 run-only) | `fcf`, `hand`, `A4`, `bcl`, `roll` |
| W4 diagnostics and observers | `NetworkIonoDiag`, `NetworkDdFrac`, `NetworkShDevVtec`, `NetworkShHealth`, `NetworkIonoDwRelease`, `NetworkLeveling`, `NetworkPhaseVisibleDiag`, `NetworkLedgerEmit`, `NetworkSecondPhaseScope`, `NetworkChannelAll`, `NetworkMultiGnss`, `NetworkParamInventory`, `NetworkContributions`, `NetworkFrameDiag`, `NetworkStationBlock`, `NetworkPrefitCensus`, `NetworkRedundancyCensus`, `NetworkBlockBaarda`, `NetworkCongruence`, `NetworkMdb`, `NetworkDatumGraph`, `NetworkAmbStateTrace` | 24 (3 adapters) | `census`, `bcl`, NOM2's trace-armed arm |

Each worker: flip its rows, run the generator (it rewrites its files and, as scratch, the two shared generated files), build Debug and Release, run the suite, run `nom_ep1_verify.py --rows <its file>` with its negative controls, run G-EP on the four core arms plus its witness arms.

**Integration order:** W2 (the solve, the replay, `est_index_of`'s second writer, the internal trip call: the most binding risk), then W1 (PASS A and PASS B, the raw-subset binding), then W3, then W4. After each merge: `python scripts/nom_ep1_cut.py --shared-only` regenerates `ApplicationNetworkEpoch.cpp` and the contract test from `<B>`, EP1a-1 and the union of merged rows; Release build; suite; verifier on the merged rows. After the last: the verifier with every row and all sixteen negative controls; full G-EP (31 arm pairs: NOM2's 18, G1's `wcff`, `snapw`, `snapr`, and the ten new arms); the ABBA cost pair.

**EP1c, the const tightenings (one commit after EP1 lands on G-EP).** Each is proved by compilation and changes only generated preamble declarators: the DD build's and the MW inject's funnel parameter const (the DD build and the solve lose their `funnel` output); `networkPassB_`'s `modeled` const if it compiles (it reads it only, `ApplicationNetworkAssembly.cpp` per the types design's probe); the estimator const in `networkDdFrac`, `networkShDevVtec`, `networkShHealth`, `networkLeveling`, `networkStashStatus`, `networkPostfitSigma0`, `networkStationBlock`, `networkRhsAttribution` (only `reducedSharedNormals()` is non-const after the solve, `NetworkFrameDiag.cpp` 79 and `NetworkIonoDiag.cpp`, so `networkIonoDiag` and `networkShSpectrum` keep the writable estimator). The verifier adds one check for it: for every member call on a tightened object, the selected overload is the same function or a const twin with an identical body. G-EP core arms.

**Docs (with the last merge).** The NOM3 card's EP1 result section with the verifier's summary line and the arm table; `docs/INDEX.md`; CLAUDE.md: the seam contract (below), the routing tree, the driver's line counts and the network tour.

> Seam contract (draft for CLAUDE.md). A per-epoch network seam is a free function (or one of the five `Application` members) `R networkX(NetworkRun& run, const NetworkEpoch& ep[, kept...][, outputs...])`. It owns no state. It opens with one binding per name it uses, reads through `ep` (const), writes through its output parameters (marked `// written`), and below the bindings never spells `ep.`. An output is the epoch group the seam is listed as a writer of in `app/NetworkEpoch.hpp`, or the single member it writes of another stage's group; after the solve the only outputs are `ep.solve.handoff` and `ep.prep.sol`. A seam reads only groups available at its spine line. A measure-only seam has the observer form `void(NetworkRun&, const NetworkEpoch&)`. Run `python scripts/nom_epoch_lint.py` before committing a change to a seam signature or to `ApplicationNetworkEpoch.cpp`; a new epoch writer is a row of `tests/test_network_epoch_contract.cpp`.

> Seam contract, amended at the EP1 review round (2026-10-14; the draft above stays as the pre-registered text). A per-epoch network seam is a free function (or one of the five `Application` members) in one of three forms: the epoch form `R networkX(NetworkRun& run, const NetworkEpoch& ep[, kept...][, outputs...])`; the run-only form `void networkX(NetworkRun& run)` for a seam that reads no epoch member (`networkClockLevelRestoreRaise`); or, for a seam with another caller (a unit test, the solve's internal trip call), the adapter form: its explicit signature kept, plus an epoch form whose body is one forwarding call that takes the explicit parameters from `run` and `ep` in its argument list. A seam owns no state. A definition in the epoch form opens with one binding per name it uses, reads through `ep` (const), writes through its output parameters (marked `// written`), and below the bindings never spells `ep.`; a seam whose body itself declares or uses `run` or `ep` names those parameters `nrun` / `nep` in its definition (the declaration keeps `run` / `ep`). An output is the epoch group the seam is listed as a writer of in `app/NetworkEpoch.hpp`, or the single member it writes of another stage's group; after the solve the only outputs are `ep.solve.handoff` and `ep.prep.sol`. A seam reads only groups available at its spine line. A measure-only seam has the observer form `void(NetworkRun&, const NetworkEpoch&)`. `ApplicationNetworkEpoch.cpp` and `tests/test_network_epoch_contract.cpp` were written by the EP1 generator and are maintained by hand once EP1 lands (nothing regenerates them): a new spine seam, or one that starts writing the epoch, changes its line of the contract test in the same commit as its signature, and `python scripts/nom_epoch_lint.py` runs before any commit that changes a seam signature, `ApplicationNetworkEpoch.cpp` or `CMakeLists.txt`.

**Merge risks and their handling.**
1. The two shared generated files: never hand-merged; a conflict is resolved by `--shared-only`.
2. Transitive includes: workers add forward declarations to seam headers and must not remove an include (another translation unit may rely on it); removals wait for EP1c with a full rebuild.
3. Files touched twice in sequence: `NetworkSolveGlue.hpp`/`.cpp` (EP1a-1 hand-off, then W2), `NetworkDdPriorBuild.*` (EP1a-1 predicate, then W2), `Application.hpp` (EP1a-1, then W1), `NetworkRunContext.hpp` (EP1a-1 comment, then W1): sequential by construction.
4. A test translation unit that includes the run-context headers may cross the Debug section limit (C1128, as `test_block_baarda.cpp` did): the contract test gets `/bigobj` if it does.
5. Concurrent work: about 100 files are owned from EP1a-1 to the landing. In flight at writing: `uom/work` (card UOM step U1a: `StateManager`, `StateFamily`, the real-time snapshot; disjoint but for one `CMakeLists.txt` line), `wcf/fix` and `nom/hv0` (paused, superseded by the landed WCF and HV0), `l5g/combined` (docs). Any commit to an owned file in the window means a rebase by regeneration from the new `<B>`, never a hand merge.
6. EP0 not on main: EP1a-1 cannot start until it is; EP1a-0 can.

---

## 8. Deviations from the card's EP1 text, each with its reason

1. **The run is non-const after the solve** (card: `(const NetworkRun&, const NetworkEpoch&)`). Const does not pass through reference members, so a const run would still hand out a writable context, traces and setup while making the owned engines read-only, which breaks every post-solve seam that writes an engine (`networkShHealth`, `networkEmitStore`, ...). Engine constness stays per seam in the preamble declarators (and EP1c tightens eight of them).
2. **Before the solve the epoch is const too, with named group outputs** (card: `(NetworkRun&, NetworkEpoch&)`). The card's form would let any pre-solve seam write any member and would erase from the signatures the data flow the card says they record; with outputs, every epoch write is visible on its spine line and compiler-checked on both sides of the solve. The generator also emits the card's form (`--presolve-writer epoch`) from the same rows; author decision 1.
3. **Seams take more than one extra parameter in places**: the two kept parameters, the solve's four outputs (its replay rebuilds `ws`, `est_index_of`, `dd_build` and declares `funnel` writable until EP1c), the PASS-A screens' two groups. The card's "plus the one member they write" holds after the solve.
4. **Eleven adapters keep the explicit forms** (ten for tests, one for the solve's internal trip call); step EP2 retires the ten through a test fixture, step SG the last.
5. **Unconverted calls**: the input stage, the EP2 blocks' calls and the method calls keep their text; `networkClockLevelRestoreRaise` takes `(NetworkRun&)`.
6. **Epoch grouping**: `funnel` for the card's "slips", `dd_build` for its "carry"; `ar_fixes`, `fixed_cycles` and `emitted` added (card FCF's stages postdate the card); the hand-off is nested in `solve`; `prep` holds the prepared solution.
7. **EP4 work pulled in, at its minimum**: `networkEngineSpec` (the engines' constructor arguments, a character-exact cut) and `networkIsUncarriedThirdBand` (the predicate), both forced by building the owner in place at line 285; EP4 splits the spec into its concerns' builders.
8. **SG work pulled in, at its minimum**: the two solve callbacks become direct calls (two body lines and the signature), because with `(run, ep)` the closures are pure indirection and that indirection is where card WCF's defect hid; the result type, the exits and the replay's fold order (finding 2) stay for SG.
9. **One `emitted` value** replaces the four `n_emitted != fc_n_emitted_before` comparisons (single writer, VALUE_EQUAL).
10. **`LoopSignal` stays** (now `Application::LoopSignal`); unifying the epoch's exits on one enum belongs to EP2's admission function, which the card says returns Process, Skip or Stop.
11. **`subBegin` / `subEnd` become functions over the run** (`networkSubBegin` / `networkSubEnd`), not a scoped bracket; the scoped bracket comes with EP2, when its partial-time attribution on the two skips inside a bracket can be declared.

---

## 9. Decisions for the author

1. **Pre-solve signatures**: named group outputs (this specification; deviation 2) or the card's `NetworkEpoch&` (one generator flag).
2. **The solve's callbacks**: direct calls in EP1 (this specification; deviation 8) or kept until SG with the binding check through the closures.
3. **Adapter retirement**: in EP2 through `tests/support/NetworkTestRun.hpp` (this specification) or seam by seam as each seam's own step rewrites it.
4. **Exits no configuration reaches** (datum failure, solve failure, T2 restore failure): accept them as named-unreached with static G-R coverage (this specification), or add a test-only fault knob, which is a new configuration key and outside EP1's identical-output scope.
5. **The rollover arm**: regenerate a short sim fixture across a GPS week boundary for `roll`, or name the rollover unreached.
6. **The lint** as a script run by the seam contract (this specification) or registered as a ctest.

---

## 10. The judges' defects, checked against the code and disposed

| # | defect (who raised it, for which design) | check at `a453448` | disposition |
|---|---|---|---|
| 1 | reloc's V3e misses outermost `ep`/`run` declarations, so four direct conversions are ill-formed (both judges) | confirmed: `NetworkBorderPrior.cpp` 780, `NetworkFixedCycles.cpp` 741, `NetworkRedundancyCensus.cpp` 241, `NetworkContributions.cpp` 746, all inside their seams' bodies; nested: `ApplicationNetworkAssembly.cpp` 3574, `SignalRoster.cpp` 1750, 1797 | 3.2 pre-flight at any depth with fallback definition names; V13; N12 |
| 2 | reloc defers engine ownership to EP4 against the card (both) | the obstacle stated (moving logging setup) does not hold: the engines' constructor arguments read only `config_`, `nc`, `ctx.knobs`; the one log line in range stays | owner in EP1 (2.2, 4.1), V12, N11 |
| 3 | reloc's interim two bundles of the engines (judge 2) | `NetworkRun` held 19 references beside `NetworkRunEndInputs` | one owner: `run.engines`; the run end binds it |
| 4 | reloc's pre-solve writers take the whole `NetworkEpoch&`; readers can read a later stage (both) | confirmed by construction | named outputs (3.1), V10 stage order, the permanent lint, N13, N15 |
| 5 | reloc keeps the two callbacks (judge 1) | `NetworkSolveGlue.hpp` 144-145, replay 2131, 2141 | direct calls with the WCF counter kept (3.4), N14 |
| 6 | reloc's `x_emit` self-pointer and mutable post-solve members (both) | confirmed in its 3.2 | accessor (2.3); driver stores limited to four (V4, lint) |
| 7 | reloc's hand-off includes `retro_feed` (judge 2) | `retro_feed` written only by the solve (2855), read by value at RAIM and clock QC | kept in the result (2.4) |
| 8 | reloc's generic signatures hide the data flow; 55-alias epoch preamble (judge 2) | the epoch function's preamble at EP1's exit is 48 lines (4.2), all frozen declaration text | outputs on spine lines, `// written`, banners checked by V11, generated Doxygen (V7), bracket functions instead of relocated closures |
| 9 | reloc's 3 000 lines of tooling on the serial path (both) | estimate 3 100 lines | EP1a-0 separated, startable before EP0 lands, reviewed alone (section 7) |
| 10 | types/reader: the solve's internal `networkRhsAttribTrip` call (both) | confirmed: `NetworkSolveGlue.cpp` 2711, local plan, no `x_s` | explicit form kept, epoch-form adapter for the driver; N4 |
| 11 | types/reader: test callers missed (both) | the full set by `callers.py`: 11 seams with other callers (3.6), including `networkCarryDecide` (four estimators) and `networkPbHoldCloneHarvest` | adapters; no test edited in EP1 |
| 12 | types: retiring the solve's result type rewrites its body | | result type kept; five body lines declared |
| 13 | types/reader: the replay sketch drops the WCF `resolves` counter (both) | counter at 2139-2140, between the builder calls | kept, shown in 3.4; N14 |
| 14 | types: `/we4834` for all of `gnss_core` (both) | | not taken |
| 15 | types: two homes for `NetworkRunEngines` (both) | | stays in `NetworkRunGroups.hpp` |
| 16 | types: `loop_begin` changes meaning (judge 2) | the default-constructed estimator now precedes it | comment updated; wall clock, masked by G-EP |
| 17 | types/reader: no verifier tooling, no worker split | | sections 5 and 7 |
| 18 | reader: `NetworkRunEndView::run` as `const NetworkRun&` reopens run-end writes (judge 1) | `NetworkRunEnd.hpp`, `NetworkRunEndView` (a const context today) | run end untouched; `NetworkRunEndInputs` binds the owned groups |
| 19 | reader: 150 setup lines reordered at 991; `PppUtils` logs at debug (judge 2) | `PppUtils::PppUtils` at `PppUtils.cpp` 40 | nothing that logs moves; both `make_unique` lines stay |
| 20 | reader: `GrossLosPhase::AfterHarvest` does not exist | `NetworkGrossLos.hpp` 484-488: `Full`, `Decide`, `Act` | kept argument with the frozen conditional |
| 21 | reader: engine constness only a convention | | preamble declarators verified by V3a; EP1c tightens; the contract test pins signatures |
| 22 | reader: `networkSignalIntegrity`'s literal `carry_invariant=true` in its test | | adapter keeps the explicit form; the test is unchanged |

---

## 11. Verification notes

- EP0's existence, content and base: `git log`, `git show --stat a453448`, `git diff --stat main nom/ep0` (EP0's 21 files plus the card).
- The call table at the base: `spec_tools/calltable.py` with `EP_SRC`/`EP_INC` on the frozen copies (99 calls, 753 `network*` arguments); the capture set: `spec_tools/captures.py` (62 names plus `LoopSignal`).
- Callers of every converted seam: `spec_tools2/callers.py` over `git archive a453448 src tests`, comments and strings blanked.
- The solve's exits (762, 1327, 1461, 2506, 2736), the trip call (2711), the replay lines (2131, 2139-2141), `resolve_dropping`'s `[&]` capture (1819), the epilogue (2847-2864): read in `NetworkSolveGlue.cpp` at `a453448`.
- The setup lines of 4.1 and the lambda's structure: read in `ApplicationNetwork.cpp` at `a453448`.
- Engine constructors and destructors: `NetworkEstimator.hpp` 86, `NetworkInvariantCarryDriver.hpp` 412-419, `NetworkCarryVce.hpp` 195, `NetworkPriorResidual.hpp` 614, `NetworkIonoDeweightGate.hpp` 86/101, `NetworkMwWlLayer.hpp` 165, `NetworkPbEmitFilter.hpp` 230, `NetworkClockEmitRebase.hpp` 134; no user-declared destructor on any engine.
- The single writer of `n_emitted` in the loop: a write scan over `src/` (only `NetworkEmitStore.cpp` 670 and the pre-loop resume `NetworkWarmResume.cpp` 1239).
- Name collisions: ripgrep over the seam definition files for `ep`/`run` declarations, matched to the enclosing function's extent.
- The gate arms' differences from `f2`: the `arm.json` of `runs/_rap_g1_ref_{zero,joint,snapw,snapr,bcl}` flattened and diffed.
- Not built or run: no compile, no arm. Every claim about compilation (the fallback names, the contract test's casts, EP1c) is an acceptance check of the step, not a measured fact.

---

## Appendix A. Every converted call site, every frozen parameter's binding

Generated by `spec_tools/convtable.py` and `compact.py` from the frozen base. `[W]` non-const reference, `[W*]` pointer to non-const, `[R]` const reference, `[R*]` pointer to const, `[V]` by value, `[F]` `std::function`. A bold entry is an output: the group member the seam writes, or a single-member output parameter. For an adapter the same bindings are the forwarding call's arguments.

| # | seam (declared in) | form | parameter = binding [frozen kind] |
|---|---|---|---|
| 4 | `networkBuildSignalRoster` (app/SignalRoster.hpp:886) | direct | ctx [W] = `run.ctx`; config_ [R] = `run.config`; log [R] = `run.log`; traces [W] = `run.traces`; se [R] = `ep.se`; stations [R] = `run.setup.stations` |
| 7 | `networkFastClockTick` (filter/NetworkFastClock.hpp:187) | direct | ctx [W] = `run.ctx`; config_ [R] = `run.config`; log [R] = `run.log`; traces [W] = `run.traces`; setup [W] = `run.setup`; se [R] = `ep.se`; shadow_pass [V] = kept: `false` |
| 8 | `networkParamInventoryNoteSatellites` (filter/NetworkParamInventory.hpp:895) | direct | inv [W] = `run.ctx.param_inventory`; se [R] = `ep.se` |
| 12 | `networkFastClockTick` (filter/NetworkFastClock.hpp:187) | direct | ctx [W] = `run.ctx`; config_ [R] = `run.config`; log [R] = `run.log`; traces [W] = `run.traces`; setup [W] = `run.setup`; se [R] = `ep.se`; shadow_pass [V] = kept: `true` |
| 14 | `networkPassAHead_` (app/Application.hpp:788) | member | se [R] = `ep.se`; ctx [W] = `run.ctx`; setup [W] = `run.setup`; traces [W] = `run.traces`; modeled [W] = **pass_a.modeled** (out); present [W] = **pass_a.present** (out); shared_phase_keys [W] = **pass_a.shared_phase_keys** (out) |
| 15 | `networkPassASlipScan_` (app/Application.hpp:813) | member | se [R] = `ep.se`; ctx [W] = `run.ctx`; setup [W] = `run.setup`; traces [W] = `run.traces`; dt [V] = `ep.clock.dt`; pl_now_s [V] = `ep.clock.pl_now_s`; carry_warm [V] = `ep.clock.carry_warm`; modeled [W] = **pass_a.modeled** (out); present [W] = **pass_a.present** (out); shared_phase_keys [R] = `ep.pass_a.shared_phase_keys`; carry_slipped_ambs [W] = **funnel.carry_slipped_raw** (out); qc [W] = `run.engines.slip_qc` |
| 16 | `networkAssemblyPrefilter_` (app/Application.hpp:831) | member | se [R] = `ep.se`; ctx [W] = `run.ctx`; setup [W] = `run.setup`; traces [W] = `run.traces`; qc [R] = `run.engines.slip_qc`; modeled [W] = **pass_a.modeled** (out); present [R] = `ep.pass_a.present`; n_dropped_sats [W] = **pass_a.n_dropped_sats** (out) |
| 17 | `networkSignalIntegrity` (filter/NetworkSignalIntegrity.hpp:314) | adapter | ctx [W] = `run.ctx`; config_ [R] = `run.config`; log [R] = `run.log`; traces [W] = `run.traces`; t [R] = `ep.se.time`; epoch_idx [V] = `run.engines.slip_qc.epoch_idx`; stations [W] = `run.setup.stations`; present [R] = `ep.pass_a.present`; modeled [W] = **pass_a.modeled** (out); carry_slipped_ambs [W] = **funnel.carry_slipped_ambs** (out); carry_slipped_raw [W] = **funnel.carry_slipped_raw** (out); carry_invariant [V] = `run.ctx.knobs.carry_invariant`; carry_warm [V] = `ep.clock.carry_warm` |
| 18 | `networkMeasurementAdmission` (filter/NetworkMeasurementAdmission.hpp:397) | adapter | ctx [W] = `run.ctx`; config_ [R] = `run.config`; log [R] = `run.log`; traces [W] = `run.traces`; t [R] = `ep.se.time`; epoch_idx [V] = `run.engines.slip_qc.epoch_idx`; stations [W] = `run.setup.stations`; present [R] = `ep.pass_a.present`; modeled [W] = **pass_a.modeled** (out); carry_slipped_ambs [W] = **funnel.carry_slipped_ambs** (out); carry_slipped_raw [W] = **funnel.carry_slipped_raw** (out); carry_invariant [V] = `run.ctx.knobs.carry_invariant`; carry_warm [V] = `ep.clock.carry_warm` |
| 36 | `networkLevelAuthorityCensus` (filter/NetworkLevelAuthority.hpp:43) | direct | ctx [W] = `run.ctx`; config_ [R] = `run.config`; log [R] = `run.log`; traces [W] = `run.traces`; se [R] = `ep.se`; stations [R] = `run.setup.stations`; modeled [R] = `ep.pass_a.modeled`; present [R] = `ep.pass_a.present` |
| 37 | `networkAssemblyRegisterColumns_` (app/Application.hpp:852) | member | se [R] = `ep.se`; ctx [W] = `run.ctx`; setup [W] = `run.setup`; traces [W] = `run.traces`; est [W] = `run.engines.est`; present [R] = `ep.pass_a.present`; shared_phase_keys [R] = `ep.pass_a.shared_phase_keys`; registered_clock_sats [W] = **columns.registered_clock_sats** (out); registered_code_keys [W] = **columns.registered_code_keys** (out); col_desc [W] = **columns.col_desc** (out); n_clock [W] = **columns.n_clock** (out); n_pb [W] = **columns.n_pb** (out); n_cb [W] = **columns.n_cb** (out); n_present [W] = **columns.n_present** (out) |
| 38 | `networkSdEwlAr` (filter/NetworkSdEwlAr.hpp:269) | direct | ctx [W] = `run.ctx`; config_ [R] = `run.config`; log [R] = `run.log`; traces [W] = `run.traces`; setup [W] = `run.setup`; se [R] = `ep.se`; now_s [V] = `ep.clock.pl_now_s`; modeled [R] = `ep.pass_a.modeled`; present [R] = `ep.pass_a.present` |
| 39 | `networkRxCbDemeanPrepass` (app/NetworkRunContext.hpp:5256) | direct | ctx [W] = `run.ctx`; stations [R] = `run.setup.stations`; present [R] = `ep.pass_a.present`; pl_now_s [V] = `ep.clock.pl_now_s` |
| 40 | `networkOraclePinEpoch` (filter/NetworkOraclePin.hpp:306) | direct | ctx [W] = `run.ctx`; config_ [R] = `run.config`; stations [R] = `run.setup.stations`; t [R] = `ep.se.time`; log [R] = `run.log`; traces [W] = `run.traces` |
| 43 | `networkClockLevelRestoreRaise` (filter/NetworkClockLevel.hpp:343) | run | ctx [W] = `run.ctx`; log [R] = `run.log` |
| 44 | `networkHandoverStransformBeforeRows` (filter/NetworkHandoverStransform.hpp:291) | adapter | ctx [W] = `run.ctx`; config_ [R] = `run.config`; log [R] = `run.log`; traces [W] = `run.traces`; sat_health [R] = `run.setup.sat_health`; est [R] = `run.engines.est`; t [R] = `ep.se.time`; registered_clock_sats [R] = `ep.columns.registered_clock_sats` |
| 45 | `networkCarryDecide` (filter/NetworkCarryDisposition.hpp:117) | adapter | ctx [W] = `run.ctx`; est [R] = `run.engines.est`; t [R] = `ep.se.time`; dt [V] = `ep.clock.dt` |
| 46 | `networkCouplingExactPrepare` (app/NetworkRunContext.hpp:5215) | direct | ctx [W] = `run.ctx`; est [R] = `run.engines.est`; t [R] = `ep.se.time`; dt [V] = `ep.clock.dt`; log [R] = `run.log` |
| 47 | `networkPassB_` (app/Application.hpp:886) | member | se [R] = `ep.se`; ctx [W] = `run.ctx`; setup [W] = `run.setup`; traces [W] = `run.traces`; est [W] = `run.engines.est`; iono_deweight [R] = `run.engines.iono_deweight`; carry_slipped_ambs [R] = `ep.funnel.carry_slipped_ambs`; carry_slipped_raw [R] = `ep.funnel.carry_slipped_raw`; carry_warm [V] = `ep.clock.carry_warm`; pl_now_s [V] = `ep.clock.pl_now_s`; modeled [W] = **pass_a.modeled** (out); present [W] = **pass_a.present** (out); shared_phase_keys [R] = `ep.pass_a.shared_phase_keys`; registered_clock_sats [R] = `ep.columns.registered_clock_sats`; registered_code_keys [R] = `ep.columns.registered_code_keys`; n_clock [V] = `ep.columns.n_clock`; n_pb [V] = `ep.columns.n_pb`; n_present [V] = `ep.columns.n_present`; asm_res [W] = **assembly.asm_res** (out); est_index_of [W] = **assembly.est_index_of** (out); total_pins [W] = **assembly.total_pins** (out); sec_asm_ms_epoch [W] = **assembly.sec_asm_ms_epoch** (out); sec_asm_wall_ms [W] = `run.timing.asm_wall_ms` |
| 48 | `networkBuildEnsembleGauges` (filter/NetworkDdPriorBuild.hpp:50) | direct | est [W] = `run.engines.est`; stations [R] = `run.setup.stations`; est_index_of [R] = `ep.assembly.est_index_of`; asm_res [R] = `ep.assembly.asm_res`; shared_phase_keys [R] = `ep.pass_a.shared_phase_keys`; prev_carry [R] = `run.ctx.carry.prev_carry`; phase_datum_ensemble [V] = `run.ctx.knobs.phase_datum_ensemble`; phase_hold_on [V] = `run.ctx.knobs.phase_hold_on` |
| 50 | `networkBuildCarryDdPriors` (filter/NetworkDdPriorBuild.hpp:72) | direct | ctx [W] = `run.ctx`; config_ [R] = `run.config`; log [R] = `run.log`; traces [W] = `run.traces`; carry_driver [W] = `run.engines.carry_driver`; est [W] = `run.engines.est`; stations [R] = `run.setup.stations`; asm_res [R] = `ep.assembly.asm_res`; est_index_of [R] = `ep.assembly.est_index_of`; present [R] = `ep.pass_a.present`; shared_phase_keys [R] = `ep.pass_a.shared_phase_keys`; se [R] = `ep.se`; carry_arc_amb_key [W] = **dd_build.carry_arc_amb_key** (out); carry_arc_amb_key2 [W] = **dd_build.carry_arc_amb_key2** (out); carry_slipped_ambs [W] = **funnel.carry_slipped_ambs** (out); carry_slipped_raw [R] = `ep.funnel.carry_slipped_raw`; we_culprits [R] = `ep.ws.we_culprits`; n_threads [V] = `run.setup.n_threads`; lane_run [W] = `run.lanes`; isUncarriedThirdBand [F] = (removed: preamble lambda over networkIsUncarriedThirdBand) |
| 52 | `networkSolveGlue` (filter/NetworkSolveGlue.hpp:120) | direct | ctx [W] = `run.ctx`; config_ [R] = `run.config`; log [R] = `run.log`; traces [W] = `run.traces`; setup [W] = `run.setup`; est [W] = `run.engines.est`; ws [W] = **out `ws`** (the call passes `ep.ws`); se [R] = `ep.se`; asm_res [R] = `ep.assembly.asm_res`; est_index_of [W] = **out `est_index_of`** (the call passes `ep.assembly.est_index_of`); present [R] = `ep.pass_a.present`; shared_phase_keys [R] = `ep.pass_a.shared_phase_keys`; registered_clock_sats [R] = `ep.columns.registered_clock_sats`; registered_code_keys [R] = `ep.columns.registered_code_keys`; col_desc [R] = `ep.columns.col_desc`; n_clock [V] = `ep.columns.n_clock`; n_pb [V] = `ep.columns.n_pb`; n_cb [V] = `ep.columns.n_cb`; n_present [V] = `ep.columns.n_present`; total_pins [V] = `ep.assembly.total_pins`; carry_warm [V] = `ep.clock.carry_warm`; dt [V] = `ep.clock.dt`; pl_now_s [V] = `ep.clock.pl_now_s`; buildEnsembleGauges [F] = (removed: direct call in the T2 replay); buildCarryDdPriors [F] = (removed: direct call in the T2 replay) |
| 53 | `networkInstallCarry` (filter/NetworkInstallCarry.hpp:58) | direct | ctx [W] = `run.ctx`; config_ [R] = `run.config`; log [R] = `run.log`; traces [W] = `run.traces`; carry_driver [W] = `run.engines.carry_driver`; est [R] = `run.engines.est`; x_s [R] = `ep.solve.x_s`; plan [R] = `ep.solve.plan`; se [R] = `ep.se`; stations [R] = `run.setup.stations`; slip_qc [W] = `run.engines.slip_qc`; carry_arc_amb_key [R] = `ep.dd_build.carry_arc_amb_key`; carry_arc_amb_key2 [R] = `ep.dd_build.carry_arc_amb_key2`; carry_par [V] = `run.setup.carry_par`; n_threads [V] = `run.setup.n_threads`; seam_threads [V] = `run.setup.seam_threads`; lane_run [W] = `run.lanes` |
| 54 | `networkProducerAr` (filter/NetworkProducerAr.hpp:321) | direct | ctx [W] = `run.ctx`; config_ [R] = `run.config`; log [R] = `run.log`; traces [W] = `run.traces`; se [R] = `ep.se`; est [R] = `run.engines.est`; x_s [R] = `ep.solve.x_s`; stations [R] = `run.setup.stations`; est_index_of [R] = `ep.assembly.est_index_of`; asm_res [R] = `ep.assembly.asm_res`; plan [R] = `ep.solve.plan`; log_dir_ [R] = `run.log_dir` |
| 55 | `networkGrossLosScreen` (filter/NetworkGrossLos.hpp:550) | direct | ctx [W] = `run.ctx`; config_ [R] = `run.config`; log [R] = `run.log`; traces [W] = `run.traces`; se [R] = `ep.se`; stations [W] = `run.setup.stations`; est [R] = `run.engines.est`; x_s [R] = `ep.solve.x_s`; asm_res [R] = `ep.assembly.asm_res`; est_index_of [R] = `ep.assembly.est_index_of`; now_s [V] = `ep.clock.pl_now_s`; phase [V] = kept: `GrossLosPhase::Decide` |
| 56 | `networkFixedCycles` (filter/NetworkFixedCycles.hpp:494) | direct | ctx [W] = `run.ctx`; config_ [R] = `run.config`; log [R] = `run.log`; traces [W] = `run.traces`; se [R] = `ep.se`; est [R] = `run.engines.est`; x_s [R] = `ep.solve.x_s`; datum [R] = `ep.solve.plan.shared_datum`; fixes [R] = `ep.ar_fixes`; we [R] = `ep.solve.we_landed`; stations [R] = `run.setup.stations` |
| 57 | `networkSolPrep` (filter/NetworkSolPrep.hpp:125) | adapter | ctx [W] = `run.ctx`; config_ [R] = `run.config`; log [R] = `run.log`; traces [W] = `run.traces`; setup [W] = `run.setup`; se [R] = `ep.se`; est [R] = `run.engines.est`; x_s [R] = `ep.x_emit()`; plan [R] = `ep.solve.plan`; pl_now_s [V] = `ep.clock.pl_now_s`; datum_changed [V] = `ep.solve.datum_changed`; changed_consts [R] = `ep.solve.changed_consts`; sigma0_adapt [V] = `ep.solve.sigma0_adapt`; perconst_active [V] = `ep.solve.perconst_active`; perconst_publish [R] = `ep.solve.perconst_publish`; perconst_dec_withhold [R] = `ep.solve.perconst_dec_withhold`; perconst_stats [R] = `ep.solve.perconst_stats` |
| 58 | `networkPbHoldCloneUpdate` (filter/NetworkPbHoldClone.hpp:503) | adapter | ctx [W] = `run.ctx`; log [R] = `run.log`; est [R] = `run.engines.est`; x_s [R] = `ep.solve.x_s`; ws [R] = `ep.ws`; datum [R] = `ep.solve.plan.shared_datum` |
| 59 | `networkEmitStore` (filter/NetworkEmitStore.hpp:76) | direct | ctx [W] = `run.ctx`; config_ [R] = `run.config`; log [R] = `run.log`; traces [W] = `run.traces`; setup [W] = `run.setup`; se [R] = `ep.se`; est [R] = `run.engines.est`; x_s [R] = `ep.x_emit()`; pl_now_s [V] = `ep.clock.pl_now_s`; datum_changed [V] = `ep.solve.datum_changed`; sigma0_adapt [V] = `ep.solve.sigma0_adapt`; sol [W] = **out `sol`** (the call passes `ep.prep.sol`); epoch_withheld_consts [R] = `ep.prep.epoch_withheld_consts`; global_bump [V] = `ep.prep.global_bump`; serialized_consts [R] = `ep.prep.serialized_consts`; retro_emit_sats [R] = `ep.prep.retro_emit_sats`; emit_engines [W] = `run.emit`; present [R] = `ep.pass_a.present`; est_index_of [R] = `ep.assembly.est_index_of`; modeled [R] = `ep.pass_a.modeled`; shared_phase_keys [R] = `ep.pass_a.shared_phase_keys`; carry_slipped_ambs [R] = `ep.funnel.carry_slipped_ambs`; we_t2_opp [V] = `ep.solve.we_t2_opp`; we_t2_flag [V] = `ep.solve.we_t2_flag`; cl_tally [W] = `run.closed_loop` |
| 60 | `networkFastClockHarvest` (filter/NetworkFastClock.hpp:158) | direct | ctx [W] = `run.ctx`; config_ [R] = `run.config`; log [R] = `run.log`; setup [W] = `run.setup`; se [R] = `ep.se`; present [R] = `ep.pass_a.present`; modeled [R] = `ep.pass_a.modeled`; sol [R] = `ep.prep.sol`; fc_ppp_utils [R] = `run.engines.fc_ppp_utils` |
| 61 | `networkIonoDiag` (filter/NetworkIonoDiag.hpp:49) | direct | ctx [W] = `run.ctx`; config_ [R] = `run.config`; log [R] = `run.log`; traces [W] = `run.traces`; setup [W] = `run.setup`; ws [R] = `ep.ws`; se [R] = `ep.se`; est [W] = `run.engines.est`; x_s [R] = `ep.solve.x_s`; plan [R] = `ep.solve.plan`; asm_res [R] = `ep.assembly.asm_res`; est_index_of [R] = `ep.assembly.est_index_of`; modeled [R] = `ep.pass_a.modeled`; registered_clock_sats [R] = `ep.columns.registered_clock_sats` |
| 62 | `networkDdFrac` (filter/NetworkDdFrac.hpp:54) | direct | ctx [W] = `run.ctx`; config_ [R] = `run.config`; log [R] = `run.log`; traces [W] = `run.traces`; setup [W] = `run.setup`; se [R] = `ep.se`; est [W] = `run.engines.est`; x_s [R] = `ep.solve.x_s`; plan [R] = `ep.solve.plan`; asm_res [R] = `ep.assembly.asm_res`; est_index_of [R] = `ep.assembly.est_index_of` |
| 63 | `networkShDevVtec` (filter/NetworkShDevVtec.hpp:50) | direct | ctx [W] = `run.ctx`; config_ [R] = `run.config`; log [R] = `run.log`; traces [W] = `run.traces`; setup [W] = `run.setup`; se [R] = `ep.se`; est [W] = `run.engines.est`; x_s [R] = `ep.solve.x_s`; plan [R] = `ep.solve.plan`; asm_res [R] = `ep.assembly.asm_res`; est_index_of [R] = `ep.assembly.est_index_of`; modeled [R] = `ep.pass_a.modeled`; iono_deweight [W] = `run.engines.iono_deweight` |
| 64 | `networkShHealth` (filter/NetworkShHealth.hpp:50) | direct | ctx [W] = `run.ctx`; config_ [R] = `run.config`; log [R] = `run.log`; traces [W] = `run.traces`; setup [W] = `run.setup`; se [R] = `ep.se`; est [W] = `run.engines.est`; x_s [R] = `ep.solve.x_s`; asm_res [R] = `ep.assembly.asm_res`; est_index_of [R] = `ep.assembly.est_index_of`; modeled [R] = `ep.pass_a.modeled`; iono_deweight [W] = `run.engines.iono_deweight`; carry_vce_est [W] = `run.engines.carry_vce`; prires_est [W] = `run.engines.prires`; datum_changed [V] = `ep.solve.datum_changed`; pl_now_s [V] = `ep.clock.pl_now_s` |
| 65 | `networkIonoDwRelease` (filter/NetworkIonoDwRelease.hpp:239) | direct | ctx [W] = `run.ctx`; config_ [R] = `run.config`; log [R] = `run.log`; traces [W] = `run.traces`; setup [W] = `run.setup`; se [R] = `ep.se`; est [R] = `run.engines.est`; x_s [R] = `ep.solve.x_s`; est_index_of [R] = `ep.assembly.est_index_of`; modeled [R] = `ep.pass_a.modeled`; iono_deweight [R] = `run.engines.iono_deweight` |
| 66 | `networkLeveling` (filter/NetworkLeveling.hpp:48) | direct | ctx [W] = `run.ctx`; config_ [R] = `run.config`; log [R] = `run.log`; traces [W] = `run.traces`; setup [W] = `run.setup`; se [R] = `ep.se`; est [W] = `run.engines.est`; x_s [R] = `ep.solve.x_s`; plan [R] = `ep.solve.plan`; asm_res [R] = `ep.assembly.asm_res`; est_index_of [R] = `ep.assembly.est_index_of`; carry_vce_est [W] = `run.engines.carry_vce`; prires_est [W] = `run.engines.prires`; carry_warm [V] = `ep.clock.carry_warm`; pl_now_s [V] = `ep.clock.pl_now_s`; registered_clock_sats [R] = `ep.columns.registered_clock_sats`; registered_code_keys [R] = `ep.columns.registered_code_keys`; shared_phase_keys [R] = `ep.pass_a.shared_phase_keys` |
| 67 | `networkPostFitRaim` (filter/NetworkRaim.hpp:277) | direct | ctx [W] = `run.ctx`; config_ [R] = `run.config`; log [R] = `run.log`; traces [W] = `run.traces`; se [R] = `ep.se`; stations [W] = `run.setup.stations`; asm_res [R] = `ep.assembly.asm_res`; est_index_of [R] = `ep.assembly.est_index_of`; present [R] = `ep.pass_a.present`; x_s [R] = `ep.solve.x_s`; est [R] = `run.engines.est`; shared_datum [R] = `ep.solve.plan.shared_datum`; clock_refloat_sats [W] = **handoff.clock_refloat_sats** (out); retro_flag_sats [W] = **handoff.retro_flag_sats** (out); retro_feed [V] = `ep.solve.retro_feed`; n_arc_refloat_epoch [W] = **handoff.n_arc_refloat_epoch** (out); prires [W] = `run.engines.prires`; slip_ep [R*] = `&run.engines.slip_qc.epoch` |
| 68 | `networkGrossLosScreen` (filter/NetworkGrossLos.hpp:550) | direct | ctx [W] = `run.ctx`; config_ [R] = `run.config`; log [R] = `run.log`; traces [W] = `run.traces`; se [R] = `ep.se`; stations [W] = `run.setup.stations`; est [R] = `run.engines.est`; x_s [R] = `ep.solve.x_s`; asm_res [R] = `ep.assembly.asm_res`; est_index_of [R] = `ep.assembly.est_index_of`; now_s [V] = `ep.clock.pl_now_s`; phase [V] = kept: `ctx.knobs.gl_emit_exclude?GrossLosPhase::Act:GrossLosPhase::Full` |
| 69 | `networkClockQc` (filter/NetworkClockQc.hpp:98) | direct | ctx [W] = `run.ctx`; config_ [R] = `run.config`; log [R] = `run.log`; traces [W] = `run.traces`; ws [R] = `ep.ws`; se [R] = `ep.se`; est [R] = `run.engines.est`; x_s [R] = `ep.solve.x_s`; registered_clock_sats [R] = `ep.columns.registered_clock_sats`; datum_changed [V] = `ep.solve.datum_changed`; changed_consts [R] = `ep.solve.changed_consts`; clk_model_warm [V] = `ep.solve.clk_model_warm`; emit_rate_warm [V] = `ep.solve.emit_rate_warm`; pl_now_s [V] = `ep.clock.pl_now_s`; dt [V] = `ep.clock.dt`; clock_refloat_sats [W] = **handoff.clock_refloat_sats** (out); retro_flag_sats [W] = **handoff.retro_flag_sats** (out); retro_feed [V] = `ep.solve.retro_feed` |
| 70 | `networkPhaseVisibleMonitor` (filter/NetworkPhaseVisibleDiag.hpp:65) | adapter | ctx [W] = `run.ctx`; traces [W] = `run.traces`; se [R] = `ep.se`; est [R] = `run.engines.est`; x_s [R] = `ep.solve.x_s`; ws [R] = `ep.ws` |
| 71 | `networkSignalRosterAudit` (app/SignalRoster.hpp:1062) | direct | ctx [W] = `run.ctx`; config_ [R] = `run.config`; log [R] = `run.log`; traces [W] = `run.traces`; se [R] = `ep.se`; stations [R] = `run.setup.stations`; modeled [R] = `ep.pass_a.modeled`; present [R] = `ep.pass_a.present`; raw [R*] = `run.engines.slip_qc.epoch.computed?&run.engines.slip_qc.epoch:nullptr`; freed [R] = `ep.funnel.carry_slipped_ambs`; sol [R] = `ep.prep.sol`; emitted [V] = `ep.emitted` |
| 72 | `networkLedgerEmitFold` (filter/NetworkLedgerEmit.hpp:220) | adapter | ctx [W] = `run.ctx`; traces [W] = `run.traces`; se [R] = `ep.se` |
| 73 | `networkSecondPhaseEpochTail` (filter/NetworkSecondPhaseScope.hpp:143) | direct | ctx [W] = `run.ctx`; config_ [R] = `run.config`; stations [R] = `run.setup.stations` |
| 74 | `networkChannelAllEpoch` (filter/NetworkChannelAll.hpp:153) | direct | ctx [W] = `run.ctx`; config_ [R] = `run.config`; stations [R] = `run.setup.stations`; modeled [R] = `ep.pass_a.modeled`; present [R] = `ep.pass_a.present` |
| 75 | `networkMultiGnssEpoch` (filter/NetworkMultiGnss.hpp:129) | direct | ctx [W] = `run.ctx`; config_ [R] = `run.config`; stations [R] = `run.setup.stations`; modeled [R] = `ep.pass_a.modeled`; present [R] = `ep.pass_a.present` |
| 76 | `networkParamInventoryEpochTail` (filter/NetworkParamInventory.hpp:934) | direct | ctx [W] = `run.ctx`; log [R] = `run.log`; traces [W] = `run.traces`; nc [R] = `run.nc`; emitted [V] = `ep.emitted` |
| 77 | `networkContributionsEpochTail` (filter/NetworkContributions.hpp:559) | direct | ctx [W] = `run.ctx`; est [R] = `run.engines.est`; log [R] = `run.log`; traces [W] = `run.traces`; nc [R] = `run.nc`; emitted [V] = `ep.emitted` |
| 80 | `networkPostfitSigma0` (filter/NetworkFrameDiag.hpp:183) | direct | ctx [W] = `run.ctx`; config_ [R] = `run.config`; log [R] = `run.log`; traces [W] = `run.traces`; se [R] = `ep.se`; stations [R] = `run.setup.stations`; est [W] = `run.engines.est`; x_s [R] = `ep.solve.x_s`; asm_res [R] = `ep.assembly.asm_res`; est_index_of [R] = `ep.assembly.est_index_of`; modeled [R*] = `&ep.pass_a.modeled` |
| 81 | `networkShSpectrum` (filter/NetworkFrameDiag.hpp:88) | direct | ctx [W] = `run.ctx`; config_ [R] = `run.config`; log [R] = `run.log`; traces [W] = `run.traces`; se [R] = `ep.se`; est [W] = `run.engines.est` |
| 82 | `networkStationBlock` (filter/NetworkStationBlock.hpp:222) | direct | ctx [W] = `run.ctx`; config_ [R] = `run.config`; log [R] = `run.log`; traces [W] = `run.traces`; se [R] = `ep.se`; stations [R] = `run.setup.stations`; est [W] = `run.engines.est`; x_s [R] = `ep.solve.x_s`; asm_res [R] = `ep.assembly.asm_res`; est_index_of [R] = `ep.assembly.est_index_of` |
| 83 | `networkPrefitCensus` (filter/NetworkPrefitCensus.hpp:311) | adapter | ctx [W] = `run.ctx`; config_ [R] = `run.config`; log [R] = `run.log`; traces [W] = `run.traces`; se [R] = `ep.se`; stations [R] = `run.setup.stations`; est [R] = `run.engines.est`; asm_res [R] = `ep.assembly.asm_res`; est_index_of [R] = `ep.assembly.est_index_of`; iono_deweight [R*] = `&run.engines.iono_deweight` |
| 84 | `networkRedundancyCensus` (filter/NetworkRedundancyCensus.hpp:162) | direct | ctx [W] = `run.ctx`; config_ [R] = `run.config`; log [R] = `run.log`; traces [W] = `run.traces`; se [R] = `ep.se`; stations [R] = `run.setup.stations`; est [R] = `run.engines.est`; plan_datum [R] = `ep.solve.plan.shared_datum`; asm_res [R] = `ep.assembly.asm_res`; est_index_of [R] = `ep.assembly.est_index_of`; x_s [R] = `ep.solve.x_s` |
| 85 | `networkBlockBaardaEmit` (filter/NetworkBlockBaarda.hpp:687) | direct | ctx [W] = `run.ctx`; config_ [R] = `run.config`; log [R] = `run.log`; traces [W] = `run.traces`; se [R] = `ep.se`; stations [R] = `run.setup.stations` |
| 86 | `networkCongruenceEmit` (filter/NetworkCongruence.hpp:904) | direct | ctx [W] = `run.ctx`; config_ [R] = `run.config`; log [R] = `run.log`; traces [W] = `run.traces`; se [R] = `ep.se`; stations [R] = `run.setup.stations` |
| 87 | `networkMdbFlush` (filter/NetworkMdb.hpp:376) | direct | ctx [W] = `run.ctx`; config_ [R] = `run.config`; log [R] = `run.log`; traces [W] = `run.traces`; se [R] = `ep.se` |
| 88 | `networkDatumGraphCensus` (filter/NetworkDatumGraph.hpp:482) | direct | ctx [W] = `run.ctx`; config_ [R] = `run.config`; log [R] = `run.log`; traces [W] = `run.traces`; se [R] = `ep.se`; stations [R] = `run.setup.stations`; modeled [R] = `ep.pass_a.modeled`; est_index_of [R] = `ep.assembly.est_index_of`; plan [R] = `ep.solve.plan`; shared_phase_keys [R] = `ep.pass_a.shared_phase_keys`; registered_code_keys [R] = `ep.columns.registered_code_keys`; registered_clock_sats [R] = `ep.columns.registered_clock_sats`; datum_changed [V] = `ep.solve.datum_changed` |
| 90 | `networkRhsAttribution` (filter/NetworkRhsAttrib.hpp:763) | direct | ctx [W] = `run.ctx`; config_ [R] = `run.config`; log [R] = `run.log`; traces [R] = `run.traces`; se [R] = `ep.se`; stations [R] = `run.setup.stations`; est [W] = `run.engines.est`; plan_datum [R] = `ep.solve.plan.shared_datum`; asm_res [R] = `ep.assembly.asm_res`; est_index_of [R] = `ep.assembly.est_index_of`; x_s [R*] = `&ep.solve.x_s` |
| 91 | `networkRhsAttribTrip` (filter/NetworkRhsAttrib.hpp:807) | adapter | ctx [W] = `run.ctx`; config_ [R] = `run.config`; log [R] = `run.log`; traces [R] = `run.traces`; se [R] = `ep.se`; stations [R] = `run.setup.stations`; est [R] = `run.engines.est`; plan_datum [R] = `ep.solve.plan.shared_datum`; asm_res [R] = `ep.assembly.asm_res`; est_index_of [R] = `ep.assembly.est_index_of`; x_s [R*] = `&ep.solve.x_s` |
| 92 | `networkStashStatus` (filter/NetworkStashStatus.hpp:51) | direct | ctx [W] = `run.ctx`; config_ [R] = `run.config`; log [R] = `run.log`; traces [W] = `run.traces`; setup [W] = `run.setup`; se [R] = `ep.se`; est [W] = `run.engines.est`; x_s [R] = `ep.solve.x_s`; plan [R] = `ep.solve.plan`; carry_vce_est [W] = `run.engines.carry_vce`; prires_est [W] = `run.engines.prires`; clock_refloat_sats [R] = `ep.solve.handoff.clock_refloat_sats`; n_arc_refloat_epoch [V] = `ep.solve.handoff.n_arc_refloat_epoch`; registered_clock_sats [R] = `ep.columns.registered_clock_sats`; registered_code_keys [R] = `ep.columns.registered_code_keys`; retro_emit_sats [R] = `ep.prep.retro_emit_sats`; shared_phase_keys [R] = `ep.pass_a.shared_phase_keys`; sigma0_adapt [V] = `ep.solve.sigma0_adapt`; total_pins [V] = `ep.assembly.total_pins`; dt [V] = `ep.clock.dt`; datum_changed [V] = `ep.solve.datum_changed`; changed_consts [R] = `ep.solve.changed_consts`; n_clock [V] = `ep.columns.n_clock`; n_present [V] = `ep.columns.n_present`; n_pb [V] = `ep.columns.n_pb`; n_cb [V] = `ep.columns.n_cb`; n_dropped_sats [V] = `ep.pass_a.n_dropped_sats`; n_adapt [V] = `ep.solve.n_adapt` |
| 93 | `networkPbHoldCloneHarvest` (filter/NetworkPbHoldClone.hpp:528) | adapter | ctx [W] = `run.ctx`; est [R] = `run.engines.est`; x_s [R] = `ep.solve.x_s`; now_s [V] = `TimeUtils::continuousGpsSeconds(ep.se.time)` |
| 94 | `networkClockLevelRebase` (filter/NetworkClockLevel.hpp:328) | direct | ctx [W] = `run.ctx`; traces [W] = `run.traces`; t [R] = `ep.se.time`; est [R] = `run.engines.est`; x_s [R] = `ep.solve.x_s`; asm_res [R] = `ep.assembly.asm_res`; est_index_of [R] = `ep.assembly.est_index_of`; datum_changed [V] = `ep.solve.datum_changed` |
| 95 | `networkClockLevelMonitor` (filter/NetworkClockLevel.hpp:369) | direct | ctx [W] = `run.ctx`; traces [W] = `run.traces`; t [R] = `ep.se.time`; est [R] = `run.engines.est`; x_s [R] = `ep.solve.x_s`; asm_res [R] = `ep.assembly.asm_res`; est_index_of [R] = `ep.assembly.est_index_of`; datum_changed [V] = `ep.solve.datum_changed` |
| 96 | `networkAmbStateTrace` (filter/NetworkAmbStateTrace.hpp:169) | direct | ctx [W] = `run.ctx`; log [R] = `run.log`; traces [W] = `run.traces` |
| 99 | `networkWriteStateSnapshot` (app/NetworkRunContext.hpp:5114) | direct | ctx [W] = `run.ctx`; config_ [R] = `run.config`; t [R] = `ep.se.time`; pl_now_s [V] = `ep.clock.pl_now_s`; emitter [W] = `*run.setup.emitter`; carry_driver [R] = `run.engines.carry_driver`; stations [R] = `run.setup.stations`; log [R] = `run.log`; clk_rebase [R*] = `&run.emit.clk_rebase`; emit_rebase [R*] = `&run.emit.rebase`; pb_filter [R*] = `&run.emit.pb_filter`; consumer [W*] = `run.setup.consumer.get()` |

