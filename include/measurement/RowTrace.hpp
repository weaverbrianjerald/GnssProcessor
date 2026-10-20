#pragma once
/**
 * @file RowTrace.hpp
 * @brief The decomposition trace of a code or phase row: the predicted observable and the
 *        terms of it that carry a clock, an ionosphere, a bias or an ambiguity.
 *
 * Every row is the sum of the terms of one observation equation
 * (measurement/ObservationEquation.hpp), whoever builds it: the single-receiver model
 * (@c MeasurementModel::buildJacobianRow, the PPP loop and the real-time engine) and the
 * network producer's row loop (network/assembly/StationRows.hpp). The trace is therefore a free
 * function of the row's observation, its predicted value and its term values, called by both.
 * It writes to the @c measurement_model logger at trace level and nothing else.
 *
 * ### PHASE-DECOMP
 *
 * Prints the zero-difference predicted phase and the terms that carry a clock, an ionosphere,
 * a bias or an ambiguity, so that a wandering float or a product datum step can be attributed
 * to one channel. It is not a complete decomposition: the a-priori model, the range correction,
 * the zenith wet delay, the troposphere gradients, the inter-system bias and the phase-colour
 * term are in the prediction and are not printed.
 *  - @c ssr_pbias is the applied SSR satellite phase-bias term (0 when the store has no entry
 *    for this satellite and signal; the closed-loop datum attribution uses it);
 *  - @c sat_pbias is the estimated per-(satellite, signal) phase-bias parameter, plus the
 *    receiver phase-bias split parameter when that is armed (the network producer's);
 *  - @c sat_clk is the estimated residual satellite-clock parameter (the network producer's);
 *  - @c amb is the float-ambiguity contribution in metres (the narrow-lane and wide-lane basis
 *    included).
 *
 * ### CODE-DECOMP
 *
 * Prints, for any code signal, the zero-difference predicted observable and the clock,
 * ionosphere and bias terms in it, so that a residual of hundreds of metres or kilometres can
 * be attributed. It is not a complete decomposition: the a-priori model, the range correction,
 * the zenith wet delay, the troposphere gradients, the inter-system bias and the satellite
 * clock term are in the prediction and are not printed.
 *  - @c rx_clk is the receiver-clock term (0 under between-satellite differencing, where the
 *    differencing removes the physical clock, so @c zd_innov still contains it, equally on every
 *    signal, and it cancels only after differencing);
 *  - @c iono is the per-satellite slant-ionosphere term (+alpha I on code);
 *  - @c ssr_cbias is the SSR code bias matched by the exact RINEX observation code (0 when the
 *    provider's set lacks this code);
 *  - @c sat_bias is the estimated per-(satellite, signal) code-bias parameter;
 *  - @c bia_cbias is the receiver DCB or IFB term. A .BIA code bias
 *    (@c enable_code_phase_bias) is applied in @c PppModel and is already part of the modeled
 *    value.
 *
 * Plain functions of a logger, an observation and values; no state.
 */

#include "common/PppObservation.hpp"
#include "measurement/ObservationEquation.hpp"   // ObsTermValues

#include <string>

namespace GnssProcessor {

class LogManager;

/**
 * @brief Whether a row's decomposition trace will print: the satellite is named and the
 *        @c measurement_model logger is at trace level.
 *
 * The callers ask this before evaluating a row, so that the term values are collected only
 * when a line will be written (@c evaluate fills an @ref ObsTermValues on request).
 *
 * @param log Logging; the @c measurement_model logger is read.
 * @param obs The satellite's modeled observation.
 * @return True when @ref traceRowDecomposition would write a line.
 */
bool wantsRowDecomposition(LogManager& log, const PppObservation& obs);

/**
 * @brief Prints the PHASE-DECOMP or CODE-DECOMP line of a row at trace level.
 *
 * A phase signal prints the phase line, any other the code line (a Doppler row has no
 * equation and no trace). The observed value is the observation's own; a signal with none
 * prints 0.
 *
 * @param log       Logging; the @c measurement_model logger is written.
 * @param obs       The satellite's modeled observation.
 * @param ref_type  The signal (a RINEX code, or an iono-free pair "a_b").
 * @param predicted The row's predicted observable (m), the sum of @p terms.
 * @param terms     The value of each term of the row's equation (from @c evaluate).
 */
void traceRowDecomposition(LogManager& log, const PppObservation& obs, const std::string& ref_type,
                           double predicted, const ObsTermValues& terms);

} // namespace GnssProcessor
