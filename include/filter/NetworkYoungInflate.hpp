/**
 * @file NetworkYoungInflate.hpp
 * @brief Age-aware variance inflation of freshly re-levelled phase-leveling arcs
 *        (@c network.phase_leveling_young_inflate_var_m2), SPEC_amb_iono_split_overcount
 *        §6.17–§6.18.
 *
 * ## The defect addressed
 *
 * A detector slip response frees the arc for one epoch
 * (@c network.cycle_slip.relevel_free_epoch) and the next harvest re-stashes it at that
 * epoch's posterior.  The stashed level's value has code quality (0.2–0.4 m off in the
 * truth census of the slipped 60-station bed), while its harvested variance is
 * phase-stiff, because the conditional @f$(N_{rr}^{-1})_{cc}@f$ it is read from is
 * conditioned on the station's mature arcs.  The arc is therefore inconsistent with
 * geometry until about 20 min of geometry change refines it.  Meanwhile the T4
 * attribution tier reads that inconsistency as a slip and erases the arc, the next
 * harvest re-levels it from code again, and the repeated erase and re-level sustains a
 * metre-class ZWD/clock plateau.
 *
 * ## Why this shape
 *
 * Uniform variance floors (1e-4 / 1e-3 m²) and a uniform level process noise
 * (1e-7 / 1e-6 m²/s) were both falsified on this bed: too small to matter, and applied
 * to every arc, mature ones included.  Keeping young legs free (out of the F3 block
 * entirely; arm 5b) was falsified on the quiet bed's post-eclipse legs.  This inflation
 * differs on both counts:
 *
 *  - only arcs young since their last re-level are touched.  The age clock is
 *    @ref LocalCarry::relevel_s, stamped at creation and at every free-epoch response
 *    (@ref localCarryStampRelevel, NetworkRunTypes.hpp), not @ref LocalCarry::birth_s
 *    (which a re-level deliberately preserves);
 *  - the inflated arc stays inside the F3 whitened block with its cross terms.  The
 *    inflation is applied as a congruence scaling of the arc's row/column of the
 *    harvested Σ (@ref congruence), which keeps Σ positive-definite and leaves every
 *    correlation coefficient exactly unchanged; only the arc's own marginal loosens;
 *  - the inflation starts at code level (V0 ≈ (0.3 m)²) and decays with age, so it
 *    states what the level is actually worth rather than acting as a tuning constant.
 *
 * ## Interlocks
 *
 * Requires @c network.phase_leveling (otherwise there is no leveling carry to inflate);
 * disabled under @c network.carry_invariant (the invariant DD carry of Card 4 is a different
 * vehicle and the leveling inject is skipped outright).  The loader disables the knob
 * with a warning in both cases.  Works with or without F3: off the block the inflation
 * is a plain @c var_eff += var_add on the per-column diagonal row.
 * @c var_m2 == 0 (the default) is off and structurally byte-identical.
 */
#ifndef GNSS_NETWORK_YOUNG_INFLATE_HPP
#define GNSS_NETWORK_YOUNG_INFLATE_HPP

#include <Eigen/Dense>

#include <cmath>
#include <cstddef>
#include <vector>

namespace GnssProcessor {
namespace NetworkYoungInflate {

/// The three knobs, as the PASS-B inject reads them.
struct Knobs {
    /// @c network.phase_leveling_young_inflate_var_m2: the variance added at age 0
    /// (m²).  0 = off.  0.09 = (0.3 m)², the measured code-level error of a
    /// one-epoch re-level.
    double var_m2{0.0};
    /// @c network.phase_leveling_young_inflate_tau_ep: e-folding age in loop epochs.
    double tau_ep{20.0};
    /// @c network.phase_leveling_young_inflate_max_age_ep: beyond this age no
    /// inflation is applied at all (and the column is counted as age-skipped).
    int max_age_ep{60};

