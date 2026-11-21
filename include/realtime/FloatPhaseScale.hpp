#pragma once

/**
 * @file FloatPhaseScale.hpp
 * @brief The receiver's own phase noise factor, estimated from its post-fit residuals, and its application to the
 *        phase rows of satellites whose ambiguities are float.
 *
 * The configured phase sigma is one number for every receiver, while the white part of the phase noise belongs to
 * the receiver: the redundancy-normalised variance factor of a 24 h user is 0.17 at WTZZ00DEU and 1.1 at UTQI, and
 * the residuals are serially correlated (lag-one 0.62 to 0.82 at 30 s).  A float ambiguity absorbs the slow part of
 * its own satellite's phase error, so the white part is the weight that fits that satellite's phase rows while the
 * ambiguity is float; once its integers are held it cannot absorb anything, and the configured weight (which equals
 * the white part inflated for the correlation at WTZZ00DEU, and keeps the covariance calibrated) is the one that
 * fits.  This file holds the estimator of the white factor and the per-satellite map of the factor applied; the
 * real-time engine feeds it and reads it (@c filter.measurement_noise.phase_scale_measure,
 * @c filter.measurement_noise.float_phase_scale).
 *
 * The estimator, per constellation:
 *  - per epoch, over the between-satellite phase rows whose two satellites carry the same applied factor f and whose
 *    redundancy r = Sr_ii / R_ii is at least @c min_redundancy, the variance factor relative to the configured
 *    weights, sum f v^2 / R_applied over sum r (E[v^2 / R_applied] = r under the applied model);
 *  - the factor k^2 is the median of the per-epoch values over a trailing window, formed from at least
 *    @c min_epochs values (until then the factor is 1, the configured weight);
 *  - a row whose w-test exceeds tail_z sqrt(k^2 / f) is in the tail: it stays out of the estimate, and its
 *    satellite keeps the configured weight at the next epoch; an epoch whose tail share in a constellation exceeds
 *    @c freeze_share leaves that constellation's window unchanged (frozen).
 * There is no fixed floor under k: the scale comes from the receiver's own rows, never from the tested row alone.
 *
 * Measured against the configured weights on four 24 h users and 85 three-hour starts: the factor forms only once
 * the float ambiguities have converged (their rows reach the redundancy bar), and, read from the rows it tightens,
 * it runs below the measure-only value; applied, it left the covariance overconfident and let wrong integers through
 * on observed data. The application stays off; the estimator is kept as an instrument.
 */

#include <cstddef>
#include <deque>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include <Eigen/Dense>

