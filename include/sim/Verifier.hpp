#pragma once

/**
 * @file Verifier.hpp
 * @brief Grading oracle: diff the processor's recovered solution against truth.
 *
 * Closes the simulate->verify loop in-process: joins the synthesized answer key
 * (truth_position.csv + truth_scalars.csv) against the real @c gnss_processor run
 * traces (state_trace.csv + measurement_residuals.csv) on @c (week,tow_s) and
 * grades the recovered dual-frequency PPP solution against the DESIGN §10.3
 * tolerances: position accuracy, GPS->Galileo ISB, ZWD, storm-window flatness, and
 * two anti-circularity checks (the share of epochs with 3D error below 0.3 sigma,
 * and a non-zero phase residual floor).  k-sigma covariance coverage is recorded
 * as an informational check with no threshold.  The same gates serve as the
 * simulator's self-consistency battery and as the per-submission grading oracle.
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

/**
 * @brief One machine-readable gate outcome, recorded next to its report line.
 *
 * Populated at the site that emits the human report line, so the JSON verdict
 * and the printed verdict cannot diverge.  @c value / @c gate are the
 * quantity and its threshold (a tolerance-style gate stores the error as
 * @c value and the tolerance as @c gate); @c gate == 0 marks an informational
 * check with no threshold (e.g. coverage %).
 */
struct Check {
    std::string id;       ///< Stable slug: position_median, isb, storm_flatness, ...
    bool pass{ true };    ///< True when the gate is met; always true for an informational check (gate == 0).
    double value{ 0.0 };  ///< Measured quantity (the error, for a tolerance-style gate), in @c units.
    double gate{ 0.0 };   ///< Threshold for @c value; 0 marks an informational check.
    std::string units;    ///< Unit label of @c value and @c gate (e.g. "m", "%", "mm").
    std::string detail;   ///< Free-text qualifier shown with the check; may be empty.
};

/** @brief Outcome of a verification run: a PASS/FAIL plus a human report. */
struct VerifierResult {
    bool pass{ false };   ///< True when no gate failed (@c failures == 0).
    int failures{ 0 };    ///< Number of failed gates.
    std::vector<std::string> report; ///< One line per check (printed by the caller).
    std::vector<Check> checks;       ///< Structured per-gate outcomes (for verify_report.json).
    double k_sigma{ 3.0 };           ///< Echo of the coverage multiplier used.
    int converge_skip{ 30 };         ///< Echo of the leading epochs skipped.
};

/** @brief Stateless grader. */
class Verifier {
public:
    /**
     * @brief Run all gates; never throws (errors surface as failures in the result).
     * @param cfg Truth and run-log directories, coverage multiplier and the number of
     *            leading epochs to skip.
     * @return Overall pass flag, failure count, one report line per check and the
     *         structured per-gate outcomes; a missing input file yields a failed result
     *         rather than an exception.
     */
    static VerifierResult run(const VerifierConfig& cfg);

    /**
     * @brief Write @p res to @p path as verify_report.json (SPEC §4.6).
     *
     * Additive output read by the pyviz dashboard; it does not re-grade.  Emits
     * @c {pass, failures, k_sigma, converge_skip, checks[], report_lines[]}.
     * Returns false only on a file-open error (non-fatal to the caller).
     *
     * @param res  Result of @c run to serialize.
     * @param path Output JSON file path.
     * @return True when the file was written; false when it could not be opened.
     */
    static bool writeJsonReport(const VerifierResult& res, const std::filesystem::path& path);
};

} // namespace sim
} // namespace GnssProcessor
