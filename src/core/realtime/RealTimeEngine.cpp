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
#include "common/Constants.hpp"
#include "common/PppUtils.hpp"
#include <algorithm>
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
    // historical run can use precise products.
    const bool skip_precise = config_.realtimeUsesLiveOrbitClockSource();
    product_parser_->loadProducts(!skip_precise);

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

void RealTimeEngine::feedRtcmFrame(const std::vector<uint8_t>& frame) {
    // Decode the frame through the existing RtcmParser message API.
    // This is a temporary file-less parser invocation.
    RtcmParser parser;
    parser.parseFrame(frame);

    // Observations: buffer until a new epoch is detected, then submit the
    // previous epoch.  MSM4/MSM7 decode is a follow-up phase — until then
    // getObservations() is empty and this loop is a no-op.
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
        } else if (std::abs(obs.time.tow - pending_epoch_time_.tow) > 0.5) {
            // New epoch  process the buffered one.
            processEpoch(pending_obs_);
            pending_obs_.clear();
            pending_epoch_time_ = obs.time;
        }
        pending_obs_.push_back(obs);
    }

    // Push decoded SSR records into the shared SsrCorrectionStore, carrying the
    // real message header (SSR epoch + IOD-SSR) so staleness / IOD diagnostics
    // reflect the provider solution, not wall-clock receipt.  The orbit / clock
    // query paths in Geometry and GnssCorrections consult the store
    // transparently — no further wiring at the per-epoch level.
    auto& ssr_store = SsrCorrectionStore::getInstance();
    const SsrHeader& hdr = parser.getLastSsrHeader();
    for (const auto& r : parser.getSsrOrbits())         ssr_store.pushOrbit(r, hdr);
    for (const auto& r : parser.getSsrClocks())         ssr_store.pushClock(r, hdr);
    for (const auto& r : parser.getSsrCombined())       ssr_store.pushCombined(r, hdr);
    for (const auto& r : parser.getSsrCodeBiases())     ssr_store.pushCodeBias(r, hdr);
    for (const auto& r : parser.getSsrPhaseBiases())    ssr_store.pushPhaseBias(r, hdr);
    for (const auto& r : parser.getSsrUras())           ssr_store.pushUra(r, hdr);
    for (const auto& r : parser.getSsrHighRateClocks()) ssr_store.pushHighRateClock(r, hdr);

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
    if (n_ssr > 0) {
        log_manager_.getLogger("rt_engine")->trace(
            "RTCM {}: pushed {} SSR records (epoch wk{} tow{:.0f}, IOD-SSR {}) "
            "to store ({} sats cached)",
            parser.lastMessageType(), n_ssr, hdr.epoch_time.week,
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
            const size_t pidx = state_manager_->getPositionIndex();
            Eigen::VectorXd sv = state_manager_->getStateVector();
            if (sv.size() >= static_cast<Eigen::Index>(pidx + 3)) {
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
        processEpoch(pending_obs_);
        pending_obs_.clear();
    }
    draining_final_ = false;
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
            if ((backlog_warn_ctr_++ % 30) == 0)
                log->warn("{}obs backlog: epoch wk{} tow{:.1f} is {:.1f}s behind wall-clock GPS "
                          "(corrections will be fresher than these obs)",
                          epoch_prefix, current_time.week, current_time.tow, lag_s);
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

    // --- SSR phase-bias discontinuity -> ambiguity reset ---
    // An incremented IGS-SSR discontinuity counter means the satellite phase
    // bias for a signal jumped, breaking the integer property of that signal's
    // ambiguity; reset it before the KF update.  No-op when no SSR phase biases
    // are present (post-processing / corrections disabled).
    {
        const auto& ssr = ssrStore();
        const double reset_std = config_.getAmbiguityStd();
        for (const auto& obs : modeled_obs) {
            for (const auto& kv : obs.modeled) {
                const std::string& code = kv.first;
                if (code.empty() || code[0] != 'L') continue;       // phase only
                if (code.find('_') != std::string::npos) continue;   // raw signal only
                if (auto pb = ssr.getPhaseBias(obs.satellite, code)) {
                    if (state_manager_->noteSsrDiscontinuity(
                            obs.satellite + "|" + code, pb->discontinuity_counter)) {
                        state_manager_->resetAmbiguity(obs.satellite + "_" + code, reset_std);
                        log->info("{}SSR phase-bias discontinuity: reset {} {}",
                                  epoch_prefix, obs.satellite, code);
                    }
                }
            }
        }
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
                                 bool consume_clock, bool consume_bias) {
    // Clock/orbit overlay lives in PppModel (geometry + corrections); bias overlay
    // + the phase-bias-discontinuity reset hook live in MeasurementModel.  Route
    // each independently so the caller can, e.g., keep precise SP3/CLK for the
    // clock while consuming only the network phase bias (clean integer test).
    // Called after initialize(), so the models exist.
    ssr_store_override_ = consume_bias ? store : nullptr;   // discontinuity hook reads phase bias
    if (ppp_model_)  ppp_model_->setSsrStore(consume_clock ? store : nullptr);
    if (meas_model_) meas_model_->setSsrStore(consume_bias  ? store : nullptr);
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
