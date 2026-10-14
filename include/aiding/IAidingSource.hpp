#pragma once

/**
 * @file IAidingSource.hpp
 * @brief Polymorphic contract shared by every navigation aiding source.
 *
 * An aiding source is anything that contributes navigation information beyond
 * GNSS pseudorange, phase and Doppler: IMU, ZUPT, NHC, wheel odometry, baro
 * height, vision.  Without this interface each modality needs its own branch at
 * four sites (StateManager, the KalmanFilter IMU predict, a MeasurementModel
 * branch, and Application::run).  The interface lets sources compose: each
 * contributes up to three things, each at the seam where the IMU injects that
 * same thing.
 *
 *   - State: the source names (tier 1, pre-existing core offsets) or reserves
 *     (tier 2, variable cardinality) its block through StateManager, never
 *     through a sibling owner; StateManager remains the single layout authority.
 *     Error-state sources keep their nominal state off-vector in the designated
 *     single owner (the KF's C_b_e_), never inside the source.
 *   - Predict: the source returns a Phi/Q sub-block (plus an optional nominal
 *     write-back) that KalmanFilter writes over the StateManager baseline
 *     (assignment, not accumulation).
 *   - Measurement: the source appends independent rows (H/z/R), or
 *     re-parameterizes existing GNSS rows through extra Jacobian columns (the
 *     TC-IMU pattern).
 *
 * The design mirrors @c ISlipDetector: one uniform
 * @c (const GnssConfig&, LogManager&) constructor (so a single registry Creator
 * works), a narrow base contract, and the source-specific API kept off the base
 * and recovered through a typed down-cast without RTTI (@c asImu), as
 * @c ISlipDetector::asMelbourneWubbena() does.  A source opts out of a phase
 * through the natural empty or invalid return (empty @c MeasurementRows, a
 * @c PredictContribution with @c valid==false), the same signals the no-source
 * fast path keys on, so simple sources (ZUPT, baro) implement almost nothing.
 *
 * @see docs/aiding_source_abstraction_design.md
 */

#include "aiding/AidingTypes.hpp"
#include "logging/LogManager.hpp"
#include <Eigen/Core>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace GnssProcessor {

class StateManager;     // forward decl: state reservation host (single layout authority)
class MeasurementModel; // forward decl: measurement row sink host
class ImuAidingSource;  // forward decl: typed down-cast target (the privileged source)

/**
 * @class IAidingSource
 * @brief Abstract base every aiding source implements.
 *
 * Lifecycle: construct (uniform 2-arg ctor) -> @c reserveStates(StateManager&)
 * once after the core state is laid out -> per-epoch @c ingestSample /
 * @c contributePredict / @c contributeMeasurement -> @c reset on
 * dropout/reacquire.
 */
class IAidingSource {
public:
    virtual ~IAidingSource() = default;

    // ---- identity --------------------------------------------------------

    /// @brief Modality tag (registry key / provenance tag / config token).
    virtual AidingSourceKind kind() const = 0;

    /// @brief Human-readable name (for logs/traces).
    virtual std::string name() const = 0;

    // ---- (a) State: establish a block through StateManager ---------------

    /**
     * @brief Establish this source's state block via StateManager.
     *
     * Tier-1 sources (the IMU core error states; ZUPT, NHC and baro own no new
     * states) describe pre-existing core offsets, with no allocation and no
     * P initialisation (the core path has already seeded P).  Tier-2 sources
     * (variable cardinality, e.g. per-landmark vision) reserve and seed through
     * StateManager.  The returned @c StateBlockSpec is a one-time receipt, not a
     * durable index cache: per-epoch index resolution has to use a live
     * StateManager lookup, because @c removeStateAtIndex shifts block indices on
     * every prune below the block.
     *
     * @param sm The single source of truth for state layout.
     * @return The block descriptor, or std::nullopt if the source owns none.
     */
    virtual std::optional<StateBlockSpec> reserveStates(StateManager& sm) = 0;

    // ---- (b) Predict: contribute Phi/Q (plus optional mechanization) -----

