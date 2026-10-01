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
 *   Setup / teardown    app/ApplicationNetworkSetup.cpp · filter/NetworkTeardown
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

#include "rinex/MultiRinexSync.hpp"
#include "products/ProductCache.hpp"
#include "products/SsrCorrectionStore.hpp"
#include "products/NetworkProductEmitter.hpp"
#include "products/NetworkEmitRebase.hpp"   // emit-layer frame re-base
#include "products/NetworkPbEmitFilter.hpp" // phase-bias emission-continuity filter
#include "products/NetworkClockEmitRebase.hpp" // clock emission re-base + per-sat step marks
#include "products/NetworkProductIO.hpp"
#include "products/CombinedCorrectionSigma.hpp"   // combined-correction σ arithmetic
#include "products/NetworkStateSnapshot.hpp"   // atomic state snapshot + warm resume
#include "products/LiveEphemerisStore.hpp"     // broadcast a-priori store for brdc_rts
#include "products/RinexNavParser.hpp"         // RINEX-nav → LiveEphemerisStore loader (card 5, RTF-1)
#include "filter/NetworkEstimator.hpp"
#include "filter/NetworkClockLevel.hpp"         // code-only satellite-clock level re-base (card CLV)
#include "filter/IlsCore.hpp"                 // producer-side network AR (Phase 2+): ils::ilsFix
#include "filter/NetworkSbasis.hpp"
#include "filter/NetworkPivotHandover.hpp"   // pivot-handover datum-change detector (A1/A2)
#include "filter/NetworkCarryVce.hpp"        // LS-VCE variance-component monitor for carry rows (M2)
#include "filter/NetworkPriorResidual.hpp"   // prior-residual census (SPEC_prior_residual_census)
#include "filter/NetworkIonoDeweightGate.hpp" // Station-adaptive SH-VTEC-constraint deweight (iono deweight card)
#include "filter/NetworkInvariantCarryDriver.hpp"  // DD cycle-basis invariant carry (card 4, M1 C3b)
#include "filter/NetworkMwWlLayer.hpp"             // MW-seeded wide-lane pseudo-observation layer (card M5 WL-1)
#include "filter/StateKeySchema.hpp"
#include "filter/NetworkRunTypes.hpp"      // run-loop record types: NetStation, StationAssembly, carry PODs (INC-2)
#include "filter/NetworkWlRunTypes.hpp" // WL and RAII types hoisted out of runNetwork() (INC-9b)
#include "filter/NetworkRobustStats.hpp"   // median / MAD helpers for robust attribution (INC-2)
#include "filter/NetworkPhaseDatum.hpp"    // census, ensemble datum, anchor election, drop-and-heal (INC-8b)
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
#include "filter/NetworkFastClock.hpp"    // split-cadence clocks-only fast pass (FC-1)
#include "filter/NetworkTeardown.hpp"
#include "filter/NetworkPhaseVisibleDiag.hpp"  // phase-visible consistency instrument (card PVN)
#include "filter/NetworkPbHoldClone.hpp"       // the smoothed hold's clock clones (card SMH)
#include "filter/NetworkProducerAr.hpp" // log-only producer AR and NEQ export (INC-9b)
#include "filter/NetworkInject.hpp"
#include "filter/NetworkCanonicalPairAdmission.hpp" // canonical-pair admission seam (card CPA)
#include "app/SignalRoster.hpp"  // signal roster and arc-ledger observatory (card SID phase 1)
#include "filter/NetworkSecondPhaseScope.hpp"  // second phase channel, epoch tail (card SID phase 6a)
#include "filter/NetworkChannelAll.hpp"      // all-channel census (card SID phase 6b)
#include "filter/NetworkLedgerEmit.hpp"  // emitter read against the ledger, declared roster (card SID phase 5a)
#include "filter/NetworkInstallCarry.hpp" // installPosterior and the WL zone-4 fold / inject (INC-9b)
#include "filter/NetworkDdPriorBuild.hpp" // buildEnsembleGauges / buildCarryDdPriors free functions (INC-9b)
#include "filter/NetworkSolveGlue.hpp" // datum, solve and within-epoch RAIM (INC-9b)
#include "filter/NetworkIonoDiag.hpp"    // iono / ZWD / anchor-info viz traces (INC-11b)
#include "filter/NetworkDdFrac.hpp"    // cross-station DD fractional-residual trace (INC-11b)
#include "filter/NetworkSdEwlAr.hpp"   // SD EWL Route-B integer decision (increment 2a)
#include "filter/NetworkShDevVtec.hpp"    // SH-deviation carry KF and VTEC-coefficient trace (INC-11b)
#include "filter/NetworkShHealth.hpp"    // SH-field health probe, adaptive-deweight gate, carry LS-VCE (INC-11b)
#include "filter/NetworkIonoDwRelease.hpp" // iono-deweight release hygiene: reseed / park bound / row screen
#include "filter/NetworkLevelAuthority.hpp"  // per-epoch level-authority census (LV-6)
#include "filter/NetworkLeveling.hpp"    // phase-leveling carry harvest, D2/C1 stash, SH-convergence latch (INC-11b)
#include "filter/NetworkStashStatus.hpp"    // carry re-stash, LS-VCE fold, epoch-status QC row (INC-11b)
#include "logging/CsvRotator.hpp"     // size-based rotation of the unbounded service CSVs (P1)
#include "logging/NetworkRunTraces.hpp" // trace-stream ownership, open logic, roster, FlagRate (INC-3)
#include "app/NetworkRunContext.hpp"    // knobs, tier-1 carry state and service counters, as ctx (INC-4)
#include "app/NetworkSetupResult.hpp"   // setup-phase handoff (setup) and navAvailabilityGpsSeconds (INC-5)
#include "slip/MwLaneTiers.hpp"         // per-constellation lane-tier band table (UWL-1)
#include "slip/MwSlipDetector.hpp"      // phase-leveling slip repair (T0, increment 2)
#include "slip/TurboEditResolver.hpp"   // MW+GF per-signal integer repair (Increment 2)
#include "slip/IfPhaseSlipDetector.hpp" // IF-phase between-sat/between-epoch slip detection
#include "slip/NetworkSlipQc.hpp"       // pure slip-QC helpers: ifElevMap, parseIfPairResetKey (INC-2)
#include "slip/NetworkSlipOracle.hpp"   // simulation-only oracle slip response (level-preserving ceiling)
#include "atmosphere/SphericalHarmonicIonosphere.hpp"
#include "common/Constants.hpp"
#include "common/PppUtils.hpp"        // signal wavelengths for the DD fractional-cycle diagnostic (B5)
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
    const ClosedLoopConfig& cl = nc.closed_loop;
    const bool closed_loop_on = setup.closed_loop_on;
    const bool brdc_rts = setup.brdc_rts;
    auto& brdc_nav_staged = setup.brdc_nav_staged;
    auto& brdc_nav_cursor = setup.brdc_nav_cursor;
    auto& sh_model = setup.sh_model;
    const GlobalVtecShConfig& shc = config_.getGlobalVtecShConfig();
    auto& stations = setup.stations;
    const bool parallel_on = setup.parallel_on;
    const int n_threads = setup.n_threads;
    const int seam_threads = setup.seam_threads;
    const bool carry_par = setup.carry_par;
    SsrCorrectionStore& network_store = setup.network_store;
    NetworkProductEmitter& emitter = *setup.emitter;
    auto& consumer = setup.consumer;

    // Closed-loop metrics + trace.  ARP-referenced truth (setup owns the marker ->
    // ARP reduction; see NetworkSetupResult::cl_truth_arp) so the consumer's ENU
    // error is not offset by the station's antenna eccentricity.
    const Eigen::Vector3d cl_truth = setup.cl_truth_arp;
    const Coordinates::Llh cl_truth_llh =
        closed_loop_on ? Coordinates::ecefToLlh(cl_truth) : Coordinates::Llh{};
    std::size_t cl_epochs = 0, cl_ar_ran = 0, cl_ar_fixed = 0;
    long long   cl_sum_fixed = 0;
    double cl_first_fix_tow = -1.0; int cl_first_fix_week = -1;
    std::deque<double> cl_recent_err3d;   // last-N 3D errors → converged-segment RMS
    constexpr std::size_t kClRecentWindow = 30;

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
    // per-epoch writers live in the seams, which receive `traces`; the references
    // below keep the former local names for the uses left in this file.
    NetworkRunTraces traces(log_dir_);
    auto& trace                = traces.trace;
    auto& dd_frac_trace        = traces.dd_frac_trace;
    auto& slip_raw_trace       = traces.slip_raw_trace;
    std::unique_ptr<PppUtils> dd_ppp_utils;   // DD-trace wavelength provider, built only with that trace on (B5)
    // Producer-side network AR (network.ambiguity_resolution, Phase 2+): a
    // wavelength provider for the between-station DD metres-to-cycles conversion,
    // built only when AR is armed.  Off by default; with it off the AR block is
    // skipped entirely and the float product is byte-identical.
    const auto& netar = nc.ambiguity_resolution;
    std::unique_ptr<PppUtils> ar_ppp_utils;
    if (netar.enable) ar_ppp_utils = std::make_unique<PppUtils>(log_manager_);
    // Slip-QC hardening channel: knobs +
    // per-station cross-epoch state + this epoch's raw evidence + run-end
    // counters, threaded by reference into the PASS-A slip seam and the
    // assembly pre-filter.  Sized once here so the per-station EWMAs survive
    // the whole run.
    SlipQcChannel slip_qc;
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
        // The CS-GF authority (iono_carry_source = smoothed_gf) has the same need.
        // The LV-6 census arms the advance independently: the kernel is
        // instrumentation sanctioned under 8(b), and the carry's inject path,
        // which is falsified, stays gated on carry_iono and untouched
        // (SPEC_level_authority_internal §3).  Arming iono_cs flips cardsArmed(),
        // so census A/B pairs must pin cycle_slip.gap_instrument in both arms.
        sp.iono_cs             = (config_.networkCarryIono()
                                  && config_.networkIonoCarrySource() == "smoothed_gf")
                                 || config_.networkLevelAuthorityCsgf();
        // Corroborated retap: the SB_RAIM keep
        // consumes the qualified verdicts, so it must arm the evidence passes.
        sp.retap_corr          = config_.networkCycleSlipRelevelRetapCorroborate();
        // Per SPEC_lane_slip_response §7.3, the lane observatory consumes both the
        // raw rows and the qualified verdict (d_mw / measured), so it arms both
        // anyConsumer and cardsArmed; the member's doc records the failure
        // measured when only the former was armed.
        sp.lane_snap           = config_.networkCycleSlipRelevelLaneSnap();
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
    // Wavelength providers for the DD trace (B5) and the raw-slip path, gated on
    // the streams openAll just opened.  They are built here because they need
    // log_manager_, which the traces unit does not hold.  The raw-slip provider
    // serves three consumers (the trace, card L and card G) and is built when any
    // of them is armed, per spec §1: "compute once per epoch when any consumer
    // needs it".  With none armed it stays null and the raw-observable path is
    // skipped.
    if (dd_frac_trace) dd_ppp_utils = std::make_unique<PppUtils>(log_manager_);
    if (slip_qc.params.anyConsumer(static_cast<bool>(slip_raw_trace)))
        slip_qc.utils = std::make_unique<PppUtils>(log_manager_);
    // Split-cadence clocks-only fast pass.  Two hooks, both no-ops when off: a
    // clocks-only tick on every decimated-out epoch (the T2e hook), and the anchor
    // harvest on every epoch that emitted.  fc_shadow also runs the tick on full
    // epochs for grading and discards the result; it is a permanent instrument
    // and never feeds back.
    const bool fc_on     = nc.fast_clock.enable;
    const bool fc_shadow = nc.fast_clock.enable && nc.fast_clock.shadow;
    // The fast tier's own wavelength provider, built here for the same reason
    // (log_manager_ is not reachable from the seam).  It converts raw RINEX cycles
    // to metres at both the anchor harvest and the tick, and the wavelength it
    // returns is cached per band in the anchor, so the tick reuses the identical
    // value.  A wavelength that differed by 1e-9 m between the two ends would turn
    // a phase count of about 1e8 cycles into a 0.1 m error, so the tick must use
    // the same number, not merely the same formula.
    std::unique_ptr<PppUtils> fc_ppp_utils;
    if (fc_on) fc_ppp_utils = std::make_unique<PppUtils>(log_manager_);



    // === Epoch loop =========================================================
    auto& prev_carry = ctx.carry.prev_carry;
    auto& last_pivot = ctx.carry.last_pivot;
    auto& anchor_incumbent = ctx.carry.anchor_incumbent;
    auto& this_epoch_anchor = ctx.carry.this_epoch_anchor;
    auto& eq34_reref = ctx.carry.eq34_reref;
    auto& clock_osc = ctx.carry.clock_osc;
    auto& clock_rate_est = ctx.carry.clock_rate_est;
    auto& start_anchor_s = ctx.carry.start_anchor_s;
    auto& local_carry = ctx.carry.local_carry;
    auto& ura_sigma0_ewma = ctx.carry.ura_sigma0_ewma;
    auto& grade_metric_s = ctx.carry.grade_metric_s;
    const bool   pl_on         = ctx.knobs.pl_on;
    const double pl_q_amb      = ctx.knobs.pl_q_amb;
    const int    pl_warmup     = ctx.knobs.pl_warmup;
    const double pl_max_gap_s  = ctx.knobs.pl_max_gap_s;
    const double pl_var_floor  = ctx.knobs.pl_var_floor;  // lock-in guard; 0 = off
    const bool   pl_warmup_on_conv  = ctx.knobs.pl_warmup_on_conv;  // warm-up on SH convergence (T1d)
    const bool   fullcov_on    = ctx.knobs.fullcov_on;       // dense (P_prev+Q)⁻¹ shared carry (C1)
    const bool   carry_cpl_cond = ctx.knobs.carry_cpl_cond; // conditional local carry on the coupling rows; requires fullcov_on, enforced at load (card 1)
    // Variance-component monitor for the carry rows (M2, LS-VCE carry-q).  It
    // accumulates the post-fit carry innovations (carried − solved) into a
    // residual-form LS-VCE estimator and logs σ̂ (floor in m², rate in m²/s) per
    // family and constellation against the hand-set floors.  It does not feed back
    // into the solve: the whole path is gated on carry_vce, so with it off the
    // output is byte-identical.  The estimator is seeded with the PL floor and q
    // as a weak a-priori worth about one row, which is immaterial against the
    // thousands of carry rows per epoch; the pb and cb families share that
    // PL-scale prior, which only sets the regularisation and the no-data fallback.
    const bool   carry_vce     = ctx.knobs.carry_vce;
    const std::vector<std::string> vce_families =
        carry_vce ? NetworkCarryVce::parseComponents(config_.networkCarryVceComponents())
                  : std::vector<std::string>{};
    const bool   vce_wants_pb = std::find(vce_families.begin(), vce_families.end(),
                                          std::string("pb")) != vce_families.end();
    const bool   vce_wants_cb = std::find(vce_families.begin(), vce_families.end(),
                                          std::string("cb")) != vce_families.end();
    NetworkCarryVce carry_vce_est(vce_families,
        static_cast<double>(config_.networkCarryVceWindowEpochs()),
        /*split_by_const=*/true,
        /*seed_floor=*/pl_var_floor, /*seed_rate=*/pl_q_amb);
    int last_vce_hour = -1;   // content-hour bucket for the per-hour VCE log line
    double first_vce_s = -1.0;  // content time of the first monitored epoch (hour anchor)
    // Apply step (M2 v2): arms the LASER 5-95 variance-factor iteration over the
    // monitor above.  Unlike the monitor's estimator this is not a run-TU local:
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
    // Prior-residual census.  A run-TU local for the same reason carry_vce_est is
    // one (see the "engine objects stay run-TU locals" note in
    // app/NetworkRunContext.hpp), threaded into the same four seams: SB_T1A
    // advances it, SB_LEVELING feeds pl/zwd/rxcb, SB_STASH_STATUS feeds pb/cb and
    // drains it, and the teardown reads it.  It shares the M2 monitor's per-row
    // arithmetic and does not affect the solve: with the census armed the
    // solution is byte-identical.
    NetworkPriorResidual prires_est(
        ctx.knobs.prires_on
            ? NetworkCarryVce::parseComponents(nc.prior_residual_families)
            : std::vector<std::string>{},
        static_cast<double>(nc.prior_residual_window_epochs),
        static_cast<double>(nc.prior_residual_min_rows),
        ctx.knobs.prires_report_sigma,
        ctx.knobs.prires_corrob,
        // The constraint-aware global variance component uses this accumulator:
        // its prior side is the census's folded rows, and its data side comes
        // from SB_RAIM, one seam ahead of the SB_STASH_STATUS drain.
        ctx.knobs.prires_s0g);
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
    // the redundancy-census seam, which takes ctx but no run-TU reference), so it
    // is seated here from config rather than constructed by the context.  An
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
    const bool   ura_on        = ctx.knobs.ura_on;                  // emit the per-sat URA quality channel (B1)
    const bool   raim_on       = ctx.knobs.raim_on;            // post-fit network RAIM (Increment 3, T2)
    const bool   clock_signal_basis_hold = ctx.knobs.clock_signal_basis_hold; // hold each satellite's IF-pin reference pair fixed across epochs (a time-consistent δtˢ↔dˢ split) instead of re-selecting it from each epoch's coverage; inert without estimate_sat_code_bias (W6)
    // Two-state (offset+drift) oscillator clock model (network.clock_model): when on, the
    // satellite clock is carried via the Allan-derived Q model in clock_osc (below) instead of
    // the scalar prev_carry random walk, and a satellite is flagged by the normalized-innovation
    // dynamics-fault test rather than the absolute/jump bounds.
    const bool   clk_model_on   = ctx.knobs.clk_model_on;
    // Emitting C1 (clock rate) from the finite-differenced solved clock without the two-state
    // carry (network.emit_clock_rate) uses a decoupled rate estimator (clock_rate_est, aliased
    // above): no prior and no refloat, so the solution is byte-identical.  Inert when
    // clk_model_on, which already supplies C1.
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
    traces.appendRunManifestRecord(nc, ctx.params.modelJson());
    auto& prev_tow = ctx.counters.prev_tow;
    auto& prev_week = ctx.counters.prev_week;
    auto& n_emitted = ctx.counters.n_emitted;
    auto& last_health_write_s = ctx.counters.last_health_write_s;
    auto& n_gated_epochs = ctx.counters.n_gated_epochs;
    auto& n_gated_key_epochs = ctx.counters.n_gated_key_epochs;

    // --- Empirical-alpha flag-rate counters (network.trace_flag_rates) -----
    // Observation only: {opportunities, flags} per QC tier, reported at the end of
    // the run as the realized rate against the implied one-sided Gaussian α for
    // the configured k.  Never fed back into a decision; every increment is
    // guarded, so with the knob off the output is byte-identical.
    const bool trace_flag_rates = ctx.knobs.trace_flag_rates;
    auto& max_dur_anchor_s = ctx.counters.max_dur_anchor_s;
    auto& max_dur_anchored = ctx.counters.max_dur_anchored;

    // The gauge-pin predicate ("is (sat, signal) at station i a gauge-pinned
    // ambiguity this epoch?") is networkIsGaugePinned_ in
    // ApplicationNetworkAssembly.cpp, the single test that replaces the scattered
    // is_pivot gauge tests.
    //   Legacy (network.phase_datum_prioritized off): membership means the station
    //     is the pivot, which pins the whole shared_phase_keys set, so the
    //     predicate returns is_pivot and every rewritten site stays byte-identical.
    //   Prioritized: membership means the "sat_signal" key is in station i's
    //     elected pinned_amb_targets, the anchor for that gauge freedom, whose pin
    //     is folded into the station's own local block.  The key form matches the
    //     pin fold and the ambiguity-state key.
    // The anchor-free sum-to-zero phase-bias datum reuses all of the prioritized
    // machinery (no pivot-first, every station carries its arcs, per-(sat,signal)
    // shared set) and differs only in how the gauge is realized: a kernel
    // sum-to-zero fold instead of a per-(sat,signal) anchor pin.  The prioritized
    // code paths therefore run under either mode, and the two diverge only at the
    // anchor election (skipped) and the pin fold (inert, because no station gets a
    // pinned_amb_targets entry).
    const bool phase_datum_ensemble    = ctx.knobs.phase_datum_ensemble;
    // The Eq-34 / de Jonge single-receiver-chain datum.  Like the ensemble it
    // drives all the prioritized machinery, diverging only at the election (a
    // spanning-tree chain election) and the pin fold (per-tree-edge ambiguity pins
    // with a target, plus a datum-receiver RPB pin).  Mutually exclusive with the
    // ensemble, enforced at config load.
    const bool phase_datum_eq34        = ctx.knobs.phase_datum_eq34;
    // DD cycle-basis invariant carry gate, declared early so the superseded eq34
    // hygiene knobs below can gate on it.  Requires phase_datum_eq34.
    const bool carry_invariant         = ctx.knobs.carry_invariant;
    // The DD cycle-basis invariant carry driver and its persistent state (the
    // carried z-store lives across epochs).  It supersedes the S-dependent
    // PhaseBias / local / RPB carries, the eq34 pin-round/shift hygiene for phase
    // keys (bypassed below), and the carry S-transform hygiene: the datum is
    // re-realized freely and the RPB re-estimates fresh, with δ cancelling in the
    // gauge-free DD.  GLONASS (FDMA) is excluded.  Off: no z-store, every fold
    // gated, output byte-identical.
    NetworkInvariantCarryDriver::Config carry_cfg;
    carry_cfg.qz = config_.networkCarryInvariantQz();
    // Estimation-side fine-slip innovation gate: the invariant-carry-native
    // replacement for the RAIM fine-slip layer that the invariant bypass empties.
    // Requires carry_invariant (rejected at config load otherwise); off, the output is
    // byte-identical.
    const bool carry_innov_gate = carry_invariant && config_.networkCarryInvariantInnovGate();
    carry_cfg.innov_gate = carry_innov_gate;
    carry_cfg.innov_k    = config_.networkCarryInvariantInnovK();
    // LS-VCE calibration of the gate threshold (σ̂₀²); requires innov_gate, rejected at
    // config load otherwise (D5 v2).
    carry_cfg.innov_calibrate        = carry_innov_gate && config_.networkCarryInvariantInnovCalibrate();
    // Corroboration gate on the re-tap (card 4, D5 v3).  The loader already forces the
    // knob false when the D5 gate is off; the && here keeps the driver Cfg
    // self-consistent on any load path.
    carry_cfg.innov_corroborate      = carry_innov_gate && config_.networkCarryInvariantInnovCorroborate();
    carry_cfg.innov_corroborate_backstop_k = config_.networkCarryInvariantInnovCorroborateBackstopK();
    carry_cfg.innov_carried_only     = carry_innov_gate && config_.networkCarryInvariantInnovCarriedOnly();
    carry_cfg.innov_calibrate_window = config_.networkCarryInvariantInnovCalibrateWindow();
    // PD-safe DD fold (card 4).  The driver half owns the two edges of invariant #7,
    // the install-time degenerate-block rebirth and the warm-resume restore guard; the
    // kernel half (est.setDdFoldPdSafe, below) owns the arithmetic.  The loader already
    // disarms it without carry_invariant; the && here keeps the driver Cfg
    // self-consistent on any load path.
    carry_cfg.pd_safe = carry_invariant && config_.networkCarryInvariantPdSafe();
    // Linearization-frame halves of the carry (RS-2, SPEC_event_harness §21.4).
    // carry_lin captures Σ c·x_lin per chord at every harvest (write side, snapshot only);
    // carry_rebase re-bases a restored branch onto this run's x_lin at the first post-resume
    // DD build (read side).  The loader disarms carry_lin without state_snapshot_carry; both
    // are ANDed with carry_invariant here so the driver Cfg is self-consistent on any load
    // path.
    carry_cfg.carry_lin    = carry_invariant
                             && config_.getNetworkConfig().state_snapshot_carry_lin;
    carry_cfg.carry_rebase = carry_invariant && config_.networkResumeCarryRebase();
    // Lane-coordinate carry (card M5 WL-3).  The loader enforces carry_invariant and
    // mw_wl.ewl_enable.  lane_joint separates the lane shape (joint 2K pair blocks, lane
    // prior/q, lane measurement rows, fold-on-change fixing) from the §6.1 equivalence
    // shape (every lane feature at its isotropic/off sentinel ⇒ split per-band emission,
    // information-identical to the per-band carry).
    const auto& lane_cfg = config_.getNetworkConfig().lane_carry;
    const bool lane_on = carry_invariant && lane_cfg.enable;
    const bool lane_joint = lane_on
        && (lane_cfg.q_wl_scale != 1.0 || lane_cfg.sigma_wl_fresh_cyc > 0.0
            || lane_cfg.lane_meas || lane_cfg.bridge_meas);
    if (lane_on) {
        carry_cfg.q_wl_scale         = lane_cfg.q_wl_scale;
        carry_cfg.sigma_wl_fresh_cyc = lane_cfg.sigma_wl_fresh_cyc;
        carry_cfg.lane_joint         = lane_joint;
        // The optimized fold shape.  Inert unless lane_joint (the only shape with
        // lane rows); the driver primitives + finalizeLaneBatch key off carry_cfg.fold_blocked.
        carry_cfg.fold_blocked       = lane_joint && lane_cfg.fold_blocked;
        // Pinned-cycle increment skip (also inert unless lane_joint).
        carry_cfg.skip_pinned_increments = lane_joint && lane_cfg.skip_pinned_increments;
        carry_cfg.skip_pinned_within_n   = lane_cfg.skip_pinned_within_n;
    }
    NetworkInvariantCarryDriver carry_driver(carry_cfg);
    // MW-seeded wide-lane pseudo-observation layer (card M5 WL-1).  It uses the carry
    // driver's chord basis, so it requires carry_invariant (rejected at config load
    // otherwise).  Off (mw_wl.enable=false) ⇒ NetworkMwWlLayer::build returns no blocks and
    // the per-station MW detectors are not built for it, so the producer is byte-identical.
    const bool mw_wl_on = carry_invariant && config_.networkMwWlSeed();
    // With the EWL lane armed (card M5 WL-2), the constellation's third band (the EWL
    // pair's uncarried leg: GPS L5, Galileo E5b) is excluded from the invariant-carry
    // driver, so that lane acquires no unearned stiff prior (the F6 wall) and its EWL rows
    // are not opposed by a carried value.  The band stays in the eq34 election and datum
    // (shared_phase_keys and pins are built upstream and are untouched).  Off ⇒ the third
    // band is carried like the others (byte-identical).  v1 covers GPS and Galileo; GLONASS
    // is never carried.
    const auto& nmw = config_.getNetworkConfig().mw_wl;
    const bool ewl_carry_exclude = carry_invariant && nmw.ewl_enable;
    // With card M5 UWL-1 armed, the uncarried band comes from the per-constellation
    // lane-tier table (Galileo's top band is selectable: '7' = E5b, lane λ 9.768 m;
    // '8' = E5 AltBOC, lane λ 19.537 m).  uwl_enable=false keeps the legacy digits
    // verbatim, which is the §6.1(b) gate: arming a constellation without the card
    // changes nothing.  For GPS (and Galileo at the default '7') the table reproduces the
    // legacy digits, so armed GPS-only runs stay bit-identical too.
    const bool uwl_excl_on = ewl_carry_exclude && nmw.uwl_enable;
    const char uwl_gal_top = nmw.uwl_gal_top_band;
    const bool uwl_dual    = nmw.uwl_dual_top;   // increment 5: both {b_top, b_top2} uncarried
    auto isUncarriedThirdBand = [uwl_excl_on, uwl_gal_top, uwl_dual](
                                    char cons, const std::string& signal) -> bool {
        const char band = signal.size() >= 2 ? signal[1] : '\0';   // "L5Q" → '5'
        if (uwl_excl_on) {
            const auto tier = mwLaneTierOf(cons, uwl_gal_top, false, uwl_dual);
            return tier && (band == tier->b_top
                            || (tier->b_top2 != '\0' && band == tier->b_top2));
        }
        switch (cons) {
            case 'G': return band == '5';   // GPS  EWL = (L2,L5) ⇒ L5 uncarried
            case 'E': return band == '7';   // Galileo EWL = (E5b,E5a) ⇒ E5b(7) uncarried
            default:  return false;         // BDS/QZSS deferred; GLONASS excluded from the carry anyway
        }
    };
    NetworkMwWlLayer::Config mw_wl_cfg;
    mw_wl_cfg.enable             = mw_wl_on;
    mw_wl_cfg.min_arc_epochs     = config_.getNetworkConfig().mw_wl.min_arc_epochs;
    mw_wl_cfg.sigma_floor_cyc    = config_.getNetworkConfig().mw_wl.sigma_floor_cyc;
    mw_wl_cfg.max_rows_per_epoch = config_.getNetworkConfig().mw_wl.max_rows_per_epoch;
    mw_wl_cfg.debug_r_scale      = config_.getNetworkConfig().mw_wl.debug_r_scale;   // diagnostic probe (F6)
    NetworkMwWlLayer mw_wl_layer(mw_wl_cfg);
    if (mw_wl_on && mw_wl_cfg.debug_r_scale != 1.0)
        log->warn("Network (M5/WL-1): network.mw_wl.debug_r_scale = {} — DIAGNOSTIC probe, the WL "
                  "rows' Pz is scaled (not honest MW variance).  Do NOT grade this run.",
                  mw_wl_cfg.debug_r_scale);
    // EWL lane (card M5 WL-2): the same layer, run on the (band2, band5) pair with FFR
    // statistic-gated fixing.  The third band is uncarried (excluded above), so these rows
    // meet no opposing F6 prior; a passed FFR folds an integer-hard row.  It uses the
    // primary-sig1 cycle basis, as WL-1 does.
    const bool ewl_on = carry_invariant && nmw.ewl_enable;
    // Per-constellation top tier (card M5 UWL-1).  uwl_on generalizes the EWL lane's
    // hard-coded GPS band digits to the lane-tier table (Galileo UWL (E5b|E5,E5a)); the
    // cold fix also admits n=1 unlagged statistics to the decision on tiers with
    // λ ≥ uwl_cold_min_lambda_m (SPEC §3.3; GPS's 5.861 m EWL is excluded on purpose).
    const bool uwl_on      = ewl_on && nmw.uwl_enable;
    const bool uwl_cold_on = uwl_on && nmw.uwl_cold_fix;
    // 60-epoch age-bias fix: the bridge accumulates per-sample iono-de-biased GF, so every
    // read-time C_iono·Σc·ı̃ subtraction is disabled.  The two frames must not mix:
    // subtracting twice re-creates the bias with the opposite sign.
    const bool bridge_debias_on = ewl_on && nmw.bridge_sample_debias;
    // Mid-ladder bridge hop through E6 (motivated by the census): the WL decision is
    // chained through the Galileo (E6,E5a) λ 2.931 m rung; re-rounding at λ_MID removes the
    // top hop's λ-scale phase/iono amplification before it reaches the 0.751 m WL statistic.
    const bool uwl_mid_on  = uwl_on && nmw.uwl_mid_enable;
    // Per-cycle preferred ('8') / fallback ('7') Galileo top rung, plus the
    // untaken-sibling closure statistic 2·N(8,5) = N(7,5) (increment 5).  The hard gate
    // is off by default until the residual distribution is measured (SPEC
    // reconciliation note 2).
    const bool uwl_dual_on = uwl_on && nmw.uwl_dual_top;
    const bool uwl_sib_gate_on = uwl_dual_on && nmw.uwl_sib_gate;
    // Census recovery (v2): minority receiver-family primary pairs get their own lane
    // pair groups (an admissible set of census pairs instead of the single majority pair).
    const bool uwl_family_on = uwl_on && lane_on && nmw.uwl_family_pairs;
    NetworkMwWlLayer::Config ewl_cfg;
    ewl_cfg.enable              = ewl_on;
    ewl_cfg.min_arc_epochs      = nmw.ewl_min_arc_epochs;
    ewl_cfg.sigma_floor_cyc     = nmw.sigma_floor_cyc;
    ewl_cfg.max_rows_per_epoch  = nmw.max_rows_per_epoch;
    ewl_cfg.ffr_fix             = true;
    ewl_cfg.fix_sigma_cyc       = nmw.ewl_fix_sigma_cyc;
    ewl_cfg.fix_frac_cyc        = nmw.ewl_fix_frac_cyc;
    ewl_cfg.fix_weight_sigma_cyc= nmw.fix_weight_sigma_cyc;
    ewl_cfg.max_abs_xlin_m      = nmw.lane_max_abs_xlin_m;
    // With the card armed there is one statistic-layer instance per tabled
    // constellation, so the telemetry is per constellation (the F-A dump showed that
    // pooled lines hide which lane the events come from).  Otherwise a single
    // filterless instance runs (the legacy shape; for a GPS-only run the 'G'-filtered
    // instance is bit-identical to it, because the filter never fires).  An armed
    // constellation that decides 0 cycles still prints its 0 line (invariant #4).
    std::vector<EwlLane> ewl_lanes;
    if (ewl_on) {
        if (uwl_on) {
            for (const char cons : { 'G', 'E' }) {
                const auto tier = mwLaneTierOf(cons, nmw.uwl_gal_top_band,
                                               nmw.uwl_mid_enable, nmw.uwl_dual_top);
                if (!tier) continue;
                NetworkMwWlLayer::Config cfg = ewl_cfg;
                cfg.cons_filter = cons;
                // The preferred rung's instance runs first, so its ewl_fixed entries
                // precede the fallback's and the walk's dedupe keeps the wider-λ
                // decision.  The fallback instance decides only what the preferred one
                // did not: a cycle with any arc lacking the preferred band ends there
                // as skip_missing_band, the per-cycle "where tracked" resolution
                // (increment 5).
                ewl_lanes.push_back(EwlLane{ cons, tier->b_top, NetworkMwWlLayer(cfg), {}, {} });
                if (tier->b_top2 != '\0')
                    ewl_lanes.push_back(EwlLane{ cons, tier->b_top2,
                                                 NetworkMwWlLayer(cfg), {}, {} });
            }
        } else {
            ewl_lanes.push_back(EwlLane{ '\0', '\0', NetworkMwWlLayer(ewl_cfg), {}, {} });
        }
    }
    // WL lane (card M5 WL-2, SPEC §3.3/§3.4): the phase-bridge WL decision plus the
    // NL-preserving inject.  The bridge path needs the EWL integers, so it runs only when
    // both masters are on.  wl_inject alone would run the MW(1,2) fallback path (§3.5),
    // which is deferred: v1 requires the bridge, and without it no WL decision is produced
    // and the lane is armed but inert.  WL-3 supersedes the WL-2 hold/inject walk at
    // runtime (the loader warns): under lane_carry the decided integers become
    // fold-on-change lane fixes on the pair blocks, and the inject primitive remains only
    // as the pair-block fallback.
    const bool wl_inject_on = ewl_on && nmw.wl_inject_enable && !lane_on;
    // Per-epoch lane measurement sources (WL-3).  The FFR fix walk runs whenever the
    // joint lane shape is armed (fold-on-change is the §3.3 core and has no separate knob).
    const bool lane_meas_on   = lane_on && lane_joint && lane_cfg.lane_meas;
    const bool lane_bridge_on = lane_on && lane_joint && lane_cfg.bridge_meas;
    const bool lane_fix_on    = lane_on && lane_joint;
    // Fan the MW lane-increment folds across components (byte-identical to serial).
    const bool lane_parallel_components = lane_on && lane_joint && lane_cfg.parallel_components;
    // One decided WL inject: the two bands' carry groups, the shared chord ArcKey, the integer
    // target, the per-band linearization DD sums (F5 correction frame) and the wavelengths.
    // Computed in buildCarryDdPriors from detector statistics (§3.0.1 decision source) and
    // applied after the solve against the fresh posterior, so the preserved NL sum is this
    // epoch's.  Refreshed each build; the last build before the accepted solve wins, as for
    // ewl_epoch.
    std::vector<WlInjectPlan> wl_inject_epoch;   // per-build; consumed post-installPosterior.
    // F-A attribution dump (network.mw_wl.d5_dump, log-only): adjacency tags for each D5
    // flag.  They are the (station, sat) pairs an EWL-fixed cycle touched this build, the
    // chords of hold-folded cycles this build, and the chords injected in the previous epoch
    // (an inject's residual tension lands in the next install; the injected chord itself is
    // exempt, its component siblings are not).  All are rebuilt per accepted build and stay
    // empty unless d5_dump is set.
    const bool mw_d5_dump = (ewl_on || wl_inject_on || lane_on) && nmw.d5_dump;
    // F-B lane trace (network.mw_wl.lane_trace, log-only): per epoch, the carried WL lane
    // value of every primary cycle whose chord is carried in both bands.  The constrained
    // (held or injected) and unconstrained populations come from one run, so the
    // injected-cycle circR can be graded (scripts/grade_lane_trace.py).  Entries are built
    // per accepted build and written after the install against the fresh posterior.
    const bool mw_lane_trace = (wl_inject_on || lane_fix_on) && nmw.lane_trace;
    const bool mw_tagsets    = mw_d5_dump || mw_lane_trace;   // hold/inject tag sets feed both
    // Both dumps are ordinary NetworkRunTraces streams, opened once (truncating, with
    // their header) before the epoch loop.  The open sits here rather than in openAll
    // because the arming expressions above are not yet derived at that point.  The D5
    // arm includes carry_innov_gate for the same reason its writer does: without the gate
    // there are no flags, so an unarmed gate yields no file rather than an empty one, and
    // the artifact set shows what actually ran.
    traces.openMwWlDumps(mw_d5_dump && carry_innov_gate, mw_lane_trace, log);
    std::vector<LaneTraceEntry> lane_trace_epoch;   // per-build; consumed post-install.
    WlCounters wl_run, wl_epoch;
    // Per-constellation WL-walk tallies when the card is armed (pooled lines hide
    // which lane the events come from).  wl_run stays the aggregate; the per-epoch
    // constellation tallies fold into both at the accepted-install accumulation.
    std::map<char, WlCounters> wl_cons_epoch, wl_cons_run;
    // Accepted-install accumulation helper, kept in one place so the field list cannot
    // drift between two copies.
    auto wlAccumulate = [](WlCounters& a, const WlCounters& b) {
        a.rows_seen           += b.rows_seen;
        a.cycles_decided      += b.cycles_decided;
        a.holds_folded        += b.holds_folded;
        a.injects             += b.injects;
        a.inject_abs_sum      += b.inject_abs_sum;
        a.skip_bridge_missing += b.skip_bridge_missing;
        a.skip_no_iono        += b.skip_no_iono;
        a.skip_bridge_wide    += b.skip_bridge_wide;
        a.skip_not_fixed      += b.skip_not_fixed;
        a.skip_closure        += b.skip_closure;
        a.skip_quarantine     += b.skip_quarantine;
        a.skip_basis          += b.skip_basis;
        a.sum_bridge_R        += b.sum_bridge_R;
        a.lane_mw_rows        += b.lane_mw_rows;
        a.lane_bridge_rows    += b.lane_bridge_rows;
        a.lane_fix_folded     += b.lane_fix_folded;
        a.lane_fix_unchanged  += b.lane_fix_unchanged;
        a.lane_skip_meas      += b.lane_skip_meas;
        a.lane_pinned_skips   += b.lane_pinned_skips;
        a.cold_bridge_cycles  += b.cold_bridge_cycles;
        a.mid_decided         += b.mid_decided;
        a.mid_fallback        += b.mid_fallback;
        a.mid_closure         += b.mid_closure;
        a.mid_disagree        += b.mid_disagree;
        a.top2_decided        += b.top2_decided;
        a.dual_dedupe         += b.dual_dedupe;
        a.sib_checked         += b.sib_checked;
        a.sib_unavail         += b.sib_unavail;
        a.sib_int_off         += b.sib_int_off;
        a.sib_blocked         += b.sib_blocked;
        a.sib_abs_frac_sum    += b.sib_abs_frac_sum;
    };
    // FDMA-correct per-signal wavelengths for the WL/EWL-row coefficients and z.  Built when
    // any lane layer is armed (mw_wl_on, ewl_on or lane_on), as ar_ppp_utils is for AR.
    std::unique_ptr<PppUtils> mw_wl_ppp_utils;
    if (mw_wl_on || ewl_on || lane_on) mw_wl_ppp_utils = std::make_unique<PppUtils>(log_manager_);
    NetworkMwWlLayer::Counters mw_wl_run;    // run-total eligibility/skip tallies (invariant #4)
    NetworkMwWlLayer::Counters mw_wl_epoch;  // per-build tallies (the last build before the accepted solve wins)
    // One-shot F6 diagnostic (card M5 WL-1): a per-arc, per-row attribution dump of the
    // WL rows of the first arming epoch.  Populated in buildCarryDdPriors at build time
    // (z_MW, z_lin, z_corr and the per-arc MW̄ and x_lin, where the resolver and revmap
    // are in scope) and finalized after the solve (reconstructed_abs from
    // reconstructStation plus the kernel's WL-block innovation).  Log-only; off by
    // default.
    const bool mw_wl_f6_dump = mw_wl_on && config_.getNetworkConfig().mw_wl.f6_dump;
    F6Capture mw_wl_f6;
    // Emit-layer frame re-base (card 4 phase D).  It re-references the emitted
    // per-(sat,σ) phase biases onto the previous emission frame, so the per-epoch gauge
    // churn of the freely re-realized eq34 datum does not send the consumer spurious
    // ambiguity resets; the integer step of a genuine reference-path slip is left
    // intact, so the emitter jump test still fires.  On by default when carry_invariant
    // is armed (spec D7); the off path is carry_invariant=false, which never touches
    // the engine (byte-identical).
    const bool emit_rebase_on = carry_invariant && config_.networkCarryInvariantEmitRebase();
    NetworkEmitRebase emit_rebase_engine;
    // PB emission-continuity filter (an M5 prerequisite): a slip-gated per-key scalar KF
    // on the re-based emitted biases, plus a self-tuning k·σ̂_step jump gate that clears
    // the threshold-in-noise discontinuity floor.  It runs inside the re-base block (it
    // filters the re-based emission frame), so it requires emit_rebase_on.  Off by
    // default ⇒ filterEpoch is never called; the engine is constructed like
    // emit_rebase_engine but stays inert (byte-identical).
    const bool emit_pb_filter_on = emit_rebase_on && config_.networkEmitPbFilter();
    NetworkPbEmitFilter::Config pb_filter_cfg;
    pb_filter_cfg.q_m2_s         = config_.networkEmitPbFilterQ();
    pb_filter_cfg.jump_k         = config_.networkEmitPbFilterJumpK();
    pb_filter_cfg.meas_sigma_m   = config_.networkEmitPbFilterMeasSigmaM();
    pb_filter_cfg.max_gap_s      = config_.networkEmitPbFilterMaxGapS();
    pb_filter_cfg.window_epochs  = config_.networkEmitPbFilterWindowEpochs();
    pb_filter_cfg.gate_v2        = config_.networkEmitPbFilterGateV2();  // PB-gate-v2 merge point (leaf wiring)
    NetworkPbEmitFilter pb_filter_engine(pb_filter_cfg);
    const bool pb_filter_emit_sigma = config_.networkEmitPbFilterEmitSigma();
    // Clock emission re-base (card CLK-R): the common-mode frame hold (stage 1) and the
    // per-satellite step marks (stage 2).  A run-TU local like the PB engines
    // (NetworkRunContext banner rule).  Both knobs are off by default ⇒ rebaseEpoch is
    // never called (byte-identical).  It affects outputs only: the solve is
    // byte-identical even when armed.
    NetworkClockEmitRebase::Config clk_rebase_cfg;
    clk_rebase_cfg.frame_hold = ctx.knobs.clk_frame_hold;
    clk_rebase_cfg.leash_m    = config_.networkClockFrameLeashM();
    clk_rebase_cfg.jump_test  = ctx.knobs.clk_jump_test;
    clk_rebase_cfg.jump_m     = config_.networkClockDiscJumpM();
    clk_rebase_cfg.gap_jump_m = config_.networkClockDiscGapJumpM();
    NetworkClockEmitRebase clk_rebase_engine(clk_rebase_cfg);
    auto& carry_sat_vertex = ctx.carry.carry_sat_vertex;
    auto& carry_group_id = ctx.carry.carry_group_id;
    auto& carry_prev_tow = ctx.carry.carry_prev_tow;
    auto& carry_innov_pending = ctx.carry.carry_innov_pending;
    const bool phase_datum_prioritized = ctx.knobs.phase_datum_prioritized;
    // Seed-once and time-lock for the ensemble datum (fix A,
    // network.phase_datum_hold_sigma_m).  Without the hold, the ensemble references the
    // emitted bias to mean_r(ẑ_r), a float-ambiguity mean re-realized every epoch whose
    // absolute level is limited to code/iono precision, so the product moves by about a
    // metre per epoch (a dynamic datum on a static product).  With the sigma > 0, the
    // gauge is seeded once per (sat,signal), and from then on the shared bias is held by
    // a stiff temporal prior of that sigma: the Σz=0 fold is skipped for a (sat,signal)
    // that already has a carried bias, and the D2 stash carries that bias with a fixed
    // hold variance instead of dropping it.  0 = re-realize every epoch (the churning
    // baseline); inert unless phase_datum_ensemble.
    const bool   phase_hold_on    = ctx.knobs.phase_hold_on;
    // The assembly helpers are Application member functions in
    // ApplicationNetworkAssembly.cpp: networkIsGaugePinned_, networkSplitStation_,
    // networkAppendGimIonoRows_ and networkAppendShIonoRows_ (formerly the lambdas
    // isGaugePinned, splitStation, appendGimIonoRows and appendShIonoRows).  Their former
    // [&] captures are explicit parameters or member reads.
    auto& sh_dev_carry = ctx.carry.sh_dev_carry;
    const double sh_dev_tau = sh_model ? shc.rep_error_tau_s : 0.0;
    const bool   sh_dev_on  = sh_dev_tau > 0.0;
    const bool   sh_dev_demean = sh_dev_on && shc.rep_error_demean;  // anti-ratchet de-mean (Iono-2 v3)
    const bool   sh_dev_d5_keep = sh_dev_on && shc.rep_error_d5_keep;  // erase-policy split (v3k)

    // ---- Station-adaptive SH-VTEC-constraint deweight (iono deweight card) ----------
    // A per-station factor w_s ∈ (0,1] scales the information of the station's SH↔slant
    // iono pseudo-obs (σ² ×= 1/w_s ⇒ info ×= w_s).  w_s→0 decouples the station's per-LOS
    // iono from the global SH field in both directions (there is a single joint
    // pseudo-obs, so one w_s covers both), while the station still serves the shared
    // clocks/biases/datum/position.  All of the gate math (v1 static list, v2
    // network-relative IGG auto-trigger, EWMA, hysteresis, slew) lives in
    // NetworkIonoDeweightGate.  The serial post-solve harvest feeds it per-station
    // statistics, and the parallel assembly reads iono_deweight.weight() (a const find,
    // race-free under the sh_dev_carry contract).  An empty static list with adaptive off
    // ⇒ weight() is 1.0 for every station ⇒ byte-identical to off (invariant #6).
    const IonoConstraintDeweightConfig& idw = config_.getIonoConstraintDeweightConfig();
    NetworkIonoDeweightGate iono_deweight(
        std::set<std::string>(idw.stations.begin(), idw.stations.end()), idw.factor,
        idw.adaptive, idw.window_epochs, idw.sigma_hi, idw.sigma_lo, idw.floor,
        idw.rate_limit, idw.min_epochs);
    // Iono-deweight release hygiene: the gate's per-epoch release-edge capture is armed
    // only when a response is armed, so an unarmed run never allocates the list.  The
    // edge is "the weight rose above eps", captured around the rate-limited slew.  The
    // engage/release transition list cannot serve it: a station climbs off the floor
    // with no flag flip whenever its z decays inside the hysteresis band, and
    // Transition::w is post-slew only.
    if (ctx.knobs.dw_hygiene_on)
        iono_deweight.enableReleaseTracking(ctx.knobs.dw_release_eps);
    const bool idw_on = iono_deweight.active();
    if (idw_on && !sh_model)
        log->warn("Network (iono deweight): configured but network.estimate_iono_sh is OFF — "
                  "there is no SH field to deweight; the setting is inert this run.");
    if (sh_model && iono_deweight.staticCount() > 0) {
        std::set<std::string> idw_present;
        for (const auto& st : stations) idw_present.insert(st.name);
        for (const auto& nm : iono_deweight.staticStations())
            if (!idw_present.count(nm))
                log->warn("Network (iono deweight): statically-listed station '{}' is not in this "
                          "network — ignored (check the name).", nm);
    }

    // appendShIonoRows is Application::networkAppendShIonoRows_ in
    // ApplicationNetworkAssembly.cpp; sh_model, iono_deweight, shc and the sh_dev state
    // are passed explicitly at the PASS-B call site.

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
    // that entered processing (past the max-duration cutoff).
    const auto t_loop_begin = std::chrono::steady_clock::now();
    auto& n_loop_epochs = ctx.counters.n_loop_epochs;
    auto& decim_counter = ctx.counters.decim_counter;
    auto& sh_converged_latch = ctx.carry.sh_converged_latch;

    // The joint estimator is constructed once, outside the epoch loop, and reset() and
    // reused each epoch.  A fresh NetworkEstimator per epoch reallocates one dense Schur
    // block per station every epoch, and the process heap retains that churn as committed
    // memory, saturating at ~0.5 GB per station (≈146 GB at 305 stations over a
    // 121-epoch run, which exhausts RAM).  Reusing the estimator's station-block pool
    // (NetworkNormalEquations::reset) holds the footprint to one epoch's working set.
    NetworkEstimator est;
    // Opt-in fan-out of the Schur reduction, kept separate from the build-phase
    // parallelism.  The build's serial-ordered scatter is byte-identical to a serial
    // run, but the reduce fan-out reassociates the per-station summation: reproducible
    // for a given thread count, yet ULP-different from the serial reduce, and the
    // network's ill-conditioning amplifies that to ~1e-5 in the products (physically
    // null, but not byte-identical).  Off by default, which keeps the reduce serial while
    // the build still threads and the deterministic serial-tail savings (eigensolver
    // gate, no double reduce, factor reuse) still apply.
    //
    // parallel_reduce and parallel_carry are mutually exclusive: the first fans
    // reduce()'s per-station Schur loop, the second fans the fold's per-station
    // reduction while est.solve()'s dense algebra is itself Eigen-threaded, so arming
    // both nests OpenMP inside Eigen and oversubscribes.  The loader resolves it
    // (parallel_carry is forced off when both are set), so at most one is true here;
    // the `!carry_par` below is a redundant guard.
    const bool reduce_par   = parallel_on && nc.parallel_reduce && !carry_par;
    const int  reduce_threads = reduce_par ? n_threads : 1;
    est.setReduceThreads(reduce_threads);
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
    if (reduce_par)
        log->info("Network: Schur reduce PARALLEL across {} threads "
                  "(serial≡parallel byte-identity intentionally relaxed to ~1e-5)",
                  n_threads);

    // === Tier D0: per-epoch section timers (network.trace_section_timers) =====
    // Arms the estimator's wall-clock accumulators around assembly, reduce, solve, the
    // shared-covariance full inverse and the per-station covariance consumers, so a
    // profiling run shows which term dominates the per-epoch cost (and so which tier-D
    // scaling lever is worth pulling).  The estimator accumulates continuously across the
    // run; the driver snapshots it per epoch for the per-epoch delta line and reads the
    // run total at exit.  Off (default) ⇒ no clock reads, byte-identical.
    const bool sec_timers_on = nc.trace_section_timers;
    est.setPerfTiming(sec_timers_on);
    est.resetSectionTimers();
    // The carry driver's per-op timers are armed on the same toggle (INC-C0).  They split
    // the SB_DDBUILD and SB_INSTALL wall brackets into basis projection, posterior
    // read-back and D5 gate (the dense K² operations the perf-restructure card ranks).
    // Byte-identical off.
    carry_driver.setPerfTiming(sec_timers_on);
    carry_driver.resetPerfTimers();
    double sec_asm_wall_ms = 0.0;      // run-total driver-side assembly wall, produce + serial scatter (D0)
    std::size_t sec_profiled_epochs = 0;  // epochs that reached the post-solve per-epoch readout (D0)
    if (sec_timers_on)
        log->info("Network (D0): per-epoch section timers ARMED "
                  "(assembly / reduce / solve / cov-inverse / cov-consumers).");


    // === Tier D0b: sub-instrumenting the "other" bucket (network.trace_section_timers) ==
    // "other" = loop wall − assembly − reduce − solve − cov-inverse − cov-consumers.  It
    // was measured at ~81 % of the epoch on the 300 s GPS-only tier and scales with LOS
    // count.  These per-block driver-side wall timers attribute it.  For each block,
    // net = wall − Δ(reduce + solve + cov-inverse + cov-consumers) incurred inside it, so
    // recon and solve time already counted in the five D0 sections is not counted twice:
    // net is the block's own contribution to "other" (a block that reconstructs N
    // stations shows its driver glue as net and its O(nₛ²) recon as cov-consumers).  The
    // brackets are disjoint and tile the loop body between the assembly wall and the
    // emitted-epoch readout; the residual (other − Σ net) is logged as "unattributed".
    // With the toggle off, the subBegin/subEnd lambdas return early (no clock reads, no
    // est.sectionTimers() calls), so the output is byte-identical to an untimed run.
    enum SubBlock {
        SB_SLIP = 0,     // block 1: PASS-A slip scans (MW refloat + IF-TD jumps), pre-assembly
        SB_PREASM,       //          pre-assembly measurement build (post-slip → assembly wall)
        SB_GAUGEBUILD,   //          buildEnsembleGauges (Σz=0 gauges); the ensemble datum is off in the tier
        SB_DDBUILD,      //          buildCarryDdPriors: eq34 arc graph + carry_driver.buildDdPriors
                         //          (W·Pᶻ·Wᵀ dense DD-cov projection over K carried chords) (card 4)
        SB_SOLVEGLUE,    //          pre-solve prep + clock predict + solve + within-epoch re-solve
                         //          (net removes the internal solve/reduce)
        SB_INSTALL,      //          carry_driver.installPosterior (DD posterior mean/cov readback) (card 4)
        SB_NETAR,        //          producer-side network AR, phase 2 (log-only; off in the tier)
        SB_SOLPREP,      //          toSolutionEpoch + per-sat discontinuity signalling
        SB_EMITSTORE,    // block 7: emit re-base + PB-emit filter + store push + solution CSV
        SB_IONODIAG,     // block 4: iono / ZWD / anchor-info viz traces (per-station recon sweeps)
        SB_DDFRAC,       // block 6: cross-station DD fractional-residual (B5) trace
        SB_SHDEV_VTEC,   // block 4: T5 SH-deviation carry KF + co-estimated VTEC coeff trace
        SB_T1A,          // block 3: T1a SH-field health probe + fused H1 adaptive-deweight gate
        SB_LEVELING,     // block 5: phase-leveling carry harvest + D2/C1 stash + T1d SH-conv
        SB_RAIM,         // block 2: post-fit network RAIM pass-1/2 + median-polish attribution
        SB_CLOCKUPD,     // block 8: two-state clock update + fault QC + retro-flag feed
        SB_FRAMEDIAG,    // shared-normal spectrum + post-fit sigma0, both off by default (P1-3/P1-4)
        SB_STASH_STATUS, // block 5/8: D2/C1/SH carry re-stash + VCE fold + epoch-status QC
        SB_ROSTER,       // signal-roster build and arc-ledger advance at the top of the
                         //       epoch, and the disagreement pass after the clock update
                         //       (card SID phase 1).  Both brackets accumulate into one
                         //       entry: they are the same observatory, and the cost of
                         //       interest is what the card adds to an epoch.
        SB_FASTCLOCK,    // fast-clock anchor harvest + the shadow-mode grading tick (FC-1).
                         //       The tick on decimated-out epochs is outside the loop and
                         //       untimed here; it has its own ctx.counters.fc_wall_ms
                         //       accumulator, because a tick is not a loop epoch and must
                         //       not enter the D0 wall.
        SB_COUNT
    };
    static constexpr const char* kSubNames[SB_COUNT] = {
        "slip-scans", "pre-asm", "gauge-build", "dd-build", "solve-glue", "install-post",
        "netar", "sol-prep", "emit-store", "iono-diag", "dd-frac", "shdev-vtec",
        "t1a-health", "leveling", "raim", "clock-upd", "frame-diag", "stash-status",
        "roster", "fast-clock"
    };
    std::vector<SubBlockTimer> sec_sub(SB_COUNT);        // run totals
    std::vector<SubBlockTimer> sec_sub_ep0(SB_COUNT);    // per-epoch loop-top snapshot of sec_sub
    std::vector<std::chrono::steady_clock::time_point> sub_wall0(SB_COUNT);
    std::vector<NetworkNormalEquations::SectionTimers>  sub_tmr0(SB_COUNT);
    auto subBegin = [&](SubBlock b) {
        if (!sec_timers_on) return;
        sub_wall0[b] = std::chrono::steady_clock::now();
        sub_tmr0[b]  = est.sectionTimers();
    };
    auto subEnd = [&](SubBlock b) {
        if (!sec_timers_on) return;
        const auto& t = est.sectionTimers();
        sec_sub[b].wall_ms += std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - sub_wall0[b]).count();
        sec_sub[b].timed_ms +=
            (t.reduce_ms        - sub_tmr0[b].reduce_ms)
          + (t.solve_ms         - sub_tmr0[b].solve_ms)
          + (t.cov_inverse_ms   - sub_tmr0[b].cov_inverse_ms)
          + (t.cov_consumers_ms - sub_tmr0[b].cov_consumers_ms);
        sec_sub[b].covcon_ms += t.cov_consumers_ms   - sub_tmr0[b].cov_consumers_ms;
        sec_sub[b].covcalls  += t.cov_consumer_calls - sub_tmr0[b].cov_consumer_calls;
    };

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
                         &clk_rebase_engine,    // the CLK-R held frame is restored too (§18.10),
                         &emit_rebase_engine,   // as are the two phase-bias emit layers,
                         &pb_filter_engine);    //   for the same reason (RS-5 §21.10)
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
    // windows, and records the station names.  Disarmed (network.signal_roster absent),
    // every member of ctx.roster and ctx.arc_ledger is untouched and both seams are
    // unreachable.
    networkSignalRosterArm(ctx, config_, log, stations);
    // Emitter read against the ledger, and the wire declaration (card SID phase 5a,
    // docs/refound/SID_signal_identity_2026-09-22.md section 4).  Disarmed
    // (network.ledger_emit absent), every member of ctx.ledger_emit is untouched and
    // all three seams are unreachable.
    networkLedgerEmitArm(ctx, config_, log);

    // The a-priori satellite-clock anchor σ is resolved once (it is loop-invariant and
    // config-only), so the anchor fold and its information-fraction diagnostic share
    // one value.  The policy (explicit > 0 wins; brdc_rts auto → 3.0 m; sp3 auto →
    // initial_std.sat_clock) lives in GnssConfig as a single unit-testable source of
    // truth.  The < 100 m binding gate stays at the fold site (NetworkSolveGlue.cpp).
    const double clk_anchor_sigma = config_.resolveNetworkAprioriClockAnchorSigmaM();

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
    const bool streaming_on = nc.streaming.enable;
    if (streaming_on && config_.networkCbAdmission()
        && config_.estimateNetworkSatCodeBias())
        log->warn("Network (RTF-3): network.cb_admission census SKIPPED under "
                  "streaming — it would classify over the metadata RINEX files, "
                  "not the live epochs.  Falling back to the legacy per-epoch "
                  ">= 2-station gate.");
    if (!streaming_on && config_.networkCbAdmission()
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
                ConsTally& t = per_cons[p.satellite.empty() ? '?' : p.satellite[0]];
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

    // === One synced epoch through the full producer (RTF-3 carve-out) =========
    // The per-epoch body as a callable over `se` alone; every other name it reads is
    // loop-invariant setup or ctx-carried run state, captured by reference.  Loop
    // control maps one to one (a top-level `continue` becomes return Next, the
    // max-duration `break` becomes return Stop), so the batch iteration below is
    // structurally identical to an inline loop.  The callable exists so a streaming
    // epoch source (network.streaming, the RTF-3 multi-station front-end) can drive the
    // identical per-epoch body without a pre-built synced vector.
    enum class LoopSignal { Next, Stop };
    auto processSyncedEpoch = [&](const SyncedEpoch& se_raw) -> LoopSignal {
        // Per-epoch workspace (INC-4): the RAIM-replay capture channel and the
        // clock-model prediction maps.  It is constructed fresh every iteration, and
        // that construction is the per-epoch reset; see EpochWorkspace in
        // filter/NetworkRunTypes.hpp for the writer-site contract.
        EpochWorkspace ws;
        // Optional testing fast-forward (network.start_offset_hours): epochs are skipped
        // until the elapsed data span from the first synced epoch reaches the offset, and
        // the solve cold-starts there.  Skipped epochs are not counted, modeled, solved or
        // stashed, so the carry begins empty at the offset.  This reproduces an externally
        // driven late-hour event (for example the ~4.3 h lock-in) after a fresh warm-up,
        // not the exact accumulated history.
        if (start_offset_s > 0.0) {
            const double now_s = TimeUtils::continuousGpsSeconds(se_raw.time);
            if (start_anchor_s < 0.0) start_anchor_s = now_s;
            if (now_s - start_anchor_s < start_offset_s) return LoopSignal::Next;
        }
        // The log prefix is bound before the injector runs, so the injector's own
        // warnings carry this epoch's bracket, not the previous one.  se.time ==
        // se_raw.time always (the injector never touches the time tag), so the
        // position of this call affects log text only.
        LogManager::setCurrentEpochTime(se_raw.time);
        // Event-harness seam (filter/NetworkInject.cpp), the single observation
        // injection site.  It is upstream of networkFastClockTick, computeModels, every
        // T0 detector, networkComputeRawObservables and assembly, so a one-scalar write
        // on the RinexObservation (phase in cycles) moves the metre-valued design row
        // and the cycle-valued MW/GF/IF-TD feed together.  It sits after the
        // start-offset gate so a fast-forwarded epoch cannot consume an event.
        // Unarmed, it returns se_raw itself (no copy, no branch, byte-identical).
        // `se` shadows nothing; the whole body below reads it.
        SyncedEpoch se_inject_scratch;
        const SyncedEpoch& se_admit = networkInjectEpoch(ctx, log, traces, se_raw,
                                                         se_inject_scratch);
        // Canonical-pair admission seam (filter/NetworkCanonicalPairAdmission.cpp,
        // card CPA).  It sits immediately after the injector, on the same contract,
        // because this is the only site upstream of everything the withhold must be
        // absent from: computeModels, the T0 detectors, networkComputeRawObservables,
        // assembly, the RAIM pools and the traces all derive from this
        // RinexObservation.  Dropping the record here is therefore equivalent to a
        // missing RINEX record, which lets the row-level gap re-entry tier read the
        // satellite's return as a re-entry.  It screens the producer roster only, so
        // the held-out closed-loop consumer is untouched.  Disarmed, it returns
        // se_admit itself (no copy, no walk, byte-identical).
        SyncedEpoch se_cpa_scratch;
        const SyncedEpoch& se = networkCanonicalPairAdmission(ctx, log, traces, stations,
                                                              se_admit, se_cpa_scratch);

        // Signal-roster seam (app/SignalRoster.cpp, card SID phase 1).  It sits
        // immediately after the canonical-pair admission returns the epoch, for the
        // reason given above: this site is upstream of computeModels, the T0
        // detectors, networkComputeRawObservables, assembly, the RAIM pools and the
        // traces, so the roster answers "which signal stands for this band at this
        // station" from the same RinexObservation all of them derive from.  It fills
        // ctx.roster and advances ctx.arc_ledger.  In phase 1 only the disagreement
        // counters and the trace read either object, so with the roster alone armed
        // the solve, the wire and every graded trace are byte-identical; later SID
        // phases (the ledger re-entry response, the anchor hygiene, the ledger emit)
        // also read them.  Disarmed, it returns on its first line (no walk, no
        // allocation, no counter moved).
        subBegin(SB_ROSTER);
        networkBuildSignalRoster(ctx, config_, log, traces, se, stations);
        subEnd(SB_ROSTER);

        // Reduced-RHS class attribution: the estimator's shared-info capture recorder
        // is armed before any prior-inject site runs this epoch (the post-solve seam
        // reads the captures).  One branch per epoch when disarmed.
        //
        // With the flight recorder armed (rhs_attrib_on_trip) the recorder runs on
        // every epoch, because a trip is known only after the solve and a trip must be
        // able to attribute its own epoch.  The cost is ~10 vectors x n_shared doubles,
        // cleared at each beginEpoch, which is negligible next to the solve.
        est.attribArm(nc.rhs_attrib_on_trip
                      || (nc.rhs_attrib_tow >= 0.0
                          && se.time.tow >= nc.rhs_attrib_tow - 0.25
                          && ctx.carry.rhs_attrib_done < std::max(1, nc.rhs_attrib_n_ep)));
        // The per-epoch trip handoff is a one-epoch value.  It is cleared here, at the
        // top of the epoch, so a trip raised by a site that runs before the
        // SB_FRAMEDIAG consumer cannot leak into the next epoch's dump.
        ctx.carry.rhs_trip_pending.clear();

        // Datum graph census: the spectral hand-off is also a one-epoch value.  An
        // epoch that skips, withholds or throws before the rank gate would otherwise
        // leave the previous epoch's eigenvalue count in place, and the next census
        // pass would reconcile this epoch's topology against it with no column saying
        // so.  Clearing it here, at the top, means the census only ever reads an
        // observation from its own epoch.
        if (ctx.knobs.dg_on) ctx.carry.dg_spec.clear();

        // Feeds the staged broadcast nav records whose availability (toc, GPS scale)
        // this epoch has reached.  The store's current/previous retention then evolves
        // as under live arrival: IODE rolls happen at their true times (the card 6
        // re-levels fire), and the fit-window guard sees fresh records instead of the
        // end-of-day pair a bulk load would leave.  The first processed epoch catches up
        // over any start_offset span in one sorted batch, which ends in the same store
        // state as epoch-by-epoch feeding.
        if (brdc_rts) {
            auto& eph_store = LiveEphemerisStore::getInstance();
            const double now_s = TimeUtils::continuousGpsSeconds(se.time);
            while (brdc_nav_cursor < brdc_nav_staged.size()
                   && navAvailabilityGpsSeconds(brdc_nav_staged[brdc_nav_cursor]) <= now_s) {
                eph_store.insert(brdc_nav_staged[brdc_nav_cursor], /*from_igs=*/true);
                ++brdc_nav_cursor;
            }
        }

        // Optional max-duration cutoff (processing.max_duration_s): anchor on the
        // first synced epoch, stop once the elapsed data span reaches the limit.
        if (config_.getMaxDuration() > 0.0) {
            const double now_s = TimeUtils::continuousGpsSeconds(se.time);
            if (!max_dur_anchored) { max_dur_anchor_s = now_s; max_dur_anchored = true; }
            else if (now_s - max_dur_anchor_s >= config_.getMaxDuration()) {
                log->info("Network: reached max processing duration ({:.1f} s of data); stopping",
                          config_.getMaxDuration());
                return LoopSignal::Stop;
            }
        }
        // Epoch decimation (T2e): only every Nth synced epoch is processed (N=1 ⇒ the
        // modulo is always 0 ⇒ nothing is skipped ⇒ byte-identical).  It sits after the
        // start-offset and max-duration guards, so the physical-time window and break are
        // evaluated on every raw epoch, and before ++n_loop_epochs, so decimated-out
        // epochs are not counted and n_loop_epochs stays the processed-epoch count.  The
        // brdc_rts nav catch-up is idempotent across skips.
        {
            const size_t dn = static_cast<size_t>(std::max(1, nc.epoch_decimation_n));
            if ((decim_counter++ % dn) != 0) {
                // A decimated-out epoch has fully synced observations but is not
                // processed.  With the fast tier armed it runs a clocks-only fast tick
                // against the last emitted full solve instead.  Off ⇒ the bare Next
                // return below is reached unchanged (structurally byte-identical).  A
                // tick is not a loop epoch: n_loop_epochs is untouched, the D0/D0b
                // brackets never open, and nothing outside ctx.carry.fast_clock is
                // mutated (the isolation contract).
                if (fc_on) networkFastClockTick(ctx, config_, log, traces, setup, se, false);
                return LoopSignal::Next;
            }
        }
        ++n_loop_epochs;  // [TIMING]

        // Per-epoch section-timer baseline (D0).  Deltas of the estimator's run-cumulative
        // accumulators against this snapshot give this epoch's reduce, solve, cov-inverse
        // and cov-consumer cost; sec_epoch_t0 anchors the epoch wall, and
        // sec_asm_ms_epoch is filled at the assembly wall below.  Inert (no clock read)
        // when the toggle is off.
        NetworkNormalEquations::SectionTimers sec_t0;
        std::chrono::steady_clock::time_point sec_epoch_t0;
        double sec_asm_ms_epoch = 0.0;
        if (sec_timers_on) {
            sec_t0 = est.sectionTimers();
            sec_epoch_t0 = std::chrono::steady_clock::now();
            sec_sub_ep0 = sec_sub;   // per-epoch deltas of the sub-block run totals (D0b)
        }

        // Rotation of the unbounded service CSVs (P1); the body is in NetworkRunTraces
        // (INC-4).  Off ⇒ a byte-identical no-op.
        traces.rotateServiceCsvs(nc, log);

        // Health heartbeat (RTB-6); the body is in NetworkRunTraces (INC-4), and the
        // window anchors and last_health_write_s live in ctx.counters.
        traces.writeHealthRow(se.time, nc, ctx.counters, t_loop_begin,
                              ura_on, raim_on, pl_on, ura_sigma0_ewma,
                              last_pivot, grade_metric_s);

        // Fast-clock shadow mode (FC-1): the same tick body runs on this full epoch,
        // immediately before the full pipeline, so the trace carries a fast-vs-full delta
        // for every epoch of any run.  It is bracketed so its cost is attributed in the
        // D0b table instead of inflating `other`; the result is traced and discarded, so
        // the full pipeline stays byte-identical (gated at every increment).
        if (fc_shadow) {
            subBegin(SB_FASTCLOCK);
            networkFastClockTick(ctx, config_, log, traces, setup, se, true);
            subEnd(SB_FASTCLOCK);
        }

        // The row-writer bodies live in NetworkRunTraces (L1/L2); these thin forwarders
        // add the epoch time, so the downstream call sites pass only the row.
        auto emitEpochStatus = [&](const char* disposition, const EpochStatusRow& r) {
            traces.emitEpochStatus(se.time, disposition, r);
        };

        auto emitEvent = [&](const char* tier, const char* event,
                             const std::string& sat, const std::string& station,
                             std::optional<double> statistic, std::optional<double> threshold,
                             const char* action, const std::string& detail) {
            traces.emitEvent(se.time, tier, event, sat, station, statistic, threshold,
                             action, detail);
        };

        const double dt = (prev_tow < 0.0) ? 0.0
            : ((se.time.week - prev_week) * 604800.0 + (se.time.tow - prev_tow));

        // Phase-leveling carry timing (increment 1): the continuous-GPS seconds of this
        // epoch (for the per-ambiguity inter-epoch gap) and whether the cold-start warmup
        // has elapsed.  The warmup counts emitted product epochs, not raw loop epochs, so
        // the carry waits for the clock/SH convergence horizon; locking an unconverged
        // ambiguity onto a contaminated cold-start value would propagate that value.
        // n_emitted here excludes the current epoch, which is not yet emitted.
        const double pl_now_s   = TimeUtils::continuousGpsSeconds(se.time);
        // With convergence-gated warmup armed and an SH field active, the carry warms only
        // once the field has gone stationary (sh_converged_latch, set after the solve in
        // NetworkLeveling from the epoch-over-epoch coefficient RMS Δ and the a₀₀ physical
        // band); otherwise, including when there is no SH field to converge, the fixed
        // emitted-epoch count applies.  Off (default) ⇒ the exact epoch-count gate ⇒
        // byte-identical (T1d).
        const bool   carry_warm = pl_on && ((pl_warmup_on_conv && sh_model)
                                                ? sh_converged_latch
                                                : (static_cast<int>(n_emitted) >= pl_warmup));

        // This epoch's slip event feed for the DD invariant carry: the (app station index,
        // ambiguity key) pairs the PASS-A slip detectors flagged.  It is filled at the
        // detection site (the slip/NetworkSlipQc.cpp drains) rather than read from
        // pending-reseed at fold time, because PASS-B MeasurementModel::compute re-seeds
        // and clears the pending-reseed flag before buildCarryDdPriors runs (the flag is a
        // one-shot the assembly consumes).  It is recorded regardless of carry_warm (the
        // detectors run cold, and the DD carry is live from epoch 1), so the carry is
        // slip-protected through the whole warmup and not only after the SH field's
        // convergence gate releases.  carry_invariant only.
        std::set<std::pair<int, std::string>> carry_slipped_ambs;
        // The innovation gate's flags from the previous epoch seed this epoch's slip set (a
        // fine slip caught after last epoch's solve re-taps its arc fresh here, via the same
        // C3c membership-exclusion path the coarse detectors feed).  One-shot: consumed now.
        if (carry_innov_gate && !carry_innov_pending.empty()) {
            carry_slipped_ambs.insert(carry_innov_pending.begin(), carry_innov_pending.end());
            carry_innov_pending.clear();
        }
        // Retap-only drain (card 4 D5 v4, network.carry_invariant_innov_retap_only): the
        // corroborated-flat subset of those flags is drained in lockstep.  It is a strict
        // subset of what just went into the funnel (the entries stay in the funnel and
        // every side consumer is untouched), and it names the arcs whose raw MW/GF level
        // was measured and did not step, that is, the arcs with no integer jump for a
        // refloat to complete.  networkBuildCarryDdPriors spares exactly those from the
        // A_old membership exclusion, so a DD the gate re-tapped in-epoch keeps its history
        // instead of being reborn from a diffuse prior every epoch.  Cleared
        // unconditionally so a disarmed run cannot inherit a stale set.
        ctx.carry.carry_innov_flat_now.clear();
        if (!ctx.carry.carry_innov_pending_flat.empty()) {
            ctx.carry.carry_innov_flat_now.swap(ctx.carry.carry_innov_pending_flat);
            ctx.carry.carry_innov_pending_flat.clear();
        }
        // Erase-policy split (Iono-2 v3k, SPEC_iono2_mechB_v3 addendum): the raw-detector
        // portion of the funnel, captured at insertion.  PASS A fills this set and the
        // driver unions it into the funnel below, so every existing funnel consumer sees
        // the same union, while the shdev pre-pass can tell raw-confirmed slips (erase)
        // from D5-only re-taps.  97 % of D5-only re-taps have no raw observable step: they
        // are iono-tension events the deviation should absorb rather than be erased by.
        std::set<std::pair<int, std::string>> carry_slipped_raw;
        // Iono-deweight release hygiene (INC-1): the previous epoch's release-edge
        // refloats are drained here.  The edge is detected after the solve (SB_T1A, inside
        // the gate's own slew); by then this set's load-bearing consumer has already run,
        // and a bare pending-reseed mark would be wiped by the leveling harvest's
        // clearPendingReseed.  The refloat is therefore deferred to here, as the D5
        // innovation gate defers its post-solve flags above.  It goes into the raw subset
        // because a re-coupled parked slant is a raw-observable-class event: the union
        // below carries it into the funnel for every existing consumer, and the raw
        // provenance keeps the Iono-2 v3k d5_keep policy from restoring the deviation
        // level this response discards.  The pending-reseed mark is also issued here
        // (nothing between here and PASS B clears it), so the ambiguity re-seed reads the
        // already-reseeded slant.
        if (!ctx.carry.dw_release_pending.empty()) {
            for (const auto& sk : ctx.carry.dw_release_pending) {
                carry_slipped_raw.insert(sk);
                if (sk.first >= 0 && sk.first < static_cast<int>(stations.size()))
                    stations[static_cast<std::size_t>(sk.first)]
                        .sm->markAmbiguityPendingReseed(sk.second);
            }
            ctx.counters.dwr.refloats_drained +=
                static_cast<long long>(ctx.carry.dw_release_pending.size());
            ctx.carry.dw_release_pending.clear();
        }
        // RAIM pool re-host: the previous epoch's post-fit RAIM flags are drained here.
        // The deferral is the same as for the two drains above, for the same reason: RAIM
        // detects after the solve, at SB_RAIM, when this funnel's load-bearing consumer
        // (buildCarryDdPriors, SB_DDBUILD) has already run.
        //
        // The flags go into `carry_slipped_ambs` and not into `carry_slipped_raw`.  The
        // raw subset means a raw-observable step, which the Iono-2 v3k d5_keep erase
        // classifier and the WL-3 MW-row slip-skip both read.  A post-fit residual
        // consensus is estimation-side evidence, the same class as the D5 innovation
        // gate, which also drains into the funnel only.  Draining RAIM into raw would
        // change the Iono-2 deviation-erase policy with no visible sign.
        //
        // The pending-reseed mark is not re-issued here (unlike the dw-release drain,
        // whose slant rewrite must be visible to the ambiguity re-seed): the seam already
        // set it at detection time, and on this lineage it is bookkeeping either way.
        //
        // The clear is unconditional: the one-epoch lifetime bound (standing invariant #1)
        // keeps a stale (station index, key) pair from outliving its arc across a datum
        // change or a station drop.
        if (!ctx.carry.raim_refloat_pending.empty()) {
            for (const auto& sk : ctx.carry.raim_refloat_pending)
                carry_slipped_ambs.insert(sk);
            ctx.counters.raim_ci.refloats_drained +=
                static_cast<long long>(ctx.carry.raim_refloat_pending.size());
            ctx.carry.raim_refloat_pending.clear();
        }

        // --- PASS A: per-station geometry + shared-state layout -------------
        std::vector<std::vector<PppObservation>> modeled(stations.size());
        std::vector<bool> present(stations.size(), false);
        // Legacy: the pivot's allocated phase-bias keys are the shared set.
        // Prioritized: seeded empty here; the anchor election after the cross-station
        // observability prune rebuilds it as the anchored union over present stations
        // (each key backed by exactly one elected pin).
        std::set<std::string> shared_phase_keys;
        // The pivot-first head, the PASS-A parallel tail and the slip scans live in
        // ApplicationNetworkAssembly.cpp.  networkPassAHead_ models the legacy pivot
        // (with its skip bookkeeping; SkipEpoch ends the epoch) and seeds the legacy
        // shared set; the SB_SLIP wall times the scan.
        if (networkPassAHead_(se, ctx, setup, traces, modeled, present,
                              shared_phase_keys) == EpochAction::SkipEpoch)
            return LoopSignal::Next;

        subBegin(SB_SLIP);   // block 1: PASS-A slip scans, pre-assembly (D0b)
        ++slip_qc.epoch_idx;   // monotonic processed-epoch index (card G gap arithmetic)
        networkPassASlipScan_(se, ctx, setup, traces, dt, pl_now_s, carry_warm,
                              modeled, present, shared_phase_keys,
                              carry_slipped_raw, slip_qc);
        // The union preserves the funnel's previous contents: the PASS-A drains in
        // NetworkSlipQc.cpp only insert, so routing their writes through
        // carry_slipped_raw changes nothing downstream.
        carry_slipped_ambs.insert(carry_slipped_raw.begin(), carry_slipped_raw.end());
        subEnd(SB_SLIP);       // close block 1, slip scans (D0b)
        subBegin(SB_PREASM);   // pre-assembly measurement build, post-slip → assembly wall (D0b)

        // The raw slip observables and the cross-station observability pre-filter live in
        // ApplicationNetworkAssembly.cpp; the writer-before-prune ordering contract is
        // structural inside networkAssemblyPrefilter_.
        int n_dropped_sats = 0;
        networkAssemblyPrefilter_(se, ctx, setup, traces, slip_qc,
                                  modeled, present, n_dropped_sats);

        // Flicker / gap signal integrity.  Classifies every present (station, satellite)
        // line of sight against its own band history, publishes the per-station phase
        // sigma-scale map that PASS B's fold reads, and refloats a line of sight whose
        // band flickered back with no producer-registered gap.
        //
        // The call has one valid position.  It must run after the PASS-A slip drains and
        // their union, because it inserts into carry_slipped_ambs / _raw and every
        // consumer must see those insertions; after networkAssemblyPrefilter_, so the
        // census counts only the LOS set that folds rows and a satellite erased by the
        // two-station observability gate is not reported as an event; and before the
        // phase-datum census and election, because a withhold prunes phase-bias states
        // the election might otherwise seat a datum pin on.
        //
        // It is not placed in the PASS-B serial pre-pass, which runs after the election.
        // The epoch unit is slip_qc.epoch_idx, the counter the gap instrument uses, so
        // their re-entry totals are directly comparable.  No-op unless
        // network.signal_integrity.census.
        networkSignalIntegrity(ctx, config_, log, traces, se.time, slip_qc.epoch_idx,
                               stations, present, modeled,
                               carry_slipped_ambs, carry_slipped_raw,
                               carry_invariant, carry_warm);
        // Product-referenced measurement admission, the first layer of the defence in
        // depth (this screen, then tie admission, the prefit fuse, and the SH health
        // gate).  It is the absolute test for a channel whose code and phase are
        // displaced together, the class every differential check passes by construction
        // (stations have been seen +198.2 km and +99.9 km off SP3+CLK).  The statistic is
        // `observed - modeled` on code alone, centred on the station median; `modeled` is
        // the state-free deterministic model, which lets the screen fire at a cell's
        // birth, where a prefit innovation structurally cannot.
        //
        // The call has one valid position.  After the PASS-A slip drains and their union,
        // because it inserts into carry_slipped_ambs / _raw for its onset, heal
        // (readmission) and station-guard refloats.  After networkAssemblyPrefilter_, so
        // a satellite erased by the observability gate cannot enter the median, raise a
        // verdict, or count as admitted.  After networkSignalIntegrity, because `modeled`
        // is rebuilt every epoch, and screening first would make the flicker classifier
        // read each withhold as a band loss and each heal as a FlickerRegain, creating
        // the events that census exists to measure (this seam only reads `observed` /
        // `modeled`, which the classifier does not write, so the later position costs
        // nothing).  And before the phase-datum election and
        // networkAssemblyRegisterColumns_, because a withhold prunes orphaned phase-bias /
        // code-bias / clock states the election could otherwise pin, and would otherwise
        // leave the ≥ 2-station code-bias coverage gate counting a station that feeds it
        // nothing.  No-op unless network.adm_screen or network.adm_trace.
        networkMeasurementAdmission(ctx, config_, log, traces, se.time,
                                    slip_qc.epoch_idx, stations, present, modeled,
                                    carry_slipped_ambs, carry_slipped_raw,
                                    carry_invariant, carry_warm);
        // The final shared phase-bias set, and which station anchors (gauge-pins) each
        // (sat,signal).  Two realizations of the same minimum constraint (exactly one
        // ambiguity pin per phase-bias freedom):
        //   Legacy (network.phase_datum_prioritized off): the single pivot's pruned
        //     phase-bias states are the shared set (each backed by a pivot-receiver
        //     pin).  Byte-identical.
        //   Prioritized: per (sat,signal), the highest-coverage receiver that tracks it
        //     with a live ambiguity is elected as its anchor.  The anchor is decoupled
        //     from the clock pivot, so a satellite low or unseen at the clock pivot keeps
        //     a clean gauge at another station instead of jumping.
        if (!phase_datum_prioritized) {
            shared_phase_keys.clear();
            for (const auto& kv : stations.front().sm->getSatPhaseBiasStates())
                shared_phase_keys.insert(kv.first);
        } else {
            const bool   sticky  = config_.networkPhaseDatumSticky();

            // This epoch's candidate census and the network.phase_datum_min_stations
            // coverage gate (filter/NetworkPhaseDatum, INC-8b).  It is built once here and
            // shared by whichever realization the dispatch below picks.  The builder also
            // prunes the phase-bias states of gated keys, which all three elections
            // require to have happened already, and it is the sole writer of the two
            // run-end gate counters (computing the census twice would double-count them).
            PhaseDatumCandidates cnd;
            AnchorHygiene hyg;   // eq34 reattach hygiene verdicts (inactive when off)
            networkBuildPhaseDatumCandidates(config_, stations, present, modeled,
                                             cnd, n_gated_epochs, n_gated_key_epochs);

            // eq34 reattach hygiene.  Ages this epoch's anchorable arcs, decides which
            // keys may be re-hosted and on what, runs the INC-2 bridge lifecycle, and
            // (when a key has no converged host and no bridge) removes it from the
            // candidate union as the coverage gate removes a thin key.  It must sit here:
            // after the census (it mutates cnd.union_keys and needs the coverage prune
            // done) and after the PASS-A slip drains (carry_slipped_ambs must be complete,
            // or a just-refloated arc reads as mature; invariant #1), but before any
            // election reads the union.  No-op unless a hygiene knob is armed.
            networkAnchorHygiene(ctx, config_, log, traces, se.time, stations, present,
                                 carry_slipped_ambs, cnd, hyg);

            if (phase_datum_ensemble) {
                networkElectEnsembleDatum(stations, cnd, this_epoch_anchor,
                                          shared_phase_keys);
            } else if (phase_datum_eq34) {
                // Chain datum (card 3).  It mutates the carries before the solve and
                // fills ctx.carry.eq34_reref, so it must stay ahead of the D3c RPB
                // allocation below (it is the sole writer of shared_phase_keys on this
                // path, and D3c derives its need_rpb set from that) and therefore ahead
                // of est.beginEpoch and PASS B.  The full ordering contract is in
                // filter/NetworkChainDatum.hpp.
                networkElectChainDatum(ctx, config_, log_manager_, log, stations,
                                       present, cnd, hyg, carry_slipped_ambs,
                                       shared_phase_keys);
            } else {
                // Two-edge continuous handover.  The ledger is passed only when armed
                // (it decides the pin target, so it affects the solve), but the event
                // sink is unconditional (since R0): the cold re-pin of the default
                // posture is also an event on both edges, and leaving it unlogged is
                // §5.07.4 defect 5.  The rows are trace-class (network_events.csv under
                // trace_events); the solve is untouched, and `tally.reattaches` counts
                // either way.
                std::vector<AnchorHandoverEvent> handovers;
                const bool hv_on = ctx.knobs.pd_handover_continuous;
                // The re-referenced (sat,signal) set (queue item 1(d)): filled here when
                // armed, consumed by the D2/C1 carry fold (marginalize) and the erase
                // directly below, and cleared by the carry stash, not by this election.
                // A withheld epoch skips the stash, so its prev_carry still holds the
                // retired-frame values while the incumbent swap has already persisted;
                // clearing here would let that stale carry fold unmarked one epoch
                // later.  Clearing at the stash keeps the mark until a solve commits the
                // new frame.
                auto& pa_reref = ctx.carry.phase_anchor_reref;
                const std::size_t pa_before = pa_reref.size();
                // Anchor-slip handover (network.cycle_slip.anchor_slip_handover,
                // docs/SPEC_event_harness.md §12.7).  It must sit exactly here: after
                // every PASS-A slip drain and the signal-integrity / admission refloats
                // (so both freed sets are complete), and immediately before the
                // election, its only consumer.  It stages the same one-epoch candidacy
                // ban the harness injector uses, so a family whose anchor arc was just
                // freed is handed to a successor, at the successor's own carried
                // value_abs, in this epoch: before the freed arc's level is re-drawn
                // from one epoch of code and the whole family's gauge follows it.  Knob
                // off: not called, nothing staged, and the `inject` argument below
                // resolves unchanged.
                if (config_.networkCycleSlipAnchorSlipHandover()) {
                    const int n_st = networkStageAnchorSlipHandovers(
                            stations, anchor_incumbent,
                            ctx.carry.relevel.free_ambs, carry_slipped_ambs,
                            ctx.inject);
                    if (n_st > 0) {
                        ctx.counters.n_anc_slip_staged += n_st;
                        ++ctx.counters.n_anc_slip_epochs;
                    }
                }
                // Cold anchor fallback (SPEC_event_harness §15): the candidacy pass's
                // read-only view.  It is built here and not inside the election because
                // every member is a run-scope carry object: the freed sets this epoch's
                // drains just filled, and the leveling carry the age bar reads
                // `birth_ep` from.  Default-constructed when the knob is off, which
                // reproduces the election without the card.
                AnchorCandidacyUsable cand_usable;
                if (ctx.knobs.pd_candidacy_usable) {
                    cand_usable.on           = true;
                    cand_usable.satellite_clause = ctx.knobs.pd_candidacy_satellite;
                    cand_usable.min_age_ep   = ctx.knobs.pd_candidacy_min_age_ep;
                    cand_usable.now_ep       =
                        static_cast<long long>(ctx.counters.n_loop_epochs);
                    cand_usable.free_ambs    = &ctx.carry.relevel.free_ambs;
                    cand_usable.slipped_ambs = &carry_slipped_ambs;
                    cand_usable.local_carry  = &ctx.carry.local_carry;
                }
                // The phase datum as a held S-basis (RF-2,
                // docs/refound/RF-1_steps345_mechanism_2026-09-15.md).  Assembled here
                // because this scope owns the ledger, the carry, the flag map and this
                // epoch's elevations; `on` is false on every unarmed run, and the
                // pointer below is then null.
                PhaseHeldBasis held_basis;
                std::vector<std::map<std::string, double>> pd_elev_of;
                held_basis.on = ctx.knobs.pd_held_basis;
                if (held_basis.on) {
                    held_basis.min_age_ep    = ctx.knobs.pa_min_age_ep;
                    held_basis.flag_quiet_ep = ctx.knobs.pa_flag_quiet_ep;
                    held_basis.now_ep = static_cast<long long>(ctx.counters.n_loop_epochs);
                    held_basis.basis   = &ctx.carry.phase_basis;
                    held_basis.flag_ep = &ctx.carry.amb_flag_ep;
                    held_basis.carry   = &ctx.carry.local_carry;
                    // This epoch's LOS elevations, the election's rank key (item 3).
                    pd_elev_of.assign(modeled.size(), {});
                    for (std::size_t mi = 0; mi < modeled.size(); ++mi)
                        for (const auto& mo : modeled[mi])
                            pd_elev_of[mi][mo.satellite] = mo.elevation;
                    held_basis.elev = &pd_elev_of;
                }
                // The coverage gate's drop edge (card EV7 §6).  It is passed on every
                // run, armed or not: requirement F2 asks the unarmed run to report the
                // size of the class, and detection is pure observation over the census
                // and the pre-swap incumbent map (the erase below is the only thing the
                // knob gates).
                PhaseGateDrop gate_drop;
                gate_drop.on = config_.networkPhaseDatumGateDropErase();
                // The ledger read at the election's first-sighting branch (card SID
                // phase 4, S2 / B1).  `observe` alone is the observatory and is
                // byte-identical; `restore` adds the phase 4b action.  Both are false on
                // every unarmed run, which leaves every branch on the pointer
                // unreachable inside the election.
                AnchorReentry pd_reentry;
                pd_reentry.observe = config_.networkLedgerScreens() && ctx.roster.armed;
                pd_reentry.restore = pd_reentry.observe && config_.networkLedgerAnchor();
                pd_reentry.led     = &ctx.arc_ledger;
                pd_reentry.state   = &ctx.carry.sid_anchor_reentry;
                pd_reentry.now_ep  = ctx.arc_ledger.epochIndex();
                networkElectPhaseAnchors(sticky, stations, present, cnd, hyg,
                                         ctx.counters.hyg,
                                         anchor_incumbent,
                                         ctx.carry.anchor_pin_target,
                                         this_epoch_anchor,
                                         shared_phase_keys,
                                         hv_on ? &ctx.carry.handover : nullptr,
                                         &handovers,
                                         ctx.knobs.pd_handover_max_var,
                                         ctx.knobs.pa_carry_reref ? &pa_reref
                                                                  : nullptr,
                                         ctx.knobs.pd_handover_max_var_ratio,
                                         // This epoch's forced retirements, or
                                         // nullptr (the normal path).  Two sources:
                                         // the event-harness schedule and the
                                         // production anchor-slip handover staged
                                         // just above; either one alone makes the
                                         // directive set live.
                                         (ctx.inject.armed
                                          && !ctx.inject.anchor_now.empty())
                                         || !ctx.inject.anchor_now_slip.empty()
                                             ? &ctx.inject : nullptr,
                                         &cand_usable,
                                         // The held S-basis (RF-2).  Null unless armed,
                                         // so every branch it gates is unreachable and
                                         // the election is byte-identical.
                                         held_basis.on ? &held_basis : nullptr,
                                         // always non-null (card EV7 §6, see above)
                                         &gate_drop,
                                         // In effect null when neither half is armed:
                                         // the election tests `observe` on its first
                                         // line (card SID phase 4).
                                         &pd_reentry);
                // Both edges of the gate are events (card EV7 §6, F2).  One row per drop
                // edge, with the identities on either side (what left, which station was
                // anchoring it, which stations still carry it) and `handled` saying
                // whether the response ran, so an unarmed row and an armed row are
                // separable offline.  Trace-class; the solve never reads these.
                for (const auto& gd : gate_drop.events) {
                    ++ctx.counters.n_pd_gate_drop_edges;
                    if (gd.handled) ++ctx.counters.n_pd_gate_drop_handled;
                    // Asks the arc ledger what state the dropped family's arcs are in,
                    // counted where the edge is already evented (card SID phase 1,
                    // class (h)).  Read-only and a no-op when the knob is off.
                    networkSignalRosterGateDrop(ctx, gd.sat, gd.signal);
                    // The coverage-gate writer (card SID phase 4a, S1).  The gate
                    // removes a phase-bias column and, when armed, the family's carried
                    // ambiguities; it does not remove the phase rows, which is the
                    // whole of card EV7's mechanism, so this bit is expected to open
                    // few or no admitted gaps.  It is recorded anyway, so the readout
                    // can state that the gate fired and opened nothing.
                    arcReasonExpand(ctx.reasons, ctx.arc_ledger, -1, gd.sat,
                                    gd.signal, kArcReasonGate);
                    emitEvent("PHASE_ANCHOR", "gate_drop",
                              gd.sat + "_" + gd.signal,
                              gd.survivors.empty() ? std::string("-") : gd.survivors,
                              static_cast<double>(gd.n_trackers),
                              static_cast<double>(config_.networkPhaseDatumMinStations()),
                              gd.handled ? "erase" : "none",
                              fmt::format("survivors={};trackers_now={};prev_anchor={};"
                                          "handled={}",
                                          gd.survivors.empty() ? std::string("-")
                                                               : gd.survivors,
                                          gd.n_trackers,
                                          gd.prev_anchor.empty() ? std::string("-")
                                                                 : gd.prev_anchor,
                                          gd.handled ? 1 : 0));
                }
                // Folds the ledger read (card SID phase 4, S2 / B1).  It happens here,
                // where every other election readback is folded, so the counters and
                // the two event tiers are written in one place and in epoch order.
                networkSignalRosterAnchorRead(ctx, traces, se.time, pd_reentry);
                // The families the gate-drop response owns, for the erase-cause split
                // below.  A gated key is erased from union_keys before the election's
                // loop, so it cannot also be a handover or first-sighting mark this
                // epoch; the two populations are disjoint by construction.
                std::set<std::pair<std::string, std::string>> gate_drop_fams;
                for (const auto& gd : gate_drop.events)
                    if (gd.handled) gate_drop_fams.insert({gd.sat, gd.signal});
                // Folds the election's write-back into the run counters (RF-2).
                if (held_basis.on) {
                    auto& HC = ctx.counters;
                    HC.n_pb_basis_held      += held_basis.n_held;
                    HC.n_pb_basis_elected   += held_basis.n_elected;
                    HC.n_pb_basis_cold      += held_basis.n_elected_cold;
                    HC.n_pb_basis_refuse_young   += held_basis.n_refused_young;
                    HC.n_pb_basis_refuse_flagged += held_basis.n_refused_flagged;
                    HC.n_pb_basis_rank_changed   += held_basis.n_rank_changed;
                    HC.n_pb_basis_refuse_unusable += held_basis.n_refused_unusable;
                    HC.n_pb_basis_elev_fallback   += held_basis.n_elev_fallback;
                    HC.n_pb_basis_depose_flag     += held_basis.n_depose_flag;
                    HC.n_pb_basis_depose_death    += held_basis.n_depose_arc_death;
                    HC.n_pb_basis_depose_unusable += held_basis.n_depose_unusable;
                    HC.n_pb_basis_value_drift    += held_basis.n_value_drift;
                }
                // Split-referenced satellite census (§14.5).  Always on and
                // knob-independent (invariant #4): the number of satellites whose
                // per-frequency phase-bias families are anchored at different
                // stations, whose arc levels walk independently.  It is read here
                // because the election has just swapped this epoch's anchors into
                // anchor_incumbent, so the map is current.  Pure and O(map), with no
                // contact with the solve: the counters below are its only output, so
                // an unarmed run measures the same thing and the effect of the scope
                // on datum fragmentation is comparable across arms.
                {
                    const SplitRefCount sr =
                        networkSplitReferencedSats(anchor_incumbent);
                    ctx.counters.n_cb_split_sats_sum += sr.split;
                    ctx.counters.n_cb_split_multi_sum += sr.multi_anchored;
                    ++ctx.counters.n_cb_split_epochs;
                    if (sr.split > ctx.counters.n_cb_split_sats_max)
                        ctx.counters.n_cb_split_sats_max = sr.split;
                }
                // Event-harness readback: the election filled in the directive with the
                // stations it retired and elected.  One row per directive, and the
                // firing ledger is closed here: an anchor_handover that named a
                // satellite no family used never fires, and the run-end readout
                // reports that.
                for (const auto& d : ctx.inject.anchor_now) {
                    if (d.n_families == 0) continue;
                    std::string ret, ele;
                    for (const auto& r : d.retired) ret += (ret.empty() ? "" : "|") + r;
                    for (const auto& e2 : d.elected) ele += (ele.empty() ? "" : "|") + e2;
                    traces.emitEvent(se.time, "INJECT", "anchor_handover", d.sat, "",
                                     static_cast<double>(d.n_families), std::nullopt,
                                     "injected",
                                     fmt::format("mode={};families={};forced_cold={};"
                                                 "retired={};elected={}",
                                                 d.cold ? "cold" : "continuous",
                                                 d.n_families, d.n_forced_cold,
                                                 ret.empty() ? "-" : ret,
                                                 ele.empty() ? "-" : ele));
                    log->warn("Network (fault inject): TOW {:.1f} anchor_handover on {} "
                              "({}) forced {} family(ies): retired [{}] -> elected [{}].",
                              se.time.tow, d.sat, d.cold ? "cold" : "continuous",
                              d.n_families, ret.empty() ? "-" : ret,
                              ele.empty() ? "-" : ele);
                    for (std::size_t k = 0; k < ctx.inject.events.size(); ++k)
                        if (ctx.inject.events[k].kind == "anchor_handover"
                            && ctx.inject.events[k].sat == d.sat
                            && !ctx.inject.onset_logged[k]) {
                            ++ctx.inject.n_applied[k];
                            ctx.inject.onset_logged[k] = true;
                            ++ctx.inject.fired_by_kind["anchor_handover"];
                        }
                }
                // Anchor-slip handover readback (§12.7), both edges with identities
                // (invariant #7): the retired incumbent is the row's station; the
                // successor and the branch are its detail.  A cold outcome is a gauge
                // re-realization, and the row says so; a directive that found no
                // successor left the family unpinned for the epoch, and the row says
                // that too.
                for (const auto& d : ctx.inject.anchor_now_slip) {
                    if (d.n_families == 0) {
                        // The family was not in this epoch's union (coverage-gated,
                        // bridged or pruned).  There are no identities to report; it is
                        // counted so the run-end readout can separate it from a
                        // handover that happened.
                        ++ctx.counters.n_anc_slip_inert;
                        continue;
                    }
                    const char* branch = d.n_no_successor > 0 ? "none"
                                       : (d.n_cold_fallback > 0 ? "cold" : "continuous");
                    if (d.n_no_successor > 0) {
                        ++ctx.counters.n_anc_slip_no_successor;   // not re-hosted
                    } else {
                        ++ctx.counters.n_anc_slip_handed;
                        if (d.n_cold_fallback > 0) ++ctx.counters.n_anc_slip_cold;
                        else                       ++ctx.counters.n_anc_slip_continuous;
                    }
                    emitEvent("PHASE_ANCHOR", "anchor_slip_handover",
                              d.sat + "_" + d.signal,
                              d.retired.empty() ? std::string("-") : d.retired.front(),
                              std::nullopt, std::nullopt, "handover",
                              fmt::format("to={};branch={}{}",
                                          d.elected.empty() ? std::string("-")
                                                            : d.elected.front(),
                                          branch,
                                          // Cold anchor fallback (§15): what the
                                          // candidacy pass skipped to get here, so
                                          // the cold-handover census can be re-run
                                          // from the trace.  Absent with the knob off.
                                          ctx.knobs.pd_candidacy_usable
                                              ? fmt::format(";cand_skipped={};cand_first_unusable={}",
                                                            d.n_cand_skipped,
                                                            d.cand_first_unusable.empty()
                                                                ? std::string("-")
                                                                : d.cand_first_unusable)
                                              : std::string{}));
                }
                // Both edges in one row (invariant #7): which station released the
                // gauge, which took it, and whether the frame was preserved.  A
                // handover logged only on its incoming half is the unlogged
                // re-referencing that the FT63 topology ledger was built to recover.
                for (const auto& h : handovers)
                    traces.emitEvent(se.time, "PBGAUGE",
                                     h.continuous ? "handover_continuous"
                                                  : "handover_cold",
                                     h.amb, h.to, h.target_m, std::nullopt,
                                     h.continuous ? "s_transform" : "re_realize",
                                     "from=" + (h.from.empty() ? std::string("(none)")
                                                               : h.from)
                                     // With the ratio ceiling armed, the successor's ledger
                                     // var and first-stash var are added to the row (the
                                     // prerequisite named in SPEC_amb_iono §7 item 15: trace
                                     // the successor's conditional var per handover, not
                                     // only the target).
                                     + (ctx.knobs.pd_handover_max_var_ratio > 0.0
                                            ? fmt::format(";var={:.3e};var_first={:.3e}",
                                                          h.var, h.var_first)
                                            : std::string{})
                                     // Cold anchor fallback (§15): the per-handover
                                     // candidacy record, that is, how many better-ranked
                                     // candidates this successor was chosen over, and
                                     // the first one refused.  Absent with the knob off,
                                     // so rows without the card are byte-identical.
                                     + (ctx.knobs.pd_candidacy_usable
                                            ? fmt::format(";cand_skipped={};cand_first_unusable={}",
                                                          h.cand_skipped,
                                                          h.cand_first_unusable.empty()
                                                              ? std::string("-")
                                                              : h.cand_first_unusable)
                                            : std::string{}));
                // Local-carry hygiene (1(d)): the marked families' phase-leveling carries
                // are erased at every station before PASS B consumes the carry (the eq34
                // D4 placement), in the `if (!pa_reref.empty())` block below.  Counted for
                // the run-end summary; the counter takes only this election's new marks (a
                // set persisting across a withheld epoch must not be counted twice).  An
                // empty set (knob off, or a quiet election) is a no-op, and re-erasing a
                // persisting set is a no-op too.
                // ---- Every handover declared (RF-2b item 2) ----------------------------
                // This loop sees every handover the election produced.  It must not sit
                // inside `if (!pa_reref.empty())`, whose predicate (whether the
                // anchor-reref marking fired this epoch) is unrelated and lets only a
                // fraction of the handovers through.
                //
                // A handover is declared to the consumer when its basis value changed,
                // that is, when the emitted phase bias steps.  A continuous handover
                // inherits the outgoing frame by construction (Δ = 0), and marking it
                // would force a consumer reset for a wire that did not move (RF-2 marked
                // them, and its PB marks rose to 5.3× the baseline against a 1.5× bar).  A
                // cold handover pins the successor at 0 against an outgoing basis of
                // `prev_value`, so it steps by exactly that, and the consumer must be told.
                // Every handover is traced; only a real step is marked, and the row
                // carries `marked=` so the two populations are separable offline.
                //
                // The mark goes through `ura_retro_sats`, the retro-bump queue that
                // NetworkSolPrep drains into markSatDiscontinuity with mark-equals-land
                // semantics: a satellite withheld this epoch stays pending and is marked
                // at its next actual emission, which is the epoch the consumer needs the
                // reset.
                if (ctx.knobs.pd_held_basis)
                    for (const auto& h : handovers) {
                        const auto pk = StateKeys::parseAmbiguityKey(h.amb);
                        if (!pk) continue;
                        const auto be = ctx.carry.phase_basis.find(h.amb);
                        const double dlt =
                            (be != ctx.carry.phase_basis.end())
                                ? (be->second.value - be->second.prev_value) : 0.0;
                        // Cold-mark rule (RF-2d, network.phase_datum_held_mark_cold):
                        // |value − prev_value| is the between-station level difference, not
                        // the wire step.  A continuous pin is the successor's level in the
                        // outgoing frame, so the family does not step while the difference
                        // is non-zero; a cold pin at 0 steps the family by the successor's
                        // level, and out of a cold tenure the difference reads 0 − 0.  The
                        // Δ predicate is therefore inverted (measured on both beds in
                        // RF-3-read), and the armed rule is the frame's own: a handover is
                        // declared if and only if it is cold.
                        const bool steps = ctx.knobs.pd_held_mark_cold
                                               ? !h.continuous
                                               : std::abs(dlt) > kStsExactTolM;
                        if (steps) {
                            ctx.carry.ura_retro_sats.insert(pk->satellite);
                            ++ctx.counters.n_pb_basis_declared;
                            if (!h.continuous) ++ctx.counters.n_pb_basis_declared_cold;
                        }
                        traces.emitEvent(se.time, "STRANSFORM",
                                         h.continuous ? "sts_applied" : "sts_cold_declared",
                                         h.amb, h.to, dlt, std::nullopt,
                                         h.continuous ? "s_transform" : "re_realize",
                                         fmt::format("from={};basis={:.6f};prev={:.6f};marked={}",
                                                     h.from.empty() ? std::string("(none)")
                                                                    : h.from,
                                                     be != ctx.carry.phase_basis.end()
                                                         ? be->second.value : 0.0,
                                                     be != ctx.carry.phase_basis.end()
                                                         ? be->second.prev_value : 0.0,
                                                     steps ? 1 : 0));
                    }
                if (!pa_reref.empty()) {
                    ctx.carry.pa_reref_total +=
                            static_cast<long long>(pa_reref.size() - pa_before);
                    // Declared anchor S-transform (RF-1 step 5, §3.6).  A frame step is a
                    // known, exact basis change, so the carry is expressed in the new
                    // basis instead of being discarded.  Families the transform takes
                    // ownership of are removed from the erase set below (transformed or
                    // erased, never both), and a cold handover (no Δ) keeps its erase,
                    // which is the limit of this step and the subject of RF-2.  The
                    // handover declaration is in the loop above this block (RF-2b).
                    std::set<std::pair<std::string, std::string>> pa_sts_handled;
                    networkAmbFloatDeclaredHandover(ctx, log, traces, stations, handovers,
                                                    se.time.week, se.time.tow,
                                                    pa_sts_handled);
                    for (const auto& fam : pa_sts_handled) pa_reref.erase(fam);
                    // With every handover declared and the basis held (RF-2 item 2), the
                    // network-wide erase is retired: the carries are not stale, they are
                    // expressed against a datum whose move the consumer has been told
                    // about.  The erase counter is therefore expected to read 0 on an armed
                    // arm; the erase stays in the code so the off path and every other
                    // datum realization keep it unchanged.
                    //
                    // Cold release (RF-2d, network.phase_datum_held_cold_release): that
                    // premise holds for continuous handovers only, and R2 already keeps
                    // those out of this set.  What reaches here are the cold and
                    // first-sighting families, whose frame did step.  Clearing them leaves
                    // their retired-frame carries folding against the re-realized design,
                    // and the satellite clock absorbs the step (65 m on sim G29,
                    // RF-3-read).  Armed, the set is kept and released as on the level
                    // vehicle.
                    if (ctx.knobs.pd_held_basis && !ctx.knobs.pd_held_cold_release)
                        pa_reref.clear();
                }
                if (!pa_reref.empty()) {
                    // With the trace armed, the entries are captured before they are
                    // destroyed: this scope owns the station names and the epoch stamp,
                    // which the map operation does not need to know (RF-1 §7.4 D3).
                    std::vector<std::pair<std::tuple<int, std::string, std::string>,
                                          LocalCarry>> pa_reref_erased;
                    // The capture is also taken when a drop edge was handled, so the
                    // gate-drop arc counter does not read 0 on a run that leaves
                    // network.trace_amb_state off; the row writer itself stays
                    // knob-gated and does nothing there (card EV7 §6).
                    const int n_erased = networkApplyPhaseAnchorCarryReref(
                            pa_reref, ctx.carry.local_carry,
                            (ctx.knobs.trace_amb_state || !gate_drop_fams.empty())
                                ? &pa_reref_erased : nullptr);
                    for (const auto& [k, lc] : pa_reref_erased) {
                        const int sidx = std::get<0>(k);
                        // The two populations that share this erase path are tagged
                        // apart in network_amb_state.csv: a handover or first-sighting
                        // re-reference (`anchor_reref`) and a coverage-gate drop edge
                        // (`gate_drop`).  They are disjoint by construction, so the test
                        // is a lookup, not a precedence rule (card EV7 §6, F2).
                        const bool is_gd =
                            gate_drop_fams.count({std::get<1>(k), std::get<2>(k)}) > 0;
                        if (is_gd) ++ctx.counters.n_pd_gate_drop_arcs;
                        ambStateNoteErase(
                            ctx, se.time.week, se.time.tow,
                            (sidx >= 0 && sidx < static_cast<int>(stations.size()))
                                ? stations[sidx].name : std::string{},
                            std::get<1>(k), std::get<2>(k), lc,
                            is_gd ? "gate_drop" : "anchor_reref");
                    }
                    ctx.carry.pa_reref_erase_total += n_erased;
                    ctx.counters.n_cr_phase_datum_reref += n_erased;   // retirement census: frame step
                    log->debug("Network[1(d)]: {} marked (sat,signal) family(ies) "
                               "marginalized ({} new this election); {} phase-"
                               "leveling local carry(ies) erased — refloat next "
                               "epoch.",
                               pa_reref.size(), pa_reref.size() - pa_before,
                               n_erased);
                }
            }  // end anchor election (!ensemble && !eq34)
        }

        // RPB allocation (card 3 eq34, rule D3c): a present station gets the
        // per-(receiver,system,signal) split state δ_{r,σ} if and only if it carries an
        // ungated shared phase-bias column on σ (a phase-bias state whose key is in
        // shared_phase_keys), that is, if it is a vertex of the D2 graph.  A station whose
        // σ-keys are all gated allocates no RPB and keeps the legacy lumping.  The set is
        // re-derived each epoch (allocate what is needed, prune the rest), before
        // est.reset() and PASS B, so buildJacobianRow's +1 RPB coefficient sees the column
        // and splitStation routes it into the local block A_r.  A spanning tree touches
        // every vertex, so every RPB-carrying station carries ≥1 tree-edge pin ⇒ its N_rr
        // stays PD.  Inert (no allocation) when eq34 is off.
        if (phase_datum_eq34) {
            for (size_t i = 0; i < stations.size(); ++i) {
                if (!present[i]) continue;
                StateManager& sm = *stations[i].sm;
                std::set<std::string> need_rpb;
                for (const auto& kv : sm.getSatPhaseBiasStates()) {
                    if (!shared_phase_keys.count(kv.first)) continue;   // gated → legacy lump
                    const auto p = StateKeys::parseSatPhaseBiasKey(kv.first);
                    if (!p || p->satellite.empty()) continue;
                    need_rpb.insert(StateKeys::makeReceiverPhaseBiasKey(
                        PppUtils::getSystemString(p->satellite), p->signal));
                }
                for (const auto& rk : need_rpb)
                    if (sm.getReceiverPhaseBiasIndex(rk) < 0)
                        sm.addReceiverPhaseBiasState(rk, 0.0);
                sm.pruneReceiverPhaseBiasStatesExcept(need_rpb);
            }
        }

        // Shared-column registration, counts and estimability descriptors are in
        // ApplicationNetworkAssembly.cpp (networkAssemblyRegisterColumns_ below).  The
        // outputs are fresh per epoch; the empty-system skip keeps its bookkeeping inside
        // (SkipEpoch return), and the Next return below bypasses subEnd(SB_PREASM).
        // est.beginEpoch() runs inside on Proceed.
        // --- Level-authority census (LV-6) --
        // Serial, after PASS A's CS-GF advance; it runs before the RegisterColumns
        // SkipEpoch below (an empty-system epoch still gets its census row) and upstream
        // of every withhold decision (the census measures through withhold eras).
        // Measure-only; no-op unless armed.
        networkLevelAuthorityCensus(ctx, config_, log, traces, se,
                                    setup.stations, modeled, present);

        std::vector<std::string> registered_clock_sats;
        std::vector<std::pair<std::string, std::string>> registered_code_keys;
        std::vector<std::string> col_desc;
        int n_clock = 0, n_pb = 0, n_cb = 0, n_present = 0;
        if (networkAssemblyRegisterColumns_(se, ctx, setup, traces, est, present,
                                            shared_phase_keys, registered_clock_sats,
                                            registered_code_keys, col_desc,
                                            n_clock, n_pb, n_cb, n_present)
            == EpochAction::SkipEpoch)
            return LoopSignal::Next;

        // --- SD EWL Route-B integer decision (increment 2a, filter/NetworkSdEwlAr) ----
        // Its position is fixed by four constraints: after networkAssemblyPrefilter_
        // (satellites seen by fewer than 2 stations are erased there, so an integer
        // decided for one that then vanishes is wasted), after the phase-datum election
        // and the eq34 RPB allocation (both mutate the StateManager layout), after
        // networkAssemblyRegisterColumns_ (so the epoch is committed; that site can still
        // SkipEpoch), and before networkPassB_, whose parallel produce loop reads the
        // published map read-only.  It runs inside the existing SB_PREASM bracket rather
        // than a new SubBlock, because a new timer slot would change the section-timing
        // artifact for every run, armed or not.  Off by default ⇒ clears the map and
        // returns ⇒ byte-identical.
        networkSdEwlAr(ctx, config_, log, traces, setup, se, pl_now_s, modeled, present);

        // --- rx-CB de-mean pre-pass (card LV-5) ----------------
        // The rx-CB de-mean μ is reduced here, after PASS A has settled which stations
        // are present and before PASS B's parallel produce folds anything.  Both terms of
        // every carry innovation (the stashed absolute posterior and the frozen x_lin) are
        // already known at this point, so the LV-2 mean applies to the epoch it describes
        // instead of arriving one epoch late.  That lag is a feedback delay whose loop gain
        // scales with how much carry information folds; with all priors admitted (LV-4)
        // it produces a ~3-epoch limit cycle in the level.  Serial with one writer, and
        // PASS B only reads the map, which is the race contract of the rest of the carry
        // cycle.  No-op unless armed.
        networkRxCbDemeanPrepass(ctx, stations, present, pl_now_s);

        // Capture of the shared-border folds (the code-bias gauge constraint and all
        // carried and anchor priors) applied to this epoch's arrowhead, so the
        // within-epoch adaptation (drop a culprit, re-solve) can replay the identical
        // shared system over a fresh beginEpoch().  addSharedPrior is additive, so the
        // three prior sets concatenate into one replayable vector.  Filled only when the
        // adaptation is armed; otherwise the appends are cheap no-ops.
        const bool within_epoch_armed = config_.networkRaimWithinEpoch();
        auto& cap_H = ws.cap_H;
        auto& cap_z = ws.cap_z;
        auto& cap_constraint_weight = ws.cap_constraint_weight;
        auto& cap_has_constraint = ws.cap_has_constraint;
        auto& cap_priors = ws.cap_priors;
        auto& cap_dense_keys = ws.cap_dense_keys;
        auto& cap_dense_cov = ws.cap_dense_cov;

        // --- Oracle ambiguity pin, simulation only (filter/NetworkOraclePin) --------
        // Its position is fixed by three constraints: after networkElectPhaseAnchors
        // (the fit reads this epoch's anchors and their enforced pin targets; a handover
        // moves a family's frame term and the pin must follow it), after
        // networkAssemblyRegisterColumns_ (so the epoch is committed; that site can still
        // SkipEpoch), and before networkPassB_, whose parallel produce loop reads the
        // published table read-only.  It runs inside the existing SB_PREASM bracket rather
        // than a new SubBlock, because a new timer slot would change the section-timing
        // artifact for every run, armed or not (as for networkSdEwlAr above).  With no
        // `oracle_pin` event the pointer is null, no branch is reachable, and the output
        // is byte-identical.
        if (ctx.carry.oracle_pin)
            networkOraclePinEpoch(ctx, config_, stations, se.time, log, traces);

        // The bounded drop (RF-1 step 4, docs/refound/RF-1_float_ambiguities_design.md
        // §3.4), applied serially here: after PASS A's slip scans have latched
        // relevel.free_ambs and before the parallel PASS-B inject reads the carry.  By the
        // time a station's produce sees an arc, the drop has happened, and the inject folds
        // the arc at birth strength instead of skipping it.  A no-op unless the vehicle is
        // float_kf.  `modeled` carries this epoch's LOS elevations, with which the birth
        // floor is mapped; the map is built only when the floor is armed.
        {
            std::vector<std::map<std::string, double>> amb_elev_of;
            if (ctx.knobs.amb_float_kf && ctx.knobs.amb_birth_var_floor > 0.0) {
                amb_elev_of.resize(modeled.size());
                for (std::size_t mi = 0; mi < modeled.size(); ++mi)
                    for (const auto& mo : modeled[mi])
                        amb_elev_of[mi][mo.satellite] = mo.elevation;
            }
            networkAmbFloatApplyDrops(ctx, config_, log, stations, amb_elev_of,
                                      se.time.week, se.time.tow, pl_now_s);
        }
        // --- Exact coupling time update, per-epoch inputs (card CTU) ---------------
        // After the columns are registered (the layout the rows resolve against) and
        // before PASS B, whose parallel produce reads the result.  It mirrors this
        // epoch's D2 fold, which runs later in networkSolveGlue and checks the mirror.
        // Inside the SB_PREASM bracket for the reason networkSdEwlAr gives.  No-op off.
        networkClockLevelRestoreRaise(ctx, log);   // card CTU G5: once, the first leg epoch after a warm resume
        networkCouplingExactPrepare(ctx, est, se.time, dt, log);
        subEnd(SB_PREASM);   // close pre-assembly; the assembly wall below is timed separately (D0b)

        // --- PASS B (in ApplicationNetworkAssembly.cpp): networkPassB_ owns the D0
        // assembly wall (a raw chrono pair; the sinks are passed by reference), the T5
        // sh-dev aging, the card-1d reinit history, the parallel produce loop (including
        // the coupling-injection cluster, whose row builder is filter/NetworkCoupling
        // since INC-7b), and the serial consume.  The failed / empty-epoch skip keeps its
        // bookkeeping inside (SkipEpoch return).
        std::vector<StationAssembly> asm_res(stations.size());
        int  total_pins = 0;
        // est-station index per station (the order addStation was called) so the
        // post-solve back-substitution can recover each present station's local
        // slant-iono block; -1 for stations not added this epoch.
        std::vector<int> est_index_of(stations.size(), -1);
        if (networkPassB_(se, ctx, setup, traces, est, iono_deweight,
                          sh_dev_tau, sh_dev_on, sh_dev_demean, sh_dev_d5_keep,
                          carry_slipped_ambs, carry_slipped_raw,
                          carry_warm, pl_now_s,
                          modeled, present, shared_phase_keys,
                          registered_clock_sats, registered_code_keys,
                          n_clock, n_pb, n_present,
                          asm_res, est_index_of, total_pins,
                          sec_asm_ms_epoch, sec_asm_wall_ms) == EpochAction::SkipEpoch)
            return LoopSignal::Next;
        subBegin(SB_GAUGEBUILD); // buildEnsembleGauges + carry-driver lambdas, pre-DD-build (D0b)

        // Ensemble datum (fix A, network.phase_datum_ensemble): builds the anchor-free
        // sum-to-zero phase-bias gauges from every tracking station's ambiguity column and
        // hands them to the estimator, which folds Σ_r zˢ_{r,σ}=0 through the kernel
        // (Schur-projected) at the gate/solve reduce.  One gauge per (sat,signal) that has
        // a registered shared phase-bias column (thin or gated keys have no column, so
        // their ambiguity floats).  Re-invoked on the within-epoch RAIM re-solve (the
        // station set and indices may change).  Empty and a no-op when off.
        auto buildEnsembleGauges = [&]() {
            networkBuildEnsembleGauges(est, stations, est_index_of, asm_res,
                                       shared_phase_keys, prev_carry,
                                       phase_datum_ensemble, phase_hold_on);
        };
        buildEnsembleGauges();

        // Within-epoch RAIM culprit set forwarded into the DD-carry re-solve rebuild
        // (spec §3 culprit-withhold).  The pointers alias the RAIM block's drop sets while
        // it runs, and are null otherwise.  The PASS-A slip feed (carry_slipped_ambs,
        // above) is the primary source; both are carry_invariant only, so a run with the
        // toggle off is byte-identical (buildCarryDdPriors returns early).
        const std::set<std::string>*                carry_we_drop_sats = nullptr;
        const std::map<int, std::set<std::string>>* carry_we_drop_arcs = nullptr;

        // Reverse map (group key → chord ArcKey → ambiguity state key) so a
        // post-solve innovation flag (group, chord) maps back to its (station, ambiguity) for
        // the next epoch's slip funnel.  Rebuilt with the blocks each buildCarryDdPriors call
        // (the last call before the solve is authoritative).  Empty unless carry_innov_gate.
        std::map<int, std::map<ArcKey, std::string>> carry_arc_amb_key;
        // The pair groups' band-2 ambiguity keys, parallel to carry_arc_amb_key
        // (band 1).  A D5 flag on a pair chord feeds both bands into the slip funnel
        // (§3.4 union refloat).  Empty unless lane_carry and carry_innov_gate are both on.
        std::map<int, std::map<ArcKey, std::string>> carry_arc_amb_key2;

        // --- Arc-resumption bridge classification.  Instrumentation only: nothing
        //     below changes a decision.  Of the arcs that came back after a tracking
        //     break whose raw level shifted, it counts how many re-entered carrying
        //     state that bridged the break (the harmful case) and how many re-entered
        //     as a fresh float (benign).
        //
        //     The position matters: after the slip drains, so carry_slipped_ambs (the
        //     only thing that forces a chord to re-tap) is complete for this epoch; and
        //     before buildCarryDdPriors re-keys the carry, so carriedChordValue,
        //     carry_sat_vertex and carry_group_id still describe the state the arc is
        //     returning to.  carry_group_id is read with find(), never operator[]:
        //     minting a group id here would corrupt the driver's stable keying.
        //
        //     The unit is the (station, satellite, signal) arc, not the resumption.
        //     Chords are per arc, so a dual-band satellite returning once contributes
        //     two chords that can bridge independently.  The classification is armed
        //     by the instrument or by the response
        //     (network.cycle_slip.gap_reentry_relevel): with only the response armed,
        //     the ledger is the before/after measurement of what the response did, and
        //     gating it on the diagnostic would let a response-only arm report its own
        //     effect as a structural zero.
        if ((nc.slip_qc.gap_instrument || nc.slip_qc.gap_reentry)
            && !slip_qc.epoch.resumptions.empty()) {
            // Whether this carry_group_id key's signal component covers `sig`.  Under
            // the WL-3 lane shape a paired arc lives in a group keyed "<sigA>+<sigB>",
            // not in its per-band group, so a per-band-only lookup would report every
            // paired arc as un-bridged (a false negative on the question this
            // classification answers).
            auto groupCoversSignal = [](const std::string& gsig, const std::string& sig) {
                if (gsig == sig) return true;
                std::size_t p = 0;
                while (p <= gsig.size()) {
                    const std::size_t q = gsig.find('+', p);
                    const std::size_t e = (q == std::string::npos) ? gsig.size() : q;
                    if (gsig.compare(p, e - p, sig) == 0) return true;
                    if (q == std::string::npos) break;
                    p = q + 1;
                }
                return false;
            };
            for (const auto& g : slip_qc.epoch.resumptions) {
                const int i = g.station;
                if (i < 0 || i >= static_cast<int>(stations.size())) continue;
                const char cons = g.satellite.empty() ? '\0' : g.satellite.front();
                const auto svit = carry_sat_vertex.find(g.satellite);
                for (const std::string& sig : {g.sig1, g.sig2}) {
                    if (sig.empty()) continue;
                    const std::string amb_key = StateKeys::makeAmbiguityKey(g.satellite, sig);
                    // "Marked slipped" covers both vehicles.  carry_slipped_ambs is
                    // the card-4 C3c funnel, the only thing that forces a DD chord to
                    // re-tap; on the leveling vehicle, what stops the stale value
                    // from folding is the free-epoch latch (`relevel.free_ambs`, read
                    // by the PASS-B inject site).  A classification that read only
                    // the funnel would report a freed leveling arc as
                    // `shifted_bridged`, blind to its own response, and the §G3
                    // response could not be graded on the lineage it was built for.
                    const bool slip_marked =
                        carry_slipped_ambs.count({i, amb_key}) != 0
                        || ctx.carry.relevel.free_ambs.count({i, amb_key}) != 0;
                    // (a) Legacy phase-leveling carry: a value inside the
                    //     max-gap window is exactly what the PASS-B injector
                    //     would fold across the break.
                    bool pl_bridged = false;
                    {
                        const auto cit = local_carry.find(std::make_tuple(i, g.satellite, sig));
                        if (cit != local_carry.end()) {
                            const double gap = pl_now_s - cit->second.last_seen_s;
                            pl_bridged = (gap >= 0.0 && gap <= pl_max_gap_s);
                        }
                    }
                    // (b) Card-4 invariant DD carry: whether the driver still holds a
                    //     carried chord for this arc from before the break.
                    //     carry_sat_vertex already stores the kSatBase-offset vertex
                    //     id, so it is used as is; adding the base again would look
                    //     up a chord that cannot exist and report "never bridged" for
                    //     everything.
                    bool dd_bridged = false;
                    if (carry_invariant && svit != carry_sat_vertex.end() && cons != '\0') {
                        for (const auto& [gkey, gid] : carry_group_id) {
                            if (gkey.first != cons || gid == 0) continue;
                            if (!groupCoversSignal(gkey.second, sig)) continue;
                            double zg = 0.0, vg = 0.0;
                            if (carry_driver.carriedChordValue(gid, ArcKey(i, svit->second),
                                                               zg, vg)) {
                                dd_bridged = true;
                                break;
                            }
                        }
                    }
                    const bool bridged = (pl_bridged || dd_bridged) && !slip_marked;
                    // The arc ledger mirrors the resumption's three-way split
                    // (shifted / measured and unshifted / unmeasurable), and every
                    // cell is counted, so each row closes as bridged + fresh instead
                    // of leaving the missing cell to be inferred (H2).  Unmeasurable
                    // is kept separate from clean: an arc that bridged a break whose
                    // level could not be checked is unknown, not benign, and G3's
                    // "shifted-and-bridged ≈ 0 ⇒ defer" ruling holds only as far as
                    // that count is small.
                    if (g.shifted) {
                        if (bridged) ++slip_qc.counters.gap_shifted_bridged;
                        else         ++slip_qc.counters.gap_shifted_fresh;
                    } else if (g.measurable) {
                        if (bridged) ++slip_qc.counters.gap_clean_bridged;
                        else         ++slip_qc.counters.gap_clean_fresh;
                    } else {
                        if (bridged) ++slip_qc.counters.gap_unmeas_bridged;
                        else         ++slip_qc.counters.gap_unmeas_fresh;
                    }
                    // The per-arc event carries the same three-way verdict as the
                    // counters above: an unmeasurable resumption is tagged
                    // `unmeas_*`, never `clean_*`, and its ΔGF is written blank, as
                    // its ΔMW is.  A "dGF=0.0000" on an arc with no pre-gap level
                    // would read as "the level did not move", a claim the instrument
                    // has no evidence for.
                    emitEvent("T0_gap", "arc_resume", g.satellite, stations[i].name,
                              g.measurable ? std::optional<double>(g.d_mw) : std::nullopt,
                              std::nullopt,
                              g.shifted     ? (bridged ? "shifted_bridged" : "shifted_fresh")
                              : g.measurable ? (bridged ? "clean_bridged"   : "clean_fresh")
                                             : (bridged ? "unmeas_bridged"  : "unmeas_fresh"),
                              fmt::format("{};gap_ep={};dGF={};pl={};dd={};slip={}",
                                          sig, g.gap_epochs,
                                          g.measurable ? fmt::format("{:.4f}", g.d_gf)
                                                       : std::string{},
                                          pl_bridged ? 1 : 0, dd_bridged ? 1 : 0,
                                          slip_marked ? 1 : 0));
                }
            }
        }

        // Builds this epoch's DD-prior blocks from the eq34 election tree and folds the
        // carried invariant DDs into the producer solve.  Each arc (receiver, satellite)
        // ambiguity on a (const,signal) is resolved to its estimator (station, local
        // ambiguity column); pinned_amb_targets marks the spanning-tree edges (the gauge),
        // and the rest are chords (the carried DDs).  GLONASS (FDMA) is excluded.  Vertex
        // and group ids are stable across epochs (anchored on the satellite label;
        // receiver = app index).  Re-invoked on the RAIM re-solve (est.beginEpoch there
        // clears the priors and rebuilds est_index_of), like buildEnsembleGauges.  A
        // no-op, byte-identical, unless carry_invariant.
        auto buildCarryDdPriors = [&]() {
            networkBuildCarryDdPriors(ctx, config_, log, traces, carry_driver, est, stations, asm_res,
                                      est_index_of, present, shared_phase_keys, se, carry_arc_amb_key,
                                      carry_arc_amb_key2, carry_slipped_ambs, carry_slipped_raw,
                                      carry_we_drop_sats,
                                      carry_we_drop_arcs, n_threads, ewl_lanes, ewl_cfg, mw_wl_layer,
                                      mw_wl_ppp_utils, mw_wl_f6, mw_wl_epoch, wl_inject_epoch, lane_trace_epoch,
                                      wl_epoch, wl_cons_epoch, isUncarriedThirdBand, carry_innov_gate, mw_wl_on,
                                      ewl_on, uwl_on, uwl_cold_on, uwl_mid_on, uwl_dual_on, uwl_family_on,
                                      uwl_sib_gate_on, wl_inject_on, bridge_debias_on, ewl_carry_exclude,
                                      lane_on, lane_meas_on, lane_bridge_on, lane_fix_on,
                                      lane_parallel_components, mw_d5_dump, mw_lane_trace, mw_tagsets,
                                      mw_wl_f6_dump);
        };
        subEnd(SB_GAUGEBUILD);   // close gauge build and carry-driver setup (D0b)
        subBegin(SB_DDBUILD);    // carry_driver.beginEpoch + buildCarryDdPriors, W·Pᶻ·Wᵀ (D0b)
        if (carry_invariant) {
            // The z-store predict (q_z·Δt on the carried DD covariance) runs once per
            // epoch; the build and fold may re-run on the RAIM re-solve.  D5 v2 passes the
            // epoch time (σ̂₀² reporting bucket) and a clean flag, always clean here: under
            // carry_invariant the eq34 datum re-realizes freely, so there is no re-level
            // burst to gate, and the robust geometric mean absorbs the event tail at
            // content hour 4 on its own.
            const double dt = std::isfinite(carry_prev_tow) ? (se.time.tow - carry_prev_tow) : 0.0;
            carry_driver.beginEpoch(dt > 0.0 ? dt : 0.0, se.time.tow, /*clean=*/true);
        }
        { CarryEigenScope es(carry_par, n_threads); buildCarryDdPriors(); }  // parallel_carry: thread W·Pᶻ·Wᵀ
        subEnd(SB_DDBUILD);      // close buildCarryDdPriors, the card-4 DD-cov projection (D0b)
        subBegin(SB_SOLVEGLUE);  // pre-solve prep + clock predict + solve + within-epoch re-solve (D0b)
        NetworkSolveGlueResult sg_ = networkSolveGlue(ctx, config_, log, traces, setup, est, ws, se, asm_res, est_index_of,
                                                      present, shared_phase_keys, registered_clock_sats, registered_code_keys,
                                                      col_desc, n_clock, n_pb, n_cb, n_present, total_pins, carry_we_drop_sats,
                                                      carry_we_drop_arcs, carry_warm, within_epoch_armed, dt, pl_now_s,
                                                      clk_anchor_sigma, buildEnsembleGauges, buildCarryDdPriors);
        if (sg_.action == EpochAction::SkipEpoch) return LoopSignal::Next;
        auto& x_s = sg_.x_s;
        auto& plan = sg_.plan;
        auto& changed_consts = sg_.changed_consts;
        auto& datum_changed = sg_.datum_changed;
        auto& clk_model_warm = sg_.clk_model_warm;
        auto& emit_rate_warm = sg_.emit_rate_warm;
        auto& clock_refloat_sats = sg_.clock_refloat_sats;
        auto& retro_flag_sats = sg_.retro_flag_sats;
        auto& retro_feed = sg_.retro_feed;
        auto& n_arc_refloat_epoch = sg_.n_arc_refloat_epoch;
        auto& n_adapt = sg_.n_adapt;
        auto& sigma0_adapt = sg_.sigma0_adapt;
        auto& perconst_active = sg_.perconst_active;
        auto& perconst_publish = sg_.perconst_publish;
        auto& perconst_dec_withhold = sg_.perconst_dec_withhold;
        auto& perconst_stats = sg_.perconst_stats;
        auto& we_t2_opp = sg_.we_t2_opp;
        auto& we_t2_flag = sg_.we_t2_flag;
        subEnd(SB_SOLVEGLUE);   // close solve glue; net excludes the internal solve/reduce (D0b)
        subBegin(SB_INSTALL);   // carry_driver.installPosterior, DD posterior mean/cov readback (D0b)
        // Installs the DD posterior (mean and covariance) back into the carry from the
        // final accepted x_s and datum: the invariant DDs the next epoch projects forward.
        // It runs after acceptance (after the skip paths of the observability and σ̂0
        // withhold gates above), so a withheld or contaminated epoch never enters the
        // carry, and outside the within-epoch RAIM block, so it runs on every accepted
        // epoch, not only RAIM-armed ones.  A failure resets the carry (a safe cold
        // start).  No-op when off.
        if (carry_invariant) {
            networkInstallCarry(ctx, config_, log, traces, carry_driver, est, x_s, plan, se, stations,
                                slip_qc, carry_arc_amb_key, carry_arc_amb_key2, log_dir_, carry_par,
                                n_threads, seam_threads, carry_innov_gate, mw_wl_on, lane_on, ewl_on, uwl_on,
                                wl_inject_on, lane_fix_on, mw_d5_dump, mw_lane_trace, mw_tagsets,
                                mw_wl_f6_dump, ewl_lanes, wl_inject_epoch, lane_trace_epoch, mw_wl_run,
                                mw_wl_epoch, wl_run, wl_epoch, wl_cons_run, wl_cons_epoch, mw_wl_f6);
        }
        subEnd(SB_INSTALL);     // close installPosterior, the card-4 posterior readback (D0b)
        subBegin(SB_NETAR);     // producer-side network AR phase 2, log-only, off in the tier (D0b)
        networkProducerAr(ctx, config_, log, traces, se, est, x_s, stations,
                          est_index_of, asm_res, plan, ar_ppp_utils, log_dir_);
        subEnd(SB_NETAR);      // close producer AR / pre-emit tail (D0b)
        subBegin(SB_SOLPREP);  // toSolutionEpoch + per-satellite discontinuity signalling (D0b)
        // Decision half of the gross-LOS screen (RF-3b B3,
        // docs/refound/RF-3_code_row_qc_2026-09-22.md §4).  It runs ahead of the emission,
        // so a declaration reaches the epoch it declares rather than the one after it.  The
        // response half stays at the post-harvest site below, where it must be.  It runs
        // inside this existing bracket rather than behind a new timer slot, because a new
        // slot would change the section-timing artifact for every run, armed or not (as
        // for networkSdEwlAr).  Unreachable unless network.gross_los_emit_exclude.
        if (ctx.knobs.gl_emit_exclude)
            networkGrossLosScreen(ctx, config_, log, traces, se, stations, est, x_s,
                                  asm_res, est_index_of, pl_now_s,
                                  GrossLosPhase::Decide);
        NetworkSolPrepResult sp_ = networkSolPrep(ctx, config_, log, traces, setup, se, est, x_s,
                                                  plan, pl_now_s, datum_changed, changed_consts,
                                                  sigma0_adapt, perconst_active, perconst_publish,
                                                  perconst_dec_withhold, perconst_stats);
        auto& sol = sp_.sol;
        auto& epoch_withheld_consts = sp_.epoch_withheld_consts;
        auto& global_bump = sp_.global_bump;
        auto& serialized_consts = sp_.serialized_consts;
        auto& retro_emit_sats = sp_.retro_emit_sats;
        subEnd(SB_SOLPREP);     // close sol-prep and discontinuity signalling (D0b)
        // The fast anchor may advance only on an epoch whose solution reached the wire.
        // Every whole-epoch withhold (observability gate, A4 sigma0 ceiling, SH health
        // gate) `continue`s or returns before networkEmitStore's emit site, and n_emitted
        // is incremented exactly there, so comparing that counter across the call tests
        // emission continuity.  Keying on solve continuity instead is the R11 defect
        // documented by the C1 card: a delta measured inside a withheld era could be
        // published across a consumer-visible gap.
        const std::size_t fc_n_emitted_before = ctx.counters.n_emitted;
        // The smoothed hold's clock clones (card SMH): advanced over this epoch before the
        // hold reads them at emit.  No-op off.
        networkPbHoldCloneUpdate(ctx, log, est, x_s, ws);
        subBegin(SB_EMITSTORE); // block 7: re-base + PB-emit filter + store push + solution CSV (D0b)
        networkEmitStore(ctx, config_, log, traces, setup, se, est, x_s, pl_now_s,
                         datum_changed, sigma0_adapt, sol, epoch_withheld_consts, global_bump,
                         serialized_consts, retro_emit_sats, emit_rebase_on, emit_pb_filter_on,
                         pb_filter_emit_sigma, emit_rebase_engine, pb_filter_engine,
                         clk_rebase_engine, present,
                         est_index_of, modeled, shared_phase_keys, carry_slipped_ambs,
                         we_t2_opp, we_t2_flag, cl_epochs, cl_ar_ran, cl_ar_fixed, cl_sum_fixed,
                         cl_first_fix_tow, cl_first_fix_week, cl_recent_err3d);
        subEnd(SB_EMITSTORE);   // close block 7: emit/store, order-sensitive, serial (D0b)
        // Harvests the fast anchor from this epoch's emitted solution.  `sol` is read after
        // networkEmitStore, so the cached c0 values are the re-based, PB-filtered ones the
        // consumer received; that makes the fast product a delta in the emitted frame, in
        // which every per-era frame constant cancels.
        if (fc_on && ctx.counters.n_emitted != fc_n_emitted_before) {
            subBegin(SB_FASTCLOCK);
            networkFastClockHarvest(ctx, config_, log, setup, se, present, modeled, sol,
                                    fc_ppp_utils);
            subEnd(SB_FASTCLOCK);
        }
        subBegin(SB_IONODIAG);  // block 4a: iono / ZWD / anchor-info viz traces (D0b)
        networkIonoDiag(ctx, config_, log, traces, setup, ws, se, est, x_s, plan, asm_res,
                        est_index_of, modeled, registered_clock_sats, clk_anchor_sigma);
        subEnd(SB_IONODIAG);   // close block 4a, iono/ZWD/anchor traces (D0b)
        subBegin(SB_DDFRAC);   // block 6: cross-station DD fractional-residual trace, B5 (D0b)
        networkDdFrac(ctx, config_, log, traces, setup, se, est, x_s, plan, asm_res,
                      est_index_of, dd_ppp_utils);
        subEnd(SB_DDFRAC);       // close block 6, dd-frac B5 (D0b)
        subBegin(SB_SHDEV_VTEC); // block 4b: T5 SH-deviation carry KF + VTEC coefficient trace (D0b)
        networkShDevVtec(ctx, config_, log, traces, setup, se, est, x_s, plan, asm_res,
                         est_index_of, modeled, iono_deweight, sh_dev_on);
        subEnd(SB_SHDEV_VTEC);  // close block 4b, T5 sh-dev + VTEC trace (D0b)
        subBegin(SB_T1A);       // block 3: T1a SH health probe + fused adaptive-deweight gate (D0b)
        networkShHealth(ctx, config_, log, traces, setup, se, est, x_s, asm_res, est_index_of,
                        modeled, iono_deweight, carry_vce_est, prires_est, datum_changed,
                        pl_now_s, first_vce_s, last_vce_hour);
        // Iono-deweight release hygiene.  It must sit here: networkShHealth above is the
        // only caller of the gate's advance(), and the release edge created by that slew
        // raises the weight that the next epoch's PASS-B assembly multiplies into the
        // SH<->slant tie rows, so the response has exactly this epoch to act before the
        // parked states are re-coupled.  It also comes after networkShDevVtec: that seam's
        // harvest is the epoch's legitimate deviation measurement update, and erasing the
        // stash ahead of it would discard good information.  No-op unless an increment is
        // armed.
        networkIonoDwRelease(ctx, config_, log, traces, setup, se, est, x_s,
                             est_index_of, modeled, iono_deweight);
        subEnd(SB_T1A);        // close block 3, T1a health + deweight (D0b)
        subBegin(SB_LEVELING); // block 5: phase-leveling carry harvest + T1d SH convergence (D0b)
        networkLeveling(ctx, config_, log, traces, setup, se, est, x_s, plan, asm_res,
                        est_index_of, carry_vce_est, prires_est, carry_warm, pl_now_s,
                        registered_clock_sats, registered_code_keys, shared_phase_keys);
        subEnd(SB_LEVELING);   // close block 5, leveling + T1d SH convergence (D0b)
        subBegin(SB_RAIM);     // block 2: post-fit network RAIM + median-polish attribution (D0b)
        // RAIM pool re-host: `carry_warm` requires network.phase_leveling, so gating on
        // it alone would leave post-fit RAIM unreachable on every card-4 lineage that
        // leaves leveling off (armed at load, never executed).  ctx.knobs.raim_ci_on is
        // the re-hosted arming condition (already ANDed with raim_on and the resolved
        // carry_invariant).
        if (raim_on && (carry_warm || ctx.knobs.raim_ci_on)) {
            networkPostFitRaim(ctx, config_, log, traces, se,
                               stations, asm_res, est_index_of, present, x_s, est,
                               plan.shared_datum, clock_refloat_sats, retro_flag_sats,
                               retro_feed, n_arc_refloat_epoch, prires_est,
                               &slip_qc.epoch);
        }
        // Gross line-of-sight screen (card GL, docs/SPEC_event_harness.md §24.18), the
        // only acting post-fit tier that screens code.  It sits inside this bracket,
        // immediately after the RAIM pass, for the ordering reason RAIM's own header
        // gives: erasing a `local_carry` entry is the refloat, so it must run after the
        // carry harvest (SB_LEVELING) and before the epoch stash tail.  It has no arming
        // interlock on RAIM: it owns its own residual walk and its own state because
        // every other tier is phase-only, and it must be able to run on a lineage that
        // arms none of them.  No-op (not even a state allocation) unless
        // network.gross_los_screen.
        // With network.gross_los_emit_exclude armed (RF-3b B3,
        // docs/refound/RF-3_code_row_qc_2026-09-22.md §4), the decision half already ran
        // above, before the emission, and this call is the deferred response half, which
        // belongs at this site for the ordering reason above.  Unarmed, this one call does
        // both halves.
        networkGrossLosScreen(ctx, config_, log, traces, se, stations, est, x_s,
                              asm_res, est_index_of, pl_now_s,
                              ctx.knobs.gl_emit_exclude ? GrossLosPhase::Act
                                                        : GrossLosPhase::Full);
        subEnd(SB_RAIM);       // close block 2, RAIM (D0b)
        subBegin(SB_CLOCKUPD); // block 8: two-state clock update + fault QC + retro-flag feed (D0b)
        networkClockQc(ctx, config_, log, traces, ws, se, est, x_s,
                       registered_clock_sats, datum_changed, changed_consts,
                       clk_model_warm, emit_rate_warm, pl_now_s, dt,
                       clock_refloat_sats, retro_flag_sats, retro_feed);
        subEnd(SB_CLOCKUPD);       // close block 8, clock update + retro feed (D0b)
        // The phase-visible consistency instrument (card PVN): measure-only, after the
        // epoch's last re-solve, while the fold capture in ws still holds the prior.
        networkPhaseVisibleMonitor(ctx, traces, se, est, x_s, ws);
        // Roster disagreement pass (app/SignalRoster.cpp, card SID phase 1).  It sits here
        // because this is the first point at which every input of P4 exists in one scope:
        // the arc-level tracker's rows were built in PASS A, local_carry is post-harvest
        // (networkLeveling), the ambiguity states are post-assembly, the anchor
        // incumbents are post-election, the freed-ambiguity set carries this epoch's slip
        // responses, and `sol` is the solution the wire received.  Read-only; every class
        // is a counter.  Disarmed, it returns on its first line.
        subBegin(SB_ROSTER);
        // Phase 2 adds `modeled` and `present`: the admitted row set that feeds the
        // second lifecycle.  They are in the same scope as every other input of the pass,
        // which is why the pass runs here and not at the build seam.
        networkSignalRosterAudit(ctx, log, traces, se, stations, modeled, present,
                                 slip_qc.epoch.computed ? &slip_qc.epoch : nullptr,
                                 carry_slipped_ambs, sol,
                                 ctx.counters.n_emitted != fc_n_emitted_before);
        // Fold half of the ledger emit (card SID phase 5a, E1).  It runs here and not at
        // the emit site because the ledger's admitted machine is advanced by the audit
        // pass immediately above: an admitted arc birth at this epoch does not exist
        // before that pass runs.  Read-only; every class is a counter plus an ARC event.
        // Disarmed, or with no capture from this epoch, it returns on its first line.
        networkLedgerEmitFold(ctx, traces, se);
        // Epoch tail of the second phase channel (card SID phase 6a, R3 + R4).  It sits
        // here for the same reason as the pass above: this is the first point at which
        // both of its inputs are final.  `relevel.free_cause` carries every tier's
        // freeing verdict for this epoch (including the post-solve RAIM ones, written at
        // SB_RAIM) and is not cleared until the next epoch's slip scan, and the
        // phase-datum anchors have been elected.  Read-only; every class is a counter.
        // Disarmed, it returns on its first line.
        networkSecondPhaseEpochTail(ctx, config_, stations);
        // All-channel census (card SID phase 6b), folded here beside the tail above
        // because it reads the same two republished cell sets plus the stations' own
        // state maps, all final at this point.  Measure-only; it returns on its first
        // line unless the widening is active.
        networkChannelAllEpoch(ctx, config_, stations, modeled, present);
        // Second-constellation census (card GAL): inter-system bias columns, satellites
        // per constellation, and the non-GPS-only station-epochs.  It sits here for the
        // same reason as the census above: `present` is final.  Measure-only; it returns
        // on its first line unless a constellation other than GPS is active.
        networkMultiGnssEpoch(ctx, config_, stations, modeled, present);
        subEnd(SB_ROSTER);
        // Trigger instrumentation (SPEC_day129_frame_guards P1-3/P1-4).  It runs before
        // the stash/status block, because the epoch-status row written there reads the
        // post-fit sigma0 this fills in.  All off by default; an unarmed run makes
        // neither call (byte-identical, no cost beyond the flag tests).  The §4.3 keyed
        // trace and the per-row trace use the same residual pass, so any of the three
        // trace toggles runs it.
        subBegin(SB_FRAMEDIAG);
        // Predict half of the block-Baarda census (BLOCKFORMAL): each family's σ̂₀
        // through the previous epoch is frozen and the per-epoch ladders are cleared
        // before the census's rider feeds this epoch's blocks.  Predict-then-update is
        // the D5 v2 contract: a block standardized by a calibration it contributed to can
        // standardize itself away.
        if (ctx.knobs.bb_on) ctx.carry.bb.beginEpoch(pl_now_s, se.time.tow);
        // Station-congruence predict half, with the same contract and reason: σ̂₀ through
        // the previous epoch is frozen before the census's rider feeds this epoch's
        // stations, so a station cannot standardize its own coordinate fault away.
        if (ctx.knobs.cg_on) ctx.carry.cg.beginEpoch(pl_now_s, se.time.tow);
        if (nc.trace_postfit_sigma0 || nc.trace_postfit_keyed || nc.trace_postfit_rows)
            networkPostfitSigma0(ctx, config_, log, traces, se, stations, est, x_s,
                                 asm_res, est_index_of, &modeled);
        if (nc.trace_sh_spectrum)
            networkShSpectrum(ctx, config_, log, traces, se, est);
        // Per-station local-block audit (card ZP, SPEC_event_harness §24.10).
        // Measure-only (const accessors on the solved estimator); it belongs in this
        // group because it is the station-side companion of the shared-side spectrum
        // above.  Its cadence gate (emitted epochs 1..20, then every 60th) lives in the
        // seam, so the driver carries no counter.
        if (nc.trace_station_block)
            networkStationBlock(ctx, config_, log, traces, se, stations, est, x_s,
                                asm_res, est_index_of);
        // Prefit innovation census, seed-birth tripwire and topology ledger
        // (docs/SPEC_prefit_census_observability.md INC-1/3/4), all on one walk of this
        // epoch's assembled row stacks.  It is first in the measure-only group: it costs
        // O(rows), it writes the epoch-status scalars the stash block picks up, and it is
        // the free tier the redundancy census escalates from.  The master knob keeps an
        // unarmed run out of the translation unit entirely.
        if (ctx.knobs.pc_walk_on)
            networkPrefitCensus(ctx, config_, log, traces, se, stations, est,
                                asm_res, est_index_of, &iono_deweight);
        // Redundancy / testability census (docs/SPEC_redundancy_census.md).  It runs
        // before networkRhsAttribution: a flight-recorder trip epoch forces a full sweep,
        // and the trip predicate is the attribution's own (unconsumed) window test, so the
        // census must read it before the attribution seam advances rhs_attrib_done.
        // Measure-only; the master knob keeps an unarmed run out of the translation unit
        // entirely.
        if (ctx.knobs.redun_census_on)
            networkRedundancyCensus(ctx, config_, log, traces, se, stations, est,
                                    plan.shared_datum, asm_res, est_index_of, x_s);
        // Block-Baarda drain (docs/SPEC_redundancy_census.md BB-INC-1..3): drains this
        // epoch's LOS-block ladders, writes network_block_baarda.csv and emits the block
        // identities.  The feed is inside the census above (the block sweep inherits its
        // stride and escalation instead of adding a scheduler), so this must run after
        // it.  The accumulator carries nothing forward, so an epoch that swept blocks and
        // never reached here does not report them.
        if (ctx.knobs.bb_on)
            networkBlockBaardaEmit(ctx, config_, log, traces, se, stations);
        // Congruence drain (docs/SPEC_redundancy_census.md CG-INC-1..3): drains this
        // epoch's per-station coordinate-offset records, writes network_congruence.csv and
        // emits the identities of the flagged stations.  The feed is inside the census
        // above (same host, same stride, no scheduler of its own), so this must run after
        // it.  It does not read ctx.carry.bb_hits (the separability join was resolved at
        // the sweep site), so its position relative to the block census's own emit is
        // free.
        if (ctx.knobs.cg_on)
            networkCongruenceEmit(ctx, config_, log, traces, se, stations);
        // MDB drain (docs/SPEC_redundancy_census.md MDB-INC-1): aggregates this epoch's
        // Baarda MDB rows, writes network_mdb.csv, emits the reporting events and
        // reconciles the uncheckable join.  Both orderings matter: after
        // networkPostfitSigma0, whose per-family σ̂₀ the scaled variant needs (read
        // earlier it would apply the previous epoch's inflation), and after the redundancy
        // census, whose per-epoch row counts the join compares against.  The feed sites are
        // in SB_RAIM and in the census; this is only the drain, and the accumulator is
        // stamped so an epoch that fed rows and never reached here leaks nothing forward.
        if (ctx.knobs.mdb_on)
            networkMdbFlush(ctx, config_, log, traces, se);
        // Structural S-basis dimension (docs/SPEC_datum_graph_census.md): predicted from
        // this epoch's admitted support topology and reconciled against the spectral count
        // the solve glue handed forward.  It is measure-only: it reads the registration
        // sets and `modeled`, and writes nothing but rows, events and counters.  Its order
        // within the group is free (it shares no accumulator with the censuses above), so
        // it sits last among them, ahead of the attribution seams that consume the trip
        // predicate.  The master knob keeps an unarmed run out of the translation unit
        // entirely.
        if (ctx.knobs.dg_on)
            networkDatumGraphCensus(ctx, config_, log, traces, se, stations, modeled,
                                    est_index_of, plan, shared_phase_keys,
                                    registered_code_keys, registered_clock_sats,
                                    datum_changed);
        if (est.attribArmed())
            networkRhsAttribution(ctx, config_, log, traces, se, stations, est,
                                  plan.shared_datum, asm_res, est_index_of);
        // Flight recorder (INC-5): consumes a trip raised anywhere earlier this epoch
        // (SH-gate engage, clock runaway, a00-band exit; the A4 withhold consumes its own,
        // since that epoch never reaches here), then retains this epoch as the next trip's
        // "before" frame.  Last in the group, so a trip dump sees the same kernel state
        // every other diagnostic just read.
        networkRhsAttribTrip(ctx, config_, log, traces, se, stations, est,
                             plan.shared_datum, asm_res, est_index_of);
        subEnd(SB_FRAMEDIAG);
        subBegin(SB_STASH_STATUS); // D2/C1/SH carry re-stash + VCE fold + epoch-status QC (D0b)
        networkStashStatus(ctx, config_, log, traces, setup, se, est, x_s, plan, carry_vce_est,
                           prires_est, clock_refloat_sats, n_arc_refloat_epoch, registered_clock_sats,
                           registered_code_keys, retro_emit_sats, shared_phase_keys,
                           sigma0_adapt, total_pins, vce_wants_cb, vce_wants_pb, dt,
                           datum_changed, changed_consts, n_clock, n_present, n_pb, n_cb,
                           n_dropped_sats, n_adapt);
        // Card SMH: the clones re-harvested on the stash just written, and the clones of
        // the generations that started at this epoch's emit born on it.  No-op off.
        networkPbHoldCloneHarvest(ctx, est, x_s, TimeUtils::continuousGpsSeconds(se.time));
        // Code-only level re-base (card CLV).  It runs after the stash, because it moves
        // the carry the stash just wrote (and every coupling term the leveling harvest
        // wrote), and only on a kept epoch, which is every epoch that reaches here.
        networkClockLevelRebase(ctx, traces, se.time, est, x_s, asm_res, est_index_of,
                                datum_changed);
        // The level monitor of card CTU reads the same post-fit code and the block the
        // stash just wrote; with the bound raise armed it raises that block before the
        // next fold.  Mutually exclusive with the re-base above (loader).  No-op off.
        networkClockLevelMonitor(ctx, traces, se.time, est, x_s, asm_res, est_index_of,
                                 datum_changed);
        // Drains this epoch's buffered carry-state rows (RF-1 §7.4).  It is last inside
        // the bracket because this is the first point after every row source has run: the
        // leveling harvest and the gap prune (SB_LEVELING), the T4 / T3 refloats (SB_RAIM)
        // and the phase-anchor carry re-reference (the datum block).  A no-op with an
        // empty buffer, which is every unarmed run.
        networkAmbStateTrace(ctx, log, traces);
        // Card LGM: drain the phase-colour trace rows buffered at the harvest.
        if (!ctx.carry.pc_trace_rows.empty()) {
            if (traces.phase_colour_trace.is_open())
                for (const std::string& row : ctx.carry.pc_trace_rows)
                    traces.phase_colour_trace << row;
            ctx.carry.pc_trace_rows.clear();
        }
        // Card AIL: the same drain for the ambiguity information trace.
        if (!ctx.carry.amb_if_rows.empty()) {
            if (traces.amb_if_trace.is_open())
                for (const std::string& row : ctx.carry.amb_if_rows)
                    traces.amb_if_trace << row;
            ctx.carry.amb_if_rows.clear();
        }
        subEnd(SB_STASH_STATUS);   // close the stash and epoch-status tail, before the readout (D0b)

        // Per-epoch section-timer readout, emitted-epoch path (D0).  Deltas of the
        // estimator's run-cumulative accumulators against the loop-top snapshot give this
        // epoch's kernel cost.  The driver assembly wall (sec_asm_ms_epoch) covers the
        // primary produce + scatter section; reduce, solve, cov-inverse and cov-consumers
        // happen after the wall, so `other` (QC decision logic, emit, I/O, any within-epoch
        // RAIM re-solve) = total − assembly − reduce − solve − cov-inverse − cov-consumers.
        // d_estadd is the kernel est.addStation delta, a cross-cutting figure: it also
        // captures re-solve adds after the wall, so it is not a strict part of the
        // assembly wall, and it is reported separately rather than nested under it.
        if (sec_timers_on) {
            const auto& stmr = est.sectionTimers();
            const double d_total  = std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - sec_epoch_t0).count();
            const double d_reduce = stmr.reduce_ms        - sec_t0.reduce_ms;
            const double d_solve  = stmr.solve_ms         - sec_t0.solve_ms;
            const double d_covinv = stmr.cov_inverse_ms   - sec_t0.cov_inverse_ms;
            const double d_covcon = stmr.cov_consumers_ms - sec_t0.cov_consumers_ms;
            const double d_estadd = stmr.add_station_ms   - sec_t0.add_station_ms;
            const std::size_t d_covcalls =
                stmr.cov_consumer_calls - sec_t0.cov_consumer_calls;
            const double d_other = d_total - sec_asm_ms_epoch - d_reduce - d_solve
                                   - d_covinv - d_covcon;
            ++sec_profiled_epochs;
            log->info("Network (D0) TOW {:.1f}: total {:.1f} ms | assembly-wall {:.1f} "
                      "| reduce {:.1f} | solve {:.1f} | cov-inv {:.1f} | cov-consumers {:.1f} "
                      "({} calls) | other {:.1f} | est.addStation {:.1f} (cross-cutting) | "
                      "{} shared cols, {} stations",
                      se.time.tow, d_total, sec_asm_ms_epoch, d_reduce, d_solve,
                      d_covinv, d_covcon, d_covcalls, d_other, d_estadd,
                      static_cast<long long>(est.numShared()), n_present);

            // Per-epoch sub-block attribution of `other` (D0b).  net = wall − Δ(timed
            // sections) inside each block; Σ net should approach d_other, and the residual
            // is logged as "unattributed" (loop regions this instrument does not bracket).
            std::string d0b;
            double d0b_net_sum = 0.0;
            for (int b = 0; b < SB_COUNT; ++b) {
                const double net = (sec_sub[b].wall_ms  - sec_sub_ep0[b].wall_ms)
                                 - (sec_sub[b].timed_ms - sec_sub_ep0[b].timed_ms);
                d0b_net_sum += net;
                d0b += fmt::format("{}={:.1f} ", kSubNames[b], net);
            }
            log->info("Network (D0b) TOW {:.1f}: other-net ms [ {}] Σnet {:.1f} | other {:.1f} "
                      "| unattributed {:.1f}",
                      se.time.tow, d0b, d0b_net_sum, d_other, d_other - d0b_net_sum);
        }

        // Periodic atomic snapshot (RTB-7(a)); the body is in NetworkRunContext.cpp
        // (INC-4).  It stays after the prev_week/prev_tow advance, so the snapshot
        // captures this epoch.
        networkWriteStateSnapshot(ctx, config_, se.time, pl_now_s, emitter, carry_driver,
                                  stations,              // CN rows (RS-8 §21.16; the latch
                                                         //   lives in the station models)
                                                         //   and RL x_lin rows (RS-9 §21.18)
                                  log,
                                  &clk_rebase_engine,    // frame rows, v2 (§18.10)
                                  &emit_rebase_engine,   // emit-layer rows (RS-5 §21.10):
                                  &pb_filter_engine,     //   ER / PF / PS
                                  setup.consumer.get()); // consumer snapshot beside each history copy (card CC (B))
        return LoopSignal::Next;
    };

    // === Epoch source =======================================================
    // Batch (default): iterates the pre-built synced vector.  Streaming
    // (network.streaming.enable, RTF-3): per-station live lanes → wait-budget aligner →
    // bounded epoch queue, popped here on the solve thread.  Only the iteration differs:
    // same per-epoch body, same order, same teardown.
    if (streaming_on) {
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
            if (processSyncedEpoch(se_live) == LoopSignal::Stop) break;
        }
        frontend->stop();
        frontend->logRunEndSummary(log);
    } else {
        for (const auto& se : synced)
            if (processSyncedEpoch(se) == LoopSignal::Stop) break;
    }

    if (parallel_on) Eigen::setNbThreads(eigen_threads_saved);  // restore Eigen's own threading

    return networkTeardown(ctx, config_, log, traces, setup, est, carry_driver, carry_vce_est, prires_est, slip_qc, mw_wl_run, ewl_lanes, wl_run, wl_cons_run, emit_rebase_engine, pb_filter_engine, clk_rebase_engine, iono_deweight, sec_sub, kSubNames, synced, carry_innov_gate, mw_wl_on, ewl_on, wl_inject_on, uwl_on, lane_on, sh_dev_on, idw_on, emit_rebase_on, emit_pb_filter_on, sec_asm_wall_ms, sec_profiled_epochs, t_loop_begin, cl_epochs, cl_ar_ran, cl_ar_fixed, cl_sum_fixed, cl_first_fix_tow, cl_first_fix_week, cl_recent_err3d, cl_truth, cl_truth_llh, log_dir_);
}

} // namespace GnssProcessor
