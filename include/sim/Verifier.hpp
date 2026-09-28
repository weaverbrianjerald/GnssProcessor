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
    bool pass{ true };
    double value{ 0.0 };
    double gate{ 0.0 };
    std::string units;
    std::string detail;
};

/** @brief Outcome of a verification run: a PASS/FAIL plus a human report. */
struct VerifierResult {
    bool pass{ false };
    int failures{ 0 };
    std::vector<std::string> report; ///< One line per check (printed by the caller).
    std::vector<Check> checks;       ///< Structured per-gate outcomes (for verify_report.json).
    double k_sigma{ 3.0 };           ///< Echo of the coverage multiplier used.
    int converge_skip{ 30 };         ///< Echo of the leading epochs skipped.
};

/** @brief Stateless grader. */
class Verifier {
public:
    /** @brief Run all gates; never throws (errors surface as failures in the result). */
    static VerifierResult run(const VerifierConfig& cfg);

    /**
     * @brief Write @p res to @p path as verify_report.json (SPEC §4.6).
     *
     * Additive output read by the pyviz dashboard; it does not re-grade.  Emits
     * @c {pass, failures, k_sigma, converge_skip, checks[], report_lines[]}.
     * Returns false only on a file-open error (non-fatal to the caller).
     */
    static bool writeJsonReport(const VerifierResult& res, const std::filesystem::path& path);
};

} // namespace sim
} // namespace GnssProcessor
