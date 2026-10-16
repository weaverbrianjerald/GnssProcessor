#pragma once
/**
 * @file BaardaReliability.hpp
 * @brief The single Baarda internal-reliability convention in this tree: the
 *        non-centrality @f$\delta_0@f$ and the minimal detectable bias (MDB),
 *        shared unchanged by the PPP path and the network path.
 *
 * Origin: NGS LASER Guide v2.7 slide 5-97 (PDF p. 409), Snow (2002); Baarda
 * (1968) reliability theory.  See docs/SPEC_redundancy_census.md.
 *
 * ## Meaning
 *
 * For a single row @f$i@f$ of a least-squares or Kalman update with a-priori
 * standard deviation @f$\sigma_i@f$ and local redundancy
 * @f$r_i = (\mathrm{Cov}\{v\}\,R^{-1})_{ii} \in [0,1]@f$, the smallest fault in
 * that row that a w-test at critical value @f$k@f$ detects with power
 * @f$1-\beta@f$ is
 *
 *   @f[ \mathrm{MDB}_i \;=\; \frac{\sigma_i\,\delta_0}{\sqrt{r_i}},
 *       \qquad \delta_0 \;=\; k + \Phi^{-1}(1-\beta). @f]
 *
 * It depends on the geometry and the stochastic model only; the residuals do
 * not enter.  The redundancy census's uncheckable flag says only whether a row
 * can be tested; the MDB gives the size of fault that could remain undetected
 * in the row at the current epoch.
 *
 * ## Two fixed conventions
 *
 *  - @f$\beta = 0.20@f$ (80 % power), i.e. @f$\Phi^{-1}(0.80) = 0.8416@f$.  The
 *    project uses one power convention so that a PPP MDB and a network MDB are
 *    directly comparable; there is no @f$\alpha/\beta@f$ knob
 *    (docs/SPEC_redundancy_census.md §11.7 records the deferral).
 *  - The a-priori variance of unit weight is @f$\sigma_0^2 = 1@f$: every weight
 *    in this tree is absolute (@f$w = 1/\sigma^2@f$ in metres@f$^{-2}@f$), so no
 *    variance factor multiplies the formula.  The empirical inflation is applied
 *    downstream and reported separately (`mdb_scaled`), not folded in here.  The
 *    formal @f$\sigma@f$ has been measured 10–30x optimistic on field data (card
 *    D5), and keeping the formal MDB uninflated keeps that factor auditable.
 *
 * ## The @f$r \to 0@f$ convention
 *
 * At or below @ref kBaardaRedundancyFloor the row is uncheckable and its MDB is
 * @c +infinity, neither a large finite number nor a clamp.  No residual-based
 * test (post-fit RAIM, @f$\hat\sigma_0@f$, robust weighting) can see that row's
 * misfit at any magnitude, so a finite value would be misleading to a consumer.
 * Callers treat a non-finite MDB as its own category; the count of rows at
 * infinity then reconciles against the redundancy census's `uncheckable`
 * population.
 *
 * @note This header has no project includes.  It is consumed by
 *       @c filter/KalmanFilter.cpp (the PPP path) and by @c filter/NetworkMdb.*
 *       (the network path), and keeping it dependency-free keeps both paths on
 *       one copy of the convention.
 */

#include <cmath>
#include <limits>

namespace GnssProcessor {

/// @f$\Phi^{-1}(0.80)@f$: the standard-normal quantile for the project's fixed
/// 80 % test power (@f$\beta = 0.20@f$).  Fixed; see the file header.
inline constexpr double kBaardaPowerQuantile = 0.8416;

/// Local-redundancy floor at or below which a row is uncheckable and its MDB is
/// @c +infinity.  Guards the @f$1/\sqrt{r}@f$ division.  The network RAIM pool
/// (@c Raim.cpp) uses the same constant to exclude a row from
/// @f$\hat\sigma_0@f$, so the two boundaries agree by construction.
inline constexpr double kBaardaRedundancyFloor = 1e-6;

/**
 * @brief Baarda's non-centrality @f$\delta_0 = k + \Phi^{-1}(1-\beta)@f$.
 *
 * @param critical_value The w-test critical value @f$k@f$ the run is configured
 *                       with (@c GnssConfig::dataSnoopingCritical, default 3.29,
 *                       giving @f$\delta_0 \approx 4.13@f$).
 * @return The non-centrality parameter at the project's fixed 80 % power.
 */
inline double baardaDelta0(double critical_value) {
    return critical_value + kBaardaPowerQuantile;
}

/**
 * @brief The minimal detectable bias of one row, in the row's own units
 *        (metres for every row in this tree).
 *
 * @param sigma_i    A-priori standard deviation of the row (@f$\sqrt{R_{ii}}@f$
 *                   on the PPP path, @f$1/\sqrt{w_i}@f$ on the network path;
 *                   see @ref baardaSigmaFromWeight).
 * @param redundancy Local redundancy @f$r_i@f$ of the row, in [0,1].
 * @param delta_0    Non-centrality from @ref baardaDelta0.
 * @return @f$\sigma_i \delta_0/\sqrt{r_i}@f$, or @c +infinity when
 *         @f$r_i \le@f$ @ref kBaardaRedundancyFloor.
 */
inline double baardaMinimalDetectableBias(double sigma_i, double redundancy,
                                          double delta_0) {
    return (redundancy > kBaardaRedundancyFloor)
               ? sigma_i * delta_0 / std::sqrt(redundancy)
               : std::numeric_limits<double>::infinity();
}

/**
 * @brief Row @f$\sigma@f$ from an assembled information weight.
 *
 * The network assemblies carry @f$w_i = 1/\sigma_i^2@f$ directly (absolute
 * units, no variance factor), so this is the whole conversion.  A non-positive
 * or non-finite weight yields NaN, so the caller can count the row as unusable
 * instead of binning an MDB computed from an invalid weight.
 *
 * @param w Information weight of the row.
 * @return @f$1/\sqrt{w}@f$, or NaN when @p w is not a usable positive weight.
 */
inline double baardaSigmaFromWeight(double w) {
    return (std::isfinite(w) && w > 0.0)
               ? 1.0 / std::sqrt(w)
               : std::numeric_limits<double>::quiet_NaN();
}

}  // namespace GnssProcessor
