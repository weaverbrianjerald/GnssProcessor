#include "Interpolate.hpp"
#include <algorithm>
#include <cmath>
#include <limits>

namespace GnssProcessor {

    std::vector<double> Interpolate::barycentricWeights(const std::vector<double>& x) {
        size_t n = x.size();
        if (n == 0) return {};
        if (n == 1) return { 1.0 };

        // Transform nodes to small indices to keep products well-conditioned.
        //
        // Reference implementations (e.g. IGS/JPL) use integer-stride indices
        // {1,2,...,n} instead of raw times {0,900,...,7200}. With raw 900 s SP3
        // spacing the product of 8 differences reaches ~1.7e26, giving weights
        // ~5.8e-27; with unit-stride indices the product is at most 9! = 362880,
        // giving weights ~2.8e-6 — well within double precision.
        //
        // We shift to center around zero (to avoid cancellation for large
        // absolute values like GPS seconds ~1.36e8), then divide by the minimum
        // node spacing to produce near-unit-stride indices. The resulting
        // weights differ from the "true" weights by a common factor of
        // s^(n-1) which cancels exactly in the barycentric N/D ratio used by
        // barycentricEval — so the interpolated values are unchanged.
        // barycentricDeriv replicates this normalization and applies the
        // chain-rule correction dp/dx = dp/ds / min_spacing.
        //
        // IMPORTANT: this normalization is internal to weight computation only.
        // Callers pass the original (un-normalized) x to barycentricEval/Deriv.
        // In barycentricEval the common scaling factor cancels in the N/D ratio.
        // In barycentricDeriv the same normalization is replicated and the
        // chain-rule correction (divide by min_spacing) is applied.
        double x_center = 0.5 * (x.front() + x.back());

        // Find minimum spacing for scale normalization
        double min_spacing = std::numeric_limits<double>::max();
        for (size_t i = 1; i < n; ++i) {
            double sp = std::abs(x[i] - x[i - 1]);
            if (sp > 1e-15) {
                min_spacing = std::min(min_spacing, sp);
            }
        }
        if (min_spacing <= 1e-15 || min_spacing == std::numeric_limits<double>::max()) {
            min_spacing = 1.0; // fallback: no scaling
        }

        std::vector<double> xs(n);
        for (size_t i = 0; i < n; ++i) {
            xs[i] = (x[i] - x_center) / min_spacing;
        }

        std::vector<double> w(n, 0.0);

        // Compute raw log-weights and normalize in log-space so max|w| == 1.
        std::vector<double> log_abs_w(n, 0.0);
        std::vector<int> signs(n, 1);
        std::vector<bool> valid(n, true);

        for (size_t j = 0; j < n; ++j) {
            double log_abs_prod = 0.0;
            int sign = 1;

            for (size_t k = 0; k < n; ++k) {
                if (k == j) continue;
                double diff = xs[j] - xs[k];
                if (std::abs(diff) < 1e-12) {
                    valid[j] = false;
                    break;
                }
                log_abs_prod += std::log(std::abs(diff));
                if (diff < 0.0) sign = -sign;
            }

            if (valid[j]) {
                log_abs_w[j] = -log_abs_prod;
                signs[j] = sign;
            }
        }

        // Normalize: subtract max log-weight so largest |w| == 1.
        double max_log = -std::numeric_limits<double>::infinity();
        for (size_t j = 0; j < n; ++j) {
            if (valid[j]) {
                max_log = std::max(max_log, log_abs_w[j]);
            }
        }

        for (size_t j = 0; j < n; ++j) {
            if (!valid[j]) {
                w[j] = 0.0;
                continue;
            }

            double shifted = log_abs_w[j] - max_log;

            if (shifted < -700.0) {
                w[j] = 0.0;
            }
            else {
                w[j] = signs[j] * std::exp(shifted);
            }
        }

        return w;
    }

    double Interpolate::barycentricEval(const std::vector<double>& x,
        const std::vector<double>& w,
        const std::vector<double>& y,
        double xq) {
        const size_t n = x.size();
        if (n == 0) return 0.0;
        if (w.size() != n || y.size() != n) return 0.0;

        // Apply the same centering used by barycentricWeights so that
        // w[j] / (xq_s - x_s[j]) keeps all terms at consistent magnitudes.
        // The shift cancels in the N/D ratio, so the result is identical
        // mathematically, but avoids mixing O(1) weights with O(900) diffs.
        double x_center = 0.5 * (x.front() + x.back());
        double xq_s = xq - x_center;

        double num = 0.0;
        double den = 0.0;
        for (size_t j = 0; j < n; ++j) {
            double diff = xq_s - (x[j] - x_center);
            if (std::abs(diff) < 1e-12) {
                return y[j]; // exact node match
            }
            double term = w[j] / diff;
            num += term * y[j];
            den += term;
        }
        return (den == 0.0) ? 0.0 : (num / den);
    }

