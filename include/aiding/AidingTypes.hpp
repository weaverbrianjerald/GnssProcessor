#pragma once

/**
 * @file AidingTypes.hpp
 * @brief Value types shared by IAidingSource, AidingSourceRegistry, the
 *        AidingSourceManager, and the KalmanFilter / MeasurementModel /
 *        StateManager seams.
 *
 * @c AidingSourceKind is the single enum that serves triple duty (registry
 * key, provenance tag on every contribution, config-selection token) — exactly
 * as @c SlipDetectorKind does for the slip layer.  Free functions
 * @c toString / @c aidingKindFromString mirror the slip pair.
 *
 * The contribution value types generalize how the IMU injects today:
 *   - @c PredictContribution generalizes the ins_map Phi/Q injection in
 *     @c KalmanFilter::predictImu;
 *   - @c AidingMeasurementRow / @c MeasurementRows generalize one appended
 *     measurement row (H/z/R + label);
 *   - @c StateBlockSpec describes the state block a source owns (a one-shot
 *     reservation RECEIPT — NOT a durable index cache; see field doc).
 *
 * @see docs/aiding_source_abstraction_design.md
 */

#include "common/PppTypes.hpp"   // GnssTime
#include <Eigen/Core>
#include <optional>
#include <string>
#include <vector>

