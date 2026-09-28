#pragma once

/**
 * @file ZuptAidingSource.hpp
 * @brief Zero-velocity update (ZUPT) aiding source: the second client of the
 *        aiding-source abstraction and the first source that contributes
 *        measurement rows.
 *
 * @c ImuAidingSource uses the state, predict and column seams; ZUPT uses the
 * row seam.  While the platform is stationary the true velocity is about 0,
 * so each velocity state gets an independent pseudo-measurement
 * @f$ z = 0,\; h(x) = v_i,\; \text{innovation} = -v_i @f$ with a small
 * variance.  The rows are appended to the finalized EpochMeasurement through
 * @c MeasurementModel::appendAidingRow and consumed by the KF update like any
 * other row, so neither KalmanFilter nor StateManager needs a change.
 *
 * Adding this source required only (1) this class, (2) the
 * @c AidingSourceKind::Zupt registry entry, (3) the @c estimation.aiding.zupt
 * config toggle, and (4) the CMake source entry; the estimator core is
 * unchanged.
 *
 * Scope: rows are emitted whenever velocity states exist, which is correct for
 * a static survey receiver (the validation case).  A variant gated on detected
 * stationarity, for kinematic data, is not implemented.  Row sources are
 * restricted to undifferenced PPP / loosely-coupled GNSS: the row append
 * throws when BSSD/DD is active (R_full populated).
 */

#include "aiding/IAidingSource.hpp"
#include "config/GnssConfig.hpp"
#include "logging/LogManager.hpp"
#include <optional>
#include <string>

namespace GnssProcessor {

class StateManager;  // forward decl: holds the velocity index (stashed via reserveStates)

/**
 * @class ZuptAidingSource
 * @brief Zero-velocity-update pseudo-measurement source (row-contributing).
 */
class ZuptAidingSource : public IAidingSource {
public:
    /**
     * @brief Construct from configuration (reads the ZUPT pseudo-measurement std).
     *
     * A non-positive std is replaced by 0.05 m/s with a warning.
     *
     * @param config      Global configuration (estimation.aiding.zupt_sigma_mps).
     * @param log_manager Logging facility.
     */
    ZuptAidingSource(const GnssConfig& config, LogManager& log_manager);

    // ---- identity --------------------------------------------------------
    AidingSourceKind kind() const override { return AidingSourceKind::Zupt; }
    std::string name() const override { return "ZUPT (zero-velocity update)"; }

    // ---- (a) State -------------------------------------------------------
    /// @brief Owns no states; stores the StateManager to read the velocity index each epoch.
    std::optional<StateBlockSpec> reserveStates(StateManager& sm) override;

    // ---- (c) Measurement -------------------------------------------------
    /**
     * @brief Emit per-axis zero-velocity pseudo-measurement rows.
     *
     * Returns three rows (vx/vy/vz) pinning the velocity states to zero, or
     * none when velocity states are absent or @c reserveStates has not been
     * called.
     */
    MeasurementRows contributeMeasurement(const AidingEpochContext& ctx) override;

    // ---- lifecycle -------------------------------------------------------
    /// @brief Always true: ZUPT is a constraint, not a sensor feed.
    bool hasData() const override { return true; }

private:
    // Test seam: ZuptAidingSourceTestAccess (tests/test_aiding_source.cpp)
    // forwards to the private row-building kernel so the pseudo-measurement
    // math is testable without a velocity-enabled StateManager.
    friend struct ZuptAidingSourceTestAccess;

    /**
     * @brief Build the three per-axis zero-velocity rows (pure kernel).
     *
     * Row @c i: jacobian unit at @c vel_idx+i (h = v_i), innovation @c -state(vel_idx+i)
     * (z - h = 0 - v_i), variance @c sigma_mps^2, label "ZUPT_v{x,y,z}", signal "AID".
     * Returns no rows when @c vel_idx+3 exceeds @c state_size.
     *
     * @param state      Current nominal state.
     * @param vel_idx    Index of the velocity block's first component.
     * @param state_size Full state dimension (jacobian length).
     * @param sigma_mps  Pseudo-measurement std (m/s).
     */
    static MeasurementRows buildZuptRows(const Eigen::VectorXd& state,
                                         std::size_t vel_idx,
                                         std::size_t state_size,
                                         double sigma_mps);

    const GnssConfig& config_;
    LogManager& log_manager_;
    double sigma_mps_{ 0.05 };               ///< Cached pseudo-measurement std (m/s).
    StateManager* state_manager_{ nullptr }; ///< Set in reserveStates; read for the velocity index each epoch (non-owning).
};

} // namespace GnssProcessor
