#pragma once

#include <vector>

namespace GnssProcessor {

    /**
     * @brief Centralized interpolation utilities.
     *
     * Provides barycentric Lagrange weight computation, barycentric evaluation
     * and a general linear interpolation/extrapolation helper.
     *
     * All methods are static and stateless so they are safe for concurrent use.
     */
    class Interpolate {
    public:
        /**
         * @brief Compute barycentric Lagrange interpolation weights for a set of nodes.
         *
         * Produces barycentric weights w_j proportional to:
         *   w_j ∝ 1 / ∏_{k != j} (x_j - x_k)
         * which are used by the barycentric Lagrange formula to evaluate the
         * interpolation polynomial in a numerically stable O(n) fashion.
         *
         * Behavior:
         *  - If `x` is empty returns an empty vector.
         *  - If `x` has a single element returns {1.0}.
         *  - If two abscissae are nearly identical (|dx| < 1e-12) the corresponding
         *    weight is set to 0.0 and the caller should avoid duplicate nodes.
         *  - The product of differences is computed in log-space to reduce overflow/
         *    underflow; extremely large or small magnitudes are clamped to safe limits.
         *
         * @param[in] x Vector of node abscissae (x_0 .. x_{n-1}). Values should be distinct.
         * @return A vector of barycentric weights of the same length as `x`.
         *
         * @note Complexity: O(n^2) where n = x.size().
         * @note Thread-safety: Stateless and safe for concurrent use with independent inputs.
         */
        static std::vector<double> barycentricWeights(const std::vector<double>& x);

        /**
         * @brief Evaluate the barycentric Lagrange interpolant at a query abscissa.
         *
         * Evaluates the interpolation polynomial using precomputed barycentric weights:
         *   p(xq) = ( sum_j (w_j / (xq - x_j)) * y_j ) / ( sum_j (w_j / (xq - x_j)) )
         *
         * Behavior:
         *  - If `x` is empty the function returns 0.0.
         *  - If `w.size() != x.size()` or `y.size() != x.size()` the function returns 0.0.
         *  - If the query `xq` coincides with a node `x[j]` within tolerance (|xq - x[j]| < 1e-12)
         *    the function returns `y[j]` directly to preserve node values and avoid division by zero.
         *  - If the denominator is (nearly) zero the function returns 0.0 as a safe fallback.
         *
         * @param[in] x  Vector of node abscissae (x_0 .. x_{n-1}). Must correspond to `w` and `y`.
         * @param[in] w  Vector of barycentric weights computed for `x` (see barycentricWeights()).
         * @param[in] y  Vector of node ordinates (y_0 .. y_{n-1}).
         * @param[in] xq Query abscissa where the interpolant is evaluated.
         * @return Interpolated value p(xq). Returns 0.0 for invalid inputs or numerical degeneracy.
         *
         * @note Complexity: O(n).
         * @note Thread-safety: Stateless and safe for concurrent use with independent inputs.
         */
        static double barycentricEval(const std::vector<double>& x,
            const std::vector<double>& w,
            const std::vector<double>& y,
            double xq);

        /**
         * @brief Evaluate the derivative of the barycentric Lagrange interpolant.
         *
         * Computes p'(xq) analytically using the differentiated barycentric formula.
         * This provides mathematically consistent velocity when position uses
         * barycentricEval, avoiding the numerical noise of finite differences.
         *
         * The formula uses the quotient rule on p(x) = N(x)/D(x):
         *   p'(x) = (N'D - ND') / D²
         *
         * At nodes, a special L'Hôpital-based formula handles the 0/0 case:
         *   p'(x_k) = Σ_{j≠k} (w_j/w_k) * (y_k - y_j) / (x_k - x_j)
         *
         * @param x  Node x-coordinates.
         * @param w  Precomputed barycentric weights from barycentricWeights().
         * @param y  Node y-values (function values at nodes).
         * @param xq Query point.
         * @return   Derivative p'(xq).
         */
        static double barycentricDeriv(const std::vector<double>& x,
            const std::vector<double>& w,
            const std::vector<double>& y,
            double xq);

        /**
         * @brief Simple linear interpolation / extrapolation helper.
         *
         * Performs linear interpolation (or linear extrapolation) of the ordinate
         * values `y` at the query abscissa `xq` using nodes given by `x`/`y`.
         *
         * Behavior and edge-cases:
         *  - If `x` or `y` is empty or `x.size() != y.size()` the function returns 0.0.
         *  - If `x.size() == 1` the single ordinate `y[0]` is returned.
         *  - The routine uses a stable sort of index pairs to handle unsorted `x`.
         *  - If `xq` is before the first node the function linearly extrapolates using
         *    the first two sorted nodes. If `xq` is after the last node it extrapolates
         *    using the last two sorted nodes.
         *  - If `xq` lies between two nodes the function returns the linear interpolation
         *    between those nodes.
         *  - If a node interval width `dx` is nearly zero (|dx| < 1e-12) the function
         *    avoids division by zero and returns the nearer node's ordinate (implementation
         *    chooses the left node's value for interior intervals and a sensible single-node
         *    value for extrapolation).
         *
         * @param[in] x  Vector of abscissae (may be unsorted; values should ideally be distinct).
         * @param[in] y  Vector of ordinates corresponding to `x`.
         * @param[in] xq Query abscissa where the value is required.
         * @return Interpolated (or extrapolated) ordinate at `xq`. Returns 0.0 for invalid inputs
         *         or numerical degeneracy.
         *
         * @note Numerical tolerance for zero interval comparison: 1e-12.
         * @note Complexity: O(n log n) due to the stable sort of indices (n = x.size()).
         * @note Thread-safety: Stateless and safe for concurrent use with independent inputs.
         */
        static double linearInterp(const std::vector<double>& x,
            const std::vector<double>& y,
            double xq);


