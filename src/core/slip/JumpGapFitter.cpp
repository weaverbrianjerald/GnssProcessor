/**
 * @file JumpGapFitter.cpp
 * @brief Implementation of the two-sided trend+step least-squares fitter.
 */
#include "slip/JumpGapFitter.hpp"

#include <cmath>

namespace GnssProcessor {

namespace {

/// One-side fit outcome: @f$ y = \text{level} + \text{slope}\,(t - t_b) @f$.
struct SideFit {
    bool ok{false};
    double level{0};  ///< Intercept = value at the (centred) boundary.
    double slope{0};
};

/**
 * @brief Least-squares @f$ y = a + b\,u @f$ with @f$ u = t - t_b @f$.
 *
 * Returns a degenerate-but-flagged fit (slope 0, level = mean) when the time
 * spread is too small to resolve a slope; the caller decides whether that is
 * acceptable.  Any non-finite sample fails the side.
 */
SideFit fitSide(const JumpGapFitter::Window& w, double t_boundary_s,
                int min_points) {
    SideFit out;
    const std::size_t n = w.t_s.size();
    if (n != w.y_m.size()) return out;             // length mismatch
    if (static_cast<int>(n) < min_points) return out;

    double s0 = 0.0, s1 = 0.0, s2 = 0.0, sy = 0.0, suy = 0.0;
    for (std::size_t i = 0; i < n; ++i) {
        const double u = w.t_s[i] - t_boundary_s;  // centre for conditioning
        const double y = w.y_m[i];
        if (!std::isfinite(u) || !std::isfinite(y)) return out;  // non-finite
        s0 += 1.0;
        s1 += u;
        s2 += u * u;
        sy += y;
        suy += u * y;
    }

    const double det = s0 * s2 - s1 * s1;
    if (std::fabs(det) < 1e-12) {
        // Degenerate time spread (e.g. all samples at one epoch): fall back to
        // a constant fit so the level is still usable; slope is unresolved.
        out.slope = 0.0;
        out.level = sy / s0;
        out.ok = std::isfinite(out.level);
        return out;
    }

    out.slope = (s0 * suy - s1 * sy) / det;
    out.level = (s2 * sy - s1 * suy) / det;  // intercept at u = 0 ⇒ t = t_b
    out.ok = std::isfinite(out.level) && std::isfinite(out.slope);
    return out;
}

}  // namespace

JumpGapFitter::FitResult JumpGapFitter::fitStep(const Window& pre,
                                                const Window& post,
                                                double t_boundary_s) const {
    FitResult r;
    if (!std::isfinite(t_boundary_s)) return r;

    const SideFit pf = fitSide(pre, t_boundary_s, min_window_points);
    const SideFit qf = fitSide(post, t_boundary_s, min_window_points);
    if (!pf.ok || !qf.ok) return r;

    r.n_pre = static_cast<int>(pre.t_s.size());
    r.n_post = static_cast<int>(post.t_s.size());
    r.pre_level_m = pf.level;
    r.post_level_m = qf.level;
    r.pre_slope = pf.slope;
    r.post_slope = qf.slope;
    r.step_m = qf.level - pf.level;

    // Combined RMS of residuals against each side's own fitted line.
    double ss = 0.0;
    for (std::size_t i = 0; i < pre.t_s.size(); ++i) {
        const double u = pre.t_s[i] - t_boundary_s;
        const double res = pre.y_m[i] - (pf.level + pf.slope * u);
        ss += res * res;
    }
    for (std::size_t i = 0; i < post.t_s.size(); ++i) {
        const double u = post.t_s[i] - t_boundary_s;
        const double res = post.y_m[i] - (qf.level + qf.slope * u);
        ss += res * res;
    }
    const double ntot = static_cast<double>(r.n_pre + r.n_post);
    r.rms_resid_m = (ntot > 0.0) ? std::sqrt(ss / ntot) : 0.0;

    r.ok = std::isfinite(r.step_m) && std::isfinite(r.rms_resid_m);
    return r;
}

}  // namespace GnssProcessor
