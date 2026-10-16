#pragma once
/**
 * @file PivotFrameQ.hpp
 * @brief The carried satellite clocks' process noise expressed in the pivot frame
 *        (docs/refound/PFQ_pivot_frame_process_noise_2026-10-08.md,
 *        @c network.clock_pivot_frame_q).
 *
 * The clock datum pins one satellite clock per constellation every epoch
 * (@c SbasisDeclaration::PerConstellationSatClockPin), so a carried clock is the
 * pivot-frame quantity @f$x_j = c_j - c_p@f$.  If each satellite's correction walks
 * independently, @f$\mathrm{Var}\,\Delta c_j = r_j\,\Delta t_j@f$, the state equation in
 * that basis is the S-transform of the absolute one (Snow, *Adjustment Computations*,
 * 15.9 with the dynamic linear model of chapter 18: @f$Q_S = S\,Q\,S^\top@f$).  Over the
 * interval @f$[t-\Delta t_j,\,t]@f$ that clock @f$j@f$'s own increment covers,
 *
 * @f[ \mathrm{Var}\,\Delta x_j = r_j\,\Delta t_j + r_p\,\Delta t_j, \qquad
 *     \mathrm{Cov}(\Delta x_j, \Delta x_k) = r_p \min(\Delta t_j, \Delta t_k), @f]
 *
 * the second term being the pivot's walk over the overlap of the two intervals (both
 * end at the fold epoch).  The pivot itself is zero by definition and has no increment.
 * The receiver clocks absorb the pivot's level, so the phase and code biases carry no
 * such term.
 *
 * The producer's fold already adds @f$r_j\,\Delta t_j@f$; these helpers give the two
 * missing pieces, so every site that forms the clock prior (the fold decision, the
 * dense fold and the exact coupling update) uses one formula.
 */

#include <algorithm>

namespace GnssProcessor {

/// The pivot-frame diagonal addition of a carried clock: @f$r_p\,\Delta t_j@f$ (m^2).
/// @param rate_p the pivot's own process-noise rate (m^2/s).
/// @param dt_j   the interval the clock's own increment covers (s); a negative value is 0.
inline double pivotFrameDiag(double rate_p, double dt_j) {
    return (rate_p > 0.0) ? rate_p * std::max(0.0, dt_j) : 0.0;
}

/// The pivot-frame covariance of two carried clocks of the pivot's constellation:
/// @f$r_p \min(\Delta t_a, \Delta t_b)@f$ (m^2).  Equals @ref pivotFrameDiag for a = b.
inline double pivotFrameCross(double rate_p, double dt_a, double dt_b) {
    return (rate_p > 0.0) ? rate_p * std::max(0.0, std::min(dt_a, dt_b)) : 0.0;
}

/// Run totals of the pivot-frame process noise term (standing invariant 4: an armed term must be countable).
struct PivotFrameCounters {
    long long epochs{0};          ///< decision passes the term was armed on
    long long keys{0};            ///< clock key-epochs given the diagonal term
    long long pivot_anchored{0};  ///< constellation-epochs with a carried pivot folded at the anchor's q
    long long pivot_loose{0};     ///< constellation-epochs with a carried pivot folded at the legacy (loose) rate
    long long pivot_model{0};     ///< constellation-epochs whose pivot is not carried (the pinned pivot, the usual case): the model's physical rate (the anchor's q when armed, else the legacy rate)
    long long dense_pairs{0};     ///< clock pairs given the pairwise term in the C1 dense fold
    long long cxu_pairs{0};       ///< ordered clock pairs given the pairwise term in the exact update's Q_S
    double    add_sum_m2{0.0};    ///< summed diagonal additions (m^2)
    double    rate_max{0.0};      ///< the largest pivot rate used (m^2/s)
};

}  // namespace GnssProcessor
