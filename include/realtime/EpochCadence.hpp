#pragma once
/**
 * @file EpochCadence.hpp
 * @brief Measured epoch spacing of a live stream and the data-gap threshold
 *        derived from it.
 *
 * A post-processing run knows its observation interval from the RINEX header.
 * A real-time stream has no header, so the engine measures the spacing of the
 * epochs it accepts and takes the median of the most recent spacings as the
 * nominal interval.  The median ignores an isolated outage or a stray epoch, and
 * follows a lasting change of rate within a few epochs.
 *
 * The data-gap threshold is the configured @c processing.data_gap_tolerance_s
 * when it is positive, and otherwise 1.5 nominal intervals, the same default the
 * post-processing path applies, but never below @ref kFloorToleranceS.  The floor
 * is the fixed 1 s the engine used before the interval was measured: a stream of
 * 1.5 Hz or faster keeps that threshold, so one dropped epoch at 10 Hz is not a
 * gap that re-seeds the receiver clock from a single epoch of code.  The
 * threshold is @ref kFloorToleranceS too until a first spacing has been measured.
 */

#include <algorithm>
#include <array>
#include <cstddef>

namespace GnssProcessor {

/**
 * @class EpochCadence
 * @brief Sliding window of accepted epoch spacings with its median.
 *
 * A plain value type: it owns only the window.  The caller decides when a
 * spacing is observed; the engine calls @ref observe once per accepted,
 * advancing epoch, after it has asked @ref gapToleranceS about that same
 * spacing, so a spacing is never judged against a window that contains it.
 */
class EpochCadence {
public:
    /// Number of most recent spacings the median is taken over.
    static constexpr std::size_t kWindow = 9;

    /// Multiple of the nominal interval beyond which a spacing is a data gap.
    static constexpr double kGapFactor = 1.5;

    /// Lowest gap threshold (s): the fixed 1 s the real-time engine used for
    /// every stream.  It is the threshold until a first spacing has been
    /// measured, which still flags the first spacing after a warm resume (it
    /// spans the whole outage), and the lower bound once the interval is known,
    /// so the default never reads a fast stream more tightly than before.
    static constexpr double kFloorToleranceS = 1.0;

    /**
     * @brief Record one accepted epoch spacing.
     * @param dt_s Seconds from the previous accepted epoch (> 0); other values are ignored.
     */
    void observe(double dt_s) {
        if (!(dt_s > 0.0)) return;
        window_[next_] = dt_s;
        next_ = (next_ + 1) % kWindow;
        if (count_ < kWindow) ++count_;
    }

    /// Number of spacings currently in the window (at most @ref kWindow).
    std::size_t sampleCount() const { return count_; }

    /**
     * @brief The nominal epoch interval.
     * @return Median of the window in seconds (the lower middle value for an
     *         even count), or 0 while the window is empty.
     */
    double nominalIntervalS() const {
        if (count_ == 0) return 0.0;
        std::array<double, kWindow> sorted = window_;
        std::sort(sorted.begin(), sorted.begin() + static_cast<std::ptrdiff_t>(count_));
        return sorted[(count_ - 1) / 2];
    }

    /**
     * @brief The data-gap threshold for the next spacing.
     *
     * @param configured_s The configured @c processing.data_gap_tolerance_s; a
     *                     value above 0 is used as given.
     * @return The threshold in seconds: @p configured_s when positive, else
     *         @ref kGapFactor times the nominal interval but at least
     *         @ref kFloorToleranceS, and @ref kFloorToleranceS while no spacing
     *         has been measured.
     */
    double gapToleranceS(double configured_s) const {
        if (configured_s > 0.0) return configured_s;
        const double nominal = nominalIntervalS();
        return std::max(kFloorToleranceS, kGapFactor * nominal);
    }

private:
    std::array<double, kWindow> window_{};
    std::size_t                 next_{0};
    std::size_t                 count_{0};
};

} // namespace GnssProcessor
