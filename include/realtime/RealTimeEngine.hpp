/**
 * @file RealTimeEngine.hpp
 * @brief Real-time GNSS processing engine.
 *
 * Owns the full processing pipeline (state manager, PPP model, Kalman filter,
 * WLS estimator, slip detectors) and processes epochs one-at-a-time as they
 * arrive from a live data stream.  The engine is transport-agnostic: the
 * Application layer feeds it complete RTCM frames which it decodes, converts
 * to RinexObservation/RtcmSsrCorrection and runs through the same PPP
 * algorithm used by post-processing.
 *
 * Lifetime:
 *   1. Construct with config + log manager.
 *   2. Call initialize() to set up products, filter, models.
 *   3. Call processRtcmFrame() each time a complete frame is available.
 *   4. Call processEpoch() when a full epoch of observations is buffered.
 *   5. Query latest state via getState() / getPosition().
 */
#pragma once

#include "config/GnssConfig.hpp"
#include "logging/LogManager.hpp"
#include "filter/KalmanFilter.hpp"
#include "aiding/AidingSourceManager.hpp"
#include "measurement/MeasurementModel.hpp"
#include "slip/MwSlipDetector.hpp"
#include "slip/TurboEditResolver.hpp"
#include "slip/SoftLliBridge.hpp"
#include "filter/AmbiguityResolver.hpp"
#include "model/PppModel.hpp"
#include "common/PppTypes.hpp"
#include "products/ProductParser.hpp"
#include "rinex/RinexParser.hpp"
#include "rtcm/RtcmParser.hpp"
#include "sbf/SubframeAssembler.hpp"
#include "filter/StateManager.hpp"
#include "logging/TraceLogger.hpp"
#include "math/WlsEstimator.hpp"
#include "math/TimeUtils.hpp"
#include <Eigen/Dense>
#include <memory>
#include <vector>

namespace GnssProcessor {

/**
 * @brief Self-contained real-time PPP processing engine.
 *
 * Encapsulates all processing components and exposes a simple frame-in /
 * state-out interface suitable for streaming operation.
 */
class RealTimeEngine {
public:
    /**
     * @brief Construct the engine.
     * @param config  Loaded GNSS configuration.
     * @param log_manager Application log manager.
     */
    RealTimeEngine(GnssConfig& config, LogManager& log_manager);

    /**
     * @brief Initialize products, filter and models.
     *
     * Must be called once before processing any data.  Loads static
     * products (ATX, BIA, broadcast nav) needed for real-time operation.
     *
     * @return true on success.
     */
    bool initialize();

    /**
     * @brief Feed a complete RTCM3 frame for decoding.
     *
     * Decoded observations are buffered internally.  When a new epoch
     * boundary is detected the previous epoch is automatically submitted
     * for processing via processEpoch().
     *
     * @param frame  Complete RTCM3 frame (header + payload + CRC).
     */
    void feedRtcmFrame(const std::vector<uint8_t>& frame);

    /**
     * @brief Feed a complete Septentrio SBF block for decoding.
     *
     * The SBF analogue of @c feedRtcmFrame: a MeasEpoch block carries a full
     * epoch of observations and is processed immediately; broadcast-ephemeris
     * blocks (GPSNav/GALNav/BDSNav/QZSNav/GLONav) are pushed into the live
     * ephemeris store.  SSR corrections still arrive via the RTCM corrections
     * stream — SBF carries only the rover observations and nav.
     *
     * @param block  Complete SBF block (Sync + CRC + ID + Length + body).
     */
    void feedSbfFrame(const std::vector<uint8_t>& block);

    /**
     * @brief Feed a set of observations directly (e.g. from RTCM replay file).
     *
     * @param observations  Observations for one epoch.
     */
    void feedObservations(const std::vector<RinexObservation>& observations);

    /**
     * @brief Feed one streaming IMU sample (real-time INS aiding).
     *
     * Buffers the sample into the IMU aiding source (if one is active); it is
     * consumed by the next epoch's @c predictAided over the (prev, current]
     * interval.  No-op when no IMU source is configured, so a GNSS-only stream
     * is unaffected.  The RT-IMU end-to-end path is wired but not yet validated
     * against live IMU data (the rooftop AsteRx-U is GNSS-only); see
     * docs/aiding_source_abstraction_design.md.
     *
     * @param sample Streaming aiding sample (kind == Imu).
     */
    void feedImuSample(const AidingSample& sample);

    /**
     * @brief Process any epoch still buffered for assembly.
     *
     * Epoch flushing in @c feedRtcmFrame is edge-triggered: a buffered epoch
     * is only submitted when the next epoch's first observation arrives.  On
     * stream stop / disconnect the final buffered epoch would otherwise be
     * dropped.  Call this once after the stream(s) end to process it.
     */
    void flush();

