#pragma once

/**
 * @file ConsumerTrace.hpp
 * @brief Schema of `logs/closed_loop_trace.csv`, the per-epoch row of the
 *        closed-loop PPP-RTK consumer: position error, ambiguity-resolution outcome
 *        and the formal position sigma.
 *
 * Two writers produce this file: the network producer's emit/store seam (the
 * consumer runs in the same process as the producer) and the consumer-only replay
 * (@c Application::runConsumerReplay, which reads a stored wire).  Every grader
 * reads both outputs with one parser, so the header, the row and the formatting
 * live here once and both writers call them.
 *
 * Column groups, in order:
 *  - identity: `week,tow,n_sats`
 *  - ambiguity resolution: `ar_ran,ar_success,num_fixed,num_candidates,ratio`
 *  - actual error against the configured truth coordinate (m):
 *    `pos_err_3d_m,east_m,north_m,up_m`
 *  - `ar_gate` (which gate stopped AR, see @ref GnssProcessor::consumerArGateLabel "consumerArGateLabel") and `p_succ`
 *  - formal 1-sigma of the same position from the filter covariance, in the local
 *    east / north / up frame at the truth coordinate (m):
 *    `sig_e_m,sig_n_m,sig_u_m`, and `sig_3d_m`, the root of the covariance trace
 *    (the formal counterpart of `pos_err_3d_m`)
 *
 * The sigma columns are appended after `p_succ` so a reader indexing the first
 * fourteen columns by position, or by name, is unchanged.  The ratio of an actual
 * error to its formal sigma (per axis, or the position NEES e^T P^-1 e) is the
 * consistency grade of the consumer's covariance (scripts/consumer_nees.py).
 *
 * Measure-only: the filter reads nothing back from the row.
 */

#include "math/Coordinates.hpp"

#include <Eigen/Dense>

#include <cstddef>
#include <string>

namespace GnssProcessor {

struct AmbiguityResolution;

/// CSV header of `logs/closed_loop_trace.csv` (newline-terminated).  Kept beside
/// the row struct and the formatter so the three stay in step.
inline constexpr const char* kConsumerTraceHeader =
    "week,tow,n_sats,ar_ran,ar_success,num_fixed,num_candidates,"
    "ratio,pos_err_3d_m,east_m,north_m,up_m,ar_gate,p_succ,"
    "sig_e_m,sig_n_m,sig_u_m,sig_3d_m\n";

/// Number of columns of @ref kConsumerTraceHeader.
inline constexpr std::size_t kConsumerTraceColumns = 18;

/**
 * @brief One row of `closed_loop_trace.csv`: the consumer's solution at one epoch.
 *
 * The writer fills the identity, the error and the sigma; @ref fillConsumerTraceAr
 * fills the ambiguity-resolution group from the engine's last AR result, so the
 * gate wording is defined in one place.
 */
struct ConsumerTraceRow {
    int         week{ 0 };              ///< GPS week of the consumer epoch.
    double      tow{ 0.0 };             ///< GPS time of week of the consumer epoch (s).
    std::size_t n_sats{ 0 };            ///< Observations the consumer was handed this epoch.

    bool        ar_ran{ false };        ///< AR was attempted this epoch.
    bool        ar_success{ false };    ///< AR was attempted and accepted.
    int         num_fixed{ 0 };         ///< Ambiguities fixed (0 when AR did not run).
    int         num_candidates{ 0 };    ///< AR candidates (0 when AR did not run).
    double      ratio{ 0.0 };           ///< Ratio-test value (0 when AR did not run).
    std::string ar_gate{ "not_run" };   ///< Gate label, see @ref consumerArGateLabel.
    double      p_succ{ 0.0 };          ///< Bootstrap success rate (0 when AR did not run).

    double          err_3d_m{ 0.0 };                        ///< |position - truth| (m).
    Eigen::Vector3d err_enu_m{ Eigen::Vector3d::Zero() };   ///< Error east, north, up (m).
    Eigen::Vector3d sig_enu_m{ Eigen::Vector3d::Zero() };   ///< Formal 1-sigma east, north, up (m).
    double          sig_3d_m{ 0.0 };                        ///< Root of the position covariance trace (m).
};

/**
 * @brief Label of the gate that stopped (or passed) AR this epoch, the `ar_gate`
 *        column.
 *
 * `not_run` when AR was not attempted; otherwise by @c gate_reason: `disabled`,
 * `min_epochs`, `sigma_gate(<sigma>m)`, `no_candidates`, `wl_guard`; with no early
 * gate, the MLAMBDA outcome: `fixed`, `wl_only`, `ratio_battery` (a ratio was
 * computed and failed) or `mlambda_fail` (no ratio).
 *
 * @param ar_ran True when the engine attempted AR this epoch.
 * @param ar     The engine's last AR result (read only when @p ar_ran).
 * @return The label (contains no comma).
 */
std::string consumerArGateLabel(bool ar_ran, const AmbiguityResolution& ar);

/**
 * @brief Fill the ambiguity-resolution columns of @p row from the engine's last AR
 *        result.  Every numeric field is 0 when AR did not run.
 *
 * @param row    Row to fill (identity, error and sigma are left untouched).
 * @param ar_ran True when the engine attempted AR this epoch.
 * @param ar     The engine's last AR result.
 */
void fillConsumerTraceAr(ConsumerTraceRow& row, bool ar_ran, const AmbiguityResolution& ar);

/**
 * @brief Rotate an ECEF position covariance into the local frame and return the
 *        formal 1-sigma per axis.
 *
 * P_enu = R^T P_ecef R with R = Coordinates::enuToEcefMatrix at @p ref, the same
 * frame in which the trace writes the error (Coordinates::ecefToEnu at the truth
 * coordinate).  A negative diagonal (a covariance that has lost positive
 * definiteness) is written as sigma 0 rather than NaN.
 *
 * @param p_ecef Position covariance, ECEF axes (m^2).
 * @param ref    Geodetic reference point of the local frame.
 * @return Formal sigma east, north, up (m).
 */
Eigen::Vector3d enuSigmaFromEcefCovariance(const Eigen::Matrix3d& p_ecef,
                                           const Coordinates::Llh& ref);

/**
 * @brief Root of the trace of a position covariance (m): the formal counterpart of
 *        a 3D error.  The trace is rotation invariant, so the ECEF covariance gives
 *        the same value as its local-frame rotation.
 *
 * @param p_ecef Position covariance (m^2).
 * @return sqrt(max(0, trace)).
 */
double sigma3dFromCovariance(const Eigen::Matrix3d& p_ecef);

/**
 * @brief Format one @ref GnssProcessor::ConsumerTraceRow "ConsumerTraceRow" as a CSV line for @ref GnssProcessor::kConsumerTraceHeader "kConsumerTraceHeader".
 *
 * Pure (no I/O, no engine state), so the schema is unit-testable on its own
 * (tests/test_consumer_trace.cpp).  Precision: 3 dp for tow, 2 dp for ratio, 4 dp
 * for every metre column (error and sigma), 6 dp for @c p_succ.
 *
 * @param r The row.
 * @return The CSV line, newline-terminated.
 */
std::string formatConsumerTraceRow(const ConsumerTraceRow& r);

} // namespace GnssProcessor
