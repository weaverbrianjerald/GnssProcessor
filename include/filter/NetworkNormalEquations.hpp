/**
 * @file NetworkNormalEquations.hpp
 * @brief Arrowhead normal-equation accumulator with exact Schur reduction for
 *        the joint multi-station PPP-RTK network estimator (W4-S1).
 *
 * This is the numerical core of the minimum-constraint S-system network
 * PRODUCER (`ProcessingMode::PPP_RTK_Network`).  It realizes the locked-decision
 * topology — *per-station local normals + exact Schur reduction onto a shared
 * satellite-parameter block* — without touching the single-receiver core: each
 * station keeps its own `MeasurementModel`/`StateManager` (single-receiver
 * invariants intact) and contributes pre-formed local normal-equation blocks
 * here, coupled only through the shared satellite border.
 *
 * The network least-squares system has the bordered block-arrow ("arrowhead")
 * structure
 *
 *     [ N_ss   N_s1   N_s2  ...  N_sR ] [ x_s ]   [ u_s ]
 *     [ N_1s   N_11    0    ...   0   ] [ x_1 ]   [ u_1 ]
 *     [ N_2s    0     N_22  ...   0   ] [ x_2 ] = [ u_2 ]
 *     [  ...                          ] [ ... ]   [ ... ]
 *     [ N_Rs    0      0    ...  N_RR ] [ x_R ]   [ u_R ]
 *
 * where
 *   - @c x_s is the SHARED satellite-parameter block (satellite clock /
 *     phase bias / code bias) — *identical columns across every station*, and
 *   - each @c x_r is station r's PRIVATE nuisance block (receiver clock, ISB,
 *     ZWD, slant ionosphere, ZD ambiguities).
 *
 * The station-station off-diagonal blocks are exactly zero: stations couple
 * ONLY through @c x_s.  That sparsity is what makes the marginalization exact
 * and cheap.  Each station's nuisance block is Schur-eliminated (marginalized)
 * onto the shared border:
 *
 *     Ñ_ss += N_ss^r − N_sr^r (N_rr^r)^{-1} N_rs^r
 *     ũ_s  += u_s^r  − N_sr^r (N_rr^r)^{-1} u_r^r
 *
 * Solving the reduced shared system @f$ \tilde N_{ss}\,x_s = \tilde u_s @f$ and
 * back-substituting per station
 *
 *     x_r = (N_rr^r)^{-1} (u_r^r − N_rs^r x_s)
 *
 * reproduces the dense stacked normal-equation solve to machine precision (the
 * reduction is a *lossless* marginalization, not an approximation — cf. the
 * two-stage covariance-form alternative the design panel rejected).  Fill-in is
 * confined to the satellite border and each station's elimination is
 * independent (parallelizable).
 *
 * This mirrors NGS M-PAGES `NgsLsq::NormalEquations::reduce` /
 * `reconstructLocals` (reference at
 * `reference_software/mpages/M-PAGES/Lib/NgsLsq/NgsLsq.cpp:278,380`), specialized
 * to the network arrowhead (the GLOBAL block is the shared satellite border;
 * the per-epoch swept block becomes N independent station blocks).
 *
 * SCOPE (W4-S1): this kernel is pure linear algebra over abstract shared/local
 * blocks.  It owns no GNSS state, no `StateManager`, no epoch loop, and no
 * `StateKeySchema` keys — those land in W4-S2 (`NetworkEstimator`, the epoch
 * driver).  Deliberately deferred out of this slice:
 *   - W4-S2: the epoch loop, per-station block assembly from the single-receiver
 *     `MeasurementModel`/`StateManager`, time update, and covariance recovery /
 *     a-posteriori variance factor (the M-PAGES "basement window"); this kernel
 *     accumulates no goodness-of-fit and reports no error bars.
 *   - W5 (`NetworkSbasis`): the explicit minimum-constraint datum and the
 *     null-space rank gate (SVD the pre-constraint reduced normals, assert the
 *     small-singular-value count equals the declared S-basis dimension).  W5
 *     consumes this kernel through the seam: @ref reduce, then inspect/constrain
 *     a COPY of @ref reducedSharedNormals / @ref reducedSharedRhs (apply hard
 *     pins / a guarded S-transform), then @ref solveSharedConstrained, then
 *     @ref reconstructStation per station.
 * Soft physical priors and additive exact-gauge pins on the shared border can
 * also be applied pre-reduction via @ref addSharedInfo (they carry through the
 * Schur reduction unchanged).
 */

