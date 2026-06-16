/**
 * @file ZuptAidingSource.cpp
 * @brief Zero-velocity-update aiding source (row-contributing client).
 *
 * @see docs/aiding_source_abstraction_design.md
 */

#include "aiding/ZuptAidingSource.hpp"
#include "filter/StateManager.hpp"

namespace GnssProcessor {

ZuptAidingSource::ZuptAidingSource(const GnssConfig& config, LogManager& log_manager)
    : config_(config),
      log_manager_(log_manager),
      sigma_mps_(config.getAidingConfig().zupt_sigma_mps) {
    if (sigma_mps_ <= 0.0) {
        log_manager_.getLogger("aiding")->warn(
            "ZuptAidingSource: non-positive zupt_sigma_mps {:.4f}; clamping to 0.05 m/s.",
            sigma_mps_);
        sigma_mps_ = 0.05;
    }
}

std::optional<StateBlockSpec> ZuptAidingSource::reserveStates(StateManager& sm) {
    state_manager_ = &sm;   // stash for live velocity-index reads (ZUPT owns no states)
    return std::nullopt;
}

MeasurementRows ZuptAidingSource::buildZuptRows(const Eigen::VectorXd& state,
                                                std::size_t vel_idx,
                                                std::size_t state_size,
                                                double sigma_mps) {
    MeasurementRows rows;
    if (vel_idx + 3 > state_size)
        return rows;  // defensive: velocity block must be within the state.

    const double variance = sigma_mps * sigma_mps;
    static const char* kAxisLabel[3] = { "ZUPT_vx", "ZUPT_vy", "ZUPT_vz" };

    rows.reserve(3);
    for (int axis = 0; axis < 3; ++axis) {
        const Eigen::Index vi = static_cast<Eigen::Index>(vel_idx + static_cast<std::size_t>(axis));
        AidingMeasurementRow row;
        row.jacobian = Eigen::VectorXd::Zero(static_cast<Eigen::Index>(state_size));
        row.jacobian(vi) = 1.0;          // h(x) = v_axis
        row.innovation = -state(vi);     // z - h(x) = 0 - v_axis
        row.variance = variance;
        row.label = kAxisLabel[axis];    // unique per epoch
        row.signal = "AID";
        rows.push_back(std::move(row));
    }
    return rows;
}

MeasurementRows ZuptAidingSource::contributeMeasurement(const AidingEpochContext& ctx) {
    if (!state_manager_ || !state_manager_->hasVelocity())
        return MeasurementRows{};  // no velocity states to constrain.
    return buildZuptRows(ctx.state, state_manager_->getVelocityIndex(),
                         ctx.state_size, sigma_mps_);
}

} // namespace GnssProcessor
