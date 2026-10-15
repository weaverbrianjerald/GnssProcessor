/**
 * @file ApplicationNetwork.cpp
 * @brief Implementation of Application::runNetwork (minimum-constraint
 *        S-system PPP-RTK network producer).
 *
 * A joint multi-station estimator: per-station local normals are reduced onto a
 * shared satellite-parameter block by an exact Schur complement, which estimates
 * the estimable satellite clock and phase bias under an explicitly declared
 * minimum-constraint S-basis.  The products are emitted in IGS-SSR convention for
 * the companion PPP_RTK_User consumer.  The translation unit sits alongside
 * ApplicationBaseline.cpp and ApplicationRealtime.cpp.
 *
 * runNetwork is a thin orchestrator: the epoch loop is glue around calls into
 * single-concern seams, which hold the bodies relocated verbatim from the former
 * monolith.  New network code goes in the matching seam, not here; CLAUDE.md
 * carries the placement decision tree:
 *   Setup / run end     app/ApplicationNetworkSetup.cpp · filter/NetworkTeardown (the
 *                       run-end table, filter/NetworkRunEnd)
 *   Setup builders      one per concern, in that concern's file, called in order from
 *                       runNetwork (card NOM3 step EP4)
 *   Assembly (PASS A/B) app/ApplicationNetworkAssembly.cpp
 *   Datum / solve       filter/NetworkSbasis · NetworkChainDatum · NetworkPhaseDatum ·
 *                       NetworkSolveGlue · NetworkInstallCarry · NetworkDdPriorBuild ·
 *                       NetworkProducerAr
 *   Per-epoch QC        filter/NetworkRaim · NetworkClockQc · NetworkRelevel ·
 *                       slip/NetworkSlipQc · filter/NetworkCoupling
 *   Emit / store        filter/NetworkSolPrep · NetworkEmitStore
 *   Diag / stash        filter/NetworkIonoDiag · NetworkDdFrac · NetworkShDevVtec ·
 *                       NetworkShHealth · NetworkLeveling · NetworkStashStatus
 *   Context / logging   app/NetworkRunContext · logging/NetworkRunTraces
 *   Run-loop types      filter/NetworkRunTypes · NetworkWlRunTypes
 *
 * Each solution epoch runs the arrowhead estimator end to end:
 *
 *   PASS A (layout)  Per station, PppModel::computeModels, then
 *       MeasurementModel::ensureNetworkSharedStates pre-allocates the shared
 *       satellite states (per-satellite clock, per-(sat, signal) phase bias)
 *       behind the same usability gate the measurement build uses.  The datum
 *       (pivot) station goes first, so its usable phase-signal set restricts the
 *       phase-bias columns to those backed by a pivot-receiver ambiguity pin.  The
 *       shared column layout is then registered: a clock for every satellite any
 *       station sees, and a phase bias for each signal in the pivot's set.
 *
 *   PASS B (assembly)  MeasurementModel::computeMeasurementUpdate produces an
 *       undifferenced H / innovation / R block per station
 *       (DifferencingMode::None).  Its columns split three ways: satellite clock
 *       (−1) and phase bias (+1) scatter into the shared border A_s; the three
 *       receiver-position columns are dropped (coordinates are fixed); the rest
 *       form the station's private nuisance block A_r.  The pivot also folds
 *       tight-prior pins on its own ambiguities.  These pins are the local half of
 *       the S-basis, so the emitted phase bias carries that pivot-receiver
 *       ambiguity lump (Odijk Eq. 4).
 *
 *   Solve  NetworkSbasis builds the shared minimum-constraint datum (one
 *       satellite-clock pin per constellation); the null-space rank gate checks
 *       that the reduced shared system has exactly that deficiency; then
 *       NetworkEstimator::solve produces the shared estimate.
 *       NetworkProductEmitter serializes it into an owned SsrCorrectionStore, not
 *       the process-wide singleton the user side reads, which keeps the closed
 *       loop isolated.
 *
 *   Between epochs  the shared posterior (value, variance) carries forward as a
 *       diagonal prior on the next epoch's shared border, inflated by the
 *       per-parameter process noise over the gap.  With
 *       network.shared_carry_full_cov the carried prior is dense instead.
 *
 * Base scope: estimable satellite clock and phase bias, station coordinates and
 * orbits fixed, GPS and Galileo, per-band uncombined, float product.  Added since:
 * satellite code bias (network.estimate_sat_code_bias), post-fit network QC (RAIM,
 * satellite-clock QC), a streaming epoch source (network.streaming.enable) and a
 * log-only producer-side AR (network.ambiguity_resolution) that leaves the product
 * float.  Atmosphere products and the GLONASS FCN bias are not produced; GLONASS
 * (FDMA) is excluded.  Every station's coordinates must be known and its observation
 * RINEX path must be set explicitly in the config: the PathResolver network gate is
 * deferred, so resolveData() here serves only products and broadcast navigation.
 */

