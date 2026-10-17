#pragma once
/**
 * @file ObservationEquationCheck.hpp
 * @brief The comparator of the term-list row builder against the row builder it replaced
 *        (config key processing.obs_equation_check): process-wide counters, the carrier
 *        frequency sources compared per band and constellation, and the report.
 *
 * With the key on, every code and phase row is built twice, by the term list
 * (measurement/ObservationEquation.hpp) and by the row builder as it was written before
 * (MeasurementModelRowReference.cpp), and the two are compared bit for bit: whether the row
 * is usable, every entry of the Jacobian row, and the predicted observable.  Each row adds to
 * the counters here; a run that must report zero mismatches reads them at its end.  The
 * counters are process-wide and atomic because the network producer builds the rows of its
 * stations in parallel and the closed-loop consumer builds its own in the same process.
 *
 * The check also lists where the carrier frequency sources of the three copies of the
 * equation differ (the row builder's @c PppUtils, the ambiguity seed's table and the identity
 * layer's), per band and constellation, so that one frequency rule can be chosen with the
 * differences in front of the reader.
 *
 * Temporary: it is deleted with the reference row builder.
 */

#include "measurement/ObservationEquation.hpp"

#include <string>
#include <vector>

namespace GnssProcessor {

class LogManager;

/// The counts of the check since the process began (or since @ref obsEquationCheckReset).
struct ObsEquationCheckCounts {
    long long rows{0};                 ///< Rows built by both builders.
    long long usable_mismatch{0};      ///< Rows one builder accepted and the other refused.
    long long unmapped_identity{0};    ///< Of those, rows the term list refused for a satellite or signal text that is not canonical.
    long long jacobian_mismatch{0};    ///< Rows both accepted whose Jacobian rows differ in any bit.
    long long predicted_mismatch{0};   ///< Rows both accepted whose predicted observable differs in any bit.
};

/**
 * @brief Adds one compared row to the counters.
 * @param usable_new       Whether the term list accepted the row.
 * @param usable_reference Whether the reference builder accepted the row.
 * @param fault            Why the term list refused the row (None when it accepted).
 * @param jacobian_equal   Whether the two Jacobian rows are bitwise equal (read when both accepted).
 * @param predicted_equal  Whether the two predicted observables are bitwise equal (read when both accepted).
 */
void obsEquationCheckRecord(bool usable_new, bool usable_reference, ObsFault fault,
                            bool jacobian_equal, bool predicted_equal);

/**
 * @brief The counters now.
 * @return A snapshot.
 */
ObsEquationCheckCounts obsEquationCheckCounts();

/// Sets every counter to zero (the tests' fixture; a run never calls it).
void obsEquationCheckReset();

/**
 * @brief Whether two doubles hold the same bits (so +0.0 and -0.0 differ and a NaN equals the
 *        same NaN).
 * @param a First value.
 * @param b Second value.
 * @return True when the 64 bits are equal.
 */
bool sameBits(double a, double b);

/**
 * @brief Whether two Jacobian rows hold the same bits entry by entry.
 * @param a First row.
 * @param b Second row.
 * @return True when both have the same size and every entry has the same bits.
 */
bool sameBits(const Eigen::VectorXd& a, const Eigen::VectorXd& b);

/// One (constellation, band digit) cell of the carrier frequency comparison.
struct ObsFrequencyRow {
    System sys{System::Unknown};  ///< The constellation.
    char   digit{'\0'};           ///< The RINEX band digit.
    double ppp_utils_hz{0.0};     ///< @c PppUtils::getFrequency (slot 0 for a FDMA band); 0 when it knows no such band.
    double slot_spread_hz{0.0};   ///< Largest distance of any slot's @c PppUtils frequency from slot 0 (FDMA bands).
    double seed_hz{0.0};          ///< The ambiguity seed's table (@c StateManager::ambiguitySeedFrequencyHz).
    double identity_hz{0.0};      ///< The identity layer's nominal frequency (@ref nominalCarrierFrequencyHz).
};

/**
 * @brief The carrier frequency of every band of every constellation from the three sources.
 * @param log The log manager the temporary frequency helper logs through.
 * @return One row per (constellation, band digit) of the identity layer's tables, in table order.
 */
std::vector<ObsFrequencyRow> obsFrequencyAudit(LogManager& log);

/**
 * @brief The audit rows that disagree between sources, as report lines.
 * @param rows The audit.
 * @return One line per row whose three frequencies are not equal (to 1 mHz) or whose FDMA
 *         slots spread; empty when the sources agree everywhere.
 */
std::vector<std::string> obsFrequencyDifferences(const std::vector<ObsFrequencyRow>& rows);

/**
 * @brief Runs the frequency audit once per process and keeps its differing lines for the report.
 * @param log The log manager for the temporary frequency helper.
 */
void obsEquationCheckNoteFrequencies(LogManager& log);

/**
 * @brief The report of the check: the counters, then the frequency differences.
 * @return The lines, without a prefix; empty when no row was compared.
 */
std::vector<std::string> obsEquationCheckReport();

} // namespace GnssProcessor
