#pragma once

/**
 * @file ZuptAidingSource.hpp
 * @brief Zero-velocity update (ZUPT) aiding source — second client of the
 *        aiding-source abstraction, and the first MEASUREMENT-ROW source.
 *
 * Where @c ImuAidingSource exercises the state + predict + column seams, ZUPT
 * exercises the row seam: while the platform is stationary, true velocity is
 * ~0, so each velocity state gets an independent pseudo-measurement
 * @f$ z = 0,\; h(x) = v_i,\; \text{innovation} = -v_i @f$ with a small
 * variance.  The rows are appended into the finalized EpochMeasurement via
 * @c MeasurementModel::appendAidingRow and consumed by the KF update like any
 * other row — no KalmanFilter or StateManager change is needed, which is the
 * whole point of the abstraction.
 *
 * Composition proof: implementing this required only (1) this class, (2) the
 * @c AidingSourceKind::Zupt registry entry, (3) the @c estimation.aiding.zupt
 * config toggle, and (4) the CMake source entry — no estimator-core surgery.
 *
 * Scope: emits rows whenever velocity states exist (a static survey receiver
 * is genuinely zero-velocity, the validation case).  A motion-gated /
 * stationarity-detected variant for kinematic data is a later refinement.
 * Restricted to undifferenced PPP / loosely-coupled GNSS for now (the row
 * append hard-errors under active BSSD/DD R_full).
 */

#include "aiding/IAidingSource.hpp"
#include "config/GnssConfig.hpp"
#include "logging/LogManager.hpp"
#include <optional>
#include <string>

namespace GnssProcessor {

class StateManager;  // forward decl — velocity index host (stashed via reserveStates)

/**
 * @class ZuptAidingSource
 * @brief Zero-velocity-update pseudo-measurement source (row-contributing).
 */
class ZuptAidingSource : public IAidingSource {
public:
    /**
     * @brief Construct from configuration (reads the ZUPT pseudo-measurement std).
     * @param config      Global configuration (estimation.aiding.zupt_sigma_mps).
     * @param log_manager Logging facility.
     */
    ZuptAidingSource(const GnssConfig& config, LogManager& log_manager);

    // ---- identity --------------------------------------------------------
    AidingSourceKind kind() const override { return AidingSourceKind::Zupt; }
    std::string name() const override { return "ZUPT (zero-velocity update)"; }

    // ---- (a) STATE -------------------------------------------------------
    /// @brief Owns no states; stashes StateManager for live velocity-index reads.
    std::optional<StateBlockSpec> reserveStates(StateManager& sm) override;

    // ---- (c) MEASUREMENT -------------------------------------------------
    /**
     * @brief Emit per-axis zero-velocity pseudo-measurement rows.
     *
     * Returns three rows (vx/vy/vz) pinning the velocity states to zero, or
     * empty when velocity states are absent or the source is not yet wired.
     */
    MeasurementRows contributeMeasurement(const AidingEpochContext& ctx) override;

    // ---- lifecycle -------------------------------------------------------
    /// @brief Always available — ZUPT is a constraint, not a sensor feed.
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
     * (z - h = 0 - v_i), variance @c sigma_mps^2, label "ZUPT_v{x,y,z}".
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
    StateManager* state_manager_{ nullptr }; ///< Stashed in reserveStates for live velocity-index reads (non-owning).
};

} // namespace GnssProcessor