#include "app/Application.hpp"
#include "app/NetworkRun.hpp"     // NetworkRun, networkEngineSpec (card NOM3 step EP1)

#include "rinex/MultiRinexSync.hpp"
#include "products/ProductCache.hpp"
#include "products/SsrCorrectionStore.hpp"
#include "products/NetworkProductEmitter.hpp"
#include "products/NetworkEmitRebase.hpp"   // emit-layer frame re-base
#include "products/NetworkClockEmitRebase.hpp" // clock emission re-base + per-sat step marks
#include "products/NetworkProductIO.hpp"
#include "products/CombinedCorrectionSigma.hpp"   // combined-correction σ arithmetic
#include "products/NetworkStateSnapshot.hpp"   // atomic state snapshot + warm resume
#include "products/LiveEphemerisStore.hpp"     // broadcast a-priori store for brdc_rts
#include "products/RinexNavParser.hpp"         // RINEX-nav → LiveEphemerisStore loader (card 5, RTF-1)
#include "filter/NetworkEstimator.hpp"
#include "filter/NetworkClockLevel.hpp"         // satellite-clock level monitor and restore raise (card CTU)
#include "filter/IlsCore.hpp"                 // producer-side network AR (Phase 2+): ils::ilsFix
#include "filter/NetworkSbasis.hpp"
#include "filter/NetworkPivotHandover.hpp"   // pivot-handover datum-change detector (A1/A2)
#include "filter/NetworkCarryVce.hpp"        // LS-VCE variance-component monitor for carry rows (M2)
#include "filter/NetworkPriorResidual.hpp"   // prior-residual census (SPEC_prior_residual_census)
#include "filter/NetworkInvariantCarryDriver.hpp"  // DD cycle-basis invariant carry (card 4, M1 C3b)
#include "filter/StateKeySchema.hpp"
#include "common/GnssIdentity.hpp"         // systemOfSat, letterOfSat, rinexLetter, systemName
#include "filter/NetworkRunTypes.hpp"      // run-loop record types: NetStation, StationAssembly, carry PODs (INC-2)
#include "filter/NetworkWlRunTypes.hpp" // RAII and section-timer types hoisted out of runNetwork() (INC-9b)
#include "filter/NetworkRunGroups.hpp"  // the driver-owned run groups: engines, emit engines, timing, closed-loop tally (card NOM TD1a)
#include "filter/NetworkRobustStats.hpp"   // median / MAD helpers for robust attribution (INC-2)
#include "filter/NetworkPhaseDatum.hpp"    // census, anchor election, drop-and-heal (INC-8b)
#include "filter/NetworkAmbStateTrace.hpp" // per-arc carry-state trace: drain and erase capture (RF-1 §7.4)
#include "filter/NetworkAmbFloat.hpp"      // floored birth prior and bounded drop (RF-1 step 4)
#include "filter/NetworkHandoverStransform.hpp" // kStsExactTolM (RF-2b: the declaration step test)
#include "filter/NetworkChainDatum.hpp"    // eq34 single-receiver-chain datum (card 3, INC-8b)
#include "filter/NetworkAnchorHygiene.hpp" // eq34 reattach hygiene: anchor-host eligibility + pin bridge
#include "filter/NetworkSignalIntegrity.hpp" // flicker/gap: band-state classifier + unvetted phase demotion
#include "filter/NetworkMeasurementAdmission.hpp" // product-referenced admission: the corrupt-channel screen
#include "filter/NetworkGrossLos.hpp"    // gross line-of-sight screen (card GL, §24.18)
#include "filter/NetworkRaim.hpp"        // post-fit network RAIM (INC-9)
#include "filter/NetworkClockQc.hpp"     // post-solve satellite-clock QC (INC-10)
#include "filter/NetworkFrameDiag.hpp"   // frame-guard trigger instrumentation (P1-3/P1-4, SPEC_day129_frame_guards)
#include "filter/NetworkStationBlock.hpp"  // per-station local-block audit, measure-only (card ZP, §24.10)
#include "filter/NetworkRhsAttrib.hpp"   // reduced-RHS class attribution (era-2 injector probe)
#include "filter/NetworkMdb.hpp"               // Baarda reliability numbers / MDB (SPEC_redundancy_census)
#include "filter/NetworkRedundancyCensus.hpp"  // redundancy / testability census (SPEC_redundancy_census)
#include "filter/NetworkPrefitCensus.hpp"      // prefit census / fuse / birth check / topology (SPEC_prefit_census_observability)
#include "filter/NetworkDatumGraph.hpp"        // structural S-basis dimension census (SPEC_datum_graph_census)
#include "filter/NetworkSolPrep.hpp"    // emit-side solution prep and discontinuity signalling (INC-11pre)
#include "filter/NetworkEmitStore.hpp"    // emit, store, product write and closed loop (INC-11)
#include "filter/NetworkTeardown.hpp"
#include "filter/NetworkPhaseVisibleDiag.hpp"  // phase-visible consistency instrument (card PVN)
#include "filter/NetworkPbHoldClone.hpp"       // the smoothed hold's clock clones (card SMH)
#include "filter/NetworkCarryDisposition.hpp"  // the decided-once fold disposition (card PRM phase 1b)
#include "filter/NetworkProducerAr.hpp" // log-only producer AR and NEQ export (INC-9b)
#include "filter/NetworkFixedCycles.hpp" // the fixed-cycle fold and its ledger of fixed cycles: a detached held solve only the emission reads (card FCF)
#include "filter/NetworkInject.hpp"
#include "filter/NetworkCanonicalPairAdmission.hpp" // canonical-pair admission seam (card CPA)
#include "app/SignalRoster.hpp"  // signal roster and arc-ledger observatory (card SID phase 1)
#include "filter/NetworkSecondPhaseScope.hpp"  // second phase channel, epoch tail (card SID phase 6a)
#include "filter/NetworkChannelAll.hpp"      // all-channel census (card SID phase 6b)
#include "filter/NetworkLedgerEmit.hpp"  // emitter read against the ledger, declared roster (card SID phase 5a)
#include "filter/NetworkInstallCarry.hpp" // installPosterior and the WL zone-4 fold / inject (INC-9b)
#include "filter/NetworkDdPriorBuild.hpp" // buildCarryDdPriors free function (INC-9b)
#include "filter/NetworkSolveGlue.hpp" // datum, solve and within-epoch RAIM (INC-9b)
#include "filter/NetworkIonoDiag.hpp"    // iono / ZWD / anchor-info viz traces (INC-11b)
#include "filter/NetworkDdFrac.hpp"    // cross-station DD fractional-residual trace (INC-11b)
#include "filter/NetworkSdEwlAr.hpp"   // SD EWL Route-B integer decision (increment 2a)
#include "filter/NetworkShDevVtec.hpp"    // SH-deviation carry KF and VTEC-coefficient trace (INC-11b)
#include "filter/NetworkShHealth.hpp"    // SH-field health probe, carry LS-VCE (INC-11b)
#include "filter/NetworkLevelAuthority.hpp"  // per-epoch level-authority census (LV-6)
#include "filter/NetworkLeveling.hpp"    // phase-leveling carry harvest, D2/C1 stash (INC-11b)
#include "filter/NetworkStashStatus.hpp"    // carry re-stash, LS-VCE fold, epoch-status QC row (INC-11b)
#include "logging/CsvRotator.hpp"     // size-based rotation of the unbounded service CSVs (P1)
#include "logging/NetworkRunTraces.hpp" // trace-stream ownership, open logic, roster, FlagRate (INC-3)
#include "app/NetworkRunContext.hpp"    // knobs, tier-1 carry state and service counters, as ctx (INC-4)
#include "app/NetworkSetupResult.hpp"   // setup-phase handoff (setup) and navAvailabilityGpsSeconds (INC-5)
#include "slip/MwSlipDetector.hpp"      // phase-leveling slip repair (T0, increment 2)
#include "slip/TurboEditResolver.hpp"   // MW+GF per-signal integer repair (Increment 2)
#include "slip/IfPhaseSlipDetector.hpp" // IF-phase between-sat/between-epoch slip detection
#include "slip/NetworkSlipQc.hpp"       // pure slip-QC helpers: ifElevMap, parseIfPairResetKey (INC-2)
#include "slip/NetworkSlipOracle.hpp"   // simulation-only oracle slip response (level-preserving ceiling)
#include "atmosphere/SphericalHarmonicIonosphere.hpp"
#include "common/Constants.hpp"
#include "common/PppUtils.hpp"        // the slip-QC wavelength provider (GLONASS slots)
#include "realtime/RealTimeEngine.hpp"
#include "realtime/NetworkStreamFrontEnd.hpp"  // multi-station streaming epoch source (RTF-3)
#include "math/TimeUtils.hpp"
#include "math/Coordinates.hpp"