namespace GnssProcessor {

/// One post-fit row of the user's between-satellite update, as the estimator reads it.
struct FloatPhaseScaleRow {
    std::string sat_i;      ///< Non-reference satellite of the row.
    std::string sat_p;      ///< Reference satellite of the row (empty for an undifferenced row).
    std::string signal;     ///< Observation type ("L1C", "C2W", ...); only 'L' rows are read.
    double v{ 0.0 };        ///< Post-fit residual [m].
    double r_applied{ 0.0 };///< Row variance the update used [m^2].
    double sr{ 0.0 };       ///< Post-fit residual variance Sr_ii [m^2].
};

/// Settings of the estimator.
struct FloatPhaseScaleSettings {
    double window_s{ 3600.0 };      ///< Trailing window of the per-epoch values [s].
    int    min_epochs{ 40 };        ///< Values needed in the window before the factor is formed.
    double tail_z{ 3.29 };          ///< Tail bar in standard deviations of the bulk (0.1 % two-sided).
    double freeze_share{ 0.10 };    ///< A constellation's epoch with a larger tail share is not counted.
    double min_redundancy{ 0.1 };   ///< Rows with less redundancy do not inform the factor.
};

/// Run counters of the estimator and of the map (measure only; all zero with both keys off).
struct FloatPhaseScaleCounters {
    long long epochs_seen{ 0 };     ///< Epochs fed to the estimator.
    long long values_pushed{ 0 };   ///< Per-constellation epoch values entered into a window.
    long long values_frozen{ 0 };   ///< Per-constellation epoch values withheld by the freeze rule.
    long long rows_used{ 0 };       ///< Phase rows that informed a value.
    long long rows_mixed{ 0 };      ///< Phase rows skipped because their two satellites carried different factors.
    long long tail_rows{ 0 };       ///< Phase rows in the tail.
    long long tail_enter{ 0 };      ///< Satellites entering the tail set.
    long long tail_leave{ 0 };      ///< Satellites leaving it.
    long long to_bulk{ 0 };         ///< Satellites switched to the receiver's factor (float, not in the tail).
    long long to_present{ 0 };      ///< Satellites switched back to the configured weight (held, tail, or factor lost).
    long long scaled_sat_epochs{ 0 };   ///< Satellite-epochs whose phase rows carried the factor.
};

/**
 * @brief Multiply the variance of every phase row of a satellite in @p scale by its factor.
 *
 * The rows are the undifferenced rows of one epoch before the between-satellite transform: @p satellite_ids and
 * @p signal_types index @p R_v.  A row is phase when its type starts with 'L'.  An empty map leaves @p R_v as it is.
 *
 * @param R_v            Row variances [m^2], modified in place.
 * @param satellite_ids  Satellite of each row.
 * @param signal_types   Observation type of each row.
 * @param scale          Satellite to factor on its phase rows' variance.
 * @return Number of rows scaled.
 */
int applyPhaseVarianceScale(Eigen::VectorXd& R_v, const std::vector<std::string>& satellite_ids,
                            const std::vector<std::string>& signal_types,
                            const std::map<std::string, double>& scale);

/**
 * @brief Split a row identity "G05-G12" into its two satellites (the second empty for "G05").
 * @param id The row's satellite identity.
 * @return (non-reference satellite, reference satellite).
 */
std::pair<std::string, std::string> splitRowSatellites(const std::string& id);

/// The estimator of the receiver's phase factor and the per-satellite map of its application.
class FloatPhaseScale {
public:
    explicit FloatPhaseScale(FloatPhaseScaleSettings s = {}) : s_(s) {}

    /**
     * @brief Feed one epoch's post-fit rows.
     * @param tow      Epoch time [s].
     * @param rows     The epoch's rows (code rows are ignored).
     * @param applied  The factor applied to each satellite's phase rows this epoch (absent = 1).
     */
    void observeEpoch(double tow, const std::vector<FloatPhaseScaleRow>& rows,
                      const std::map<std::string, double>& applied);

    /// True when the factor of a constellation (first character of a satellite id) is formed.
    bool formed(char constellation) const { return k2_.count(constellation) > 0; }

    /// The factor k^2 of a constellation, 1 until formed.
    double factor(char constellation) const;

    /// Values in the constellation's window.
    std::size_t windowSize(char constellation) const;

    /**
     * @brief The map of the factor for this epoch, and its edges against the previous epoch's map.
     *
     * A satellite gets its constellation's factor when it is not held, its factor is formed and none of its phase
     * rows was in the tail at the last epoch.  A satellite entering the map counts @c to_bulk, one leaving it while
     * still observed counts @c to_present; @ref lastEdges lists them.
     *
     * @param sats  The satellites observed this epoch (duplicates allowed).
     * @param held  The satellites held at the last integer search.
     * @return Satellite to factor.
     */
    std::map<std::string, double> scaleMap(const std::vector<std::string>& sats, const std::set<std::string>& held);

    /// The edges of the last @ref scaleMap call: (satellite, true for to the receiver's factor).
    const std::vector<std::pair<std::string, bool>>& lastEdges() const { return last_edges_; }

    /// Satellites with a phase row in the tail at the last epoch.
    const std::set<std::string>& tailSatellites() const { return tail_sats_; }

    const FloatPhaseScaleCounters& counters() const { return c_; }

    /**
     * @brief The run-end line: the factor and window of each constellation, the values pushed and frozen, the rows
     *        used and mixed, the tail, and the switches of the map.
     */
    std::string runEndLine() const;

private:
    FloatPhaseScaleSettings s_;
    std::map<char, std::deque<std::pair<double, double>>> window_;   ///< Constellation to (tow, epoch value).
    std::map<char, double> k2_;                                      ///< Formed factors.
    std::set<std::string> tail_sats_;
    std::map<std::string, double> last_map_;
    std::vector<std::pair<std::string, bool>> last_edges_;
    FloatPhaseScaleCounters c_;
};

} // namespace GnssProcessor
