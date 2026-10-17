#pragma once
/**
 * @file ObsEquationCheckRunEnd.hpp
 * @brief Run-end report of the observation-equation check (processing.obs_equation_check):
 *        the rows compared by the term-list builder and the reference builder and the bitwise
 *        mismatches, with the carrier frequency sources that differ.
 *
 * The report defined in ObsEquationCheckRunEnd.cpp is a row of kNetworkRunEndReports
 * (network/runend/RunEnd.hpp).  The counters live in measurement/ObservationEquationCheck.hpp;
 * the producer's stations and the closed-loop consumer add to them in one process.
 */

namespace GnssProcessor {

struct NetworkRunEndView;

/**
 * @brief Run-end report: the observation-equation check.
 *
 * Prints the counters of the check and the carrier frequency differences between the
 * row builder's, the ambiguity seed's and the identity layer's sources.
 *
 * Gate: prints under `config.obsEquationCheck()`.
 *
 * @param v The run at its end (network/runend/RunEnd.hpp).
 * @return 0 (this report does not set the exit code).
 */
int networkObsEquationCheckRunEnd(const NetworkRunEndView& v);

} // namespace GnssProcessor
