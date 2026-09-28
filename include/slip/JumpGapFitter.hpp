/**
 * @file JumpGapFitter.hpp
 * @brief Pure, stateless both-sides trend+step least-squares estimator.
 *
 * Fits an independent first-order polynomial @f$ y = a + b\,(t - t_b) @f$ to a
 * "pre" window and a "post" window of samples bracketing a boundary epoch
 * @f$ t_b @f$, then reports the discontinuity (step) of the extrapolated level
 * at the boundary together with the per-side slopes and a combined residual
 * RMS.  This is the non-causal kernel behind the smoother cycle-slip bridge:
 * given geometry-free or Melbourne-Wübbena series on both sides of a flagged
 * reset, it recovers the jump from future as well as past data, which a causal
 * detector cannot use.  Mirrors the NGS M-PAGES @c fitGap two-sided window fit
 * (DbEdit.cpp), generalized to arbitrary @f$(t, y)@f$ and free of GNSS
 * dependencies.
 *
 * Numerics: the time axis is centred on @f$ t_b @f$ before the normal-equation
 * solve, so the level at the boundary is simply the intercept @f$ a @f$ and the
 * 2x2 system stays well conditioned even for raw continuous-GPS-second
 * timestamps (~1.4e9).
 */
#pragma once

#include <vector>

namespace GnssProcessor {

/**
 * @brief Stateless two-sided linear trend + step fitter.
 *
 * No GNSS dependencies: operates on raw @f$(t, y)@f$ pairs in any consistent
 * units (the bridge feeds seconds and metres).  A single instance is reusable.
 * fitStep() is const and reads only @c min_window_points, so concurrent calls on
 * one instance are safe while @c min_window_points is not being modified.
 */
struct JumpGapFitter {
    /**
     * @brief One side of the fit: parallel time / value samples.
     *
     * @c t_s and @c y_m must be the same length; entries are consumed in order
     * but order is irrelevant to the least-squares solution.
     */
    struct Window {
        std::vector<double> t_s;  ///< Sample times (same epoch base as @c t_boundary_s).
        std::vector<double> y_m;  ///< Sample values (e.g. GF or MW, in metres).
    };

    /**
     * @brief Result of a two-sided fit.
     *
     * All level/step fields are in the input @c y units; slopes are in
     * @c y-units per @c t-unit.  Inspect @c ok before using any field.
     */
    struct FitResult {
        bool ok{false};          ///< True iff both sides fit cleanly (see fitStep()).
        double pre_level_m{0};   ///< Pre-side level extrapolated to @f$ t_b @f$.
        double post_level_m{0};  ///< Post-side level extrapolated to @f$ t_b @f$.
        double step_m{0};        ///< Discontinuity @f$ \text{post\_level} - \text{pre\_level} @f$.
        double pre_slope{0};     ///< Pre-side trend slope.
        double post_slope{0};    ///< Post-side trend slope.
        double rms_resid_m{0};   ///< Combined RMS of residuals over both sides.
        int n_pre{0};            ///< Pre-side sample count used.
        int n_post{0};           ///< Post-side sample count used.
    };

    /// Minimum samples required on each side; fewer ⇒ @c FitResult::ok = false.
    int min_window_points{4};

    /**
     * @brief Fit both sides and report the step at the boundary.
     *
     * @param pre         Samples strictly before the boundary.
     * @param post        Samples at/after the boundary.
     * @param t_boundary_s Boundary epoch; the time axis is centred here.
     * @return A populated @c FitResult.  @c ok is false if @p t_boundary_s is
     *         non-finite, or if either window has fewer than
     *         @c min_window_points samples, mismatched @c t_s/@c y_m lengths or
     *         any non-finite input.  A degenerate side (time spread too small to
     *         resolve a slope, e.g. all samples at one epoch) does not fail: it
     *         falls back to a constant fit (level = mean, slope 0), and @c ok
     *         can still be true.
     */
    FitResult fitStep(const Window& pre, const Window& post,
                      double t_boundary_s) const;
};

}  // namespace GnssProcessor
