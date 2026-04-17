
#pragma once

/**
 * @file BssdTransform.hpp
 * @brief Between-satellite single-difference S-transformation of EpochMeasurement.
 *
 * BssdTransform applies the differencing operator D to an undifferenced
 * EpochMeasurement, producing single-difference innovations, H-matrix,
 * and noise covariance.  It handles both combined (IF) and uncombined
 * (PPP-RTK) modes with different pivot selection strategies.
 *
 * Extracted from MeasurementModel to isolate the ~280-line S-transformation
 * from the per-row Jacobian assembly and state management.
 */

#include "GnssConfig.hpp"
#include "StateManager.hpp"
#include "BssdPivotManager.hpp"
#include "LogManager.hpp"
#include "PppTypes.hpp"
#include <string>
#include <vector>

namespace GnssProcessor {

/**
 * @class BssdTransform
 * @brief Applies the BSSD S-transformation to an EpochMeasurement.
 */
class BssdTransform {
public:
    BssdTransform(const GnssConfig& config,
                  const StateManager& state_manager,
                  const BssdPivotManager& pivot_manager,
                  LogManager& log_manager);

    /**
     * @brief Apply between-satellite single-difference transformation.
     *
     * Combined (IF) mode:
     *   Pivot: one per (constellation, signal_type), highest elevation.
     *   Columns zeroed: receiver clock, phase clock, ISB.
     *
     * Uncombined PPP-RTK mode:
     *   Pivot: reads the sticky pivot from BssdPivotManager.
     *   Columns zeroed: receiver clock, phase clock, ISB, receiver DCB/IFB,
     *   reference satellite ionosphere.
     *
     * In both modes reference ambiguity columns are zeroed and the
     * differencing operator D is applied to z, H, and R.
     *
     * @param update         EpochMeasurement to transform (modified in place).
     * @param epoch_prefix   Log prefix string.
     */
    void apply(EpochMeasurement& update, const std::string& epoch_prefix);

private:
    const GnssConfig& config_;
    const StateManager& state_manager_;
    const BssdPivotManager& pivot_manager_;
    LogManager& log_manager_;
};

} // namespace GnssProcessor
