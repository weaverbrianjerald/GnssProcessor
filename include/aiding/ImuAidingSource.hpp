#pragma once

/**
 * @file ImuAidingSource.hpp
 * @brief IMU as the first client of the aiding-source abstraction.
 *
 * @c ImuAidingSource WRAPS (does not replace) the existing @c ImuIntegrator,
 * exposing the IMU as an @c IAidingSource so the four legacy IMU touch-points
 * (StateManager carve-out, KalmanFilter::predictImu, the MeasurementModel TC
 * Jacobian branch, and Application::run construction) collapse onto one
 * polymorphic seam.  It is the privileged source the @c AidingSourceManager
 * recovers via @c asImu() (the no-RTTI typed-handle pattern
 * @c CycleSlipManager uses for its MW detector).
 *
 * State-vector discipline: the IMU error states (attitude / accel-bias /
 * gyro-bias) live in StateManager's existing core carve-out; @c reserveStates
 * only DESCRIBES those offsets (no allocation).  The nominal body-to-ECEF
 * attitude (C_b_e_) is NOT owned here — it stays in its single owner (the
 * KalmanFilter), which this source reads/writes through the @c attachFilter
 * handle.  StateManager index resolution is done LIVE each epoch (no cached
 * indices), per the design.
 *
 * Migration staging (docs/aiding_source_abstraction_design.md, "IMU migration
 * plan"): the contributions implemented here are not yet wired into the live
 * epoch loop — predictImu and the existing TC branch remain the live paths
 * until Step 3 — so the IMU stays numerically inert.
 */

#include "aiding/IAidingSource.hpp"
#include "filter/ImuIntegrator.hpp"   // ImuIntegrator + ImuPrediction
#include "config/GnssConfig.hpp"
#include "logging/LogManager.hpp"
#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace GnssProcessor {

class StateManager;     // forward decl — index host (stashed via reserveStates)
class KalmanFilter;     // forward decl — nominal-attitude owner (handle via attachFilter)
class MeasurementModel; // forward decl — TC Jacobian sink (applyTcJacobianInputs)

/**
 * @class ImuAidingSource
 * @brief Inertial aiding source wrapping @c ImuIntegrator.
 */
class ImuAidingSource : public IAidingSource {
public:
    /**
     * @brief Construct and wrap an @c ImuIntegrator.
     *
     * Mirrors the legacy Application::run wiring: builds the integrator and,
     * if an IMU file is configured, loads it.  A load failure leaves
     * @c hasData()==false — the existing graceful GNSS-only fallback.
     *
     * @param config      Global configuration (IMU noise params, coupling mode).
     * @param log_manager Logging facility.
     */
    ImuAidingSource(const GnssConfig& config, LogManager& log_manager);

    // ---- identity --------------------------------------------------------
    AidingSourceKind kind() const override { return AidingSourceKind::Imu; }
    std::string name() const override { return "IMU (INS error-state aiding)"; }

    // ---- (a) STATE -------------------------------------------------------
    /**
     * @brief Describe the existing core IMU error-state offsets.
     *
     * Tier-1: stashes @p sm for live per-epoch index resolution and returns a
     * descriptor over the contiguous attitude/accel-bias/gyro-bias block
     * StateManager already reserved under its @c enableImu() carve-out — it
     * allocates nothing (bit-for-bit identical state layout).  Returns
     * std::nullopt if IMU error states are not present.
     */
    std::optional<StateBlockSpec> reserveStates(StateManager& sm) override;

    // ---- (b) PREDICT -----------------------------------------------------
    /**
     * @brief Run strapdown mechanization and map its 15x15 Phi/Q onto the ins_map.
     *
     * Reads pos/vel/biases from @c ctx.state (via live StateManager indices) and
     * the nominal attitude from the owner, integrates over [t_prev, t_now],
     * writes the new nominal attitude back through the owner, and returns the
     * contribution.  Returns an invalid contribution (KF keeps the baseline) on
     * the first epoch, when not yet wired (no StateManager/owner), with no data,
     * or when integration yields no valid samples.
     */
    PredictContribution contributePredict(const AidingEpochContext& ctx) override;

