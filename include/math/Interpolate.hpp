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
         *  - The nodes are centred and divided by the smallest nonzero node spacing
         *    before the products are formed, and the weights are rescaled so that
         *    max|w_j| = 1.  Both are common factors that cancel in barycentricEval()
         *    and barycentricDeriv().
         *  - If a node lies within 1e-12 of another in these normalized units, its
         *    weight is set to 0.0; callers should avoid duplicate nodes.
         *  - The product of differences is computed in log-space to avoid overflow
         *    and underflow; a weight below e^-700 times the largest is set to 0.0.
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
         *  - If the denominator is exactly zero the function returns 0.0.
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
         *  - If the width `dx` of the interval used is below 1e-12, the function does
         *    not divide and returns one node's ordinate: the left node for an interior
         *    interval, the first sorted node below the range and the last sorted node
         *    above it.
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
         * Cubic-spline and cubic-Hermite utilities of the Interpolate facade.
         * They are implemented in `src/core/math/Interpolate.cpp` and are
         * stateless, so they are thread-safe on independent input vectors.
         * ---------------------------------------------------------------------
         */

         /**
          * @brief Compute the node coefficients of a natural cubic spline.
          *
          * Produces the vector `c` for a natural cubic spline (zero second
          * derivative at the endpoints).  Each piece is
          *   S_i(x) = a_i + b_i·dx + c_i·dx² + d_i·dx³,  dx = x - x_i,
          * so c_i = S''(x_i)/2: half the second derivative, despite the
          * function name.  splineEval() and splineDeriv() expect exactly this `c`.
          *
          * - If `x.size() != y.size()` or `x.size() < 2` an empty vector is returned.
          * - Requires strictly increasing `x` values.  A non-positive interval next
          *   to an interior node returns an all-zero vector; results are otherwise
          *   undefined for non-monotonic or duplicate abscissae.
          *
          * @param[in] x Node abscissae (strictly increasing).
          * @param[in] y Node ordinates.
          * @return Vector `c` of length `x.size()` with the coefficients c_i = S''(x_i)/2.
          */
        static std::vector<double> naturalSplineSecondDerivs(const std::vector<double>& x,
            const std::vector<double>& y);

        /**
         * @brief Evaluate a natural cubic spline at `xq` using precomputed node coefficients.
         *
         * - `c` must be the result of `naturalSplineSecondDerivs(x, y)`.
         * - If the sizes of `x`, `y` and `c` differ or `x` is empty, returns 0.0.
         * - Outside [x_0, x_{n-1}] the first or last cubic piece is extrapolated.
         *
         * @param[in] x Node abscissae (strictly increasing).
         * @param[in] y Node ordinates.
         * @param[in] c Coefficients c_i = S''(x_i)/2 returned by naturalSplineSecondDerivs().
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
         * @param[in] c Coefficients c_i = S''(x_i)/2 returned by naturalSplineSecondDerivs().
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
         * Endpoint slopes are the adjacent secants.  An interior slope is the
         * weighted harmonic mean of the two adjacent secants, limited in magnitude
         * to the smaller of them; it is zero where the secants differ in sign or
         * either is zero, and both slopes of a zero secant are zero.
         *
         * @warning This function forces slopes to zero at local extrema to preserve
         *          monotonicity, so it is unsuitable for oscillating data such as
         *          satellite ECEF coordinates. Use `catmullRomSlopes()` instead for
         *          general-purpose cubic Hermite interpolation of non-monotone data.
         *
         * - If `x.size() != y.size()` or `x.size() < 2` an empty vector is returned.
         * - A zero node interval returns an all-zero vector.
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
         * Unlike `monotoneHermiteSlopes()`, this function does not clamp slopes
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
         * - `m` comes from `monotoneHermiteSlopes(x, y)` when monotonicity must be
         *   preserved, or from `catmullRomSlopes(x, y)` for oscillating data.
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

        /**
         * @brief Neville's algorithm: polynomial interpolation with simultaneous derivative.
         *
         * Evaluates the unique polynomial of degree (n-1) passing through the n
         * data points (x[i], y[i]) and its first derivative at the query point xq.
         * Uses the classical Neville tableau with an Aitken-like derivative extension.
         *
         * The tableau combines the nodes in the order the caller supplies them.
         * The final polynomial does not depend on that order, but the rounding
         * does; sorting the nodes by increasing distance from xq before the call
         * gives the best numerical behaviour.  The nodes are shifted by xq before
         * the recurrence, so large abscissae (GPS seconds) do not lose precision.
         *
         * The polynomial is unique for a given node set, but continuity of the
         * interpolant across a streaming query (where the caller selects nodes
         * per query) depends on the upstream node-selection strategy.
         * A bracket-centered window (the window slides one node per source-epoch
         * crossing) preserves value continuity, because the shared bracket node
         * pins the old and the new polynomial to the same value at the swap.
         * A nearest-N window swaps a node at the midpoint between source epochs,
         * where the two polynomials disagree, which produces a sawtooth in the
         * interpolated position.
         *
         * @param[in]  x   Node abscissae (must have same size as y; need not be sorted).
         * @param[in]  y   Node ordinates.
         * @param[in]  xq  Query abscissa.
         * @param[out] val Interpolated value p(xq).
         * @param[out] deriv Interpolated derivative p'(xq).
         *
         * @note Complexity: O(n^2) time and O(n) auxiliary space.
         * @note If x is empty or y.size() != x.size(), val and deriv are set to 0.0.
         *       With one node, val = y[0] and deriv = 0.0.
         */
        static void nevilleInterpDeriv(const std::vector<double>& x,
            const std::vector<double>& y,
            double xq,
            double& val,
            double& deriv);

        /**
         * @brief Sidereal-trigonometric least-squares SP3 fit with analytic derivative.
         *
         * Fits the @p n data points (x[i], y[i]) with the 9-term trigonometric
         * basis M-PAGES uses for SP3 satellite-coordinate interpolation
         * (Intrpsp3::TrigExt) and evaluates the fitted value and its first
         * derivative at @p xq.  The basis is
         *   a(t) = { 1,
         *            sin(1*P0*t), cos(1*P0*t), sin(2*P0*t), cos(2*P0*t),
         *            sin(3*P0*t), cos(3*P0*t), sin(4*P0*t), cos(4*P0*t) }
         * (constant + four sin/cos harmonic pairs, sin before cos, in the
         * M-PAGES index order).  The fundamental period is the sidereal
         * day (Astronomical Almanac 1995, 0.99726956634 mean solar days).
         *
         * @par Time units.
         * @p P0 is in rad per second ( 2*pi / (0.99726956634 * 86400) ), so the
         * basis argument tau = (x[i] - x[0]) is in the caller's seconds and the
         * analytic derivative is returned per second, with no day/second rescale.
         * This is identical to the M-PAGES rad/day basis followed by its /86400
         * velocity scaling; only the unit of tau differs.  The fit origin is the
         * first node x[0]; an origin shift only re-phases the harmonics (absorbed
         * by each sin/cos coefficient pair), so the fitted curve is invariant to
         * the choice of origin while the small tau improves conditioning.
         *
         * @par Fit regime.
         *   - n == 9 : square system; the model passes through all 9 nodes (exact,
         *              matching the M-PAGES 9-point ±4 window).
         *   - n  > 9 : overdetermined least-squares smoother, which does not pass
         *              through the nodes.  The engine's default window is up to 11
         *              nodes (ModelGeometry IGS_POINTS), so the production call is
         *              an 11-point LS smoother.  This departs from the M-PAGES exact
         *              fit on purpose, so that the trig branch reuses the existing
         *              derotated window unchanged.
         *
         * @par Weighting.
         * Per-node sigmas weight the fit by 1/sigma row-scaling of both A and b.
         * If @p sigmas is empty or not of size x.size(), has all positive entries
         * equal, or has no entry > 0, unit weights are used (a uniform weight
         * cancels out of the normal equations).  In a weighted fit a node with
         * sigma <= 0 gets weight 1.  This mirrors M-PAGES, whose SP3 weight
         * sig[i] = 2^acc/1000 m is a single per-satellite value applied uniformly
         * to every node (hence inert for per-satellite interpolation); the
         * parameter exists so a future per-epoch accuracy source can drive a
         * genuinely weighted fit.  The solve is a singular-value-truncated
         * pseudoinverse (Eigen JacobiSVD, relative threshold 1e-15), the NR
         * svdfit/svbksb minimum-norm convention.
         *
         * @param[in]  x      Node abscissae (same size as y; need not be sorted).
         * @param[in]  y      Node ordinates.
         * @param[in]  sigmas Per-node 1-sigma weights (same size as x, or empty
         *                    for an unweighted fit).
         * @param[in]  xq     Query abscissa.
         * @param[out] val    Fitted value p(xq).
         * @param[out] deriv  Fitted derivative p'(xq) (per x-unit; per second when
         *                    x is in seconds).
         *
         * @note If x is empty (or y.size() != x.size()), val and deriv are set to
         *       0.0.  With fewer than 9 valid nodes the system is rank-deficient
         *       and solved via the truncated pseudoinverse (under-determined).
         */
        static void trigSvdInterp(const std::vector<double>& x,
            const std::vector<double>& y,
            const std::vector<double>& sigmas,
            double xq,
            double& val,
            double& deriv);
    };

} // namespace GnssProcessor