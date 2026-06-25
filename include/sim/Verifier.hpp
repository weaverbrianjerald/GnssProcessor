#pragma once

/**
 * @file Verifier.hpp
 * @brief Grading oracle: diff the processor's recovered solution against truth.
 *
 * Closes the simulate->verify loop in-process: joins the synthesized answer key
 * (truth_position.csv + truth_scalars.csv) against the real @c gnss_processor run
 * traces (state_trace.csv + measurement_residuals.csv) on @c (week,tow_s) and
 * grades the recovered dual-frequency PPP solution against the DESIGN §10.3
 * tolerances — position accuracy, k-sigma covariance coverage, GPS->Galileo ISB,
 * ZWD, storm-window flatness, and a non-zero residual floor (the anti-circularity
 * check).  The same gates serve as the professor's self-consistency battery and
 * as the per-submission grading oracle.
 */

#include <filesystem>
#include <string>
#include <vector>

namespace GnssProcessor {
namespace sim {

/** @brief Inputs and tolerances for one verification run. */
struct VerifierConfig {
    std::filesystem::path truth_dir;    ///< Holds truth_position.csv + truth_scalars.csv.
    std::filesystem::path run_logs_dir; ///< Holds state_trace.csv (+ measurement_residuals.csv).
    double k_sigma{ 3.0 };              ///< Coverage multiplier.
    int converge_skip{ 30 };            ///< Leading epochs excluded from steady-state stats.
};

/** @brief Outcome of a verification run: a PASS/FAIL plus a human report. */
struct VerifierResult {
    bool pass{ false };
    int failures{ 0 };
    std::vector<std::string> report; ///< One line per check (printed by the caller).
};

/** @brief Stateless grader. */
class Verifier {
public:
    /** @brief Run all gates; never throws (errors surface as failures in the result). */
    static VerifierResult run(const VerifierConfig& cfg);
};

} // namespace sim
} // namespace GnssProcessor