    // ---- (c) MEASUREMENT -------------------------------------------------
    /// @brief TC-IMU re-parameterizes GNSS rows (column-write seam) under tight coupling.
    bool reparameterizesGnssRows() const override { return tightly_coupled_; }

    /// @brief Tightly-coupled IMU folds nominal feedback into C_b_e_ -> naive IEKF.
    bool requiresNaiveRelinearization() const override { return tightly_coupled_; }

    // ---- lifecycle -------------------------------------------------------

    /**
     * @brief Buffer a streaming IMU sample into the wrapped integrator.
     *
     * Real-time ingest: an @c AidingSourceKind::Imu sample is converted to an
     * @c ImuSample and appended via @c ImuIntegrator::addSample; the next
     * epoch's @c contributePredict integrates the buffered samples over its
     * interval.  Samples of other kinds are ignored.
     */
    void ingestSample(const AidingSample& sample) override;

    bool hasData() const override { return integrator_ && integrator_->hasData(); }

    /// @brief The IMU bridges full GNSS outages (INS dead-reckon) — the faithful
    ///        replacement for the legacy @c enableImu() gap/MW-tolerance queries.
    bool bridgesGaps() const override { return true; }

    ImuAidingSource* asImu() override { return this; }

    // ---- IMU-specific rich API (recovered via asImu()) -------------------

    /**
     * @brief Attach the owner of the nominal attitude (the KalmanFilter).
     *
     * The IMU reads/writes C_b_e_ through this owner — it does NOT own the
     * nominal attitude itself (state-vector discipline: one off-vector owner).
     */
    void attachFilter(KalmanFilter& filter) { filter_ = &filter; }

    /**
     * @brief Feed the tightly-coupled Doppler-Jacobian inputs to the model.
     *
     * Computes the current body-to-ECEF attitude (read through the owner) and
     * the gravity-subtracted body specific force from the live state, and calls
     * @c MeasurementModel::setImuState so the Doppler rows carry attitude-error
     * and accelerometer-bias partials.  No-op unless tightly coupled with data
     * and the StateManager/owner handles are attached.  This relocates the
     * computation that previously lived (twice) in Application::run; the
     * setImuState arithmetic and the model's partials are unchanged.
     */
    void applyTcJacobianInputs(MeasurementModel& model) const;

    /// @brief Access the wrapped integrator (null only if construction failed).
    const ImuIntegrator* integrator() const { return integrator_.get(); }

private:
    // Test seam: ImuAidingSourceTestAccess (tests/test_aiding_source.cpp)
    // forwards to the private projection kernel so the ins_map projection of a
    // synthetic ImuPrediction is testable without a wired StateManager/owner.
    friend struct ImuAidingSourceTestAccess;

    /**
     * @brief Project an @c ImuPrediction onto a full-state contribution.
     *
     * Pure: copies the 15x15 Phi/Q_ins and the @p ins_map into the
     * contribution and builds the nominal write-backs (pos always; vel and
     * attitude-zero when their ins_map slots are valid) — replicating
     * predictImu's x updates exactly.  Sets @c requires_naive_relinearization
     * from the cached coupling mode.  Expects the canonical 15-slot INS map.
     */
    PredictContribution mapPredictionToContribution(
        const ImuPrediction& pred, const std::vector<std::size_t>& ins_map) const;

    /// @brief Build the canonical 15-slot ins_map from live StateManager indices
    ///        (pos, vel, att, accel-bias, gyro-bias), sentinel for absent blocks.
    static std::vector<std::size_t> buildInsMap(const StateManager& sm);

    const GnssConfig& config_;
    LogManager& log_manager_;
    std::unique_ptr<ImuIntegrator> integrator_;
    bool tightly_coupled_{ false };       ///< Cached config_.tightlyCoupledImu() (routes the TC column seam / naive IEKF).
    StateManager* state_manager_{ nullptr }; ///< Stashed in reserveStates for live index resolution (non-owning).
    KalmanFilter* filter_{ nullptr };        ///< Nominal-attitude owner handle (non-owning; via attachFilter).
};

} // namespace GnssProcessor