namespace GnssProcessor {

/**
 * @brief Identifies the modality behind an aiding source.
 *
 * Doubles as the @c AidingSourceRegistry map key, the contribution provenance
 * tag, and the config-selection token (see @c aidingKindFromString).
 */
enum class AidingSourceKind {
    Imu,        ///< Inertial (strapdown INS error states) — primary, always built first.
    Zupt,       ///< Zero-velocity update (pseudo-measurement on velocity).
    Nhc,        ///< Non-holonomic constraint (lateral/vertical body velocity ~ 0).
    WheelOdom,  ///< Wheel/odometer speed.
    Baro,       ///< Barometric height.
    Vision      ///< Visual odometry / landmark (STAGED — registry skips with a warning).
};

/// @brief Canonical lower-case token ("imu","zupt","nhc","wheel","baro","vision").
std::string toString(AidingSourceKind kind);

/// @brief Parse a (case-insensitive) token into a kind; std::nullopt on miss.
std::optional<AidingSourceKind> aidingKindFromString(const std::string& token);

/**
 * @brief Descriptor of the contiguous state block a source owns.
 *
 * Returned by @c IAidingSource::reserveStates once the block is established
 * THROUGH StateManager.  @c start_index .. @c start_index + @c size is the
 * half-open range at reservation time; @c layout names the per-state meaning
 * for traces/diagnostics.
 *
 * IMPORTANT: this is a one-shot reservation RECEIPT, not a durable index
 * cache.  @c StateManager::removeStateAtIndex shifts block indices on every
 * lower prune, so per-epoch index resolution MUST go through a LIVE lookup
 * (getBlockRange / the getAttitudeIndex-style getters), never a cached
 * @c start_index.
 */
struct StateBlockSpec {
    AidingSourceKind owner{ AidingSourceKind::Imu }; ///< Which source owns the block.
    std::string key_prefix;                          ///< StateKeySchema prefix (e.g. "AID|IMU"); empty for core-offset tier-1.
    std::size_t start_index{ 0 };                    ///< First full-state index at reservation (re-resolve live each epoch).
    std::size_t size{ 0 };                           ///< Number of states in the block.
    std::vector<std::string> layout;                 ///< Per-state labels (size == .size).
};

/**
 * @brief A source's predict-time contribution to F/Q (and optional nominal x).
 *
 * Generalizes the IMU ins_map injection in @c KalmanFilter::predictImu: the
 * source lists the FULL-state indices it owns and supplies square Phi/Q
 * sub-blocks over exactly those indices.  KalmanFilter ASSIGNS (never @c +=)
 * these onto the StateManager baseline, then propagates P once.  A propagated
 * nominal segment is written directly into x at @c nominal_indices before
 * propagation.
 */
struct PredictContribution {
    AidingSourceKind source{ AidingSourceKind::Imu }; ///< Provenance.
    std::vector<std::size_t> indices;                 ///< Full-state indices this source owns (the ins_map).
    Eigen::MatrixXd Phi;                              ///< (n x n) transition over @c indices (n == indices.size()).
    Eigen::MatrixXd Q;                                ///< (n x n) process noise over @c indices.
    std::vector<std::size_t> nominal_indices;         ///< Full-state indices to overwrite in x (may be empty).
    Eigen::VectorXd nominal_values;                   ///< Values for @c nominal_indices (size matches).
    double dt_total{ 0.0 };                           ///< Integrated interval (drives the baseline-Q interval; diagnostics).
    std::size_t num_samples{ 0 };                     ///< Samples consumed (diagnostics).
    bool requires_naive_relinearization{ false };     ///< Set ONLY for the tight-coupling error-state-feedback case (forces the naive IEKF second pass).
    bool valid{ false };                              ///< False => KalmanFilter skips this contribution (time-only baseline kept).
};

/**
 * @brief One independent measurement row a source contributes (H/z/R + label).
 *
 * Mirrors what the MeasurementModel per-row loop fills: a Jacobian over the
 * full state, the innovation (observed - predicted), the diagonal variance,
 * and a row label.  The label MUST be UNIQUE within the epoch (e.g. "ZUPT_vx"
 * / "ZUPT_vy" / "ZUPT_vz") so KF outlier/exclusion keys never collide.
 */
struct AidingMeasurementRow {
    Eigen::VectorXd jacobian;   ///< (state_size) design-matrix row.
    double innovation{ 0.0 };   ///< z - h(x).
    double variance{ 0.0 };     ///< Diagonal R entry (m^2 or (m/s)^2).
    std::string label;          ///< Synthetic provenance token, unique per epoch (e.g. "ZUPT_vx").
    std::string signal;         ///< Secondary token for signal_types alignment (e.g. "AID").
};

/// @brief A source's full row set for one epoch (appended into EpochMeasurement).
using MeasurementRows = std::vector<AidingMeasurementRow>;

/**
 * @brief One streaming aiding sample (real-time push); interpreted by @c kind.
 *
 * Lets a source buffer asynchronous samples (a live IMU) until the next GNSS
 * epoch boundary defines the integration interval — the capability that
 * unlocks real-time IMU in @c RealTimeEngine.  Unused in pre-loaded replay.
 */
struct AidingSample {
    AidingSourceKind kind{ AidingSourceKind::Imu };
    GnssTime time;                                    ///< GPS time of the sample.
    Eigen::Vector3d accel{ Eigen::Vector3d::Zero() }; ///< IMU specific force, body frame (m/s^2).
    Eigen::Vector3d gyro{ Eigen::Vector3d::Zero() };  ///< IMU angular rate, body frame (rad/s).
    double scalar{ 0.0 };                             ///< Baro height / wheel speed (by kind).
};

/**
 * @brief Per-epoch context handed uniformly to a source's contribute* calls.
 *
 * Bundled (mirrors @c SlipEpochContext) so the contribute signatures stay
 * stable: the [t_prev, t_now] interval (predict propagation), the current
 * nominal state and its size (measurement Jacobian/innovation), and a
 * first-epoch flag that forces the GNSS-only predict on epoch 0.
 */
struct AidingEpochContext {
    GnssTime t_prev;                ///< Previous processed epoch (predict t_start).
    GnssTime t_now;                 ///< Current epoch (predict t_end).
    const Eigen::VectorXd& state;   ///< Current nominal state x.
    std::size_t state_size{ 0 };    ///< x.size() (== measurement-row column count).
    bool first_epoch{ false };      ///< Forces GNSS-only predict on epoch 0.

    /**
     * @brief Construct over the live state vector and epoch interval.
     * @param x     Current nominal state (held by reference for this epoch only).
     * @param tp    Previous epoch time (predict interval start).
     * @param tn    Current epoch time (predict interval end).
     * @param first Whether this is the first processed epoch.
     */
    AidingEpochContext(const Eigen::VectorXd& x,
                       const GnssTime& tp,
                       const GnssTime& tn,
                       bool first)
        : t_prev(tp), t_now(tn), state(x),
          state_size(static_cast<std::size_t>(x.size())), first_epoch(first) {}
};

} // namespace GnssProcessor
