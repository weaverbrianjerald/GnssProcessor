/**
 * @file RealTimeEngineInit.cpp
 * @brief Construction-phase seams for RealTimeEngine::initialize().
 *
 * Runs exactly once, before any frame is decoded.  Ordering is a genuine
 * dependency chain, not a stylistic sequence: the product parsers feed the
 * StateManager's a-priori, which the processing components bind to, which
 * the resolver and the state seed then read.  seedInitialState in
 * particular MUST follow initProcessingComponents � PppModel::initializeState
 * would otherwise clobber the a-priori with ppp_state_'s zero position.
 */

#include "realtime/RealTimeEngine.hpp"
#include "aiding/ImuAidingSource.hpp"
#include "math/TimeUtils.hpp"
#include "products/ProductCache.hpp"
#include "products/SsrCorrectionStore.hpp"
#include "common/Constants.hpp"
#include <filesystem>

namespace GnssProcessor {

/**
 * @brief Applies the configured leap-second override (RTB-1(c)) and the two opt-in process-wide SSR store policies: the code-bias band fallback (RTA-13) and the orbit/clock skew budget.  Must run before any stream thread decodes a frame.
 *
 * Relocated verbatim from the pre-cut RealTimeEngine::initialize
 * (frozen lines 98-130); see scripts/rt_seam_verify.py.
 */
void RealTimeEngine::armSsrStoreOptions() {
    auto log = log_manager_.getLogger("rt_engine");

    // Apply the configured GPS-UTC leap-second count to the real-time
    // wall-clock-to-GPS conversion (nowContinuousGpsSeconds).  That conversion is
    // the host-clock anchor of the RTCM decode paths (cold-start week promotion,
    // GLONASS day resolution) and of the obs-lag measurement.  The default equals
    // the compiled TimeUtils::LEAP_SECONDS, so an unconfigured run is unchanged.
    // Set before any stream thread decodes a frame.  Leap seconds from navigation
    // data (LNAV ΔtLS) are not used yet (RTF-scale follow-on).  (RTB-1(c))
    TimeUtils::setLeapSecondsOverride(
        static_cast<double>(config_.getRealTimeConfig().leap_seconds));

    // Opt-in: arm the band-wise code-bias fallback on the process-wide store for
    // the live-stream path.  An injected store owned by Application::runRealTime
    // is armed there at creation.  Off by default; the store is untouched and
    // every lookup byte-identical.  (RTA-13)
    if (config_.ssrBiasBandFallback()) {
        SsrCorrectionStore::getInstance().setCodeBiasBandFallback(true);
        log->info("SSR code-bias band fallback ON (processing.ssr_bias_band_fallback): an "
                  "exact-attribute miss consumes the same-band sibling CODE bias (residual "
                  "attribute DCB absorbed by receiver code-bias states); PHASE never falls back.");
    }

    // Opt-in, same pattern: arm the orbit/clock coherence skew budget on the
    // process-wide store.  An injected store owned by Application::runRealTime is
    // armed there at creation.  The default 0 leaves the store untouched and clock
    // application byte-identical.
    if (const double skew = config_.getNetworkConsumerConfig().orbit_clock_max_skew_s;
        skew > 0.0) {
        SsrCorrectionStore::getInstance().setOrbitEchoMaxSkew(skew);
        log->info("Orbit/clock coherence budget ON "
                  "(processing.network_consumer.orbit_clock_max_skew_s = {:.1f}s): a "
                  "network clock whose orbit echo disagrees with the applied SSR orbit "
                  "is withheld per satellite (RTA-9 ladder then governs).", skew);
    }
}

/**
 * @brief ProductParser / RinexParser construction, receiver-antenna seeding and the product load + cache.  False if the cache load fails.
 *
 * Relocated verbatim from the pre-cut RealTimeEngine::initialize
 * (frozen lines 132-181); see scripts/rt_seam_verify.py.
 */
bool RealTimeEngine::initProducts() {
    auto log = log_manager_.getLogger("rt_engine");

    // ProductParser (loads static products: ATX, BIA, broadcast nav).  In a
    // live session orbits/clocks come from the broadcast ephemeris + IGS-SSR,
    // not precise SP3/CLK, so those are skipped below.
    product_parser_ = std::make_unique<ProductParser>(config_, log_manager_);

    // PppModel still depends on RinexParser for header metadata (antenna type,
    // approximate position).  In real-time mode the parser is constructed, and
    // parsing the observation file is optional: a configured file is parsed to
    // seed the header, otherwise defaults are used.
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
    // gate).  A RINEX header value (post-processing) always wins: this only
    // fills an empty field.  This is the only injection site;
    // Application::run() (post-processing) never calls setReceiverAntennaType.
    if (rinex_parser_->getReceiverAntennaType().empty()) {
        const std::string& ov = config_.getModelCorrections().receiver_antenna_type_override;
        if (!ov.empty()) {
            rinex_parser_->setReceiverAntennaType(ov);
            log->info("Seeded receiver antenna type from config override: '{}'", ov);
        }
    }

    // Load products.  Skip precise SP3/CLK when a live broadcast+SSR source
    // replaces them (SSR corrections enabled and/or a live socket primary);
    // a real-time *file* replay with no corrections still loads them so a
    // historical run can use precise products.  Orbit and clock are gated
    // independently, so a "precise (ultra-rapid) orbits + network clocks" config
    // loads SP3 but skips the precise CLK (RTB-4).  With use_precise_orbits=false
    // (default) both gates equal realtimeUsesLiveOrbitClockSource(), which is
    // byte-identical to a single gate.
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

    return true;
}

/**
 * @brief StateManager construction and the coarse a-priori position seed.
 *
 * Relocated verbatim from the pre-cut RealTimeEngine::initialize
 * (frozen lines 183-211); see scripts/rt_seam_verify.py.
 */
void RealTimeEngine::initStateManagerAndApriori() {
    auto log = log_manager_.getLogger("rt_engine");

    // State manager.
    constexpr size_t core_state_size = 5; // Pos(3) + Clk(1) + Tropo(1)
    state_manager_ = std::make_unique<StateManager>(core_state_size, log_manager_, config_);
    // Generation IDs are local to this manager's lifetime. A public initialize()
    // call must not pair a new manager with windows from the previous instance.
    amb_level_check_ = AmbLevelCheck{};
    amb_level_configured_ = false;
    amb_level_generations_.clear();
    amb_level_events_.clear();
    amb_level_epoch_checks_ = 0;
    // The innovation screen's exclusion episodes name keys of the previous manager.
    innovation_screen_ = InnovationScreen{};
    innovation_screen_configured_ = false;

    // Seed initial position.
    initial_position_ = Eigen::Vector3d(0.0, 0.0, Constants::EARTH_RADIUS);
    auto approx_pos = rinex_parser_->getHeader().approx_position;
    bool valid_pos = true;
    for (double val : approx_pos) {
        if (!std::isfinite(val) || std::abs(val) > 1e8) { valid_pos = false; break; }
    }
    if (valid_pos && approx_pos != std::array<double, 3>{0.0, 0.0, 0.0}) {
        // ARP, not the marker: the state vector (and the ATX PCO/PCV applied to it)
        // is antenna-referenced, while APPROX POSITION XYZ is the monument.  This
        // matters for the file-fed closed-loop consumer, whose RINEX header carries
        // a real eccentricity; a live SBF/RTCM stream has neither field, so
        // markerToArp is an exact no-op there.
        initial_position_ = rinex_parser_->getArpPositionXyz();
        const Eigen::Vector3d hen = rinex_parser_->getAntennaDeltaHen();
        if (hen.norm() > 1e-6)
            log->info("Seed position reduced marker -> ARP via ANTENNA: DELTA H/E/N "
                      "({:.4f}, {:.4f}, {:.4f}) m: ({:.3f}, {:.3f}, {:.3f}) m",
                      hen(0), hen(1), hen(2),
                      initial_position_.x(), initial_position_.y(), initial_position_.z());
    }
    // The seed is not written to the state here.  seedInitialState(), which runs
    // after initProcessingComponents(), calls PppModel::initializeState() and
    // syncPppStateToStateVector(); those would overwrite any earlier state-vector
    // write with ppp_state_'s zero position (a live SBF/RTCM stream carries no
    // RINEX APPROX POSITION), so seedInitialState() applies initial_position_ after
    // them.
}

/**
 * @brief PppModel / MeasurementModel / KalmanFilter / aiding / WLS / slip detectors, plus the three 'enabled but not wired into RT' warnings.
 *
 * Relocated verbatim from the pre-cut RealTimeEngine::initialize
 * (frozen lines 213-305); see scripts/rt_seam_verify.py.
 */
void RealTimeEngine::initProcessingComponents() {
    auto log = log_manager_.getLogger("rt_engine");

    // Processing components.
    ppp_model_       = std::make_unique<PppModel>(config_, *product_parser_, log_manager_, *rinex_parser_);
    meas_model_      = std::make_unique<MeasurementModel>(*state_manager_, *ppp_model_, config_, log_manager_);
    kalman_filter_   = std::make_unique<KalmanFilter>(*state_manager_, config_, log_manager_, *ppp_model_);

    // Aiding sources (GNSS/INS coupling).  Same manager type as Application::run,
    // so real-time and post-processing share one predict path (predictAided).
    // For a GNSS-only stream the set is empty and predictAided delegates to
    // predict(dt), byte-identical to a plain predict.  Real-time IMU samples
    // arrive via feedImuSample.
    aiding_manager_  = std::make_unique<AidingSourceManager>(config_, log_manager_);
    aiding_manager_->reserveAllStates(*state_manager_);   // every source stores the StateManager / reserves its blocks
    if (auto* imu = aiding_manager_->imu())
        imu->attachFilter(*kalman_filter_);    // IMU only: handle to the nominal-attitude owner (C_b_e_)

    wls_estimator_   = std::make_unique<WlsEstimator>(config_, *state_manager_, log_manager_);
    mw_slip_detector_  = std::make_unique<MwSlipDetector>(config_, log_manager_);

    // TurboEdit MW+GF joint slip repair (high-rate only).  The real-time path
    // streams at the receiver's native rate (typically 1 Hz), so the geometry-
    // free jump that splits a dual-frequency slip per signal is valid here.
    if (config_.getCycleSlipConfig().turbo_edit) {
        turbo_edit_resolver_ = std::make_unique<TurboEditResolver>(config_, log_manager_);
        // Tighten the GF-arc gap tolerance to 1.5× the nominal cadence, as
        // CycleSlipManager::setSampleInterval does on the post-processing side.
        // At the resolver's 120 s default, a 2–120 s stream outage (NTRIP/TCP
        // interruption) is bridged by the second difference: TD2 then mixes a
        // gap-length leg with 1 s legs and carries the uncancelled ionospheric
        // drift across the outage, which either mass-resets converged ambiguities
        // (IVI trip) or aliases into a false integer repair.  A live stream has
        // no RINEX INTERVAL header, so the configured rate is used, falling back
        // to 1 Hz (TurboEdit is designed for high-rate data only).
        const double rate_s = config_.getGnssRate() > 0.0 ? config_.getGnssRate() : 1.0;
        turbo_edit_resolver_->setMaxGapSeconds(rate_s * 1.5);
        log->info("Real-time TurboEdit MW+GF joint cycle-slip repair ENABLED "
                  "(GF-arc gap tolerance {:.1f} s).", rate_s * 1.5);

        // Soft-LLI gap bridge (shared with the post-processing path): hold an
        // AR-capable dual-frequency satellite's ambiguities across a short
        // receiver LLI gap and let the bridge integer-repair them instead of
        // resetting at once.  Inert unless turbo_edit_bridge_lli.  A live stream
        // has no interval header, so the declared rate is seeded; otherwise the
        // high-rate gate falls back to the measured epoch spacing.
        soft_lli_ = std::make_unique<SoftLliBridge>(config_, log_manager_);
        soft_lli_->setSampleInterval(rate_s);
        if (config_.getCycleSlipConfig().turbo_edit_bridge_lli)
            log->info("Real-time soft-LLI bridge ENABLED (hold + bridge-repair "
                      "across LLI gaps ≤ {:.1f} s).",
                      config_.getCycleSlipConfig().turbo_edit_max_gap_s);
    }

    // Higher-order ionosphere is not wired into the real-time path: the engine
    // takes its ionospheric content from RTCM SSR, not from an IONEX GIM, and no
    // SSR-to-STEC adapter for HigherOrderIonosphere exists.  The warning keeps a
    // globally enabled higher-order flag from running the first-order-only
    // pipeline without notice.
    if (config_.getModelCorrections().enable_higher_order_ionosphere) {
        log_manager_.getLogger("ionosphere")->warn(
            "Real-time mode: higher_order_ionosphere is enabled but unsupported "
            "in the streaming pipeline (no GIM source) — the correction will be "
            "skipped.  Disable the flag in real-time configs to avoid this warning.");
    }

    // GIM ionosphere weighting (PPP-RTK soft iono constraint) is post-processing
    // only: the pseudo-observation that pulls each per-satellite slant-iono state
    // toward the GIM lives in Application::run and is not wired into the
    // streaming path (the RTCM stream carries no IONEX/VTEC).  The warning keeps
    // an enabled flag from being a silent no-op.
    if (config_.getIonosphereWeightingConfig().enabled) {
        log_manager_.getLogger("ionosphere")->warn(
            "Real-time mode: ionosphere_weighting is enabled but unsupported in "
            "the streaming pipeline (no GIM source) — no iono constraint will be "
            "applied.  Disable the flag in real-time configs to avoid this warning.");
    }

    // Adaptive ionospheric process noise (IonoVariabilityMonitor) is constructed
    // and driven only in Application::run; the real-time engine never builds the
    // monitor, so the per-satellite iono process noise stays at
    // process_noise.ionosphere.  The warning keeps an enabled flag from being a
    // silent no-op.
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
}

/**
 * @brief Integer ambiguity resolution (MLAMBDA) setup; same configuration surface as the post-processing path.
 *
 * Relocated verbatim from the pre-cut RealTimeEngine::initialize
 * (frozen lines 307-331); see scripts/rt_seam_verify.py.
 */
void RealTimeEngine::initAmbiguityResolver() {
    auto log = log_manager_.getLogger("rt_engine");

    // Integer ambiguity resolution (MLAMBDA), with the same configuration surface
    // as the post-processing path (Application::run).  The WL feed comes from the
    // MW slip detector; the real-time path needs no CycleSlipManager.
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
        ar_config.wl_only_ffr            = config_.getArWlOnlyFfr();
        ar_config.wl_only_min_arc_count  = config_.getArWlOnlyMinArcCount();
        ar_config.wl_only_max_innov_cyc = config_.getArWlOnlyMaxInnovCyc();
        ar_config.nl_max_per_pair_frac  = config_.getArNlMaxPerPairFrac();
        ar_config.nl_iono_free_row      = config_.getArNlIonoFreeRow();
        ar_config.ar_active_systems     = config_.getArActiveSystems();
        ar_config.state_ambiguity_basis = config_.getStateAmbiguityBasis();
        ambiguity_resolver_ = std::make_unique<AmbiguityResolver>(ar_config, log_manager_);
        log->info("Real-time AR enabled: mode={}, wl_only={}, ratio>={:.1f}, elev_mask={:.1f} deg",
                  ar_config.ar_mode, ar_config.wl_only ? "on" : "off",
                  ar_config.ratio_threshold, ar_config.elevation_mask_ar);
    }
}