    /**
     * @brief Evaluate the derivative of the barycentric Lagrange interpolant.
     *
     * Computes p'(xq) analytically using the differentiated barycentric formula.
     *
     * The barycentric interpolant is:
     *   p(x) = N(x) / D(x)
     * where:
     *   N(x) = sum_j w_j * y_j / (x - x_j)
     *   D(x) = sum_j w_j / (x - x_j)
     *
     * By the quotient rule:
     *   p'(x) = (N'(x) * D(x) - N(x) * D'(x)) / D(x)^2
     *
     * where:
     *   N'(x) = -sum_j w_j * y_j / (x - x_j)^2
     *   D'(x) = -sum_j w_j / (x - x_j)^2
     *
     * At a node x = x_k, a special L'Hôpital-based formula is used:
     *   p'(x_k) = sum_{j != k} (w_j / w_k) * (y_k - y_j) / (x_k - x_j)
     *
     * Centering and spacing normalization are applied to differences to
     * minimize floating-point cancellation when node values are large
     * (e.g. GPS seconds ~1.36e8) and to keep term magnitudes consistent
     * with the precomputed weights. The result is converted back to
     * the original x-scale via the chain rule: dp/dx = dp/ds / spacing.
     *
     * This matches the integer-index approach used by IGS/JPL.
     *
     * @param x Node x-coordinates (original, un-normalized).
     * @param w Precomputed barycentric weights from barycentricWeights().
     * @param y Node y-values (function values at nodes).
     * @param xq Query point (original, un-normalized).
     * @return Derivative p'(xq) with respect to original x.
     */
    double Interpolate::barycentricDeriv(const std::vector<double>& x,
        const std::vector<double>& w,
        const std::vector<double>& y,
        double xq) {
        const size_t n = x.size();
        if (n == 0) return 0.0;
        if (w.size() != n || y.size() != n) return 0.0;
        if (n == 1) return 0.0;  // Constant interpolant has zero derivative

        // Apply the SAME centering + spacing normalization used by barycentricWeights
        // so that w[j] and 1/(xq_s - x_s[j]) are at consistent O(1) magnitudes.
        //
        // Without spacing normalization, the differences (xq - x[j]) are O(300..3000)
        // for 5-min SP3 with 11 nodes, while weights computed from unit-stride indices
        // are O(1). This magnitude mismatch degrades precision in the derivative's
        // quotient-rule formula, especially for the 1/(xq-xj)^2 terms.
        //
        // By normalizing to s = (x - x_center) / min_spacing we get near-integer
        // differences that match the weight magnitudes. The derivative in normalized
        // coordinates dp/ds must then be converted back via the chain rule:
        //   dp/dx = dp/ds * ds/dx = dp/ds * (1 / min_spacing)
        //
        // This matches the integer-index approach used by IGS/JPL reference software.
        double x_center = 0.5 * (x.front() + x.back());

        double min_spacing = std::numeric_limits<double>::max();
        for (size_t i = 1; i < n; ++i) {
            double sp = std::abs(x[i] - x[i - 1]);
            if (sp > 1e-15) {
                min_spacing = std::min(min_spacing, sp);
            }
        }
        if (min_spacing <= 1e-15 || min_spacing == std::numeric_limits<double>::max()) {
            min_spacing = 1.0;
        }

        double xq_s = (xq - x_center) / min_spacing;

        // Check if xq is at or very close to a node
        for (size_t k = 0; k < n; ++k) {
            double xk_s = (x[k] - x_center) / min_spacing;
            double diff_k = xq_s - xk_s;
            if (std::abs(diff_k) < 1e-12) {
                // At node x_k: use the special formula derived via L'Hopital's rule
                // p'(x_k) = sum_{j != k} (w_j / w_k) * (y_k - y_j) / (x_k - x_j)
                // The w_j/w_k ratio cancels the common spacing^(n-1) factor, so the
                // only remaining scaling is in the 1/(x_k - x_j) denominator.
                if (std::abs(w[k]) < 1e-300) {
                    return 0.0;  // Degenerate weight
                }
                double deriv = 0.0;
                for (size_t j = 0; j < n; ++j) {
                    if (j == k) continue;
                    double dx = xk_s - (x[j] - x_center) / min_spacing;
                    if (std::abs(dx) < 1e-12) continue;  // Skip coincident nodes
                    deriv += (w[j] / w[k]) * (y[k] - y[j]) / dx;
                }
                // Chain rule: dp/dx = dp/ds / min_spacing
                return deriv / min_spacing;
            }
        }

        // General case: xq is not at a node
        // Compute N, D, N', D' simultaneously for efficiency
        double N = 0.0;   // sum_j w_j * y_j / (xq_s - x_s[j])
        double D = 0.0;   // sum_j w_j / (xq_s - x_s[j])
        double Np = 0.0;  // -sum_j w_j * y_j / (xq_s - x_s[j])^2
        double Dp = 0.0;  // -sum_j w_j / (xq_s - x_s[j])^2

        for (size_t j = 0; j < n; ++j) {
            double diff = xq_s - (x[j] - x_center) / min_spacing;
            double diff_sq = diff * diff;
            double wj_over_diff = w[j] / diff;
            double wj_over_diff_sq = w[j] / diff_sq;

            N += wj_over_diff * y[j];
            D += wj_over_diff;
            Np -= wj_over_diff_sq * y[j];
            Dp -= wj_over_diff_sq;
        }

        if (std::abs(D) < 1e-300) {
            return 0.0;  // Degenerate denominator
        }

        // Quotient rule: p'(s) = (N' * D - N * D') / D^2
        // Chain rule: dp/dx = dp/ds / min_spacing
        double deriv_ds = (Np * D - N * Dp) / (D * D);
        return deriv_ds / min_spacing;
    }

