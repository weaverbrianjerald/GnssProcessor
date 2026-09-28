#pragma once

/**
 * @file MeasurementBuilder.hpp
 * @brief Strategy hierarchy that selects the measurement-differencing regime.
 *
 * The Kalman measurement assembly differs across processing modes in a few
 * well-defined places: PPP builds raw (undifferenced) rows; PPP-RTK adds a
 * between-satellite single-difference pre/post pass (pivot handover, pivot
 * signal-coverage map, pivot-covariance freeze, and the in-place S-transform);
 * baseline modes build double-differenced rows from two per-receiver inputs.
 *
 * @c DifferencingMode is the dispatch key: a @c MeasurementDifferencingStrategy
 * is selected once (by @c MeasurementBuilderFactory from @c differencingModeFor),
 * and @c MeasurementModel calls its polymorphic hooks instead of branching on
 * @c config_.enableBetweenSatelliteSd() / @c config_.isBaselineMode().
 *
 * The numeric building blocks (row assembly, pivot management, the transforms)
 * stay owned by @c MeasurementModel; the strategies are thin orchestrators that
 * call back into it (@c MeasurementModel declares them @c friend).  The
 * measurement numerics stay in one place, and the per-mode step selection sits
 * behind the strategy interface.
 */

#include "measurement/BssdTransform.hpp"  // DifferencingMode
#include "common/PppTypes.hpp"
#include "common/EpochMeasurement.hpp"
#include "config/GnssConfig.hpp"
#include "logging/LogManager.hpp"
#include <Eigen/Dense>
#include <memory>
#include <set>
#include <string>
#include <vector>

namespace GnssProcessor {

class MeasurementModel;  // forward declaration; strategies hold no MeasurementModel

/**
 * @brief Resolve the differencing regime implied by the configuration.
 *
 * Network producer (@c ProcessingMode::PPP_RTK_Network) ⇒ @c None, whatever the
 * SD toggle says; otherwise baseline modes ⇒ @c DoubleDifference; otherwise
 * between-satellite SD enabled ⇒ @c BetweenSatellite; otherwise @c None (plain PPP).
 *
 * @param config Processing configuration.
 * @return The differencing mode the measurement builder should use.
 */
DifferencingMode differencingModeFor(const GnssConfig& config);

/**
 * @class MeasurementDifferencingStrategy
 * @brief Polymorphic measurement-assembly regime (PPP / PPP-RTK SD / DD).
 *
 * The single-vector (PPP / PPP-RTK) build skeleton lives in
 * @c MeasurementModel::computeMeasurementUpdate and calls the three hook
 * methods (@c preparePivots, @c freezePivotCovariances, @c applyDifferencing).
 * The two-vector overload calls @c buildDoubleDifference, which the
 * @c DoubleDifference strategy implements by delegating to the model's DD row
 * assembly.
 *
 * Strategies are stateless; each hook receives the @c MeasurementModel to act
 * on so a single instance can serve the model for its whole lifetime.
 */
class MeasurementDifferencingStrategy {
public:
    virtual ~MeasurementDifferencingStrategy() = default;

    /// @brief The regime this strategy implements.
    virtual DifferencingMode mode() const = 0;

    /**
     * @brief Pre-pass before state creation: pivot handover + coverage map.
     *
     * Runs at the top of the single-vector build, after the pivot
     * signal-coverage map is cleared and before @c ensureRequiredStates, so no
     * ambiguity state is created for a signal the pivot does not track.
     * No-op for undifferenced PPP.
     *
     * @param model        The measurement model to act on.
     * @param observations Epoch observations.
     * @param epoch_prefix Log prefix.
     * @param epoch        Epoch time (used for first-pass vs rebuild detection).
     */
    virtual void preparePivots(MeasurementModel& model,
                               const std::vector<PppObservation>& observations,
                               const std::string& epoch_prefix,
                               const GnssTime& epoch) {
        (void)model; (void)observations; (void)epoch_prefix; (void)epoch;
    }

