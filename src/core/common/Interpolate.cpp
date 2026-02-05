#include "Interpolate.hpp"
#include <algorithm>
#include <cmath>
#include <limits>

namespace GnssProcessor {

    std::vector<double> Interpolate::barycentricWeights(const std::vector<double>& x) {
        size_t n = x.size();
        if (n == 0) return {};
        if (n == 1) return { 1.0 };

        std::vector<double> w(n, 0.0);

        for (size_t j = 0; j < n; ++j) {
            double log_abs_prod = 0.0;
            int sign = 1;
            bool valid = true;

            for (size_t k = 0; k < n; ++k) {
                if (k == j) continue;
                double diff = x[j] - x[k];
                if (std::abs(diff) < 1e-12) { // nearly-equal nodes => invalid weight
                    valid = false;
                    break;
                }
                log_abs_prod += std::log(std::abs(diff));
                if (diff < 0.0) sign = -sign;
            }

            if (!valid) {
                w[j] = 0.0;
                continue;
            }

            // Guard against overflow/underflow using log-space thresholds
            // exp(700) ~ 1e304, exp(-700) ~ 1e-304
            if (log_abs_prod > 700.0) {
                w[j] = 0.0;
            }
            else if (log_abs_prod < -700.0) {
                w[j] = sign * 1e300;
            }
            else {
                w[j] = sign * std::exp(-log_abs_prod);
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

        double num = 0.0;
        double den = 0.0;
        for (size_t j = 0; j < n; ++j) {
            double diff = xq - x[j];
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
     * @param x Node x-coordinates.
     * @param w Precomputed barycentric weights from barycentricWeights().
     * @param y Node y-values (function values at nodes).
     * @param xq Query point.
     * @return Derivative p'(xq).
     */
    double Interpolate::barycentricDeriv(const std::vector<double>& x,
        const std::vector<double>& w,
        const std::vector<double>& y,
        double xq) {
        const size_t n = x.size();
        if (n == 0) return 0.0;
        if (w.size() != n || y.size() != n) return 0.0;
        if (n == 1) return 0.0;  // Constant interpolant has zero derivative

        // Check if xq is at or very close to a node
        for (size_t k = 0; k < n; ++k) {
            double diff_k = xq - x[k];
            if (std::abs(diff_k) < 1e-12) {
                // At node x_k: use the special formula derived via L'Hôpital's rule
                // p'(x_k) = sum_{j != k} (w_j / w_k) * (y_k - y_j) / (x_k - x_j)
                if (std::abs(w[k]) < 1e-300) {
                    return 0.0;  // Degenerate weight
                }
                double deriv = 0.0;
                for (size_t j = 0; j < n; ++j) {
                    if (j == k) continue;
                    double dx = x[k] - x[j];
                    if (std::abs(dx) < 1e-12) continue;  // Skip coincident nodes
                    deriv += (w[j] / w[k]) * (y[k] - y[j]) / dx;
                }
                return deriv;
            }
        }

        // General case: xq is not at a node
        // Compute N, D, N', D' simultaneously for efficiency
        double N = 0.0;   // sum_j w_j * y_j / (xq - x_j)
        double D = 0.0;   // sum_j w_j / (xq - x_j)
        double Np = 0.0;  // -sum_j w_j * y_j / (xq - x_j)^2
        double Dp = 0.0;  // -sum_j w_j / (xq - x_j)^2

        for (size_t j = 0; j < n; ++j) {
            double diff = xq - x[j];
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

        // Quotient rule: p'(x) = (N' * D - N * D') / D^2
        double deriv = (Np * D - N * Dp) / (D * D);
        return deriv;
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

} // namespace GnssProcessor