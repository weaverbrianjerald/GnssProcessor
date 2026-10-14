/**
 * @file PppSetup.cpp
 * @brief The setup stages of the PPP / PPP-RTK pipeline: the a-priori position, the
 *        processing components and their wiring.
 *
 * Application::runPpp (Application.cpp) calls them in this file's order after the input
 * phases.  initializePosition seeds the state's position from the RINEX header or the
 * station information file; each build or wire stage below is one block of the former
 * single Application::run, moved unchanged (card UOM step U7b; scripts/uom_u7_verify.py
 * proves the move).  The order is load-bearing: the constructors log, the IMU source
 * attaches to the KalmanFilter, PppModel::initializeState runs after the components exist,
 * and the trace logger opens last.
 */

#include "app/Application.hpp"
#include "aiding/ImuAidingSource.hpp"   // full type for aiding_manager_->imu() wiring
#include "math/Coordinates.hpp"
#include "math/TimeUtils.hpp"
#include "products/StationInfoFile.hpp"
#include "slip/MwSlipDetector.hpp"
#include <array>
#include <cmath>
#include <string>

namespace GnssProcessor {

// ---------------------------------------------------------------------------
// Phase 3: Seed filter position from RINEX header
// ---------------------------------------------------------------------------

bool Application::initializePosition() {
    const Eigen::Vector3d fallback_position(0.0, 0.0, Constants::EARTH_RADIUS);
    auto approx_pos = rinex_parser_->getHeader().approx_position;
    bool valid_pos = true;
    for (double val : approx_pos) {
        if (!std::isfinite(val) || std::abs(val) > 1e8) { valid_pos = false; break; }
    }

    initial_position_ = fallback_position;
    if (valid_pos && approx_pos != std::array<double,3>{0.0,0.0,0.0}) {
        Eigen::Vector3d marker_pos(approx_pos[0], approx_pos[1], approx_pos[2]);

        // Apply the antenna H/E/N offset (marker to ARP); the state position is the ARP.
        Eigen::Vector3d hen = rinex_parser_->getAntennaDeltaHen();
        if (hen.norm() > 1e-6 && marker_pos.norm() > 1e3) {
            initial_position_ = Coordinates::markerToArp(marker_pos, hen);

            log_manager_.getLogger("main_log")->info(
                "Applied antenna H/E/N offset ({:.4f}, {:.4f}, {:.4f}) m: marker ({:.3f}, {:.3f}, {:.3f}) -> ARP ({:.3f}, {:.3f}, {:.3f})",
                hen(0), hen(1), hen(2),
                marker_pos.x(), marker_pos.y(), marker_pos.z(),
                initial_position_.x(), initial_position_.y(), initial_position_.z());
        } else {
            initial_position_ = marker_pos;
            log_manager_.getLogger("main_log")->info(
                "Initialized position from RINEX marker (no/zero antenna offset): ({:.3f}, {:.3f}, {:.3f}) m",
                initial_position_.x(), initial_position_.y(), initial_position_.z());
        }

        Eigen::VectorXd sv = state_manager_->getStateVector();
        if (sv.size() >= static_cast<Eigen::Index>(state_manager_->getPositionIndex() + 3)) {
            sv.segment<3>(state_manager_->getPositionIndex()) = initial_position_;
            state_manager_->setStateVector(sv);
        } else {
            log_manager_.getLogger("main_log")->warn("State vector size {} too small to set position", sv.size());
            valid_pos = false;
        }
    }

    if (!valid_pos) {
        Eigen::VectorXd sv = state_manager_->getStateVector();
        if (sv.size() >= static_cast<Eigen::Index>(state_manager_->getPositionIndex() + 3)) {
            sv.segment<3>(state_manager_->getPositionIndex()) = fallback_position;
            state_manager_->setStateVector(sv);
            log_manager_.getLogger("main_log")->warn("Using fallback position: ({:.3f}, {:.3f}, {:.3f}) m",
                fallback_position.x(), fallback_position.y(), fallback_position.z());
        } else {
            log_manager_.getLogger("main_log")->error("State vector too small to set fallback position");
            return false;
        }
        initial_position_ = fallback_position;
    }

    // Station Information File a-priori: when enabled and the marker resolves,
    // seed the filter with the SIF ITRF coordinate propagated to the
    // first-observation epoch (velocity, seasonal and post-seismic terms).  It is
    // more accurate than the RINEX APPROX header and in the products' frame
    // (ITRF2020 / IGS20).  The occupation coordinate refers to the ground
    // reference point (the monument), like the RINEX APPROX position, while the
    // state vector is the ARP, so it is reduced marker -> ARP before it seeds the
    // state: through the header's ANTENNA: DELTA H/E/N as the APPROX seed above
    // and the network roster (network.station_marker_to_arp) are, or the SIF
    // antenna window's ARP_MINUS_GRP_ENU when the header carries no record
    // (reduceSifCoordinateToArp).  Used unreduced, it would seed GRAZ00AUT 1.964 m
    // below its antenna.  run() re-applies this seed after PppModel::initializeState,
    // which otherwise replaces it with the RINEX-derived position.
    //
    // The pre-override position confirms a 4-char prefix match (a marker that is
    // not an exact SIF ID / DOMES): find() accepts the candidate only within
    // SIF_MAX_PREFIX_DISTANCE_M of it.  Only a RINEX-derived position is passed;
    // the fallback placeholder above (0, 0, R_earth) carries no station
    // information, so with it find() applies its no-position rule (a bare
    // 4-char marker only).
    if (config_.enableStationInfo()) {
        if (const auto sif = config_.getStationInfo()) {
            const std::string marker = rinex_parser_->getHeader().marker_name;
            const bool rinex_seeded =
                valid_pos && approx_pos != std::array<double, 3>{0.0, 0.0, 0.0};
            const Eigen::Vector3d pre_override = initial_position_;
            if (const StationInfo* si = sif->find(marker, rinex_seeded ? &pre_override : nullptr)) {
                const GnssTime t0 = rinex_parser_->getTimeOfFirstObs();
                const double year = TimeUtils::mjdToDecimalYear(TimeUtils::gpsTimeToUtcMjd(t0));
                const ResolvedStationCoord rc = resolveStationCoordinate(*si, year);
                if (rc.valid && rc.ecef.allFinite() && rc.ecef.norm() > 1e6) {
                    if (rc.extrapolated)
                        log_manager_.getLogger("main_log")->warn(
                            "SIF a-priori for marker '{}' (matched {}) @ {:.3f} is EXTRAPOLATED "
                            "{:+.3f} yr outside its occupation segment [{:.3f}, {:.3f}); "
                            "antenna change in gap: {}; receiver change in gap: {}",
                            marker, si->id, year, rc.extrapolation_years,
                            rc.segment_start_year, rc.segment_end_year,
                            rc.antenna_change_in_gap ? "yes" : "no",
                            rc.receiver_change_in_gap ? "yes" : "no");
                    const Eigen::Vector3d rinex_hen = rinex_parser_->getAntennaDeltaHen();
                    const SifArpReduction red = reduceSifCoordinateToArp(
                        *si, rc.ecef, year,
                        rinex_parser_->hasAntennaDeltaHen() ? &rinex_hen : nullptr);
                    const char* ecc_source =
                        red.source == SifEccentricitySource::RinexHeader      ? "RINEX ANTENNA: DELTA H/E/N"
                      : red.source == SifEccentricitySource::SifAntennaWindow ? "SIF ARP_MINUS_GRP_ENU"
                                                                              : "none";
                    if (red.source == SifEccentricitySource::None)
                        log_manager_.getLogger("main_log")->warn(
                            "SIF a-priori for marker '{}' (matched {}): no RINEX ANTENNA: DELTA H/E/N "
                            "record and no SIF antenna window at {:.3f}; the monument coordinate "
                            "seeds the ARP state unreduced", marker, si->id, year);
                    if (red.source == SifEccentricitySource::RinexHeader && red.has_sif_eccentricity
                        && red.eccentricity_disagreement_m > SIF_ECCENTRICITY_TOLERANCE_M)
                        log_manager_.getLogger("main_log")->warn(
                            "SIF a-priori for marker '{}' (matched {}): RINEX ANTENNA: DELTA H/E/N "
                            "({:.4f}, {:.4f}, {:.4f}) m differs from the SIF ARP_MINUS_GRP "
                            "H/E/N ({:.4f}, {:.4f}, {:.4f}) m by {:.4f} m; the RINEX value is applied",
                            marker, si->id, red.delta_hen(0), red.delta_hen(1), red.delta_hen(2),
                            red.sif_delta_hen(0), red.sif_delta_hen(1), red.sif_delta_hen(2),
                            red.eccentricity_disagreement_m);
                    const Eigen::Vector3d prev = initial_position_;
                    initial_position_ = red.arp_ecef;
                    sif_apriori_seeded_ = true;
                    Eigen::VectorXd sv = state_manager_->getStateVector();
                    if (sv.size() >= static_cast<Eigen::Index>(state_manager_->getPositionIndex() + 3)) {
                        sv.segment<3>(state_manager_->getPositionIndex()) = initial_position_;
                        state_manager_->setStateVector(sv);
                    }
                    log_manager_.getLogger("main_log")->info(
                        "SIF a-priori for marker '{}' (matched {}) @ {:.3f}: ITRF {} monument "
                        "({:.3f}, {:.3f}, {:.3f}) m, H/E/N ({:.4f}, {:.4f}, {:.4f}) m from {} -> ARP "
                        "({:.3f}, {:.3f}, {:.3f}) m; shift vs RINEX/fallback = {:.3f} m",
                        marker, si->id, year, si->reference_frame,
                        rc.ecef.x(), rc.ecef.y(), rc.ecef.z(),
                        red.delta_hen(0), red.delta_hen(1), red.delta_hen(2), ecc_source,
                        initial_position_.x(), initial_position_.y(), initial_position_.z(),
                        (initial_position_ - prev).norm());
                }

                // Antenna / receiver metadata QC against the SIF active windows
                // at the first-obs epoch: flag DO_NOT_USE equipment and antenna
                // type disagreements between the RINEX header and the SIF.
                const auto trimmed = [](std::string v) {
                    const auto b = v.find_first_not_of(" \t");
                    if (b == std::string::npos) return std::string{};
                    const auto e = v.find_last_not_of(" \t");
                    return v.substr(b, e - b + 1);
                };
                for (const auto& aw : si->antennas) {
                    if (year < aw.start_year || year >= aw.end_year) continue;
                    if (aw.do_not_use)
                        log_manager_.getLogger("main_log")->warn(
                            "SIF: antenna window for '{}' at {:.3f} is flagged DO_NOT_USE "
                            "(type '{}')", marker, year, aw.type);
                    const std::string rinex_ant = trimmed(rinex_parser_->getHeader().antenna_type);
                    const std::string sif_ant   = trimmed(aw.type);
                    if (!rinex_ant.empty() && !sif_ant.empty() && rinex_ant != sif_ant)
                        log_manager_.getLogger("main_log")->warn(
                            "SIF: antenna type mismatch for '{}': RINEX='{}' vs SIF='{}'",
                            marker, rinex_ant, sif_ant);
                    break;  // one covering window
                }
                for (const auto& rw : si->receivers) {
                    if (year < rw.start_year || year >= rw.end_year) continue;
                    if (rw.do_not_use)
                        log_manager_.getLogger("main_log")->warn(
                            "SIF: receiver window for '{}' at {:.3f} is flagged DO_NOT_USE "
                            "(type '{}')", marker, year, rw.type);
                    break;
                }
            }
        }
    }

    return true;
}

// ---------------------------------------------------------------------------
// The processing components
// ---------------------------------------------------------------------------

void Application::buildPppComponents_() {
    ppp_model_       = std::make_unique<PppModel>(config_, *product_parser_, log_manager_, *rinex_parser_, *state_manager_);
    meas_model_      = std::make_unique<MeasurementModel>(*state_manager_, *ppp_model_, config_, log_manager_);
    kalman_filter_   = std::make_unique<KalmanFilter>(*state_manager_, config_, log_manager_, *ppp_model_);
    wls_estimator_   = std::make_unique<WlsEstimator>(config_, *state_manager_, log_manager_);
    cycle_slip_manager_  = std::make_unique<CycleSlipManager>(config_, log_manager_);
}

// ---------------------------------------------------------------------------
// The global ionosphere map (GIM)
// ---------------------------------------------------------------------------

void Application::buildGlobalIonosphereMap_() {
    // GIM construction.  Two independent consumers can request it:
    //   - estimation.ionosphere_weighting.enabled  → soft pseudo-obs on
    //     the per-satellite slant iono state (PPP-RTK).
    //   - model_corrections.enable_higher_order_ionosphere → STEC source
    //     for the I₂ + I₃ correction applied per-signal in PppModel.
    // Either flag triggers the IONEX parse; downstream code uses
    // gim_->isLoaded() to decide whether its work runs.
    const bool need_gim_for_weighting =
        config_.getIonosphereWeightingConfig().enabled;
    const bool need_gim_for_higher_order =
        config_.getModelCorrections().enable_higher_order_ionosphere;
    if (need_gim_for_weighting || need_gim_for_higher_order) {
        gim_ = std::make_unique<GlobalIonosphereMap>(
            config_.getProductPath("INX").string(), log_manager_);
        if (!gim_->isLoaded()) {
            log_manager_.getLogger("ionosphere")->warn(
                "GIM is required by the configuration ({}{}{}) but no usable "
                "IONEX file resolved — the dependent feature(s) will be skipped. "
                "Set data_download.download_gim (production mode) or place an "
                ".INX file under the date's products/ directory (internal mode). "
                "For bias consistency, source the GIM from the same analysis "
                "centre as the code-bias (OSB) product.",
                need_gim_for_weighting ? "ionosphere_weighting" : "",
                (need_gim_for_weighting && need_gim_for_higher_order) ? " + " : "",
                need_gim_for_higher_order ? "higher_order_ionosphere" : "");
        } else if (need_gim_for_higher_order) {
            // PppModel needs the pointer to evaluate the I₂+I₃ correction
            // each epoch.  The pointer is non-owning.  gim_ is declared after
            // ppp_model_ in Application.hpp, so it is destroyed first: during
            // Application teardown PppModel's pointer dangles until ppp_model_
            // is destroyed, and must not be dereferenced in that window.
            ppp_model_->setGim(gim_.get());
        }
    }
}

// ---------------------------------------------------------------------------
// The adaptive ionospheric process-noise monitor
// ---------------------------------------------------------------------------

void Application::buildIonoVariabilityMonitor_() {
    // Adaptive ionospheric process-noise monitor (opt-in via
    // estimation.adaptive_iono_process_noise.enable).  Constructed only
    // when enabled; when absent the iono process noise stays static.
    if (config_.getAdaptiveIonoQConfig().enabled) {
        iono_variability_monitor_ =
            std::make_unique<IonoVariabilityMonitor>(config_, log_manager_);
    }
}

// ---------------------------------------------------------------------------
// The cross-session normal-equation accumulator
// ---------------------------------------------------------------------------

void Application::buildNeqAccumulator_() {
    // Cross-session normal-equation accumulator (item #13B).  Post-processing
    // only (the config loader forces enable_neq_export off in real-time mode).
    // Accumulates Hᵀ R⁻¹ H and Hᵀ R⁻¹ Δz from each accepted update, eliminating
    // epoch-local states by Schur reduction, and exports .neq / .asc at the end
    // of the run.  It does not modify the filter state.
    if (config_.enableNeqExport()) {
        neq_accumulator_ = std::make_unique<NeqAccumulator>();
        log_manager_.getLogger("main_log")->info(
            "Normal-equation export enabled: accumulating cross-session NEQ "
            "(.neq/.asc will be written beside the navigation solution)");
    }
}

// ---------------------------------------------------------------------------
// The cycle-slip manager's inputs and the KF tiebreak
// ---------------------------------------------------------------------------

void Application::wireCycleSlipManager_() {
    // Provide BIA satellite biases to the MW and EWL detectors for integer rounding
    cycle_slip_manager_->setCorrections(&ppp_model_->getCorrections());

    // Adaptive MW arc-reset gap tolerance.  Without IMU gap bridging and with
    // a sampling interval coarser than 5 s, an inter-epoch gap longer than the
    // nominal cadence indicates an outage.  Tightening the gap tolerance to the
    // data rate keeps the MW detector from starting a new arc on a
    // slip-corrupted sample after a brief outage.  IMU mode keeps the default
    // 120 s, because INS propagation bridges gaps that long.
    //
    // The tolerance is 1.5× the nominal cadence, so a back-to-back epoch
    // (gap equal to the nominal interval) does not trip the strict
    // greater-than comparison in MwSlipDetector when floating-point error in
    // (epoch_s - last_epoch_s) gives values such as 30.000000001 against a
    // 30 s threshold.  A missed epoch gives a gap of about 2× the cadence
    // (60 s on a 30 s file), which still exceeds 1.5×.
    //
    // With processing.gnss_rate_s set (epoch decimation), the effective
    // inter-epoch spacing seen by MW is the decimated cadence, not the RINEX
    // interval.
    {
        const double rinex_interval = rinex_parser_->getHeader().interval.value_or(0.0);
        const double effective_interval = (config_.getGnssRate() > 0.0)
                                              ? config_.getGnssRate()
                                              : rinex_interval;
        if (!config_.enableImu() && effective_interval > 5.0) {
            if (auto* mw = cycle_slip_manager_->mwDetector())
                mw->setMaxGapSeconds(effective_interval * 1.5);
        }
        // TurboEdit needs the cadence regardless of IMU mode: it tightens its
        // own GF-arc gap tolerance to the cadence and logs the coarse-rate
        // advisory (GF trust itself is evidence-gated per arc; see the
        // TurboEditResolver header's "Rate adaptivity" note).
        cycle_slip_manager_->setSampleInterval(effective_interval);
    }

    // Provide MW data to the KF outlier handler for tiebreaking.
    kalman_filter_->setCycleSlipManager(cycle_slip_manager_.get());
}

// ---------------------------------------------------------------------------
// The module traces (MW, TurboEdit, wind-up, census, iono monitor)
// ---------------------------------------------------------------------------

void Application::enablePppTraceStreams_() {
    const auto& logging_cfg = config_.getLoggingConfig();
    if (logging_cfg.enable_mw_slip_trace) {
        cycle_slip_manager_->enableMwTracing(log_dir_);
    }
    if (logging_cfg.enable_turbo_edit_trace) {
        cycle_slip_manager_->enableTurboEditTracing(log_dir_);
    }
    if (logging_cfg.enable_windup_trace && ppp_model_) {
        ppp_model_->enableWindUpTracing(log_dir_);
    }
    // Wind-up census (estimation.trace_windup_census): the same read-only
    // step/gap counters the network teardown sums across its roster, armed here
    // for the single-receiver paths.  carry_gap_s = 0 disables the leveling-carry
    // bucketing, which has no meaning off the network producer.
    if (config_.traceWindUpCensus() && ppp_model_) {
        ppp_model_->enableWindUpCensus(config_.phaseWindUpMaxGapS(), 0.0);
    }
    if (iono_variability_monitor_ && config_.getAdaptiveIonoQConfig().trace) {
        iono_variability_monitor_->enableTracing(log_dir_);
    }
}

// ---------------------------------------------------------------------------
// The aiding sources
// ---------------------------------------------------------------------------

void Application::buildAidingSources_() {
    // Aiding sources (GNSS/INS coupling; the IMU is the first client, with
    // ZUPT/NHC/baro/... composing later via AidingSourceRegistry).  The
    // AidingSourceManager owns the config-selected set; ImuAidingSource wraps
    // the integrator (load + graceful GNSS-only fallback on failure, logged in
    // its ctor).  state_manager_ and kalman_filter_ are already constructed
    // above, so the IMU source can be wired immediately.
    aiding_manager_ = std::make_unique<AidingSourceManager>(config_, log_manager_);
    aiding_manager_->reserveAllStates(*state_manager_);   // every source stashes StateManager / reserves blocks
    if (auto* imu = aiding_manager_->imu()) {
        imu->attachFilter(*kalman_filter_);    // IMU-only: nominal-attitude owner handle (C_b_e_)
        if (imu->hasData() && imu->integrator()) {
            const char* mode_str = config_.tightlyCoupledImu()
                ? "tightly-coupled" : "loosely-coupled";
            log_manager_.getLogger("main_log")->info(
                "IMU integration enabled ({}): {} samples loaded",
                mode_str, imu->integrator()->sampleCount());
        }
    }
}

// ---------------------------------------------------------------------------
// The initial PPP state
// ---------------------------------------------------------------------------

void Application::initializePppState_() {
    ppp_model_->initializeState(ppp_state_);
    // initializeState() re-derives the position from a configured reference
    // position or the RINEX APPROX header, and the sync below would overwrite the
    // SIF a-priori that initializePosition() wrote to the state vector (as the
    // real-time engine's seedInitialState() notes for its own seed).  Re-apply it;
    // a configured reference position keeps precedence.  Inert unless
    // processing.enable_station_info resolved the marker.
    if (sif_apriori_seeded_) {
        if (config_.hasReferencePosition()) {
            log_manager_.getLogger("main_log")->info(
                "SIF a-priori not applied: the configured reference position seeds the state");
        } else {
            ppp_state_.position = initial_position_;
        }
    }
    state_manager_->syncPppStateToStateVector(ppp_state_);
}

// ---------------------------------------------------------------------------
// The ambiguity resolver
// ---------------------------------------------------------------------------

void Application::buildAmbiguityResolver_() {
    // Ambiguity resolution (MLAMBDA)
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
        ar_config.wl_only_ffr           = config_.getArWlOnlyFfr();
        ar_config.wl_only_min_arc_count = config_.getArWlOnlyMinArcCount();
        ar_config.wl_only_max_innov_cyc = config_.getArWlOnlyMaxInnovCyc();
        ar_config.min_arc_info          = config_.getArMinArcInfo();
        ar_config.nl_max_per_pair_frac  = config_.getArNlMaxPerPairFrac();
        ar_config.nl_iono_free_row      = config_.getArNlIonoFreeRow();
        ar_config.enable_partial_ar     = config_.getArEnablePartialAr();
        ar_config.ar_active_systems     = config_.getArActiveSystems();
        ar_config.state_ambiguity_basis = config_.getStateAmbiguityBasis();
        ambiguity_resolver_ = std::make_unique<AmbiguityResolver>(ar_config, log_manager_);
        log_manager_.getLogger("main_log")->info(
            "Ambiguity resolution enabled: mode={}, wl_only={}, partial_ar={}, nl_row={}, "
            "ratio=[{:.1f}, {:.1e}], elev_mask={:.1f} deg, "
            "min_epochs={}, max_pos_σ={:.3f} m",
            ar_config.ar_mode, ar_config.wl_only ? "on" : "off", ar_config.enable_partial_ar ? "on" : "off",
            ar_config.nl_iono_free_row ? "iono-free" : "narrowlane",
            ar_config.ratio_threshold, ar_config.ratio_upper_bound,
            ar_config.elevation_mask_ar, ar_config.min_epochs, ar_config.max_position_sigma_m);
    }
}

// ---------------------------------------------------------------------------
// The CSV trace logger
// ---------------------------------------------------------------------------

void Application::buildTraceLogger_(bool is_ppp_rtk) {
    trace_logger_ = std::make_unique<TraceLogger>(log_manager_, log_dir_, is_ppp_rtk, config_.getLoggingConfig());
    // Warn only when requested streams failed to open; with no enable_*_trace
    // toggle set, the logger is uninitialized by design.
    if (!trace_logger_->isInitialized() && trace_logger_->requestedStreamCount() > 0) {
        log_manager_.getLogger("main_log")->warn(
            "TraceLogger could not open CSV trace files in '{}'; continuing without CSV traces", log_dir_.string());
    }
}

} // namespace GnssProcessor