/**
 * @brief PppModel::initializeState plus the Earth-centred a-priori fallback a live stream needs (no RINEX APPROX POSITION).
 *
 * Relocated verbatim from the pre-cut RealTimeEngine::initialize
 * (frozen lines 333-347); see scripts/rt_seam_verify.py.
 */
void RealTimeEngine::seedInitialState() {
    auto log = log_manager_.getLogger("rt_engine");

    ppp_model_->initializeState(ppp_state_);
    // initializeState() resolves the a-priori from a configured reference
    // position or the RINEX APPROX POSITION; a live SBF/RTCM stream has neither,
    // so it leaves the position at zero.  Without a finite a-priori the first
    // computeModels() rejects every satellite on the rx_pos.norm() < 1e3 sanity
    // check, so WLS never runs and the filter cannot bootstrap.  Fall back to
    // the coarse seed initial_position_ set by initStateManagerAndApriori()
    // (Earth-centred unless the RINEX header gave a position); WLS refines it to
    // metres on the first epoch with enough satellites.
    if (ppp_state_.position.norm() < 1e3 || !ppp_state_.position.allFinite()) {
        ppp_state_.position = initial_position_;
        log->info("Seeded a-priori position from Earth-centred fallback "
                  "({:.1f}, {:.1f}, {:.1f}) m (no reference/RINEX position for the live stream)",
                  initial_position_.x(), initial_position_.y(), initial_position_.z());
    }
    state_manager_->syncPppStateToStateVector(ppp_state_);
}

/**
 * @brief TraceLogger, the SSR-consumer trace (L10) and the slip-detector traces.
 *
 * Relocated verbatim from the pre-cut RealTimeEngine::initialize
 * (frozen lines 349-373); see scripts/rt_seam_verify.py.
 */
void RealTimeEngine::initTraceLoggers() {
    auto log = log_manager_.getLogger("rt_engine");

    const bool is_ppp_rtk = (config_.getProcessingMode() == ProcessingMode::PPP_RTK);
    std::filesystem::path log_dir{"logs"};
    const auto& logging_cfg = config_.getLoggingConfig();
    trace_logger_ = std::make_unique<TraceLogger>(log_manager_, log_dir, is_ppp_rtk, logging_cfg);

    // Consumer-side SSR-application trace (network.trace_ssr_consumer, L10).  It
    // uses the same "logs" dir as the producer traces; both dirs are hard-coded,
    // so in the closed loop they coincide.
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
}

} // namespace GnssProcessor