    /**
     * @brief Freeze pivot ambiguity / iono / sat-bias covariances.
     *
     * Runs after @c ensureRequiredStates, before row assembly.  No-op for PPP.
     *
     * @param model The measurement model to act on.
     */
    virtual void freezePivotCovariances(MeasurementModel& model) { (void)model; }

    /**
     * @brief Post-pass after row assembly: apply the in-place SD transform.
     *
     * Rewrites @p update to per-constellation single differences.  No-op for PPP.
     *
     * @param model        The measurement model to act on.
     * @param update       The assembled (undifferenced) measurement, rewritten in place.
     * @param epoch_prefix Log prefix.
     */
    virtual void applyDifferencing(MeasurementModel& model,
                                   EpochMeasurement& update,
                                   const std::string& epoch_prefix) {
        (void)model; (void)update; (void)epoch_prefix;
    }

    /**
     * @brief Build a double-differenced measurement from two receivers.
     *
     * Only the @c DoubleDifference strategy implements this; the others return
     * an empty measurement (their modes never receive the two-vector call).
     *
     * @param model        The measurement model to act on.
     * @param rov_modeled  Rover modeled observations.
     * @param ref_modeled  Reference modeled observations.
     * @param current_state Current filter state.
     * @param excluded     Row signatures to skip.
     * @return The DD measurement (empty for non-DD strategies).
     */
    virtual EpochMeasurement buildDoubleDifference(
        MeasurementModel& model,
        const std::vector<PppObservation>& rov_modeled,
        const std::vector<PppObservation>& ref_modeled,
        const Eigen::VectorXd& current_state,
        const std::set<std::string>& excluded) {
        (void)model; (void)rov_modeled; (void)ref_modeled;
        (void)current_state; (void)excluded;
        return EpochMeasurement{};
    }
};

/**
 * @class UndifferencedMeasurementBuilder
 * @brief Plain PPP: raw observables, no differencing (all hooks are no-ops).
 */
class UndifferencedMeasurementBuilder : public MeasurementDifferencingStrategy {
public:
    DifferencingMode mode() const override { return DifferencingMode::None; }
};

/**
 * @class BssdMeasurementBuilder
 * @brief PPP-RTK between-satellite single difference.
 *
 * Encapsulates the pivot handover, pivot signal-coverage map, pivot-covariance
 * freeze, and the in-place @c BssdTransform::apply S-transform.
 */
class BssdMeasurementBuilder : public MeasurementDifferencingStrategy {
public:
    DifferencingMode mode() const override { return DifferencingMode::BetweenSatellite; }

    void preparePivots(MeasurementModel& model,
                       const std::vector<PppObservation>& observations,
                       const std::string& epoch_prefix,
                       const GnssTime& epoch) override;

    void freezePivotCovariances(MeasurementModel& model) override;

    void applyDifferencing(MeasurementModel& model,
                           EpochMeasurement& update,
                           const std::string& epoch_prefix) override;
};

/**
 * @class DdMeasurementBuilder
 * @brief Baseline double difference (between-receiver ∘ between-satellite).
 *
 * The single-vector hooks are unused; the two-vector @c buildDoubleDifference
 * delegates to the model's DD row assembly.
 */
class DdMeasurementBuilder : public MeasurementDifferencingStrategy {
public:
    DifferencingMode mode() const override { return DifferencingMode::DoubleDifference; }

    EpochMeasurement buildDoubleDifference(
        MeasurementModel& model,
        const std::vector<PppObservation>& rov_modeled,
        const std::vector<PppObservation>& ref_modeled,
        const Eigen::VectorXd& current_state,
        const std::set<std::string>& excluded) override;
};

/**
 * @class MeasurementBuilderFactory
 * @brief Constructs the @c MeasurementDifferencingStrategy for a regime.
 */
class MeasurementBuilderFactory {
public:
    /**
     * @brief Build the strategy for a differencing mode.
     * @param mode        The differencing regime.
     * @param log_manager Logging facility (for the selection log line).
     * @return Owned strategy (never nullptr; falls back to undifferenced).
     */
    static std::unique_ptr<MeasurementDifferencingStrategy> create(
        DifferencingMode mode, LogManager& log_manager);
};

} // namespace GnssProcessor