    /// The live predicate.  A non-positive τ makes the exponential meaningless, so it
    /// switches the inflation off rather than degrading silently to a step function.
    /// The loader already rejects it; this check is the second guard.
    bool on() const { return var_m2 > 0.0 && tau_ep > 0.0 && max_age_ep > 0; }
};

/// One column's inflation, with the age it was evaluated at.
struct Add {
    double var_add{0.0};     ///< the variance to add (m²); 0 when nothing applies.
    double age_ep{0.0};      ///< (now_s − relevel_s) / epoch_s, in loop epochs.
    bool   inflated{false};  ///< a strictly positive @c var_add was produced.
    bool   over_age{false};  ///< a stamped column refused because age > max_age_ep.
};

/// @brief @f$V_{add} = V_0\,e^{-age/\tau}@f$ for a stamped, young-enough column.
///
/// @param k          the knobs.
/// @param relevel_s  the column's @ref LocalCarry::relevel_s.  ≤ 0 means unstamped
///                   (a warm-resume-restored entry, or a run whose stamping predates
///                   this inflation): treated as mature, never inflated and never
///                   counted as age-skipped, the same "0 ⇒ mature" rule @c birth_ep
///                   uses.
/// @param now_s      this epoch's @c pl_now_s.
/// @param epoch_s    the run's own observed inter-epoch spacing (s).  ≤ 0 (first
///                   epoch / implausible spacing) makes the age unknowable, so
///                   nothing is inflated: no division by zero and no assumed
///                   nominal rate.
inline Add varAdd(const Knobs& k, double relevel_s, double now_s, double epoch_s) {
    Add a;
    if (!k.on()) return a;
    if (!(relevel_s > 0.0)) return a;                 // unstamped ⇒ mature
    if (!(epoch_s > 0.0) || !std::isfinite(epoch_s)) return a;
    const double dt = now_s - relevel_s;
    if (!std::isfinite(dt) || dt < 0.0) return a;     // clock went backwards ⇒ refuse
    a.age_ep = dt / epoch_s;
    if (a.age_ep > static_cast<double>(k.max_age_ep)) { a.over_age = true; return a; }
    const double v = k.var_m2 * std::exp(-a.age_ep / k.tau_ep);
    if (!(v > 0.0) || !std::isfinite(v)) return a;
    a.var_add  = v;
    a.inflated = true;
    return a;
}

/// @brief Apply the per-column inflation to a harvested block covariance as a
///        congruence, in place.
///
/// For every column @f$j@f$ with @f$a_j > 0@f$ let
/// @f$f_j = \sqrt{(\Sigma_{jj} + a_j)/\Sigma_{jj}} \ge 1@f$ (and @f$f_j = 1@f$
/// otherwise); the transform is @f$\Sigma \leftarrow D\,\Sigma\,D@f$ with
/// @f$D = \mathrm{diag}(f_j)@f$.  Three properties, which together are the reason
/// for this shape instead of adding to the diagonal:
///
///  - **the target diagonal is inflated exactly**:
///    @f$\Sigma'_{jj} = f_j^2\Sigma_{jj} = \Sigma_{jj} + a_j@f$;
///  - **PD is preserved**: @f$D@f$ is diagonal with strictly positive entries, so
///    @f$D\Sigma D@f$ is a congruence transform of @f$\Sigma@f$ and Sylvester's law
///    of inertia gives @f$\Sigma \succ 0 \Rightarrow \Sigma' \succ 0@f$.  Adding
///    @f$a_j@f$ to the diagonal alone would also preserve PD, but it would change
///    the correlations (next point);
///  - **every correlation coefficient is unchanged**:
///    @f$\rho'_{jk} = f_jf_k\Sigma_{jk}/\sqrt{f_j^2\Sigma_{jj}\,f_k^2\Sigma_{kk}}
///       = \rho_{jk}@f$.  A freshly re-levelled arc is not less correlated with its
///    station's other columns; it is worth less on its own, and the joint whitening
///    keeps the coupling it has.
///
/// @param Sigma    the block covariance (n×n, symmetric); modified in place.
/// @param var_add  per-column additions (size n); entries ≤ 0 leave that column alone.
/// @return the number of columns actually scaled (0 ⇒ @p Sigma is untouched).
inline std::size_t congruence(Eigen::MatrixXd& Sigma,
                              const std::vector<double>& var_add) {
    const Eigen::Index n = Sigma.rows();
    if (n <= 0 || static_cast<Eigen::Index>(var_add.size()) != n) return 0;
    Eigen::VectorXd f = Eigen::VectorXd::Ones(n);
    std::size_t scaled = 0;
    for (Eigen::Index j = 0; j < n; ++j) {
        const double a = var_add[static_cast<std::size_t>(j)];
        const double d = Sigma(j, j);
        if (!(a > 0.0) || !(d > 0.0) || !std::isfinite(a) || !std::isfinite(d)) continue;
        const double fj = std::sqrt((d + a) / d);
        if (!std::isfinite(fj) || !(fj > 0.0)) continue;
        f(j) = fj;
        ++scaled;
    }
    if (scaled == 0) return 0;   // off path: no write to Sigma
    for (Eigen::Index j = 0; j < n; ++j) {
        if (f(j) == 1.0) continue;
        Sigma.row(j) *= f(j);
        Sigma.col(j) *= f(j);
    }
    return scaled;
}

}  // namespace NetworkYoungInflate
}  // namespace GnssProcessor

#endif  // GNSS_NETWORK_YOUNG_INFLATE_HPP
