#pragma once

/**
 * @file AidingSourceManager.hpp
 * @brief Owns the config-selected aiding-source set — the @c CycleSlipManager
 *        analog for navigation aiding.
 *
 * Built once per process from @c AidingSourceRegistry::createEnabled and
 * consumed identically by @c Application::run (post-processing) and
 * @c RealTimeEngine::processEpoch (streaming) so the predict/update path cannot
 * diverge between the two pipelines.  Holds the polymorphic herd plus one
 * typed handle for the privileged source (the IMU), mirroring
 * @c CycleSlipManager's @c detectors_ + @c mw_ split.
 *
 * Step 0 exposes construction + the gap-policy query
 * (@c anySourceBridgesGaps).  The per-epoch fan-out (collectPredict /
 * applyTcJacobianInputs / collectMeasurement) lands with the touch-point
 * rewiring in later steps; see docs/aiding_source_abstraction_design.md.
 */

#include "aiding/IAidingSource.hpp"
#include <memory>
#include <vector>

namespace GnssProcessor {

class GnssConfig;        // forward decl — only a const ref is taken
class LogManager;        // forward decl — only a ref is taken (full type via IAidingSource.hpp)
class ImuAidingSource;   // forward decl — non-owning typed handle target
class MeasurementModel;  // forward decl — TC Jacobian sink (forwarded to the IMU source)
class StateManager;      // forward decl — passed through to each source's reserveStates

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
     * @brief Establish every source's state THROUGH StateManager.
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
     * The faithful replacement for the scattered @c enableImu() gap/dynamics
     * and MW arc-reset-tolerance queries: true iff some source's
     * @c bridgesGaps() is true (IMU yes; baro/ZUPT no).
     */
    bool anySourceBridgesGaps() const;

    /**
     * @brief Collect this epoch's predict contributions from every source.
     *
     * Calls each source's @c contributePredict and returns the VALID ones
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
     * Gathers @c contributeMeasurement from every NON-re-parameterizing source
     * (ZUPT/baro/...; the TC-IMU column source is skipped).  The rows are
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
     * which is a no-op unless tightly coupled with data.  Replaces the two
     * scattered @c setImuState sites in Application::run.
     *
     * @param model Measurement model to receive @c setImuState.
     */
    void applyTcJacobianInputs(MeasurementModel& model) const;

    /**
     * @brief Whether any active source forces the naive IEKF relinearization.
     *
     * The faithful replacement for the @c tightlyCoupledImu() IEKF-dispatch
     * hard-coding: true iff some source applies error-state nominal feedback
     * (the tightly-coupled IMU).
     */
    bool anySourceRequiresNaiveRelinearization() const;

private:
    /// Owns the config-selected sources, in run order (IMU first).
    std::vector<std::unique_ptr<IAidingSource>> sources_;

    /// Non-owning typed handle to the IMU source inside @c sources_ (or nullptr).
    ImuAidingSource* imu_{ nullptr };
};

} // namespace GnssProcessor
