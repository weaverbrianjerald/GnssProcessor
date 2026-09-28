#pragma once

/**
 * @file RobustStats.hpp
 * @brief Robust order-statistics shared across the cycle-slip detectors.
 *
 * Single implementation of the median and of the median-absolute-deviation (MAD)
 * Gaussian-equivalent sigma, used by the slip detectors (MW, GF, EWL, GRAPHIC,
 * the ionosphere-variability monitor), the IF-phase median helper and
 * TurboEditResolver.  Each caller keeps its own minimum-sample guard and
 * sentinel value; only the arithmetic lives here.
 *
 * Both functions take a range (or any container exposing std::begin/std::end),
 * copy it into a scratch buffer, and leave the source unmodified.
 */

#include <algorithm>
#include <cmath>
#include <iterator>
#include <limits>
#include <vector>

namespace GnssProcessor {
namespace RobustStats {

    /// Scale factor converting a MAD to a Gaussian-equivalent standard
    /// deviation for normally distributed data: 1 / Φ⁻¹(3/4) ≈ 1.4826.
    constexpr double kMadToSigma = 1.4826;

    /**
     * @brief Median of the values in [first, last).
     *
     * For an even count returns the mean of the two central order statistics.
     * The source range is not modified (values are copied into a scratch
     * buffer).
     *
     * @return The median, or NaN if the range is empty.
     */
    template <typename InputIt>
    inline double median(InputIt first, InputIt last) {
        std::vector<double> buf(first, last);
        if (buf.empty()) return std::numeric_limits<double>::quiet_NaN();
        std::sort(buf.begin(), buf.end());
        const std::size_t n = buf.size();
        return (n % 2 == 0) ? 0.5 * (buf[n / 2 - 1] + buf[n / 2])
                            : buf[n / 2];
    }

    /// @brief Median of any container exposing std::begin / std::end.
    template <typename Container>
    inline double median(const Container& values) {
        using std::begin;
        using std::end;
        return median(begin(values), end(values));
    }

    /**
     * @brief Gaussian-equivalent sigma from the median absolute deviation:
     *        @c kMadToSigma · median(|xᵢ − median(x)|).
     *
     * Robust to outliers (a minority of gross errors does not move the MAD).
     * The source range is not modified.
     *
     * @return The MAD-based sigma, or NaN if the range is empty.
     */
    template <typename InputIt>
    inline double madSigma(InputIt first, InputIt last) {
        std::vector<double> buf(first, last);
        if (buf.empty()) return std::numeric_limits<double>::quiet_NaN();
        const double med = median(buf.begin(), buf.end());
        for (double& v : buf) v = std::abs(v - med);
        return kMadToSigma * median(buf.begin(), buf.end());
    }

    /// @brief MAD-based sigma of any container exposing std::begin / std::end.
    template <typename Container>
    inline double madSigma(const Container& values) {
        using std::begin;
        using std::end;
        return madSigma(begin(values), end(values));
    }

}  // namespace RobustStats
}  // namespace GnssProcessor
