#pragma once
/**
 * @file BaardaReliability.hpp
 * @brief The ONE Baarda internal-reliability convention in this tree — the
 *        non-centrality @f$\delta_0@f$ and the minimal detectable bias (MDB) —
 *        shared verbatim by the PPP path and the network path.
 *
 * Origin: NGS LASER Guide v2.7 slide 5-97 (PDF p. 409), Snow (2002); Baarda
 * (1968) reliability theory.  See docs/SPEC_network_mdb_reliability.md.
 *
 * ## What the number means
 *
 * For a single row @f$i@f$ of a least-squares / Kalman update with a-priori
 * standard deviation @f$\sigma_i@f$ and local redundancy
 * @f$r_i = (\mathrm{Cov}\{v\}\,R^{-1})_{ii} \in [0,1]@f$, the SMALLEST fault in
 * that row that a w-test at critical value @f$k@f$ detects with power
 * @f$1-\beta@f$ is
 *
 *   @f[ \mathrm{MDB}_i \;=\; \frac{\sigma_i\,\delta_0}{\sqrt{r_i}},
 *       \qquad \delta_0 \;=\; k + \Phi^{-1}(1-\beta). @f]
 *
 * It is a property of the GEOMETRY and the stochastic model — the residuals do
 * not enter.  It answers the service question the census's UNCHECKABLE binary
 * only answers yes/no: **how large a fault could hide in this row right now?**
 *
 * ## Two conventions frozen here on purpose
 *
 *  - @f$\beta = 0.20@f$ (80 % power), i.e. @f$\Phi^{-1}(0.80) = 0.8416@f$.  The
 *    project has ONE power convention so a PPP MDB and a network MDB are
 *    commensurable by construction; there is deliberately no @f$\alpha/\beta@f$
 *    knob (docs/SPEC_network_mdb_reliability.md §7 records the deferral).
 *  - The a-priori variance of unit weight is @f$\sigma_0^2 = 1@f$ — every weight
 *    in this tree is absolute (@f$w = 1/\sigma^2@f$ in metres@f$^{-2}@f$), so no
 *    variance factor multiplies the formula.  The EMPIRICAL inflation is applied
 *    downstream and reported separately (`mdb_scaled`), never folded in here:
 *    D5 measured the formal @f$\sigma@f$ 10–30x optimistic on this data, and a
 *    silently pre-inflated MDB would make that optimism unauditable.
 *
 * ## The @f$r \to 0@f$ convention
 *
 * Below @ref kBaardaRedundancyFloor the row is UNCHECKABLE and its MDB is
 * @c +infinity — not a large finite number, and not a clamp.  A finite value
 * there would be a lie a consumer could act on: no residual-based test (post-fit
 * RAIM, @f$\hat\sigma_0@f$, robust weighting) can see that row's misfit at ANY
 * magnitude.  Callers must therefore treat non-finite MDB as its own category,
 * which is exactly what makes the "rows at infinity" count reconcilable against
 * the redundancy census's `uncheckable` population.
 *
 * @note This header is intentionally free of every project include: it is
 *       consumed by @c filter/KalmanFilter.cpp (the PPP path) and by
 *       @c filter/NetworkMdb.* (the network path), and a shared convention that
 *       drags dependencies is a shared convention people fork.
 */

#include <cmath>
#include <limits>

namespace GnssProcessor {

/// @f$\Phi^{-1}(0.80)@f$ — the standard-normal quantile for the project's fixed
/// 80 % test power (@f$\beta = 0.20@f$).  Frozen: see the header rationale.
inline constexpr double kBaardaPowerQuantile = 0.8416;

/// Local-redundancy floor below which a row is UNCHECKABLE and its MDB is
/// @c +infinity.  Guards the @f$1/\sqrt{r}@f$ division; also the exact boundary
/// the network RAIM pool uses to exclude a row from @f$\hat\sigma_0@f$
/// (@c NetworkRaim.cpp), so the two agree by construction rather than by luck.
inline constexpr double kBaardaRedundancyFloor = 1e-6;

/**
 * @brief Baarda's non-centrality @f$\delta_0 = k + \Phi^{-1}(1-\beta)@f$.
 *
 * @param critical_value The w-test critical value @f$k@f$ the run is configured
 *                       with (@c GnssConfig::dataSnoopingCritical, default 3.29
 *                       @f$\Rightarrow \delta_0 \approx 4.13@f$).
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
 *                   on the PPP path, @f$1/\sqrt{w_i}@f$ on the network path —
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
 * or non-finite weight yields a non-finite @f$\sigma@f$ so the caller can
 * COUNT the row as unusable rather than silently binning a fabricated MDB.
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