    double Interpolate::linearInterp(const std::vector<double>& x,
        const std::vector<double>& y,
        double xq) {
        if (x.empty() || y.empty() || x.size() != y.size()) return 0.0;
        const size_t n = x.size();
        if (n == 1) return y[0];

        // Build sorted pairs (stable)
        std::vector<size_t> idx(n);
        for (size_t i = 0; i < n; ++i) idx[i] = i;
        std::stable_sort(idx.begin(), idx.end(), [&](size_t a, size_t b) {
            return x[a] < x[b];
            });

        // If xq is before first or after last -> extrapolate using two nearest points
        double x_first = x[idx.front()];
        double x_last = x[idx.back()];
        if (xq <= x_first) {
            double x0 = x[idx[0]], y0 = y[idx[0]];
            double x1 = x[idx[1]], y1 = y[idx[1]];
            double dx = x1 - x0;
            if (std::abs(dx) < 1e-12) return y0;
            return y0 + (xq - x0) * (y1 - y0) / dx;
        }
        if (xq >= x_last) {
            double x0 = x[idx[n - 2]], y0 = y[idx[n - 2]];
            double x1 = x[idx[n - 1]], y1 = y[idx[n - 1]];
            double dx = x1 - x0;
            if (std::abs(dx) < 1e-12) return y1;
            return y0 + (xq - x0) * (y1 - y0) / dx;
        }

        // Find interval [x_i, x_{i+1}] containing xq
        for (size_t ii = 0; ii + 1 < n; ++ii) {
            size_t i = idx[ii];
            size_t j = idx[ii + 1];
            double xi = x[i];
            double xj = x[j];
            if (xi <= xq && xq <= xj) {
                double yi = y[i];
                double yj = y[j];
                double dx = xj - xi;
                if (std::abs(dx) < 1e-12) return yi;
                return yi + (xq - xi) * (yj - yi) / dx;
            }
        }

        // Fallback (shouldn't get here)
        return 0.0;
    }