    /**
     * @brief Supply this source's predict-time contribution over the interval.
     *
     * Returns the full-state indices the source owns (resolved live from
     * StateManager), square Phi/Q sub-blocks over those indices (KalmanFilter
     * assigns them and never accumulates), and an optional nominal segment to
     * write into x.  The default returns @c valid==false, which KalmanFilter
     * treats as "skip this source", so for measurement-only sources the predict
     * path stays byte-identical to GNSS-only.
     *
     * @param ctx Epoch interval + current nominal state.
     * @return Contribution; @c valid==false => KalmanFilter keeps the baseline.
     */
    virtual PredictContribution contributePredict(const AidingEpochContext& ctx) {
        (void)ctx; return PredictContribution{};  // .valid defaults false
    }

    // ---- (c) Measurement: contribute H/z/R rows --------------------------

    /**
     * @brief Build independent measurement rows for this epoch.
     *
     * Each row's Jacobian spans the full state (cols == state_size) and writes
     * only into StateManager-owned columns.  Labels have to be unique within the
     * epoch so that KF outlier and exclusion keys never collide.  The default is
     * empty: a predict-only source leaves the update untouched.
     *
     * @param ctx Epoch context (current state, state_size, time).
     * @return Rows to append (empty if none this epoch).
     */
    virtual MeasurementRows contributeMeasurement(const AidingEpochContext& ctx) {
        (void)ctx; return MeasurementRows{};
    }

    /**
     * @brief Whether this source re-parameterizes existing GNSS rows (adds
     *        Jacobian columns) instead of appending its own rows.
     *
     * True for sources whose information is the same GNSS observable expressed
     * through extra states (TC-IMU writes attitude and accel-bias partials into
     * the Doppler row).  Such sources are routed through the host's column-write
     * seam, not the row-append seam.  This is the one capability that cannot be
     * inferred from an empty return.  Default false.
     */
    virtual bool reparameterizesGnssRows() const { return false; }

    /**
     * @brief Whether this source applies error-state nominal feedback that
     *        forces the naive (single-pass) IEKF relinearization.
     *
     * The tightly-coupled IMU folds attitude and bias error into off-vector
     * nominal state (C_b_e_) and zeroes the error states after each update.
     * That conflicts with re-deriving the posterior from the epoch prior, so a
     * config with such a source takes the naive relinearization path.  The
     * estimator queries this (via @c AidingSourceManager) instead of hard-coding
     * @c tightlyCoupledImu().  Default false: a source without nominal feedback
     * can use the prior-anchored IEKF.
     */
    virtual bool requiresNaiveRelinearization() const { return false; }

    // ---- lifecycle -------------------------------------------------------

    /**
     * @brief Ingest one streaming sample (real-time push); no-op in replay.
     *
     * Separates "ingest" from "propagate" so a source can buffer asynchronous
     * samples until the next GNSS epoch boundary defines the interval.  In
     * post-processing the whole timeline is loaded up front, so the base
     * default does nothing.
     */
    virtual void ingestSample(const AidingSample& sample) { (void)sample; }

    /// @brief Whether this source has data ready (mirrors ImuIntegrator::hasData()).
    virtual bool hasData() const = 0;

    /**
     * @brief Whether this source bridges full GNSS outages (INS dead-reckoning).
     *
     * Lets the gap-recovery policy (classifyDynamicsMode / handleGap) and the MW
     * arc-reset tolerance query the abstraction instead of @c enableImu().
     * Default false (baro or ZUPT does not bridge a full GNSS outage).
     */
    virtual bool bridgesGaps() const { return false; }

    /// @brief Soft reset (decorrelate + reinflate owned block) on dropout/reacquire.
    virtual void reset() {}

    /// @brief Enable CSV trace output for this source.
    virtual bool enableTracing(const std::filesystem::path& log_dir) {
        (void)log_dir; return false;
    }

    /// @brief Whether tracing is currently active.
    virtual bool isTracingEnabled() const { return false; }

    /**
     * @brief Typed access to the IMU source when this instance is one.
     *
     * Returns @c this for @c ImuAidingSource, nullptr otherwise.  Lets the owner
     * recover the IMU's source-specific API (TC Jacobian inputs, attitude) from a
     * registry-built handle without RTTI, as
     * @c ISlipDetector::asMelbourneWubbena() does, and keeps that API off the base.
     */
    virtual ImuAidingSource* asImu() { return nullptr; }
};

} // namespace GnssProcessor