    /**
     * @brief Process a single epoch of observations through the PPP pipeline.
     *
     * Steps: WLS seeding -> KF predict -> cycle-slip detection ->
     * KF update -> optional relinearization -> state extraction.
     *
     * @param epoch_obs Observations for this epoch.
     * @return true if the epoch was processed successfully.
     */
    bool processEpoch(const std::vector<RinexObservation>& epoch_obs);

    /// Get the current receiver position (ECEF, meters).
    Eigen::Vector3d getPosition() const;

    /// Get the current receiver clock bias (meters).
    double getClockBias() const;

    /// Get the current ZWD estimate (meters).
    double getTroposphereZwd() const;

    /// Get the full PPP state.
    const PppState& getState() const { return ppp_state_; }

    /// Number of epochs processed so far.
    uint64_t epochCount() const { return epoch_count_; }

    /// True once the processing.max_duration_s data-time budget has been
    /// reached (latched).  The real-time poll loop checks this to end the
    /// session cleanly, mirroring how post-processing breaks its epoch loop.
    /// Always false when max_duration_s is 0 (the default, unlimited).
    bool maxDurationReached() const { return max_duration_reached_; }

private:
    /// Convert GPS-scale time to UTC-scale time.
    static GnssTime gpsToUtcTime(const GnssTime& gps_time);

    GnssConfig&  config_;
    LogManager&  log_manager_;

    // Processing components (same as Application, but owned here).
    std::unique_ptr<ProductParser>       product_parser_;
    std::unique_ptr<RinexParser>         rinex_parser_;
    std::unique_ptr<StateManager>        state_manager_;
    std::unique_ptr<PppModel>            ppp_model_;
    std::unique_ptr<MeasurementModel>    meas_model_;
    std::unique_ptr<KalmanFilter>        kalman_filter_;
    std::unique_ptr<AidingSourceManager> aiding_manager_;   //!< Aiding sources (IMU first client); shared predict path with Application.
    std::unique_ptr<WlsEstimator>        wls_estimator_;
    std::unique_ptr<MwSlipDetector>      mw_slip_detector_;
    /// TurboEdit MW+GF joint slip-repair resolver (estimation.cycle_slip.turbo_edit);
    /// nullptr unless the toggle is set.  Pairs the MW wide-lane integer with the
    /// geometry-free phase jump to repair dual-frequency slips per signal.
    std::unique_ptr<TurboEditResolver>   turbo_edit_resolver_;
    /// Soft-LLI gap-bridge orchestration (snapshot / hold / restore), shared
    /// with the post-processing CycleSlipManager so the two paths cannot
    /// diverge.  Inert unless estimation.cycle_slip.turbo_edit_bridge_lli.
    std::unique_ptr<SoftLliBridge>       soft_lli_;
    std::unique_ptr<AmbiguityResolver>   ambiguity_resolver_;
    std::unique_ptr<TraceLogger>         trace_logger_;

    /// Assembles broadcast ephemeris from raw SBF nav subframes (GPSRawCA/
    /// GALRawINAV/BDSRaw) — persistent across blocks, feeds LiveEphemerisStore.
    SubframeAssembler                    subframe_assembler_;

    PppState ppp_state_{};
    Eigen::Vector3d initial_position_{ 0.0, 0.0, 0.0 };

    uint64_t  epoch_count_{ 0 };
    bool      first_epoch_{ true };

    // processing.max_duration_s data-time cutoff (0 = unlimited).  The window
    // is anchored on the first FULLY-processed epoch (deferred warmup epochs
    // never reach the anchor site), so the budget covers real estimation time.
    bool      max_duration_anchored_{ false }; ///< True once the cutoff window has been anchored.
    double    max_duration_anchor_s_{ 0.0 };   ///< Continuous GPS seconds of the first processed epoch.
    bool      max_duration_reached_{ false };  ///< Latched once the elapsed data-time span hits the limit.

    // Corrections-ready startup gate (estimation.defer_until_corrections_ready).
    // While armed and unsatisfied, processEpoch defers ALL estimation (no
    // predict/update) until every modeled above-mask sat is broadcast+SSR.
    bool      estimation_started_{ false }; ///< Gate satisfied — estimation has begun (latched).
    uint64_t  warmup_epochs_{ 0 };          ///< Count of deferred epochs (throttled progress logging).
    GnssTime  warmup_start_time_{ 0, 0.0 }; ///< First deferred epoch time (optional timeout fallback).

    bool      apriori_seeded_from_pvt_{ false };  ///< Logged once when the a-priori is taken from PVTGeodetic.
    bool      apriori_seeded_from_station_{ false }; ///< Logged once when the a-priori is taken from an RTCM 1005/1006 station ARP.
    double    prev_tow_{ 0.0 };
    GnssTime  prev_epoch_time_{ 0, 0.0 };
    uint64_t  backlog_warn_ctr_{ 0 };          ///< Throttle counter for the obs-backlog warning (every Nth lagging epoch).

    // Observation buffer for epoch assembly from streaming frames.
    GnssTime                        pending_epoch_time_{ 0, 0.0 };
    std::vector<RinexObservation>   pending_obs_;
};

} // namespace GnssProcessor