#include <Eigen/Dense>
#include <Eigen/Core>          // Eigen::setNbThreads / nbThreads (Eigen runs single-threaded inside the per-station OpenMP region, which avoids nested oversubscription and keeps the result deterministic)
#include <fmt/format.h>

#ifdef _OPENMP
#include <omp.h>               // per-station PASS A/B parallelism (gated on station count); pragmas degrade to no-ops when the toolchain lacks /openmp
#endif

#include <algorithm>
#include <cmath>
#include <chrono>
#include <ctime>
#include <deque>
#include <random>
#include <limits>
#include <fstream>
#include <map>
#include <memory>
#include <numeric>             // std::iota (netar ILS-cap subset ordering)
#include <optional>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <tuple>
#include <vector>
#include "filter/NetworkCongruenceSetup.hpp"
#include "filter/NetworkOraclePinSetup.hpp"

namespace GnssProcessor {

int Application::runNetwork() {
    auto log = log_manager_.getLogger("main_log");
    const auto& nc = config_.getNetworkConfig();

    // Consumer-only replay (Q5): an already-written product file replaces the
    // producer entirely.  The branch comes before input validation and setup, so
    // none of the producer machinery (roster, station build, emitter, snapshot
    // fingerprint) is touched and a replay run cannot perturb a producer artifact.
    if (!nc.closed_loop.replay_products_path.empty())
        return runConsumerReplay();

    log->info("PPP-RTK Network producer (V1): {} stations, clock datum station '{}'{}",
              nc.stations.size(), nc.clock_datum_station,
              nc.clock_datum_satellite.empty()
                  ? std::string{}
                  : (" + datum sat '" + nc.clock_datum_satellite + "'"));

    // --- Phase 0: input validation (in ApplicationNetworkSetup.cpp) ------------
    if (const int rc = networkValidateInputs_(); rc != 0) return rc;

    // --- Phase 1: resolve products + broadcast nav --------------------------
    // Station RINEX is explicit, so resolveData() here only fills product /
    // navigation paths (orbits + clocks needed for per-station geometry).  A
    // failure is non-fatal: the product-cache load below surfaces a precise
    // "no orbits" diagnostic if products are genuinely missing.
    if (!resolveData()) {
        log->warn("Network: resolveData() failed; relying on explicit product / "
                  "navigation paths in the config (product-cache load will report "
                  "if orbits are missing).");
    }

    // --- Phases 2-4, sinks, contract and consumer (in ApplicationNetworkSetup.cpp)
    // `setup` stays at one address for the whole run: MultiRinexSync references
    // setup.roster, and the closed-loop engine holds &setup.network_store plus a
    // GnssConfig& to setup.consumer_config (see NetworkSetupResult.hpp).
    NetworkSetupResult setup;
    if (const int rc = networkSetup_(setup); rc != 0) return rc;
    // Alias block (INC-3/4/5 migration mechanic): the run loop reads the setup
    // outputs under these local names.
    MultiRinexSync& sync = *setup.sync;
    const auto& synced = sync.getSyncedEpochs();
    const bool closed_loop_on = setup.closed_loop_on;
    auto& sh_model = setup.sh_model;
    const GlobalVtecShConfig& shc = config_.getGlobalVtecShConfig();
    const bool parallel_on = setup.parallel_on;
    auto& consumer = setup.consumer;


    // ------------------------------------------------------------------
    // The network run context (INC-4).  ctx.knobs holds the const config
    // snapshots (NetworkKnobs::snapshot keeps the original getter read order);
    // ctx.carry holds the tier-1 cross-epoch run state (a superset of the RTB-7
    // snapshot); ctx.counters holds the service counters and anchors.  The alias
    // blocks below keep the downstream use-sites under their former local names
    // (the INC-3 migration mechanic).
    NetworkRunContext ctx(config_, nc);
    // The marker -> ARP census of networkSetup_, into the counters the run end reads (card UPO).
    networkMarkerArpCensusCopy(ctx, config_, setup);

    // --- Trace ----------------------------------------------------------------
    // Stream and path ownership and the startup open logic live in
    // logging/NetworkRunTraces; each stream's documentation (column semantics,
    // arming toggle, P1 rotation policy) is on its member in that header.  The
    // per-epoch writers live in the seams, which receive `traces`; the reference
    // below keeps the former local name for the use left in this file.
    NetworkRunTraces traces(log_dir_);
    // The run (app/NetworkRun.hpp, card NOM3 step EP1): the references every epoch reads
    // and the engines and groups the run owns, each built here in place from the engines'
    // constructor arguments (networkEngineSpec, a pure function of the configuration and
    // the knobs).  The builders below configure the owned engines through references.
    const NetworkEngineSpec engine_spec = networkEngineSpec(config_, nc, ctx);
    NetworkRun run(ctx, config_, nc, log, traces, setup, log_dir_, engine_spec);
    // Setup builders (card NOM3 step EP4): each in its concern's file, filling the run member
    // its name says, called in this order (scripts/nom3_designs/EP4_design.md section 2 states
    // the constraints).  The two simulation oracles throw on a bad fixture before any trace
    // file opens; the wavelength provider follows openAll, which opens one of its consumers.
    networkSlipQcConfigure(run);
    networkSlipOracleArm(run, log_manager_);
    networkOraclePinArm(run, log_manager_);
    traces.openAll(config_, nc, shc, sh_model.get(), closed_loop_on && consumer, log);
    networkSlipQcArmWavelengthProvider(run, log_manager_);
    // The census arms (the carry-VCE apply step, the prior-residual corroborator, the
    // block-Baarda and congruence censuses).
    networkCarryVceApplyArm(run);
    networkPriorResidualArm(run);
    networkBlockBaardaArm(run);
    networkCongruenceArm(run);

    networkRunManifestStartup(run, config_path_);

    // Eigen runs single-threaded for the duration of the (parallelized) epoch loop.
    // _OPENMP is defined project-wide, so Eigen threads its own dense kernels; nesting
    // those inside the per-station OpenMP region would oversubscribe threads and make the
    // reduction order of the per-station matrix products non-deterministic.
    // Single-threaded Eigen inside each station thread is thread-safe (separate
    // matrices) and deterministic.  The setting is restored after the loop, and it is
    // changed only when parallelism is active, so the serial path (and the small-network
    // case) is byte-identical.
    const int eigen_threads_saved = Eigen::nbThreads();
    if (parallel_on) Eigen::setNbThreads(1);

    // [TIMING] Wall clock of the per-epoch loop alone (excluding startup: product
    // cache load and per-station RINEX parse).  It isolates the per-epoch compute that
    // scales with station count, which is the scaling metric, and counts only epochs
    // that entered processing (past the max-duration cutoff).  The run's timing record
    // (filter/NetworkRunGroups.hpp) also holds the D0 assembly wall, the profiled-epoch
    // count and the D0b sub-block totals below; the run end reports all of them.
    NetworkRunTiming& timing = run.timing;
    timing.loop_begin = std::chrono::steady_clock::now();

    networkEstimatorConfigure(run, engine_spec);

    networkSectionTimersArm(run);
    networkRunArm(run, log_manager_);

    networkCbAdmissionCensus(run);

    // === One synced epoch through the full producer =========================
    // Application::networkProcessEpoch_ (ApplicationNetworkEpoch.cpp, card NOM3 step EP1)
    // runs one synced epoch over the run built above; the epoch source below calls it.

    // === Epoch source =======================================================
    // Batch (default): iterates the pre-built synced vector.  Streaming
    // (network.streaming.enable, RTF-3): per-station live lanes → wait-budget aligner →
    // bounded epoch queue, popped here on the solve thread.  Only the iteration differs:
    // same per-epoch body, same order, same teardown.
    if (ctx.knobs.streaming_on) {
        auto frontend = std::make_unique<NetworkStreamFrontEnd>(config_, log_manager_);
        if (!frontend->start()) {
            if (parallel_on) Eigen::setNbThreads(eigen_threads_saved);
            log->error("Network (RTF-3): streaming front-end failed to start — no "
                       "epoch source.  Aborting the run (the batch path is not a "
                       "fallback here: an armed streaming run silently degrading to "
                       "stale files would be the quiet-failure mode).");
            return 1;
        }
        SyncedEpoch se_live;
        while (frontend->next(se_live)) {
            if (networkProcessEpoch_(run, se_live) == LoopSignal::Stop) break;
        }
        frontend->stop();
        frontend->logRunEndSummary(log);
    } else {
        for (const auto& se : synced)
            if (networkProcessEpoch_(run, se) == LoopSignal::Stop) break;
    }

    if (parallel_on) Eigen::setNbThreads(eigen_threads_saved);  // restore Eigen's own threading

    // The run end reads the run's groups through one const bundle (card NOM step TD1a).
    return networkTeardown(ctx, config_, log, traces, setup,
                           NetworkRunEndInputs{ run.engines, run.emit, run.timing, run.closed_loop });
}

} // namespace GnssProcessor
