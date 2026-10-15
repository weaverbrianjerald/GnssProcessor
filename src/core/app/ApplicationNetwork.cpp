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
    auto& stations = setup.stations;
    const bool parallel_on = setup.parallel_on;
    const int n_threads = setup.n_threads;
    const int seam_threads = setup.seam_threads;
    const bool carry_par = setup.carry_par;
    NetworkProductEmitter& emitter = *setup.emitter;
    auto& consumer = setup.consumer;


    // ------------------------------------------------------------------
    // The network run context (INC-4).  ctx.knobs holds the const config
    // snapshots (NetworkKnobs::snapshot keeps the original getter read order);
    // ctx.carry holds the tier-1 cross-epoch run state (a superset of the RTB-7
    // snapshot); ctx.counters holds the service counters and anchors.  The alias
    // blocks below keep the downstream use-sites under their former local names
    // (the INC-3 migration mechanic).
    NetworkRunContext ctx(config_, nc);
    // The marker → ARP roster census is measured in networkSetup_, which runs
    // before this context exists.  It is copied into the counters because the
    // run-end readout and the run manifest read ctx.counters, and both report it
    // in either knob state (card UPO).
    ctx.counters.marker_arp_armed         = config_.networkStationMarkerToArp();
    ctx.counters.n_marker_arp_ecc         = setup.n_marker_arp_ecc;
    ctx.counters.n_marker_arp_reduced     = setup.n_marker_arp_reduced;
    ctx.counters.n_marker_arp_over_1cm    = setup.n_marker_arp_over_1cm;
    ctx.counters.n_marker_arp_no_record   = setup.n_marker_arp_no_record;
    ctx.counters.marker_arp_max_h         = setup.marker_arp_max_h;
    ctx.counters.marker_arp_max_h_station = setup.marker_arp_max_h_station;

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
    // the knobs).  The setup below configures the owned engines through references.
    const NetworkEngineSpec engine_spec = networkEngineSpec(config_, nc, ctx);
    NetworkRun run(ctx, config_, nc, log, traces, setup, log_dir_, engine_spec);
    auto& slip_raw_trace       = traces.slip_raw_trace;
    // Slip-QC hardening channel: knobs +
    // per-station cross-epoch state + this epoch's raw evidence + run-end
    // counters, threaded by reference into the PASS-A slip seam and the
    // assembly pre-filter.  Sized once here so the per-station EWMAs survive
    // the whole run.
    SlipQcChannel& slip_qc = run.engines.slip_qc;
    {
        const auto& sq = nc.slip_qc;
        auto& sp = slip_qc.params;
        sp.lli_corroborate     = sq.lli_corroborate;
        sp.chatter_frac_hi     = sq.lli_chatter_frac_hi;
        sp.chatter_frac_lo     = sq.lli_chatter_frac_lo;
        sp.chatter_min_epochs  = sq.lli_min_epochs;
        sp.chatter_alpha       = sq.lli_ewma_alpha;
        sp.k_mw                = sq.lli_k_mw;
        sp.k_gf                = sq.lli_k_gf;
        sp.mw_floor_cyc        = sq.lli_mw_floor_cyc;
        sp.gf_floor_m          = sq.lli_gf_floor_m;
        sp.step_alpha          = sq.lli_step_alpha;
        sp.gap_instrument      = sq.gap_instrument;
        // The gap re-entry response consumes the same GapResumption records as
        // the instrument, so it arms the resumption collection on its own: the
        // response works without the diagnostic armed, and turning the diagnostic
        // off does not disarm it (card G, §G3/W1).
        sp.gap_reentry             = sq.gap_reentry;
        sp.gap_reentry_lli         = sq.gap_reentry_lli;
        sp.gap_reentry_unmeasurable = sq.gap_reentry_unmeasurable;
        // Per-signal rider of the same response (card GRS).  It also arms the
        // per-signal half of the compute and qualify passes, so a run with it off
        // never builds or reads a per-signal list.
        sp.gap_reentry_per_signal  = sq.gap_reentry_per_signal;
        sp.gap_reentry_per_signal_unmeasurable = sq.gap_reentry_per_signal_unmeasurable;
        // The co-resumed share of the per-signal tier: a verdict on one returning
        // signal of a line of sight frees its co-resumed siblings.  It reads the
        // rider's list and arms nothing of its own.
        sp.gap_reentry_coresumed_share = sq.gap_reentry_coresumed_share;
        // The arc-level tracker takes its per-band signal from the model, so the
        // tracker, the carried ambiguity key and the gap re-entry response's
        // lookup name the same signal.  Read at the compute pass only (card GRK).
        sp.tracker_modeled_first   = sq.tracker_modeled_first;
        // With the ledger re-entry armed, the response's population is the arc
        // ledger's admitted lifecycle, and the raw tiers become the verdict source,
        // looked up per arc.  The loader already refuses the knob without the roster, the roster
        // read and the row-level response, so no interlock is re-tested here.
        // Unarmed, both drains stay on their raw-driven path (card SID phase 3).
        sp.ledger_reentry          = config_.networkLedgerReentry();
        sp.ledger_reentry_refloat  =
            config_.networkLedgerReentryNoVerdict() == LedgerReentryNoVerdict::Refloat;
        // The GF-row capture needs the qualified raw verdicts (bridge v3 E4).
        sp.bridge_gf           = config_.networkCycleSlipRelevelBridgeGf();
        // The CS-GF authority has the same need.  The LV-6 census arms its advance:
        // the kernel is instrumentation (SPEC_level_authority_internal §3).  Arming
        // iono_cs flips cardsArmed(), so census A/B pairs must pin
        // cycle_slip.gap_instrument in both arms.
        sp.iono_cs             = config_.networkLevelAuthorityCsgf();
        // The arc-scoped GF jump trigger (card ARL) reads the raw rows and keeps its own
        // per-arc steps, so it arms the compute pass (anyConsumer) and not the qualify
        // pass.
        sp.gf_jump.armed       = sq.gf_arc_jump;
        sp.gf_jump.floor_m     = sq.gf_arc_jump_m;
        sp.gf_jump.k           = sq.gf_arc_jump_k;
        sp.gf_jump.window      = sq.gf_arc_jump_window;
        sp.gf_jump.min_count   = sq.gf_arc_jump_min_count;
        slip_qc.hist.resize(stations.size());
    }
    // Oracle slip response, simulation only (network.cycle_slip.oracle_relevel,
    // include/slip/NetworkSlipOracle.hpp): the level-preserving re-level ceiling.
    // It is constructed only when armed; every drain takes a NetworkSlipOracle*
    // that stays null otherwise, so the off path is structurally byte-identical.
    // The load throws on a missing or mis-pointed fixture, which is what keeps
    // the oracle off field data.  The throw propagates to runNetwork's caller like
    // any other fatal setup error; a silent zero-repair run instead would be read
    // as a falsification of the mechanism under test.
    if (nc.slip_qc.oracle_relevel) {
        ctx.carry.slip_oracle =
            networkSlipOracleLoad(config_, stations, log_manager_, log);
        ctx.carry.slip_oracle->carry = &ctx.carry.local_carry;
    }
    // Oracle ambiguity pin, simulation only (`network.inject.events[] kind
    // "oracle_pin"`, include/filter/NetworkOraclePin.hpp, docs/SPEC_oracle_pin.md):
    // holds the float ambiguities in scope at the fixture's true integers so the
    // remaining error can be located.  It is constructed only when such an event
    // is armed; every call site tests the pointer, so the off path is structurally
    // byte-identical.  As for the slip oracle above, the load throws on a missing
    // or mis-pointed fixture: a silently degraded pin would report the model's
    // error budget without the ambiguities held, which would be read as the
    // ceiling the pin exists to measure.
    {
        bool any_pin = false;
        if (nc.inject.enable)
            for (const auto& e : nc.inject.events)
                if (e.kind == "oracle_pin") any_pin = true;
        if (any_pin)
            ctx.carry.oracle_pin =
                networkOraclePinLoad(config_, stations, log_manager_, log);
    }
    traces.openAll(config_, nc, shc, sh_model.get(), closed_loop_on && consumer, log);
    // Wavelength provider for the raw-slip path, gated on the streams openAll just
    // opened.  It is built here because it needs log_manager_, which the traces
    // unit does not hold.  The raw-slip provider serves three consumers (the trace, card L and card G) and is built when any
    // of them is armed, per spec §1: "compute once per epoch when any consumer
    // needs it".  With none armed it stays null and the raw-observable path is
    // skipped.
    if (slip_qc.params.anyConsumer(static_cast<bool>(slip_raw_trace)))
        slip_qc.utils = std::make_unique<PppUtils>(log_manager_);



    // === Epoch loop =========================================================
    // The carry LS-VCE monitor is an engine the run owns (run.engines.carry_vce; a
    // run-TU local until card NOM3 step EP1).  Its hourly trace flush is scheduled by
    // ctx.carry.first_vce_s and ctx.carry.last_vce_hour, which networkShHealth writes.
    // Apply step (M2 v2): arms the LASER 5-95 variance-factor iteration over that
    // monitor.  Unlike the monitor's estimator this is not engine state:
    // α² is cross-epoch state read at three fold sites in three translation units
    // (the PASS-B ambiguity injector, the D2 shared injector, and the two harvest
    // mirrors), so it lives in ctx.carry per the decomposition rule and every seam
    // reaches it through ctx.  Unless armed it is inert (unity factor, no state
    // written), so a run with it off is byte-identical.
    if (ctx.knobs.vce_apply_on)
        ctx.carry.vce_apply.configure(
            NetworkCarryVce::parseComponents(config_.networkCarryVceApplyFamilies()),
            config_.networkCarryVceApplySlew(), config_.networkCarryVceApplyMax(),
            config_.networkCarryVceApplyMinRows());
    // Prior-residual census.  An engine the run owns, like the carry-VCE monitor
    // (run.engines.prires and run.engines.carry_vce, app/NetworkRun.hpp; both were
    // run-TU locals until card NOM3 step EP1), read by the same four seams: SB_T1A
    // advances it, SB_LEVELING feeds pl/zwd/rxcb, SB_STASH_STATUS feeds pb/cb and
    // drains it, and the teardown reads it.  It shares the M2 monitor's per-row
    // arithmetic and does not affect the solve: with the census armed the
    // solution is byte-identical.
    NetworkPriorResidual& prires_est = run.engines.prires;
    // Corroboration resolver.  It is injected here rather than plumbed through five
    // seam signatures, and it captures slip_qc by reference because the evidence
    // it reads is refreshed per epoch inside that channel.
    //
    // The resolver gathers domain counts only; the verdict rule is @ref
    // priorResidualConsensus, a pure function with its own unit cases.  The split
    // makes the roster-independence claim checkable without a network.  A plain OR
    // over a domain saturates as the roster grows, because P(any of N steps) -> 1;
    // on a 303-station roster it moved `cb` rows reading raw_stepped from 29 % to
    // 70 % on identical data.
    //
    // A carried prior's domain is the set of (station, satellite) pairs whose raw
    // MW/GF observables can witness a discontinuity in it:
    //   * `pl`          -> one pair (station, sat): the arc's own verdict;
    //   * `zwd`, `rxcb` -> receiver level: that station's satellites;
    //   * `pb`, `cb`    -> satellite level: the stations that measured that sat.
    //
    // Both multi-member domains are aggregated once per epoch into the maps below
    // rather than re-scanned per row.  Otherwise a shared-family row would walk
    // every station map (131 cb rows x 303 stations = 40k lookups per epoch),
    // making the census quadratic in the roster.  Building the maps is one pass
    // over the verdict table, and the per-row lookup is then O(log n).
    //
    // The cache is keyed on slip_qc.epoch_idx, the monotonic processed-epoch
    // index, so it invalidates itself exactly when the evidence is refreshed.  All
    // five feed sites run serially after the solve, so the mutable capture is
    // race-free by construction (the parallel produce phase never reaches it).
    if (ctx.knobs.prires_corrob) {
        struct CorrobCache {
            long long epoch = -1;
            std::map<std::string, PriorResidualEvidence> by_sat;   // satellite-level domains
            std::vector<PriorResidualEvidence>           by_sta;   // receiver-level domains
        };
        auto cache = std::make_shared<CorrobCache>();
        prires_est.setCorroborator(
            [&slip_qc, cache](std::string_view /*family*/, int station,
                              std::string_view sat) -> PriorResidualEvidence {
                const RawSlipEpoch& ep = slip_qc.epoch;
                if (!ep.computed) return PriorResidualEvidence{};
                if (cache->epoch != slip_qc.epoch_idx) {
                    cache->epoch = slip_qc.epoch_idx;
                    cache->by_sat.clear();
                    cache->by_sta.assign(ep.verdict.size(), PriorResidualEvidence{});
                    for (std::size_t i = 0; i < ep.verdict.size(); ++i)
                        for (const auto& kv : ep.verdict[i]) {
                            if (!kv.second.measured) continue;   // no predecessor ⇒ in no domain
                            const int step = kv.second.stepped ? 1 : 0;
                            PriorResidualEvidence& a = cache->by_sta[i];
                            ++a.n_measured; a.n_stepped += step;
                            PriorResidualEvidence& b = cache->by_sat[kv.first];
                            ++b.n_measured; b.n_stepped += step;
                        }
                }
                if (station >= 0 && static_cast<std::size_t>(station) < ep.verdict.size()) {
                    if (sat.empty()) return cache->by_sta[static_cast<std::size_t>(station)];
                    // pl has a one-member domain, where priorResidualConsensus
                    // reduces to the member's own verdict, so this family is
                    // bit-for-bit unaffected by the consensus rule.
                    const auto& vmap = ep.verdict[static_cast<std::size_t>(station)];
                    const auto it = vmap.find(std::string(sat));
                    if (it == vmap.end() || !it->second.measured)
                        return PriorResidualEvidence{};
                    return PriorResidualEvidence{1, it->second.stepped ? 1 : 0};
                }
                if (!sat.empty()) {
                    const auto it = cache->by_sat.find(std::string(sat));
                    if (it != cache->by_sat.end()) return it->second;
                }
                return PriorResidualEvidence{};
            });
    }
    // The block-Baarda accumulator lives in ctx.carry (its feed site is inside
    // the redundancy-census seam, which took ctx but no run-TU reference until card
    // NOM3 step EP1; the seam takes the run now, and moving the accumulator to the
    // run's engines is a later step), so it is seated here from config rather than
    // constructed by the context.  An
    // unarmed run keeps the default-constructed empty-family instance, whose
    // enabled() is false, and allocates nothing.
    if (ctx.knobs.bb_on) {
        ctx.carry.bb = NetworkBlockBaarda(
            NetworkCarryVce::parseComponents(nc.block_baarda_families),
            static_cast<double>(nc.block_baarda_window_epochs),
            static_cast<double>(std::max(0, nc.block_baarda_min_blocks)),
            ctx.knobs.bb_report_sigma, ctx.knobs.bb_corrob);
        // Corroboration resolver.  A block is a (station, satellite) pair, the key
        // RawSlipEpoch::verdict is stored under, so unlike the prior-residual
        // resolver this needs no OR-fold over stations or satellites, and the
        // b-cell is meaningful at block granularity.
        //
        // `station` here is the app roster index (the census loop's `i`), not the
        // kernel slot: ep.verdict is indexed by the former, and a slot lookup
        // would silently read another station's satellites.
        if (ctx.knobs.bb_corrob) {
            ctx.carry.bb.setCorroborator(
                [&slip_qc](int station, std::string_view sat) -> PriorResidualCorrob {
                    const RawSlipEpoch& ep = slip_qc.epoch;
                    if (station < 0
                        || static_cast<std::size_t>(station) >= ep.verdict.size()
                        || sat.empty())
                        return PriorResidualCorrob::NoEvidence;
                    const auto& vmap = ep.verdict[static_cast<std::size_t>(station)];
                    const auto it = vmap.find(std::string(sat));
                    // No entry, or an entry with no adjacent-epoch predecessor to
                    // difference against, is NoEvidence, never Flat: a missing
                    // step test says nothing about a step.  A code-only or
                    // single-frequency LOS always lands here.
                    if (it == vmap.end() || !it->second.measured)
                        return PriorResidualCorrob::NoEvidence;
                    return it->second.stepped ? PriorResidualCorrob::Stepped
                                              : PriorResidualCorrob::Flat;
                });
        }
    }
    // Station congruence.  Seated here for the block census's reason (its feed
    // site is inside the redundancy-census seam), with no resolver to install:
    // the INC-3 separability join reads the block census's own hit count at the
    // sweep site, not an injected callback.  An unarmed run keeps the
    // default-constructed unarmed instance.
    if (ctx.knobs.cg_on)
        ctx.carry.cg = NetworkCongruence(/*armed=*/true, kCongruenceWindowEpochs,
                                         kCongruenceWarmupSamples,
                                         ctx.knobs.cg_report_sigma);
    const double start_offset_s = ctx.knobs.start_offset_s;  // testing fast-forward (0 = off)

    // Run-provenance manifest, startup record (L15).  The body lives in
    // NetworkRunTraces (INC-4), where manifest_path is a member.
    traces.writeRunManifestStartup(config_, nc, ctx.knobs, config_path_,
                                   stations.size(), n_threads, log);
    // The parameter model as armed (card PRM phase 1, section 6): one line per kind, read
    // from the registry, the same rows into the manifest.  Trace class: always written,
    // never read by the solve.
    for (const auto& line : ctx.params.modelTable())
        log->info("Network (param model): {}", line);
    // Card PRM phase 3a: each kind's armed response policies per event.
    for (const auto& line : ctx.params.responseTable())
        log->info("Network (param responses): {}", line);
    traces.appendRunManifestRecord(nc, ctx.params.modelJson());

    // The DD cycle-basis invariant carry driver and its persistent state (the
    // carried z-store lives across epochs).  It supersedes the S-dependent
    // PhaseBias / local / RPB carries, the eq34 pin-round/shift hygiene for phase
    // keys (bypassed in the epoch function), and the carry S-transform hygiene: the datum is
    // re-realized freely and the RPB re-estimates fresh, with δ cancelling in the
    // gauge-free DD.  GLONASS (FDMA) is excluded.  Off: no z-store, every fold
    // gated, output byte-identical.
    // Built by networkEngineSpec (app/NetworkRun.hpp, card NOM3 step EP1).
    const NetworkInvariantCarryDriver::Config& carry_cfg = engine_spec.carry_cfg;
    NetworkInvariantCarryDriver& carry_driver = run.engines.carry_driver;
    // The emit-layer engines (filter/NetworkRunGroups.hpp), run by networkEmitStore on every
    // emitted epoch:
    //  - the emit-layer frame re-base (card 4 phase D, ctx.knobs.emit_rebase_on).  It
    //    re-references the emitted per-(sat,σ) phase biases onto the previous emission
    //    frame, so the per-epoch gauge churn of the freely re-realized eq34 datum does not
    //    send the consumer spurious ambiguity resets; the integer step of a genuine
    //    reference-path slip is left intact, so the emitter jump test still fires.  On by
    //    default when carry_invariant is armed (spec D7); the off path is
    //    carry_invariant=false, which never touches the engine (byte-identical).
    //  - the clock emission re-base (card CLK-R): the common-mode frame hold (stage 1) and
    //    the per-satellite step marks (stage 2).  Both knobs are off by default ⇒
    //    rebaseEpoch is never called (byte-identical).  It affects outputs only: the solve
    //    is byte-identical even when armed.
    NetworkEmitEngines& emit_engines = run.emit;

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

    // The joint estimator is constructed once, outside the epoch loop, and reset() and
    // reused each epoch.  A fresh NetworkEstimator per epoch reallocates one dense Schur
    // block per station every epoch, and the process heap retains that churn as committed
    // memory, saturating at ~0.5 GB per station (≈146 GB at 305 stations over a
    // 121-epoch run, which exhausts RAM).  Reusing the estimator's station-block pool
    // (NetworkNormalEquations::reset) holds the footprint to one epoch's working set.
    NetworkEstimator& est = run.engines.est;
    // The Schur reduction runs serially (byte-identical at any thread count) while the
    // build phase threads.
    // Deterministic-parallel DD carry-fold fan-out (network.parallel_seam); 1 = serial.
    est.setSeamThreads(seam_threads);
    // With network.parallel_carry armed, solve()'s foldReducedDdPriors runs serially over
    // blocks with per-station threading inside each block (Eigen stays pinned, so the
    // driver does not un-pin it around solve()).  Off ⇒ the fold keeps seam_threads
    // (block-parallel seam), byte-identical.
    est.setCarryParallelThreads(carry_par ? n_threads : 1);
    // The null-space rank-gate tolerance is resolved once, here with the estimator's
    // other knobs, so the gate call sites, the estimability dump and the prioritized
    // drop-and-heal all read the same value from the estimator instead of each using the
    // compile-time constant.  The default is NetworkEstimator::kNullSpaceTol, so an unset
    // key sends the identical double through the identical arithmetic (byte-identical).
    est.setRankGateTol(config_.networkRankGateTol());
    est.setDenseFoldKappa(config_.networkDenseFoldKappa());   // card CTU section 14; config default 1e-12 (card SMH section 9)
    // network.parallel_carry also un-pins Eigen's threading (RAII) around the two
    // GEMM-bound carry stages, buildDdPriors (W·Pᶻ·Wᵀ) and installPosterior
    // (posterior-covariance read-back), whose large dense products Eigen threads well.
    // The third stage, foldReducedDdPriors, is not wrapped: its cost is a per-station
    // reduction (small inner-dimension matrix products plus a serial LDLT) that Eigen
    // cannot thread, so it fans out with its own OpenMP while Eigen stays pinned, and
    // wrapping solve() would nest the two.
    //
    // The epoch loop pins Eigen to one thread to avoid nesting inside the per-station
    // OpenMP assembly region; these two stages run outside that region, so un-pinning
    // spreads their GEMMs across cores.  The pin is restored on every exit path (early
    // continue, withheld-epoch skip, thrown call), so the next epoch's assembly and the
    // fold's own OpenMP never run with Eigen un-pinned.  Inert unless armed, so a run
    // with it off is byte-identical.

    // Pin-aware station PD guard (network.station_pd_pin_aware): a median-|pivot|
    // reference scale for the local-block rank test.  Without it, the prioritized
    // datum's in-block 1e10 anchor pins eject marginal stations every epoch.
    est.setStationPdMedianScale(config_.networkStationPdPinAware());
    // Null-direction read (card RF-PD, network.trace_station_pd_null): an eigen-read of
    // the null direction of a block the PD test rejects, so the drop names a parameter
    // instead of a pivot magnitude.  Measure-only; it runs only on an already-rejected
    // block.
    est.setStationPdNullTrace(config_.getNetworkConfig().trace_station_pd_null);
    // PD-safe DD fold, kernel half (card 4): product-form posterior read-back,
    // equilibrated scale-relative factorization, counted eigenvalue-clamp repair.  Set
    // once here (it survives est.reset(), like the timers).  Off ⇒ byte-identical; the
    // INC-0 counters it feeds are always on and solve-neutral.
    est.setDdFoldPdSafe(carry_cfg.pd_safe);

    // === Tier D0: per-epoch section timers (network.trace_section_timers) =====
    // Arms the estimator's wall-clock accumulators around assembly, reduce, solve, the
    // shared-covariance full inverse and the per-station covariance consumers, so a
    // profiling run shows which term dominates the per-epoch cost (and so which tier-D
    // scaling lever is worth pulling).  The estimator accumulates continuously across the
    // run; the driver snapshots it per epoch for the per-epoch delta line and reads the
    // run total at exit.  Off (default) ⇒ no clock reads, byte-identical.  The toggle is
    // ctx.knobs.sec_timers_on.
    est.setPerfTiming(ctx.knobs.sec_timers_on);
    est.resetSectionTimers();
    // The carry driver's per-op timers are armed on the same toggle (INC-C0).  They split
    // the SB_DDBUILD and SB_INSTALL wall brackets into basis projection, posterior
    // read-back and D5 gate (the dense K² operations the perf-restructure card ranks).
    // Byte-identical off.
    carry_driver.setPerfTiming(ctx.knobs.sec_timers_on);
    carry_driver.resetPerfTimers();
    // The D0 run totals the driver keeps (the assembly wall and the profiled-epoch count)
    // are timing.asm_wall_ms and timing.profiled_epochs.
    if (ctx.knobs.sec_timers_on)
        log->info("Network (D0): per-epoch section timers ARMED "
                  "(assembly / reduce / solve / cov-inverse / cov-consumers).");



    // === State snapshot and warm resume (RTB-7(a)) ============================
    // Separate from the product-file replay path: the snapshot restores the producer's
    // in-memory cross-epoch state, while NetworkProductIO replay reproduces every
    // discontinuity counter without a snapshot.  With the path empty (default) the
    // snapshot and resume calls below do nothing, so the output is byte-identical.
    auto& snap_path = ctx.carry.snap_path;
    if (!nc.state_snapshot_path.empty()) {
        snap_path = nc.state_snapshot_path;
        if (snap_path.is_relative()) snap_path = log_dir_ / snap_path;
    }
    networkSnapshotFingerprint(ctx, config_, stations);
    networkTryWarmResume(ctx, config_, sync, emitter, carry_driver, log,
                         &emit_engines.clk_rebase,  // the CLK-R held frame is restored too (§18.10),
                         &emit_engines.rebase);     // as is the phase-bias emit re-base (RS-5 §21.10)
    // Applies the canonical-signal latch the resume just staged (RS-8, §21.16).  This
    // happens here rather than inside networkTryWarmResume because the latch lives in
    // each station's PppModel and the roster is in scope only in the driver.  It must
    // precede the first epoch, which locks the latch.  Inert unless
    // network.resume_signal_canon_restore is armed and a warm resume staged rows.
    networkRestoreCanonicalSignals(ctx, config_, stations, log);
    // Hands the resume's stashed code-bias linearization points to the station
    // initializers (RS-9, §21.18).  This is the only window for it: the one-shot
    // isCodeBiasInitialized latch closes at each station's first
    // computeMeasurementUpdate.  A no-op unless the resume stashed RL rows.
    networkApplyRxCbLinRestore(ctx, config_, stations, log);
    // Event harness (docs/SPEC_event_harness.md): arms the scripted-fault schedule.  It
    // comes after the warm resume because the injector is outside the resume
    // fingerprint, and its "ARMED" banner must not be mistaken for restored state.
    // Unarmed (network.inject absent), every seam below is untouched.
    networkInjectArm(ctx, config_, log_manager_, log);
    // Canonical-pair admission (card CPA,
    // docs/refound/CPA_canonical_pair_admission_2026-09-20.md): sizes the per-station
    // canonical-latch cache from the producer roster.  Disarmed, every member of
    // ctx.carry.cpa is untouched and the seam is unreachable.
    networkCanonicalPairArm(ctx, config_, static_cast<int>(stations.size()));
    // Signal roster and arc ledger (card SID phase 1,
    // docs/refound/SID_signal_identity_2026-09-22.md section 5): sizes the roster's
    // per-station caches, resolves the arc ledger's horizon from the readers' own gap
    // windows, and records the station names.  Every network run sets them up (card NOM
    // step N3a); network.signal_roster switches the ledger's trace only.
    networkSignalRosterArm(ctx, config_, log, stations);
    // Emitter read against the ledger, and the wire declaration (card SID phase 5a,
    // docs/refound/SID_signal_identity_2026-09-22.md section 4).  Disarmed
    // (network.ledger_emit absent), every member of ctx.ledger_emit is untouched and
    // all three seams are unreachable.
    networkLedgerEmitArm(ctx, config_, log);

    // The code-bias admission census runs once, before the loop, so the registration
    // site can gate on a run-latched (sat,signal) verdict instead of a per-epoch
    // >= 2-station count (filter/NetworkBiasAdmission.hpp).  It is armed only when both
    // the master flag and the sat-code-bias slice are on; otherwise
    // ctx.admission.active stays false and the legacy per-epoch path runs.
    //
    // The census window is [i0, i0+K) of the synced vector, i0 being the first epoch
    // the loop processes.  The loop start is a lazy time-anchored fast-forward
    // (network.start_offset_hours), not a precomputed index, so that condition is
    // replicated here against the first synced epoch's time.  A warm resume starting a
    // different span reclassifies over that span: the census is per process, not
    // carried in the snapshot, and field-inert for post-processing.
    //
    // Under streaming the front-end replaces the batch epoch source, so this census
    // would latch verdicts from file content the run never processes (stale-day RINEX
    // kept only for station metadata).  A wrong but plausible latched admission would
    // go unnoticed, so the census is refused with a warning and the per-epoch gate is
    // used instead.
    if (ctx.knobs.streaming_on && config_.networkCbAdmission()
        && config_.estimateNetworkSatCodeBias())
        log->warn("Network (RTF-3): network.cb_admission census SKIPPED under "
                  "streaming — it would classify over the metadata RINEX files, "
                  "not the live epochs.  Falling back to the legacy per-epoch "
                  ">= 2-station gate.");
    if (!ctx.knobs.streaming_on && config_.networkCbAdmission()
        && config_.estimateNetworkSatCodeBias()
        && !synced.empty()) {
        std::size_t i0 = 0;
        if (start_offset_s > 0.0) {
            const double anchor_s =
                TimeUtils::continuousGpsSeconds(synced.front().time);
            while (i0 < synced.size()
                   && TimeUtils::continuousGpsSeconds(synced[i0].time) - anchor_s
                          < start_offset_s)
                ++i0;
        }
        ctx.admission = NetworkBiasAdmission::runCensus(
            synced, i0, config_.networkCbAdmissionCensusEpochs(),
            config_.networkCbAdmissionMinStations());
        const auto& adm = ctx.admission;
        ctx.counters.n_cb_admitted =
            static_cast<std::size_t>(adm.n_admitted);
        ctx.counters.n_cb_quarantined_census =
            static_cast<std::size_t>(adm.n_quarantined_census);
        if (adm.active) {
            struct ConsTally { int seen = 0, admitted = 0, lowcount = 0, noifpin = 0; };
            std::map<char, ConsTally> per_cons;
            for (const auto& p : adm.census) {
                ConsTally& t = per_cons[letterOfSat(p.satellite)];   // '?' when unknown
                ++t.seen;
                if (p.verdict == NetworkBiasAdmission::Verdict::Admitted)      ++t.admitted;
                else if (p.verdict == NetworkBiasAdmission::Verdict::LowCount) ++t.lowcount;
                else                                                           ++t.noifpin;
            }
            log->info("Network (CB-ADM): census over {} epochs (K_eff) x {} stations "
                      "[from i0={} of {} synced] — effective_min={}, sustained_floor={}; "
                      "{} (sat,signal) pairs seen, {} admitted, {} quarantined.",
                      adm.census_epochs_used, adm.census_stations, i0, synced.size(),
                      adm.effective_min, adm.sustained_floor,
                      static_cast<int>(adm.census.size()), adm.n_admitted,
                      adm.n_quarantined_census);
            for (const auto& [c, t] : per_cons)
                log->info("Network (CB-ADM):   [{}] {} pairs: {} admitted, {} low-count, "
                          "{} no-IF-pin.", c, t.seen, t.admitted, t.lowcount, t.noifpin);
            constexpr int kMaxRows = 40;
            int shown = 0;
            for (const auto& p : adm.census) {
                if (shown++ >= kMaxRows) {
                    log->info("Network (CB-ADM):   ... ({} more pairs omitted from the table)",
                              static_cast<int>(adm.census.size()) - kMaxRows);
                    break;
                }
                const char* verdict = "ADMIT";
                const char* reason  = "both tiers pass";
                if (p.verdict == NetworkBiasAdmission::Verdict::LowCount) {
                    verdict = "QUAR";
                    reason  = "support < effective_min (tier-1)";
                } else if (p.verdict == NetworkBiasAdmission::Verdict::NoIfPin) {
                    verdict = "QUAR";
                    reason  = "no IF-pinnable pair, < 2 distinct bands (tier-2)";
                }
                log->info("Network (CB-ADM):   {:<11} support={:<3} {:<5} ({})",
                          p.key, p.support, verdict, reason);
            }
        } else {
            log->warn("Network (CB-ADM): census produced NO classification (empty window "
                      "at i0={} of {} synced) — falling back to the legacy per-epoch "
                      ">= 2-station gate.", i0, synced.size());
        }
    }

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