    // Natural cubic spline helpers
    static std::vector<double> computeNaturalSplineSecondDerivs(const std::vector<double>& x, const std::vector<double>& y) {
        const size_t n = x.size();
        std::vector<double> c(n, 0.0);
        if (n < 2) return c;

        std::vector<double> h(n - 1);
        for (size_t i = 0; i + 1 < n; ++i) h[i] = x[i + 1] - x[i];

        // Build the tridiagonal system
        std::vector<double> alpha(n, 0.0);
        for (size_t i = 1; i + 1 < n; ++i) {
            double denom1 = h[i - 1];
            double denom2 = h[i];
            if (denom1 <= 0.0 || denom2 <= 0.0) { // guard against bad input
                return std::vector<double>(n, 0.0);
            }
            alpha[i] = 3.0 * ((y[i + 1] - y[i]) / denom2 - (y[i] - y[i - 1]) / denom1);
        }

        std::vector<double> l(n, 0.0), mu(n, 0.0), z(n, 0.0);
        l[0] = 1.0; mu[0] = z[0] = 0.0;

        for (size_t i = 1; i + 1 < n; ++i) {
            l[i] = 2.0 * (x[i + 1] - x[i - 1]) - h[i - 1] * mu[i - 1];
            if (l[i] == 0.0) { // numerical guard
                l[i] = 1e-12;
            }
            mu[i] = h[i] / l[i];
            z[i] = (alpha[i] - h[i - 1] * z[i - 1]) / l[i];
        }

        l[n - 1] = 1.0;
        z[n - 1] = 0.0;
        c[n - 1] = 0.0;

        for (int j = static_cast<int>(n) - 2; j >= 0; --j) {
            c[j] = z[j] - mu[j] * c[j + 1];
        }

        return c;
    }

    static double splineEvaluate(const std::vector<double>& x,
        const std::vector<double>& y,
        const std::vector<double>& c,
        double t)
    {
        const size_t n = x.size();
        if (n == 0) return 0.0;
        if (n == 1) return y[0];

        // locate interval i where x[i] <= t <= x[i+1]
        size_t i = 0;
        if (t <= x.front()) {
            i = 0;
        }
        else if (t >= x.back()) {
            i = n - 2;
        }
        else {
            // binary search
            size_t lo = 0, hi = n - 1;
            while (hi - lo > 1) {
                size_t mid = (lo + hi) >> 1;
                if (x[mid] <= t) lo = mid; else hi = mid;
            }
            i = lo;
        }

        double h = x[i + 1] - x[i];
        if (h == 0.0) return y[i];
        double dx = t - x[i];
        double a = y[i];
        double b = (y[i + 1] - y[i]) / h - h * (c[i + 1] + 2.0 * c[i]) / 3.0;
        double d = (c[i + 1] - c[i]) / (3.0 * h);
        double result = a + b * dx + c[i] * dx * dx + d * dx * dx * dx;
        return result;
    }

    static double splineDerivative(const std::vector<double>& x,
        const std::vector<double>& y,
        const std::vector<double>& c,
        double t)
    {
        const size_t n = x.size();
        if (n == 0) return 0.0;
        if (n == 1) return 0.0;

        // locate interval
        size_t i = 0;
        if (t <= x.front()) {
            i = 0;
        }
        else if (t >= x.back()) {
            i = n - 2;
        }
        else {
            size_t lo = 0, hi = n - 1;
            while (hi - lo > 1) {
                size_t mid = (lo + hi) >> 1;
                if (x[mid] <= t) lo = mid; else hi = mid;
            }
            i = lo;
        }

        double h = x[i + 1] - x[i];
        if (h == 0.0) return 0.0;
        double dx = t - x[i];
        double b = (y[i + 1] - y[i]) / h - h * (c[i + 1] + 2.0 * c[i]) / 3.0;
        double d = (c[i + 1] - c[i]) / (3.0 * h);
        double deriv = b + 2.0 * c[i] * dx + 3.0 * d * dx * dx;
        return deriv;
    }

    // Wrapper for natural spline second derivatives
    std::vector<double> Interpolate::naturalSplineSecondDerivs(const std::vector<double>& x,
        const std::vector<double>& y) {
        if (x.size() != y.size() || x.size() < 2) return {};
        return computeNaturalSplineSecondDerivs(x, y);
    }

    // Wrapper for spline evaluation
    double Interpolate::splineEval(const std::vector<double>& x,
        const std::vector<double>& y,
        const std::vector<double>& c,
        double xq) {
        if (x.size() != y.size() || c.size() != x.size() || x.empty()) return 0.0;
        return splineEvaluate(x, y, c, xq);
    }

    // Wrapper for spline derivative evaluation
    double Interpolate::splineDeriv(const std::vector<double>& x,
        const std::vector<double>& y,
        const std::vector<double>& c,
        double xq) {
        if (x.size() != y.size() || c.size() != x.size() || x.empty()) return 0.0;
        return splineDerivative(x, y, c, xq);
    }