#pragma once

#include <Eigen/Dense>

#include <cstddef>
#include <vector>

namespace GnssProcessor {

/**
 * @brief Accumulates per-station normal-equation blocks into an arrowhead
 *        system and exactly Schur-reduces the station nuisance blocks onto the
 *        shared satellite border.
 *
 * Typical usage (one solution epoch):
 * @code
 *   NetworkNormalEquations neq(n_shared);
 *   neq.addSharedInfo(prior_info, prior_rhs);          // S-basis / physical priors
 *   for (each station r)
 *       neq.addStationObservations(A_s_r, A_r_r, W_r, y_r);
 *   const Eigen::VectorXd x_s = neq.solveShared();      // reduce() + solve
 *   for (each station r)
 *       Eigen::VectorXd x_r = neq.reconstructStation(r, x_s);
 * @endcode
 *
 * The reduced shared system can also be inspected/constrained before solving:
 * @ref reducedSharedNormals / @ref reducedSharedRhs (after @ref reduce) expose
 * @f$\tilde N_{ss},\ \tilde u_s@f$ so a `NetworkSbasis` can apply the explicit
 * minimum-constraint datum (W5) prior to @ref solveShared.
 *
 * @note Not thread-safe.  Add/reduce/solve are a single-threaded sequence; the
 *       per-station elimination is internally trivially parallelizable but kept
 *       serial here (Eigen already threads the dense kernels under OpenMP).
 */
class NetworkNormalEquations {
public:
    /**
     * @brief Construct an empty system over a shared block of @p n_shared
     *        satellite parameters.
     * @param n_shared Dimension of the shared satellite-parameter block @c x_s
     *        (≥ 0).  The shared normals/RHS start at zero.
     */
    explicit NetworkNormalEquations(Eigen::Index n_shared);

    /**
     * @brief Add a deterministic contribution directly to the shared border.
     *
     * Used for exact-gauge S-basis constraints and genuine physical priors on
     * the satellite parameters (network mean / pivot pin / soft bias prior).
     * Accumulates @f$ N_{ss} \mathrel{+}= \Delta N,\ u_s \mathrel{+}= \Delta u @f$.
     * A diagonal info prior @f$ p\,I @f$ with target @c x0 contributes
     * @f$\Delta N = p I,\ \Delta u = p\,x0@f$.
     *
     * @param delta_info @c n_shared × @c n_shared symmetric information increment.
     * @param delta_rhs  @c n_shared right-hand-side increment.
     * @throws std::invalid_argument on a dimension mismatch.
     */
    void addSharedInfo(const Eigen::MatrixXd& delta_info,
                       const Eigen::VectorXd& delta_rhs);

