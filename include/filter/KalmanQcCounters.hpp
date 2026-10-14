#pragma once

/**
 * @file KalmanQcCounters.hpp
 * @brief The run-end counters of the KalmanFilter's post-fit QC events, declared once.
 *
 * Standing invariant 4 of the project: run-end counters are the on-path telemetry.  A
 * QC branch that never acts looks the same in a clean log as one that acts silently, and
 * on the user side most of these events log at info or debug, which the shipped configs
 * hide (they set the kalman_filter level to warn).  Each event of the post-fit screen and
 * of its responses increments one member where it happens (card UOM step U2e), and
 * KalmanFilter::logKalmanFilterTimings prints the block once at the end of the run, beside
 * the timing summary, from the rows of @ref kKalmanQcCounterRows.
 *
 * Plain value types and a constant table; the counting lives in the KalmanFilter files.
 */

#include <cstddef>
#include <string>

namespace GnssProcessor {

    /**
     * @brief Counts of the post-fit QC events of one run (or since the last print).
     */
    struct KalmanQcCounters {
        std::size_t pivot_group_slips = 0;         ///< Phase pivot-offset groups that fired
                                                   ///< (per signal or across bands).
        std::size_t pivot_code_disturbances = 0;   ///< Code pivot-offset groups that fired.
        std::size_t nonpivot_multiband_slips = 0;  ///< Satellites reset by the non-pivot
                                                   ///< multi-band slip detector.
        std::size_t huber_rows_queued = 0;         ///< Huber R targets queued or raised.
        std::size_t snooping_rejections = 0;       ///< Gated rows chosen by the Baarda w-test.
        std::size_t phase_rows_reset = 0;          ///< Phase rows sent to the ambiguity reset
                                                   ///< (the gated row and its correlated rows).
        std::size_t phase_rows_floored = 0;        ///< Phase rows only excluded, under the
                                                   ///< sub-cycle wavelength floor.
        std::size_t code_rows_excluded = 0;        ///< Code rows excluded by the dispatch.
        std::size_t mw_tiebreak_overrides = 0;     ///< Dispatches the MW tiebreak moved off the
                                                   ///< largest standardized residual.
        std::size_t complete_resets = 0;           ///< Complete-reset backstops.
        std::size_t stuck_bias_resets = 0;         ///< Stuck satellite code biases reset after
                                                   ///< the commit.
    };

    /**
     * @brief One printed row of the counters block: its label and the member it reads.
     */
    struct KalmanQcCounterRow {
        const char* label;                         ///< Label, at most 30 characters.
        std::size_t KalmanQcCounters::* member;    ///< The counter it prints.
    };

    /// The rows of the run-end block, in the order of the post-fit screen's stages.
    inline constexpr KalmanQcCounterRow kKalmanQcCounterRows[] = {
        { "pivot group slips (phase)",      &KalmanQcCounters::pivot_group_slips },
        { "pivot code disturbances",        &KalmanQcCounters::pivot_code_disturbances },
        { "non-pivot multi-band slips",     &KalmanQcCounters::nonpivot_multiband_slips },
        { "Huber R targets queued",         &KalmanQcCounters::huber_rows_queued },
        { "data-snooping rejections",       &KalmanQcCounters::snooping_rejections },
        { "phase rows reset",               &KalmanQcCounters::phase_rows_reset },
        { "phase rows floored (sub-cycle)", &KalmanQcCounters::phase_rows_floored },
        { "code rows excluded",             &KalmanQcCounters::code_rows_excluded },
        { "MW tiebreak overrides",          &KalmanQcCounters::mw_tiebreak_overrides },
        { "complete resets",                &KalmanQcCounters::complete_resets },
        { "stuck code-bias resets",         &KalmanQcCounters::stuck_bias_resets },
    };

    /**
     * @brief The counters as the run-end block: a title line, then one row per entry of
     *        @ref kKalmanQcCounterRows ("  <label padded to 30> : <count>").
     *
     * @param c The counters.
     * @return The block, rows separated by newlines, without a trailing newline.
     */
    inline std::string formatKalmanQcCounters(const KalmanQcCounters& c) {
        std::string out = "KF QC event counters (post-fit screen and its responses):";
        for (const KalmanQcCounterRow& row : kKalmanQcCounterRows) {
            std::string label = row.label;
            if (label.size() < 30) label.append(30 - label.size(), ' ');
            out += "\n  " + label + " : " + std::to_string(c.*row.member);
        }
        return out;
    }

} // namespace GnssProcessor