    // Compute monotone cubic-Hermite slopes (Fritsch–Carlson style)
    std::vector<double> Interpolate::monotoneHermiteSlopes(const std::vector<double>& x,
        const std::vector<double>& y) {
        const size_t n = x.size();
        if (n < 2 || y.size() != n) return {};

        std::vector<double> h(n - 1);
        std::vector<double> delta(n - 1);
        for (size_t i = 0; i + 1 < n; ++i) {
            h[i] = x[i + 1] - x[i];
            if (h[i] == 0.0) {
                // Degenerate spacing -> fall back to zero slopes
                return std::vector<double>(n, 0.0);
            }
            delta[i] = (y[i + 1] - y[i]) / h[i];
        }

        std::vector<double> m(n, 0.0);
        // Endpoints: use secant slope
        m[0] = delta[0];
        m[n - 1] = delta[n - 2];

        // Handle interior points
        for (size_t i = 1; i + 1 < n; ++i) {
            double dkm1 = delta[i - 1];
            double dk = delta[i];

            if (dkm1 == 0.0 || dk == 0.0 || (dkm1 > 0.0 && dk < 0.0) || (dkm1 < 0.0 && dk > 0.0)) {
                // Opposite signs or zero secant -> flat slope to preserve monotonicity
                m[i] = 0.0;
            }
            else {
                // Fritsch–Carlson weighted harmonic mean
                double w1 = 2.0 * h[i] + h[i - 1];
                double w2 = h[i] + 2.0 * h[i - 1];
                double denom = (w1 / dkm1 + w2 / dk);
                if (denom == 0.0) {
                    m[i] = 0.0;
                }
                else {
                    double mi = (w1 + w2) / denom;
                    // clamp to avoid overshoot: no larger than max of adjacent secants
                    double lim = std::min(std::abs(dkm1), std::abs(dk));
                    if (mi == 0.0) {
                        m[i] = 0.0;
                    } else {
                        double s = (mi > 0.0) ? 1.0 : -1.0;
                        m[i] = s * std::min(std::abs(mi), lim);
                    }
                }
            }
        }

        // Additional safeguard: where a secant is zero, force both adjacent slopes to zero
        for (size_t i = 0; i + 1 < n; ++i) {
            if (delta[i] == 0.0) {
                m[i] = 0.0;
                m[i + 1] = 0.0;
            }
        }

        return m;
    }


    // Compute Catmull-Rom (non-monotone) slopes for cubic Hermite interpolation
    std::vector<double> Interpolate::catmullRomSlopes(const std::vector<double>& x,
        const std::vector<double>& y) {
        const size_t n = x.size();
        if (n < 2 || y.size() != n) return {};

        std::vector<double> m(n, 0.0);

        // Interior points: centered difference  m[i] = (y[i+1] - y[i-1]) / (x[i+1] - x[i-1])
        for (size_t i = 1; i + 1 < n; ++i) {
            double dx = x[i + 1] - x[i - 1];
            if (std::abs(dx) < 1e-15) {
                m[i] = 0.0;
            } else {
                m[i] = (y[i + 1] - y[i - 1]) / dx;
            }
        }

        // Endpoints: one-sided (secant) slopes
        {
            double dx = x[1] - x[0];
            m[0] = (std::abs(dx) > 1e-15) ? (y[1] - y[0]) / dx : 0.0;
        }
        {
            double dx = x[n - 1] - x[n - 2];
            m[n - 1] = (std::abs(dx) > 1e-15) ? (y[n - 1] - y[n - 2]) / dx : 0.0;
        }

        return m;
    }


    // Evaluate cubic Hermite interpolant at xq
    double Interpolate::hermiteEval(const std::vector<double>& x,
        const std::vector<double>& y,
        const std::vector<double>& m,
        double xq) {
        const size_t n = x.size();
        if (n == 0 || y.size() != n || m.size() != n) return 0.0;
        if (n == 1) return y[0];

        // locate interval
        size_t i = 0;
        if (xq <= x.front()) {
            i = 0;
        } else if (xq >= x.back()) {
            i = n - 2;
        } else {
            size_t lo = 0, hi = n - 1;
            while (hi - lo > 1) {
                size_t mid = (lo + hi) >> 1;
                if (x[mid] <= xq) lo = mid; else hi = mid;
            }
            i = lo;
        }

        double h = x[i + 1] - x[i];
        if (h == 0.0) return y[i];
        double t = (xq - x[i]) / h;

        double t2 = t * t;
        double t3 = t2 * t;

        double h00 = 2.0 * t3 - 3.0 * t2 + 1.0;
        double h10 = t3 - 2.0 * t2 + t;
        double h01 = -2.0 * t3 + 3.0 * t2;
        double h11 = t3 - t2;

        double result = h00 * y[i] + h10 * (h * m[i]) + h01 * y[i + 1] + h11 * (h * m[i + 1]);
        return result;
    }

