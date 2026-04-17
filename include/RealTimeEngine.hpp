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

#include "GnssConfig.hpp"
#include "LogManager.hpp"
#include "KalmanFilter.hpp"
#include "MeasurementModel.hpp"
#include "MwSlipDetector.hpp"
#include "PppModel.hpp"
#include "PppTypes.hpp"
#include "ProductParser.hpp"
#include "RinexParser.hpp"
#include "RtcmParser.hpp"
#include "StateManager.hpp"
#include "TraceLogger.hpp"
#include "WlsEstimator.hpp"
#include "TimeUtils.hpp"
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
     * @brief Feed a set of observations directly (e.g. from RTCM replay file).
     *
     * @param observations  Observations for one epoch.
     */
    void feedObservations(const std::vector<RinexObservation>& observations);

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
    std::unique_ptr<WlsEstimator>        wls_estimator_;
    std::unique_ptr<MwSlipDetector>      mw_slip_detector_;
    std::unique_ptr<TraceLogger>         trace_logger_;

    PppState ppp_state_{};
    Eigen::Vector3d initial_position_{ 0.0, 0.0, 0.0 };

    uint64_t  epoch_count_{ 0 };
    bool      first_epoch_{ true };
    double    prev_tow_{ 0.0 };
    GnssTime  prev_epoch_time_{ 0, 0.0 };

    // Observation buffer for epoch assembly from streaming frames.
    GnssTime                        pending_epoch_time_{ 0, 0.0 };
    std::vector<RinexObservation>   pending_obs_;
};

} // namespace GnssProcessor
