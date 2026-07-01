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
 * @note Not thread-safe across calls on one object: add/reduce/solve are a
 *       single-threaded sequence.  @ref reduce may INTERNALLY fan its per-station
 *       elimination across threads (opt-in via its @c n_threads argument) — each
 *       station's blocks are read-only and own a private LDLᵀ factor, so the loop
 *       is race-free and combined deterministically for a fixed thread count.  The
 *       driver pins Eigen itself to one thread for the epoch, so this coarse
 *       station-level parallelism does not nest with Eigen's own kernel threading.
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
     * @brief Re-initialize for a new epoch over a (possibly different) shared
     *        block, REUSING the existing heap allocations.
     *
     * Equivalent in result to assigning a freshly-constructed
     * `NetworkNormalEquations(n_shared)`, but it does NOT free and reallocate the
     * per-station blocks: the @ref stations_ pool is retained (its count reset to
     * zero) so each epoch's @ref addStation overwrites a pooled @c Station's dense
     * buffers in place rather than allocating a new one.  This collapses the
     * per-epoch large-block allocate/free churn that the process heap would
     * otherwise retain as committed memory — the dominant driver of the network
     * producer's memory growth at scale (≈0.5 GB/station for a multi-epoch run).
     * After a few epochs the pooled buffers reach their high-water size and stop
     * reallocating entirely.
     *
     * @param n_shared Dimension of the shared satellite-parameter block for the
     *        new epoch (≥ 0).  The shared normals/RHS are zeroed (buffer reused
     *        when the size is unchanged).
     */
    void reset(Eigen::Index n_shared);

    /**
     * @brief Add a deterministic contribution directly to the shared border.
     *
     * Used for exact-gauge S-basis constraints and genuine physical priors on
     * the satellite parameters (network mean / pivot pin / soft bias prior).
     * Accumulates @f$ N_{ss} \mathrel{+}= \Delta N,\ u_s \mathrel{+}= \Delta u @f$.
     * A diagonal info prior @f$ p\,I @f$ with target @c x0 contributes
     * @f$\Delta N = p I,\ \Delta u = p\,x0@f$.
     *
     * Reduction-preserving: a shared-border increment carries through the Schur
     * reduction unchanged (the per-station subtrahends depend only on @c N_sr /
     * @c N_rr, never on @c N_ss), so when @ref reduce has already run this delta is
     * folded into the cached reduced normals in place and the reduction stays
     * current — sparing a full re-reduction for a post-reduce prior (e.g. the
     * driver's D2 carry-forward, added between the rank gate and the solve).  The
     * in-place fold symmetrizes @p delta_info so it is exact for any symmetric
     * increment regardless of floating-point asymmetry.
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
     * @brief Fold a DIAGONAL information increment into one station's local
     *        block @f$N_{rr}^r / u_r^r@f$ (the per-station analogue of
     *        @ref addSharedInfo) — the carried-ambiguity phase-leveling prior.
     *
     * For each requested local column @p local_cols[i] this applies
     * @f[
     *   N_{rr}(c,c)\mathrel{+}=\Lambda_i,\qquad u_r(c)\mathrel{+}=\eta_i,
     * @f]
     * with @f$\Lambda_i=@f$ @p delta_Nrr_diag[i] (an information weight,
     * @f$1/\text{var}_\text{eff}@f$, @b non-negative) and @f$\eta_i=@f$
     * @p delta_ur[i] (the information-form RHS @f$\Lambda_i\cdot\text{target}@f$,
     * with @c target expressed in the station's CORRECTION frame — i.e. the
     * carried absolute value minus that ambiguity's current linearization point,
     * matching the kernel's @f$x_r=\Delta x@f$ convention).
     *
     * The update is realized as a sequence of rank-1 updates of the cached
     * @f$N_{rr}@f$ LDLᵀ factor (`Eigen::LDLT::rankUpdate(e_c, Λ_i)`), so the raw
     * @f$N_{rr}@f$ need never be stored or re-factorized from scratch: a positive
     * @f$\Lambda_i@f$ keeps the factor positive-definite by construction.  The
     * shared border @f$N_{ss}/u_s@f$ is left UNTOUCHED — a per-station local
     * increment is reduction-orthogonal to every other station and to the shared
     * null-space (it conditions only this station's @f$x_r@f$), which is what makes
     * the carried prior safe and cheap (see the file-level correctness notes).
     *
     * @par Ordering contract
     * MUST be called AFTER @ref addStation (it mutates that station's cached
     * factor) and BEFORE @ref reduce: a local-block change alters the Schur
     * subtrahend, which is baked into the reduced normals during @ref reduce, and
     * there is NO post-reduce in-place fast-path for it (unlike @ref addSharedInfo,
     * whose shared-border delta bypasses Schur).  This call marks the cached
     * reduction stale so the next @ref reduce recomputes exactly once.  For
     * byte-identical determinism the caller MUST pass @p local_cols in a fixed
     * (e.g. ascending) order — the rank-1 update sequence is order-dependent at ULP.
     *
     * @param station        Index returned by @ref addStation (must have local state).
     * @param local_cols     Local columns (within @c A_r) receiving a prior.
     * @param delta_Nrr_diag Per-column information increments @f$\Lambda_i\ge0@f$.
     * @param delta_ur       Per-column RHS increments @f$\eta_i@f$.
     * @throws std::out_of_range / std::invalid_argument on a bad index, a size
     *         mismatch, an out-of-range column, or a negative information weight.
     */
    void addStationPrior(std::size_t station,
                         const std::vector<Eigen::Index>& local_cols,
                         const Eigen::VectorXd& delta_Nrr_diag,
                         const Eigen::VectorXd& delta_ur);

    /**
     * @brief Schur-eliminate every station's nuisance block onto the shared
     *        border, producing @f$\tilde N_{ss},\ \tilde u_s@f$.
     *
     * Caches the result: a no-op when the reduction is already current (the
     * internal @c reduced_ flag), so calling @ref reduce twice in a row yields
     * identical results.  @ref addStation marks the cache stale; @ref addSharedInfo
     * instead carries its shared-border delta straight through the reduction in
     * place (keeping it current), so a post-reduce prior — e.g. the driver's D2
     * carry-forward applied between the rank gate and the solve — does NOT force a
     * second full per-station Schur pass.  Per-station LDLᵀ factors are reused
     * (factorization happens once, in @ref addStation).
     *
     * @param n_threads Optional fan-out of the per-station Schur loop (default 1 =
     *        serial, bit-identical to the historical loop).  With @p n_threads > 1
     *        and OpenMP, stations are eliminated in parallel via per-thread partial
     *        accumulators combined in a fixed (ascending station-range, ascending
     *        thread) order: the result is reproducible for a given thread count but
     *        differs from the serial fold at ULP (a different summation grouping).
     *        Callers needing byte-identical output across thread counts pass 1.
     *        The dense kernels inside the loop must run single-threaded (the driver
     *        pins Eigen to one thread for the epoch) to avoid nested oversubscription.
     */
    void reduce(int n_threads = 1);

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
     * @param cov_out If non-null, receives the formal covariance of @c x_s — the
     *               inverse @f$(\tilde N_{ss}^{\text{constrained}})^{-1}@f$ computed
     *               from the SAME LDLᵀ factor used for the point solve (so the
     *               caller avoids a redundant second factorization).  The
     *               datum-pinned columns carry the implementation's @c 1/pin_scale
     *               variance; treat them as exact downstream.  Empty when
     *               @c n_shared is 0.
     * @return The shared estimate @c x_s.
     * @throws std::invalid_argument on a dimension mismatch or a still-singular
     *         system (the applied constraint did not remove the deficiency).
     */
    Eigen::VectorXd solveSharedConstrained(const Eigen::MatrixXd& constrained_N_ss,
                                           const Eigen::VectorXd& constrained_u_s,
                                           Eigen::MatrixXd* cov_out = nullptr) const;

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

    /**
     * @brief Conditional covariance of a station's nuisance block, propagating
     *        the shared-parameter covariance @p cov_free_xs into the locals.
     *
     * Applies the marginal-covariance identity (the (r,r) block of the full
     * bordered inverse)
     * @f[
     *   \mathrm{Cov}(x_r) = N_{rr}^{-1}
     *     + N_{rr}^{-1} N_{rs}\,\mathrm{Cov}(x_s)\,N_{sr}\,N_{rr}^{-1},
     * @f]
     * reusing the cached @f$N_{rr}@f$ LDLᵀ factor and @f$N_{rs}=N_{sr}^\top@f$.
     * Pass the FULL shared covariance to get the marginal local covariance; pass
     * it with the rows/columns of any EXACT (datum-pinned) shared parameter
     * zeroed to get @f$\mathrm{Cov}(x_r\mid x_s^{\text{free}})@f$ — the frame in
     * which between-station double-difference ambiguities are integer-estimable
     * (producer-side network AR, Phase 1).  Treating the gauge columns as known
     * (variance 0) instead of carrying their artificial @c 1/pin_scale variance
     * is the load-bearing correctness step: it keeps the DD ambiguity covariance
     * from being inflated by the datum's implementation artifact.
     *
     * @param station      Index from @ref addStation.
     * @param cov_free_xs  Shared-parameter covariance (@c n_shared × @c n_shared),
     *                     datum-pinned rows/cols zeroed for the conditional form.
     * @return The station's @c n_local × @c n_local covariance (symmetric; empty
     *         if the station has no locals).
     * @throws std::out_of_range / std::invalid_argument on a bad index or size.
     */
    Eigen::MatrixXd reconstructStationCovariance(
        std::size_t station, const Eigen::MatrixXd& cov_free_xs) const;

    /**
     * @brief Diagonal of @ref reconstructStationCovariance for a SUBSET of local
     *        columns — the cheap per-ambiguity conditional-variance harvest.
     *
     * Returns, for each @p local_cols[i], the @f$(c,c)@f$ entry of
     * @f$\mathrm{Cov}(x_r)=N_{rr}^{-1}+M\,\mathrm{Cov}_\text{free}\,M^\top@f$ with
     * @f$M=N_{rr}^{-1}N_{rs}@f$ — computed WITHOUT forming the full
     * @f$N_{rr}^{-1}@f$.  Each column costs one local solve
     * @f$n_c=N_{rr}^{-1}e_c@f$ (giving @f$(N_{rr}^{-1})_{cc}=n_c(c)@f$) plus the
     * shared propagation @f$ (N_{sr}n_c)^\top\,\mathrm{Cov}_\text{free}\,(N_{sr}n_c)@f$,
     * i.e. @f$O(k\,(n_r^2+n_s^2))@f$ for @c k requested columns instead of the
     * @f$O(n_r^3)@f$ full inverse @ref reconstructStationCovariance pays — the
     * per-station full inverse being a known serial-path cost at scale.  Pass the
     * SAME @p cov_free_xs (datum-pinned shared rows/cols zeroed) as the full form.
     *
     * @param station     Index from @ref addStation.
     * @param local_cols  Local columns whose conditional variance is wanted.
     * @param cov_free_xs Shared-parameter covariance (datum-pinned rows/cols zeroed).
     * @return A vector of conditional variances, one per requested column (empty if
     *         the station has no locals or @p local_cols is empty).
     * @throws std::out_of_range / std::invalid_argument on a bad index or column.
     */
    Eigen::VectorXd reconstructStationCovarianceDiag(
        std::size_t station, const std::vector<Eigen::Index>& local_cols,
        const Eigen::MatrixXd& cov_free_xs) const;

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
    /// @return Number of stations added this epoch (the active pool count).
    std::size_t numStations() const { return n_active_; }
    /// @return Local dimension of station @p station.
    Eigen::Index numLocal(std::size_t station) const;
    /// @return Whether the cached reduction is current (set by @ref reduce, kept
    ///         current by @ref addSharedInfo, cleared by @ref addStation /
    ///         @ref addStationPrior / @ref reset).
    bool isReduced() const { return reduced_; }

    /// @return How many times @ref reduce has actually RECOMPUTED the per-station
    ///         Schur loop since the last @ref reset (idempotent no-op returns do
    ///         not count).  Reset to 0 by @ref reset; used to assert the
    ///         "reduce-once-per-epoch" invariant when the carried prior is on (a
    ///         stray post-gate local injection would force a second full reduce).
    std::size_t reduceRecomputeCount() const { return reduce_recompute_count_; }

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
    std::vector<Station> stations_;  ///< per-station block POOL (capacity persists across @ref reset)
    std::size_t n_active_{0};        ///< number of pool slots in use this epoch (≤ stations_.size())

    Eigen::MatrixXd reduced_N_ss_; ///< Ñ_ss (valid when reduced_)
    Eigen::VectorXd reduced_u_s_;  ///< ũ_s  (valid when reduced_)
    bool reduced_{false};
    std::size_t reduce_recompute_count_{0}; ///< #full reduce() recomputes since reset() (reduce-once invariant)
};

} // namespace GnssProcessor