    // Evaluate derivative of cubic Hermite interpolant at xq
    double Interpolate::hermiteDeriv(const std::vector<double>& x,
        const std::vector<double>& y,
        const std::vector<double>& m,
        double xq) {
        const size_t n = x.size();
        if (n == 0 || y.size() != n || m.size() != n) return 0.0;
        if (n == 1) return 0.0;

        // locate interval
        size_t i = 0;
        if (xq <= x.front()) {
            i = 0;
        } else if (xq >= x.back()) {
            i = n - 2;
        } else {
            size_t lo = 0, hi = n - 1;
            while (hi - lo > 1) {
                size_t mid = (lo + hi) >> 1;
                if (x[mid] <= xq) lo = mid; else hi = mid;
            }
            i = lo;
        }

        double h = x[i + 1] - x[i];
        if (h == 0.0) return 0.0;
        double t = (xq - x[i]) / h;

        double t2 = t * t;

        double dh00_dt = 6.0 * t2 - 6.0 * t;
        double dh10_dt = 3.0 * t2 - 4.0 * t + 1.0;
        double dh01_dt = -6.0 * t2 + 6.0 * t;
        double dh11_dt = 3.0 * t2 - 2.0 * t;

        // chain rule: d/dx = (1/h) * d/dt
        double deriv = (dh00_dt * y[i] + dh10_dt * (h * m[i]) + dh01_dt * y[i + 1] + dh11_dt * (h * m[i + 1])) / h;
        return deriv;
    }

    void Interpolate::nevilleInterpDeriv(const std::vector<double>& x,
        const std::vector<double>& y,
        double xq,
        double& val,
        double& deriv) {
        const size_t n = x.size();
        val = 0.0;
        deriv = 0.0;
        if (n == 0 || y.size() != n) return;
        if (n == 1) { val = y[0]; return; }

        // Center the x-values around xq to avoid catastrophic cancellation.
        //
        // When x-values are raw GPS continuous seconds (~1.36e8), computing
        // (xq - x[i]) subtracts two large numbers to get a small difference,
        // losing ~8 digits of precision. By shifting s[i] = x[i] - xq, all
        // differences become O(node_spacing) and the subtraction is exact to
        // full double precision.
        //
        // The polynomial value is translation-invariant: p(xq) evaluated at
        // nodes {x[i]} equals p(0) evaluated at nodes {x[i] - xq}.
        // The derivative is also invariant: dp/dx = dp/ds since s = x - xq
        // is a unit-Jacobian shift.
        std::vector<double> s(n);
        for (size_t i = 0; i < n; ++i) {
            s[i] = x[i] - xq;
        }
        const double sq = 0.0; // xq in shifted coordinates

        // P[i] holds the current tableau column for value interpolation.
        // D[i] holds the current tableau column for derivative interpolation.
        // Neville recurrence for value:
        //   P_{i,j} = ((sq - s[i+j]) * P_{i,j-1} - (sq - s[i]) * P_{i+1,j-1})
        //             / (s[i] - s[i+j])
        // Derivative recurrence (differentiate the above w.r.t. sq):
        //   D_{i,j} = ((sq - s[i+j]) * D_{i,j-1} + P_{i,j-1}
        //             - (sq - s[i]) * D_{i+1,j-1} - P_{i+1,j-1})
        //             / (s[i] - s[i+j])
        std::vector<double> P(y.begin(), y.end());
        std::vector<double> D(n, 0.0); // derivative of degree-0 polynomial = 0

        for (size_t j = 1; j < n; ++j) {
            for (size_t i = 0; i + j < n; ++i) {
                double denom = s[i] - s[i + j];
                if (std::abs(denom) < 1e-30) {
                    P[i] = 0.0;
                    D[i] = 0.0;
                    continue;
                }
                double di_left  = sq - s[i + j];  // == -s[i+j]
                double di_right = sq - s[i];       // == -s[i]
                double new_D = (di_left * D[i] + P[i]
                              - di_right * D[i + 1] - P[i + 1]) / denom;
                double new_P = (di_left * P[i] - di_right * P[i + 1]) / denom;
                P[i] = new_P;
                D[i] = new_D;
            }
        }

        val = P[0];
        deriv = D[0];
    }

} // namespace GnssProcessor