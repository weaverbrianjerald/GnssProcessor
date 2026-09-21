/**
 * @file ApplicationNetwork.cpp
 * @brief Implementation of Application::runNetwork (minimum-constraint
 *        S-system PPP-RTK NETWORK producer).
 *
 * A joint multi-station estimator: per-station local normals are reduced onto a
 * shared satellite-parameter block by an exact Schur complement, estimating the
 * estimable satellite clock and phase bias under an explicitly declared
 * minimum-constraint S-basis, emitted in IGS-SSR convention for the companion
 * PPP_RTK_User consumer.  Lives in its own translation unit alongside
 * ApplicationBaseline.cpp / ApplicationRealtime.cpp.
 *
 * runNetwork is a THIN ORCHESTRATOR.  The former ~14.7k-line monolith was
 * relocated verbatim into single-concern seams, leaving epoch-loop glue that
 * calls them.  New network code belongs in the matching seam, NOT here —
 * CLAUDE.md carries the placement decision-tree:
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
 *   PASS A (layout)  Per station, PppModel::computeModels then
 *       MeasurementModel::ensureNetworkSharedStates pre-allocates the shared
 *       satellite states (per-sat clock, per-(sat,signal) phase bias) behind the
 *       same usability gate the measurement build uses.  The DATUM (pivot)
 *       station goes first so its usable phase-signal set restricts phase-bias
 *       columns to ones backed by a pivot-receiver ambiguity pin.  The shared
 *       column layout is then registered: clock for every satellite any station
 *       sees, phase bias for the pivot's signal set.
 *
 *   PASS B (assembly)  MeasurementModel::computeMeasurementUpdate produces an
 *       undifferenced H/innovations/R block per station (DifferencingMode::None).
 *       Its columns split three ways: satellite clock (−1) and phase bias (+1)
 *       scatter into the shared border A_s; the 3 receiver-position columns are
 *       DROPPED (coordinates fixed); the rest form the station's private nuisance
 *       block A_r.  The pivot also folds tight-prior pins on its own ambiguities
 *       — the LOCAL half of the S-basis, so the emitted phase bias carries that
 *       pivot-receiver ambiguity lump (Odijk Eq. 4).
 *
 *   SOLVE  NetworkSbasis builds the shared minimum-constraint datum (one
 *       satellite-clock pin per constellation); the null-space rank gate verifies
 *       the reduced shared system carries exactly that deficiency; then
 *       NetworkEstimator::solve produces the shared estimate.
 *       NetworkProductEmitter serializes it into an OWNED SsrCorrectionStore —
 *       never the process-wide singleton the user side reads, which is what keeps
 *       the closed loop isolated.
 *
 *   Between epochs  the shared posterior (value, variance) carries forward as a
 *       diagonal prior on the next epoch's shared border, inflated by the
 *       per-parameter process noise over the gap.
 *
 * Scope: estimable satellite CLOCK + PHASE BIAS, coordinates and orbits fixed,
 * GPS + Galileo, per-band uncombined, float (no network AR).  Satellite code
 * bias, atmosphere products, the real-time branch, GLONASS FCN bias and network
 * QC are later slices.  Every station's coordinates MUST be known and its
 * observation RINEX path MUST be explicit in the config — the PathResolver
 * network gate is deferred, so resolveData() here serves only products and
 * broadcast nav.
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
#include "products/RinexNavParser.hpp"         // Card 5 (RTF-1): RINEX-nav → LiveEphemerisStore loader
#include "filter/NetworkEstimator.hpp"
#include "filter/IlsCore.hpp"                 // producer-side network AR (Phase 2+): ils::ilsFix
#include "filter/NetworkSbasis.hpp"
#include "filter/NetworkPivotHandover.hpp"   // A1/A2: pivot-handover datum-change detector
#include "filter/NetworkCarryVce.hpp"        // M2: LS-VCE carry-row variance-component monitor
#include "filter/NetworkPriorResidual.hpp"   // prior-residual census (SPEC_prior_residual_census)
#include "filter/NetworkIonoDeweightGate.hpp" // Station-adaptive SH-VTEC-constraint deweight (iono deweight card)
#include "filter/NetworkInvariantCarryDriver.hpp"  // Card 4 (M1) C3b: DD cycle-basis invariant carry
#include "filter/NetworkMwWlLayer.hpp"             // M5 card WL-1: MW-seeded wide-lane pseudo-obs layer
#include "filter/StateKeySchema.hpp"
#include "filter/NetworkRunTypes.hpp"      // INC-2: run-loop record types (NetStation / StationAssembly / carry PODs)
#include "filter/NetworkWlRunTypes.hpp" // INC-9b: hoisted runNetwork()-local WL/RAII types
#include "filter/NetworkRobustStats.hpp"   // INC-2: median/MAD robust attribution helpers
#include "filter/NetworkPhaseDatum.hpp"    // INC-8b: census + ensemble datum + anchor election + drop-and-heal
#include "filter/NetworkAmbStateTrace.hpp" // RF-1 §7.4: per-arc carry-state trace (drain + erase capture)
#include "filter/NetworkAmbFloat.hpp"      // RF-1 step 4: the floored birth prior + the bounded drop
#include "filter/NetworkHandoverStransform.hpp" // kStsExactTolM (RF-2b: the declaration step test)
#include "filter/NetworkChainDatum.hpp"    // INC-8b: Card-3 eq34 single-receiver-chain datum
#include "filter/NetworkAnchorHygiene.hpp" // eq34 reattach hygiene: anchor-host eligibility + pin bridge
#include "filter/NetworkSignalIntegrity.hpp" // flicker/gap: band-state classifier + unvetted phase demotion
#include "filter/NetworkMeasurementAdmission.hpp" // product-referenced admission: the corrupt-channel screen
#include "filter/NetworkGrossLos.hpp"    // card GL: the gross line-of-sight screen (§24.18)
#include "filter/NetworkRaim.hpp"        // INC-9: post-fit network RAIM
#include "filter/NetworkClockQc.hpp"
#include "filter/NetworkFrameDiag.hpp"   // P1-3/P1-4 trigger instrumentation (SPEC_day129_frame_guards)    // INC-10: post-solve satellite-clock QC
#include "filter/NetworkStationBlock.hpp"  // §24.10 card ZP: per-station LOCAL-block audit (measure-only)
#include "filter/NetworkRhsAttrib.hpp"   // reduced-RHS class attribution (era-2 injector probe)
#include "filter/NetworkMdb.hpp"               // Baarda reliability numbers / MDB (SPEC_redundancy_census)
#include "filter/NetworkRedundancyCensus.hpp"  // redundancy / testability census (SPEC_redundancy_census)
#include "filter/NetworkPrefitCensus.hpp"      // prefit census / fuse / birth check / topology (SPEC_prefit_census_observability)
#include "filter/NetworkDatumGraph.hpp"        // structural S-basis dimension census (SPEC_datum_graph_census)
#include "filter/NetworkSolPrep.hpp"    // INC-11pre: emit-side solution prep + disc signalling
#include "filter/NetworkEmitStore.hpp"    // INC-11: emit / store / product-write + closed-loop
#include "filter/NetworkFastClock.hpp"    // FC-1: split-cadence clocks-only fast pass
#include "filter/NetworkTeardown.hpp"
#include "filter/NetworkProducerAr.hpp" // INC-9b: log-only producer-AR + NEQ export
#include "filter/NetworkInject.hpp"
#include "filter/NetworkCanonicalPairAdmission.hpp" // card CPA: canonical-pair admission seam
#include "filter/NetworkInstallCarry.hpp" // INC-9b: installPosterior + WL zone-4 fold/inject
#include "filter/NetworkDdPriorBuild.hpp" // INC-9b: buildEnsembleGauges/buildCarryDdPriors free fns
#include "filter/NetworkSolveGlue.hpp" // INC-9b: datum/solve/within-epoch RAIM
#include "filter/NetworkIonoDiag.hpp"    // INC-11b
#include "filter/NetworkDdFrac.hpp"    // INC-11b
#include "filter/NetworkSdEwlAr.hpp"   // SD EWL Route-B integer decision (increment 2a)
#include "filter/NetworkShDevVtec.hpp"    // INC-11b
#include "filter/NetworkShHealth.hpp"    // INC-11b
#include "filter/NetworkIonoDwRelease.hpp" // iono-deweight release hygiene: reseed / park bound / row screen
#include "filter/NetworkLevelAuthority.hpp"  // LV-6 census
#include "filter/NetworkLeveling.hpp"    // INC-11b
#include "filter/NetworkStashStatus.hpp"    // INC-11b
#include "logging/CsvRotator.hpp"     // P1: size-based rotation of the unbounded service CSVs
#include "logging/NetworkRunTraces.hpp" // INC-3: trace-stream ownership + open logic + roster + FlagRate
#include "app/NetworkRunContext.hpp"    // INC-4: knobs + Tier-1 carry state + service counters (ctx)
#include "app/NetworkSetupResult.hpp"   // INC-5: setup-phase handoff (setup) + navAvailabilityGpsSeconds
#include "slip/MwLaneTiers.hpp"         // UWL-1 per-constellation lane-tier band table
#include "slip/MwSlipDetector.hpp"      // T0 phase-leveling slip repair (Increment 2)
#include "slip/TurboEditResolver.hpp"   // MW+GF per-signal integer repair (Increment 2)
#include "slip/IfPhaseSlipDetector.hpp" // IF-phase between-sat/between-epoch slip detection
#include "slip/NetworkSlipQc.hpp"       // INC-2: slip-QC pure helpers (ifElevMap / parseIfPairResetKey)
#include "slip/NetworkSlipOracle.hpp"   // SIM-ONLY oracle slip response (level-preserving ceiling)
#include "atmosphere/SphericalHarmonicIonosphere.hpp"
#include "common/Constants.hpp"
#include "common/PppUtils.hpp"        // B5: signal wavelengths for the DD fractional-cycle diagnostic
#include "realtime/RealTimeEngine.hpp"
#include "realtime/NetworkStreamFrontEnd.hpp"  // RTF-3: multi-station streaming epoch source
#include "math/TimeUtils.hpp"
#include "math/Coordinates.hpp"

#include <Eigen/Dense>
#include <Eigen/Core>          // Eigen::setNbThreads / nbThreads (pin Eigen to 1 thread inside our per-station OpenMP region — avoids nested oversubscription and keeps the result deterministic)
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

    // Q5 consumer-only replay: an already-written product file supplants the
    // producer entirely.  Branch BEFORE input validation / setup so none of the
    // producer machinery (roster, station build, emitter, snapshot fingerprint)
    // is touched — a replay run cannot perturb a producer artifact.
    if (!nc.closed_loop.replay_products_path.empty())
        return runConsumerReplay();

    log->info("PPP-RTK Network producer (V1): {} stations, clock datum station '{}'{}",
              nc.stations.size(), nc.clock_datum_station,
              nc.clock_datum_satellite.empty()
                  ? std::string{}
                  : (" + datum sat '" + nc.clock_datum_satellite + "'"));

    // --- Phase 0: input validation — relocated to ApplicationNetworkSetup.cpp
    // Identical error text + exit status.
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

    // --- Phases 2-4 + sinks + contract + consumer — relocated to
    // ApplicationNetworkSetup.cpp.  `setup` sits at its
    // final address for the whole run: MultiRinexSync references setup.roster,
    // and the closed-loop engine holds &setup.network_store plus a GnssConfig&
    // to setup.consumer_config (see NetworkSetupResult.hpp).
    NetworkSetupResult setup;
    if (const int rc = networkSetup_(setup); rc != 0) return rc;
    // Alias block (INC-3/4/5 migration mechanic): the run loop reads the setup
    // outputs under their former local names; every use-site below is unchanged.
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
    // Decomposition INC-4: the network run context.  ctx.knobs holds the const
    // config snapshots (NetworkKnobs::snapshot preserves the original getter
    // read order); ctx.carry the Tier-1 cross-epoch run state (the RTB-7
    // snapshot's superset); ctx.counters the service counters/anchors.  The
    // alias blocks that replaced the former declaration stretches keep every
    // downstream use-site textually unchanged (the INC-3 migration mechanic).
    NetworkRunContext ctx(config_, nc);

    // D2 process noise (variance growth per second) for the carry-forward prior.

    // --- Trace ----------------------------------------------------------------
    // Stream + path ownership and the startup open logic live in
    // logging/NetworkRunTraces; each stream's full
    // documentation — column semantics, arming toggle, P1 rotation policy — is
    // on the member in that header.  The references below keep every writer
    // use-site in this TU textually unchanged; the per-epoch writers themselves
    // still live here and move with the increments that introduce the state
    // they read (INC-4+).
    NetworkRunTraces traces(log_dir_);
    auto& trace                = traces.trace;
    auto& dd_frac_trace        = traces.dd_frac_trace;
    auto& slip_raw_trace       = traces.slip_raw_trace;
    // Paths referenced outside the open logic (P1 rotation + the exit summary).
    std::unique_ptr<PppUtils> dd_ppp_utils;   // B5: wavelength provider (built only when the DD trace is on)
    // Producer-side network AR (network.ambiguity_resolution, Phase 2+): a
    // wavelength provider for the between-station DD meters→cycles conversion,
    // built only when armed (default OFF => the AR block is skipped entirely =>
    // byte-identical float product).
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
        // Card G RESPONSE (§G3/W1): the drain consumes the SAME GapResumption
        // records as the instrument, so it arms the resumption collection on its
        // own — an operator must not have to arm a diagnostic to get the fix,
        // and turning the diagnostic off must not silently disarm it.
        sp.gap_reentry             = sq.gap_reentry;
        sp.gap_reentry_lli         = sq.gap_reentry_lli;
        sp.gap_reentry_unmeasurable = sq.gap_reentry_unmeasurable;
        // Card GRS: the per-SIGNAL rider of the same response.  It also arms the
        // per-signal half of the compute and qualify passes, so an OFF run never
        // builds a per-signal list and never reads one.
        sp.gap_reentry_per_signal  = sq.gap_reentry_per_signal;
        sp.gap_reentry_per_signal_unmeasurable = sq.gap_reentry_per_signal_unmeasurable;
        // Bridge v3 E4: the GF-row capture needs the qualified raw verdicts.
        sp.bridge_gf           = config_.networkCycleSlipRelevelBridgeGf();
        // CS-GF authority (iono_carry_source = smoothed_gf): same need.  The
        // LV-6 census arms the ADVANCE independently — the kernel is
        // 8(b)-sanctioned instrumentation; the carry's falsified inject path
        // stays gated on carry_iono and untouched (SPEC_level_authority_internal
        // §3).  ⚠ Arming iono_cs flips cardsArmed(): census A/B pairs must pin
        // cycle_slip.gap_instrument in BOTH arms.
        sp.iono_cs             = (config_.networkCarryIono()
                                  && config_.networkIonoCarrySource() == "smoothed_gf")
                                 || config_.networkLevelAuthorityCsgf();
        // Corroborated retap: the SB_RAIM keep
        // consumes the qualified verdicts, so it must arm the evidence passes.
        sp.retap_corr          = config_.networkCycleSlipRelevelRetapCorroborate();
        // SPEC_lane_slip_response §7.3: the lane observatory consumes both the
        // raw rows AND the qualified verdict (d_mw / measured), so it must arm
        // anyConsumer AND cardsArmed — see the member's doc for the measured
        // failure when it armed only the former.
        sp.lane_snap           = config_.networkCycleSlipRelevelLaneSnap();
        slip_qc.hist.resize(stations.size());
    }
    // SIM-ONLY ORACLE slip response (network.cycle_slip.oracle_relevel,
    // include/slip/NetworkSlipOracle.hpp): the level-preserving re-level CEILING.
    // Constructed ONLY when armed — every drain takes a NetworkSlipOracle* that
    // stays null otherwise, so the OFF path is structurally byte-identical.  The
    // load THROWS on a missing/mis-pointed fixture (that is the field-inertness
    // mechanism); the throw propagates to runNetwork's caller like any other
    // fatal setup error rather than degrading to a silent zero-repair run, which
    // would be read as a falsification of the mechanism under test.
    if (nc.slip_qc.oracle_relevel) {
        ctx.carry.slip_oracle =
            networkSlipOracleLoad(config_, stations, log_manager_, log);
        ctx.carry.slip_oracle->carry = &ctx.carry.local_carry;
    }
    // SIM-ONLY ORACLE AMBIGUITY PIN (`network.inject.events[] kind "oracle_pin"`,
    // include/filter/NetworkOraclePin.hpp, docs/SPEC_oracle_pin.md): hold the
    // float ambiguities in scope at the fixture's TRUE integers and read where the
    // remaining error goes.  Constructed ONLY when such an event is armed — every
    // call site tests the pointer, so the OFF path is structurally byte-identical.
    // The load THROWS on a missing/mis-pointed fixture (the field-inertness
    // mechanism, exactly as for the slip oracle above): a silently-degraded pin
    // would report the model's error budget WITHOUT the ambiguities held and be
    // read as the ceiling this card exists to measure.
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
    // B5/raw-slip wavelength providers — gated on the streams openAll just
    // opened (same conditions as the former inline opens); constructed HERE
    // because they need log_manager_, which the traces unit does not hold.
    // The raw-slip provider now serves THREE consumers (the trace plus Cards L
    // and G), so it is built whenever ANY of them asked — spec §1's
    // "compute once per epoch when any consumer needs it".  With none armed it
    // stays null and the whole raw-observable path is skipped.
    if (dd_frac_trace) dd_ppp_utils = std::make_unique<PppUtils>(log_manager_);
    if (slip_qc.params.anyConsumer(static_cast<bool>(slip_raw_trace)))
        slip_qc.utils = std::make_unique<PppUtils>(log_manager_);
    // Split-cadence clocks-only fast pass.
    // Two hooks, both no-ops when OFF: a clocks-only TICK on every decimated-out epoch
    // (the T2e hook), and the anchor HARVEST on every epoch that actually emitted.
    // fc_shadow additionally runs the tick on FULL epochs for grading, discarding the
    // result — the permanent instrument, never feedback.
    const bool fc_on     = nc.fast_clock.enable;
    const bool fc_shadow = nc.fast_clock.enable && nc.fast_clock.shadow;
    // The fast tier's own wavelength provider, built here for the same reason
    // (log_manager_ is not reachable from the seam).  It converts RAW RINEX cycles to
    // metres at BOTH the anchor harvest and the tick — and, critically, the wavelength
    // it returns is CACHED per band in the anchor, so the tick re-uses the identical
    // value.  A wavelength that differed by even 1e-9 m between the two ends would
    // multiply an ~1e8-cycle phase count into a metre-scale error, so "same number,
    // not merely same formula" is a correctness requirement, not tidiness.
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
    const bool   pl_warmup_on_conv  = ctx.knobs.pl_warmup_on_conv;  // T1d
    const bool   fullcov_on    = ctx.knobs.fullcov_on;       // C1: dense (P_prev+Q)⁻¹ shared carry
    const bool   carry_cpl_cond = ctx.knobs.carry_cpl_cond; // Card 1: conditional local carry (coupling rows) — requires fullcov_on (enforced at load)
    // M2 (LS-VCE carry-q): the carry-row variance-component MONITOR.  Accumulates
    // the post-fit carry innovations (carried − solved) into a residual-form
    // LS-VCE estimator and logs σ̂ (Floor m² + Rate m²/s) per family per
    // constellation against the hand floors.  NO feedback into the solve — the
    // whole path is gated on carry_vce, so OFF is byte-identical.  The estimator
    // is seeded with the PL floor/q as its weak a-priori (worth ~1 row; immaterial
    // vs the thousands of carry rows/epoch — pb/cb families share that weak PL-
    // scale prior, which only sets the regularisation and the no-data fallback).
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
    // M2 v2 APPLY: arm the LASER 5-95 variance-factor
    // iteration over the monitor above.  Unlike the monitor's estimator this is NOT a
    // run-TU local — α² is cross-epoch state read at three fold sites in three
    // translation units (the PASS-B ambiguity injector, the D2 shared injector, and
    // the two harvest mirrors), so it lives in ctx.carry per the decomposition rule
    // and every seam reaches it through ctx.  Inert (unity factor, no state written)
    // unless armed, so an OFF run is byte-identical.
    if (ctx.knobs.vce_apply_on)
        ctx.carry.vce_apply.configure(
            NetworkCarryVce::parseComponents(config_.networkCarryVceApplyFamilies()),
            config_.networkCarryVceApplySlew(), config_.networkCarryVceApplyMax(),
            config_.networkCarryVceApplyMinRows());
    // PRIOR-RESIDUAL CENSUS.  A run-TU local
    // for the same reason carry_vce_est is one (see the "engine objects stay
    // run-TU locals" note in app/NetworkRunContext.hpp) and threaded into exactly
    // the same four seams: SB_T1A advances it, SB_LEVELING feeds pl/zwd/rxcb,
    // SB_STASH_STATUS feeds pb/cb and drains it, and the teardown reads it.  It
    // shares the M2 monitor's per-row arithmetic and adds NO solve effect: with
    // the census armed the solution is byte-identical.
    NetworkPriorResidual prires_est(
        ctx.knobs.prires_on
            ? NetworkCarryVce::parseComponents(nc.prior_residual_families)
            : std::vector<std::string>{},
        static_cast<double>(nc.prior_residual_window_epochs),
        static_cast<double>(nc.prior_residual_min_rows),
        ctx.knobs.prires_report_sigma,
        ctx.knobs.prires_corrob,
        // The constraint-aware global variance component rides this same
        // accumulator — its prior side IS the census's folded rows — and takes its
        // data side from SB_RAIM, one seam ahead of the SB_STASH_STATUS drain.
        ctx.knobs.prires_s0g);
    // Corroboration resolver.  Injected here rather than plumbed through five
    // seam signatures, capturing slip_qc by reference because the evidence it
    // reads is refreshed per epoch inside that channel.
    //
    // It gathers DOMAIN COUNTS only; the verdict RULE is @ref
    // priorResidualConsensus, a pure function with its own unit cases.  That
    // split is what makes the roster-independence claim checkable without a
    // network, and it exists because an OR here SATURATED: at 303 stations `cb`
    // rows reading raw_stepped went 29 % -> 70 % and the b-cell collapsed
    // 57 % -> 21 % on identical data, purely because P(any of N steps) -> 1.
    //
    // A carried prior's DOMAIN is the (station, satellite) pairs whose raw MW/GF
    // observables can witness a discontinuity in it:
    //   * `pl`          -> ONE pair (station, sat): the arc's own verdict;
    //   * `zwd`, `rxcb` -> RECEIVER-level: that station's satellites;
    //   * `pb`, `cb`    -> SATELLITE-level: the stations that measured that sat.
    //
    // Both multi-member domains are aggregated once per epoch into the maps below
    // rather than re-scanned per row.  Otherwise a shared-family row would walk
    // every station map — 131 cb rows x 303 stations = 40k lookups per epoch —
    // making the census roster-quadratic.  Building them is one pass over the
    // verdict table and the per-row lookup is then O(log n).
    //
    // The cache is keyed on slip_qc.epoch_idx, the monotonic PROCESSED-epoch
    // index, so it self-invalidates exactly when the evidence is refreshed.  All
    // five feed sites are serial post-solve, so the mutable capture is race-free
    // by construction (the parallel produce phase never reaches this).
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
                            if (!kv.second.measured) continue;   // no predecessor ⇒ not in ANY domain
                            const int step = kv.second.stepped ? 1 : 0;
                            PriorResidualEvidence& a = cache->by_sta[i];
                            ++a.n_measured; a.n_stepped += step;
                            PriorResidualEvidence& b = cache->by_sat[kv.first];
                            ++b.n_measured; b.n_stepped += step;
                        }
                }
                if (station >= 0 && static_cast<std::size_t>(station) < ep.verdict.size()) {
                    if (sat.empty()) return cache->by_sta[static_cast<std::size_t>(station)];
                    // pl: a ONE-member domain.  priorResidualConsensus degenerates
                    // to the member's own verdict there, so this family is
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
    // The accumulator lives in ctx.carry (its feed site is inside the
    // redundancy-census seam, which takes ctx but no run-TU reference), so it is
    // SEATED here from config rather than constructed by the context: an unarmed
    // run keeps the default-constructed empty-family instance, whose enabled() is
    // false, and allocates nothing.
    if (ctx.knobs.bb_on) {
        ctx.carry.bb = NetworkBlockBaarda(
            NetworkCarryVce::parseComponents(nc.block_baarda_families),
            static_cast<double>(nc.block_baarda_window_epochs),
            static_cast<double>(std::max(0, nc.block_baarda_min_blocks)),
            ctx.knobs.bb_report_sigma, ctx.knobs.bb_corrob);
        // Corroboration resolver.  A block IS a (station, satellite), exactly the
        // key RawSlipEpoch::verdict is stored under, so unlike the prior-residual
        // resolver this needs no OR-fold over stations or satellites.  That
        // directness is why the b-cell means something at block granularity.
        //
        // `station` here is the APP roster index (the census loop's `i`), NOT the
        // kernel slot: ep.verdict is indexed by the former and a slot lookup would
        // silently read another station's satellites.
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
                    // difference against, is NO EVIDENCE — never "flat".  Absence
                    // of evidence is not evidence of absence, and a code-only or
                    // single-frequency LOS lands here BY CONSTRUCTION.
                    if (it == vmap.end() || !it->second.measured)
                        return PriorResidualCorrob::NoEvidence;
                    return it->second.stepped ? PriorResidualCorrob::Stepped
                                              : PriorResidualCorrob::Flat;
                });
        }
    }
    // STATION CONGRUENCE.  Seated here for the
    // block census's reason (its feed site is inside the redundancy-census seam),
    // and with no resolver to install: the INC-3 separability join reads the block
    // census's own hit count at the sweep site, not an injected callback.  An
    // unarmed run keeps the default-constructed UNARMED instance.
    if (ctx.knobs.cg_on)
        ctx.carry.cg = NetworkCongruence(/*armed=*/true, kCongruenceWindowEpochs,
                                         kCongruenceWarmupSamples,
                                         ctx.knobs.cg_report_sigma);
    const bool   ura_on        = ctx.knobs.ura_on;                  // B1: emit per-sat URA quality channel
    const bool   raim_on       = ctx.knobs.raim_on;            // post-fit network RAIM (Increment 3, T2)
    const bool   clock_signal_basis_hold = ctx.knobs.clock_signal_basis_hold; // W6: HOLD each sat's IF-pin reference pair fixed across epochs (time-consistent δtˢ↔dˢ split) instead of re-selecting from each epoch's coverage — inert without estimate_sat_code_bias
    // Two-state (offset+drift) oscillator clock model (network.clock_model): when on, the
    // satellite clock is carried via the Allan-derived Q model in clock_osc (below) instead of
    // the scalar prev_carry random walk, and a satellite is flagged by the normalized-innovation
    // dynamics-fault test rather than the absolute/jump bounds.
    const bool   clk_model_on   = ctx.knobs.clk_model_on;
    // Emit C1 (clock rate) from the finite-differenced solved clock WITHOUT the two-state carry
    // (network.emit_clock_rate): decoupled rate estimator (clock_rate_est below) — no prior, no
    // refloat ⇒ byte-identical solution.  Inert when clk_model_on (which already supplies C1).
    const double start_offset_s = ctx.knobs.start_offset_s;  // testing fast-forward (0 = off)

    // L15: run-provenance manifest — startup record (body relocated to
    // NetworkRunTraces, decomposition INC-4; manifest_path is a traces member).
    traces.writeRunManifestStartup(config_, nc, ctx.knobs, config_path_,
                                   stations.size(), n_threads, log);
    auto& prev_tow = ctx.counters.prev_tow;
    auto& prev_week = ctx.counters.prev_week;
    auto& n_emitted = ctx.counters.n_emitted;
    auto& last_health_write_s = ctx.counters.last_health_write_s;
    auto& n_gated_epochs = ctx.counters.n_gated_epochs;
    auto& n_gated_key_epochs = ctx.counters.n_gated_key_epochs;

    // --- Empirical-alpha flag-rate counters (network.trace_flag_rates) -----
    // Pure observation: {opportunities, flags} per QC tier, reported at end of run
    // as realized rate vs the implied one-sided Gaussian α for the configured k.
    // Never fed back into a decision; every increment is guarded, so OFF is
    // byte-identical.
    const bool trace_flag_rates = ctx.knobs.trace_flag_rates;
    auto& max_dur_anchor_s = ctx.counters.max_dur_anchor_s;
    auto& max_dur_anchored = ctx.counters.max_dur_anchored;

    // Single source of truth for "is (sat,signal) at station i a gauge-pinned
    // ambiguity this epoch?" — the predicate replacing the scattered is_pivot
    // gauge tests.
    //   Legacy (network.phase_datum_prioritized OFF): membership means the station
    //     IS the pivot, which pins the whole shared_phase_keys set, so this returns
    //     exactly is_pivot and every rewritten site stays byte-identical.
    //   Prioritized: membership means this "sat_signal" key is in station i's
    //     elected pinned_amb_keys — the anchor for that gauge freedom, its pin
    //     folded into its OWN local block.  Key form matches the pin fold and the
    //     ambiguity-state key.
    // The anchor-free sum-to-zero phase-bias datum reuses ALL the prioritized
    // machinery (no pivot-first, every station carries its arcs, per-(sat,signal)
    // shared set) and differs only in the gauge REALIZATION — a kernel sum-to-zero
    // fold instead of a per-(sat,signal) anchor pin.  So drive the prioritized code
    // paths under either mode and diverge only at the anchor election (skipped) and
    // the pin fold (inert: no station gets a pinned_amb_key here).
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
    // carried z-store lives across epochs).  SUPERSEDES the S-dependent
    // PhaseBias / local / RPB carries, the eq34 pin-round/shift hygiene for phase
    // keys (bypassed below), and the carry S-transform hygiene — the datum is
    // re-realized freely and the RPB re-estimates fresh, δ cancelling in the
    // gauge-free DD.  GLONASS (FDMA) is excluded.  OFF = no z-store and all folds
    // gated, byte-identical.
    NetworkInvariantCarryDriver::Config carry_cfg;
    carry_cfg.qz = config_.networkCarryInvariantQz();
    // Estimation-side fine-slip innovation gate: the invariant-carry-native
    // replacement for the RAIM fine-slip layer that the invariant bypass empties.
    // Requires carry_invariant (config-rejected otherwise); OFF = byte-identical.
    const bool carry_innov_gate = carry_invariant && config_.networkCarryInvariantInnovGate();
    carry_cfg.innov_gate = carry_innov_gate;
    carry_cfg.innov_k    = config_.networkCarryInvariantInnovK();
    // D5 v2: LS-VCE calibration of the gate threshold (σ̂₀²); requires innov_gate (config-rejected).
    carry_cfg.innov_calibrate        = carry_innov_gate && config_.networkCarryInvariantInnovCalibrate();
    // Card 4 D5 v3: corroboration gate on the re-tap.  The loader already forced the
    // knob false if the D5 gate itself is off, but AND it here too so the driver Cfg is
    // self-consistent regardless of load path.
    carry_cfg.innov_corroborate      = carry_innov_gate && config_.networkCarryInvariantInnovCorroborate();
    carry_cfg.innov_corroborate_backstop_k = config_.networkCarryInvariantInnovCorroborateBackstopK();
    carry_cfg.innov_carried_only     = carry_innov_gate && config_.networkCarryInvariantInnovCarriedOnly();
    carry_cfg.innov_calibrate_window = config_.networkCarryInvariantInnovCalibrateWindow();
    // Card 4 PD-SAFE DD fold.  The driver half owns
    // invariant #7's two edges — the install-time degenerate-block rebirth and
    // the warm-resume restore guard; the kernel half (est.setDdFoldPdSafe, below)
    // owns the arithmetic.  The loader already disarms it without carry_invariant, but AND
    // it here too so the driver Cfg is self-consistent regardless of load path.
    carry_cfg.pd_safe = carry_invariant && config_.networkCarryInvariantPdSafe();
    // RS-2 (SPEC_event_harness §21.4): the LINEARIZATION-frame halves of the carry.
    // carry_lin captures Σ c·x_lin per chord at every harvest (write side, snapshot only);
    // carry_rebase re-bases a restored branch onto THIS run's x_lin at the first post-resume
    // DD build (read side).  The loader disarms carry_lin without state_snapshot_carry; AND
    // both with carry_invariant here so the driver Cfg is self-consistent on any load path.
    carry_cfg.carry_lin    = carry_invariant
                             && config_.getNetworkConfig().state_snapshot_carry_lin;
    carry_cfg.carry_rebase = carry_invariant && config_.networkResumeCarryRebase();
    // M5 card WL-3: the lane-coordinate carry.  The loader
    // enforces carry_invariant + mw_wl.ewl_enable.  lane_joint discriminates the LANE
    // shape (joint 2K pair blocks, lane prior/q, lane measurement rows, fold-on-change
    // fixing) from the §6.1 EQUIVALENCE shape (all lane features at their isotropic/off
    // sentinels ⇒ split per-band emission, information-identical to the per-band carry).
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
    // M5 card WL-1: the MW-seeded wide-lane pseudo-observation layer.
    // Rides the carry driver's chord basis, so it requires carry_invariant (config-rejected
    // otherwise).  OFF (mw_wl.enable=false) ⇒ NetworkMwWlLayer::build returns no blocks and the
    // per-station MW detectors are not built for it, so the producer is byte-identical.
    const bool mw_wl_on = carry_invariant && config_.networkMwWlSeed();
    // M5 card WL-2: when the EWL lane is armed the
    // constellation's THIRD band (the EWL pair's uncarried leg — GPS L5, Galileo E5b) is
    // deliberately EXCLUDED from the invariant-carry driver so that lane acquires no unearned
    // stiff prior (the F6 wall) and its EWL rows win cleanly.  The band stays in the eq34
    // election/datum (shared_phase_keys/pins are built upstream, untouched).  OFF ⇒ the third
    // band is carried exactly as today (byte-identical).  v1 = GPS+Galileo (GLONASS never carried).
    const auto& nmw = config_.getNetworkConfig().mw_wl;
    const bool ewl_carry_exclude = carry_invariant && nmw.ewl_enable;
    // M5 card UWL-1: under the armed card the uncarried band
    // comes from the per-constellation lane-tier table (Galileo's top band is selectable:
    // '7' = E5b λ 9.768 m, '8' = E5 AltBOC λ 19.537 m).  uwl_enable=false keeps the legacy
    // digits verbatim — the §6.1(b) gate: arming a constellation without the card changes
    // NOTHING.  For GPS (and Galileo at the default '7') the table reproduces the legacy
    // digits, so armed GPS-only runs stay bit-identical too.
    const bool uwl_excl_on = ewl_carry_exclude && nmw.uwl_enable;
    const char uwl_gal_top = nmw.uwl_gal_top_band;
    const bool uwl_dual    = nmw.uwl_dual_top;   // increment 5: BOTH {b_top, b_top2} uncarried
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
    mw_wl_cfg.debug_r_scale      = config_.getNetworkConfig().mw_wl.debug_r_scale;   // F6 probe (diagnostic)
    NetworkMwWlLayer mw_wl_layer(mw_wl_cfg);
    if (mw_wl_on && mw_wl_cfg.debug_r_scale != 1.0)
        log->warn("Network (M5/WL-1): network.mw_wl.debug_r_scale = {} — DIAGNOSTIC probe, the WL "
                  "rows' Pz is scaled (not honest MW variance).  Do NOT grade this run.",
                  mw_wl_cfg.debug_r_scale);
    // M5 card WL-2 EWL lane: the SAME layer, run on the
    // (band2, band5) pair with FFR statistic-gated fixing.  The third band is uncarried
    // (excluded above) so these rows win with no F6 fight; a passed FFR folds an integer-hard
    // row.  Rides the primary-sig1 cycle basis exactly as WL-1.
    const bool ewl_on = carry_invariant && nmw.ewl_enable;
    // M5 card UWL-1: the per-constellation top tier.  uwl_on generalizes the EWL lane's
    // hardcoded GPS band digits to the lane-tier table (Galileo UWL (E5b|E5,E5a)); the
    // cold fix additionally admits n=1 UNLAGGED statistics to the DECISION on tiers with
    // λ ≥ uwl_cold_min_lambda_m (SPEC §3.3 — GPS's 5.861 m EWL deliberately excluded).
    const bool uwl_on      = ewl_on && nmw.uwl_enable;
    const bool uwl_cold_on = uwl_on && nmw.uwl_cold_fix;
    // 60-ep age-bias fix: the bridge accumulates PER-SAMPLE iono-de-biased GF, so
    // every read-time C_iono·Σc·ı̃ subtraction is DISABLED (the frames must never
    // mix — subtracting twice re-creates the bias with the opposite sign).
    const bool bridge_debias_on = ewl_on && nmw.bridge_sample_debias;
    // E6 mid-ladder bridge hop (census-motivated): chain the WL decision through the
    // Galileo (E6,E5a) λ 2.931 m rung — re-rounding at λ_MID strips the top hop's
    // λ-scale phase/iono amplification before it reaches the 0.751 m WL statistic.
    const bool uwl_mid_on  = uwl_on && nmw.uwl_mid_enable;
    // Increment 5: per-cycle preferred('8')/fallback('7') Galileo top rung + the
    // untaken-sibling closure statistic (2·N(8,5) = N(7,5)); hard gate default OFF
    // (measure the residual distribution first — SPEC reconciliation note 2).
    const bool uwl_dual_on = uwl_on && nmw.uwl_dual_top;
    const bool uwl_sib_gate_on = uwl_dual_on && nmw.uwl_sib_gate;
    // v2 census recovery: minority receiver-family primary pairs get their OWN lane
    // pair groups (admissible census-pair SET instead of the single majority pair).
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
    // One statistic-layer instance per TABLED constellation when the card is armed
    // (per-cons telemetry — the F-A dump taught us pooled lines hide which lane is
    // storming); a single filterless instance otherwise (the legacy shape — for a
    // GPS-only run the 'G'-filtered instance is bit-identical to it, the filter never
    // fires).  An armed constellation that decides 0 cycles prints a loud 0-line
    // (invariant #4), never silence.
    std::vector<EwlLane> ewl_lanes;
    if (ewl_on) {
        if (uwl_on) {
            for (const char cons : { 'G', 'E' }) {
                const auto tier = mwLaneTierOf(cons, nmw.uwl_gal_top_band,
                                               nmw.uwl_mid_enable, nmw.uwl_dual_top);
                if (!tier) continue;
                NetworkMwWlLayer::Config cfg = ewl_cfg;
                cfg.cons_filter = cons;
                // Increment 5: the PREFERRED rung's instance runs FIRST (its ewl_fixed
                // entries precede the fallback's, so the walk's dedupe keeps the wider-λ
                // decision); the fallback instance decides only what the preferred one
                // did not (a cycle with any preferred-band-less arc dies there as
                // skip_missing_band — the per-cycle "where tracked" resolution).
                ewl_lanes.push_back(EwlLane{ cons, tier->b_top, NetworkMwWlLayer(cfg), {}, {} });
                if (tier->b_top2 != '\0')
                    ewl_lanes.push_back(EwlLane{ cons, tier->b_top2,
                                                 NetworkMwWlLayer(cfg), {}, {} });
            }
        } else {
            ewl_lanes.push_back(EwlLane{ '\0', '\0', NetworkMwWlLayer(ewl_cfg), {}, {} });
        }
    }
    // M5 card WL-2 WL lane (SPEC §3.3/§3.4): the phase-bridge WL decision + the NL-preserving
    // inject.  The bridge path needs the EWL integers, so it runs only when BOTH masters are on;
    // wl_inject alone runs the MW(1,2) fallback path (§3.5, deferred to a later increment — v1
    // requires the bridge, else no WL decision is produced and the lane is inert but armed).
    // WL-3 supersedes the WL-2 hold/inject walk at runtime (loader-warned): under
    // lane_carry the decided integers become fold-on-change LANE fixes on the pair
    // blocks and the inject primitive survives only as the pair-block fallback.
    const bool wl_inject_on = ewl_on && nmw.wl_inject_enable && !lane_on;
    // WL-3 per-epoch lane measurement sources; the FFR fix walk runs whenever
    // the joint lane shape is armed (fold-on-change is the §3.3 core, no separate knob).
    const bool lane_meas_on   = lane_on && lane_joint && lane_cfg.lane_meas;
    const bool lane_bridge_on = lane_on && lane_joint && lane_cfg.bridge_meas;
    const bool lane_fix_on    = lane_on && lane_joint;
    // Fan the MW lane-increment folds across components (byte-identical to serial).
    const bool lane_parallel_components = lane_on && lane_joint && lane_cfg.parallel_components;
    // One decided WL inject: the two bands' carry groups + the shared chord ArcKey + the integer
    // target + the per-band linearization DD sums (F5 correction frame) + wavelengths.  Computed
    // in buildCarryDdPriors from DETECTOR statistics (§3.0.1 decision source), APPLIED post-solve
    // against the fresh posterior (so the preserved NL sum is this epoch's).  Refreshed each build
    // (the last before the accepted solve wins, mirroring ewl_epoch).
    std::vector<WlInjectPlan> wl_inject_epoch;   // per-build; consumed post-installPosterior.
    // F-A attribution dump (network.mw_wl.d5_dump, log-only): adjacency tags for each D5
    // flag — the (station, sat) pairs an EWL-FIXED cycle touched this build, the chords of
    // hold-folded cycles this build, and the chords injected LAST epoch (an inject's
    // residual tension lands in the NEXT install; the injected chord itself is exempt, its
    // component siblings are not).  All rebuilt per accepted build; empty unless d5_dump.
    const bool mw_d5_dump = (ewl_on || wl_inject_on || lane_on) && nmw.d5_dump;
    // F-B lane trace (network.mw_wl.lane_trace, log-only): per epoch, the carried WL lane
    // value of EVERY primary cycle whose chord is carried in both bands — the constrained
    // (held/injected) and unconstrained populations in one run, so the injected-cycle circR
    // is finally gradable (scripts/grade_lane_trace.py).  Entries built per accepted build;
    // written post-install against the FRESH posterior.
    const bool mw_lane_trace = (wl_inject_on || lane_fix_on) && nmw.lane_trace;
    const bool mw_tagsets    = mw_d5_dump || mw_lane_trace;   // hold/inject tag sets feed both
    // Both dumps are ordinary NetworkRunTraces streams — opened ONCE, truncating, with
    // their header, before the epoch loop.  The open sits here rather than in openAll
    // only because the arming expressions above are not derived yet at that point.
    // The D5 arm carries carry_innov_gate for the same reason its writer does: without
    // the gate there are no flags, so an unarmed gate must yield NO file (not an empty
    // one), keeping the artifact set an honest report of what actually ran.
    traces.openMwWlDumps(mw_d5_dump && carry_innov_gate, mw_lane_trace, log);
    std::vector<LaneTraceEntry> lane_trace_epoch;   // per-build; consumed post-install.
    WlCounters wl_run, wl_epoch;
    // Per-constellation WL-walk tallies when the card is armed (pooled lines hide
    // which lane is storming).  wl_run stays the aggregate — the per-epoch cons tallies
    // fold into BOTH at the accepted-install accumulation.
    std::map<char, WlCounters> wl_cons_epoch, wl_cons_run;
    // Accepted-install accumulation helper (one place — the field list bit-rots in two).
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
    // EITHER lane is armed (mirrors ar_ppp_utils).
    std::unique_ptr<PppUtils> mw_wl_ppp_utils;
    if (mw_wl_on || ewl_on || lane_on) mw_wl_ppp_utils = std::make_unique<PppUtils>(log_manager_);
    NetworkMwWlLayer::Counters mw_wl_run;    // run-total eligibility/skip tallies (invariant #4)
    NetworkMwWlLayer::Counters mw_wl_epoch;  // per-build tallies (the last build before the accepted solve wins)
    // M5 card WL-1 F6 diagnostic: one-shot per-arc/per-row attribution dump of the FIRST
    // arming epoch's WL rows.  Populated in
    // buildCarryDdPriors (build-time: z_MW/z_lin/z_corr + per-arc MW̄/x_lin, where the
    // resolver + revmap are in scope) and finalized post-solve (reconstructed_abs from
    // reconstructStation + the kernel's WL-block innovation).  Log-only; default OFF.
    const bool mw_wl_f6_dump = mw_wl_on && config_.getNetworkConfig().mw_wl.f6_dump;
    F6Capture mw_wl_f6;
    // Card 4 phase D: the emit-layer frame re-base.
    // Re-references the emitted per-(sat,σ) phase biases onto the previous EMISSION
    // frame so the freely-re-realized eq34 datum's per-epoch gauge churn does not
    // storm the consumer with spurious ambiguity resets; a genuine reference-path
    // slip's integer step is left intact so the emitter jump test still fires.
    // Default-ON when carry_invariant is armed (spec D7); the OFF path is
    // carry_invariant=false (byte-identical — the engine is never touched).
    const bool emit_rebase_on = carry_invariant && config_.networkCarryInvariantEmitRebase();
    NetworkEmitRebase emit_rebase_engine;
    // M5 prerequisite: PB emission-continuity filter — a slip-gated per-key scalar
    // KF on the RE-BASED emitted biases + a self-tuning k·σ̂_step jump gate that
    // clears the threshold-in-noise discontinuity floor.  Runs INSIDE the re-base
    // block (it filters the re-based emission frame), so it requires emit_rebase_on;
    // OFF (default) ⇒ filterEpoch is never called (byte-identical — the engine is
    // constructed like emit_rebase_engine but stays inert).
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
    // Card CLK-R: clock emission re-base — the
    // common-mode frame hold (stage 1) + per-sat step marks (stage 2).  Run-TU
    // local like the PB engines (NetworkRunContext banner rule); both knobs
    // default OFF ⇒ rebaseEpoch is never called (byte-identical).  Outputs-only:
    // the solve is byte-identical even when armed.
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
    // Fix A seed-once + time-lock (network.phase_datum_hold_sigma_m).  The un-held
    // ensemble references the emitted bias to mean_r(ẑ_r) — a per-epoch-re-realized
    // float-ambiguity mean whose absolute LEVEL is code/iono-precision-limited — so
    // the product churns ~metre/epoch (a dynamic datum on a static product).  When
    // >0 the gauge is SEEDED once per (sat,signal) and thereafter the shared bias is
    // HELD by a stiff temporal prior of this sigma: the Σz=0 fold is skipped for a
    // (sat,signal) that already has a carried bias, and the D2 stash carries that
    // bias with a fixed (clean) hold-variance instead of dropping it.  0 = re-realize
    // every epoch (the churny baseline); inert unless phase_datum_ensemble.
    const bool   phase_hold_on    = ctx.knobs.phase_hold_on;
    // Assembly helpers relocated to ApplicationNetworkAssembly.cpp:
    // isGaugePinned / splitStation / appendGimIonoRows (+ appendShIonoRows
    // below) are now the Application member functions networkIsGaugePinned_ /
    // networkSplitStation_ / networkAppendGimIonoRows_ / networkAppendShIonoRows_
    // — former [&] captures are explicit parameters or member reads.
    auto& sh_dev_carry = ctx.carry.sh_dev_carry;
    const double sh_dev_tau = sh_model ? shc.rep_error_tau_s : 0.0;
    const bool   sh_dev_on  = sh_dev_tau > 0.0;
    const bool   sh_dev_demean = sh_dev_on && shc.rep_error_demean;  // Iono-2 v3 anti-ratchet
    const bool   sh_dev_d5_keep = sh_dev_on && shc.rep_error_d5_keep;  // v3k erase-policy split

    // ---- Station-adaptive SH-VTEC-constraint deweight (iono deweight card) ----------
    // Per-station factor w_s ∈ (0,1] scaling the INFORMATION of this station's SH↔slant
    // iono pseudo-obs (σ² ×= 1/w_s ⇒ info ×= w_s).  w_s→0 decouples the station's per-LOS
    // iono from the global SH field in BOTH directions (the single joint pseudo-obs means
    // one w_s covers both) while the station still serves the shared
    // clocks/biases/datum/position.  All the gate math (v1 static list + v2 network-relative
    // IGG auto-trigger, EWMA + hysteresis + slew) lives in NetworkIonoDeweightGate; here we
    // just feed it per-station statistics in the serial post-solve harvest and READ
    // iono_deweight.weight() in the parallel assembly (const find, race-free — the
    // sh_dev_carry contract).  Empty static list + adaptive OFF ⇒ weight() is 1.0 for every
    // station ⇒ byte-identical to OFF (invariant #6).
    const IonoConstraintDeweightConfig& idw = config_.getIonoConstraintDeweightConfig();
    NetworkIonoDeweightGate iono_deweight(
        std::set<std::string>(idw.stations.begin(), idw.stations.end()), idw.factor,
        idw.adaptive, idw.window_epochs, idw.sigma_hi, idw.sigma_lo, idw.floor,
        idw.rate_limit, idw.min_epochs);
    // Iono-deweight RELEASE hygiene: arm the
    // gate's per-epoch release-edge capture ONLY when a response is armed, so an
    // unarmed run never allocates the list.  The edge is "the weight rose above eps",
    // captured around the rate-limited slew — the engage/release transition list cannot
    // serve it (a station climbs off the floor with no flag flip whenever its z decays
    // inside the hysteresis band, and Transition::w is post-slew only).
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

    // appendShIonoRows relocated to ApplicationNetworkAssembly.cpp:
    // Application::networkAppendShIonoRows_ — sh_model / iono_deweight / shc /
    // sh_dev state now passed explicitly at the PASS-B call site.

    // Pin Eigen to a single thread for the duration of the (parallelized) epoch
    // loop.  _OPENMP is defined project-wide so Eigen threads its own dense
    // kernels; nesting those inside our per-station OpenMP region would cause
    // thread oversubscription AND non-deterministic reduction order in the
    // per-station matmuls.  Single-threaded Eigen inside each station thread is
    // fully thread-safe (separate matrices) and deterministic; restored after the
    // loop.  Only touched when parallelism is active, so the serial path (and the
    // small-network case) is byte-identical to before.
    const int eigen_threads_saved = Eigen::nbThreads();
    if (parallel_on) Eigen::setNbThreads(1);

    // [TIMING] Wall-clock of just the per-epoch loop (excludes ~startup: product
    // cache load + per-station RINEX parse).  Isolates the per-epoch compute that
    // scales with station count — the scaling metric.  Counts only epochs that
    // actually entered processing (past the max-duration cutoff).
    const auto t_loop_begin = std::chrono::steady_clock::now();
    auto& n_loop_epochs = ctx.counters.n_loop_epochs;
    auto& decim_counter = ctx.counters.decim_counter;
    auto& sh_converged_latch = ctx.carry.sh_converged_latch;

    // The joint estimator is HOISTED out of the epoch loop and reset()/reused each
    // epoch.  Constructing a fresh NetworkEstimator per epoch reallocated one dense
    // Schur block PER STATION every epoch; the process heap retained that churn as
    // committed memory, saturating at ~0.5 GB/station (≈146 GB at 305 stations over
    // a 121-epoch run → exhausted RAM).  Reusing the estimator's station-block pool
    // (NetworkNormalEquations::reset) holds the footprint to one epoch's working set.
    NetworkEstimator est;
    // OPT-IN fan-out of the Schur reduction, kept SEPARATE from the build-phase
    // parallelism.  The build's serial-ordered scatter is byte-identical to a
    // serial run, but the reduce fan-out reassociates the per-station summation:
    // reproducible per thread count, yet ULP-different from the serial reduce, and
    // the network's ill-conditioning amplifies that to ~1e-5 in the products —
    // physically null but not byte-identical.  Default OFF keeps the reduce serial
    // while the build still threads and the deterministic serial-tail wins
    // (eigensolver gate, no double-reduce, factor reuse) still apply.
    //
    // parallel_reduce and parallel_carry are MUTUALLY EXCLUSIVE: the first fans
    // reduce()'s per-station Schur loop, the second fans the fold's per-station
    // reduction while est.solve()'s dense algebra is itself Eigen-threaded, so
    // arming both nests OpenMP inside Eigen and oversubscribes.  The loader
    // resolves it (parallel_carry forced OFF when both are set), so at most one is
    // true here and the `!carry_par` below is belt-and-suspenders.
    const bool reduce_par   = parallel_on && nc.parallel_reduce && !carry_par;
    const int  reduce_threads = reduce_par ? n_threads : 1;
    est.setReduceThreads(reduce_threads);
    // Deterministic-parallel DD carry-fold fan-out (network.parallel_seam); 1 = serial.
    est.setSeamThreads(seam_threads);
    // network.parallel_carry: when armed, solve()'s foldReducedDdPriors runs SERIAL-over-
    // blocks with WITHIN-BLOCK per-station threading (Eigen stays PINNED, so the driver does
    // NOT un-pin it around solve()).  OFF ⇒ the fold keeps seam_threads (block-parallel
    // seam), byte-identical.
    est.setCarryParallelThreads(carry_par ? n_threads : 1);
    // Resolve the null-space rank-gate tolerance
    // ONCE, here with the estimator's other knobs, so the gate call sites, the
    // estimability dump and the prioritized drop-and-heal all read the SAME value
    // off the estimator instead of each reaching for the compile constant.  The
    // default is NetworkEstimator::kNullSpaceTol, so an unset key sends the
    // identical double through the identical arithmetic (byte-identical).
    est.setRankGateTol(config_.networkRankGateTol());
    // network.parallel_carry: RAII un-pin of Eigen's threading around the two
    // GEMM-bound carry stages — buildDdPriors (W·Pᶻ·Wᵀ) and installPosterior
    // (posterior-covariance read-back) — whose big dense products Eigen threads
    // well.  The third stage, foldReducedDdPriors, is deliberately NOT wrapped:
    // its cost is a per-station reduction (small inner-dim matmuls plus a serial
    // LDLT) that Eigen cannot thread, so it self-fans with its own OpenMP and
    // Eigen PINNED, and wrapping solve() would nest the two.
    //
    // The epoch loop pins Eigen to one thread to avoid nesting inside the
    // per-station OpenMP assembly region; these two stages run outside it, so
    // un-pinning fans their GEMMs across cores.  The pin is restored on EVERY exit
    // path — early continue, withheld-epoch skip, thrown call — so the next
    // epoch's assembly and the fold's own OpenMP never run with Eigen un-pinned.
    // Inert unless armed, so an OFF run is byte-identical.

    // Pin-aware station PD guard (network.station_pd_pin_aware): median-|pivot|
    // reference scale for the local-block rank test — the prioritized datum's
    // in-block 1e10 anchor pins otherwise eject marginal stations every epoch.
    est.setStationPdMedianScale(config_.networkStationPdPinAware());
    // Card RF-PD (network.trace_station_pd_null): eigen-read the NULL DIRECTION of a
    // block the PD test rejects, so the drop names a parameter instead of a pivot
    // magnitude.  Measure-only — it runs only on an already-rejected block.
    est.setStationPdNullTrace(config_.getNetworkConfig().trace_station_pd_null);
    // Card 4 PD-safe DD fold: the KERNEL half —
    // product-form posterior read-back, equilibrated scale-relative factorization, counted
    // eigenvalue-clamp repair.  Set once here (it survives est.reset(), like the timers).
    // OFF ⇒ byte-identical; the INC-0 counters it feeds are always-on and solve-neutral.
    est.setDdFoldPdSafe(carry_cfg.pd_safe);
    if (reduce_par)
        log->info("Network: Schur reduce PARALLEL across {} threads "
                  "(serial≡parallel byte-identity intentionally relaxed to ~1e-5)",
                  n_threads);

    // === Tier D0: per-epoch section timers (network.trace_section_timers) =====
    // Arm the estimator's wall-clock accumulators around assembly / reduce / solve
    // / the shared-covariance full inverse / the per-station covariance consumers,
    // so a profiling run can see which term dominates the per-epoch cost (⇒ which
    // Tier-D scaling lever is worth pulling).  The estimator accumulates CONTINUOUSLY
    // across the run; the driver snapshots it per epoch for the per-epoch delta line
    // and reads the run total at exit.  OFF (default) ⇒ no clock reads, byte-identical.
    const bool sec_timers_on = nc.trace_section_timers;
    est.setPerfTiming(sec_timers_on);
    est.resetSectionTimers();
    // INC-C0: arm the carry driver's per-op timers on the SAME toggle — splits the
    // SB_DDBUILD / SB_INSTALL wall brackets into basis-projection vs posterior read-back
    // vs D5-gate (the dense-K² ops the perf-restructure card ranks).  Byte-identical off.
    carry_driver.setPerfTiming(sec_timers_on);
    carry_driver.resetPerfTimers();
    double sec_asm_wall_ms = 0.0;      // D0: run-total driver-side assembly wall (produce + serial scatter)
    std::size_t sec_profiled_epochs = 0;  // D0: #epochs that reached the post-solve per-epoch readout
    if (sec_timers_on)
        log->info("Network (D0): per-epoch section timers ARMED "
                  "(assembly / reduce / solve / cov-inverse / cov-consumers).");


    // === Tier D0b: sub-instrument the "other" bucket (network.trace_section_timers) ==
    // "other" = loop wall − assembly − reduce − solve − cov-inverse − cov-consumers was
    // measured at ~81 % (42.9 s/ep) of the epoch on the 300 s GPS-only tier and scales
    // with LOS count — yet none of it was
    // timed individually.  These per-block driver-side wall timers attribute it.  For each
    // block, net = wall − Δ(reduce + solve + cov-inverse + cov-consumers) incurred INSIDE
    // it, so the recon/solve time already counted in the five D0 sections is NOT double-
    // counted: net is the block's TRUE contribution to "other" (a block that reconstructs
    // N stations shows its driver glue as net, its O(nₛ²) recon as cov-consumers).  The
    // brackets are disjoint and tile the loop body between the assembly wall and the
    // emitted-epoch readout; the residual (other − Σ net) is logged as "unattributed".
    // Inert (no clock reads, no est.sectionTimers() calls) when the toggle is OFF — the
    // subBegin/subEnd lambdas early-return, so byte-identity with an untimed run holds.
    enum SubBlock {
        SB_SLIP = 0,     // block 1: PASS-A slip scans (MW refloat + IF-TD jumps), pre-assembly
        SB_PREASM,       //          pre-assembly measurement build (post-slip → assembly wall)
        SB_GAUGEBUILD,   //          buildEnsembleGauges (Σz=0 gauges) — OFF (ensemble) in the tier
        SB_DDBUILD,      //          buildCarryDdPriors: eq34 arc graph + carry_driver.buildDdPriors
                         //          (W·Pᶻ·Wᵀ dense DD-cov projection over K carried chords) [Card-4]
        SB_SOLVEGLUE,    //          pre-solve prep + clock predict + solve + within-epoch re-solve
                         //          (net removes the internal solve/reduce)
        SB_INSTALL,      //          carry_driver.installPosterior (DD posterior mean/cov readback) [Card-4]
        SB_NETAR,        //          producer-side network AR Phase-2 (log-only; OFF in the tier)
        SB_SOLPREP,      //          toSolutionEpoch + per-sat discontinuity signalling
        SB_EMITSTORE,    // block 7: emit re-base + PB-emit filter + store push + solution CSV
        SB_IONODIAG,     // block 4: iono / ZWD / anchor-info viz traces (per-station recon sweeps)
        SB_DDFRAC,       // block 6: cross-station DD fractional-residual (B5) trace
        SB_SHDEV_VTEC,   // block 4: T5 SH-deviation carry KF + co-estimated VTEC coeff trace
        SB_T1A,          // block 3: T1a SH-field health probe + fused H1 adaptive-deweight gate
        SB_LEVELING,     // block 5: phase-leveling carry harvest + D2/C1 stash + T1d SH-conv
        SB_RAIM,         // block 2: post-fit network RAIM pass-1/2 + median-polish attribution
        SB_CLOCKUPD,     // block 8: two-state clock update + fault QC + retro-flag feed
        SB_FRAMEDIAG,    // P1-3/P1-4: shared-normal spectrum + post-fit sigma0 (both default-OFF)
        SB_STASH_STATUS, // block 5/8: D2/C1/SH carry re-stash + VCE fold + epoch-status QC
        SB_FASTCLOCK,    // FC-1: fast-clock anchor harvest + the shadow-mode grading tick
                         //       (the DECIMATED-out tick is outside the loop, untimed here —
                         //       it has its own ctx.counters.fc_wall_ms accumulator, since a
                         //       tick is not a loop epoch and must not enter the D0 wall)
        SB_COUNT
    };
    static constexpr const char* kSubNames[SB_COUNT] = {
        "slip-scans", "pre-asm", "gauge-build", "dd-build", "solve-glue", "install-post",
        "netar", "sol-prep", "emit-store", "iono-diag", "dd-frac", "shdev-vtec",
        "t1a-health", "leveling", "raim", "clock-upd", "frame-diag", "stash-status",
        "fast-clock"
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

    // === RTB-7(a): state snapshot + warm resume ==============================
    // Entirely OUTSIDE the product-file replay path: the snapshot restores the
    // producer's in-memory cross-epoch state; NetworkProductIO replay still
    // reproduces every discontinuity counter with no snapshot involved.  Path
    // empty (default) ⇒ nothing below runs — byte-identical.
    auto& snap_path = ctx.carry.snap_path;
    if (!nc.state_snapshot_path.empty()) {
        snap_path = nc.state_snapshot_path;
        if (snap_path.is_relative()) snap_path = log_dir_ / snap_path;
    }
    networkSnapshotFingerprint(ctx, config_, stations);
    networkTryWarmResume(ctx, config_, sync, emitter, carry_driver, log,
                         &clk_rebase_engine,    // §18.10: the CLK-R held frame travels too
                         &emit_rebase_engine,   // RS-5 §21.10: ...and the two PHASE-BIAS
                         &pb_filter_engine);    //   emit layers, for the same reason
    // RS-8 (§21.16): APPLY the canonical-signal latch the resume just staged.  Here and
    // not inside networkTryWarmResume because the latch lives in each station's PppModel
    // and the roster is in scope only from the driver; it must precede the first epoch,
    // which locks the latch.  Inert unless network.resume_signal_canon_restore is armed
    // and a warm resume actually staged rows.
    networkRestoreCanonicalSignals(ctx, config_, stations, log);
    // RS-9 (§21.18): hand the resume's stashed code-bias linearization points to the
    // station initializers.  HERE and nowhere else — the one-shot isCodeBiasInitialized
    // latch closes at each station's first computeMeasurementUpdate, so this is the only
    // window; a no-op unless the resume stashed RL rows.
    networkApplyRxCbLinRestore(ctx, config_, stations, log);
    // EVENT HARNESS (docs/SPEC_event_harness.md): arm the scripted-fault schedule.
    // AFTER the warm resume on purpose — the injector is deliberately outside the
    // resume fingerprint, so its ARMED banner must not be mistaken for restored
    // state.  Unarmed (network.inject absent) leaves every seam below untouched.
    networkInjectArm(ctx, config_, log_manager_, log);
    // Card CPA (docs/refound/CPA_canonical_pair_admission_2026-09-20.md): size the
    // per-station canonical-latch cache from the PRODUCER roster.  Disarmed leaves
    // every member of ctx.carry.cpa untouched and the seam unreachable.
    networkCanonicalPairArm(ctx, config_, static_cast<int>(stations.size()));

    // Resolve the a-priori satellite-clock anchor σ once — it is loop-invariant and
    // config-only — so the anchor fold and its information-fraction diagnostic
    // share one value.  The policy (explicit > 0 wins; brdc_rts auto → 3.0 m; sp3
    // auto → initial_std.sat_clock) lives in GnssConfig to stay a single
    // unit-testable source of truth.  The < 100 m binding gate stays at the fold
    // site in the driver.
    const double clk_anchor_sigma = config_.resolveNetworkAprioriClockAnchorSigmaM();

    // Run the code-bias admission census ONCE, pre-loop, so the registration site
    // can gate on a run-latched (sat,signal) verdict instead of a per-epoch
    // >= 2-station count (filter/NetworkBiasAdmission.hpp).  Armed only when both
    // the master flag and the sat-code-bias slice are on; otherwise
    // ctx.admission.active stays false and the legacy per-epoch path runs.
    //
    // The census window is [i0, i0+K) of the synced vector, i0 being the first
    // epoch the loop processes.  The loop start is a lazy time-anchored
    // fast-forward (network.start_offset_hours), not a precomputed index, so that
    // condition is replicated here against the first synced epoch's time.  A warm
    // resume starting a different span reclassifies over that span — per-process,
    // not carried in the snapshot, and field-inert for post-processing.
    //
    // Under streaming the front-end replaces the batch epoch source, so this
    // census would latch verdicts from FILE content the run never processes
    // (stale-day RINEX kept only for station metadata).  A wrong-but-plausible
    // latched admission is the quiet failure mode, so refuse it LOUDLY and fall
    // back to the per-epoch gate.
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

    // === RTF-3 (green carve-out): one synced epoch through the full producer ===
    // The former for-loop body, VERBATIM, as a callable over `se` alone — every
    // other name it reads is loop-invariant setup or ctx-carried run state,
    // captured by reference.  Loop control maps 1:1 (each top-level `continue`
    // → return Next; the max-duration `break` → return Stop), so the batch
    // iteration below is structurally identical to the former inline loop.  The
    // seam exists so a STREAMING epoch source (network.streaming — the RTF-3
    // multi-station front-end) can drive the identical per-epoch body without a
    // pre-built synced vector.
    enum class LoopSignal { Next, Stop };
    auto processSyncedEpoch = [&](const SyncedEpoch& se_raw) -> LoopSignal {
        // Decomposition INC-4: per-epoch workspace (the RAIM-replay capture
        // channel + the clock-model prediction maps) — constructed FRESH every
        // iteration (fresh construction IS the per-epoch reset semantics); see
        // filter/NetworkRunTypes.hpp EpochWorkspace for the writer-site contract.
        EpochWorkspace ws;
        // Optional testing fast-forward (network.start_offset_hours): skip epochs until the
        // elapsed data span from the first synced epoch reaches the offset, then cold-start the
        // solve there.  Skipped epochs are not counted, modeled, solved, or stashed, so the
        // carry begins empty at the offset — reproduces an externally-driven late-hour event
        // (e.g. the ~4.3 h lock-in) after a fresh warm-up, not the exact accumulated history.
        if (start_offset_s > 0.0) {
            const double now_s = TimeUtils::continuousGpsSeconds(se_raw.time);
            if (start_anchor_s < 0.0) start_anchor_s = now_s;
            if (now_s - start_anchor_s < start_offset_s) return LoopSignal::Next;
        }
        // Bind the log prefix BEFORE the injector so its own warnings carry THIS
        // epoch's bracket, not the previous one.  se.time == se_raw.time always
        // (the injector never touches the time tag), so moving this call across
        // the seam changes log text only.
        LogManager::setCurrentEpochTime(se_raw.time);
        // EVENT HARNESS seam (filter/NetworkInject.cpp).  THE single observation
        // injection site: upstream of networkFastClockTick, of computeModels, of
        // every T0 detector, of networkComputeRawObservables and of assembly, so a
        // one-scalar write on the RinexObservation (phase in CYCLES) moves the
        // metre-valued design row and the cycle-valued MW/GF/IF-TD feed together.
        // Placed AFTER the start-offset gate so a fast-forwarded epoch cannot
        // consume an event.  UNARMED: returns se_raw itself — no copy, no branch,
        // byte-identical.  `se` shadows nothing; the whole body below reads it.
        SyncedEpoch se_inject_scratch;
        const SyncedEpoch& se_admit = networkInjectEpoch(ctx, log, traces, se_raw,
                                                         se_inject_scratch);
        // CANONICAL-PAIR ADMISSION seam (filter/NetworkCanonicalPairAdmission.cpp,
        // card CPA).  Placed HERE, immediately after the injector and on the same
        // contract, because the site is the only one upstream of EVERYTHING the
        // withhold must be absent from: computeModels, the T0 detectors,
        // networkComputeRawObservables, assembly, the RAIM pools and the traces all
        // derive from this RinexObservation, so dropping the record here is exactly
        // "as if the RINEX record were missing", which is what lets the row-level
        // gap re-entry tier read the satellite's return as a RE-ENTRY.  Screens the
        // PRODUCER roster only, so the held-out closed-loop consumer is untouched.
        // DISARMED: returns se_admit itself (no copy, no walk, byte-identical).
        SyncedEpoch se_cpa_scratch;
        const SyncedEpoch& se = networkCanonicalPairAdmission(ctx, log, traces, stations,
                                                              se_admit, se_cpa_scratch);

        // Reduced-RHS class attribution: arm the estimator's shared-info capture
        // recorder BEFORE any prior-inject site runs this epoch (the post-solve
        // seam reads the captures).  One branch per epoch when disarmed.
        //
        // With the FLIGHT
        // RECORDER armed the recorder runs on EVERY epoch, because a trip is only
        // known post-solve and a trip that cannot attribute its own epoch is
        // worth very little.  Cost is ~10 vectors x n_shared doubles, cleared at
        // each beginEpoch — negligible next to the solve.
        est.attribArm(nc.rhs_attrib_on_trip
                      || (nc.rhs_attrib_tow >= 0.0
                          && se.time.tow >= nc.rhs_attrib_tow - 0.25
                          && ctx.carry.rhs_attrib_done < std::max(1, nc.rhs_attrib_n_ep)));
        // The per-epoch trip handoff is a ONE-EPOCH value: clear it here, at the
        // top of the epoch, so a trip raised by a site that runs before the
        // SB_FRAMEDIAG consumer cannot leak into the next epoch's dump.
        ctx.carry.rhs_trip_pending.clear();

        // Datum graph census: the spectral
        // hand-off is a ONE-EPOCH value too, and for a sharper reason — an epoch
        // that skips, withholds or throws before the rank gate would otherwise
        // leave the PREVIOUS epoch's eigenvalue count in place and the next
        // census pass would reconcile this epoch's topology against it without
        // any column saying so.  Cleared here, at the top, so the only thing the
        // census can ever read is an observation from its own epoch.
        if (ctx.knobs.dg_on) ctx.carry.dg_spec.clear();

        // Feed the staged broadcast nav records whose
        // availability (toc, GPS scale) this epoch has reached.  The store's
        // current/previous retention then evolves exactly as under live
        // arrival — IODE rolls happen at their true times (Card 6 re-levels
        // fire), and the fit-window guard sees fresh records instead of the
        // end-of-day pair a bulk load would leave.  The first processed epoch
        // catches up over any start_offset span in one sorted batch, which
        // ends in the identical store state as epoch-by-epoch feeding.
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
        // T2e: epoch decimation — process only every Nth synced epoch (N=1 ⇒ modulo 0 always
        // ⇒ never skips ⇒ byte-identical).  Placed AFTER the start-offset + max-duration guards
        // (so the physical-time window/break stay evaluated on every raw epoch) and BEFORE
        // ++n_loop_epochs (so decimated-out epochs are not counted — n_loop_epochs stays the
        // processed-epoch count).  The brdc_rts nav catch-up is idempotent across skips.
        {
            const size_t dn = static_cast<size_t>(std::max(1, nc.epoch_decimation_n));
            if ((decim_counter++ % dn) != 0) {
                // A decimated-out epoch has fully synced observations and is simply
                // not processed today.  With the tier armed it instead runs a clocks-only
                // fast tick against the last EMITTED full solve.  OFF => the bare
                // Next-return below is reached unchanged => byte-identical, structurally.
                // A tick is NOT a loop epoch: n_loop_epochs is untouched, the D0/D0b
                // brackets never open, and nothing outside ctx.carry.fast_clock is
                // mutated (the isolation contract).
                if (fc_on) networkFastClockTick(ctx, config_, log, traces, setup, se, false);
                return LoopSignal::Next;
            }
        }
        ++n_loop_epochs;  // [TIMING]

        // D0: per-epoch section-timer baseline.  Deltas of the estimator's (run-
        // cumulative) accumulators against this snapshot give THIS epoch's reduce /
        // solve / cov-inverse / cov-consumer cost; sec_epoch_t0 anchors the epoch
        // wall; sec_asm_ms_epoch is filled at the assembly wall below.  Inert (no
        // clock read) when the toggle is OFF.
        NetworkNormalEquations::SectionTimers sec_t0;
        std::chrono::steady_clock::time_point sec_epoch_t0;
        double sec_asm_ms_epoch = 0.0;
        if (sec_timers_on) {
            sec_t0 = est.sectionTimers();
            sec_epoch_t0 = std::chrono::steady_clock::now();
            sec_sub_ep0 = sec_sub;   // D0b: per-epoch deltas of the sub-block run totals
        }

        // P1 rotation of the unbounded service CSVs (body relocated to
        // NetworkRunTraces, INC-4; OFF => byte-identical no-op).
        traces.rotateServiceCsvs(nc, log);

        // RTB-6 health heartbeat (body relocated to NetworkRunTraces, INC-4;
        // window anchors + last_health_write_s live in ctx.counters).
        traces.writeHealthRow(se.time, nc, ctx.counters, t_loop_begin,
                              ura_on, raim_on, pl_on, ura_sigma0_ewma,
                              last_pivot, grade_metric_s);

        // FC-1 shadow mode: run the SAME tick body on this FULL epoch, immediately before
        // the full pipeline, so the trace carries a fast-vs-full delta for every epoch of
        // any run.  Bracketed (not hidden) so its cost is attributed in the D0b table
        // rather than silently inflating `other`; the result is traced and discarded, so
        // the full pipeline stays byte-identical (gated at every increment).
        if (fc_shadow) {
            subBegin(SB_FASTCLOCK);
            networkFastClockTick(ctx, config_, log, traces, setup, se, true);
            subEnd(SB_FASTCLOCK);
        }

        // L1/L2: the row-writer bodies live in NetworkRunTraces; these
        // thin forwarders keep every downstream call site textually unchanged.
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

        // Phase-leveling carry timing (Increment 1): continuous-GPS seconds of this
        // epoch (for the per-ambiguity inter-epoch gap) and whether the cold-start
        // warmup has elapsed.  Count EMITTED product epochs (not raw loop epochs) so
        // the carry waits for the clock/SH convergence horizon — locking an
        // unconverged ambiguity onto a contaminated cold-start value would propagate
        // it.  n_emitted here excludes the current epoch (not yet emitted).
        const double pl_now_s   = TimeUtils::continuousGpsSeconds(se.time);
        // T1d: when convergence-gated warmup is armed AND an SH field is active, warm the
        // carry only once the field has gone stationary (sh_converged_latch, set post-solve
        // below from the epoch-over-epoch coefficient RMS Δ + a₀₀ physical band) — otherwise
        // the fixed emitted-epoch count.  Falls back to the epoch count when there is no SH
        // field to converge.  OFF (default) ⇒ the exact epoch-count gate ⇒ byte-identical.
        const bool   carry_warm = pl_on && ((pl_warmup_on_conv && sh_model)
                                                ? sh_converged_latch
                                                : (static_cast<int>(n_emitted) >= pl_warmup));

        // This epoch's slip event feed for the DD invariant carry —
        // (app station index, ambiguity key) pairs the PASS-A slip detectors flagged.
        // Populated at the DETECTION site (the slip/NetworkSlipQc.cpp drains)
        // rather than read from pending-reseed at fold time, because PASS-B
        // MeasurementModel::compute re-seeds and CLEARS the pending-reseed flag before
        // buildCarryDdPriors runs (the flag is a one-shot the assembly consumes).  Also
        // recorded REGARDLESS of carry_warm (the detectors run cold, and the DD carry is
        // live from epoch 1) so the carry is slip-protected through the whole warmup —
        // not only after the SH field's convergence gate releases.  carry_invariant-only.
        std::set<std::pair<int, std::string>> carry_slipped_ambs;
        // Seed this epoch's slip set with the innovation gate's PREVIOUS-epoch
        // flags (a fine slip caught post-solve last epoch re-taps its arc fresh here, via the
        // same C3c membership-exclusion path the coarse detectors feed).  One-shot: consumed now.
        if (carry_innov_gate && !carry_innov_pending.empty()) {
            carry_slipped_ambs.insert(carry_innov_pending.begin(), carry_innov_pending.end());
            carry_innov_pending.clear();
        }
        // Card 4 D5 v4 (network.carry_invariant_innov_retap_only): drain the
        // CORROBORATED-FLAT subset of those flags in lockstep.  It is a strict subset of
        // what just went into the funnel — the entries stay in the funnel and every side
        // consumer is untouched — and it names the arcs whose raw MW/GF level was MEASURED
        // and did not step, i.e. the ones with no integer jump for a refloat to complete.
        // networkBuildCarryDdPriors spares exactly those from the A_old membership
        // exclusion, so the DD the gate re-tapped in-epoch keeps its history instead of
        // being reborn from a diffuse prior every epoch.  Cleared unconditionally so a
        // disarmed run can never inherit a stale set.
        ctx.carry.carry_innov_flat_now.clear();
        if (!ctx.carry.carry_innov_pending_flat.empty()) {
            ctx.carry.carry_innov_flat_now.swap(ctx.carry.carry_innov_pending_flat);
            ctx.carry.carry_innov_pending_flat.clear();
        }
        // Iono-2 v3k erase-policy split (SPEC_iono2_mechB_v3 addendum): the RAW-detector
        // portion of the funnel, captured at insertion — PASS-A fills THIS set and the
        // driver unions it into the funnel below, so every existing funnel consumer sees
        // the identical union while the shdev pre-pass can distinguish raw-confirmed
        // slips (erase) from D5-only re-taps (97 % have no raw observable step — they
        // are iono-tension events the deviation should absorb, not die on).
        std::set<std::pair<int, std::string>> carry_slipped_raw;
        // Iono-deweight RELEASE hygiene INC-1:
        // drain the PREVIOUS epoch's release-edge refloats.  The edge is detected
        // post-solve (SB_T1A, inside the gate's own slew) — by which point THIS set's
        // load-bearing consumer has already run and a bare pending-reseed mark would be
        // wiped by the leveling harvest's clearPendingReseed — so it is deferred here,
        // exactly as the D5 innovation gate defers its post-solve flags above.  Drained
        // into the RAW subset because a re-coupled parked slant IS a raw-observable-class
        // event: the union below carries it into the funnel for every existing consumer,
        // AND the raw provenance stops the Iono-2 v3k d5_keep policy from resurrecting
        // the very deviation level this response exists to discard.  The pending-reseed
        // mark is issued from HERE too (nothing between here and PASS B clears it),
        // which is what makes the ambiguity re-seed read the ALREADY-reseeded slant.
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
        // RAIM POOL RE-HOST: drain the PREVIOUS epoch's
        // post-fit RAIM flags.  Same deferral as the two drains above and for the same
        // reason — RAIM detects post-solve at SB_RAIM, by which point THIS funnel's
        // load-bearing consumer (buildCarryDdPriors, SB_DDBUILD) has already run.
        //
        // Drained into `carry_slipped_ambs` and deliberately NOT into `carry_slipped_raw`:
        // the raw subset means a RAW-OBSERVABLE step, which the Iono-2 v3k d5_keep erase
        // classifier and the WL-3 MW-row slip-skip both read.  A post-fit residual
        // consensus is estimation-side evidence — the same class as the D5 innovation
        // gate, which also drains into the funnel only.  Draining RAIM into raw would
        // silently change the Iono-2 deviation-erase policy.
        //
        // The pending-reseed mark is NOT re-issued here (unlike the dw-release drain,
        // whose slant rewrite must be visible to the ambiguity re-seed): the seam already
        // set it at detection time, and on this lineage it is bookkeeping either way.
        //
        // UNCONDITIONAL clear — the one-epoch lifetime bound (standing invariant #1) is
        // what keeps a stale (station index, key) pair from outliving its arc across a
        // datum change or a station drop.
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
        // Legacy: the pivot's allocated phase-bias keys ARE the shared set.
        // Prioritized: seeded empty here; the anchor election after the cross-
        // station observability prune rebuilds it as the anchored UNION over
        // present stations (each key backed by exactly one elected pin).
        std::set<std::string> shared_phase_keys;
        // Pivot-first head + PASS-A parallel tail + slip scans relocated to
        // ApplicationNetworkAssembly.cpp.  networkPassAHead_ models the
        // legacy pivot (skip bookkeeping verbatim inside; SkipEpoch replaces the
        // former `continue`s) and seeds the legacy shared set; the SB_SLIP wall
        // times the scan exactly as before.
        if (networkPassAHead_(se, ctx, setup, traces, modeled, present,
                              shared_phase_keys) == EpochAction::SkipEpoch)
            return LoopSignal::Next;

        subBegin(SB_SLIP);   // D0b block 1: PASS-A slip scans (pre-assembly)
        ++slip_qc.epoch_idx;   // monotonic PROCESSED-epoch index (Card G gap arithmetic)
        networkPassASlipScan_(se, ctx, setup, traces, dt, pl_now_s, carry_warm,
                              modeled, present, shared_phase_keys,
                              carry_slipped_raw, slip_qc);
        // Union preserves the funnel's exact previous contents (PASS-A consumers only
        // ever INSERT — NetworkSlipQc.cpp:155/:373 — so redirecting its writes through
        // carry_slipped_raw is behavior-identical for everything downstream).
        carry_slipped_ambs.insert(carry_slipped_raw.begin(), carry_slipped_raw.end());
        subEnd(SB_SLIP);       // D0b: close block 1 (slip scans)
        subBegin(SB_PREASM);   // D0b: pre-assembly measurement build (post-slip → assembly wall)

        // RAW slip observables + cross-station observability pre-filter relocated
        // to ApplicationNetworkAssembly.cpp: the writer-before-prune
        // ordering contract is structural inside networkAssemblyPrefilter_.
        int n_dropped_sats = 0;
        networkAssemblyPrefilter_(se, setup, traces, slip_qc,
                                  modeled, present, n_dropped_sats);

        // FLICKER / GAP SIGNAL INTEGRITY.  Classifies every present (station,
        // satellite) line of sight against its own band history, publishes the
        // per-station phase sigma-scale map PASS B's fold reads, and refloats a
        // line of sight whose band flickered back with no producer-registered gap.
        //
        // The slot is one point wide.  It must run AFTER the PASS-A slip drains
        // and their union, since it INSERTS into carry_slipped_ambs / _raw and
        // every consumer must see those insertions; AFTER
        // networkAssemblyPrefilter_, so the census counts only the LOS set that
        // actually folds rows and a satellite the two-station observability gate
        // erased is not reported as an event; and BEFORE the phase-datum census /
        // election, because a WITHHOLD prunes phase-bias states the election might
        // otherwise seat a datum pin on.
        //
        // Deliberately NOT the PASS-B serial pre-pass, which runs after the
        // election.  The epoch unit is slip_qc.epoch_idx — the same counter the gap
        // instrument uses — so their re-entry totals are directly comparable.
        // No-op unless network.signal_integrity.census.
        networkSignalIntegrity(ctx, config_, log, traces, se.time, slip_qc.epoch_idx,
                               stations, present, modeled,
                               carry_slipped_ambs, carry_slipped_raw,
                               carry_invariant, carry_warm);
        // PRODUCT-REFERENCED MEASUREMENT ADMISSION — the first layer of the
        // defense in depth (this screen, then tie admission, the prefit fuse, and
        // the SH health gate).  It is the absolute test for a channel whose CODE
        // and PHASE are displaced TOGETHER, the class every differential check
        // passes by construction (a station has been seen +198.2 km and +99.9 km
        // off SP3+CLK).  The statistic is `observed - modeled` on code alone,
        // centred on the station median; `modeled` is the STATE-FREE deterministic
        // model, which is what lets the screen fire at a cell's BIRTH, where a
        // prefit innovation structurally cannot.
        //
        // The slot is one point wide.  AFTER the PASS-A slip drains and their
        // union, since it INSERTS into carry_slipped_ambs / _raw for its onset,
        // heal and station-guard refloats.  AFTER networkAssemblyPrefilter_, so a
        // satellite the observability gate erased cannot enter the median, raise a
        // verdict, or count as admitted.  AFTER networkSignalIntegrity, because
        // `modeled` is rebuilt every epoch and screening first would make the
        // flicker classifier read each withhold as a band LOSS and each heal as a
        // FLICKER-REGAIN — manufacturing the very events that census exists to
        // measure (this seam only reads `observed` / `modeled`, which the
        // classifier does not write, so the later slot is free).  And BEFORE the
        // phase-datum election and networkAssemblyRegisterColumns_, since a
        // withhold prunes orphaned phase-bias / code-bias / clock states the
        // election could otherwise pin, and would leave the ≥ 2-station code-bias
        // coverage gate phantom-supported by a station feeding it nothing.
        // No-op unless network.adm_screen or network.adm_trace.
        networkMeasurementAdmission(ctx, config_, log, traces, se.time,
                                    slip_qc.epoch_idx, stations, present, modeled,
                                    carry_slipped_ambs, carry_slipped_raw,
                                    carry_invariant, carry_warm);
        // The final shared phase-bias set + which station anchors (gauge-pins) each
        // (sat,signal).  Two realizations of the SAME minimum constraint (exactly
        // one ambiguity pin per phase-bias freedom):
        //   Legacy (network.phase_datum_prioritized OFF): the single pivot's pruned
        //     phase-bias states ARE the shared set (each backed by a pivot-receiver
        //     pin).  Byte-identical.
        //   Prioritized: elect, per (sat,signal), the highest-coverage receiver that
        //     tracks it with a live ambiguity as its anchor — decoupled from the
        //     clock pivot, so a satellite low/unseen at the clock pivot keeps a clean
        //     gauge at another station instead of jumping.
        if (!phase_datum_prioritized) {
            shared_phase_keys.clear();
            for (const auto& kv : stations.front().sm->getSatPhaseBiasStates())
                shared_phase_keys.insert(kv.first);
        } else {
            const bool   sticky  = config_.networkPhaseDatumSticky();

            // This epoch's candidate census + the network.phase_datum_min_stations
            // coverage gate.  Relocated to filter/NetworkPhaseDatum at INC-8b:
            // built ONCE here and shared by whichever realization the dispatch
            // below picks.  The builder also PRUNES the phase-bias states of
            // gated keys, which all three elections depend on having already
            // happened, and it is the sole writer of the two run-end gate
            // counters (computing the census twice would double-count them).
            PhaseDatumCandidates cnd;
            AnchorHygiene hyg;   // eq34 reattach hygiene verdicts (inactive when off)
            networkBuildPhaseDatumCandidates(config_, stations, present, modeled,
                                             cnd, n_gated_epochs, n_gated_key_epochs);

            // eq34 REATTACH HYGIENE.  Ages this
            // epoch's anchorable arcs, decides which keys may be RE-HOSTED and on
            // what, runs the INC-2 bridge lifecycle, and (when a key has no converged
            // host and no bridge) removes it from the candidate union exactly as the
            // coverage gate removes a thin key.  MUST sit here: after the census (it
            // mutates cnd.union_keys and needs the coverage prune done) and after the
            // PASS-A slip drains (carry_slipped_ambs must be complete, or a
            // just-refloated arc reads as mature — invariant #1), but before any
            // election reads the union.  No-op unless a hygiene knob is armed.
            networkAnchorHygiene(ctx, config_, log, traces, se.time, stations, present,
                                 carry_slipped_ambs, cnd, hyg);

            if (phase_datum_ensemble) {
                networkElectEnsembleDatum(stations, cnd, this_epoch_anchor,
                                          shared_phase_keys);
            } else if (phase_datum_eq34) {
                // Card 3 chain datum.  MUTATES the carries pre-solve and fills
                // ctx.carry.eq34_reref, so it must stay ahead of the D3c RPB
                // allocation below (it is the sole writer of shared_phase_keys on
                // this path, and D3c derives its need_rpb set from that) and hence
                // ahead of est.beginEpoch / PASS B.  Full ordering contract in
                // filter/NetworkChainDatum.hpp.
                networkElectChainDatum(ctx, config_, log_manager_, log, stations,
                                       present, cnd, hyg, carry_slipped_ambs,
                                       shared_phase_keys);
            } else {
                // TWO-EDGE CONTINUOUS HANDOVER
                // The LEDGER is
                // passed only when armed (it decides the pin target — solve-side),
                // but the event sink is UNCONDITIONAL as of R0: the cold
                // re-pin was an unlogged both-edge event in the default posture,
                // and §5.07.4 defect 5 names exactly that silence.  The rows are
                // trace-class (network_events.csv under trace_events); the solve is
                // untouched, and `tally.reattaches` still counts either way.
                std::vector<AnchorHandoverEvent> handovers;
                const bool hv_on = ctx.knobs.pd_handover_continuous;
                // QUEUE item 1(d): the re-referenced (sat,signal) set — populated
                // here when armed, consumed by the D2/C1 carry fold (marginalize)
                // and the erase directly below, and CLEARED BY THE CARRY STASH,
                // not by this election (review finding, 2026-08-14): a withheld
                // epoch skips the stash, so its prev_carry still holds the
                // retired-frame values while the incumbent swap has already
                // persisted — clearing here would let that stale carry fold
                // unmarked one epoch later.  Stash-side clearing makes the mark
                // survive exactly until a solve actually commits the new frame.
                auto& pa_reref = ctx.carry.phase_anchor_reref;
                const std::size_t pa_before = pa_reref.size();
                // ANCHOR-SLIP HANDOVER (network.cycle_slip.anchor_slip_handover,
                // docs/SPEC_event_harness.md §12.7).  MUST sit exactly here: after
                // every PASS-A slip drain and the signal-integrity / admission
                // refloats (so both freed sets are complete) and immediately before
                // the election, which is the only consumer.  It stages the same
                // one-epoch candidacy ban the harness injector uses, so a family
                // whose ANCHOR arc was just freed is handed to a successor at the
                // successor's own carried value_abs IN THIS EPOCH — before the freed
                // arc's level is re-drawn from one epoch of code and the whole
                // family's gauge follows it.  Knob OFF: not called, nothing staged,
                // and the `inject` argument below resolves exactly as before.
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
                // COLD ANCHOR-FALLBACK (SPEC_event_harness §15): the candidacy
                // pass's read-only view.  Built here and not inside the election
                // because every member is a run-scope carry object — the freed sets
                // this epoch's drains just filled, and the leveling carry the age
                // bar reads `birth_ep` from.  Default-constructed when the knob is
                // off, which is the pre-card election exactly.
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
                // RF-2 (docs/refound/RF-1_steps345_mechanism_2026-09-15.md): the phase
                // datum as a HELD S-BASIS.  Assembled here because this scope owns the
                // ledger, the carry, the flag map and this epoch's elevations; `on` is
                // false on every unarmed run and the pointer below is then null.
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
                // Card EV7 §6 — the coverage gate's DROP EDGE.  Passed on EVERY
                // run, armed or not: requirement F2 asks the OFF run to report the
                // size of the class, and detection is pure observation over the
                // census and the pre-swap incumbent map (the erase below is the
                // only thing the knob gates).
                PhaseGateDrop gate_drop;
                gate_drop.on = config_.networkPhaseDatumGateDropErase();
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
                                         // nullptr (the normal path).  TWO sources:
                                         // the EVENT HARNESS schedule, and the
                                         // production anchor-slip handover staged
                                         // just above — either one alone is enough
                                         // to make the directive set live.
                                         (ctx.inject.armed
                                          && !ctx.inject.anchor_now.empty())
                                         || !ctx.inject.anchor_now_slip.empty()
                                             ? &ctx.inject : nullptr,
                                         &cand_usable,
                                         // RF-2: the HELD S-BASIS.  Null unless armed,
                                         // so every branch it gates is unreachable and
                                         // the election is byte-identical.
                                         held_basis.on ? &held_basis : nullptr,
                                         // Card EV7 §6: always non-null (see above).
                                         &gate_drop);
                // Card EV7 §6 (F2): BOTH EDGES of the gate are events now.  One row
                // per drop edge with the identities on either side — what left, who
                // was anchoring it, who is left carrying it — and `handled` saying
                // whether the response actually ran, so an OFF row and an armed row
                // are separable offline.  Trace-class; the solve never reads these.
                for (const auto& gd : gate_drop.events) {
                    ++ctx.counters.n_pd_gate_drop_edges;
                    if (gd.handled) ++ctx.counters.n_pd_gate_drop_handled;
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
                // The families this card owns, for the erase-cause split below.  A
                // gated key is erased from union_keys before the election's loop, so
                // it can never also be a handover / first-sighting mark this epoch —
                // the two populations are disjoint by construction.
                std::set<std::pair<std::string, std::string>> gate_drop_fams;
                for (const auto& gd : gate_drop.events)
                    if (gd.handled) gate_drop_fams.insert({gd.sat, gd.signal});
                // RF-2: fold the election's write-back into the run counters.
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
                // §14.5 SPLIT-REFERENCED SATELLITE CENSUS.  ALWAYS ON and
                // knob-independent (invariant #4): how many satellites now have
                // their per-frequency phase-bias families anchored at DIFFERENT
                // stations, whose arc levels walk independently.  Read HERE —
                // the election just swapped this epoch's anchors into
                // anchor_incumbent, so the map is current.  Pure, O(map), no
                // solve contact: the counters below are the only output, so an
                // unarmed run measures the same thing and the scope's effect on
                // datum fragmentation is comparable across arms.
                {
                    const SplitRefCount sr =
                        networkSplitReferencedSats(anchor_incumbent);
                    ctx.counters.n_cb_split_sats_sum += sr.split;
                    ctx.counters.n_cb_split_multi_sum += sr.multi_anchored;
                    ++ctx.counters.n_cb_split_epochs;
                    if (sr.split > ctx.counters.n_cb_split_sats_max)
                        ctx.counters.n_cb_split_sats_max = sr.split;
                }
                // EVENT HARNESS readback: the election filled the directive in with
                // the stations it retired and elected.  One row per directive, and
                // the firing ledger is closed HERE — an anchor_handover that named a
                // satellite no family used never fires, and run-end says so.
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
                // ANCHOR-SLIP HANDOVER readback (§12.7).  BOTH EDGES with identities
                // (invariant #7): the retired incumbent is the row's station, the
                // successor and the branch its detail.  A cold OUTCOME is a gauge
                // re-realization and says so; a directive that found no successor at
                // all left the family unpinned for the epoch and says that too.
                for (const auto& d : ctx.inject.anchor_now_slip) {
                    if (d.n_families == 0) {
                        // The family was not in this epoch's union (coverage-gated,
                        // bridged, or pruned).  No identities to report — counted so
                        // run-end can separate it from a handover that happened.
                        ++ctx.counters.n_anc_slip_inert;
                        continue;
                    }
                    const char* branch = d.n_no_successor > 0 ? "none"
                                       : (d.n_cold_fallback > 0 ? "cold" : "continuous");
                    if (d.n_no_successor > 0) {
                        ++ctx.counters.n_anc_slip_no_successor;   // NOT re-hosted
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
                                          // COLD ANCHOR-FALLBACK (§15): what the
                                          // candidacy pass walked past to get here,
                                          // so the cold-handover census re-runs from
                                          // the trace.  Absent with the knob off.
                                          ctx.knobs.pd_candidacy_usable
                                              ? fmt::format(";cand_skipped={};cand_first_unusable={}",
                                                            d.n_cand_skipped,
                                                            d.cand_first_unusable.empty()
                                                                ? std::string("-")
                                                                : d.cand_first_unusable)
                                              : std::string{}));
                }
                // BOTH EDGES in one row (invariant #7): who released the gauge, who
                // took it, and whether the frame was preserved.  A handover logged
                // only on its incoming half is exactly the silent re-referencing the
                // FT63 topology ledger was built to recover.
                for (const auto& h : handovers)
                    traces.emitEvent(se.time, "PBGAUGE",
                                     h.continuous ? "handover_continuous"
                                                  : "handover_cold",
                                     h.amb, h.to, h.target_m, std::nullopt,
                                     h.continuous ? "s_transform" : "re_realize",
                                     "from=" + (h.from.empty() ? std::string("(none)")
                                                               : h.from)
                                     // Ratio ceiling armed: the successor's ledger var and
                                     // first-stash var ride the row (the prerequisite
                                     // SPEC_amb_iono §7 item 15 named — trace the successor's
                                     // conditional var per handover, not only the target).
                                     + (ctx.knobs.pd_handover_max_var_ratio > 0.0
                                            ? fmt::format(";var={:.3e};var_first={:.3e}",
                                                          h.var, h.var_first)
                                            : std::string{})
                                     // COLD ANCHOR-FALLBACK (§15): the per-handover
                                     // candidacy record — how many better-ranked
                                     // candidates this successor was chosen OVER,
                                     // and the first one refused.  Absent (so every
                                     // pre-card row is byte-identical) with the knob
                                     // off.
                                     + (ctx.knobs.pd_candidacy_usable
                                            ? fmt::format(";cand_skipped={};cand_first_unusable={}",
                                                          h.cand_skipped,
                                                          h.cand_first_unusable.empty()
                                                              ? std::string("-")
                                                              : h.cand_first_unusable)
                                            : std::string{}));
                // 1(d) local-carry hygiene: erase the marked families'
                // phase-leveling carries at EVERY station, before PASS B consumes
                // the carry (the eq34 D4 placement).  Counted for the run-end
                // summary; the counter takes only THIS election's new marks (a
                // set persisting across a withheld epoch must not double-count).
                // Empty set (knob off, or a quiet election) is a no-op, and the
                // re-erase over a persisting set is a no-op too.
                // ---- RF-2b item 2: EVERY HANDOVER DECLARED ------------------------
                // ⚠ RF-2 put this loop INSIDE `if (!pa_reref.empty())` — an UNRELATED
                // predicate (whether the anchor-reref marking fired this epoch) — so on
                // the sim only 9 of 100 handovers ever reached it.  Hoisted here it sees
                // every handover the election produced, which is what the card asked for.
                //
                // …and the RULE is now the principled one: a handover is declared to the
                // consumer iff its basis VALUE actually changed, i.e. iff the emitted
                // phase bias steps.  A CONTINUOUS handover inherits the outgoing frame by
                // construction (Δ = 0) and marking it would force a consumer reset for a
                // wire that did not move — which is exactly what took RF-2's PB marks to
                // 5.3× the baseline against a 1.5× bar.  A COLD one pins the successor at
                // 0 against an outgoing basis of `prev_value`, so it steps by exactly
                // that, and the consumer MUST be told.  EVERY handover is TRACED; only a
                // real step is MARKED, and the row carries `marked=` so the two
                // populations are separable offline.
                //
                // The mark rides `ura_retro_sats`, the retro-bump queue NetworkSolPrep
                // drains into markSatDiscontinuity with mark-==-land semantics: a
                // satellite withheld this epoch stays PENDING and is marked at its next
                // ACTUAL emission, which is the epoch the consumer needs the reset.
                if (ctx.knobs.pd_held_basis)
                    for (const auto& h : handovers) {
                        const auto pk = StateKeys::parseAmbiguityKey(h.amb);
                        if (!pk) continue;
                        const auto be = ctx.carry.phase_basis.find(h.amb);
                        const double dlt =
                            (be != ctx.carry.phase_basis.end())
                                ? (be->second.value - be->second.prev_value) : 0.0;
                        // RF-2d (network.phase_datum_held_mark_cold): |value − prev_value|
                        // is the BETWEEN-STATION level difference, not the wire step.  A
                        // continuous pin is the successor's level in the outgoing frame, so
                        // the family does not step while the difference is non-zero; a cold
                        // pin at 0 steps the family by the successor's level, and out of a
                        // cold tenure the difference reads 0 − 0.  RF-3-read measured the
                        // predicate inverted on both beds (field: 144/145 continuous marked,
                        // 10/12 cold unmarked), so the armed rule is the frame's own: a
                        // handover is declared iff it is cold.
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
                    // RF-1 step 5 (§3.6): the DECLARED ANCHOR S-TRANSFORM.  A frame step
                    // is a KNOWN, exact basis change, so express the carry in the new
                    // basis instead of throwing it away.  Families the transform takes
                    // ownership of are REMOVED from the erase set below — transformed or
                    // erased, never both — and a COLD handover (no Δ) keeps its erase,
                    // which is the honest limit of this step and RF-2's subject.
                    // (RF-2b moved the handover DECLARATION out of this block — see the
                    //  hoisted loop above `if (!pa_reref.empty())`.)
                    std::set<std::pair<std::string, std::string>> pa_sts_handled;
                    networkAmbFloatDeclaredHandover(ctx, log, traces, stations, handovers,
                                                    se.time.week, se.time.tow,
                                                    pa_sts_handled);
                    for (const auto& fam : pa_sts_handled) pa_reref.erase(fam);
                    // RF-2 item 2: with every handover DECLARED and the basis HELD, the
                    // network-wide erase is retired — the carries are not stale, they are
                    // expressed against a datum whose move the consumer has been told
                    // about.  The erase counter must therefore read 0 on an armed arm;
                    // it is left in the code (not deleted) so the OFF path and every
                    // other datum realization keep it verbatim.
                    //
                    // ⚠ RF-2d (network.phase_datum_held_cold_release): that premise holds
                    // for CONTINUOUS handovers only, and R2 already keeps those out of this
                    // set — what reaches here is the COLD and first-sighting families,
                    // whose frame DID step.  Clearing them leaves their retired-frame
                    // carries folding against the re-realized design, and the satellite
                    // clock absorbs the step (RF-3-read, sim G29: 65 m).  Armed, the set is
                    // kept and released exactly as on the level vehicle.
                    if (ctx.knobs.pd_held_basis && !ctx.knobs.pd_held_cold_release)
                        pa_reref.clear();
                }
                if (!pa_reref.empty()) {
                    // RF-1 §7.4 D3: on an armed trace, take the entries before they
                    // are destroyed — this scope owns the station names and the epoch
                    // stamp, which the map operation itself has no business knowing.
                    std::vector<std::pair<std::tuple<int, std::string, std::string>,
                                          LocalCarry>> pa_reref_erased;
                    // Card EV7 §6: the capture is ALSO taken when a drop edge was
                    // handled, so the card's arc counter does not silently read 0 on
                    // a run that leaves network.trace_amb_state off (the row writer
                    // itself stays knob-gated and no-ops there).
                    const int n_erased = networkApplyPhaseAnchorCarryReref(
                            pa_reref, ctx.carry.local_carry,
                            (ctx.knobs.trace_amb_state || !gate_drop_fams.empty())
                                ? &pa_reref_erased : nullptr);
                    for (const auto& [k, lc] : pa_reref_erased) {
                        const int sidx = std::get<0>(k);
                        // Card EV7 §6 (F2): the two populations that share this one
                        // erase path are tagged apart in network_amb_state.csv — a
                        // handover / first-sighting re-reference (`anchor_reref`,
                        // unchanged) and a coverage-gate drop edge (`gate_drop`).
                        // Disjoint by construction, so the test is a lookup, not a
                        // precedence rule.
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
                    ctx.counters.n_cr_phase_datum_reref += n_erased;   // retirement census: FRAME step
                    log->debug("Network[1(d)]: {} marked (sat,signal) family(ies) "
                               "marginalized ({} new this election); {} phase-"
                               "leveling local carry(ies) erased — refloat next "
                               "epoch.",
                               pa_reref.size(), pa_reref.size() - pa_before,
                               n_erased);
                }
            }  // end anchor election (!ensemble && !eq34)
        }

        // Card 3 (eq34) RPB allocation (rule D3c): a present station gets the
        // per-(receiver,system,signal) split state δ_{r,σ} iff it carries an
        // UNGATED shared phase-bias column on σ (a phase-bias state whose key is in
        // shared_phase_keys) — i.e. iff it is a vertex of the D2 graph.  A station
        // whose σ-keys are all gated allocates no RPB and keeps legacy lumping.
        // Re-derived fresh each epoch (allocate what's needed, prune the rest); runs
        // BEFORE est.reset()/PASS B so buildJacobianRow's +1 RPB coefficient sees the
        // column and splitStation routes it into the local block A_r.  A spanning
        // tree touches every vertex, so every RPB-carrying station carries ≥1
        // tree-edge pin ⇒ its N_rr stays PD.  Inert (no allocation) when eq34 off.
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

        // Shared-column registration + counts + estimability descriptors relocated
        // to ApplicationNetworkAssembly.cpp.  Outputs are fresh per epoch;
        // the empty-system skip keeps its exact bookkeeping inside (SkipEpoch
        // return), and the Next-return below bypasses subEnd(SB_PREASM) exactly as
        // the original site did.  est.beginEpoch() runs inside on Proceed.
        // --- LV-6 level-authority census --
        // Serial, after PASS A's CS-GF advance, deliberately BEFORE the
        // RegisterColumns SkipEpoch below (an empty-system epoch still gets its
        // census row) and upstream of every withhold decision (the census
        // measures through withhold eras).  Measure-only; no-op unless armed.
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

        // Capture the shared-border folds (code-bias gauge constraint + all carried /
        // anchor priors) applied to THIS epoch's arrowhead, so the within-epoch
        // adaptation (drop a culprit → re-solve) can replay the identical shared system
        // over a fresh beginEpoch().  addSharedPrior is additive, so the three prior
        // sets concatenate into one replayable vector.  Populated only when the
        // adaptation is armed; otherwise the appends are cheap no-ops.
        // --- SD EWL Route-B integer decision (increment 2a, filter/NetworkSdEwlAr) ----
        // Placed HERE deliberately: after networkAssemblyPrefilter_ (satellites seen by
        // <2 stations are erased there, so an integer decided for one that then vanishes
        // is wasted), after the phase-datum election and the eq34 RPB allocation (both
        // mutate StateManager layout), after networkAssemblyRegisterColumns_ (so the
        // epoch is committed — that site can still SkipEpoch), and BEFORE networkPassB_,
        // whose PARALLEL produce loop reads the published map read-only.  Inside the
        // existing SB_PREASM bracket rather than a new SubBlock, because adding a timer
        // slot would change the section-timing artifact for every run, armed or not.
        // Default OFF => clears the map and returns => byte-identical.
        networkSdEwlAr(ctx, config_, log, traces, setup, se, pl_now_s, modeled, present);

        // --- Card LV-5 ----------------
        // Reduce the rx-CB de-mean μ HERE — after PASS A has settled which stations
        // are present, before PASS B's parallel produce folds anything.  Both terms
        // of every carry innovation (the stashed absolute posterior, the frozen
        // x_lin) are already known at this point, so the LV-2 mean can be applied to
        // the SAME epoch it describes instead of arriving one epoch late.  That lag
        // is a feedback delay whose loop gain scales with how much carry information
        // folds: harmless while the defective frame screen withheld two thirds of
        // the priors, and a ~3-epoch limit cycle in the level once LV-4 admits them
        // all.  Serial, one writer, and PASS B only reads the map — the same race
        // contract as the rest of the carry cycle.  No-op unless armed.
        networkRxCbDemeanPrepass(ctx, stations, present, pl_now_s);

        const bool within_epoch_armed = config_.networkRaimWithinEpoch();
        auto& cap_H = ws.cap_H;
        auto& cap_z = ws.cap_z;
        auto& cap_constraint_weight = ws.cap_constraint_weight;
        auto& cap_has_constraint = ws.cap_has_constraint;
        auto& cap_priors = ws.cap_priors;
        auto& cap_dense_keys = ws.cap_dense_keys;
        auto& cap_dense_cov = ws.cap_dense_cov;

        // --- SIM-ONLY ORACLE AMBIGUITY PIN (filter/NetworkOraclePin) ------------
        // Placed HERE deliberately: AFTER networkElectPhaseAnchors (the fit reads
        // THIS epoch's anchors and their enforced pin targets — a handover moves a
        // family's frame term and the pin must follow it) and AFTER
        // networkAssemblyRegisterColumns_ (so the epoch is committed; that site can
        // still SkipEpoch), and BEFORE networkPassB_, whose PARALLEL produce loop
        // reads the published table read-only.  Inside the existing SB_PREASM
        // bracket rather than a new SubBlock, because adding a timer slot would
        // change the section-timing artifact for every run, armed or not (the
        // networkSdEwlAr precedent above).  No `oracle_pin` event => the pointer is
        // null => not one branch is reachable => byte-identical.
        if (ctx.carry.oracle_pin)
            networkOraclePinEpoch(ctx, config_, stations, se.time, log, traces);

        // RF-1 step 4 (docs/refound/RF-1_float_ambiguities_design.md §3.4): the BOUNDED
        // DROP, applied SERIALLY here — after PASS A's slip scans have latched
        // relevel.free_ambs and before the parallel PASS-B inject reads the carry, so by
        // the time a station's produce sees an arc the drop has already happened and the
        // inject folds it at birth strength instead of skipping it.  A no-op unless the
        // vehicle is float_kf.  `modeled` carries this epoch's LOS elevations, which the
        // birth floor is mapped with; the map is built only when the floor is armed.
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
        subEnd(SB_PREASM);   // D0b: close pre-assembly (the assembly wall below is timed separately)

        // --- PASS B relocated to ApplicationNetworkAssembly.cpp:
        // networkPassB_ owns the D0 assembly wall (raw chrono pair — moves
        // whole, sinks by reference), the T5 sh-dev aging, the Card-1d reinit
        // history, the parallel produce loop (pragma verbatim, incl. the
        // coupling-injection cluster — INC-7b carves that later), and the
        // serial consume; the failed/empty-epoch skip keeps its exact
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
        subBegin(SB_GAUGEBUILD); // D0b: buildEnsembleGauges + carry-driver lambda defs (pre-DD-build)

        // Fix A (network.phase_datum_ensemble): build the anchor-free sum-to-zero
        // phase-bias gauges from every tracking station's ambiguity column and hand
        // them to the estimator, which folds Σ_r zˢ_{r,σ}=0 through the kernel
        // (Schur-projected) at the gate/solve reduce.  One gauge per (sat,signal)
        // that has a registered shared phase-bias column (thin/gated keys have no
        // column → their ambiguity floats).  Re-invoked on the within-epoch RAIM
        // re-solve (station set / indices may change).  Empty / no-op when OFF.
        auto buildEnsembleGauges = [&]() {
            networkBuildEnsembleGauges(est, stations, est_index_of, asm_res,
                                       shared_phase_keys, prev_carry,
                                       phase_datum_ensemble, phase_hold_on);
        };
        buildEnsembleGauges();

        // Within-epoch RAIM culprit set forwarded into the DD-carry
        // re-solve rebuild (spec §3 culprit-withhold).  The pointers alias the RAIM
        // block's drop sets while it runs, else null.  The PASS-A slip feed
        // (carry_slipped_ambs, above) is the primary source; both are carry_invariant-only
        // ⇒ OFF byte-identical (buildCarryDdPriors early-returns when the toggle is off).
        const std::set<std::string>*                carry_we_drop_sats = nullptr;
        const std::map<int, std::set<std::string>>* carry_we_drop_arcs = nullptr;

        // Reverse map (group key → chord ArcKey → ambiguity state key) so a
        // post-solve innovation flag (group, chord) maps back to its (station, ambiguity) for
        // the next epoch's slip funnel.  Rebuilt with the blocks each buildCarryDdPriors call
        // (the last call before the solve is authoritative).  Empty unless carry_innov_gate.
        std::map<int, std::map<ArcKey, std::string>> carry_arc_amb_key;
        // The pair groups' BAND-2 ambiguity keys, parallel to carry_arc_amb_key
        // (band-1).  A D5 flag on a pair chord feeds BOTH bands into the slip funnel
        // (§3.4 union refloat).  Empty unless lane_carry + carry_innov_gate.
        std::map<int, std::map<ArcKey, std::string>> carry_arc_amb_key2;

        // --- Arc-resumption bridge classification.  INSTRUMENTATION ONLY —
        //     nothing below changes a decision.  It asks: of the arcs that came
        //     back after a tracking break whose RAW level shifted, how many
        //     re-entered carrying state that bridged the break (the poison case)
        //     versus as a fresh float (benign)?
        //
        //     Placement is load-bearing: AFTER the slip drains, so
        //     carry_slipped_ambs — the only thing that forces a chord to re-tap —
        //     is complete for this epoch; and BEFORE buildCarryDdPriors re-keys the
        //     carry, so carriedChordValue / carry_sat_vertex / carry_group_id still
        //     describe the state the arc is RETURNING TO.  carry_group_id is read
        //     with find(), never operator[]: minting a group id here would corrupt
        //     the driver's stable keying.
        //
        //     The unit is the (station, satellite, signal) ARC, not the resumption.
        //     Chords are per-arc, so a dual-band satellite returning once
        //     contributes two chords that can bridge independently.
        //     The classification is armed by the instrument OR by the RESPONSE
        //     (network.cycle_slip.gap_reentry_relevel): with only the response
        //     armed the ledger is exactly the before/after measurement of what
        //     the response did, and leaving it gated on the diagnostic would let
        //     a response-only arm report its own effect as a structural zero.
        if ((nc.slip_qc.gap_instrument || nc.slip_qc.gap_reentry)
            && !slip_qc.epoch.resumptions.empty()) {
            // Does this carry_group_id key's signal component cover `sig`?  Under
            // the WL-3 lane shape a paired arc lives in a group keyed
            // "<sigA>+<sigB>", not in its per-band group — a per-band-only lookup
            // would report every paired arc as un-bridged (a silent false NEGATIVE
            // on the exact question this card exists to answer).
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
                    // "Marked slipped" spans BOTH vehicles.  carry_slipped_ambs
                    // is the Card-4 C3c funnel — the only thing that forces a DD
                    // CHORD to re-tap — but on the LEVELING vehicle the thing
                    // that stops the stale value folding is the free-epoch latch
                    // (`relevel.free_ambs`, read by the PASS-B inject site).  A
                    // classification that read only the funnel reported a freed
                    // leveling arc as `shifted_bridged`, i.e. it could not see
                    // its own response — which would have made the §G3 response
                    // card ungradable on the exact lineage it was built for.
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
                    // (b) Card-4 invariant DD carry: does the driver still hold a
                    //     carried chord for this arc from BEFORE the break?
                    //     carry_sat_vertex ALREADY stores the kSatBase-offset
                    //     vertex id, so it is used verbatim — re-adding the base
                    //     would look up a chord that cannot exist and report
                    //     "never bridged" for everything.
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
                    // H2: the arc ledger mirrors the RESUMPTION three-way split
                    // (shifted / measured-and-unshifted / unmeasurable) and every
                    // cell is counted, so it closes as bridged+fresh on each row
                    // instead of leaving a reader to infer the missing one.
                    // Unmeasurable is kept SEPARATE from clean deliberately: an
                    // arc that bridged a break whose level nobody could check is
                    // not benign, it is UNKNOWN, and G3's "shifted-and-bridged ≈ 0
                    // ⇒ defer" ruling is only as strong as that number is small.
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
                    // The per-arc event carries the SAME three-way verdict as the
                    // counters above — an unmeasurable resumption is tagged
                    // `unmeas_*`, never `clean_*`, and its ΔGF is written BLANK
                    // exactly as its ΔMW already was.  A fabricated "dGF=0.0000"
                    // on an arc with no pre-gap level reads as "the level did not
                    // move", which is the absence-of-evidence claim this whole
                    // instrument exists to avoid making.
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

        // Build this epoch's DD-prior blocks from the eq34 election
        // tree and fold the carried invariant DDs into the producer solve.  Each arc
        // (receiver, satellite) ambiguity on a (const,signal) is resolved to its
        // estimator (station, local ambiguity column); pinned_amb_targets marks the
        // spanning-tree edges (the gauge), the rest are chords (the carried DDs).
        // GLONASS (FDMA) excluded.  Vertex/group ids are stable across epochs (sat
        // label anchored; receiver = app index).  Re-invoked on the RAIM re-solve
        // (est.beginEpoch there clears the priors + rebuilds est_index_of), mirroring
        // buildEnsembleGauges.  No-op / OFF byte-identical unless carry_invariant.
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
        subEnd(SB_GAUGEBUILD);   // D0b: close gauge build + carry-driver setup
        subBegin(SB_DDBUILD);    // D0b: carry_driver.beginEpoch + buildCarryDdPriors (W·Pᶻ·Wᵀ)
        if (carry_invariant) {
            // The z-store predict (q_z·Δt on the carried DD covariance) runs ONCE per
            // epoch; the build/fold may re-run on the RAIM re-solve.  D5 v2: the epoch time
            // (σ̂₀² reporting bucket) + clean flag (always clean here — under carry_invariant
            // the eq34 datum re-realizes freely, so there is no re-level storm to gate; the
            // robust geo-mean shrugs off the content-hr-4 event tail on its own).
            const double dt = std::isfinite(carry_prev_tow) ? (se.time.tow - carry_prev_tow) : 0.0;
            carry_driver.beginEpoch(dt > 0.0 ? dt : 0.0, se.time.tow, /*clean=*/true);
        }
        { CarryEigenScope es(carry_par, n_threads); buildCarryDdPriors(); }  // parallel_carry: thread W·Pᶻ·Wᵀ
        subEnd(SB_DDBUILD);      // D0b: close buildCarryDdPriors (the Card-4 DD-cov projection)
        subBegin(SB_SOLVEGLUE);  // D0b: pre-solve prep + clock predict + solve + within-epoch re-solve
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
        subEnd(SB_SOLVEGLUE);   // D0b: close solve glue (net removed the internal solve/reduce)
        subBegin(SB_INSTALL);   // D0b: carry_driver.installPosterior (DD posterior mean/cov readback)
        // Install the DD posterior (mean + covariance) back into the
        // carry from the FINAL accepted x_s + datum — the invariant DDs the next epoch
        // projects forward.  POST-ACCEPTANCE (after the observability + σ̂0 withhold
        // gates' skip-paths above), so a withheld/contaminated epoch never poisons the
        // carry; and OUTSIDE the within-epoch RAIM block, so it runs every accepted
        // epoch (not only RAIM-armed ones).  A failure resets the carry (safe cold
        // start).  No-op when OFF.
        if (carry_invariant) {
            networkInstallCarry(ctx, config_, log, traces, carry_driver, est, x_s, plan, se, stations,
                                slip_qc, carry_arc_amb_key, carry_arc_amb_key2, log_dir_, carry_par,
                                n_threads, seam_threads, carry_innov_gate, mw_wl_on, lane_on, ewl_on, uwl_on,
                                wl_inject_on, lane_fix_on, mw_d5_dump, mw_lane_trace, mw_tagsets,
                                mw_wl_f6_dump, ewl_lanes, wl_inject_epoch, lane_trace_epoch, mw_wl_run,
                                mw_wl_epoch, wl_run, wl_epoch, wl_cons_run, wl_cons_epoch, mw_wl_f6);
        }
        subEnd(SB_INSTALL);     // D0b: close installPosterior (the Card-4 posterior readback)
        subBegin(SB_NETAR);     // D0b: producer-side network AR Phase-2 (log-only; OFF in tier)
        networkProducerAr(ctx, config_, log, traces, se, est, x_s, stations,
                          est_index_of, asm_res, plan, ar_ppp_utils, log_dir_);
        subEnd(SB_NETAR);      // D0b: close producer-AR / pre-emit tail
        subBegin(SB_SOLPREP);  // D0b: toSolutionEpoch + per-sat discontinuity signalling
        NetworkSolPrepResult sp_ = networkSolPrep(ctx, config_, log, traces, setup, se, est, x_s,
                                                  plan, pl_now_s, datum_changed, changed_consts,
                                                  sigma0_adapt, perconst_active, perconst_publish,
                                                  perconst_dec_withhold, perconst_stats);
        auto& sol = sp_.sol;
        auto& epoch_withheld_consts = sp_.epoch_withheld_consts;
        auto& global_bump = sp_.global_bump;
        auto& serialized_consts = sp_.serialized_consts;
        auto& retro_emit_sats = sp_.retro_emit_sats;
        subEnd(SB_SOLPREP);     // D0b: close sol-prep + discontinuity signalling
        // The fast anchor may advance ONLY on an epoch whose solution reached the
        // wire.  Every whole-epoch withhold (observability gate, A4 sigma0 ceiling, SH
        // health gate) `continue`s or returns before networkEmitStore's emit site, and
        // n_emitted is incremented exactly there — so observing that counter across the
        // call is the honest EMISSION-continuity test.  Keying on solve continuity
        // instead is precisely the R11 defect the C1 card documented, where a delta
        // measured inside a withheld era was publishable across a consumer-visible gap.
        const std::size_t fc_n_emitted_before = ctx.counters.n_emitted;
        subBegin(SB_EMITSTORE); // D0b block 7: re-base + PB-emit filter + store push + solution CSV
        networkEmitStore(ctx, config_, log, traces, setup, se, est, x_s, pl_now_s,
                         datum_changed, sigma0_adapt, sol, epoch_withheld_consts, global_bump,
                         serialized_consts, retro_emit_sats, emit_rebase_on, emit_pb_filter_on,
                         pb_filter_emit_sigma, emit_rebase_engine, pb_filter_engine,
                         clk_rebase_engine, present,
                         est_index_of, modeled, shared_phase_keys, carry_slipped_ambs,
                         we_t2_opp, we_t2_flag, cl_epochs, cl_ar_ran, cl_ar_fixed, cl_sum_fixed,
                         cl_first_fix_tow, cl_first_fix_week, cl_recent_err3d);
        subEnd(SB_EMITSTORE);   // D0b: close block 7 (emit/store, order-sensitive serial)
        // Harvest the fast anchor from THIS epoch's emitted solution.  `sol` is read
        // AFTER networkEmitStore, so the cached c0 values are the re-based / PB-filtered
        // ones the consumer actually received — which is what makes the fast product a
        // delta in the EMITTED frame, cancelling every per-era frame constant.
        if (fc_on && ctx.counters.n_emitted != fc_n_emitted_before) {
            subBegin(SB_FASTCLOCK);
            networkFastClockHarvest(ctx, config_, log, setup, se, present, modeled, sol,
                                    fc_ppp_utils);
            subEnd(SB_FASTCLOCK);
        }
        subBegin(SB_IONODIAG);  // D0b block 4a: iono / ZWD / anchor-info viz traces
        networkIonoDiag(ctx, config_, log, traces, setup, ws, se, est, x_s, plan, asm_res,
                        est_index_of, modeled, registered_clock_sats, clk_anchor_sigma);
        subEnd(SB_IONODIAG);   // D0b: close block 4a (iono/ZWD/anchor traces)
        subBegin(SB_DDFRAC);   // D0b block 6: cross-station DD fractional-residual (B5) trace
        networkDdFrac(ctx, config_, log, traces, setup, se, est, x_s, plan, asm_res,
                      est_index_of, dd_ppp_utils);
        subEnd(SB_DDFRAC);       // D0b: close block 6 (dd-frac B5)
        subBegin(SB_SHDEV_VTEC); // D0b block 4b: T5 SH-deviation carry KF + VTEC coeff trace
        networkShDevVtec(ctx, config_, log, traces, setup, se, est, x_s, plan, asm_res,
                         est_index_of, modeled, iono_deweight, sh_dev_on);
        subEnd(SB_SHDEV_VTEC);  // D0b: close block 4b (T5 sh-dev + VTEC trace)
        subBegin(SB_T1A);       // D0b block 3: T1a SH health probe + fused adaptive-deweight gate
        networkShHealth(ctx, config_, log, traces, setup, se, est, x_s, asm_res, est_index_of,
                        modeled, iono_deweight, carry_vce_est, prires_est, datum_changed,
                        pl_now_s, first_vce_s, last_vce_hour);
        // IONO-DEWEIGHT RELEASE HYGIENE.
        // MUST sit HERE: networkShHealth above is the ONLY caller of the gate's
        // advance(), and the release edge created by that slew raises the weight the
        // NEXT epoch's PASS-B assembly multiplies into the SH<->slant tie rows — so the
        // response has exactly this one epoch to land before the parked states are
        // re-coupled.  After networkShDevVtec too: that seam's harvest is the epoch's
        // legitimate deviation measurement update, and erasing the stash ahead of it
        // would discard good information.  No-op unless an increment is armed.
        networkIonoDwRelease(ctx, config_, log, traces, setup, se, est, x_s,
                             est_index_of, modeled, iono_deweight);
        subEnd(SB_T1A);        // D0b: close block 3 (T1a health + deweight)
        subBegin(SB_LEVELING); // D0b block 5: phase-leveling carry harvest + T1d SH-convergence
        networkLeveling(ctx, config_, log, traces, setup, se, est, x_s, plan, asm_res,
                        est_index_of, carry_vce_est, prires_est, carry_warm, pl_now_s,
                        registered_clock_sats, registered_code_keys, shared_phase_keys);
        subEnd(SB_LEVELING);   // D0b: close block 5 (leveling + T1d SH-convergence)
        subBegin(SB_RAIM);     // D0b block 2: post-fit network RAIM + median-polish attribution
        // POOL RE-HOST: `carry_warm` requires
        // network.phase_leveling, so this gate alone made post-fit RAIM unreachable on
        // every Card-4 lineage that leaves leveling off — the seam reported ARMED at load
        // and never executed.  ctx.knobs.raim_ci_on is the re-hosted arming condition
        // (already ANDed with raim_on and the resolved carry_invariant).
        if (raim_on && (carry_warm || ctx.knobs.raim_ci_on)) {
            networkPostFitRaim(ctx, config_, log, traces, se,
                               stations, asm_res, est_index_of, present, x_s, est,
                               plan.shared_datum, clock_refloat_sats, retro_flag_sats,
                               retro_feed, n_arc_refloat_epoch, prires_est,
                               &slip_qc.epoch);
        }
        // Card GL (docs/SPEC_event_harness.md §24.18): the GROSS LINE-OF-SIGHT
        // screen — the one acting post-fit tier that screens CODE.  Placed inside
        // this bracket, immediately after the RAIM pass, for the ordering reason
        // RAIM's own header gives: erasing a `local_carry` entry IS the refloat, so
        // it must run AFTER the carry harvest (SB_LEVELING) and BEFORE the epoch
        // stash tail.  It has NO arming interlock on RAIM: it owns its own residual
        // walk and its own state precisely because every other tier is phase-only,
        // and it must be able to run on a lineage that arms none of them.  No-op
        // (not even a state allocation) unless network.gross_los_screen.
        networkGrossLosScreen(ctx, config_, log, traces, se, stations, est, x_s,
                              asm_res, est_index_of, pl_now_s);
        subEnd(SB_RAIM);       // D0b: close block 2 (RAIM)
        subBegin(SB_CLOCKUPD); // D0b block 8: two-state clock update + fault QC + retro-flag feed
        networkClockQc(ctx, config_, log, traces, ws, se, est, x_s,
                       registered_clock_sats, datum_changed, changed_consts,
                       clk_model_warm, emit_rate_warm, pl_now_s, dt,
                       clock_refloat_sats, retro_flag_sats, retro_feed);
        subEnd(SB_CLOCKUPD);       // D0b: close block 8 (clock update + retro feed)
        // SPEC_day129_frame_guards P1-3/P1-4: trigger instrumentation.  BEFORE the
        // stash/status block, because the epoch-status row written there reads the
        // post-fit sigma0 this fills in.  All default-OFF; an unarmed run makes
        // neither call (byte-identical, no cost beyond the flag tests).  The §4.3
        // keyed trace rides the same residual pass, so either toggle runs it.
        subBegin(SB_FRAMEDIAG);
        // BLOCKFORMAL predict half: freeze each
        // family's through-PREVIOUS-epoch σ̂₀ and clear the per-epoch ladders
        // BEFORE the census's rider feeds this epoch's blocks.  Predict-then-
        // update is the D5 v2 contract — a block standardized by a calibration it
        // contributed to can standardize itself away.
        if (ctx.knobs.bb_on) ctx.carry.bb.beginEpoch(pl_now_s, se.time.tow);
        // Station-congruence predict half,
        // same contract and same reason: freeze the through-PREVIOUS-epoch σ̂₀
        // before the census's rider feeds this epoch's stations, so a station
        // cannot standardize its own coordinate fault away.
        if (ctx.knobs.cg_on) ctx.carry.cg.beginEpoch(pl_now_s, se.time.tow);
        if (nc.trace_postfit_sigma0 || nc.trace_postfit_keyed || nc.trace_postfit_rows)
            networkPostfitSigma0(ctx, config_, log, traces, se, stations, est, x_s,
                                 asm_res, est_index_of, &modeled);
        if (nc.trace_sh_spectrum)
            networkShSpectrum(ctx, config_, log, traces, se, est);
        // SPEC_event_harness §24.10 card ZP: the per-station LOCAL-block audit.
        // Measure-only (const accessors on the solved estimator), and placed in
        // this group because it is the station-side companion of the shared-side
        // spectrum above.  Its own cadence gate (EMITTED epochs 1..20, then every
        // 60th) lives in the seam, so the driver carries no counter.
        if (nc.trace_station_block)
            networkStationBlock(ctx, config_, log, traces, se, stations, est, x_s,
                                asm_res, est_index_of);
        // docs/SPEC_prefit_census_observability.md INC-1/3/4 — the PREFIT
        // innovation census, the seed-birth tripwire and the topology ledger, all
        // on one walk of this epoch's assembled row stacks.  FIRST in the
        // measure-only group: it costs O(rows), it writes the epoch-status
        // scalars the stash block picks up, and it is the free tier the
        // redundancy census escalates from.  The master knob keeps an unarmed run
        // out of the translation unit entirely.
        if (ctx.knobs.pc_walk_on)
            networkPrefitCensus(ctx, config_, log, traces, se, stations, est,
                                asm_res, est_index_of, &iono_deweight);
        // docs/SPEC_redundancy_census.md — the redundancy / testability census.
        // BEFORE networkRhsAttribution on purpose: a flight-recorder trip epoch
        // forces a full sweep, and the trip predicate is the attribution's own
        // (un-consumed) window test, so the census must read it before the
        // attribution seam advances rhs_attrib_done.  Measure-only; the master
        // knob keeps an unarmed run out of the translation unit entirely.
        if (ctx.knobs.redun_census_on)
            networkRedundancyCensus(ctx, config_, log, traces, se, stations, est,
                                    plan.shared_datum, asm_res, est_index_of, x_s);
        // docs/SPEC_redundancy_census.md BB-INC-1..3 — drain this epoch's LOS-block
        // ladders, write network_block_baarda.csv and emit the block identities.
        // The FEED is inside the census above (that is the whole point: the block
        // sweep inherits its stride and escalation instead of adding a scheduler),
        // so this must run AFTER it; the accumulator carries nothing forward, so
        // an epoch that swept blocks and never reached here would simply not
        // report them.
        if (ctx.knobs.bb_on)
            networkBlockBaardaEmit(ctx, config_, log, traces, se, stations);
        // docs/SPEC_redundancy_census.md CG-INC-1..3 — drain this epoch's per-station
        // coordinate-offset records, write network_congruence.csv and emit the loud
        // stations' identities.  The FEED is inside the census above (same host,
        // same stride, no scheduler of its own), so this must run AFTER it.  It
        // does NOT read ctx.carry.bb_hits — the separability join was resolved at
        // the sweep site — so its position relative to the block census's own emit
        // is free.
        if (ctx.knobs.cg_on)
            networkCongruenceEmit(ctx, config_, log, traces, se, stations);
        // docs/SPEC_redundancy_census.md MDB-INC-1 — aggregate this epoch's
        // Baarda MDB rows, write network_mdb.csv, emit the reporting events and
        // reconcile the uncheckable join.  BOTH orderings here are load-bearing:
        // AFTER networkPostfitSigma0, whose per-family σ̂₀ the SCALED variant needs
        // (read earlier it would silently apply the PREVIOUS epoch's inflation),
        // and AFTER the redundancy census, whose per-epoch row counts the join
        // compares against.  The feed sites are in SB_RAIM and in the census; this
        // is only the drain, and the accumulator is stamped so an epoch that fed
        // rows and never reached here leaks nothing forward.
        if (ctx.knobs.mdb_on)
            networkMdbFlush(ctx, config_, log, traces, se);
        // docs/SPEC_datum_graph_census.md — the STRUCTURAL S-basis dimension:
        // predicted from this epoch's admitted support topology and reconciled
        // against the spectral count the solve glue handed forward.  Placed in
        // the measure-only group because it is one: it reads the registration
        // sets and `modeled`, and writes nothing but rows, events and counters.
        // Order within the group is free — it shares no accumulator with the
        // censuses above — so it sits last among them, ahead of the attribution
        // seams that consume the trip predicate.  The master knob keeps an
        // unarmed run out of the translation unit entirely.
        if (ctx.knobs.dg_on)
            networkDatumGraphCensus(ctx, config_, log, traces, se, stations, modeled,
                                    est_index_of, plan, shared_phase_keys,
                                    registered_code_keys, registered_clock_sats,
                                    datum_changed);
        if (est.attribArmed())
            networkRhsAttribution(ctx, config_, log, traces, se, stations, est,
                                  plan.shared_datum, asm_res, est_index_of);
        // INC-5 flight recorder: consume a trip raised anywhere earlier this
        // epoch (SH-gate engage, clock runaway, a00-band exit — the A4 withhold
        // consumes its own, since that epoch never reaches here), then RETAIN
        // this epoch for the next trip's "before" frame.  Last in the group so a
        // trip dump sees the same kernel state every other diagnostic just read.
        networkRhsAttribTrip(ctx, config_, log, traces, se, stations, est,
                             plan.shared_datum, asm_res, est_index_of);
        subEnd(SB_FRAMEDIAG);
        subBegin(SB_STASH_STATUS); // D0b: D2/C1/SH carry re-stash + VCE fold + epoch-status QC
        networkStashStatus(ctx, config_, log, traces, setup, se, est, x_s, plan, carry_vce_est,
                           prires_est, clock_refloat_sats, n_arc_refloat_epoch, registered_clock_sats,
                           registered_code_keys, retro_emit_sats, shared_phase_keys,
                           sigma0_adapt, total_pins, vce_wants_cb, vce_wants_pb, dt,
                           datum_changed, changed_consts, n_clock, n_present, n_pb, n_cb,
                           n_dropped_sats, n_adapt);
        // RF-1 §7.4: drain THIS epoch's buffered carry-state rows.  Placed last inside
        // the bracket because it is the first point after EVERY row source has run —
        // the leveling harvest and the gap prune (SB_LEVELING), the T4 / T3 refloats
        // (SB_RAIM) and the phase-anchor carry re-reference (the datum block).  A no-op
        // with an empty buffer, which is every unarmed run.
        networkAmbStateTrace(ctx, log, traces);
        subEnd(SB_STASH_STATUS);   // D0b: close the stash + epoch-status tail (before the readout)

        // D0: per-epoch section-timer readout (emitted-epoch path).  Deltas of the
        // estimator's run-cumulative accumulators against the loop-top snapshot give
        // THIS epoch's kernel cost.  The driver assembly wall (sec_asm_ms_epoch) covers
        // the primary produce+scatter section; reduce/solve/cov-inverse/cov-consumers
        // happen after the wall, so `other` (QC decision logic + emit + I/O + any
        // within-epoch RAIM re-solve) = total − assembly − reduce − solve − cov-inverse
        // − cov-consumers.  d_estadd is the kernel est.addStation delta — a CROSS-CUTTING
        // figure (it also captures re-solve adds that fall AFTER the wall, so it is NOT a
        // strict sub-part of the assembly wall) reported separately, not nested under it.
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

            // D0b: per-epoch sub-block attribution of `other`.  net = wall − Δ(timed
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

        // RTB-7(a) periodic atomic snapshot (INC-4: body in NetworkRunContext.cpp;
        // stays AFTER the prev_week/prev_tow advance — snap captures THIS epoch).
        networkWriteStateSnapshot(ctx, config_, se.time, pl_now_s, emitter, carry_driver,
                                  stations,              // RS-8 §21.16 CN rows (the latch
                                                         //   lives in the station models)
                                                         //   + RS-9 §21.18 RL x_lin rows
                                  log,
                                  &clk_rebase_engine,    // §18.10 frame rows (v2)
                                  &emit_rebase_engine,   // RS-5 §21.10 emit-layer rows
                                  &pb_filter_engine,     //   (ER / PF / PS)
                                  setup.consumer.get()); // Card CC (B): consumer snapshot beside each history copy
        return LoopSignal::Next;
    };

    // === Epoch source =======================================================
    // Batch (default): iterate the pre-built synced vector.  Streaming
    // (network.streaming.enable — RTF-3): per-station live lanes → wait-budget
    // aligner → bounded epoch queue, popped here on the solve thread.  ONLY
    // the iteration differs: same per-epoch body, same order, same teardown.
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
