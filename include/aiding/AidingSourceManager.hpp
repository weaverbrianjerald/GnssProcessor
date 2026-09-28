#pragma once

/**
 * @file AidingSourceManager.hpp
 * @brief Owns the config-selected aiding-source set; the navigation-aiding
 *        counterpart of @c CycleSlipManager.
 *
 * Built once per process from @c AidingSourceRegistry::createEnabled.  Both
 * @c Application::run (post-processing) and @c RealTimeEngine::processEpoch
 * (streaming) take their aided predict from @c collectPredict, so the predict
 * path cannot diverge between the two pipelines.  The post-processing path also
 * uses @c applyTcJacobianInputs and @c collectMeasurement; the real-time engine
 * uses @c ingestSample.  Holds the polymorphic source list plus one typed handle for
 * the privileged source (the IMU), mirroring @c CycleSlipManager's
 * @c detectors_ + @c mw_ split.
 *
 * The gap-policy query @c anySourceBridgesGaps has no caller yet.  See
 * docs/aiding_source_abstraction_design.md.
 */

#include "aiding/IAidingSource.hpp"
#include <memory>
#include <vector>

namespace GnssProcessor {

class GnssConfig;        // forward decl: only a const ref is taken
class LogManager;        // forward decl: only a ref is taken (full type via IAidingSource.hpp)
class ImuAidingSource;   // forward decl: target of the non-owning typed handle
class MeasurementModel;  // forward decl: TC Jacobian sink (forwarded to the IMU source)
class StateManager;      // forward decl: passed through to each source's reserveStates

/**
 * @class AidingSourceManager
 * @brief Owns @c std::vector<std::unique_ptr<IAidingSource>> + a typed IMU handle.
 */
class AidingSourceManager {
public:
    /**
     * @brief Build the config-selected source set.
     * @param config      Processing configuration (getAidingConfig()).
     * @param log_manager Logging facility.
     */
    AidingSourceManager(const GnssConfig& config, LogManager& log_manager);

    /**
     * @brief Establish every source's state through StateManager.
     *
     * Calls @c reserveStates on each source once after the core state is laid
     * out (stashes the StateManager for live index resolution; reserves tier-2
     * blocks).  Must be called before per-epoch contribution collection.
     */
    void reserveAllStates(StateManager& sm);

    /**
     * @brief Forward a streaming sample to every source (real-time ingest).
     *
     * Each source consumes samples of its own kind and ignores the rest
     * (the IMU buffers @c AidingSourceKind::Imu samples).  No-op when empty.
     */
    void ingestSample(const AidingSample& sample);

    /// @brief Whether no aiding source is active (the GNSS-only fast path).
    bool empty() const { return sources_.empty(); }

    /// @brief Number of active aiding sources.
    std::size_t size() const { return sources_.size(); }

    /// @brief Non-owning typed handle to the IMU source (nullptr if none active).
    ImuAidingSource* imu() { return imu_; }
    const ImuAidingSource* imu() const { return imu_; }

    /**
     * @brief Whether any active source bridges full GNSS outages.
     *
     * Intended to replace the scattered @c enableImu() gap/dynamics and MW
     * arc-reset-tolerance queries: true iff some source's @c bridgesGaps() is
     * true (IMU true; ZUPT false).  No caller uses it yet; those queries still
     * read @c enableImu().
     */
    bool anySourceBridgesGaps() const;

    /**
     * @brief Collect this epoch's predict contributions from every source.
     *
     * Calls each source's @c contributePredict and returns the valid ones
     * (an invalid/absent contribution is dropped).  An empty result signals
     * @c KalmanFilter::predictAided to fall back to the time-only predict.
     *
     * @param ctx Per-epoch context (interval + current nominal state).
     * @return Valid predict contributions in source order (IMU first).
     */
    std::vector<PredictContribution> collectPredict(const AidingEpochContext& ctx);

    /**
     * @brief Collect this epoch's independent measurement rows from row sources.
     *
     * Gathers @c contributeMeasurement from every source that does not
     * re-parameterize the GNSS rows (ZUPT and the other row sources; the TC-IMU
     * column source is skipped).  The rows are
     * appended into the finalized EpochMeasurement by the caller via
     * @c MeasurementModel::appendAidingRow.
     *
     * @param ctx Per-epoch context (current state, state size, time).
     * @return Rows to append (empty if none).
     */
    MeasurementRows collectMeasurement(const AidingEpochContext& ctx);

    /**
     * @brief Feed tightly-coupled Jacobian inputs to the measurement model.
     *
     * Forwards to the IMU source (the only re-parameterizing source today),
     * which is a no-op unless tightly coupled with data.  The post-processing
     * path calls it from Application::runFilterUpdateLoop and
     * Application::runGeometryRelinearizationNaive.
     *
     * @param model Measurement model to receive @c setImuState.
     */
    void applyTcJacobianInputs(MeasurementModel& model) const;

    /**
     * @brief Whether any active source forces the naive IEKF relinearization.
     *
     * Replaces the @c tightlyCoupledImu() check in the IEKF dispatch: true iff
     * some source applies error-state nominal feedback (the tightly-coupled IMU).
     */
    bool anySourceRequiresNaiveRelinearization() const;

private:
    /// Owns the config-selected sources, in run order (IMU first).
    std::vector<std::unique_ptr<IAidingSource>> sources_;

    /// Non-owning typed handle to the IMU source inside @c sources_ (or nullptr).
    ImuAidingSource* imu_{ nullptr };
};

} // namespace GnssProcessor