        /**
         * ---------------------------------------------------------------------
         * Natural cubic-spline and Hermite (monotone) helpers
         *
         * These declarations add cubic-spline and monotone cubic-Hermite
         * utilities to the Interpolate facade. Implementations belong in
         * `src\core\common\Interpolate.cpp` (or a new .cpp) and are thread-safe
         * (stateless) when given independent input vectors.
         * ---------------------------------------------------------------------
         */

         /**
          * @brief Compute second derivatives for a natural cubic spline.
          *
          * Produces the vector `c` of second derivatives at the nodes for a
          * natural cubic spline (zero second derivative at endpoints).
          *
          * - If `x.size() != y.size()` or `x.size() < 2` an empty vector is returned.
          * - Requires strictly increasing `x` values; behavior is undefined for
          *   non-monotonic or duplicate abscissae.
          *
          * @param[in] x Node abscissae (strictly increasing).
          * @param[in] y Node ordinates.
          * @return Vector `c` of length `x.size()` with spline second derivatives.
          */
        static std::vector<double> naturalSplineSecondDerivs(const std::vector<double>& x,
            const std::vector<double>& y);

        /**
         * @brief Evaluate a natural cubic spline at `xq` using precomputed second derivatives.
         *
         * - `c` must be the result of `naturalSplineSecondDerivs(x, y)`.
         * - If inputs are invalid the function returns 0.0.
         *
         * @param[in] x Node abscissae (strictly increasing).
         * @param[in] y Node ordinates.
         * @param[in] c Second derivatives returned by naturalSplineSecondDerivs().
         * @param[in] xq Query abscissa.
         * @return Spline value at `xq`.
         */
        static double splineEval(const std::vector<double>& x,
            const std::vector<double>& y,
            const std::vector<double>& c,
            double xq);

        /**
         * @brief Evaluate the derivative of a natural cubic spline at `xq`.
         *
         * - Uses the same `x`, `y`, `c` as `splineEval`.
         * - Returns the first derivative (slope) at `xq`.
         *
         * @param[in] x Node abscissae (strictly increasing).
         * @param[in] y Node ordinates.
         * @param[in] c Second derivatives returned by naturalSplineSecondDerivs().
         * @param[in] xq Query abscissa.
         * @return Spline derivative at `xq`.
         */
        static double splineDeriv(const std::vector<double>& x,
            const std::vector<double>& y,
            const std::vector<double>& c,
            double xq);

        /**
         * @brief Compute node slopes for a monotone cubic Hermite (Fritsch–Carlson).
         *
         * Produces per-node derivatives `m` that preserve monotonicity of the
         * input data when used with a cubic-Hermite interpolant.
         *
         * @warning This function forces slopes to zero at local extrema to preserve
         *          monotonicity. It is **not suitable** for oscillating data such as
         *          satellite ECEF coordinates. Use `catmullRomSlopes()` instead for
         *          general-purpose cubic Hermite interpolation of non-monotone data.
         *
         * - If `x.size() != y.size()` or `x.size() < 2` an empty vector is returned.
         *
         * @param[in] x Node abscissae (strictly increasing).
         * @param[in] y Node ordinates.
         * @return Vector `m` of length `x.size()` with monotone-preserving slopes.
         */
        static std::vector<double> monotoneHermiteSlopes(const std::vector<double>& x,
            const std::vector<double>& y);

        /**
         * @brief Compute Catmull-Rom node slopes for cubic Hermite interpolation.
         *
         * Produces per-node derivatives using the standard Catmull-Rom formula:
         *   m[i] = (y[i+1] - y[i-1]) / (x[i+1] - x[i-1])
         * with one-sided differences at the endpoints.
         *
         * Unlike `monotoneHermiteSlopes()`, this function does **not** clamp slopes
         * at local extrema and is therefore suitable for oscillating data such as
         * satellite ECEF coordinates.
         *
         * - If `x.size() != y.size()` or `x.size() < 2` an empty vector is returned.
         *
         * @param[in] x Node abscissae (strictly increasing).
         * @param[in] y Node ordinates.
         * @return Vector `m` of length `x.size()` with Catmull-Rom slopes.
         */
        static std::vector<double> catmullRomSlopes(const std::vector<double>& x,
            const std::vector<double>& y);

        /**
         * @brief Evaluate a cubic Hermite interpolant at `xq` using node slopes `m`.
         *
         * - `m` should be produced by `monotoneHermiteSlopes(x, y)` to guarantee
         *   monotonicity where applicable.
         *
         * @param[in] x Node abscissae (strictly increasing).
         * @param[in] y Node ordinates.
         * @param[in] m Node slopes (derivatives).
         * @param[in] xq Query abscissa.
         * @return Hermite interpolated value at `xq`.
         */
        static double hermiteEval(const std::vector<double>& x,
            const std::vector<double>& y,
            const std::vector<double>& m,
            double xq);

        /**
         * @brief Evaluate the derivative of a cubic Hermite interpolant at `xq`.
         *
         * - Uses the same `x`, `y`, `m` as `hermiteEval`.
         *
         * @param[in] x Node abscissae (strictly increasing).
         * @param[in] y Node ordinates.
         * @param[in] m Node slopes (derivatives).
         * @param[in] xq Query abscissa.
         * @return Hermite interpolant derivative at `xq`.
         */
        static double hermiteDeriv(const std::vector<double>& x,
            const std::vector<double>& y,
            const std::vector<double>& m,
            double xq);
    };

} // namespace GnssProcessor