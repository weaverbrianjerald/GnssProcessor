/**
 * @file RealTimeEngine.cpp
 * @brief Real-time GNSS PPP processing engine implementation.
 *
 * Mirrors the per-epoch logic of Application (post-processing) but designed
 * for epoch-at-a-time streaming operation rather than batch iteration.
 */

#include "realtime/RealTimeEngine.hpp"
#include "aiding/ImuAidingSource.hpp"   // full type for aiding_manager_->imu() wiring
#include "app/EpochUpdateSupport.hpp"
#include "products/ProductCache.hpp"
#include "products/SsrCorrectionStore.hpp"
#include "products/LiveEphemerisStore.hpp"
#include "sbf/SbfParser.hpp"
#include "math/Coordinates.hpp"
#include "measurement/StateInitializer.hpp"   // systemToIsbName (clock-disc ISB scoping)
#include "common/Constants.hpp"
#include "common/PppUtils.hpp"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <limits>
#include <map>
#include <set>
#include <fmt/format.h>

namespace GnssProcessor {

namespace {

/// Config-section name for a constellation, or nullptr if not user-selectable.
const char* systemConfigName(System sys) {
    switch (sys) {
    case System::GPS:     return "GPS";
    case System::Galileo: return "Galileo";
    case System::GLONASS: return "GLONASS";
    case System::BeiDou:  return "BeiDou";
    case System::QZSS:    return "QZSS";
    case System::SBAS:    return "SBAS";
    default:              return nullptr;
    }
}

/// True iff @p sys appears in the configured active-systems list.
bool isSystemActive(System sys, const std::vector<std::string>& active) {
    const char* name = systemConfigName(sys);
    if (!name) return false;
    return std::find(active.begin(), active.end(), name) != active.end();
}

/// Push the live store's GLONASS FDMA channel map (R## → k) into PppModel
/// whenever this frame delivered GLONASS broadcast ephemeris.  Shared by the
/// RTCM and SBF ingest paths so both seed per-satellite GLONASS wavelengths
/// identically — from the live 1020 / GLONav @c freq_channel, not the
/// hard-coded default table.  No-op when the frame carried no GLONASS nav.
void refreshGlonassSlots(const std::vector<BroadcastEphemeris>& fresh,
                         LiveEphemerisStore& store, PppModel& model) {
    for (const auto& e : fresh) {
        if (e.system == System::GLONASS) {
            model.setGlonassSlots(store.glonassFreqChannels());
            return;
        }
    }
}

} // namespace

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------

RealTimeEngine::RealTimeEngine(GnssConfig& config, LogManager& log_manager)
    : config_(config), log_manager_(log_manager) {
}

// ---------------------------------------------------------------------------
// GPS -> UTC helper
// ---------------------------------------------------------------------------

GnssTime RealTimeEngine::gpsToUtcTime(const GnssTime& gps_time) {
    const double gps_secs = TimeUtils::continuousGpsSeconds(gps_time);
    const double utc_secs = gps_secs - TimeUtils::LEAP_SECONDS;
    int utc_week = static_cast<int>(std::floor(utc_secs / TimeUtils::SECONDS_IN_WEEK));
    double utc_tow = utc_secs - static_cast<double>(utc_week) * TimeUtils::SECONDS_IN_WEEK;
    if (utc_tow < 0.0) { utc_tow += TimeUtils::SECONDS_IN_WEEK; --utc_week; }
    return GnssTime{ utc_week, utc_tow };
}

// ---------------------------------------------------------------------------
// Initialization
// ---------------------------------------------------------------------------

bool RealTimeEngine::initialize() {
    auto log = log_manager_.getLogger("rt_engine");
    log->info("Initializing real-time engine");

    // RTB-1(c): apply the configured GPS-UTC leap-second count to the RT
    // wall-clock→GPS conversion (nowContinuousGpsSeconds) — the host-clock
    // anchor of the RTCM decode paths (cold-start week promotion, GLONASS day
    // resolution) and the obs-lag measurement.  Default equals the compiled
    // TimeUtils::LEAP_SECONDS, so an unconfigured run is a no-op.  Set BEFORE
    // any stream thread decodes a frame.  Nav-derived leap seconds (LNAV
    // ΔtLS) are the RTF-scale follow-on.
    TimeUtils::setLeapSecondsOverride(
        static_cast<double>(config_.getRealTimeConfig().leap_seconds));

    // RTA-13 (opt-in): arm the CODE-domain band-wise bias fallback on the
    // process-wide store for the live-stream path.  An injected OWNED store is
    // armed by its owner (Application::runRealTime) at creation.  Default off —
    // the store is untouched and every lookup byte-identical.
    if (config_.ssrBiasBandFallback()) {
        SsrCorrectionStore::getInstance().setCodeBiasBandFallback(true);
        log->info("SSR code-bias band fallback ON (processing.ssr_bias_band_fallback): an "
                  "exact-attribute miss consumes the same-band sibling CODE bias (residual "
                  "attribute DCB absorbed by receiver code-bias states); PHASE never falls back.");
    }

    // Card 7 (opt-in, same pattern): arm the orbit/clock coherence skew budget
    // on the process-wide store.  An injected OWNED store is armed by its owner
    // (Application::runRealTime) at creation.  Default 0 — the store is
    // untouched and clock application byte-identical.
    if (const double skew = config_.getNetworkConsumerConfig().orbit_clock_max_skew_s;
        skew > 0.0) {
        SsrCorrectionStore::getInstance().setOrbitEchoMaxSkew(skew);
        log->info("Orbit/clock coherence budget ON "
                  "(processing.network_consumer.orbit_clock_max_skew_s = {:.1f}s): a "
                  "network clock whose orbit echo disagrees with the applied SSR orbit "
                  "is withheld per satellite (RTA-9 ladder then governs).", skew);
    }

    // ProductParser (loads static products: ATX, BIA, broadcast nav).  In a
    // live session orbits/clocks come from the broadcast ephemeris + IGS-SSR,
    // not precise SP3/CLK, so those are skipped below.
    product_parser_ = std::make_unique<ProductParser>(config_, log_manager_);

    // RinexParser is still needed as a structural dependency of PppModel for
    // header metadata (antenna type, approximate position).  In real-time mode
    // we construct it but parsing the observation file is optional  if one is
    // configured we parse it to seed the header, otherwise we set defaults.
    rinex_parser_ = std::make_unique<RinexParser>(config_, log_manager_, *product_parser_);

    std::filesystem::path obs_path = config_.getObservationPath();
    if (!obs_path.empty() && std::filesystem::exists(obs_path)) {
        if (!rinex_parser_->parse()) {
            log->warn("Failed to parse RINEX for header seeding  using defaults");
        }
    }

    // Live SBF/RTCM streams carry no RINEX ANT # / TYPE record.  Seed the
    // receiver antenna type from config when the header is empty so the
    // receiver ATX entry can be matched (PCO/PCV + the require_atx_coverage
    // gate).  The RINEX header (post-processing) always wins — this only
    // fills a genuinely-empty field.  This is the single injection seam;
    // Application::run() (post-processing) never calls setReceiverAntennaType.
    if (rinex_parser_->getReceiverAntennaType().empty()) {
        const std::string& ov = config_.getModelCorrections().receiver_antenna_type_override;
        if (!ov.empty()) {
            rinex_parser_->setReceiverAntennaType(ov);
            log->info("Seeded receiver antenna type from config override: '{}'", ov);
        }
    }

    // Load products.  Skip precise SP3/CLK when a live broadcast+SSR source
    // supplants them (SSR corrections enabled and/or a live socket primary);
    // a real-time *file* replay with no corrections still loads them so a
    // historical run can use precise products.  RTB-4: orbit and clock are gated
    // independently so a "precise (ultra-rapid) orbits + network clocks" config
    // loads SP3 but skips the precise CLK.  With use_precise_orbits=false (default)
    // both gates equal realtimeUsesLiveOrbitClockSource() — byte-identical.
    const bool skip_orbit = config_.realtimeUsesLiveOrbitSource();
    const bool skip_clock = config_.realtimeUsesLiveOrbitClockSource();
    product_parser_->loadProducts(!skip_orbit, !skip_clock);

    auto& cache = ProductCache::getInstance();
    if (!cache.loadProducts(*product_parser_, log_manager_)) {
        log->error("Failed to initialize product cache");
        return false;
    }
    log->info("Product cache: {} orbits, {} clocks",
              cache.getOrbitCount(), cache.getClockCount());

    // State manager.
    constexpr size_t core_state_size = 5; // Pos(3) + Clk(1) + Tropo(1)
    state_manager_ = std::make_unique<StateManager>(core_state_size, log_manager_, config_);

    // Seed initial position.
    initial_position_ = Eigen::Vector3d(0.0, 0.0, Constants::EARTH_RADIUS);
    auto approx_pos = rinex_parser_->getHeader().approx_position;
    bool valid_pos = true;
    for (double val : approx_pos) {
        if (!std::isfinite(val) || std::abs(val) > 1e8) { valid_pos = false; break; }
    }
    if (valid_pos && approx_pos != std::array<double, 3>{0.0, 0.0, 0.0}) {
        initial_position_ = Eigen::Vector3d(approx_pos[0], approx_pos[1], approx_pos[2]);
    }
    // NOTE: the seed is applied to ppp_state_ AFTER initializeState() below, not
    // here.  initializeState() + syncPppStateToStateVector() run later in this
    // function and would clobber any early state-vector write with ppp_state_'s
    // zero position (a live SBF/RTCM stream carries no RINEX APPROX POSITION).

    // Processing components.
    ppp_model_       = std::make_unique<PppModel>(config_, *product_parser_, log_manager_, *rinex_parser_, *state_manager_);
    meas_model_      = std::make_unique<MeasurementModel>(*state_manager_, *ppp_model_, config_, log_manager_);
    kalman_filter_   = std::make_unique<KalmanFilter>(*state_manager_, config_, log_manager_, *ppp_model_);

    // Aiding sources (GNSS/INS coupling).  Same manager type as Application::run,
    // so real-time and post-processing share ONE predict path (predictAided).
    // For a GNSS-only stream the set is empty and predictAided delegates to
    // predict(dt), byte-identical to before.  RT IMU samples arrive via
    // feedImuSample.
    aiding_manager_  = std::make_unique<AidingSourceManager>(config_, log_manager_);
    aiding_manager_->reserveAllStates(*state_manager_);   // every source stashes StateManager / reserves blocks
    if (auto* imu = aiding_manager_->imu())
        imu->attachFilter(*kalman_filter_);    // IMU-only: nominal-attitude owner handle (C_b_e_)

    wls_estimator_   = std::make_unique<WlsEstimator>(config_, *state_manager_, log_manager_);
    mw_slip_detector_  = std::make_unique<MwSlipDetector>(config_, log_manager_);

    // TurboEdit MW+GF joint slip repair (high-rate only).  The real-time path
    // streams at the receiver's native rate (typically 1 Hz), so the geometry-
    // free jump used to split a dual-frequency slip per signal is valid here.
    if (config_.getCycleSlipConfig().turbo_edit) {
        turbo_edit_resolver_ = std::make_unique<TurboEditResolver>(config_, log_manager_);
        // Tighten the GF-arc gap tolerance to ~1.5× the nominal cadence,
        // mirroring CycleSlipManager::setSampleInterval on the
        // post-processing side.  Left at the resolver's 120 s default, a
        // 2–120 s stream outage (NTRIP/TCP hiccup) bridges the second
        // difference: TD2 then mixes a gap-length leg with 1 s legs and
        // carries the un-cancelled ionospheric drift across the outage,
        // either mass-resetting converged ambiguities (IVI trip) or
        // aliasing into a false integer repair.  A live stream has no
        // RINEX INTERVAL header, so use the configured rate and fall back
        // to the 1 Hz high-rate design assumption (TurboEdit is
        // high-rate-only by design).
        const double rate_s = config_.getGnssRate() > 0.0 ? config_.getGnssRate() : 1.0;
        turbo_edit_resolver_->setMaxGapSeconds(rate_s * 1.5);
        log->info("Real-time TurboEdit MW+GF joint cycle-slip repair ENABLED "
                  "(GF-arc gap tolerance {:.1f} s).", rate_s * 1.5);

        // Soft-LLI gap bridge (shared with the post-processing path): hold an
        // AR-capable dual-frequency sat's ambiguities across a short receiver
        // LLI gap and let the bridge integer-repair them instead of an
        // immediate reset.  Inert unless turbo_edit_bridge_lli.  A live stream
        // has no interval header, so seed the declared rate (the high-rate
        // gate else falls back to the measured epoch spacing).
        soft_lli_ = std::make_unique<SoftLliBridge>(config_, log_manager_);
        soft_lli_->setSampleInterval(rate_s);
        if (config_.getCycleSlipConfig().turbo_edit_bridge_lli)
            log->info("Real-time soft-LLI bridge ENABLED (hold + bridge-repair "
                      "across LLI gaps ≤ {:.1f} s).",
                      config_.getCycleSlipConfig().turbo_edit_max_gap_s);
    }

    // Higher-order ionosphere is not yet wired into the real-time path:
    // the engine relies on RTCM SSR for ionospheric content, not on an
    // IONEX GIM, and an SSR-to-STEC adapter for HigherOrderIonosphere has
    // not been written.  Surface this clearly so a user who enabled
    // higher-order globally doesn't silently get the first-order-only
    // pipeline at runtime.
    if (config_.getModelCorrections().enable_higher_order_ionosphere) {
        log_manager_.getLogger("ionosphere")->warn(
            "Real-time mode: higher_order_ionosphere is enabled but unsupported "
            "in the streaming pipeline (no GIM source) — the correction will be "
            "skipped.  Disable the flag in real-time configs to avoid this warning.");
    }

    // GIM ionosphere-weighting (PPP-RTK soft iono constraint) is a
    // post-processing-only feature: the pseudo-observation that pulls each
    // per-sat slant-iono state toward the GIM lives in Application::run and is
    // not wired into the streaming path (the RTCM stream carries no IONEX/VTEC).
    // Warn so an enabled flag isn't a silent no-op at runtime.
    if (config_.getIonosphereWeightingConfig().enabled) {
        log_manager_.getLogger("ionosphere")->warn(
            "Real-time mode: ionosphere_weighting is enabled but unsupported in "
            "the streaming pipeline (no GIM source) — no iono constraint will be "
            "applied.  Disable the flag in real-time configs to avoid this warning.");
    }

    // Adaptive ionospheric process noise (IonoVariabilityMonitor) is only
    // constructed and driven in Application::run; the real-time engine never
    // builds the monitor, so the per-sat iono process noise stays static at
    // process_noise.ionosphere.  Warn so an enabled flag isn't a silent no-op.
    if (config_.getAdaptiveIonoQConfig().enabled) {
        log_manager_.getLogger("ionosphere")->warn(
            "Real-time mode: adaptive_iono_process_noise is enabled but not wired "
            "into the streaming pipeline — per-sat iono process noise will stay "
            "static.  Disable the flag in real-time configs to avoid this warning.");
    }

    // Provide BIA satellite biases to the MW detector for WL integer rounding
    mw_slip_detector_->setCorrections(&ppp_model_->getCorrections());
    if (turbo_edit_resolver_)
        turbo_edit_resolver_->setCorrections(&ppp_model_->getCorrections());

    // Integer ambiguity resolution (MLAMBDA) — same configuration surface as
    // the post-processing path (Application::run).  The WL feed comes from the
    // MW slip detector; no CycleSlipManager is required in the realtime path.
    if (config_.getArMode() > 0) {
        AmbiguityResolverConfig ar_config;
        ar_config.ar_mode               = config_.getArMode();
        ar_config.ratio_threshold       = config_.getArRatioThreshold();
        ar_config.elevation_mask_ar     = config_.getArElevationMask();
        ar_config.hold_variance         = config_.getArHoldVariance();
        ar_config.min_epochs            = config_.getArMinEpochs();
        ar_config.max_position_sigma_m  = config_.getArMaxPositionSigma();
        ar_config.ratio_upper_bound     = config_.getArRatioUpperBound();
        ar_config.wl_only               = config_.getArWlOnly();
        ar_config.wl_only_pseudo_var    = config_.getArWlOnlyPseudoVar();
        ar_config.wl_max_frac           = config_.getArWlMaxFrac();
        ar_config.min_success_rate      = config_.getArMinSuccessRate();
        ar_config.nl_max_per_pair_frac  = config_.getArNlMaxPerPairFrac();
        ar_config.ar_active_systems     = config_.getArActiveSystems();
        ar_config.state_ambiguity_basis = config_.getStateAmbiguityBasis();
        ambiguity_resolver_ = std::make_unique<AmbiguityResolver>(ar_config, log_manager_);
        log->info("Real-time AR enabled: mode={}, wl_only={}, ratio>={:.1f}, elev_mask={:.1f} deg",
                  ar_config.ar_mode, ar_config.wl_only ? "on" : "off",
                  ar_config.ratio_threshold, ar_config.elevation_mask_ar);
    }

    ppp_model_->initializeState(ppp_state_);
    // initializeState() resolves the a-priori from a configured reference
    // position or the RINEX APPROX POSITION; a live SBF/RTCM stream has neither,
    // so it leaves the position at zero.  Without a finite a-priori the first
    // computeModels() rejects every satellite on the rx_pos.norm() < 1e3 sanity
    // check, so WLS never runs and the filter can never bootstrap.  Fall back to
    // the coarse Earth-centred seed computed above; WLS refines it to metres on
    // the first epoch with enough satellites.
    if (ppp_state_.position.norm() < 1e3 || !ppp_state_.position.allFinite()) {
        ppp_state_.position = initial_position_;
        log->info("Seeded a-priori position from Earth-centred fallback "
                  "({:.1f}, {:.1f}, {:.1f}) m (no reference/RINEX position for the live stream)",
                  initial_position_.x(), initial_position_.y(), initial_position_.z());
    }
    state_manager_->syncPppStateToStateVector(ppp_state_);

    const bool is_ppp_rtk = (config_.getProcessingMode() == ProcessingMode::PPP_RTK);
    std::filesystem::path log_dir{"logs"};
    const auto& logging_cfg = config_.getLoggingConfig();
    trace_logger_ = std::make_unique<TraceLogger>(log_manager_, log_dir, is_ppp_rtk, logging_cfg);

    // L10: consumer-side SSR-application trace (network.trace_ssr_consumer).  Same "logs"
    // dir as the producer traces (the two hardcoded log dirs coincide in the closed loop).
    if (config_.getNetworkConfig().trace_ssr_consumer) {
        std::error_code ec;
        std::filesystem::create_directories(log_dir, ec);
        ssr_consumer_trace_.open(log_dir / "ssr_consumer_trace.csv");
        if (ssr_consumer_trace_)
            ssr_consumer_trace_ << "week,tow,sat,ssr_age_s,clk_applied,c0_m,ura_m,r_add_var_m2,"
                                   "pb_age_s,n_pb,clk_disc,pb_disc_max,cb_disc_max,pb_fresh\n";
        else
            log->warn("Real-time engine: failed to open SSR-consumer trace {}",
                      (log_dir / "ssr_consumer_trace.csv").string());
    }

    if (logging_cfg.enable_mw_slip_trace) {
        mw_slip_detector_->enableTracing(log_dir);
    }
    if (logging_cfg.enable_turbo_edit_trace && turbo_edit_resolver_) {
        turbo_edit_resolver_->enableTracing(log_dir);
    }

    log->info("Real-time engine initialized (position seeded at ({:.1f}, {:.1f}, {:.1f}))",
              initial_position_.x(), initial_position_.y(), initial_position_.z());
    return true;
}

// ---------------------------------------------------------------------------
// RTCM frame ingestion
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// RTA-8(b): SSR ingest filter — pure record-kind → push-action policy matrix,
// factored out of feedRtcmFrame so it is unit-testable (record-in →
// push-decision-out) without a parser or an engine.
// ---------------------------------------------------------------------------

RealTimeEngine::SsrPushAction
RealTimeEngine::ssrPushAction(SsrAcceptPolicy policy, SsrRecordKind kind) {
    if (policy == SsrAcceptPolicy::All) return SsrPushAction::Push;
    // OrbitOnly: orbit + URA flow as today; combined records are decomposed at
    // the push site (never entering the store, so its combined-wins merge is
    // untouched); everything carrying clock or bias content is dropped —
    // including the high-rate clock, an additive delta on the clock polynomial.
    switch (kind) {
        case SsrRecordKind::Orbit:
        case SsrRecordKind::Ura:            return SsrPushAction::Push;
        case SsrRecordKind::Combined:       return SsrPushAction::DecomposeOrbit;
        case SsrRecordKind::Clock:
        case SsrRecordKind::CodeBias:
        case SsrRecordKind::PhaseBias:
        case SsrRecordKind::HighRateClock:  return SsrPushAction::Drop;
    }
    return SsrPushAction::Drop;   // unreachable; keeps -Wreturn-type quiet
}

SsrOrbitCorrection
RealTimeEngine::combinedOrbitPart(const SsrCombinedCorrection& rec) {
    SsrOrbitCorrection orb;
    orb.sat             = rec.sat;
    orb.iode            = rec.iode;
    orb.delta_rac       = rec.delta_rac;
    orb.delta_rac_dot   = rec.delta_rac_dot;
    orb.received_time   = rec.received_time;
    orb.epoch_time      = rec.epoch_time;    // shared orbit+clock content epoch → orbit dt base
    orb.iod_ssr         = rec.iod_ssr;
    orb.reference_point = rec.reference_point;  // RTB-5(b): the orbit part keeps its dialect tag
    // c0/c1/c2 intentionally discarded: the whole point of the decomposition is
    // that no clock content from this stream reaches the store.
    return orb;
}

RealTimeEngine::SsrAcceptPolicy
RealTimeEngine::parseSsrAcceptPolicy(const std::string& value, bool* recognized) {
    std::string v;
    v.reserve(value.size());
    for (char c : value) v.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    if (recognized) *recognized = true;
    if (v.empty() || v == "all")  return SsrAcceptPolicy::All;
    if (v == "orbit_only")        return SsrAcceptPolicy::OrbitOnly;
    if (recognized) *recognized = false;
    return SsrAcceptPolicy::All;
}

void RealTimeEngine::feedRtcmFrame(const std::vector<uint8_t>& frame,
                                   SsrAcceptPolicy accept,
                                   std::chrono::steady_clock::time_point receipt) {
    // Decode the frame through the engine's PERSISTENT parser (RTB-1(a)):
    // outputs are drained per frame (clear() below), but the parser's
    // stream-time tracker survives, so mod-week epoch fields are promoted by
    // nearest continuation to the last decoded epoch instead of re-anchoring
    // on the host clock every frame.  All three RTCM streams (primary /
    // corrections / ephemeris) share this parser — same GPS timeline.
    rtcm_parser_.clear();
    RtcmParser& parser = rtcm_parser_;
    parser.parseFrame(frame);

    // RTB-3: MSM lock-time → LLI 0x1 derivation, armed by
    // realtime.msm_lli_from_locktime (default OFF — arming changes
    // slip-detection behaviour on RT replays).
    const bool lli_from_locktime =
        config_.getRealTimeConfig().msm_lli_from_locktime;

    // Observations: buffer until a new epoch is detected, then submit the
    // previous epoch.
    // Drop disabled constellations BEFORE the epoch-boundary test (not just in
    // processEpoch): a mis-tagged clock — notably GLONASS, carried as week-0
    // seconds-of-day until full GPS-week promotion lands — emits a wildly
    // different tow that would split a coherent epoch in two.  The leading
    // systems get submitted early (without the trailing ones), and the trailing
    // systems then arrive at the same tow as the just-submitted epoch and are
    // dropped by processEpoch's dt==0 duplicate guard.  That silently starved
    // GPS+Galileo of Galileo whenever GLONASS was present-but-disabled.
    const auto& active = config_.getActiveSystems();
    for (const auto& obs : parser.getObservations()) {
        if (!isSystemActive(obs.system, active)) continue;
        if (pending_obs_.empty()) {
            pending_epoch_time_ = obs.time;
            // RTB-1(b): the assembling epoch's receipt = the stamp of the
            // first frame that contributed observations to it.
            pending_epoch_receipt_       = receipt;
            pending_epoch_receipt_valid_ = true;
        } else if (std::abs(obs.time.tow - pending_epoch_time_.tow) > 0.5) {
            // RTB-1(b): carry the buffered epoch's receipt stamp into
            // processEpoch for the sub-second latency diagnostics.
            processing_epoch_receipt_       = pending_epoch_receipt_;
            processing_epoch_receipt_valid_ = pending_epoch_receipt_valid_;
            // New epoch  process the buffered one.
            processEpoch(pending_obs_);
            processing_epoch_receipt_valid_ = false;
            pending_obs_.clear();
            pending_epoch_time_          = obs.time;
            pending_epoch_receipt_       = receipt;   // RTB-1(b): new epoch's first frame
            pending_epoch_receipt_valid_ = true;
        }
        pending_obs_.push_back(obs);
        if (lli_from_locktime) {
            // RTB-3: derive the loss-of-lock bit from the decoded MSM lock
            // times on the STORED copy (never the parser's).  Station key ""
            // — single-rover engine; the multi-station front-end reuses one
            // LockTimeTracker across streams with per-stream station keys.
            auto& stored = pending_obs_.back();
            for (const auto& [code, lock_ms] : stored.lock_time_ms) {
                if (lock_time_tracker_.update("", stored.satellite, code, lock_ms)) {
                    stored.lli_flags[code] |= 0x1;
                }
            }
        }
    }

    // Push decoded SSR records into the shared SsrCorrectionStore, carrying the
    // real message header (SSR epoch + IOD-SSR) so staleness / IOD diagnostics
    // reflect the provider solution, not wall-clock receipt.  The orbit / clock
    // query paths in Geometry and GnssCorrections consult the store
    // transparently — no further wiring at the per-epoch level.
    //
    // RTA-8(b): each push consults the per-stream accept policy (ssrPushAction —
    // the caller passes the policy of the stream this frame came from).  Under
    // the default All every action is Push and the loop below is byte-identical
    // to the unfiltered original.  Under OrbitOnly, clock/bias content from this
    // stream is dropped and combined orbit+clock records are decomposed into an
    // orbit-only push, so an RTS stream's 1060/1066 clock content can never
    // shadow an injected network clock.
    auto& ssr_store = SsrCorrectionStore::getInstance();
    const SsrHeader& hdr = parser.getLastSsrHeader();
    std::size_t n_ssr_dropped = 0;
    auto allowed = [&](SsrRecordKind kind) {
        if (ssrPushAction(accept, kind) == SsrPushAction::Push) return true;
        ++n_ssr_dropped;
        return false;
    };
    for (const auto& r : parser.getSsrOrbits())
        if (allowed(SsrRecordKind::Orbit))         ssr_store.pushOrbit(r, hdr);
    for (const auto& r : parser.getSsrClocks())
        if (allowed(SsrRecordKind::Clock))         ssr_store.pushClock(r, hdr);
    for (const auto& r : parser.getSsrCombined()) {
        switch (ssrPushAction(accept, SsrRecordKind::Combined)) {
            case SsrPushAction::Push:           ssr_store.pushCombined(r, hdr); break;
            case SsrPushAction::DecomposeOrbit: ssr_store.pushOrbit(combinedOrbitPart(r), hdr); break;
            case SsrPushAction::Drop:           ++n_ssr_dropped; break;
        }
    }
    for (const auto& r : parser.getSsrCodeBiases())
        if (allowed(SsrRecordKind::CodeBias))      ssr_store.pushCodeBias(r, hdr);
    for (const auto& r : parser.getSsrPhaseBiases())
        if (allowed(SsrRecordKind::PhaseBias))     ssr_store.pushPhaseBias(r, hdr);
    for (const auto& r : parser.getSsrUras())
        if (allowed(SsrRecordKind::Ura))           ssr_store.pushUra(r, hdr);
    for (const auto& r : parser.getSsrHighRateClocks())
        if (allowed(SsrRecordKind::HighRateClock)) ssr_store.pushHighRateClock(r, hdr);

    // Push decoded broadcast ephemeris into the live store — this is what lets
    // Geometry::getSatellitePV resolve satellite orbits with no SP3 file loaded.
    auto& eph_store = LiveEphemerisStore::getInstance();
    for (const auto& eph : parser.getBroadcastEphemeris()) eph_store.insert(eph);
    refreshGlonassSlots(parser.getBroadcastEphemeris(), eph_store, *ppp_model_);

    const std::size_t n_ssr =
        parser.getSsrOrbits().size()      + parser.getSsrClocks().size() +
        parser.getSsrCombined().size()    + parser.getSsrCodeBiases().size() +
        parser.getSsrPhaseBiases().size() + parser.getSsrUras().size() +
        parser.getSsrHighRateClocks().size();
    // RTA-8(b): only records that actually reached the store count as "pushed"
    // (a decomposed combined record did — as an orbit).  With the default All
    // policy nothing is dropped and this is exactly the pre-filter n_ssr.
    const std::size_t n_ssr_pushed = n_ssr - n_ssr_dropped;
    if (n_ssr_pushed > 0) {
        if (n_ssr_dropped > 0)
            log_manager_.getLogger("rt_engine")->trace(
                "RTCM {}: pushed {} SSR records ({} dropped by ingest policy) "
                "(epoch wk{} tow{:.0f}, IOD-SSR {}) to store ({} sats cached)",
                parser.lastMessageType(), n_ssr_pushed, n_ssr_dropped,
                hdr.epoch_time.week, hdr.epoch_time.tow, hdr.iod_ssr, ssr_store.size());
        else
            log_manager_.getLogger("rt_engine")->trace(
                "RTCM {}: pushed {} SSR records (epoch wk{} tow{:.0f}, IOD-SSR {}) "
                "to store ({} sats cached)",
                parser.lastMessageType(), n_ssr_pushed, hdr.epoch_time.week,
                hdr.epoch_time.tow, hdr.iod_ssr, ssr_store.size());
        // obs↔SSR sync: the freshest SSR content just advanced, so a previously
        // held obs epoch may now be covered — re-attempt it.  No-op when nothing
        // is held or the sync feature is off.
        drainHeldObs();
    }
    if (!parser.getBroadcastEphemeris().empty()) {
        log_manager_.getLogger("rt_engine")->trace(
            "RTCM {}: cached {} broadcast ephemeris ({} sats in live store)",
            parser.lastMessageType(), parser.getBroadcastEphemeris().size(),
            eph_store.size());
    }
    // Reference-station coordinate (RTCM 1005/1006).  Seed the filter a-priori
    // from the antenna reference point until the KF has bootstrapped — the RTCM
    // analogue of the PVTGeodetic seed in feedSbfFrame.  A live RTCM obs stream
    // carries no RINEX APPROX POSITION, so without this the a-priori is the
    // coarse Earth-centred fallback (geographic pole) from which almost every
    // satellite sits below the elevation mask and the filter can never converge.
    // A configured reference_position always wins; once first_epoch_ clears the
    // KF owns the position and we stop overwriting it.  The startup defer gate
    // keeps first_epoch_ true through warmup, so a 1005/1006 arriving a few
    // seconds into the stream (they come less often than the 1 Hz obs) still
    // seeds before the first estimation epoch.
    if (!parser.getStationCoords().empty()) {
        const auto& s = parser.getStationCoords().front();
        if (first_epoch_ && !config_.hasReferencePosition()
            && s.arp_ecef.allFinite() && s.arp_ecef.norm() > 1e3) {
            // RTA-12(3): blunder gate.  When armed (realtime.arp_blunder_gate_m
            // > 0) and a plausible a-priori already exists (RINEX-header approx
            // position, SBF PVT seed, or an earlier ARP seed), a decoded ARP that
            // disagrees by more than the gate is REJECTED — a mis-configured
            // caster (wrong mountpoint, stale 1005 coordinates) must not teleport
            // the a-priori.  0 (default) keeps the legacy seed-always behaviour.
            const double gate_m = config_.getRealTimeConfig().arp_blunder_gate_m;
            bool blunder = false;
            // RTA-12 fix: only cross-check against a GENUINE prior.  The initial
            // a-priori is the geographic-pole fallback (0,0,EARTH_RADIUS,
            // norm ~6.37e6), which trivially clears norm()>1e3 — testing against
            // it rejects the FIRST legitimate ARP seed (thousands of km from the
            // pole) and the filter never bootstraps.  A prior exists only once
            // ppp_state_.position has moved away from initial_position_ (an SBF
            // PVT seed, an earlier ARP seed, or a RINEX-approx a-priori).
            if (gate_m > 0.0 && ppp_state_.position.allFinite()
                && (ppp_state_.position - initial_position_).norm() > 1e3) {
                const double dist_m = (s.arp_ecef - ppp_state_.position).norm();
                if (dist_m > gate_m) {
                    blunder = true;
                    if (!arp_blunder_warned_) {
                        arp_blunder_warned_ = true;
                        log_manager_.getLogger("rt_engine")->warn(
                            "RTCM {}: station {} broadcast ARP ({:.3f}, {:.3f}, {:.3f}) m "
                            "disagrees with the existing a-priori ({:.3f}, {:.3f}, {:.3f}) m "
                            "by {:.1f} m (> realtime.arp_blunder_gate_m = {:.1f}) — NOT "
                            "overwriting the a-priori (suspect caster/mountpoint "
                            "misconfiguration or stale 1005/1006 coordinates)",
                            parser.lastMessageType(), s.reference_station_id,
                            s.arp_ecef.x(), s.arp_ecef.y(), s.arp_ecef.z(),
                            ppp_state_.position.x(), ppp_state_.position.y(),
                            ppp_state_.position.z(), dist_m, gate_m);
                    }
                }
            }
            const size_t pidx = state_manager_->getPositionIndex();
            Eigen::VectorXd sv = state_manager_->getStateVector();
            if (!blunder && sv.size() >= static_cast<Eigen::Index>(pidx + 3)) {
                sv.segment<3>(pidx) = s.arp_ecef;
                state_manager_->setStateVector(sv);
                ppp_state_.position = s.arp_ecef;
                if (!apriori_seeded_from_station_) {
                    log_manager_.getLogger("rt_engine")->info(
                        "RTCM {}: seeded a-priori from station {} ARP "
                        "({:.3f}, {:.3f}, {:.3f}) m",
                        parser.lastMessageType(), s.reference_station_id,
                        s.arp_ecef.x(), s.arp_ecef.y(), s.arp_ecef.z());
                    apriori_seeded_from_station_ = true;
                }
            }
        } else {
            // A-priori already pinned (config reference_position) or already
            // owned by the KF — just surface the broadcast ARP so it can be
            // eyeballed against the configured/estimated position.
            // RTA-12(3): with the gate armed, additionally CROSS-CHECK the
            // broadcast ARP against a configured reference position (which is
            // never overwritten regardless) and upgrade to a one-shot WARN on
            // disagreement — the operator pointed the engine at the wrong
            // mountpoint or configured the wrong station.
            const double gate_m = config_.getRealTimeConfig().arp_blunder_gate_m;
            bool flagged = false;
            if (gate_m > 0.0 && config_.hasReferencePosition()
                && s.arp_ecef.allFinite() && s.arp_ecef.norm() > 1e3) {
                const auto& rp = config_.getReferencePositionEcef();
                const Eigen::Vector3d ref(rp[0], rp[1], rp[2]);
                const double dist_m = (s.arp_ecef - ref).norm();
                if (dist_m > gate_m) {
                    flagged = true;
                    if (!arp_blunder_warned_) {
                        arp_blunder_warned_ = true;
                        log_manager_.getLogger("rt_engine")->warn(
                            "RTCM {}: station {} broadcast ARP ({:.3f}, {:.3f}, {:.3f}) m "
                            "disagrees with the configured reference position "
                            "({:.3f}, {:.3f}, {:.3f}) m by {:.1f} m (> "
                            "realtime.arp_blunder_gate_m = {:.1f}) — the configured "
                            "position stays authoritative (check mountpoint/station)",
                            parser.lastMessageType(), s.reference_station_id,
                            s.arp_ecef.x(), s.arp_ecef.y(), s.arp_ecef.z(),
                            ref.x(), ref.y(), ref.z(), dist_m, gate_m);
                    }
                }
            }
            if (!flagged)
                log_manager_.getLogger("rt_engine")->info(
                    "RTCM {}: station {} ARP=({:.4f},{:.4f},{:.4f})",
                    parser.lastMessageType(), s.reference_station_id,
                    s.arp_ecef.x(), s.arp_ecef.y(), s.arp_ecef.z());
        }
    }
}

// ---------------------------------------------------------------------------
// Direct observation feed
// ---------------------------------------------------------------------------

void RealTimeEngine::feedSbfFrame(const std::vector<uint8_t>& block) {
    SbfParser parser;
    parser.parseFrame(block);

    // Seed the filter a-priori from the receiver's own PVTGeodetic solution
    // until the KF has bootstrapped.  A live SBF stream carries no RINEX
    // APPROX POSITION, so without this the a-priori is the coarse Earth-centred
    // fallback (geographic pole) from which almost every satellite sits below
    // the elevation mask and the filter can never converge.  A configured
    // reference_position always wins; once first_epoch_ clears, the KF owns the
    // position and we stop overwriting it.
    if (first_epoch_ && !config_.hasReferencePosition()) {
        if (const auto& pvt = parser.getReceiverPositionEcef()) {
            const size_t pidx = state_manager_->getPositionIndex();
            Eigen::VectorXd sv = state_manager_->getStateVector();
            if (sv.size() >= static_cast<Eigen::Index>(pidx + 3)) {
                sv.segment<3>(pidx) = *pvt;
                state_manager_->setStateVector(sv);
                ppp_state_.position = *pvt;
                if (!apriori_seeded_from_pvt_) {
                    log_manager_.getLogger("rt_engine")->info(
                        "Seeded a-priori from PVTGeodetic: ({:.3f}, {:.3f}, {:.3f}) m",
                        pvt->x(), pvt->y(), pvt->z());
                    apriori_seeded_from_pvt_ = true;
                }
            }
        }
    }

    // Broadcast ephemeris -> live store (same path RTCM nav uses).  These are
    // the receiver's DECODED nav blocks, emitted only on ephemeris change.
    // from_igs=false: receiver nav must not clobber an authoritative IGS-stream
    // record (the IGS-RTS SSR is referenced to IGS broadcast — pairing it with a
    // receiver ephemeris is inconsistent; see LiveEphemerisStore::insert).
    auto& eph_store = LiveEphemerisStore::getInstance();
    for (const auto& eph : parser.getBroadcastEphemeris()) eph_store.insert(eph, /*from_igs=*/false);
    refreshGlonassSlots(parser.getBroadcastEphemeris(), eph_store, *ppp_model_);
    if (!parser.getBroadcastEphemeris().empty()) {
        log_manager_.getLogger("rt_engine")->trace(
            "SBF block {}: cached {} broadcast ephemeris ({} sats in live store)",
            parser.lastBlockNumber(), parser.getBroadcastEphemeris().size(), eph_store.size());
    }

    // Raw navigation subframes -> assemble ephemeris -> live store.  Unlike the
    // on-change decoded-nav blocks above, the receiver streams these
    // continuously, so this is the primary ephemeris source for a live SBF run.
    for (const auto& m : parser.getRawNavMessages()) {
        if (auto eph = subframe_assembler_.consume(m)) {
            eph_store.insert(*eph, /*from_igs=*/false);   // receiver nav: never clobbers IGS-stream eph
            log_manager_.getLogger("rt_engine")->trace(
                "SBF raw-nav: assembled {} ephemeris (iode {}, {} sats in live store)",
                eph->satellite, eph->iode, eph_store.size());
        }
    }

    // A MeasEpoch carries a complete epoch of observations — process it
    // directly (no multi-message assembly as in the RTCM/MSM path).
    const auto& obs = parser.getObservations();
    if (!obs.empty()) {
        processEpoch(obs);
    }
}

void RealTimeEngine::feedObservations(const std::vector<RinexObservation>& observations) {
    processEpoch(observations);
}

void RealTimeEngine::feedImuSample(const AidingSample& sample) {
    // Buffer into the IMU aiding source (no-op when no IMU source is active);
    // consumed by the next epoch's predictAided.
    if (aiding_manager_) aiding_manager_->ingestSample(sample);
}

void RealTimeEngine::flush() {
    // Final drain at stream stop: process whatever is buffered WITHOUT the
    // obs↔SSR hold or skip-to-latest (there is no "later" to defer to).  A held
    // sync epoch (older) is submitted before the still-assembling pending epoch
    // (newer) so they reach the filter in time order.
    draining_final_ = true;
    if (!held_obs_.empty()) {
        processEpoch(held_obs_);
        held_obs_.clear();
    }
    if (!pending_obs_.empty()) {
        // RTB-1(b): the final buffered epoch still carries its receipt stamp.
        processing_epoch_receipt_       = pending_epoch_receipt_;
        processing_epoch_receipt_valid_ = pending_epoch_receipt_valid_;
        processEpoch(pending_obs_);
        processing_epoch_receipt_valid_ = false;
        pending_obs_.clear();
        pending_epoch_receipt_valid_ = false;
    }
    draining_final_ = false;

    // RTA-13: end-of-run obs-code attribute-mismatch summary (flush is the
    // engine's end-of-stream hook).  Silent when no mismatch was counted.
    logBiasAttrMismatchSummary("end of run");
    // Card 7: ditto for the orbit/clock coherence-echo mismatches.
    logOrbitEchoMismatchSummary("end of run");
}

void RealTimeEngine::drainHeldObs() {
    if (held_obs_.empty()) return;
    // Move out before re-submitting: processEpoch may re-hold (re-assign held_obs_)
    // if the SSR stream has not yet advanced far enough to cover this epoch.
    std::vector<RinexObservation> obs = std::move(held_obs_);
    held_obs_.clear();
    processing_synced_epoch_ = true;   // bypass skip-to-latest for the SSR-aligned epoch
    processEpoch(obs);
    processing_synced_epoch_ = false;
}

// ---------------------------------------------------------------------------
// Process a single epoch
// ---------------------------------------------------------------------------

bool RealTimeEngine::processEpoch(const std::vector<RinexObservation>& epoch_obs) {
    auto log = log_manager_.getLogger("rt_engine");

    if (epoch_obs.empty()) return false;

    const GnssTime current_time = epoch_obs.front().time;
    state_manager_->setStateEpochTime(current_time);
    LogManager::setCurrentEpochTime(current_time);

    // Mode tag only — the spdlog %E field already carries the GPS time.
    // "[RT] " distinguishes realtime-engine output from post-processing
    // when both modes share a console.
    const std::string epoch_prefix = "[RT] ";

    // --- Real-time obs/wall-clock lag: backlog warn + skip-to-latest ---
    // A live obs stream that lags real time gets processed against FRESHER
    // orbit/clock/SSR corrections → desync garbage ("zero/invalid clock" on
    // every sat).  Measure the lag once against wall-clock GPS time; warn on a
    // backlog, and (opt-in) DROP stale epochs so the filter fast-forwards to the
    // freshest one.  The skip returns BEFORE any continuity state (prev_tow_,
    // prev_epoch_time_, first_epoch_, the KF) is touched — exactly like the
    // duplicate-epoch early-return below — so dt/gap/cycle-slip semantics are
    // preserved: on resume the large dt correctly trips the gap path (WLS clock
    // re-seed).  setStateEpochTime/setCurrentEpochTime above only stamp the log
    // tag, never continuity state, so running them first is harmless.
    {
        const double now_gps = TimeUtils::nowContinuousGpsSeconds();
        const double obs_gps = TimeUtils::continuousGpsSeconds(current_time);
        const double lag_s   = now_gps - obs_gps;
        if (config_.rtBacklogWarnSeconds() > 0.0 && lag_s > config_.rtBacklogWarnSeconds()) {
            if ((backlog_warn_ctr_++ % 30) == 0) {
                // RTB-1(b): lag_s (wall clock vs obs epoch) is truncated to
                // whole seconds by nowContinuousGpsSeconds — good enough for
                // the gates, coarse for diagnosis.  The framer receipt stamp
                // gives the SUB-SECOND receive→process component (assembly
                // wait + backlog queueing) on the steady clock, immune to
                // wall-clock steps.  Diagnostic only; no gate reads it.
                double rx_to_proc_s = -1.0;
                if (processing_epoch_receipt_valid_) {
                    rx_to_proc_s = std::chrono::duration<double>(
                        std::chrono::steady_clock::now() - processing_epoch_receipt_).count();
                }
                if (rx_to_proc_s >= 0.0)
                    log->warn("{}obs backlog: epoch wk{} tow{:.1f} is {:.1f}s behind wall-clock GPS "
                              "(frame rx→process {:.3f}s; corrections will be fresher than these obs)",
                              epoch_prefix, current_time.week, current_time.tow, lag_s, rx_to_proc_s);
                else
                    log->warn("{}obs backlog: epoch wk{} tow{:.1f} is {:.1f}s behind wall-clock GPS "
                              "(corrections will be fresher than these obs)",
                              epoch_prefix, current_time.week, current_time.tow, lag_s);
            }
        }

        // --- obs↔SSR sync: hold obs ahead of the SSR coverage horizon ---
        // PPP-RTK needs obs time-ALIGNED with the corrections, not pinned to wall
        // clock.  An obs epoch newer than (freshest SSR content epoch + ssr_max_age)
        // has no SSR any satellite can use, so processing it now would drive the
        // whole epoch broadcast-grade (and, with drop_broadcast_grade, wipe it).
        // Defer it instead; drainHeldObs() re-attempts it the moment the SSR stream
        // advances.  Liveness: if the feed stalls longer than obs_ssr_sync_max_hold_s
        // we latch a degraded "outage" and pass every epoch through (broadcast-grade
        // / dropped via the existing path) until SSR re-covers the obs — a dead feed
        // can never starve the filter.  Live-SSR real-time only; never on the final
        // flush drain or before the first epoch.
        if (config_.enableObsSsrSync() && !first_epoch_ && !draining_final_
            && config_.realtimeUsesLiveOrbitClockSource()) {
            const auto newest = ssrStore().newestSsrEpoch();
            if (newest) {
                const double horizon = TimeUtils::continuousGpsSeconds(*newest)
                                     + config_.getSsrMaxAge();
                if (obs_gps <= horizon) {
                    sync_hold_since_s_ = 0.0;   // SSR covers this epoch — resume normal
                    sync_outage_       = false;
                    held_obs_.clear();
                } else if (sync_outage_) {
                    held_obs_.clear();          // confirmed outage — pass through degraded
                } else {
                    if (sync_hold_since_s_ <= 0.0) sync_hold_since_s_ = now_gps;
                    const double held_for = now_gps - sync_hold_since_s_;
                    if (held_for <= config_.getObsSsrSyncMaxHoldS()) {
                        held_obs_        = epoch_obs;   // keep-latest (skip-to-latest semantics)
                        held_epoch_time_ = current_time;
                        if ((sync_hold_warn_ctr_++ % 30) == 0)
                            log->debug("{}obs↔SSR hold: obs tow{:.1f} ahead of SSR horizon "
                                       "(newest SSR content tow{:.1f} +{:.0f}s); held {:.1f}/{:.1f}s",
                                       epoch_prefix, current_time.tow, newest->tow,
                                       config_.getSsrMaxAge(), held_for,
                                       config_.getObsSsrSyncMaxHoldS());
                        return false;   // deferred — drained when SSR advances
                    }
                    // Held past the budget: the SSR feed is stalled.  Latch degraded.
                    sync_outage_       = true;
                    sync_hold_since_s_ = 0.0;
                    held_obs_.clear();
                    log->warn("{}obs↔SSR hold expired after {:.1f}s (SSR stream stalled); "
                              "processing degraded until SSR recovers", epoch_prefix, held_for);
                }
            }
        }

        if (config_.enableObsSkipToLatest() && !first_epoch_
            && !draining_final_ && !processing_synced_epoch_
            && lag_s > config_.obsSkipMaxLagS()) {
            log->debug("{}Skipping stale obs epoch TOW {:.3f} ({:.1f}s behind real-time)",
                       epoch_prefix, current_time.tow, lag_s);
            return false;
        }
    }

    // --- Max-duration cutoff (processing.max_duration_s) ---
    // Once the data-time span from the first processed epoch reaches the
    // limit, latch max_duration_reached_ and stop processing.  The poll loop
    // in Application::runRealTime watches the latch and ends the session
    // cleanly (flush + teardown).  The window is anchored below where
    // first_epoch_ clears, so warmup-deferred epochs don't burn the budget.
    if (config_.getMaxDuration() > 0.0 && max_duration_anchored_) {
        const double span = TimeUtils::continuousGpsSeconds(current_time)
                          - max_duration_anchor_s_;
        if (span >= config_.getMaxDuration()) {
            if (!max_duration_reached_)
                log->info("{}Reached max processing duration: {:.1f} s of data processed "
                          "(limit {:.1f} s); ending session", epoch_prefix, span,
                          config_.getMaxDuration());
            max_duration_reached_ = true;
            return false;
        }
    }

    // --- Time step / gap detection ---
    double dt;
    bool gap_detected = false;
    if (first_epoch_) {
        dt = 1.0; // assume 1 s for first real-time epoch
        log->info("{}First epoch", epoch_prefix);
    } else {
        dt = current_time.tow - prev_tow_;
        if (dt < 0.0) dt += TimeUtils::SECONDS_IN_WEEK; // week rollover
        if (dt <= 0.0) {
            // Non-advancing TOW: this is the SAME physical epoch arriving a
            // second time, not a new time step.  A single GNSS epoch reaches the
            // engine as more than one observation-bearing SBF block whenever the
            // receiver co-logs MeasEpoch (4027) AND Meas3Ranges (4109): both
            // decode to observations at the identical TOW (SbfParser::parseFrame)
            // and each independently drives processEpoch via feedSbfFrame.  A
            // retransmitted frame looks the same.  Folding the duplicate in would
            // (a) double-count its measurements in the KF update and (b) feed
            // dt=0 into KalmanFilter::predict -> StateManager::getProcessNoiseMatrix,
            // which hard-throws "Invalid time step: 0" and aborts the session.
            // Skip it cleanly — leave prev_tow_ / state untouched so the next
            // genuinely-advancing epoch steps off the last accepted one.
            log->debug("{}Skipping duplicate epoch at TOW {:.3f} (no time advance; "
                       "second co-timestamped SBF block or retransmit)",
                       epoch_prefix, current_time.tow);
            return false;
        }
        const double gap_tol = config_.getDataGapTolerance() > 0.0
                             ? config_.getDataGapTolerance()
                             : 1.0;  // default: 1 s (typical RT rate)
        gap_detected = dt > gap_tol;
    }
    // Capture the pre-update epoch boundary + first-epoch flag for aiding-source
    // prediction below: prev_epoch_time_ and first_epoch_ are both clobbered
    // before the predict site, but predictAided needs the (prev, current]
    // integration interval and the first-epoch skip.  Unused on the GNSS-only
    // path (empty aiding manager).
    const GnssTime aiding_t_prev = prev_epoch_time_;
    const bool     aiding_first  = first_epoch_;

    prev_tow_ = current_time.tow;
    prev_epoch_time_ = current_time;

    // --- Sync PPP state ---
    try {
        state_manager_->updatePppStateFromVector(
            state_manager_->getStateVector(), ppp_state_, *state_manager_);
    } catch (const std::exception& e) {
        log->error("{}State sync failed: {}", epoch_prefix, e.what());
        return false;
    }

    // --- Compute modeled observations ---
    // Filter to active systems.
    std::vector<RinexObservation> filtered_obs;
    {
        const auto& active = config_.getActiveSystems();
        for (const auto& obs : epoch_obs) {
            if (isSystemActive(obs.system, active)) filtered_obs.push_back(obs);
        }
    }
    if (filtered_obs.empty()) {
        log->debug("{}No observations after system filtering", epoch_prefix);
        return false;
    }

    std::vector<PppObservation> modeled_obs;
    std::map<std::string, DropReason> drop_reasons;
    if (!ppp_model_->computeModels(filtered_obs, ppp_state_, modeled_obs, &drop_reasons)) {
        log->warn("{}Failed to compute models", epoch_prefix);
        return false;
    }

    // --- Corrections-ready startup gate (real-time + SSR only) ---
    // Defer ALL estimation until every modeled above-mask satellite is
    // broadcast+SSR (cm-grade clock, phase retained — NOT broadcast_only) and a
    // solvable floor of them is present, so the filter seeds code+phase together
    // on a clean state instead of cold-starting code-only on metre-grade
    // broadcast clocks (and baking a contaminated iono/bias/ambiguity partition
    // as each sat's SSR flips it cm-grade mid-arc).  No-op when disabled or in
    // post-processing (computeModels never yields broadcast_only with precise
    // CLK present).  Latches via estimation_started_ so it runs only at startup.
    if (config_.getCorrectionsReadyGate().enabled && !estimation_started_) {
        const auto& gate = config_.getCorrectionsReadyGate();

        // Per-constellation tally: cm-grade (broadcast+SSR, phase-retained) and
        // total above-mask counts per active constellation.
        std::map<std::string, int> cm_per_cons, total_per_cons;
        size_t n_cm = 0;
        std::vector<std::string> not_ready;
        for (const auto& o : modeled_obs) {
            const std::string cons = PppUtils::getSystemString(o.satellite);
            ++total_per_cons[cons];
            if (o.broadcast_only) { not_ready.push_back(o.satellite); }
            else { ++n_cm; ++cm_per_cons[cons]; }
        }

        // Per-constellation quorum: every CONFIGURED constellation must field a
        // cm-grade SD quorum before estimation starts — not just the ones that
        // happen to be present this epoch.  Iterating active_systems (rather than
        // total_per_cons) is the crux: a constellation whose broadcast ephemeris
        // is still assembling contributes ZERO satellites, so it is absent from
        // total_per_cons and the old present-only loop waved it through.  That
        // let the gate latch single-constellation (Galileo-only while GPS had no
        // orbit yet); GPS then joined mid-run and perturbed an already-converging
        // filter, driving the ZWD<->height null-space runaway and flagging code.
        // Holding the gate for the absent constellation forces a multi-system
        // start — or the timeout below forces a degraded one.  per_system <= 0
        // disables the per-system check (flat global floor only).
        bool per_system_ok = true;
        std::vector<std::string> cons_waiting;
        if (gate.min_satellites_per_system > 0) {
            for (const auto& cons : config_.getActiveSystems()) {
                // GLONASS yields no SD rows when BSSD is active without the
                // GLONASS-BSSD opt-in, so it can never reach cm-grade quorum;
                // requiring it would stall every startup to the timeout.
                if (cons == "GLONASS" && config_.enableBetweenSatelliteSd()
                    && !config_.enableGlonassBssd()) continue;

                const int total = total_per_cons.count(cons) ? total_per_cons[cons] : 0;
                const int have  = cm_per_cons.count(cons)    ? cm_per_cons[cons]    : 0;
                // Smaller of the configured floor and what the constellation can
                // currently field, but never below 2 (an SD pair): an absent or
                // single-sat constellation (total < 2) still demands 2 cm-grade
                // sats and holds the gate until it shows up.
                const int need = std::min(gate.min_satellites_per_system,
                                          std::max(total, 2));
                if (have < need) {
                    per_system_ok = false;
                    cons_waiting.push_back(cons + " " + std::to_string(have) + "/" + std::to_string(need));
                }
            }
        }

        const bool ready = n_cm >= static_cast<size_t>(gate.min_satellites)
                        && per_system_ok
                        && (!gate.require_all_above_mask || not_ready.empty());

        if (++warmup_epochs_ == 1) warmup_start_time_ = current_time;
        bool timed_out = false;
        if (!ready && gate.timeout_s > 0.0) {
            const double elapsed = TimeUtils::continuousGpsSeconds(current_time)
                                 - TimeUtils::continuousGpsSeconds(warmup_start_time_);
            timed_out = elapsed >= gate.timeout_s;
        }

        std::string waiting;
        for (size_t i = 0; i < not_ready.size(); ++i) {
            if (i) waiting += ",";
            waiting += not_ready[i];
        }
        std::string cons_short;
        for (size_t i = 0; i < cons_waiting.size(); ++i) {
            if (i) cons_short += ", ";
            cons_short += cons_waiting[i];
        }

        if (!ready && !timed_out) {
            if (warmup_epochs_ == 1 || (warmup_epochs_ % 10) == 0) {
                log->info("{}Startup gate: deferring — {} cm-grade total (need >= {}), "
                          "per-constellation [{}], {} still broadcast-only [{}]",
                          epoch_prefix, n_cm, gate.min_satellites,
                          cons_short.empty() ? "ok" : cons_short,
                          not_ready.size(), waiting);
                // SSR-store census localises the problem: zero orbit/clock counts
                // for a constellation => decode/store gap; non-zero counts while it
                // stays broadcast-only => an IODE-match issue (see the per-sat
                // "SSR clock present ... but NOT applied" trace in GnssCorrections).
                log->info("{}  SSR store census: {}",
                          epoch_prefix, ssrStore().censusSummary());
            }
            return false;   // accumulate nav/SSR; no predict/update; first_epoch_ stays true
        }

        estimation_started_ = true;
        if (timed_out)
            log->warn("{}Startup gate: timeout ({:.0f}s) — {} sat(s) still broadcast-only; "
                      "starting in DEGRADED mode", epoch_prefix, gate.timeout_s, not_ready.size());
        else
            log->info("{}Startup gate satisfied: {} cm-grade sat(s) across {} constellation(s) — "
                      "starting estimation (code+phase together){}",
                      epoch_prefix, n_cm, cm_per_cons.size(),
                      not_ready.empty() ? "" : " (broadcast-only stragglers join as SSR arrives)");
    }

    // --- SSR coverage summary (per epoch, info) ---
    // One glanceable line answering "are corrections flowing AND applying?".
    // clk-applied is the TRUE signal: a sat is cm-grade only when its SSR clock
    // cleared the IODE gate (getSatelliteClockCorrection) — i.e. !broadcast_only.
    // The in-store counts are what the SSR store holds fresh for these SAME sats;
    // a gap between in-store clk and clk-applied localises an IODE-match problem
    // (present but rejected) vs a decode/stream gap (not present at all).  Live
    // broadcast+SSR path only — the precise-product replay carries no SSR.
    if (config_.realtimeUsesLiveOrbitClockSource()) {
        std::vector<std::string> sats;
        sats.reserve(modeled_obs.size());
        size_t n_cm = 0;
        for (const auto& o : modeled_obs) {
            sats.push_back(o.satellite);
            if (!o.broadcast_only) ++n_cm;
        }
        const auto cov = ssrStore().coverageFor(sats, current_time);
        log->info("{}SSR coverage: {} sats | clk-applied {}/{} | in-store orb {}/{} "
                  "clk {}/{} cbias {}/{} pbias {}/{}",
                  epoch_prefix, cov.total, n_cm, cov.total,
                  cov.orbit, cov.total, cov.clock, cov.total,
                  cov.code_bias, cov.total, cov.phase_bias, cov.total);
    }

    // --- L10: consumer-side SSR-application trace (network.trace_ssr_consumer) ---
    // A SIBLING of the coverage census above, but gated ONLY on the toggle (NOT on
    // realtimeUsesLiveOrbitClockSource, which is FALSE for the closed-loop consumer — the
    // exact V4 case).  Placed BEFORE the broadcast-grade drop below so dropped sats still
    // appear.  One row per modeled sat: correction age, clk-applied (the TRUE applied signal
    // !broadcast_only — a fresh-in-store clock can still be IODE-rejected), c0/URA from the
    // engine's store, the R-inflation add-variance RECOMPUTED with the IDENTICAL formula as
    // the apply site (MeasurementModel.cpp: ura² + q2·dt³/3, q2=networkUraStalenessQ2, dt from
    // getSsrEpoch), phase-bias age/count/freshness (A5 hold semantics) and disc counters.
    // Blanks (not 0) for absent optionals.  Allocation-light — shares the live 1 Hz path.
    if (ssr_consumer_trace_.is_open()) {
        const auto& store = ssrStore();
        const double now_s = TimeUtils::continuousGpsSeconds(current_time);
        // RTA-9(b): mirror the apply-site windows — with the clock stale-hold
        // rung armed, the clock/URA lookups accept the same widened window the
        // apply gate uses (the recomputed r_add_var then grows with the true dt
        // exactly like the URA channel at the apply site; the rung's fallback-q²
        // term for URA-less sats is NOT folded in here — clk_applied + blank
        // ura_m identifies those rows).
        const double ssr_max_age = SsrCorrectionStore::clockHoldWindow(
            config_.getSsrMaxAge(), config_.getSsrClockStaleHoldFactor());
        const bool   consume_ura = config_.networkConsumeUra();
        const double ura_q2 = config_.networkUraStalenessQ2();
        const bool   bias_age_on = config_.enableSsrBiasMaxAge();
        const double bias_limit = config_.getSsrBiasMaxAge() * config_.getSsrBiasStaleHoldFactor();
        for (const auto& obs : modeled_obs) {
            const std::string& sat = obs.satellite;
            auto& o = ssr_consumer_trace_;
            o << current_time.week << ',' << fmt::format("{:.3f}", current_time.tow) << ','
              << sat << ',';
            const auto ep = store.getSsrEpoch(sat);
            double ssr_dt = 0.0; bool have_ep = false;
            if (ep) { ssr_dt = now_s - TimeUtils::continuousGpsSeconds(*ep); have_ep = true;
                      o << fmt::format("{:.3f}", ssr_dt); }
            o << ',';
            o << (obs.broadcast_only ? 0 : 1) << ',';                       // clk_applied (TRUE signal)
            const auto oc = store.getOrbitClock(sat, current_time, ssr_max_age);
            if (oc && oc->has_clock) o << fmt::format("{:.4f}", oc->c0);
            o << ',';
            const auto ura = store.getUra(sat, current_time, ssr_max_age);
            if (ura) o << fmt::format("{:.4f}", *ura);
            o << ',';
            double add_var = 0.0;   // recomputed R-inflation (0 when consume_ura off / no URA)
            if (consume_ura && ura) {
                add_var = (*ura) * (*ura);
                if (ura_q2 > 0.0 && have_ep && ssr_dt > 0.0)
                    add_var += ura_q2 * ssr_dt * ssr_dt * ssr_dt / 3.0;   // == MeasurementModel apply site
            }
            o << fmt::format("{:.6e}", add_var) << ',';
            const auto pbt = store.getPhaseBiasTime(sat);
            if (pbt) o << fmt::format("{:.3f}", now_s - TimeUtils::continuousGpsSeconds(*pbt));
            o << ',';
            int n_pb = 0;
            int pb_disc_max = std::numeric_limits<int>::min();
            int cb_disc_max = std::numeric_limits<int>::min();
            for (const auto& kv : obs.modeled) {              // mirror the A5 tracked-signal filter
                const std::string& code = kv.first;
                if (code.empty() || code.find('_') != std::string::npos) continue;
                if (code[0] == 'L') {
                    auto pb = bias_age_on ? store.getPhaseBias(sat, code, current_time, bias_limit)
                                          : store.getPhaseBias(sat, code);
                    if (pb) { ++n_pb; pb_disc_max = std::max(pb_disc_max, pb->discontinuity_counter); }
                } else if (code[0] == 'C') {
                    auto d = store.getCodeBiasDiscontinuity(
                        sat, code, current_time,
                        bias_age_on ? bias_limit : std::numeric_limits<double>::max());
                    if (d) cb_disc_max = std::max(cb_disc_max, *d);
                }
            }
            o << n_pb << ',';
            const auto cd = store.getClockDiscontinuity(sat);
            if (cd) o << *cd;                                              // blank on nullopt
            o << ',';
            if (pb_disc_max != std::numeric_limits<int>::min()) o << pb_disc_max;
            o << ',';
            if (cb_disc_max != std::numeric_limits<int>::min()) o << cb_disc_max;
            o << ',';
            o << (n_pb > 0 ? 1 : 0) << '\n';                              // pb_fresh
        }
    }

    // --- Drop broadcast-grade satellites (precise SSR PPP-RTK) ---
    // Once estimation runs, a broadcast_only sat carries metre-grade code (no
    // SSR clock) and no phase.  Feeding it pollutes the cm-grade solution and
    // (worse) lets it seat as a BSSD pivot, injecting a coherent offset into
    // every SD code row.  Drop such sats ENTIRELY here (not just their phase,
    // which computeModels already stripped) so they rejoin cleanly once their
    // SSR arrives; also covers a sat going broadcast_only mid-run (the latched
    // gate does not).  Gated so broadcast-only-float runs keep their code.
    if (config_.getCorrectionsReadyGate().drop_broadcast_grade) {
        size_t dropped = 0;
        for (auto it = modeled_obs.begin(); it != modeled_obs.end(); ) {
            if (it->broadcast_only) { it = modeled_obs.erase(it); ++dropped; }
            else ++it;
        }
        if (dropped > 0)
            log->debug("{}Dropped {} broadcast-grade sat(s) (no SSR clock) from the precise solution",
                       epoch_prefix, dropped);
    }

    // --- WLS seeding ---
    // Under BSSD the KF carries no receiver clock state to seed — skip WLS
    // entirely (its clock estimate is meaningless to the SD filter).
    if (state_manager_->hasClock()) {
        const auto wls_result = wls_estimator_->estimate(current_time, modeled_obs);
        if (wls_result.success() && wls_result.state.size() >= 4) {
            const double wls_clock = wls_result.state(3);
            const Eigen::VectorXd sv = state_manager_->getStateVector();
            const size_t clk_idx = state_manager_->getClockIndex();
            const double kf_clock = (sv.size() > static_cast<Eigen::Index>(clk_idx))
                                    ? sv(clk_idx) : 0.0;

            constexpr double kJumpThresh = 0.00005 * 299792458.0; // 0.5 ms
            const double delta = wls_clock - kf_clock;
            const bool should_seed = (std::abs(delta) >= kJumpThresh)
                                     || gap_detected || first_epoch_;
            if (should_seed && sv.size() > static_cast<Eigen::Index>(clk_idx)) {
                Eigen::VectorXd sv2 = sv;
                sv2(clk_idx) = wls_clock;
                state_manager_->setStateVector(sv2);
                ppp_state_.clock_bias = wls_clock;
                log->info("{}Seeded clock: wls={:.1f} kf={:.1f} d={:.1f}",
                          epoch_prefix, wls_clock, kf_clock, delta);
            }
        }
    }

    // --- BSSD code/phase re-gauge: remove the bulk receiver clock ---
    // Under BSSD the filter carries NO receiver-clock state (StateManager gates
    // it off; buildJacobianRow uses rx_clk=0).  Each per-satellite code-bias
    // state is seeded as (observed - predicted), so it absorbs the FULL
    // receiver clock at its first epoch — on this unsteered AsteRx-U that is up
    // to ~150 km (the "initial=-94715.207 m" sat-bias seed).  Subtract a single
    // per-epoch receiver-clock estimate, COMMON to every satellite, from every
    // code & phase observable BEFORE seeding / predict / SD.  Because the value
    // is identical across satellites it cancels EXACTLY in the between-satellite
    // single difference: the SD measurement, the Kalman update and the position
    // solution are provably unchanged — this is a gauge / conditioning change,
    // never a model change.  What it buys: the bias states seed at DCB scale
    // (~m), the undifferenced residuals no longer drift with the clock, and a
    // failed pivot re-reference is bounded by the clock-estimate error rather
    // than the full unsteered clock.
    //
    // The estimate is the MEDIAN over satellites of the undifferenced CODE
    // innovation (observed - modeled; modeled excludes rx_clk under BSSD).  The
    // median is dominated by the common receiver clock and is robust to the
    // per-satellite DCB / iono / multipath spread.  The standalone WLS/SPP
    // clock is deliberately NOT used on this path: WlsEstimator::estimate reads
    // state(getClockIndex()), and getClockIndex() is unallocated (SIZE_MAX)
    // under BSSD — which is exactly why the WLS-seeding block above is gated on
    // hasClock() and skipped here.  Only the KF `observed` map is touched;
    // raw_observed — consumed by the MW / slip detectors, whose combinations
    // already cancel the clock — is left untouched.  Real-time path only
    // (post-processing PPP-RTK runs Application::run, never this engine).
    if (config_.enableBetweenSatelliteSd() && !state_manager_->hasClock()) {
        std::vector<double> code_innov;
        code_innov.reserve(modeled_obs.size());
        for (const auto& o : modeled_obs) {
            for (const auto& [code, val] : o.observed) {
                if (code.empty() || code[0] != 'C') continue;      // code only
                const auto mit = o.modeled.find(code);
                if (mit == o.modeled.end()) continue;
                const double innov = val - mit->second;
                if (std::isfinite(innov)) code_innov.push_back(innov);
            }
        }
        if (!code_innov.empty()) {
            const size_t mid = code_innov.size() / 2;
            std::nth_element(code_innov.begin(), code_innov.begin() + mid, code_innov.end());
            const double common_clock_m = code_innov[mid];
            if (std::isfinite(common_clock_m)) {
                size_t n_reduced = 0;
                for (auto& o : modeled_obs) {
                    for (auto& kv : o.observed) {
                        const std::string& code = kv.first;
                        if (code.empty()) continue;
                        const char c0 = code[0];
                        if (c0 != 'C' && c0 != 'L') continue;      // code + phase
                        kv.second -= common_clock_m;               // common-mode: SD-invariant
                        ++n_reduced;
                    }
                }
                log->debug("{}BSSD re-gauge: removed common rx clock {:.3f} m "
                           "(median of {} code innovations) from {} observables",
                           epoch_prefix, common_clock_m, code_innov.size(), n_reduced);
            }
        }
    }

    // First real epoch processed — clear first_epoch_ UNCONDITIONALLY.  This was
    // previously inside the hasClock() WLS block above, but BSSD / PPP-RTK has no
    // receiver-clock state, so that block is skipped and the flag never cleared.
    // Leaving it set wedged the engine: dt stayed pinned at 1 s (gaps never
    // detected) and the PVTGeodetic a-priori was re-seeded every epoch, hard-
    // pinning the position so the filter could never converge.  Cold-start
    // epochs with no modelable observations return earlier, so reaching here
    // means this epoch produced a usable measurement set.
    first_epoch_ = false;

    // Anchor the max-duration window on this first fully-processed epoch (epoch
    // GPS time relative to first obs).  Deferred warmup epochs return before
    // here, so the budget starts at real estimation rather than cold start.
    if (config_.getMaxDuration() > 0.0 && !max_duration_anchored_) {
        max_duration_anchor_s_ = TimeUtils::continuousGpsSeconds(current_time);
        max_duration_anchored_ = true;
    }

    // --- KF predict ---
    // Aiding-augmented prediction, shared with post-processing.  GNSS-only
    // stream => empty manager => predictAided delegates to predict(dt),
    // byte-identical.  IMU samples (fed via feedImuSample) are integrated over
    // (aiding_t_prev, current_time].
    if (aiding_manager_ && !aiding_manager_->empty() && !aiding_first) {
        const Eigen::VectorXd aiding_x = state_manager_->getStateVector();
        const AidingEpochContext aiding_ctx{
            aiding_x, aiding_t_prev, current_time, aiding_first };
        kalman_filter_->predictAided(dt, aiding_manager_->collectPredict(aiding_ctx));
    } else {
        kalman_filter_->predict(dt);
    }
    state_manager_->updatePppStateFromVector(
        state_manager_->getStateVector(), ppp_state_, *state_manager_);

    // --- Cycle slip detection / repair (MW, pre-estimation) ---
    {
        const auto slip_results = mw_slip_detector_->detectLargeMwResets(modeled_obs);
        const double reset_std = config_.getAmbiguityStd();
        const double epoch_now_s = TimeUtils::continuousGpsSeconds(current_time);

        // Whether a satellite is the BSSD pivot for its constellation (its SD
        // ambiguity is frozen at 0 — a soft-LLI in-frame integer shift is
        // meaningless there; a pivot LLI needs a handover).  Hoisted so both
        // the TurboEdit block and the default LLI loop can consult it.
        auto isBssdPivot = [&](const std::string& sat) -> bool {
            if (!config_.enableBetweenSatelliteSd()) return false;
            const auto& pivots = meas_model_->getBssdPivotSatellites();
            auto it = pivots.find(PppUtils::getSystemString(sat));
            return it != pivots.end() && it->second == sat;
        };

        // --- Gap-hold registration (pre-pass) ---
        // A phase signal that DROPPED this epoch (no observation, no LLI)
        // with a recent converged snapshot is held rather than left to the
        // measurement model's prune-on-first-miss: the held key is
        // prune-protected and KF-excluded until the TurboEdit gap bridge
        // adjudicates the outage on the signal's return (mirrors
        // CycleSlipManager::detectAndRepair).  Must run before resolve() so
        // the resolver sees the signal held-continuous from the first
        // missing epoch.  No-op unless turbo_edit_bridge_lli.
        if (soft_lli_ && turbo_edit_resolver_ && soft_lli_->enabled())
            soft_lli_->registerGapHolds(modeled_obs, *state_manager_,
                                        isBssdPivot, *turbo_edit_resolver_,
                                        epoch_now_s, epoch_prefix);

        // Satellites whose soft-LLI-held ambiguities the bridge integer-repaired
        // this epoch (snapshot+dN·λ in the pre-pass below): both the TurboEdit
        // repair loop and the default reset loop skip them.
        std::set<std::string> soft_lli_repaired;

        // TurboEdit MW+GF joint repair (high-rate): pair the MW wide-lane
        // integer with the geometry-free phase jump to split a dual-frequency
        // slip into per-signal integer repairs (covariance preserved) and to
        // catch the MW dN1==dN2 blind spot.  Satellites it handles are skipped
        // by the default reset loop below; everything else falls back to a
        // plain ambiguity reset (the original real-time behavior).
        std::set<std::string> turbo_handled_sats;
        // MW-only code-domain events (GF affirmatively stable): per-sat phase
        // signals the resolver adjudicated clean.  The default reset loop
        // below skips a non-LLI MW mandate when the mandate's OWN signal
        // (the amb_key suffix) is adjudicated — per-mandate-signal, exactly
        // the post-processing CycleSlipManager rule, so a mandate on a pair
        // outside the GF solve (arc in warm-up / per-signal dropout) still
        // takes its un-adjudicated signal's reset.
        std::map<std::string, std::set<std::string>> code_clean_signals;
        // Satellites with a WL-settle deferral pending (bridged adjudication
        // waiting on the post-restart MW mean): mandates suppressed, holds
        // kept alive.  Filled inside the turbo block, consumed by the
        // default reset loop below.
        std::set<std::string> te_pending;
        if (turbo_edit_resolver_) {
            // The clean-pair set gates the resolver's GF-only equal-slip
            // split on affirmative MW evidence rather than mere MW silence
            // (a wl-locked arc absorbs real slips without emitting a result);
            // the across-gap WL evidence feeds the bridged solve's WL-settle
            // deferral.
            const auto te = turbo_edit_resolver_->resolve(
                slip_results, mw_slip_detector_->getEpochCleanPairs(),
                modeled_obs, dt,
                mw_slip_detector_->getAcrossGapWlEvidence(epoch_now_s));
            te_pending = te.pending_sats;

            // WL-settle deferral: a pending satellite's still-slipped phase
            // must stay out of the filter while the resolver waits — register
            // holds for its recently converged phase signals (idempotent;
            // the held keys feed the KF exclusion and prune protection).
            // Pivots are excluded as everywhere else; their deferral runs to
            // the resolver's timeout-reset.
            if (soft_lli_) {
                for (const auto& psat : te.pending_sats) {
                    if (isBssdPivot(psat)) {
                        log->warn("{}TurboEdit WL-settle pending for BSSD "
                                  "pivot {} — holds unavailable for a pivot; "
                                  "deferral will run to its timeout-reset",
                                  epoch_prefix, psat);
                        continue;
                    }
                    soft_lli_->registerHold(psat, {}, *turbo_edit_resolver_,
                                            epoch_now_s, epoch_prefix);
                }
            }

            // Soft-LLI bridge resolution (pre-pass): apply snapshot+dN·λ
            // covariance-restoring repairs to any held ambiguities the bridge
            // resolved this epoch, BEFORE the per-signal repair loop — a held
            // sat's reset value would otherwise make x += dN·λ wrong.  Repaired
            // sats are skipped by both loops below.  No-op unless something is
            // held (turbo_edit_bridge_lli).
            if (soft_lli_ && !soft_lli_->empty()) {
                int n_rep = 0, n_res = 0;
                const SoftLliBridge::ResetFn reset_fn =
                    [&](const std::string& key, const std::string& /*sat*/,
                        const std::string& /*sig*/) {
                        state_manager_->resetAmbiguity(key, reset_std);
                    };
                soft_lli_->resolveHolds(te, *turbo_edit_resolver_, modeled_obs,
                                        *state_manager_, epoch_now_s, epoch_prefix,
                                        soft_lli_repaired, n_rep, n_res, reset_fn);
                turbo_handled_sats.insert(soft_lli_repaired.begin(),
                                          soft_lli_repaired.end());
            }

            // Confident per-signal integer repairs (additive, covariance preserved).
            // The BSSD pivot's SD ambiguity is frozen at 0, so an integer shift is
            // meaningless there — reset instead and let the KF coherent-disturbance
            // detector / forwardKfPivotHandovers drive the re-referencing.
            for (const auto& r : te.repairs) {
                if (soft_lli_repaired.count(r.satellite)) continue;  // restored above
                const std::string key = r.satellite + "_" + r.signal;
                const int idx = state_manager_->getAmbiguityIndex(key);
                if (idx < 0) continue;
                if (isBssdPivot(r.satellite)) {
                    state_manager_->resetAmbiguity(key, reset_std);
                    log->info("{}TurboEdit repair refused for {} {} (BSSD pivot); reset",
                              epoch_prefix, r.satellite, r.signal);
                } else if (!(r.lambda > 0.0)) {
                    state_manager_->resetAmbiguity(key, reset_std);
                    log->info("{}TurboEdit repair refused for {} {} (invalid wavelength); reset",
                              epoch_prefix, r.satellite, r.signal);
                } else {
                    Eigen::VectorXd x = state_manager_->getStateVector();
                    const double prev = x(idx);
                    x(idx) += static_cast<double>(r.dN) * r.lambda;
                    state_manager_->setStateVector(x);
                    log->info("{}TurboEdit REPAIR {} {}: dN={:+d} cyc, delta={:+.4f} m, "
                              "x: {:.4f} -> {:.4f} (covariance preserved)",
                              epoch_prefix, r.satellite, r.signal, r.dN,
                              static_cast<double>(r.dN) * r.lambda, prev, x(idx));
                }
                turbo_handled_sats.insert(r.satellite);
            }

            // Satellites the GF jump flagged but could not resolve (iono
            // disturbed / non-integer split): reset their phase ambiguities.
            for (const auto& sat : te.reset_sats) {
                const PppObservation* obs = nullptr;
                for (const auto& o : modeled_obs)
                    if (o.satellite == sat) { obs = &o; break; }
                if (!obs) continue;
                for (const auto& [sig, val] : obs->observed) {
                    (void)val;
                    if (sig.empty() || sig[0] != 'L') continue;
                    if (sig.find('_') != std::string::npos) continue;
                    const std::string key = sat + "_" + sig;
                    if (state_manager_->getAmbiguityIndex(key) < 0) continue;
                    state_manager_->resetAmbiguity(key, reset_std);
                }
                turbo_handled_sats.insert(sat);
            }

            // MW-only code-domain events: MW flagged but the GF phase was
            // affirmatively stable under trusted conditions — the event
            // lives in the code observable (multipath / NLOS / DLL stress),
            // not the phase.  Record the adjudicated-clean signals so the
            // default loop below SKIPS the mandated phase reset; nothing is
            // touched here.  A wrongly vetoed real slip is a null-line pair
            // that lands ≥ |4·λ1| ≈ 0.76 m in the phase innovation at this
            // epoch's update — the KF residual-gate reset (plus the
            // MW/TurboEdit arc-clear that follows it below) is the backstop.
            for (const auto& ce : te.code_events) {
                code_clean_signals[ce.satellite].insert(ce.sig1);
                code_clean_signals[ce.satellite].insert(ce.sig2);
                log->info("{}TurboEdit code-domain event {} {}/{}: MW flagged "
                          "(dN_WL={:+d}{}) but GF stable (|TD2|={:.4f} m <= "
                          "{:.4f} m) — phase reset suppressed (KF residual "
                          "gate is the backstop)",
                          epoch_prefix, ce.satellite, ce.sig1, ce.sig2,
                          ce.dN_wl, ce.wl_known ? "" : ", integer unknown",
                          ce.td2_m, ce.zero_gate_m);
            }

            // Gap-bridge moot attributions (across-gap slip already absorbed
            // by the prune-and-reseed on the signal's return) and clean
            // adjudications (dN == 0 partners) share the code-event caller
            // semantic: suppress the mandate, touch nothing.
            for (const auto& [msat, sigs] : te.moot_signals) {
                code_clean_signals[msat].insert(sigs.begin(), sigs.end());
                for (const auto& s : sigs)
                    log->info("{}TurboEdit bridged moot adjudication {} {}: "
                              "across-gap slip already absorbed by the fresh "
                              "reseed — phase reset suppressed",
                              epoch_prefix, msat, s);
            }
            for (const auto& [csat, sigs] : te.clean_signals)
                code_clean_signals[csat].insert(sigs.begin(), sigs.end());
        }

        // Default reset path for everything TurboEdit did not handle.  LLI
        // resets (empty signal pair) always reset — a loss-of-lock is
        // authoritative; pair slips on a TurboEdit-handled satellite are
        // skipped to preserve its targeted per-signal repair, and a mandate
        // whose OWN signal the code-domain veto adjudicated clean is
        // skipped to preserve the (believed-unslipped) phase ambiguity.
        // The per-mandate-signal test matches CycleSlipManager's
        // signalFromAmbiguityKey rule exactly: a mandate from a pair
        // OUTSIDE the GF solve still resets its un-adjudicated signal, and
        // a combined-IF amb_key (suffix "L1C_L2W") never matches the
        // raw-signal clean set, so the IF ambiguity resets — identical to
        // the post-processing behavior.
        for (const auto& sr : slip_results) {
            const bool is_lli = sr.sig1.empty() || sr.sig2.empty();
            std::string sat = sr.satellite;
            if (sat.empty()) {
                const auto p = sr.amb_key.find('_');
                sat = (p != std::string::npos) ? sr.amb_key.substr(0, p) : sr.amb_key;
            }
            // Bridge-restored this epoch (snapshot+dN·λ in the pre-pass) — its
            // ambiguities are already correct; do not reset them.
            if (soft_lli_repaired.count(sat)) continue;
            if (!is_lli && turbo_handled_sats.count(sat)) continue;
            // WL-settle deferral pending: mandate suppressed (the holds keep
            // the affected phase out of the filter), GF/MW arcs preserved,
            // until the deferral resolves in a later epoch.
            if (!is_lli && te_pending.count(sat)) {
                log->info("{}MW mandate for {} suppressed: TurboEdit WL-settle "
                          "deferral pending (holds active)", epoch_prefix, sat);
                continue;
            }
            if (!is_lli) {
                const auto cit = code_clean_signals.find(sat);
                if (cit != code_clean_signals.end()) {
                    const auto p = sr.amb_key.find('_');
                    if (p != std::string::npos &&
                        cit->second.count(sr.amb_key.substr(p + 1)))
                        continue;  // mandate's own signal adjudicated clean: keep the phase
                }
            }
            state_manager_->resetAmbiguity(sr.amb_key, reset_std);
            // A receiver-declared loss of lock also invalidates the
            // TurboEdit GF arc levels for the sat: a slip baked into
            // prev_gf would echo into the next epoch's TD2 (sign-flipped)
            // exactly when MW flags the pair with the true integer, and
            // the joint solve would corrupt the just-reseeded ambiguity.
            // The MW EWMA is deliberately preserved across LLI
            // (observation-domain history; see the detector's LLI branch).
            //
            // EXCEPTION — soft-LLI bridge: for an AR-capable, non-pivot sat at
            // high rate, HOLD the ambiguities across the (short) LLI gap and
            // let the bridge integer-repair them at gap return.  That needs the
            // GF arc's pre-gap level to SURVIVE, so do NOT reset it here; the
            // hold snapshots the pre-gap values and marks the signals
            // held-continuous in the resolver.
            if (is_lli && turbo_edit_resolver_) {
                if (soft_lli_ && !isBssdPivot(sat) &&
                    soft_lli_->qualifies(sat, epoch_now_s)) {
                    soft_lli_->registerHold(sat, { sr.amb_key },
                                            *turbo_edit_resolver_, epoch_now_s,
                                            epoch_prefix);
                    // GF arc intentionally PRESERVED (no resetSatellite).
                } else {
                    turbo_edit_resolver_->resetSatellite(sat);
                }
            }
        }

        // Advance the soft-LLI measured-spacing marker for the high-rate gate.
        if (soft_lli_) soft_lli_->advanceEpoch(epoch_now_s);
    }

    // --- RTA-3: solution-generation id change -> GLOBAL datum break ---
    // The network producer stamps a per-run generation id on its products
    // (network.emit_generation_id); a CHANGE of a PRESENT id between epochs means
    // the producer RESTARTED and re-realized its whole datum — every satellite
    // phase-bias lump and code-bias level re-referenced at once, while the
    // restarted emitter's discontinuity counters re-seeded at 0 (a warm consumer
    // that stored counter=0 sees 0→0 = "no change", the exact hole this closes).
    // React with the union of the fine-grained reactions below, applied globally:
    // reset EVERY ambiguity (the noteSsrDiscontinuity→resetAmbiguity machinery,
    // whole-state scope) and re-inflate EVERY receiver code-bias state
    // (reinflateCodeBiasVariance).  Fires only when the id is present — an absent
    // id (live RTCM, pre-RTA-3 products) never reacts, so this is byte-identical
    // for every legacy stream.  First sighting records only (no reaction).
    {
        const auto gid = ssrStore().getGenerationId();
        if (gid) {
            if (last_ssr_generation_id_ != 0 && *gid != last_ssr_generation_id_) {
                log->warn("{}SSR solution-generation id changed ({} -> {}): producer restart / "
                          "global datum re-realization — resetting ALL network-corrected "
                          "ambiguities and re-inflating receiver code-bias states.",
                          epoch_prefix, last_ssr_generation_id_, *gid);
                const double reset_std = config_.getAmbiguityStd();
                std::vector<std::string> keys;   // copy: resetAmbiguity may touch the map
                keys.reserve(state_manager_->getAmbiguities().size());
                for (const auto& kv : state_manager_->getAmbiguities()) keys.push_back(kv.first);
                for (const auto& k : keys) state_manager_->resetAmbiguity(k, reset_std);
                const double cb_var =
                    config_.codeBiasReinflateStd() * config_.codeBiasReinflateStd();
                std::vector<std::string> cb_keys;
                cb_keys.reserve(state_manager_->getCodeBiasStates().size());
                for (const auto& kv : state_manager_->getCodeBiasStates())
                    cb_keys.push_back(kv.first);
                for (const auto& k : cb_keys)
                    state_manager_->reinflateCodeBiasVariance(k, cb_var);
            }
            last_ssr_generation_id_ = *gid;
        }
    }

    // --- RTA-10: producer epoch-status heartbeat -> one-shot transition log ---
    // The network producer's H heartbeat (network.emit_epoch_status_record) reaches
    // the store via the product-tail wiring (ApplicationRealtime), INCLUDING for
    // fully withheld epochs that deliver no product record — so this is where the
    // consumer learns "the producer is alive but withholding" vs "products stopped".
    // Edge-triggered on the STATE (ok/partial/withheld_*): the first sighting and
    // every CHANGE log exactly once; a steady state stays silent.  An absent
    // heartbeat (live RTCM streams / pre-RTA-10 products / toggle off) never logs.
    // Deeper integrity wiring — gating measurement usage or R-inflation on the
    // producer status — is deliberately out of scope here (RTF-9).
    {
        const auto pst = ssrStore().getProducerStatus();
        if (pst && (!last_producer_status_ || *last_producer_status_ != pst->state)) {
            const char* from = last_producer_status_
                                   ? NetworkProducerStatus::stateName(*last_producer_status_)
                                   : "(none)";
            const char* to = NetworkProducerStatus::stateName(pst->state);
            if (pst->state == NetworkProducerStatus::State::Ok)
                log->info("{}Network producer status: {} -> {} (heartbeat tow {:.1f}).",
                          epoch_prefix, from, to, pst->time.tow);
            else
                log->warn("{}Network producer status: {} -> {} (heartbeat tow {:.1f}, "
                          "sigma0 {:.2f}, withheld [{}], {} sat(s) published).",
                          epoch_prefix, from, to, pst->time.tow, pst->sigma0,
                          pst->withheld, pst->n_sats_published);
            last_producer_status_ = pst->state;
        }
    }

    // --- SSR phase-bias discontinuity -> ambiguity reset ---
    // An incremented IGS-SSR discontinuity counter means the satellite phase
    // bias for a signal jumped, breaking the integer property of that signal's
    // ambiguity; reset it before the KF update.  No-op when no SSR phase biases
    // are present (post-processing / corrections disabled).
    {
        const auto& ssr = ssrStore();
        const double reset_std = config_.getAmbiguityStd();
        const bool   bias_age_gate = config_.enableSsrBiasMaxAge();
        // A5 stale-hold: probe with the SAME effective limit the apply site uses
        // (hold_factor × max_age), so the flip detector fires exactly when the
        // applied ssr_bias_term actually steps — once per genuine outage, not per
        // provider-update cycle hovering near max_age.
        const double bias_max_age  = config_.getSsrBiasMaxAge()
                                   * config_.getSsrBiasStaleHoldFactor();
        for (const auto& obs : modeled_obs) {
            for (const auto& kv : obs.modeled) {
                const std::string& code = kv.first;
                if (code.empty() || code[0] != 'L') continue;       // phase only
                if (code.find('_') != std::string::npos) continue;   // raw signal only
                const std::string key = obs.satellite + "|" + code;
                if (auto pb = ssr.getPhaseBias(obs.satellite, code)) {
                    if (state_manager_->noteSsrDiscontinuity(
                            key, pb->discontinuity_counter)) {
                        state_manager_->resetAmbiguity(obs.satellite + "_" + code, reset_std);
                        log->info("{}SSR phase-bias discontinuity: reset {} {}",
                                  epoch_prefix, obs.satellite, code);
                    }
                }
                // A5 (P1-4): with the bias staleness gate ON, a phase bias that goes stale
                // (age-gated out) mid-arc and later returns steps the APPLIED ssr_bias_term
                // by a full bias — with NO producer disc-counter change — so the ambiguity
                // silently absorbs the jump and the next AR wrong-fixes.  Track whether the
                // bias is currently APPLIED (fresh within the gate); reset the ambiguity on
                // any fresh↔stale flip so the step re-floats instead of corrupting the fix.
                // Only when the gate is on ⇒ no flips otherwise ⇒ byte-identical.
                if (bias_age_gate) {
                    const bool applied_now =
                        static_cast<bool>(ssr.getPhaseBias(obs.satellite, code, obs.time, bias_max_age));
                    const auto it = ssr_phase_bias_applied_.find(key);
                    if (it != ssr_phase_bias_applied_.end() && it->second != applied_now) {
                        state_manager_->resetAmbiguity(obs.satellite + "_" + code, reset_std);
                        log->info("{}SSR phase-bias freshness flip ({} -> {}): reset {} {}",
                                  epoch_prefix, it->second ? "fresh" : "stale",
                                  applied_now ? "fresh" : "stale", obs.satellite, code);
                    }
                    ssr_phase_bias_applied_[key] = applied_now;
                }
            }
        }
    }

    // --- SSR code-bias datum re-reference -> receiver code-bias reinflation ---
    // The network producer references its satellite code biases to a per-(const,
    // signal) sum-to-zero datum; that level steps by a per-signal common mode
    // whenever the satellite set carrying a signal changes (rise/set).  The
    // receiver code-bias (DCB/IFB) state absorbs the step, but its near-frozen
    // random walk cannot follow a jump in one epoch.  An incremented code-bias
    // discontinuity counter signals the step — re-inflate the affected
    // constellation's receiver code-bias states so the next update re-absorbs it
    // cleanly, the code-domain analog of the phase-bias -> ambiguity reset above.
    // No-op without receiver code-bias states (estimate_ifb off) or without SSR
    // code-bias discontinuity counters (post-processing / RTCM streams carry
    // none — the wire format has no such field), so live RTCM is unaffected.
    if (config_.estimateIfb()) {
        const auto& ssr = ssrStore();
        const double cb_var =
            config_.codeBiasReinflateStd() * config_.codeBiasReinflateStd();
        const bool   bias_age_gate = config_.enableSsrBiasMaxAge();
        const double bias_max_age  = config_.getSsrBiasMaxAge()
                                   * config_.getSsrBiasStaleHoldFactor();
        std::set<std::string> reref_systems;   // system-name prefixes whose datum stepped
        for (const auto& obs : modeled_obs) {
            for (const auto& kv : obs.modeled) {
                const std::string& code = kv.first;
                if (code.empty() || code[0] != 'C') continue;          // code obs only
                if (code.find('_') != std::string::npos) continue;      // raw signal only
                if (auto c = ssr.getCodeBiasDiscontinuity(obs.satellite, code)) {
                    if (state_manager_->noteSsrDiscontinuity(
                            "CB|" + obs.satellite + "|" + code, *c)) {
                        reref_systems.insert(PppUtils::getSystemString(obs.system));
                        log->info("{}SSR code-bias discontinuity: {} {} (datum re-reference)",
                                  epoch_prefix, obs.satellite, code);
                    }
                }
                // A5 (P1-4) code analog: with the bias staleness gate ON, a code
                // bias that goes stale — or VANISHES from a wholesale-replacing
                // producer push (no disc counter travels for an absent signal) —
                // flips the applied ssr_bias_term by the full bias with no
                // counter change.  The near-frozen receiver code-bias state drags
                // that step for many epochs; treat a fresh↔absent flip exactly
                // like a datum re-reference and re-inflate.  Only when the gate
                // is on ⇒ no flips otherwise ⇒ byte-identical.
                if (bias_age_gate) {
                    const bool applied_now =
                        ssr.hasCodeBias(obs.satellite, code, obs.time, bias_max_age);
                    const std::string fkey = "CB|" + obs.satellite + "|" + code;
                    const auto it = ssr_code_bias_applied_.find(fkey);
                    if (it != ssr_code_bias_applied_.end() && it->second != applied_now) {
                        reref_systems.insert(PppUtils::getSystemString(obs.system));
                        log->info("{}SSR code-bias freshness flip ({} -> {}): {} {} "
                                  "(re-inflating rx code-bias states)",
                                  epoch_prefix, it->second ? "applied" : "absent",
                                  applied_now ? "applied" : "absent",
                                  obs.satellite, code);
                    }
                    ssr_code_bias_applied_[fkey] = applied_now;
                }
            }
        }
        // Re-inflate every receiver code-bias state of each re-referenced
        // constellation (keys "<sys>_DCB" / "<sys>_IFB<band>").  Constellation-
        // scoped (not per-band) for robustness: it reaches the reference-band
        // DCB — which the receiver clock CANNOT self-heal (the clock is shared
        // across constellations and across code+phase, so steering it injects a
        // transient into every other residual) — without a fragile
        // signal->band->state-key mapping; the handful of extra IFB/DCB states
        // it touches re-converge within an epoch or two from informative code
        // under the modest re-inflation variance.
        // T3g FINDING (per-satellite scoping is not achievable here): the receiver
        // code-bias states this reaction touches are per-(system,band) — there is NO
        // per-satellite receiver code-bias state to narrow to (a satellite's code bias
        // is on the SATELLITE side, already per-sat in the SSR product; the reaction
        // re-inflates the RECEIVER states that absorb the datum move).  The finest
        // in-principle scope is per-band, but a per-band narrowing can MISS the
        // reference-band DCB the shared clock cannot self-heal (the very failure this
        // constellation scope avoids), so it is deliberately NOT taken.  The card's
        // "per-satellite" ask is therefore structurally inapplicable to the receiver
        // reaction; the per-sat scoping that IS meaningful (the emitted SATELLITE
        // discontinuity) already lives on the producer side (markSatDiscontinuity).
        for (const auto& [key, idx] : state_manager_->getCodeBiasStates()) {
            (void)idx;
            for (const std::string& sys : reref_systems) {
                if (key.rfind(sys + "_", 0) == 0) {
                    state_manager_->reinflateCodeBiasVariance(key, cb_var);
                    break;
                }
            }
        }
    } else {
        // A5 part (b): estimate_ifb is off, so there are NO receiver code-bias states to
        // absorb a network code-bias datum re-reference.  For a STANDALONE consumer of a
        // network product that emits satellite code biases this is a silent misconfiguration
        // (the closed-loop consumer inherits estimate_ifb=true from the producer, so it is
        // safe).  Warn ONCE if the stream actually carries a network code-bias discontinuity
        // counter — turning the previously-silent no-op into a loud, actionable diagnostic.
        if (!warned_ssr_cb_no_ifb_) {
            const auto& ssr = ssrStore();
            for (const auto& obs : modeled_obs) {
                bool hit = false;
                for (const auto& kv : obs.modeled) {
                    const std::string& code = kv.first;
                    if (code.empty() || code[0] != 'C' || code.find('_') != std::string::npos) continue;
                    if (ssr.getCodeBiasDiscontinuity(obs.satellite, code)) { hit = true; break; }
                }
                if (hit) {
                    log->warn("{}Consuming NETWORK satellite code biases but estimation.estimate_ifb "
                              "is OFF — no receiver code-bias state exists to absorb a code-bias datum "
                              "re-reference; enable estimate_ifb for a standalone network-code-bias "
                              "consumer (the closed-loop consumer inherits it automatically).",
                              epoch_prefix);
                    warned_ssr_cb_no_ifb_ = true;
                    break;
                }
            }
        }
    }

    // --- SSR clock datum re-reference -> receiver-clock note + scoped ISB
    //     re-inflation (A2) ---
    // The network producer's clock datum steps every satellite clock of ONE
    // constellation by a COMMON constant when it re-references (a clock-pivot
    // handover, or a sum-to-zero membership change under
    // network.clock_disc_on_set_change).  For the REFERENCE constellation the
    // step is absorbed by the receiver clock (estimated fresh each epoch), so —
    // unlike the phase-bias step, which breaks the ambiguity integer property
    // and is reset above — no ambiguity reset is needed.  For a NON-REFERENCE
    // constellation, however, the step is DIFFERENTIAL vs the reference and
    // lands on the near-frozen ISB state, which drags it for many epochs (the
    // GPS-vs-GAL seam of the Jul-2026 closed-loop wire audit).  With
    // estimation.clock_disc_isb_reinflate_std > 0 the handler re-inflates
    // exactly the stepped constellation's ISB (scoped — never a blanket reset);
    // at the default 0 it stays note-only.  No-op without network clock disc
    // counters: RTCM-decoded clocks and producer runs with
    // network.emit_clock_disc off carry disc_counter=0, so noteSsrDiscontinuity
    // never fires and live RTCM is unaffected.
    {
        const auto& ssr = ssrStore();
        const double isb_std = config_.clockDiscIsbReinflateStd();
        std::set<std::string> reref_isb;   // ISB system names whose clock datum stepped
        std::set<std::string> present_isb; // all NON-reference ISBs observed this epoch
        bool reference_stepped = false;    // a REFERENCE-constellation (GPS) clock re-referenced
        for (const auto& obs : modeled_obs) {
            if (isb_std > 0.0) {
                const std::string sn = StateInitializer::systemToIsbName(obs.system);
                if (!sn.empty()) present_isb.insert(sn);
            }
            if (auto c = ssr.getClockDiscontinuity(obs.satellite)) {
                if (state_manager_->noteSsrDiscontinuity("SC|" + obs.satellite, *c)) {
                    log->info("{}SSR clock discontinuity: {} (datum re-reference)",
                              epoch_prefix, obs.satellite);
                    // A receiver clock estimated fresh each epoch (white noise)
                    // re-absorbs the common-mode part with no action; the ISB
                    // re-inflation below handles the differential part.
                    // T3g FINDING: the clock discontinuity is DETECTED per-satellite
                    // (getClockDiscontinuity(obs.satellite)), but the receiver-side
                    // REACTION cannot be per-satellite — there is ONE ISB per
                    // constellation and ONE shared receiver clock, no per-sat receiver
                    // clock/ISB state.  So "per-satellite scoping" of this reaction is
                    // structurally impossible; the constellation scope below is already
                    // the finest correct granularity.
                    if (isb_std > 0.0) {
                        const std::string sys_name =
                            StateInitializer::systemToIsbName(obs.system);
                        if (!sys_name.empty()) {
                            // Non-reference constellation stepped: the differential
                            // lands on its OWN ISB — scoped re-inflation of that one.
                            reref_isb.insert(sys_name);
                        } else {
                            // REFERENCE constellation (GPS) re-referenced: the receiver
                            // clock absorbs the common mode, but the step lands
                            // DIFFERENTIALLY on EVERY non-reference ISB (each is defined
                            // relative to the GPS datum).  systemToIsbName(GPS) is "", so
                            // scoping to it would re-inflate nothing and leave every
                            // Galileo/GLONASS/BeiDou ISB dragging for many epochs — the
                            // exact GPS-vs-non-ref seam this handler exists to cure.  Mark
                            // the reference step and re-inflate all present ISBs below.
                            reference_stepped = true;
                        }
                    }
                }
            }
        }
        if (reference_stepped) {
            reref_isb.insert(present_isb.begin(), present_isb.end());
            if (!present_isb.empty())
                log->info("{}Reference (GPS) clock re-reference — re-inflating {} "
                          "non-reference ISB variance(s) for the differential step.",
                          epoch_prefix, present_isb.size());
        }
        for (const std::string& sys : reref_isb)
            state_manager_->reinflateSystemTimeOffsetVariance(sys, isb_std * isb_std);
    }

    // --- KF measurement update (with outlier / pivot-handover rebuild loop) ---
    // The real-time path previously called update() once and discarded its
    // result, so a NeedsMeasurementRebuild return (post-fit outlier or a forced
    // pivot-handover request from the KF's coherent-disturbance detector) was
    // silently dropped: the measurement was never re-applied and the handover
    // was never forwarded to PivotManager, leaving the disturbed pivot in place
    // for every subsequent epoch.  This mirrors the core of
    // Application::runFilterUpdateLoop: rebuild on NeedsMeasurementRebuild,
    // forward each constellation's forced handover, carry excluded rows, and
    // peel one outlier per pass.  AR runs AFTER convergence on the committed
    // state (below).  Geometry relinearization is intentionally not replicated
    // here — it is a post-processing-only refinement.
    EpochMeasurement meas_update;
    bool update_applied = false;
    {
        // Pass budget + forced-pivot-handover forwarding are shared with the
        // post-processing driver via EpochUpdateSupport so the two paths can
        // never diverge on them again (the divergence that once left real-time
        // with no handover at all).
        const int max_passes =
            computeRebuildPassBudget(*state_manager_, config_, modeled_obs);

        std::set<std::string> carry_excluded;
        // Soft-LLI bridge: phase signals held across a receiver LLI gap pending
        // a bridge repair are excluded from the update so their still-slipped
        // ambiguities cannot contaminate shared states across the MW one-epoch
        // confirmation delay.  Empty unless turbo_edit_bridge_lli holds.
        if (soft_lli_) {
            auto held = soft_lli_->heldKeys();
            carry_excluded.insert(held.begin(), held.end());
            // The held ambiguity STATES must also survive the measurement
            // model's prune-on-first-miss for the duration of the outage —
            // deleting them would destroy the pre-gap state the bridge
            // repair restores.  Same source set as the exclusion above.
            meas_model_->setProtectedAmbiguityKeys(std::move(held));
        }
        size_t carry_bad = 0;
        for (int pass = 0; pass < max_passes; ++pass) {
            meas_update = meas_model_->computeMeasurementUpdate(
                modeled_obs, state_manager_->getStateVector(), carry_excluded);
            if (meas_update.innovations.size() == 0
                || !meas_model_->validateMeasurementUpdate(meas_update))
                break;

            meas_update.bad_residual_count = carry_bad;
            const auto result = kalman_filter_->update(meas_update);
            if (result != KalmanFilter::UpdateResult::NeedsMeasurementRebuild) {
                update_applied = true;   // clean commit (or post-reset commit)
                break;
            }

            // Rebuild requested: carry the running tally/exclusions, then forward
            // any forced pivot handovers (re-references the SD frame and
            // re-inflates contaminated non-pivot states on the next pass).
            carry_bad = meas_update.bad_residual_count;
            carry_excluded = meas_update.excluded_measurements;
            forwardKfPivotHandovers(
                *kalman_filter_, *meas_model_, mw_slip_detector_.get(),
                log_manager_, epoch_prefix, carry_excluded);

            // Re-inflate iono / sat-bias variances for any sat whose phase
            // ambiguities the KF fully reset this pass, so the reseed doesn't
            // bake a stale partition.  Shared with the post-processing driver;
            // previously absent on the real-time path (the known asymmetry).
            // No-op on a complete reset / when slip_state_reinflation is off.
            reinflateResetSatStates(*kalman_filter_, *state_manager_,
                                    *meas_model_, config_);

            // Clear MW / TurboEdit arc history for everything the KF just
            // reset, mirroring Application::runFilterUpdateLoop.  Per the
            // MwSlipDetector contract (resetSatellite docs), an externally
            // reset ambiguity must drop its arc: a stale pre-reset MW mean
            // or GF level otherwise flags against the reseeded state on the
            // next epoch, and the TurboEdit joint solve can convert that
            // echo into a confident wrong integer repair on top of a state
            // the KF already corrected.
            if (kalman_filter_->wasCompleteReset()) {
                for (const auto& obs : modeled_obs) {
                    mw_slip_detector_->resetSatellite(obs.satellite);
                    if (turbo_edit_resolver_)
                        turbo_edit_resolver_->resetSatellite(obs.satellite);
                }
            } else {
                std::set<std::string> covered_sats;
                for (const auto& [sat_id, sig] :
                     kalman_filter_->getLastResetSignals()) {
                    mw_slip_detector_->resetSatelliteSignal(sat_id, sig);
                    if (turbo_edit_resolver_)
                        turbo_edit_resolver_->resetSatelliteSignal(sat_id, sig);
                    covered_sats.insert(sat_id);
                }
                for (const auto& sat_id :
                     kalman_filter_->getLastResetSatellites()) {
                    if (covered_sats.count(sat_id)) continue;
                    mw_slip_detector_->resetSatellite(sat_id);
                    if (turbo_edit_resolver_)
                        turbo_edit_resolver_->resetSatellite(sat_id);
                }
            }
        }
    }

    // Soft-LLI bridge: roll the "last good" ambiguity snapshot from the
    // converged post-update state (the pre-gap anchor a later LLI hold repairs
    // against).  Only converged keys update it.  No-op unless turbo_edit_bridge_lli.
    if (soft_lli_)
        soft_lli_->recordSnapshot(*state_manager_,
                                  TimeUtils::continuousGpsSeconds(current_time));

    // --- Soft a-priori ZWD constraint ---
    // Anchors the BSSD [up, ZWD] near-null subspace each epoch so a common-mode
    // start error cannot run ZWD/height to ~1000 m.  One pseudo-obs row pulling
    // ZWD toward a climatological prior; applyPseudoObservation updates BOTH the
    // state and the covariance (shrinking the [up,ZWD] correlation), which is
    // what lets the height recover — a value-only clamp would not.  Reached only
    // after the startup gate, so it never fires during warm-up.
    {
        const auto& zc = config_.getZwdConstraint();
        if (zc.enabled && zc.sigma_m > 0.0) {
            const Eigen::VectorXd x = state_manager_->getStateVector();
            const size_t tropo_idx = state_manager_->getTropoIndex();
            if (tropo_idx < static_cast<size_t>(x.size())) {
                Eigen::MatrixXd H = Eigen::MatrixXd::Zero(1, x.size());
                H(0, static_cast<Eigen::Index>(tropo_idx)) = 1.0;
                Eigen::VectorXd z(1);
                z(0) = zc.apriori_m - x(static_cast<Eigen::Index>(tropo_idx));
                Eigen::MatrixXd R(1, 1);
                R(0, 0) = zc.sigma_m * zc.sigma_m;
                kalman_filter_->applyPseudoObservation(H, z, R);
            }
        }
    }

    // --- Network slant-ionosphere prior (ionosphere-weighted / BLUP) ---------
    // Replace the loose ionosphere_q random walk with an ionosphere-weighted user
    // model: the network producer evaluates its co-estimated SH-VTEC field at THIS
    // consumer's pierce points and pushes an undifferenced per-satellite slant-iono
    // value (metres on L1) into the injected store; here each per-sat slant-iono
    // state is pulled toward that value with a quality-scaled pseudo-observation
    // (Odijk-2012 / TK-2015 Eq 73 BLUP).  Placed AFTER the measurement update and
    // BEFORE AR, like the ZWD block, so the shrunk [iono, ambiguity] covariance
    // feeds the resolver.  The R block is common-mode-free per constellation
    // (diag(σ²)+σ_c²·11ᵀ): the all-ones block leaves the un-modellable receiver
    // iono-datum level FREE, so only the between-satellite structure is imposed —
    // matching the producer's own appendShIonoRows.  The per-LOS σ is the network's
    // representativeness σ (≈0.8–2.4 m at the default 5-TECU field σ) further scaled
    // by ionosphere_weighting.std_scale, so the prior can be tightened toward an
    // ionosphere-fixed model (std_scale<1) without re-tuning the producer's own
    // field σ (which is deliberately loose to avoid SH aliasing).  Frozen BSSD
    // pivots / zero-variance rows are excluded (their 0-value innovation would leak
    // through the off-diagonal block).  Only a same-epoch supply is trusted
    // (max_age = ½ the epoch spacing) so a satellite the producer did NOT re-supply
    // this epoch floats rather than inheriting a stale delay.  Default OFF ⇒ no rows
    // built, byte-identical.
    if (consume_network_iono_ && kalman_filter_ && state_manager_) {
        const auto& iw = config_.getIonosphereWeightingConfig();
        const SsrCorrectionStore& store = ssrStore();
        const Eigen::VectorXd x = state_manager_->getStateVector();
        const std::set<std::string>& frozen_pivots =
            state_manager_->getFrozenPivotSatellites();
        // Same-epoch supply only: reject a value the producer did not refresh this
        // epoch (its pierce point / STEC is stale), rather than trust it at the
        // tight per-LOS σ.  The producer pushes received_time == the epoch time.
        const double iono_max_age = (dt > 0.0) ? 0.5 * dt : 30.0;

        std::vector<int>    idx;    // iono-state column index
        std::vector<double> innov;  // network slant delay - current iono state (m)
        std::vector<double> var;    // per-row pseudo-obs variance (m^2)
        std::vector<System> cons;   // constellation, for the common-mode block

        for (const auto& obs : modeled_obs) {
            if (obs.elevation < iw.elevation_cutoff_deg) continue;
            const int iono_idx = state_manager_->getIonosphereIndex(obs.satellite);
            if (iono_idx < 0 || iono_idx >= static_cast<int>(x.size())) continue;
            if (frozen_pivots.count(obs.satellite) ||
                state_manager_->getStateVariance(iono_idx) <= 0.0) {
                continue;
            }
            const auto si = store.getSlantIono(obs.satellite, current_time, iono_max_age);
            if (!si || !(si->sigma_m > 0.0)) continue;
            double sg = iw.std_scale * si->sigma_m;   // consumer-side tightening knob
            // T3f: floor the tightened σ so std_scale < 1 cannot drive the supplied iono prior
            // below the per-LOS representativeness floor (an over-tight prior locks the slant
            // iono to an under-observed SH value).  Gated by the producer toggle ⇒ byte-identical off.
            if (config_.getNetworkConfig().iono_supply_formal_sigma)
                sg = std::max(iw.floor_m, sg);
            if (!(sg > 0.0)) continue;
            idx.push_back(iono_idx);
            innov.push_back(si->iono_m - x(iono_idx));
            var.push_back(sg * sg);
            cons.push_back(obs.system);
        }

        const int m = static_cast<int>(idx.size());
        if (m > 0) {
            const int n = static_cast<int>(x.size());
            Eigen::MatrixXd H = Eigen::MatrixXd::Zero(m, n);
            Eigen::VectorXd z(m);
            for (int k = 0; k < m; ++k) {
                H(k, idx[k]) = 1.0;
                z(k) = innov[k];
            }
            const double common_var = iw.common_mode_sigma_m * iw.common_mode_sigma_m;
            Eigen::MatrixXd R = Eigen::MatrixXd::Zero(m, m);
            for (int r = 0; r < m; ++r) {
                for (int c = 0; c < m; ++c)
                    if (cons[r] == cons[c]) R(r, c) = common_var;
                R(r, r) += var[r];
            }
            kalman_filter_->applyPseudoObservation(H, z, R);

            double sum_abs = 0.0;
            for (double e : innov) sum_abs += std::abs(e);
            log_manager_.getLogger("ionosphere")->debug(
                "Network iono prior: {} sats constrained, mean|innov|={:.3f} m", m,
                sum_abs / static_cast<double>(m));
        }
    }

    // --- Integer ambiguity resolution (MLAMBDA) ---
    // Mirrors Application::run: only after a successful float update, feed the
    // MW-resolved WL integers + fractions to the resolver; on a confident fix,
    // apply the fixed state/covariance and fix-and-hold.
    last_ar_ran_ = false;
    last_ar_result_ = AmbiguityResolution{};
    if (ambiguity_resolver_ && update_applied) {
        // Default: feed the MW-resolved WL integers (halves the LAMBDA search).
        // Closed-loop network-bias validation: suppress them so AR runs PURE
        // MLAMBDA on the KF float, which carries the consumed network phase bias
        // — the MW WL is in the separate .BIA frame and would re-inject a
        // conflicting WL datum (see setArIgnoreMwWl).
        const std::map<std::string, int>    no_wl_ambs;
        const std::map<std::string, double> no_wl_fracs;
        const auto wl_ambs  = ar_ignore_mw_wl_ ? no_wl_ambs
                                               : mw_slip_detector_->getResolvedWlAmbiguities();
        const auto wl_fracs = ar_ignore_mw_wl_ ? no_wl_fracs
                                               : mw_slip_detector_->getResolvedWlFractions();
        auto ar_result = ambiguity_resolver_->resolve(*state_manager_, modeled_obs, wl_ambs, wl_fracs);
        last_ar_ran_ = true;
        last_ar_result_ = ar_result;   // recorded even when not accepted (carries num_candidates)
        if (ar_result.success) {
            state_manager_->setStateVector(ar_result.fixed_state);
            state_manager_->setCovariance(ar_result.fixed_covariance);
            ambiguity_resolver_->holdAmbiguities(*state_manager_, ar_result);
            log->info("{}AR: fixed {}/{} ambs, ratio={:.2f}",
                      epoch_prefix, ar_result.num_fixed, ar_result.num_candidates, ar_result.ratio);
        }
    }

    // --- Extract posterior state ---
    state_manager_->updatePppStateFromVector(
        state_manager_->getStateVector(), ppp_state_, *state_manager_);

    ++epoch_count_;

    // RTA-13: periodic obs-code attribute-mismatch visibility (one line every
    // 300 processed epochs; silent when no mismatch was ever counted).
    if (epoch_count_ % 300 == 0) {
        logBiasAttrMismatchSummary("periodic");
        logOrbitEchoMismatchSummary("periodic");   // Card 7: same cadence, same silence rule
    }

    log->info("{}Epoch {}: pos=({:.3f},{:.3f},{:.3f}) clk={:.3f} zwd={:.4f} sats={}",
              epoch_prefix, epoch_count_,
              ppp_state_.position.x(), ppp_state_.position.y(), ppp_state_.position.z(),
              ppp_state_.clock_bias, ppp_state_.troposphere_zwd,
              modeled_obs.size());

    // --- Trace logging ---
    if (trace_logger_ && trace_logger_->isInitialized()) {
        // Trace files carry pure GPS time so timestamps line up exactly
        // with RINEX epoch records.  See Application::writeEpochTraces for
        // the rationale (no leap-second conversion in either direction).
        const GnssTime& gps_time = current_time;
        trace_logger_->logState(gps_time, *state_manager_);
        trace_logger_->logAmbiguities(gps_time, *state_manager_);
        trace_logger_->logIonosphere(gps_time, *state_manager_);
        trace_logger_->logSatBiases(gps_time, *state_manager_);
        trace_logger_->logReceiverCodeBiases(gps_time, *state_manager_);
        trace_logger_->logResiduals(gps_time, meas_update);

        // Satellite-status strip chart: real-time engine has no CSM-driven
        // MW slip set (slip detection lives in the post-processing
        // Application path), so the only slip signal here is the KF's
        // ambiguity-reset list from the latest update.  Raw epoch_obs is
        // forwarded so the logger can surface BelowMask rows for sats
        // dropped by ModelGeometry.  No-op for an unconfigured trace.
        const auto& reset_vec = kalman_filter_->getLastResetSatellites();
        const std::set<std::string> reset_sats(reset_vec.begin(), reset_vec.end());
        trace_logger_->logSatelliteStatus(
            gps_time, epoch_obs, modeled_obs, drop_reasons,
            meas_update, /*have_applied=*/update_applied,
            *state_manager_, /*slipped_sats=*/{}, reset_sats,
            config_.getElevationMaskDeg());
        trace_logger_->logDops(
            gps_time, modeled_obs, meas_update, /*have_applied=*/update_applied,
            *state_manager_);
    }

    return true;
}

// ---------------------------------------------------------------------------
// State accessors
// ---------------------------------------------------------------------------

const SsrCorrectionStore& RealTimeEngine::ssrStore() const {
    return ssr_store_override_ ? *ssr_store_override_
                              : SsrCorrectionStore::getInstance();
}

void RealTimeEngine::setSsrStore(const SsrCorrectionStore* store,
                                 bool consume_clock, bool consume_bias,
                                 SsrOrbitSource orbit_source) {
    // Clock/orbit overlay lives in PppModel (geometry + corrections); bias overlay
    // + the phase-bias-discontinuity reset hook live in MeasurementModel.  Route
    // each independently so the caller can, e.g., keep precise SP3/CLK for the
    // clock while consuming only the network phase bias (clean integer test).
    // Called after initialize(), so the models exist.
    ssr_store_override_ = consume_bias ? store : nullptr;   // discontinuity hook reads phase bias
    // Card 7: remember the CLOCK routing — the orbit-echo mismatch tallies land
    // on the store GnssCorrections reads clocks from, which the echo summary
    // must therefore read (it differs from the bias-routed override above
    // whenever consume_clock != consume_bias).
    clock_store_override_ = consume_clock ? store : nullptr;
    if (ppp_model_) {
        switch (orbit_source) {
        case SsrOrbitSource::FollowClock:
            // Legacy coupled routing (the default): orbit overlay rides with the
            // clock, exactly as the pre-split setSsrStore did.  The closed-loop
            // harness call site lands here — byte-identical.
            ppp_model_->setSsrStore(consume_clock ? store : nullptr);
            break;
        case SsrOrbitSource::External:
            // RTA-8(a): clocks from the injected store (per consume_clock), orbit
            // overlay pinned to the SINGLETON — the external RTS corrections
            // stream (ideally ingest-filtered to orbit_only, RTA-8(b)).
            ppp_model_->setSsrClockStore(consume_clock ? store : nullptr);
            ppp_model_->setSsrOrbitStore(nullptr);
            break;
        case SsrOrbitSource::Network:
            // RTA-8(a): a future orbit-bearing network product — orbit overlay
            // reads the injected store even when the clock is kept precise.
            ppp_model_->setSsrClockStore(consume_clock ? store : nullptr);
            ppp_model_->setSsrOrbitStore(store);
            break;
        }
    }
    if (meas_model_) meas_model_->setSsrStore(consume_bias  ? store : nullptr);
}

void RealTimeEngine::logBiasAttrMismatchSummary(const char* tag) {
    const auto counts = ssrStore().biasAttributeMismatchCounts();
    if (counts.empty()) return;   // clean run: never logs (byte-identical)
    std::uint64_t code_total = 0, phase_total = 0;
    for (const auto& [key, n] : counts) {
        if (!key.empty() && key.back() == 'L') phase_total += n;
        else                                   code_total  += n;
    }
    // Top offenders by count (deterministic tie-break: map key order).
    std::vector<std::pair<std::string, std::uint64_t>> top(counts.begin(), counts.end());
    std::stable_sort(top.begin(), top.end(),
                     [](const auto& a, const auto& b) { return a.second > b.second; });
    constexpr std::size_t kMaxListed = 8;
    std::string detail;
    for (std::size_t i = 0; i < top.size() && i < kMaxListed; ++i) {
        if (!detail.empty()) detail += ", ";
        detail += fmt::format("{}={}", top[i].first, top[i].second);
    }
    log_manager_.getLogger("rt_engine")->info(
        "[RT] SSR bias attribute mismatches ({}): tracked signal missing while a same-band "
        "sibling bias exists — code {} / phase {} lookups; top: {}{}",
        tag, code_total, phase_total, detail,
        top.size() > kMaxListed ? ", ..." : "");
}

void RealTimeEngine::logOrbitEchoMismatchSummary(const char* tag) {
    // Read the CLOCK-routed store: that is where GnssCorrections tallies the
    // echo mismatches (the engine-level ssrStore() is bias-routed and would
    // stay empty forever under a clock-on/bias-off consumer profile).
    const SsrCorrectionStore& clock_store =
        clock_store_override_ ? *clock_store_override_
                              : SsrCorrectionStore::getInstance();
    const auto counts = clock_store.orbitEchoMismatchCounts();
    if (counts.empty()) return;   // clean / pre-Card-7 run: never logs (byte-identical)
    std::uint64_t total = 0;
    for (const auto& [key, n] : counts) total += n;
    // Top offenders by count (deterministic tie-break: map key order).
    std::vector<std::pair<std::string, std::uint64_t>> top(counts.begin(), counts.end());
    std::stable_sort(top.begin(), top.end(),
                     [](const auto& a, const auto& b) { return a.second > b.second; });
    constexpr std::size_t kMaxListed = 8;
    std::string detail;
    for (std::size_t i = 0; i < top.size() && i < kMaxListed; ++i) {
        if (!detail.empty()) detail += ", ";
        detail += fmt::format("{}={}", top[i].first, top[i].second);
    }
    const double budget = clock_store.orbitEchoMaxSkew();
    log_manager_.getLogger("rt_engine")->info(
        "[RT] orbit/clock coherence-echo mismatches ({}): {} clock records whose orbit "
        "echo disagreed with the applied SSR orbit ({}); top: {}{}",
        tag, total,
        budget > 0.0
            ? fmt::format("withheld — budget {:.1f}s", budget)
            : "count-only — orbit_clock_max_skew_s unset, clocks still applied",
        detail, top.size() > kMaxListed ? ", ..." : "");
}

Eigen::Vector3d RealTimeEngine::getPosition() const {
    return ppp_state_.position;
}

double RealTimeEngine::getClockBias() const {
    return ppp_state_.clock_bias;
}

double RealTimeEngine::getTroposphereZwd() const {
    return ppp_state_.troposphere_zwd;
}

} // namespace GnssProcessor