    /**
     * @brief Add one station from pre-formed normal-equation blocks.
     *
     * @param N_ss_r This station's contribution to the shared block
     *               @f$A_s^\top W A_s@f$ (@c n_shared × @c n_shared, symmetric).
     * @param N_sr_r Shared↔local cross block @f$A_s^\top W A_r@f$
     *               (@c n_shared × @c n_local).
     * @param N_rr_r Local block @f$A_r^\top W A_r@f$ (@c n_local × @c n_local).
     *               Must be symmetric positive-definite (include the station's
     *               prior so it is full-rank); it is factorized (LDLᵀ) on entry
     *               and the factor is cached for reduction and reconstruction.
     * @param u_s_r  Shared RHS contribution @f$A_s^\top W y@f$ (@c n_shared).
     * @param u_r_r  Local RHS @f$A_r^\top W y@f$ (@c n_local).
     * @return The 0-based station index, for @ref reconstructStation.
     * @throws std::invalid_argument on a dimension mismatch.
     *
     * @note @c n_local may be 0 (a station with no private nuisance state);
     *       its contribution is then just @p N_ss_r / @p u_s_r added to the
     *       border with no elimination.
     */
    std::size_t addStation(const Eigen::MatrixXd& N_ss_r,
                           const Eigen::MatrixXd& N_sr_r,
                           const Eigen::MatrixXd& N_rr_r,
                           const Eigen::VectorXd& u_s_r,
                           const Eigen::VectorXd& u_r_r);

    /**
     * @brief Add one station from its linearized design-matrix rows with a full
     *        (possibly correlated) weight matrix.
     *
     * Forms the normal-equation blocks
     * @f$N_{ss}=A_s^\top W A_s,\ N_{sr}=A_s^\top W A_r,\ N_{rr}=A_r^\top W A_r,\
     *    u_s=A_s^\top W y,\ u_r=A_r^\top W y@f$ and forwards to @ref addStation.
     * The full @p W exactly propagates a non-diagonal undifferenced measurement
     * covariance (correlated multipath / shared pre-corrections).
     *
     * @param A_s Jacobian wrt the shared parameters (@c m × @c n_shared).
     * @param A_r Jacobian wrt this station's locals (@c m × @c n_local).
     * @param W   Symmetric positive-definite weight (inverse measurement
     *            covariance), @c m × @c m.
     * @param y   Prefit residual (observed − computed), length @c m.
     * @return The 0-based station index.
     * @throws std::invalid_argument on a dimension mismatch.
     */
    std::size_t addStationObservations(const Eigen::MatrixXd& A_s,
                                       const Eigen::MatrixXd& A_r,
                                       const Eigen::MatrixXd& W,
                                       const Eigen::VectorXd& y);

    /**
     * @brief Overload taking a diagonal weight (per-observation variances⁻¹).
     * @param w_diag Diagonal weights, length @c m (each = 1/σ²).
     * @copydetails addStationObservations
     */
    std::size_t addStationObservations(const Eigen::MatrixXd& A_s,
                                       const Eigen::MatrixXd& A_r,
                                       const Eigen::VectorXd& w_diag,
                                       const Eigen::VectorXd& y);

    /**
     * @brief Schur-eliminate every station's nuisance block onto the shared
     *        border, producing @f$\tilde N_{ss},\ \tilde u_s@f$.
     *
     * Recomputes the reduced form from the immutable accumulated pre-reduction
     * blocks (@c N_ss / @c u_s), so it is idempotent when no @c add* call
     * intervenes: calling @ref reduce twice in a row yields identical results.
     * An @ref addStation / @ref addSharedInfo marks the cached reduction stale
     * (via an internal flag) so the next @ref solveShared recomputes it; it does
     * not corrupt the previously cached @ref reducedSharedNormals.  Per-station
     * LDLᵀ factors are reused (factorization happens once, in @ref addStation).
     */
    void reduce();

    /**
     * @brief Solve the reduced shared system @f$\tilde N_{ss}\,x_s=\tilde u_s@f$.
     *
     * Lazily calls @ref reduce if needed.  The reduced block must be full rank —
     * the caller is responsible for having applied the S-basis minimum
     * constraint (W5) / sufficient priors via @ref addSharedInfo; an
     * unconstrained satellite-clock block is rank-deficient by one datum per
     * constellation.
     *
     * @return The shared satellite-parameter estimate @c x_s (length @c n_shared).
     */
    Eigen::VectorXd solveShared();

    /**
     * @brief Solve an externally-constrained reduced shared system.
     *
     * The W5 seam: after @ref reduce, a `NetworkSbasis` reads
     * @ref reducedSharedNormals / @ref reducedSharedRhs, applies the explicit
     * minimum-constraint datum to a COPY (hard pin / guarded S-transform), and
     * passes the result here.  This keeps the kernel's own reduced system
     * pristine (so @ref reconstructStation and a re-solve stay valid) while
     * letting the datum live entirely in W5.  The returned @c x_s is consistent
     * with @ref reconstructStation (the locals are reconstructed against
     * whatever shared solution is supplied).
     *
     * @param constrained_N_ss The datum-constrained reduced normals
     *                         (@c n_shared × @c n_shared, must be full rank).
     * @param constrained_u_s  The matching reduced RHS (length @c n_shared).
     * @return The shared estimate @c x_s.
     * @throws std::invalid_argument on a dimension mismatch or a still-singular
     *         system (the applied constraint did not remove the deficiency).
     */
    Eigen::VectorXd solveSharedConstrained(const Eigen::MatrixXd& constrained_N_ss,
                                           const Eigen::VectorXd& constrained_u_s) const;

    /**
     * @brief Back-substitute a station's nuisance estimate given the shared
     *        solution: @f$x_r=(N_{rr}^r)^{-1}(u_r^r - N_{rs}^r x_s)@f$.
     * @param station Index returned by @ref addStation / @ref addStationObservations.
     * @param x_s     The shared estimate from @ref solveShared (length @c n_shared).
     * @return The station's nuisance estimate (length @c n_local; empty if 0).
     * @throws std::out_of_range / std::invalid_argument on a bad index or size.
     */
    Eigen::VectorXd reconstructStation(std::size_t station,
                                       const Eigen::VectorXd& x_s) const;

    /// @return The reduced shared normal matrix @f$\tilde N_{ss}@f$ (after @ref reduce).
    const Eigen::MatrixXd& reducedSharedNormals() const { return reduced_N_ss_; }
    /// @return The reduced shared RHS @f$\tilde u_s@f$ (after @ref reduce).
    const Eigen::VectorXd& reducedSharedRhs() const { return reduced_u_s_; }

    /// @return The accumulated (pre-reduction) shared normals @f$N_{ss}=\sum_r N_{ss}^r + \text{priors}@f$.
    const Eigen::MatrixXd& sharedNormals() const { return N_ss_; }
    /// @return The accumulated (pre-reduction) shared RHS @f$u_s@f$.
    const Eigen::VectorXd& sharedRhs() const { return u_s_; }

    /// @return Shared-block dimension @c n_shared.
    Eigen::Index numShared() const { return n_shared_; }
    /// @return Number of stations added.
    std::size_t numStations() const { return stations_.size(); }
    /// @return Local dimension of station @p station.
    Eigen::Index numLocal(std::size_t station) const;
    /// @return Whether @ref reduce has run since the last mutation.
    bool isReduced() const { return reduced_; }

private:
    /// Per-station cached state needed for reduction and back-substitution.
    struct Station {
        Eigen::MatrixXd N_sr;                  ///< shared↔local cross block (n_s × n_r)
        Eigen::VectorXd u_r;                   ///< local RHS (n_r)
        Eigen::LDLT<Eigen::MatrixXd> Nrr_ldlt; ///< cached LDLᵀ factor of N_rr
        Eigen::Index n_local{0};               ///< local dimension
    };

    Eigen::Index n_shared_;       ///< shared-block dimension
    Eigen::MatrixXd N_ss_;        ///< Σ_r N_ss^r + priors (pre-reduction)
    Eigen::VectorXd u_s_;         ///< Σ_r u_s^r + priors (pre-reduction)
    std::vector<Station> stations_;

    Eigen::MatrixXd reduced_N_ss_; ///< Ñ_ss (valid when reduced_)
    Eigen::VectorXd reduced_u_s_;  ///< ũ_s  (valid when reduced_)
    bool reduced_{false};
};

} // namespace GnssProcessor
