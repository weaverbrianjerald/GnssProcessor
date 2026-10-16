/**
 * @file NormalEquations.hpp
 * @brief Arrowhead normal-equation accumulator with exact Schur reduction for
 *        the joint multi-station PPP-RTK network estimator.
 *
 * The numerical core of the minimum-constraint S-system network producer
 * (`ProcessingMode::PPP_RTK_Network`): per-station local normals are reduced onto
 * a shared satellite-parameter block without touching the single-receiver core.
 * Each station keeps its own `MeasurementModel`/`StateManager`, so the
 * single-receiver invariants hold, and contributes pre-formed local
 * normal-equation blocks here, coupled to the other stations only through the
 * shared satellite border.
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
 *   - @c x_s is the shared satellite-parameter block (satellite clock, phase
 *     bias, code bias), with the same columns for every station, and
 *   - each @c x_r is station r's private nuisance block (receiver clock, ISB,
 *     ZWD, slant ionosphere, ZD ambiguities).
 *
 * The station-station off-diagonal blocks are exactly zero: stations couple only
 * through @c x_s, and that sparsity makes the marginalization exact and cheap.
 * Each station's nuisance block is Schur-eliminated (marginalized) onto the
 * shared border:
 *
 *     Ñ_ss += N_ss^r − N_sr^r (N_rr^r)^{-1} N_rs^r
 *     ũ_s  += u_s^r  − N_sr^r (N_rr^r)^{-1} u_r^r
 *
 * Solving the reduced shared system @f$ \tilde N_{ss}\,x_s = \tilde u_s @f$ and
 * back-substituting per station
 *
 *     x_r = (N_rr^r)^{-1} (u_r^r − N_rs^r x_s)
 *
 * reproduces the dense stacked normal-equation solve to machine precision: the
 * reduction is a lossless marginalization, not an approximation.  Fill-in stays
 * confined to the satellite border and each station's elimination is independent,
 * so the eliminations can run in parallel.
 *
 * This mirrors NGS M-PAGES `NgsLsq::NormalEquations::reduce` /
 * `reconstructLocals` (`reference_software/mpages/M-PAGES/Lib/NgsLsq/NgsLsq.cpp`),
 * specialized to the network arrowhead: the global block is the shared satellite
 * border, and the per-epoch swept block becomes N independent station blocks.
 *
 * Scope: linear algebra over abstract shared and local blocks.  The kernel owns
 * no GNSS state, no `StateManager`, no epoch loop and no `StateKeySchema` keys,
 * and it accumulates no goodness-of-fit, so it reports no error bars.  Its two
 * consumers supply those:
 *   - `NetworkEstimator`, the epoch driver: per-station block assembly, time
 *     update, covariance recovery and the a-posteriori variance factor.
 *   - `NetworkSbasis`: the explicit minimum-constraint datum and the null-space
 *     rank gate (an SVD of the pre-constraint reduced normals; the count of small
 *     singular values must equal the declared S-basis dimension).
 *
 * NetworkSbasis drives this kernel in this order: @ref reduce; inspect and
 * constrain a copy of @ref reducedSharedNormals / @ref reducedSharedRhs (hard
 * pins or a guarded S-transform); @ref solveSharedConstrained; then
 * @ref reconstructStation per station.  Soft physical priors and additive
 * exact-gauge pins on the shared border can also be applied before the reduction
 * through @ref addSharedInfo; they carry through the Schur reduction unchanged.
 */

#pragma once

#include <Eigen/Dense>

#include <cmath>      // std::isfinite (the carry-aware readback's refuse-never-clamp screen)
#include <cstddef>
#include <memory>     // std::shared_ptr (the detached fold's opaque factor)
#include <utility>
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
 * The reduced shared system can also be inspected and constrained before the
 * solve: after @ref reduce, @ref reducedSharedNormals / @ref reducedSharedRhs
 * expose @f$\tilde N_{ss},\ \tilde u_s@f$, so a `NetworkSbasis` can apply the
 * explicit minimum-constraint datum (W5) to a copy and solve it with
 * @ref solveSharedConstrained.
 *
 * @note Not thread-safe across calls on one object: add, reduce and solve form a
 *       single-threaded sequence.  @ref reduce can fan its per-station
 *       elimination across threads internally (opt-in through its @c n_threads
 *       argument); each station's blocks are read-only and own a private LDLᵀ
 *       factor, so the loop is race-free, and the partial sums are combined
 *       deterministically for a fixed thread count.  The driver pins Eigen to one
 *       thread for the epoch, so this station-level parallelism does not nest with
 *       Eigen's own kernel threading.
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
     *        block, reusing the existing heap allocations.
     *
     * Afterwards the per-epoch system (normals, RHS, stations, cached reduction,
     * folded gauges and DD priors, and the last fold's @ref DdFoldStats) is as if
     * `NetworkNormalEquations(n_shared)` had been constructed, but the per-station
     * blocks are not freed: the @ref stations_ pool is retained (its active count
     * reset to zero), and each epoch's @ref addStation overwrites a pooled
     * @c Station's dense buffers in place.  Without the pool, the process heap
     * retains the per-epoch allocate/free churn of large blocks as committed
     * memory, the main source of the network producer's memory growth at scale
     * (about 0.5 GB per station over a multi-epoch run).  After a few epochs the
     * pooled buffers reach their high-water size and stop reallocating.
     * Configuration set through the @c set* methods, the run-cumulative telemetry
     * (@ref SectionTimers, @ref CarryCovStats) and the last PD-rejection record
     * (@ref lastStationPdFailure) survive a reset.
     *
     * @param n_shared Dimension of the shared satellite-parameter block for the
     *        new epoch (≥ 0).  The shared normals/RHS are zeroed (buffer reused
     *        when the size is unchanged).
     */
    void reset(Eigen::Index n_shared);

    /**
     * @brief Add a deterministic contribution directly to the shared border.
     *
     * Used for exact-gauge S-basis constraints and physical priors on the
     * satellite parameters (network mean, pivot pin, soft bias prior).
     * Accumulates @f$ N_{ss} \mathrel{+}= \Delta N,\ u_s \mathrel{+}= \Delta u @f$.
     * A diagonal info prior @f$ p\,I @f$ with target @c x0 contributes
     * @f$\Delta N = p I,\ \Delta u = p\,x0@f$.
     *
     * A shared-border increment carries through the Schur reduction unchanged (the
     * per-station subtrahends depend only on @c N_sr and @c N_rr, never on
     * @c N_ss).  When @ref reduce has already run, the delta is therefore added to
     * the cached reduced normals in place and the reduction stays current, so a
     * post-reduce prior (for example the driver's D2 carry-forward, added between
     * the rank gate and the solve) does not force a full re-reduction.  The
     * in-place update adds the symmetric part of @p delta_info, so a symmetric
     * increment is folded exactly even when its stored entries differ from their
     * transposes at rounding level.
     *
     * @param delta_info @c n_shared × @c n_shared symmetric information increment.
     * @param delta_rhs  @c n_shared right-hand-side increment.
     * @throws std::invalid_argument on a dimension mismatch.
     */
    void addSharedInfo(const Eigen::MatrixXd& delta_info,
                       const Eigen::VectorXd& delta_rhs);

    /**
     * @brief A station's diagonal prior rows, passed to @ref addStation so that
     *        the positive-definiteness test judges the block the solve uses
     *        (@c network.station_prior_before_pd).
     *
     * The same triple @ref addStationPrior takes (local columns, per-column
     * information increments @f$\Lambda_i\ge0@f$ and RHS increments
     * @f$\eta_i@f$), but folded into @f$N_{rr}^r@f$ / @f$u_r^r@f$ before the LDLᵀ
     * factorization instead of as rank-1 updates of the cached factor afterwards.
     * The order matters for a station whose observation-only block is
     * rank-deficient in the direction its own datum row gauges (the receiver-DCB ↔
     * slant-iono ↔ ambiguity confound described in
     * @c docs/refound/RF-PD_station_rejection_2026-09-15.md): with the prior
     * applied afterwards, the test rejects the block, the station is dropped for
     * the epoch, and the @ref addStationPrior call that would make it full-rank is
     * never reached.
     *
     * @par Why not the rank-1 path
     * @c Eigen::LDLT::rankUpdate divides by the existing pivots @f$d_j@f$, so it
     * cannot update the factor of a singular block, which is the case this path
     * admits.  A prior that removes a rank defect has to be inside the matrix
     * that is factorized.
     *
     * Empty (the default): @ref addStation takes no copy of @p N_rr_r and folds
     * nothing, so the result is byte-identical to a call without a prior.
     */
    struct StationPrior {
        std::vector<Eigen::Index> cols;  ///< Local columns, in ascending order (determinism).
        Eigen::VectorXd dNrr;            ///< Per-column information increments @f$\Lambda_i\ge0@f$.
        Eigen::VectorXd dur;             ///< Per-column RHS increments @f$\eta_i@f$.
        /// True when there is nothing to fold (the OFF path, and the default).
        bool empty() const { return cols.empty(); }
    };

    /**
     * @brief Add one station from pre-formed normal-equation blocks.
     *
     * @param N_ss_r This station's contribution to the shared block
     *               @f$A_s^\top W A_s@f$ (@c n_shared × @c n_shared, symmetric).
     * @param N_sr_r Shared↔local cross block @f$A_s^\top W A_r@f$
     *               (@c n_shared × @c n_local).
     * @param N_rr_r Local block @f$A_r^\top W A_r@f$ (@c n_local × @c n_local).
     *               Must be symmetric positive-definite once the station's prior
     *               is included, either inside this block or through @p prior,
     *               which is folded in here.  The block is factorized (LDLᵀ) on
     *               entry and the factor is cached for reduction and
     *               reconstruction.
     * @param u_s_r  Shared RHS contribution @f$A_s^\top W y@f$ (@c n_shared).
     * @param u_r_r  Local RHS @f$A_r^\top W y@f$ (@c n_local).
     * @param prior  Optional diagonal prior rows (@ref StationPrior) folded into
     *               @p N_rr_r / @p u_r_r before the factorization and its
     *               positive-definiteness test.  Empty: the legacy path, taken
     *               without copying @p N_rr_r.
     * @return The 0-based station index, for @ref reconstructStation.
     * @throws std::invalid_argument on a dimension mismatch, an asymmetric
     *         @p N_ss_r or @p N_rr_r, an invalid @p prior (size mismatch,
     *         out-of-range column, negative information weight or non-finite RHS,
     *         checked as @ref addStationPrior checks them), or a local block that
     *         fails the LDLᵀ factorization or the positive-definiteness test (see
     *         @ref setStationPdMedianScale).  Every check runs before anything is
     *         committed, so a rejected station leaves the shared border and the
     *         active station set unchanged and the driver can drop it.
     *
     * @note @c n_local may be 0 (a station with no private nuisance state);
     *       its contribution is then @p N_ss_r / @p u_s_r added to the border with
     *       no elimination.  A non-empty @p prior on such a station is a caller
     *       error and throws.
     * @note Folding rows through @p prior and folding the same rows again through
     *       @ref addStationPrior applies them twice (@ref addStationPrior does not
     *       know what entered here).  The driver routes each row through exactly
     *       one of the two.
     */
    std::size_t addStation(const Eigen::MatrixXd& N_ss_r,
                           const Eigen::MatrixXd& N_sr_r,
                           const Eigen::MatrixXd& N_rr_r,
                           const Eigen::VectorXd& u_s_r,
                           const Eigen::VectorXd& u_r_r,
                           const StationPrior& prior = StationPrior{});

    /**
     * @brief Add one station from its linearized design-matrix rows with a full
     *        (possibly correlated) weight matrix.
     *
     * Forms the normal-equation blocks
     * @f$N_{ss}=A_s^\top W A_s,\ N_{sr}=A_s^\top W A_r,\ N_{rr}=A_r^\top W A_r,\
     *    u_s=A_s^\top W y,\ u_r=A_r^\top W y@f$ and forwards to @ref addStation.
     * The full @p W exactly propagates a non-diagonal undifferenced measurement
     * covariance (correlated multipath, shared pre-corrections).
     *
     * @param A_s Jacobian wrt the shared parameters (@c m × @c n_shared).
     * @param A_r Jacobian wrt this station's locals (@c m × @c n_local).
     * @param W   Symmetric positive-definite weight (inverse measurement
     *            covariance), @c m × @c m.
     * @param y   Prefit residual (observed − computed), length @c m.
     * @param prior Optional diagonal prior rows folded into @f$N_{rr}@f$ /
     *              @f$u_r@f$ before the PD test; see @ref StationPrior and the
     *              @p prior parameter of @ref addStation.  Empty: no prior.
     * @return The 0-based station index.
     * @throws std::invalid_argument on a dimension mismatch, and whatever
     *         @ref addStation throws.
     */
    std::size_t addStationObservations(const Eigen::MatrixXd& A_s,
                                       const Eigen::MatrixXd& A_r,
                                       const Eigen::MatrixXd& W,
                                       const Eigen::VectorXd& y,
                                       const StationPrior& prior = StationPrior{});

    /**
     * @brief Overload taking a diagonal weight (per-observation variances⁻¹).
     * @param w_diag Diagonal weights, length @c m (each = 1/σ²).
     * @copydetails addStationObservations
     */
    std::size_t addStationObservations(const Eigen::MatrixXd& A_s,
                                       const Eigen::MatrixXd& A_r,
                                       const Eigen::VectorXd& w_diag,
                                       const Eigen::VectorXd& y,
                                       const StationPrior& prior = StationPrior{});

    /**
     * @brief Fold a diagonal information increment into one station's local
     *        block @f$N_{rr}^r / u_r^r@f$ (the per-station analogue of
     *        @ref addSharedInfo), for example the carried-ambiguity
     *        phase-leveling prior.
     *
     * For each requested local column @p local_cols[i] this applies
     * @f[
     *   N_{rr}(c,c)\mathrel{+}=\Lambda_i,\qquad u_r(c)\mathrel{+}=\eta_i,
     * @f]
     * with @f$\Lambda_i=@f$ @p delta_Nrr_diag[i] (an information weight,
     * @f$1/\text{var}_\text{eff}@f$, @b non-negative) and @f$\eta_i=@f$
     * @p delta_ur[i] (the information-form RHS @f$\Lambda_i\cdot\text{target}@f$,
     * with @c target expressed in the station's correction frame: the carried
     * absolute value minus that ambiguity's current linearization point, matching
     * the kernel's @f$x_r=\Delta x@f$ convention).
     *
     * The update is a sequence of rank-1 updates of the cached @f$N_{rr}@f$ LDLᵀ
     * factor (`Eigen::LDLT::rankUpdate(e_c, Λ_i)`), so the raw @f$N_{rr}@f$ is
     * never stored or re-factorized from scratch.  A positive @f$\Lambda_i@f$ keeps
     * the factor positive-definite by construction; a zero @f$\Lambda_i@f$ skips
     * the factor update, and its @f$\eta_i@f$ is still added to @f$u_r@f$.  The
     * pre-reduction border @f$N_{ss}/u_s@f$ is not modified: the increment reaches
     * the shared system only through this station's Schur subtrahend at the next
     * @ref reduce, and no other station's block changes.
     *
     * @par Ordering contract
     * Call after @ref addStation (this mutates that station's cached factor) and
     * before @ref reduce.  A local-block change alters the Schur subtrahend, which
     * @ref reduce bakes into the reduced normals, and unlike the shared-border
     * delta of @ref addSharedInfo it has no post-reduce in-place path.  When at
     * least one @f$\Lambda_i > 0@f$, this call marks the cached reduction stale so
     * that the next @ref reduce recomputes once.  For byte-identical results pass
     * @p local_cols in a fixed (for example ascending) order: the rank-1 update
     * sequence is order-dependent at ULP level.
     *
     * @param station        Index returned by @ref addStation (must have local state).
     * @param local_cols     Local columns (within @c A_r) receiving a prior.
     * @param delta_Nrr_diag Per-column information increments @f$\Lambda_i\ge0@f$.
     * @param delta_ur       Per-column RHS increments @f$\eta_i@f$.
     * @throws std::out_of_range / std::invalid_argument on a bad index, a size
     *         mismatch, a station without local state, an out-of-range column, a
     *         negative information weight or a non-finite RHS increment.  Every
     *         input is checked before the cached factor is modified.
     */
    void addStationPrior(std::size_t station,
                         const std::vector<Eigen::Index>& local_cols,
                         const Eigen::VectorXd& delta_Nrr_diag,
                         const Eigen::VectorXd& delta_ur);

    /// One anchor-free sum-to-zero-over-stations gauge, the ensemble
    /// phase-bias↔ambiguity datum: @f$\sum_{\text{terms}} x_{\text{local}} = 0@f$,
    /// each term a (station, local column), for example every receiver's ambiguity
    /// to one (satellite, signal).  It realizes the datum without a pivot receiver,
    /// so the emitted satellite phase bias references the whole receiver ensemble:
    /// no handover, no single-anchor point of failure, and a consistent
    /// between-satellite reference that keeps the user's DD ambiguity integer
    /// (Odijk S-system).  (Fix A)
    struct SumZeroGauge {
        std::vector<std::pair<std::size_t, Eigen::Index>> terms;  ///< (station index, local column)
        /// The shared (border) column this gauge fixes: the satellite phase-bias
        /// column referenced by the receiver ensemble.  Set by the driver.  The
        /// column is the gauge (null) direction of the unreduced normals, so after
        /// the rank-1 fold its slot of the shared covariance holds an artificial
        /// gauge-direction variance and phase↔clock/code cross terms, not a
        /// physical uncertainty.  Consumers of the datum-frame conditional
        /// covariance treat it as exact (variance 0): the NetworkEstimator
        /// readbacks (for example @ref NetworkEstimator::conditionalLocalVarianceDiag,
        /// @ref NetworkEstimator::conditionalLocalCovariance and
        /// @ref NetworkEstimator::rowRedundancy) zero its row and column as they
        /// zero the datum-pinned columns.  -1 = unset (for example in unit tests
        /// that exercise only the point solve, which does not read this).
        Eigen::Index shared_col{-1};
    };

    /**
     * @brief Fold anchor-free sum-to-zero gauges onto the reduced shared system as
     *        Schur-projected rank-1 corrections (the ensemble phase-bias datum).
     *
     * Each gauge @f$\sum_r z_r = 0@f$ couples local ambiguity columns across
     * stations, so it cannot be a shared-border row.  In the hard limit
     * (@f$w\to\infty@f$) of the soft constraint @f$w(\sum_r z_r)^2@f$, Woodbury
     * through the block-diagonal local normals gives a rank-1 update of the reduced
     * shared normals, computed from the cached per-station LDLᵀ factors alone:
     * @f[
     *   n_r = N_{rr}^{-1} e_{z_r},\quad
     *   \beta = \sum_r (n_r)_{z_r},\quad
     *   g = \sum_r N_{sr} n_r,\quad
     *   \gamma = \sum_r (N_{rr}^{-1} u_r)_{z_r},
     * @f]
     * @f[
     *   \tilde N_{ss} \mathrel{+}= g g^\top/\beta,\qquad
     *   \tilde u_s \mathrel{+}= g\,\gamma/\beta .
     * @f]
     * The stored @f$(g,\beta,\gamma)@f$ and the per-station term index let
     * @ref reconstructStation return the constrained locals
     * (@f$x_r \mathrel{-}= n_r(\gamma - g^\top x_s)/\beta@f$), so the carried and
     * QC ambiguities are in the same sum-to-zero gauge as the emitted bias.
     *
     * @par Ordering
     * Calls @ref reduce first when the reduction is stale, then folds; a second
     * call within the same reduction is a no-op.  Call after any @ref reduce that
     * rebuilt the reduced system and before the null-space gate reads
     * @ref reducedSharedNormals: the fold fixes the phase gauge, so the gate's
     * declared dimension stays equal to the clock deficiency count, as in the
     * anchor mode, whose local pins enter before the reduction.  A later
     * @ref reduce recompute or @ref reset clears the stored gauges (the rebuilt
     * reduced system does not carry the fold), so call this again after any
     * re-reduce; the driver does so, as it re-applies the code-bias and clock
     * constraints on the within-epoch RAIM re-solve.  A gauge with
     * @f$\beta\le0@f$ (no valid terms) or a non-finite @f$\beta@f$ or
     * @f$\gamma@f$ is skipped.
     *
     * @param gauges The sum-to-zero gauges to fold.  A term's station must be
     *        active this epoch and its column within that station's local block.
     * @throws std::out_of_range / std::invalid_argument on a bad station/column.
     */
    void foldReducedSumZeroGauges(const std::vector<SumZeroGauge>& gauges);

    /// @return Number of sum-to-zero gauges folded into the current reduced system
    ///         (0 when none folded since the last @ref reduce or @ref reset; a skipped
    ///         degenerate gauge is not counted).  Read by
    ///         @ref NetworkEstimator::solveHeldDetached, which refuses a folded system
    ///.
    std::size_t numSumZeroGauges() const { return sumzero_gauges_.size(); }

    /// The rank-K double-difference prior of one connected (const,signal)
    /// component: the invariant-carry z-store's contribution to the producer solve
    /// (Card-4, M1 phase C).  Each of the K rows is a DD ambiguity constraint
    /// @f$z_k=\sum(\text{coeff}\cdot x_{\text{local}})@f$ with signed coefficients on
    /// four local ambiguity columns (more for higher-order fundamental cycles)
    /// spanning up to four stations.  The quantity is gauge-free: the receiver and
    /// satellite phase biases cancel, so it touches no shared column.  The K carried
    /// DDs have prior mean @c zhat and a dense prior covariance @c Pz.  Fundamental
    /// cycles share tree edges, so the DDs are correlated and the block is folded
    /// jointly (rank K), not as K independent rank-1 folds.
    struct DdPriorBlock {
        struct Term {
            std::size_t   station;    ///< station index (from @ref addStation).
            Eigen::Index  local_col;  ///< ambiguity column within that station's @c A_r.
            double        coeff;      ///< signed cycle coefficient (±1 for a plain DD).
        };
        std::vector<std::vector<Term>> rows;  ///< K rows; row k = the k-th DD's signed terms.
        Eigen::MatrixXd Pz;                   ///< K×K prior covariance (symmetric positive-definite).
        Eigen::VectorXd zhat;                 ///< K prior means (the carried DD values).
    };

    /**
     * @brief Fold rank-K double-difference priors onto the reduced shared system as
     *        joint per-component Woodbury corrections.
     *
     * A DD prior @f$z\sim N(\hat z,P^z),\ z=C_L x_L@f$ on local ambiguity columns
     * couples columns across stations, so it cannot be a per-station @c N_rr
     * increment without breaking the block-diagonal arrowhead.  This fold is the
     * soft generalization of @ref foldReducedSumZeroGauges (which is its
     * hard-constraint limit @f$P^z\!\to\!0,\ K\!=\!1,\ \hat z\!=\!0@f$), and
     * Woodbury through the block-diagonal local normals gives, per component,
     * @f[
     *   B=\sum_r C_{L,r}N_{rr}^{-1}C_{L,r}^\top,\quad
     *   G=\sum_r N_{sr}N_{rr}^{-1}C_{L,r}^\top,\quad
     *   z_{\text{free}}=\sum_r C_{L,r}N_{rr}^{-1}u_r,\quad
     *   M=(P^z+B)^{-1},
     * @f]
     * @f[
     *   \tilde N_{ss}\mathrel{+}=G\,M\,G^\top,\qquad
     *   \tilde u_s\mathrel{+}=G\,M\,(z_{\text{free}}-\hat z).
     * @f]
     * @f$C_{L,r}@f$ is the block's @f$K\times n_r@f$ signed sub-matrix on station
     * @f$r@f$'s columns.  @f$B,G,z_{\text{free}}@f$ accumulate from the cached
     * per-station LDLᵀ factors (no re-reduce; the factors are not modified), and
     * the @f$K\times K@f$ middle inverse @f$M@f$ is per component and bounded in
     * size, so no @f$O(n^3)@f$ inverse of the border is needed.  The stored
     * @f$(G,M,z_{\text{free}},\hat z)@f$ drive the matching @ref reconstructStation
     * correction
     * @f$x_r\mathrel{-}=N_{rr}^{-1}C_{L,r}^\top M\,(z_{\text{free}}-G^\top x_s-\hat z)@f$,
     * so the reconstructed locals are the constrained (prior-folded) solution.
     *
     * @par Ordering, and exclusion of the sum-to-zero gauge fold
     * Calls @ref reduce first when the reduction is stale, then folds once; a
     * second call is a no-op until a @ref reduce recompute or @ref reset clears the
     * stored blocks (as for @ref foldReducedSumZeroGauges).  Call after
     * @ref reduce and before the solve.
     * @warning This fold and @ref foldReducedSumZeroGauges are both post-reduce
     * Woodbury folds that read the unmodified per-station @f$N_{rr}@f$ factor.
     * Composing the two over coupled local columns is inexact: it drops the cross
     * term @f$C_L N_{rr}^{-1} C_{L,\text{gauge}}^\top@f$ of the joint middle inverse,
     * because each fold sees the base @f$N_{rr}@f$, not the other's augmentation.
     * The two are therefore mutually exclusive: under @c carry_invariant the DD
     * priors replace the sum-to-zero phase-bias ensemble datum, and the per-epoch
     * phase datum comes from the pre-reduce tree pins (@ref addStationPrior).  The
     * pins fold into @f$N_{rr}@f$ before the reduce, so they compose exactly with
     * this post-reduce fold, which reads the already pinned factor.
     * @ref NetworkEstimator::solve throws if both are set.  A block whose
     * @f$P^z+B@f$ fails the positive-definiteness screen folds inertly
     * (@f$M=0@f$, an index-stable no-op) instead of being dropped, so the
     * @ref ddPriorPosteriorMean / @ref numDdPriorBlocks keys stay aligned with the
     * input block order (a chordless @f$K=0@f$ block is an inert slot too).
     *
     * @param blocks One rank-K prior block per connected component.
     * @param n_threads Optional deterministic fan-out of the per-block fold
     *        (@c network.parallel_seam; default 1 = the serial fold, byte-identical to
     *        the historical loop).  With @p n_threads > 1 and OpenMP, each block's
     *        algebra (@f$G@f$, @f$M@f$, @f$z_{\text{free}}@f$) is computed into a
     *        per-block slot in parallel; the @f$G M G^\top@f$ /
     *        @f$G M(z_{\text{free}}-\hat z)@f$ Woodbury contributions are then formed
     *        and accumulated serially in block-index order with the same expressions
     *        as the serial fold.  The floating-point operations and their order are
     *        unchanged, so the reduced system is bit-identical to the default serial
     *        fold for any thread count (unlike the opt-in fan-out of @ref reduce,
     *        which reassociates at ULP level).  The dense kernels must run
     *        single-threaded (the driver pins Eigen to one thread for the epoch) to
     *        avoid nested oversubscription.
     * @param station_threads Optional within-block fan-out (@c network.parallel_carry;
     *        default 1 = serial).  Takes effect only on the serial fold path
     *        (@p n_threads == 1); the driver runs the carry serially over blocks and
     *        asks for within-block parallelism instead.  With @p station_threads > 1,
     *        the per-station reduction (@f$\Sigma_r C_{L,r}^\top N_{rr}^{-1} C_{L,r}@f$
     *        and the like) is fanned across threads through per-thread partials
     *        combined in ascending thread order: reproducible for a given thread count,
     *        but different from the serial fold at ULP level (the parallel_carry
     *        relaxation, as in @ref reduce).  The explicit K×K middle inverse is
     *        solved column-parallel; each RHS column is independent, so that step is
     *        byte-identical for a given @f$P^z+B@f$ (the LDLT factor stays serial).
     *        The driver keeps Eigen pinned to one thread around the fold, so these
     *        OpenMP regions do not nest with Eigen's GEMM; the kernel itself raises
     *        Eigen's thread count to @p station_threads for the @f$G M G^\top@f$
     *        product only, outside any OpenMP region, and restores it on exit.
     * @throws std::out_of_range / std::invalid_argument on a bad station, column,
     *         or a @c Pz / @c zhat dimension mismatch.
     */
    void foldReducedDdPriors(const std::vector<DdPriorBlock>& blocks, int n_threads = 1,
                             int station_threads = 1);

    /**
     * @brief Posterior mean of a folded block's K double differences given the
     *        solved shared estimate: the value the z-store carries forward.
     *
     * @f$\hat z_{\text{post}}=\hat z+P^z M\,(z_{\text{free}}-G^\top x_s-\hat z)@f$
     * (equivalently @f$C_L\hat x_L@f$ of the constrained locals), the prior
     * @f$\hat z@f$ shrunk toward the data-implied DD @f$C_L x^0@f$.
     *
     * @param block_index Input block index.  @ref foldReducedDdPriors preserves the
     *        input order (a degenerate or chordless block is an inert slot), so the
     *        index stays aligned with the caller's z-store block list.
     * @param x_s The solved shared estimate (length @c n_shared).
     * @return The K posterior DD means (empty for a chordless @f$K=0@f$ slot; the prior
     *         @c zhat unchanged for an inert degenerate block, since @f$M=0@f$).
     * @throws std::out_of_range on a bad index; std::invalid_argument on an x_s size mismatch.
     */
    Eigen::VectorXd ddPriorPosteriorMean(std::size_t block_index,
                                         const Eigen::VectorXd& x_s) const;

    /**
     * @brief Posterior covariance of a folded block's K double differences: the
     *        @f$P^z@f$ the z-store carries forward.
     *
     * The block's DD @f$z=C_L x_L@f$ is a gauge-free function of the local
     * ambiguities, and its full posterior covariance (prior plus all network data,
     * including the shared-parameter coupling) has the closed form
     * @f[
     *   P^z_{\text{post}}
     *     = \underbrace{(P^z - P^z M P^z)}_{=\,(P^{z-1}+B^{-1})^{-1}=C_L N_{LL}'^{-1}C_L^\top}
     *     + (P^z M G^\top)\,\Sigma_{ss}\,(G M P^z),
     * @f]
     * built from the cached fold factors @f$(P^z,M,G)@f$ (see
     * @ref foldReducedDdPriors) and the datum-as-known shared covariance
     * @f$\Sigma_{ss}@f$.  Derivation (@f$N_{LL}'=N_{LL}+C_L^\top P^{z-1}C_L@f$,
     * @f$\Sigma_{LL}=N_{LL}'^{-1}+N_{LL}'^{-1}N_{Ls}\Sigma_{ss}N_{sL}N_{LL}'^{-1}@f$,
     * Woodbury): the first term is @f$C_L N_{LL}'^{-1} C_L^\top = B-BMB = P^z-P^zMP^z@f$
     * (the posterior conditional on @f$x_s@f$), and the coupling term uses
     * @f$C_L N_{LL}'^{-1} N_{Ls} = (I-BM)G^\top = P^z M G^\top@f$ (since
     * @f$P^zM+BM=(P^z+B)M=I@f$).  The DD is estimable, so the result is
     * datum-invariant although @f$\Sigma_{ss}@f$ is datum-constrained.  With
     * @ref setDdFoldPdSafe armed, the first term is evaluated in the product form
     * @f$\mathrm{sym}(P^zMB)@f$, which is equal in exact arithmetic.
     *
     * @param block_index Input block index (input order is preserved; see
     *        @ref ddPriorPosteriorMean).
     * @param cov_free_xs The shared covariance with the datum-pinned columns treated
     *        as exact (variance 0), the same @c cov_free the
     *        @ref reconstructStationCovariance consumers build (n_shared × n_shared).
     * @return The K×K posterior DD covariance (the prior @c Pz unchanged for an inert
     *         degenerate block since @f$M=0@f$; empty for a chordless @f$K=0@f$ slot).
     * @throws std::out_of_range on a bad index; std::invalid_argument on a
     *         @p cov_free_xs size mismatch.
     */
    Eigen::MatrixXd ddPriorPosteriorCovariance(std::size_t block_index,
                                               const Eigen::MatrixXd& cov_free_xs) const;

    /**
     * @brief Innovation (measurement minus prediction) of a folded block's K double
     *        differences and its exact H0 covariance: the estimation-side fine-slip
     *        test that the innovation gate standardizes.
     *
     * The DD carry's coarse slip layer (MW, LLI, IF-TD) is deliberately relaxed; the
     * finer slips the estimator has to catch appear as an inflated innovation on the
     * carried DD.  Per block, from the cached fold factors (@ref FoldedDdPrior) and
     * the datum-as-known shared covariance @f$\Sigma_{ss}@f$ (@p cov_free_xs, the
     * frame @ref ddPriorPosteriorCovariance also uses):
     * @f[
     *   v = z_{\text{free}} - G^\top x_s - \hat z, \qquad
     *   \mathrm{Cov}(v) = (P^z + B) - G^\top \Sigma_{ss} G = M^{-1} - G^\top\Sigma_{ss}G.
     * @f]
     * The border term enters with a minus sign (@f$-G^\top\Sigma_{ss}G@f$) because
     * @f$x_s@f$ is itself influenced by the prior; the same border coupling makes
     * @ref ddPriorPosteriorCovariance datum-invariant.  Consistency:
     * @f$P^zM\,\mathrm{Cov}(v)\,MP^z = P^z - P^{z+}@f$, i.e.
     * @f$\mathrm{Cov}(\hat z^+-\hat z)@f$ agrees with @ref ddPriorPosteriorMean's
     * @f$\hat z^+ - \hat z = P^zMv@f$.  @f$v@f$ is the same vector that
     * @ref ddPriorPosteriorMean multiplies by @f$P^zM@f$.
     *
     * @param block_index Input block index (input order preserved; see @ref ddPriorPosteriorMean).
     * @param x_s         The solved shared estimate (length @c n_shared).
     * @param cov_free_xs The datum-as-known shared covariance (n_shared × n_shared), the
     *        same @c cov_free @ref ddPriorPosteriorCovariance takes.
     * @param[out] v      The K innovations (empty for a chordless K=0 slot).
     * @param[out] cov_v  The K×K innovation covariance under H0 (empty for a K=0 slot).
     *        For an inert degenerate block (M=0) it is the raw, non-PD
     *        Pᶻ+B − GᵀΣG, so the caller must skip a chord whose @c cov_v diagonal is
     *        not strictly positive.
     * @throws std::out_of_range on a bad index; std::invalid_argument on an x_s /
     *         cov_free_xs size mismatch.
     */
    void ddPriorInnovation(std::size_t block_index, const Eigen::VectorXd& x_s,
                           const Eigen::MatrixXd& cov_free_xs,
                           Eigen::VectorXd& v, Eigen::MatrixXd& cov_v) const;

    /// @return Number of DD-prior blocks (== the input count of the last
    ///         @ref foldReducedDdPriors; degenerate/chordless blocks are inert slots).
    std::size_t numDdPriorBlocks() const { return dd_priors_.size(); }

    // --- Detached fold of fixed integer cycles ----------------------
    //
    // The second DD-fold path of this class, beside the Card-4 dense fold above
    // (@ref foldReducedDdPriors, which stays unchanged and is this path's unit
    // oracle).  The two never share an epoch: the fixed-cycle fold's loader refuses
    // carry_invariant.  The algebra is the same Woodbury fold; what differs is that
    // nothing is written into this object.  The result is handed back as operands
    // (@ref DetachedFold) that a caller adds to a copy of the reduced normals, so
    // the reduced system, the per-station factors and every reconstruction stay
    // those of the float solve.  Implemented in NormalEquationsDetached.cpp.

    /**
     * @brief One fixed integer cycle as a soft constraint on local ambiguity
     *        columns, in the increment frame.
     *
     * The constraint reads
     * @f$\sum_t c_t\,x_{r_t}[\mathrm{col}_t] \sim N(\hat z, P^z)@f$: the signed sum
     * of the terms' local increments (metres) is held at @c zhat_m with variance
     * @c pz_m2.  For a fixed cycle with integer @f$N@f$ on a band of wavelength
     * @f$\lambda@f$, @f$\hat z=\lambda N-\sum_t c_t\,x^{\mathrm{lin}}_t@f$: the fixed
     * value re-based on this epoch's linearization points.
     */
    struct CycleRow {
        std::vector<DdPriorBlock::Term> terms;  ///< (station, local column, signed coefficient) terms; the DdPriorBlock term, reused.  Repeated (station, column) terms add.
        double zhat_m{0.0};                     ///< Target @f$\lambda N-\sum c\,x^{\mathrm{lin}}@f$ (m), re-based every epoch.
        double pz_m2{0.0};                      ///< Hold variance (m², finite, ≥ 0).  The fixed-cycle seam passes @f$(\sigma\lambda)^2>0@f$; 0 is a hard constraint.  At any hold, rows that are linearly dependent make their block degenerate (the independence screen on @f$B@f$ alone, @ref DetachedFoldBlock::dependent).
    };

    /// The sparse factor of one block's equilibrated @f$D(P^z+B)D@f$ (an AMD-ordered
    /// sparse LDLᵀ).  Opaque here, defined in NormalEquationsDetached.cpp, so this
    /// header does not pull Eigen's sparse modules into every translation unit that
    /// includes it.
    struct DetachedFoldFactor;

    /**
     * @brief The fold operands of one block of cycle rows.
     *
     * A block's stations are touched by no other block, so the per-block Woodbury
     * corrections add exactly (no block-Jacobi truncation).  With @f$K@f$ rows and
     * @f$C_r@f$ the rows' signed sub-matrix on station @f$r@f$'s columns:
     * @f$G=\sum_r N_{sr}N_{rr}^{-1}C_r^\top@f$,
     * @f$z_{\mathrm{free}}=\sum_r C_rN_{rr}^{-1}u_r@f$,
     * @f$B=\sum_r C_rN_{rr}^{-1}C_r^\top@f$ and @f$A=P^z+B@f$ (sparse: rows that share
     * no station do not couple).  @f$A@f$ is factored as @f$D A D@f$ with
     * @f$D=\mathrm{diag}(A)^{-1/2}@f$.
     *
     * @par Two screens
     * @f$B@f$ is singular exactly when the rows are linearly dependent (the
     * per-station @f$N_{rr}^{-1}@f$ are positive definite), whatever the hold; a
     * positive @f$P^z@f$ hides that from any screen on @f$A@f$.  So @f$B@f$ alone is
     * equilibrated and factored first, and a block whose rows fail that screen is
     * @c dependent: a repeated cycle would double its hold, and a contradictory
     * dependent set (@f$c_3=c_1+c_2@f$ held at @f$N_3\ne N_1+N_2@f$) would be averaged
     * into the held solve.  Then @f$DAD@f$ is factored and screened the same way.
     */
    struct DetachedFoldBlock {
        Eigen::MatrixXd G;        ///< @f$\sum_r N_{sr}N_{rr}^{-1}C_r^\top@f$ (n_shared × K).
        Eigen::VectorXd z_free;   ///< @f$\sum_r C_rN_{rr}^{-1}u_r@f$ (K): the data-implied cycle values given @f$x_s=0@f$.
        Eigen::VectorXd zhat;     ///< The rows' targets (K, m).
        Eigen::VectorXd scale;    ///< The Jacobi scale @f$D=\mathrm{diag}(A)^{-1/2}@f$ of the factored @f$DAD@f$ (K).
        std::shared_ptr<const DetachedFoldFactor> factor;  ///< Factor of @f$DAD@f$; null when the block is degenerate or has no rows.
        bool      dependent{false};   ///< The rows are linearly dependent: the equilibrated @f$B@f$ alone failed its factorization or the relative-pivot screen.  Implies @c degenerate.
        bool      degenerate{false};  ///< The block folds nothing: its rows are @c dependent, or the factorization of @f$DAD@f$ failed or its relative-pivot screen refused it.
        double    b_min_pivot_rel{0.0}; ///< Smallest pivot of the equilibrated @f$B@f$ relative to the largest: the rows' independence margin (0 when nothing was factored).
        double    min_pivot_rel{0.0}; ///< Smallest pivot of the factored @f$DAD@f$ relative to the largest (0 when it was not factored).
        long long factor_nnz{0};      ///< Nonzeros of the stored sparse factor (cost telemetry).
        Eigen::MatrixXd X;            ///< @f$A^{-1}G^\top@f$ (K × n_shared); empty unless the block folds.  With the held covariance @f$\Sigma_h@f$ it gives @f$S^{-1}=A^{-1}-X\Sigma_hX^\top@f$ (@ref NetworkEstimator::HeldSolve::chi2_expect).
        Eigen::VectorXd ainv_diag;    ///< @f$\mathrm{diag}(A^{-1})@f$ (K), by a selected inversion of the factor on its own sparsity pattern; empty unless the block folds.
        Eigen::MatrixXd dN;           ///< This block's increment @f$G A^{-1}G^\top=GX@f$ (n_shared × n_shared, symmetric); empty unless the block folds.  Kept so that a caller can fold the block alone (its own test statistic, @ref NetworkEstimator::HeldSolve::chi2).
        Eigen::VectorXd du;           ///< This block's increment @f$G A^{-1}(z_{\mathrm{free}}-\hat z)@f$ (n_shared); empty unless the block folds.
    };

    /// Every block's operands and the shared-system increments of their fold.
    struct DetachedFold {
        std::vector<DetachedFoldBlock> blocks;  ///< One per input block, input order.
        Eigen::MatrixXd dN;                     ///< @f$\sum_b G_bA_b^{-1}G_b^\top@f$ over the blocks that fold (n_shared × n_shared, symmetric); the sum of the blocks' @c dN in input order.
        Eigen::VectorXd du;                     ///< @f$\sum_b G_bA_b^{-1}(z_{\mathrm{free},b}-\hat z_b)@f$ over the blocks that fold (n_shared).
    };

    /**
     * @brief The @ref foldReducedDdPriors algebra for blocks of fixed cycles, from the
     *        cached per-station factors, without modifying this object.
     *
     * Adding @c dN and @c du to a copy of @ref reducedSharedNormals and
     * @ref reducedSharedRhs gives the reduced system of the float data plus every
     * non-degenerate block's rows as the soft constraints of @ref CycleRow (the
     * Woodbury identity of @ref foldReducedDdPriors with @f$M=A^{-1}@f$); the caller
     * then applies its datum and solves (@ref NetworkEstimator::solveHeldDetached).
     *
     * Per block, each station assembles only the @f$K_r@f$ rows that touch it: the
     * compact @f$C_r^\top@f$ (@f$n_r\times K_r@f$) is solved once against the cached
     * factor, @f$Y=N_{rr}^{-1}C_r^\top@f$; @f$C_rY@f$ enters @f$B@f$ as triplets,
     * @f$N_{sr}Y@f$ the matching columns of @f$G@f$, and @f$C_rN_{rr}^{-1}u_r@f$ the
     * matching entries of @f$z_{\mathrm{free}}@f$.  @f$B@f$ and @f$A=P^z+B@f$ are sparse
     * matrices, each Jacobi-equilibrated and factored by an AMD-ordered sparse LDLᵀ.
     * The block is degenerate (it folds nothing) when either factorization fails or
     * a pivot is non-positive or below @p pivot_floor_rel times the largest: on
     * @f$B@f$ the screen finds linearly dependent rows (@ref DetachedFoldBlock), on
     * @f$DAD@f$ an ill-conditioned hold.  A block that folds keeps @f$X=A^{-1}G^\top@f$
     * and @f$\mathrm{diag}(A^{-1})@f$ for the held solve's statistics.  The arithmetic
     * is deterministic for a fixed input order and equals the dense fold to
     * tolerance, not bitwise.
     *
     * @param blocks          The blocks of cycle rows; an empty block is an inert slot
     *                        (no factor, not degenerate).
     * @param pivot_floor_rel Relative-pivot floor of both screens (default 1e-12, the
     *                        value of the dense fold's PD-safe screen).
     * @return The operands of every block and the summed increments; @c dN and @c du
     *         are zero when no block folds.
     * @throws std::invalid_argument when two blocks touch one station, a row has no
     *         terms, a coefficient, target or hold variance is not finite, a hold
     *         variance is negative, or a local column is out of range;
     *         std::out_of_range on a station index that is not active this epoch.
     *         Every input is checked before anything is factored.  std::logic_error
     *         if a sparse factor's pattern is not closed under elimination (the selected
     *         inversion's precondition, which the factorization guarantees).
     */
    DetachedFold foldCycleRowsDetached(const std::vector<std::vector<CycleRow>>& blocks,
                                       double pivot_floor_rel = 1e-12) const;

    /**
     * @brief @f$A_b^{-1}r@f$ through a block's equilibrated factor:
     *        @f$D\,(DAD)^{-1}D\,r@f$.
     * @param b A block from @ref foldCycleRowsDetached with a factor.
     * @param r A vector of the block's row count.
     * @return @f$A_b^{-1}r@f$.
     * @throws std::logic_error when the block has no factor (degenerate or empty);
     *         std::invalid_argument on a size mismatch.
     */
    static Eigen::VectorXd solveFoldBlock(const DetachedFoldBlock& b, const Eigen::VectorXd& r);

    // --- PD-safe invariant-DD fold ----------------

    /**
     * @brief Diagnostics of the positive-definiteness screen in the last
     *        @ref foldReducedDdPriors call (INC-0).
     *
     * A block that folds inertly (@f$M:=0@f$) takes no data, so its carried
     * sub-state stays at the prior, and the downstream readings (`dd_sigma_cyc`, the
     * D5 gate, the AR probe) do not show it.  These counters make that branch
     * visible (standing invariant #4; docs/SPEC_pd_safe_dd_fold.md).  They are
     * solve-neutral (reads of the factor already computed) and therefore always on.
     *
     * Per fold, not cumulative: a within-epoch RAIM re-solve re-folds and overwrites
     * them, so the values the install seam reads describe the accepted solve.
     */
    struct DdFoldStats {
        long long degen_blocks{0};   ///< blocks whose @f$P^z+B@f$ failed the PD screen ⇒ folded inertly (M=0).
        long long degen_rows{0};     ///< Σ K over those blocks: the carried dimension that took no data this fold.
        long long repairs{0};        ///< Eigen-clamp repairs applied (INC-1c; 0 unless @ref setDdFoldPdSafe).
        long long repaired_rows{0};  ///< Σ K over repaired blocks.
        long long blocks{0};         ///< blocks that reached the factorization (K > 0): the denominator.
        long long rows{0};           ///< Σ K over those blocks.
        /// Worst relative pivot @f$d_{\min}/d_{\max}@f$ over the fold (1.0 when nothing
        /// was factored).  On the PD-safe path it is the pivot of the equilibrated
        /// matrix, so it measures conditioning rather than the spread of row scales;
        /// ≤ 0 means indefinite.
        double min_pivot_rel{1.0};
        /// Largest clamped eigenvalue magnitude relative to @f$\lambda_{\max}@f$ over the
        /// fold, whether or not the repair was accepted.  Clamps up to 1e-6·λmax
        /// (round-off) are repaired.  A larger clamp is refused, because the repair
        /// would turn a genuinely indefinite block into one that factors normally;
        /// the block folds inertly and is left to the install-side rebirth (INC-2a).
        double repair_worst{0.0};
    };
    /// @return The last fold's PD-screen diagnostics (@ref DdFoldStats).
    const DdFoldStats& ddFoldStats() const { return dd_fold_stats_; }
    /// @return Whether DD-prior block @p b folded inertly (its @f$P^z+B@f$ failed the PD
    ///         screen, so @f$M=0@f$ and the posterior equals the prior).  False for a
    ///         chordless K=0 slot.  The install-side rebirth keys on this.
    /// @throws std::out_of_range on a bad index.
    bool ddPriorBlockDegenerate(std::size_t b) const;
    /// @return DD-prior block @p b's relative pivot @f$d_{\min}/d_{\max}@f$ at the fold
    ///         (0 for a chordless slot or a factorization that never completed).
    /// @throws std::out_of_range on a bad index.
    double ddPriorBlockMinPivotRel(std::size_t b) const;

    /**
     * @brief Arm the PD-safe DD-prior fold arithmetic (@c network.carry_invariant_pd_safe).
     *
     * Off (the default), the fold keeps its original arithmetic and output is
     * byte-identical (standing invariant #6): a plain LDLᵀ of @f$P^z+B@f$, the
     * absolute @c minCoeff()<=0 screen, and the subtractive posterior
     * @f$P^z-P^zMP^z@f$.  On, all three are replaced by algebraically identical forms
     * that round differently (docs/SPEC_pd_safe_dd_fold.md §4):
     *  - (a) the product-form posterior
     *        @f$\mathrm{sym}(P^zMB)+(P^zMG^\top)\Sigma(P^zMG^\top)^\top@f$, whose operands
     *        follow the scale of the result instead of subtracting two operands of
     *        order 1e6 to reach a result of order 1e-4 (@f$B@f$ is cached per block
     *        for this, K² doubles);
     *  - (b) an equilibrated factorization @f$\tilde A=S(P^z+B)S@f$ with a
     *        scale-relative pivot test (@f$d_{\min}\ge@f$ 1e-12 @f$d_{\max}@f$) and an
     *        exact unscale @f$M=S\tilde A^{-1}S@f$;
     *  - (c) a counted eigenvalue-clamp repair when (b) still fails (refused above
     *        the bar given at @ref DdFoldStats::repair_worst).
     *
     * Set once before the epoch loop; it survives @ref reset, like @ref setPerfTiming.
     */
    void setDdFoldPdSafe(bool on) { dd_fold_pd_safe_ = on; }
    /// @return Whether the PD-safe fold arithmetic is armed (@ref setDdFoldPdSafe).
    bool ddFoldPdSafe() const { return dd_fold_pd_safe_; }

    /**
     * @brief Telemetry for the carry-aware covariance readback
     *        (docs/SPEC_carry_aware_covariance.md §5b/§5c).
     *
     * Cumulative over the run: @ref reset does not clear it (like the section
     * timers), because the readback is a per-consumer opt-in and a per-epoch view
     * would reflect only whichever tracer ran.  @ref resetCarryCovStats clears it
     * (for tests).
     *
     * The key counter is @c nonpos_rows, and it measures a property of the fold
     * rather than of this readback: the fold composes one Woodbury update per block
     * against the unmodified @f$N_{rr}^{-1}@f$, which is exact only when no station
     * is touched by two blocks.  A block is one connected component of one
     * (const,signal) group, and two groups often share a station, so the summed
     * rank-K downdate can over-subtract.  No other check in the producer screens
     * for this.
     */
    struct CarryCovStats {
        long long calls{0};          ///< carry-aware readback calls that reached the correction.
        long long rows{0};           ///< requested (station,column) entries the correction covered.
        long long corrected_rows{0}; ///< …of those, entries whose corrected variance was accepted.
        long long nonpos_rows{0};    ///< …entries whose corrected variance came out ≤ 0 or non-finite and was refused (never clamped).
        long long refused_calls{0};  ///< matrix-valued calls refused as a whole (one bad diagonal entry makes the whole matrix fall back, so the returned covariance stays a single consistent model).
        /// Smallest accepted corrected/blind variance ratio: the largest information
        /// gain that the blind readback leaves out.  Starts at 1.0 and moves only when
        /// an accepted correction reduces a variance, so 1.0 at run end means no
        /// correction reduced any variance (a dead-wiring read, invariant #4).
        double ratio_min{1.0};
        /// Largest accepted ratio.  A value above 1 is legal: the local term only
        /// tightens, but the regression term @f$\tilde A^{-1}N_{rs}\Sigma_{ss}(\cdot)^\top@f$
        /// is not sign-definite, so a variance can grow.  A value far above 1 is a
        /// finding about the run, not a defect in this readback.
        double ratio_max{0.0};
        double ratio_sum{0.0};       ///< Σ of the ratio over accepted entries…
        long long ratio_n{0};        ///< …and its count, so the run-end line can print the mean.
    };
    /**
     * @brief The carry-aware covariance correction: what the @c carry_aware flag on
     *        @ref reconstructStationCovariance, @ref reconstructStationCovarianceDiag,
     *        @ref stationRegression and @ref jointConditionalCovariance turns on.
     *
     * @ref foldReducedDdPriors applies a per-block rank-K Woodbury update to the
     * unmodified per-station factors, and the reduced system it produces is exactly
     * the Schur complement taken with
     * @f[
     *   \tilde A^{-1} \;=\; N_{rr}^{-1} \;-\; \sum_b Y_b M_b Y_b^\top ,
     *   \qquad Y_b = N_{rr}^{-1}C_b^\top,\; M_b=(P^z_b+B_b)^{-1},
     * @f]
     * because @f$N_{sr}\tilde A^{-1}N_{rs}=N_{sr}N_{rr}^{-1}N_{rs}-\sum_b G_bM_bG_b^\top@f$
     * and the last sum is what the fold adds to the reduced shared normals.
     * @ref reconstructStation applies the same model to the mean (the @f$\kappa@f$
     * correction).  With @c carry_aware false, the four covariance readbacks use
     * @f$N_{rr}^{-1}@f$ and @f$N_{rr}^{-1}N_{rs}@f$, while the @c NetworkEstimator
     * shared covariance, inverted from the folded reduced normals, includes the
     * carry.  The result mixes two models (the carry in the shared block, but not in
     * the local block or in the regression coupling the two), so it is not the
     * covariance of any single estimator.
     *
     * Armed, the readbacks use @f$\tilde A^{-1}@f$ and @f$\tilde A^{-1}N_{rs}@f$
     * instead, the model the solve used.  Disarmed (the default), every expression
     * is the original one, unchanged.
     *
     * @return Whether the last fold left any DD-prior block slot, so that a
     *         @c carry_aware call can differ from a blind one.  False before
     *         @ref foldReducedDdPriors (or after a @ref reduce recompute or
     *         @ref reset), when the fold received no blocks, or with the carry off.
     *         Chordless K=0 slots count as slots but contribute no correction.
     */
    bool hasCarryCovCorrection() const { return !dd_priors_.empty(); }
    /// @return Run-cumulative carry-aware readback telemetry (@ref CarryCovStats).
    const CarryCovStats& carryCovStats() const { return carry_cov_stats_; }
    /// Clear @ref CarryCovStats (for tests or a deliberate per-leg re-zero; @ref reset does not).
    void resetCarryCovStats() const { carry_cov_stats_ = CarryCovStats{}; }

    /**
     * @brief Schur-eliminate every station's nuisance block onto the shared
     *        border, producing @f$\tilde N_{ss},\ \tilde u_s@f$.
     *
     * The result is cached: the call is a no-op while the reduction is current (the
     * internal @c reduced_ flag), so two calls in a row give identical results.
     * @ref addStation, and @ref addStationPrior with a positive weight, mark the
     * cache stale.  @ref addSharedInfo instead carries its shared-border delta
     * through the reduction in place and keeps it current, so a post-reduce prior
     * (for example the driver's D2 carry-forward, applied between the rank gate and
     * the solve) does not force a second full per-station Schur pass.  The
     * per-station LDLᵀ factors are reused; each is computed once, in @ref addStation.
     *
     * @param n_threads Optional fan-out of the per-station Schur loop (default 1 =
     *        serial, bit-identical to the historical loop).  With @p n_threads > 1
     *        and OpenMP, stations are eliminated in parallel into per-thread partial
     *        accumulators combined in a fixed order (ascending station range,
     *        ascending thread): the result is reproducible for a given thread count
     *        but differs from the serial loop at ULP level (a different summation
     *        grouping).  Callers that need byte-identical output across thread
     *        counts pass 1.  The dense kernels inside the loop must run
     *        single-threaded (the driver pins Eigen to one thread for the epoch) to
     *        avoid nested oversubscription.
     */
    void reduce(int n_threads = 1);

    /**
     * @brief Solve the reduced shared system @f$\tilde N_{ss}\,x_s=\tilde u_s@f$.
     *
     * Calls @ref reduce first when the reduction is stale.  The reduced block must
     * be full rank: the caller applies the S-basis minimum constraint (W5) or
     * sufficient priors through @ref addSharedInfo beforehand, since an
     * unconstrained satellite-clock block is rank-deficient by one datum per
     * constellation.  A reduced block that fails the positive-definiteness check
     * throws std::invalid_argument.
     *
     * @return The shared satellite-parameter estimate @c x_s (length @c n_shared).
     */
    Eigen::VectorXd solveShared();

    /**
     * @brief Solve an externally constrained reduced shared system.
     *
     * The W5 seam: after @ref reduce, a `NetworkSbasis` reads
     * @ref reducedSharedNormals / @ref reducedSharedRhs, applies the explicit
     * minimum-constraint datum to a copy (hard pin or guarded S-transform), and
     * passes the result here.  The kernel's own reduced system is not modified (so
     * @ref reconstructStation and a re-solve stay valid), and the datum lives
     * entirely in W5.  The returned @c x_s is consistent with
     * @ref reconstructStation, which reconstructs the locals against whatever
     * shared solution it is given.
     *
     * @param constrained_N_ss The datum-constrained reduced normals
     *                         (@c n_shared × @c n_shared, must be full rank).
     * @param constrained_u_s  The matching reduced RHS (length @c n_shared).
     * @param cov_out If non-null, receives the formal covariance of @c x_s: the
     *               inverse @f$(\tilde N_{ss}^{\text{constrained}})^{-1}@f$, computed
     *               from the same LDLᵀ factor as the point solve, so the caller
     *               does not factorize a second time.  The datum-pinned columns
     *               carry the implementation's @c 1/pin_scale variance; treat them
     *               as exact downstream.  Empty when @c n_shared is 0.
     * @return The shared estimate @c x_s.
     * @throws std::invalid_argument on a dimension mismatch or a system that is
     *         still singular (the applied constraint did not remove the deficiency).
     */
    Eigen::VectorXd solveSharedConstrained(const Eigen::MatrixXd& constrained_N_ss,
                                           const Eigen::VectorXd& constrained_u_s,
                                           Eigen::MatrixXd* cov_out = nullptr) const;

    /**
     * @brief Back-substitute a station's nuisance estimate given the shared
     *        solution: @f$x_r=(N_{rr}^r)^{-1}(u_r^r - N_{rs}^r x_s)@f$.
     *
     * When sum-to-zero gauges or DD priors are folded, the matching corrections
     * of @ref foldReducedSumZeroGauges and @ref foldReducedDdPriors are applied,
     * so the result is the constrained local solution.
     * @param station Index returned by @ref addStation / @ref addStationObservations.
     * @param x_s     The shared estimate from @ref solveShared or
     *                @ref solveSharedConstrained (length @c n_shared).
     * @return The station's nuisance estimate (length @c n_local; empty if 0).
     * @throws std::out_of_range / std::invalid_argument on a bad index or size.
     */
    Eigen::VectorXd reconstructStation(std::size_t station,
                                       const Eigen::VectorXd& x_s) const;

    /**
     * @brief The terms of @ref reconstructStation, held apart (the station-local
     *        attribution of filter/NetworkRhsAttrib reads them).
     *
     * @f$x_r = N_{rr}^{-1}u_r - N_{rr}^{-1}N_{rs}x_s + c_{\text{sumzero}} + c_{\text{dd}}@f$,
     * each term computed with the cached factor and folds the solve used.
     */
    struct StationReconstructionParts {
        Eigen::VectorXd free;      ///< @f$N_{rr}^{-1}u_r@f$: the local solution with every shared column at zero.
        Eigen::VectorXd feedback;  ///< @f$-N_{rr}^{-1}N_{rs}x_s@f$: the shared solution's feedback.
        Eigen::VectorXd sumzero;   ///< The sum-to-zero gauge correction (zero when no folded gauge has a term at the station).
        Eigen::VectorXd ddprior;   ///< The DD-prior correction (zero when no folded DD prior has a term at the station).
        Eigen::VectorXd x_r;       ///< @ref reconstructStation itself, computed as the solve computes it.
    };

    /**
     * @brief @ref reconstructStation with its terms held apart (diagnostics only).
     *
     * The two constraint corrections are the ones @ref reconstructStation applies (one
     * implementation serves both), each applied to a zero vector.  The terms sum to
     * @ref StationReconstructionParts::x_r up to rounding: @c x_r solves
     * @f$u_r - N_{rs}x_s@f$ in one pass, the parts in two.  Read-only on the cached
     * factor and folds, so a call changes nothing the solve reads.
     *
     * @param station Index returned by @ref addStation / @ref addStationObservations.
     * @param x_s     The shared estimate (length @c n_shared).
     * @return The parts, each of length @c n_local (empty when the station has no locals).
     * @throws std::out_of_range / std::invalid_argument on a bad index or size.
     */
    StationReconstructionParts reconstructStationParts(std::size_t station,
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
     * Pass the full shared covariance to get the marginal local covariance; pass it
     * with the rows and columns of every exact (datum-pinned) shared parameter
     * zeroed to get @f$\mathrm{Cov}(x_r\mid x_s^{\text{free}})@f$, the frame in which
     * between-station double-difference ambiguities are integer-estimable
     * (producer-side network AR, Phase 1).  Treating the gauge columns as known
     * (variance 0), instead of carrying their artificial @c 1/pin_scale variance,
     * keeps that artifact of the datum implementation out of the DD ambiguity
     * covariance.
     *
     * @param station      Index from @ref addStation.
     * @param cov_free_xs  Shared-parameter covariance (@c n_shared × @c n_shared),
     *                     datum-pinned rows/cols zeroed for the conditional form.
     * @param carry_aware  Include the folded DD priors' rank-K term (@ref hasCarryCovCorrection).
     *                     @c false (default) keeps the original expression unchanged.
     *                     If any corrected diagonal entry is ≤ 0 or non-finite, the
     *                     whole matrix falls back to the uncorrected one.
     * @return The station's @c n_local × @c n_local covariance (symmetric; empty
     *         if the station has no locals).
     * @throws std::out_of_range / std::invalid_argument on a bad index or size.
     */
    Eigen::MatrixXd reconstructStationCovariance(
        std::size_t station, const Eigen::MatrixXd& cov_free_xs,
        bool carry_aware = false) const;

    /**
     * @brief Diagonal of @ref reconstructStationCovariance for a subset of local
     *        columns: the cheap per-ambiguity conditional-variance harvest.
     *
     * Returns, for each @p local_cols[i], the @f$(c,c)@f$ entry of
     * @f$\mathrm{Cov}(x_r)=N_{rr}^{-1}+M\,\mathrm{Cov}_\text{free}\,M^\top@f$ with
     * @f$M=N_{rr}^{-1}N_{rs}@f$, computed without forming the full
     * @f$N_{rr}^{-1}@f$.  Each column costs one local solve
     * @f$n_c=N_{rr}^{-1}e_c@f$ (giving @f$(N_{rr}^{-1})_{cc}=n_c(c)@f$) plus the
     * shared propagation @f$ (N_{sr}n_c)^\top\,\mathrm{Cov}_\text{free}\,(N_{sr}n_c)@f$,
     * i.e. @f$O(k\,(n_r^2+n_s^2))@f$ for @c k requested columns, instead of the
     * @f$O(n_r^3)@f$ full inverse that @ref reconstructStationCovariance pays (a
     * known serial-path cost at scale).  Pass the same @p cov_free_xs
     * (datum-pinned shared rows/cols zeroed) as for the full form.
     *
     * @param station     Index from @ref addStation.
     * @param local_cols  Local columns whose conditional variance is wanted.
     * @param cov_free_xs Shared-parameter covariance (datum-pinned rows/cols zeroed).
     * @param carry_aware Include the folded DD priors' rank-K term (@ref hasCarryCovCorrection).
     *                    @c false (default) keeps the original expression unchanged.
     *                    The fallback here is per column (each variance stands
     *                    alone): a corrected variance that is ≤ 0 or non-finite is
     *                    replaced by the uncorrected one, whereas the matrix-valued
     *                    forms fall back as a whole.
     * @return A vector of conditional variances, one per requested column (empty if
     *         the station has no locals or @p local_cols is empty).
     * @throws std::out_of_range / std::invalid_argument on a bad index or column.
     */
    Eigen::VectorXd reconstructStationCovarianceDiag(
        std::size_t station, const std::vector<Eigen::Index>& local_cols,
        const Eigen::MatrixXd& cov_free_xs, bool carry_aware = false) const;

    /// The regression of one local column on the shared border, and that column's
    /// pure conditional variance: the two ingredients of an exact between-epoch
    /// conditional carry (@c network.carry_conditional_coupling; Card 1).
    struct StationRegression {
        Eigen::VectorXd row;   ///< @f$M_r^\top = N_{sr}N_{rr}^{-1}e_r@f$ (length @c n_shared): row @c r of @f$N_{rr}^{-1}N_{rs}@f$, i.e. the coefficients of the local's linear regression on the shared border.  Empty when @c n_shared is 0.
        double cond_var{0.0};  ///< @f$(N_{rr}^{-1})_{rr}=\mathrm{Var}(x_r\mid x_s)@f$: the local's variance conditional on the whole shared border (not the marginal that @ref reconstructStationCovarianceDiag returns).
        Eigen::VectorXd cross; ///< For network.phase_leveling_local_block (F3): @f$(N_{rr}^{-1})_{r c_j}@f$ for each requested @c cross_cols[j], the same solve @f$n_c@f$ read at the station's other carried columns (a gather, not a second solve).  Empty unless @c cross_cols was passed; NaN for an out-of-range column.  Filled on the plain-normals path only (see @ref stationRegression).
    };

    /**
     * @brief Regression row @f$M_r=(N_{rr}^{-1}e_r)^\top N_{rs}@f$ and the pure
     *        conditional variance @f$(N_{rr}^{-1})_{rr}@f$ for one local column.
     *
     * The exact recursion carries a persistent local parameter (a phase-leveling
     * ambiguity or ZWD) forward as the conditional distribution @f$r\mid x_s@f$
     * rather than as an independent marginal.  That conditional is a
     * pseudo-observation row over the local column and the shared border, with
     * coefficient @c +1 on the local column, coefficients @f$M_{r}@f$ on the shared
     * border (the local moves with the border according to its posterior
     * regression), and measurement variance equal to the conditional variance
     * @f$(N_{rr}^{-1})_{rr}@f$.  Combined with the dense border carry (which supplies
     * the border's marginal information), this reproduces the exact joint recursion
     * without counting twice the border information that the local value also
     * encodes.
     *
     * Both outputs come from the cached @f$N_{rr}@f$ LDLᵀ factor through one local
     * solve @f$n_c=N_{rr}^{-1}e_r@f$: @c cond_var @f$=n_c(r)@f$ and
     * @c row @f$=N_{sr}\,n_c@f$.  Neither depends on the S-basis: both are functions
     * of the accumulated normals alone, independent of any datum or covariance
     * frame.  The caller restricts @c row to the free (non-datum) columns.
     *
     * @param station   Index from @ref addStation (must have local state).
     * @param local_col Local column within this station's @c A_r block.
     * @param carry_aware Include the folded DD priors' rank-K term (@ref hasCarryCovCorrection) in
     *                    both outputs: @c cond_var becomes @f$(\tilde A^{-1})_{rr}@f$ and
     *                    @c row becomes @f$N_{sr}\tilde A^{-1}e_r@f$.  @c false (default)
     *                    keeps the original expressions unchanged.  A corrected
     *                    @c cond_var that is ≤ 0 or non-finite is refused, and both
     *                    outputs fall back together, so the pseudo-observation row and
     *                    its variance always describe the same model.
     * @param cross_cols  For network.phase_leveling_local_block (F3): when non-null,
     *                    the station's local columns at which the same solve
     *                    @f$n_c=N_{rr}^{-1}e_r@f$ is also read, giving @c cross(j) =
     *                    @f$n_c(c_j)@f$ = @f$(N_{rr}^{-1})_{r c_j}@f$, the conditional
     *                    cross covariance of the local with column @f$c_j@f$ (its own
     *                    entry is @c cond_var).  Gathered on the plain-normals path
     *                    only: the carry-aware @f$\tilde A^{-1}@f$ correction is
     *                    computed for this column alone, so corrected diagonals with
     *                    uncorrected cross terms would describe two models.  @c cross
     *                    therefore stays empty whenever @p carry_aware is set and a DD
     *                    prior is folded (even when the correction is then refused),
     *                    and the block caller falls back (counted) instead of mixing
     *                    the two.
     * @return @ref StationRegression (empty @c row when @c n_shared is 0).
     * @throws std::out_of_range / std::invalid_argument on a bad index or column.
     */
    StationRegression stationRegression(std::size_t station,
                                        Eigen::Index local_col,
                                        bool carry_aware = false,
                                        const std::vector<Eigen::Index>* cross_cols = nullptr) const;

    /**
     * @brief Joint conditional covariance of an arbitrary set of local parameters
     *        from one or more stations, in the datum-as-known frame: the primitive
     *        for between-station double-difference ambiguity covariance
     *        (producer-side network AR, Phase 2).
     *
     * Each request is a @c (station, local_col) pair.  Returns the @c k×k joint
     * covariance whose @f$(a,b)@f$ entry is the covariance of the two requested
     * local parameters under the full bordered inverse:
     * @f[
     *   \mathrm{Cov}\big(x^{(i)}_{c_a}, x^{(j)}_{c_b}\big)
     *     = m_{c_a}^\top\,\mathrm{Cov}_\text{free}\,m_{c_b}
     *       + [\,i=j\,]\,(N_{ii}^{-1})_{c_a c_b},
     *   \qquad m_c = N_{sr}^{(i)}\,N_{ii}^{-1} e_c .
     * @f]
     * The shared-propagation term @f$m^\top\mathrm{Cov}_\text{free}\,m@f$ is present
     * for every pair and is the only coupling between different stations (they
     * correlate only through the shared satellite parameters); the local
     * @f$N_{ii}^{-1}@f$ term is added only for same-station pairs.  This equals
     * @ref reconstructStationCovariance restricted to a column subset on the
     * diagonal blocks, plus the cross-station blocks
     * @f$M_i\,\mathrm{Cov}_\text{free}\,M_j^\top@f$ that the DD covariance needs.
     * Costs one local solve @f$N_{ii}^{-1}e_c@f$ per request.
     *
     * @param requests    @c (station, local_col) pairs; each column must be valid
     *                    in that station's local block.
     * @param cov_free_xs Shared-parameter covariance (@c n_shared × n_shared) with
     *                    the datum-pinned rows/cols zeroed (the conditional frame).
     * @param carry_aware Include the folded DD priors' rank-K term (@ref hasCarryCovCorrection).
     *                    @c false (default) keeps the original expression unchanged.
     *                    The same-station restriction of the local term above holds
     *                    for @f$N_{rr}^{-1}@f$, which is block-diagonal, but not for
     *                    @f$\tilde A^{-1}@f$: the rank-K downdate couples every column
     *                    inside a block, and a DD chord spans two stations, so the
     *                    carry-aware form includes a cross-station local term, on
     *                    which a between-station DD variance depends.  If any
     *                    corrected diagonal entry is ≤ 0 or non-finite, the whole
     *                    matrix falls back to the uncorrected one.
     * @return The @c k×k joint conditional covariance (symmetric); empty if
     *         @p requests is empty.
     * @throws std::out_of_range / std::invalid_argument on a bad station, column,
     *         or @p cov_free_xs size.
     */
    Eigen::MatrixXd jointConditionalCovariance(
        const std::vector<std::pair<std::size_t, Eigen::Index>>& requests,
        const Eigen::MatrixXd& cov_free_xs, bool carry_aware = false) const;

    /**
     * @brief The blocks of @ref jointConditionalCovariance only (block search).
     *
     * Same expression as the uncorrected (@c carry_aware false) joint covariance,
     * @f$m_a^\top \mathrm{Cov}_{free} m_b + [s_a = s_b](N_{rr}^{-1})_{c_a c_b}@f$, but
     * evaluated only for the pairs inside each block of @p blocks, with
     * @f$\mathrm{Cov}_{free} M@f$ formed as one product.  Costs the sum of the
     * squared block sizes instead of @f$k^2@f$; a search that never reads a
     * cross-block entry needs nothing else.
     *
     * @param requests    As for @ref jointConditionalCovariance.
     * @param cov_free_xs As for @ref jointConditionalCovariance.
     * @param blocks      Each a list of indices into @p requests.
     * @return One symmetric matrix per block, in @p blocks order and index order.
     */
    std::vector<Eigen::MatrixXd> blockConditionalCovariances(
        const std::vector<std::pair<std::size_t, Eigen::Index>>& requests,
        const Eigen::MatrixXd& cov_free_xs,
        const std::vector<std::vector<int>>& blocks) const;

    /**
     * @brief Baarda per-row redundancy numbers for a batch of one station's
     *        measurement rows (B2), using the cached Schur factors.
     *
     * For each row @f$i@f$ with design @f$a_i = [a^s_i \mid a^r_i]@f$ (shared border
     * columns then this station's local columns) and information weight @f$w_i@f$,
     * returns @f$r_i = 1 - w_i\,a_i^\top \mathrm{Cov}(\theta)\,a_i@f$ clamped to
     * @f$[0,1]@f$.  The joint parameter covariance is assembled from @p cov_free_xs
     * (the datum-frame shared covariance) and this station's cached
     * @f$N_{rr}^{-1}, N_{sr}@f$ exactly as @ref reconstructStationCovariance does, so
     * no re-solve is needed:
     * @f$a_i^\top\mathrm{Cov}\,a_i = (a^s_i - M^\top a^r_i)^\top C_\text{free}
     *   (a^s_i - M^\top a^r_i) + a^{r\top}_i N_{rr}^{-1} a^r_i@f$, @f$M = N_{rr}^{-1} N_{sr}^\top@f$.
     *
     * @param station     Station index (from @ref addStation).
     * @param A_s_rows    Shared-border design rows (@c m × n_shared).
     * @param A_r_rows    Local design rows (@c m × n_local).
     * @param w_rows      Per-row information weights (@c 1/σ²; length @c m).
     * @param cov_free_xs Datum-frame shared covariance (@c n_shared × n_shared).
     * @return One redundancy number per row (length @c m), each in @f$[0,1]@f$.
     */
    Eigen::VectorXd rowRedundancy(std::size_t station,
                                  const Eigen::MatrixXd& A_s_rows,
                                  const Eigen::MatrixXd& A_r_rows,
                                  const Eigen::VectorXd& w_rows,
                                  const Eigen::MatrixXd& cov_free_xs) const;

    /**
     * @brief The @c b×b residual cofactor of a block of one station's rows: the
     *        counterpart of @ref rowRedundancy that keeps the off-diagonal terms
     *        (BlockFormal, Baarda block data snooping; @c docs/SPEC_redundancy_census.md
     *        INC-0).
     *
     * For a row subset @f$B@f$ of one station (the sweep's block is one
     * @c (station,satellite) LOS across bands, so @f$b=|B|\le 8@f$) this returns
     * @f[
     *   Q_{\tilde e,B} = \big(P^{-1} - A\,N^{-}A^\top\big)_{BB},
     *   \qquad
     *   (Q_{\tilde e,B})_{ij} = \frac{\delta_{ij}}{w_i}
     *                           - a_i^\top\,\mathrm{Cov}(\theta)\,a_j ,
     * @f]
     * the cofactor matrix of the least-squares residuals @f$\tilde e_B@f$, which the
     * block statistic @f$T_B\,b = \tilde e_B^\top Q_{\tilde e,B}^{-1}\tilde e_B@f$
     * needs and no per-row number can supply.  One physical fault on one LOS
     * (multipath, a wet radome lobe, an unflagged slip) affects every band's code
     * and phase row at once, and the adjustment spreads it across all of them.  The
     * off-diagonals kept here describe that spreading, which is why each row can
     * stay below its own w-test while the block as a whole fails the block test.
     * @f$b=1@f$ reduces to the scalar w-test's denominator, so this generalizes the
     * row tier rather than duplicating it.
     *
     * Same inputs, same station-local marginalization identity and same datum frame
     * as @ref rowRedundancy: this is the bilinear polarization of that method's
     * quadratic form,
     * @f[
     *   a_i^\top \mathrm{Cov}\, a_j =
     *     (a^s_i - M^\top a^r_i)^\top C_\text{free} (a^s_j - M^\top a^r_j)
     *     + a^{r\top}_i N_{rr}^{-1} a^r_j , \qquad M = N_{rr}^{-1}N_{sr}^\top ,
     * @f]
     * so the diagonal reproduces @ref rowRedundancy exactly:
     * @f$w_i\,(Q_{\tilde e,B})_{ii} = r_i@f$ (before that method's clamp; see the
     * first note).  @p A_s_rows / @p A_r_rows carry only the block's rows; the
     * covariance comes from the cached factors, which already hold every row of
     * every station, so a block's cofactor is the same whether it is passed alone or
     * as part of the station's full stack.
     *
     * @note The diagonal is not clamped, unlike @ref rowRedundancy's
     *       @f$r_i\in[0,1]@f$.  A fully leveraged block comes back singular, and the
     *       caller's uncheckable test detects exactly that singularity (SPEC §3, the
     *       block analogue of @f$r<\varepsilon@f$).  Clamping would turn it into an
     *       invertible-looking matrix, and the block would be inverted instead of
     *       counted; the census counts such a block, keeps its identity and never
     *       inverts through it, so this method returns the matrix as computed.
     *       Callers must check conditioning and finiteness before inverting: a zero
     *       @f$w_i@f$ gives an infinite diagonal entry and a negative @f$w_i@f$ a
     *       negative one, and neither is screened here.
     * @note No @c carry_aware flag, for the same reason @ref rowRedundancy has none
     *       (@c docs/SPEC_carry_aware_covariance.md §9): both feed detection
     *       thresholds, and the carry correction moves them.  That is a QC change,
     *       which needs a QC gate, not a side effect of a census kernel.
     *
     * @param station     Station index (from @ref addStation).
     * @param A_s_rows    Shared-border design rows of the block (@c b × n_shared).
     * @param A_r_rows    Local design rows of the block (@c b × n_local).
     * @param w_rows      Per-row information weights (@c 1/σ²; length @c b).
     * @param cov_free_xs Datum-frame shared covariance (@c n_shared × n_shared),
     *                    pinned/gauge columns zeroed: the same matrix
     *                    @ref rowRedundancy takes.
     * @return The symmetric @c b×b residual cofactor; empty if @p A_s_rows has no
     *         rows.  Symmetrized explicitly (see the implementation note).
     * @throws std::out_of_range / std::invalid_argument on a bad station, a row-count
     *         mismatch, or a bad @p A_s_rows / @p A_r_rows / @p cov_free_xs shape.
     */
    Eigen::MatrixXd blockResidualCofactor(std::size_t station,
                                          const Eigen::MatrixXd& A_s_rows,
                                          const Eigen::MatrixXd& A_r_rows,
                                          const Eigen::VectorXd& w_rows,
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
    ///         @ref addStationPrior with a positive weight / @ref reset).
    bool isReduced() const { return reduced_; }

    /**
     * @brief Read-only access to station @p station's shared↔local cross block
     *        @f$N_{sr}@f$ (@ref numShared × @ref numLocal), for the NEQ export.
     * @throws std::out_of_range if @p station ≥ @ref numStations.
     */
    const Eigen::MatrixXd& stationCrossBlock(std::size_t station) const;

    /**
     * @brief Read-only access to station @p station's local RHS @f$u_r@f$
     *        (@ref numLocal), for the NEQ export.
     * @throws std::out_of_range if @p station ≥ @ref numStations.
     */
    const Eigen::VectorXd& stationLocalRhs(std::size_t station) const;

    /**
     * @brief Station @p station's local normal block @f$N_{rr}@f$
     *        (@ref numLocal × @ref numLocal), rebuilt from the cached LDLᵀ factor
     *        (the raw matrix is not kept), so it includes any prior folded
     *        through @ref addStation or @ref addStationPrior.  Exact up to
     *        factorization round-off; intended for the offline NEQ export, not for
     *        per-epoch use.
     * @throws std::out_of_range if @p station ≥ @ref numStations.
     */
    Eigen::MatrixXd stationLocalNormals(std::size_t station) const;

    /**
     * @brief Station @p station's cached LDLᵀ pivots @f$D@f$, permuted back into
     *        the original local-column order.
     *
     * @c Eigen::LDLT pivots symmetrically (it factorizes @f$P N_{rr} P^\top =
     * L D L^\top@f$), so @c vectorD() is indexed in the permuted frame and cannot
     * name a state without the permutation.  This accessor applies the inverse
     * permutation: entry @c k is the pivot of local column @c k as the caller built
     * it, so the caller does not need to know about the pivoting.
     *
     * Read-only on the cached factor: no factorization, no allocation beyond the
     * returned vector.  A pivot far below the station's median pivot marks a local
     * state that carries almost no information this epoch (the state-side
     * complement of a zero-redundancy row).
     *
     * @throws std::out_of_range if @p station ≥ @ref numStations.
     */
    Eigen::VectorXd stationLocalPivots(std::size_t station) const;

    /// @return How many times @ref reduce has recomputed the per-station Schur loop
    ///         since the last @ref reset (no-op returns on a current reduction do not
    ///         count).  Reset to 0 by @ref reset.  Serves to assert the
    ///         reduce-once-per-epoch invariant when the carried prior is on: a local
    ///         prior injected after the gate would force a second full reduce.
    std::size_t reduceRecomputeCount() const { return reduce_recompute_count_; }

    /// @name Section timers (Tier D0, @c network.trace_section_timers)
    ///
    /// Wall-clock accumulators (milliseconds) over the primitive kernel operations
    /// that make up the network's per-epoch cost, so the driver can attribute that
    /// cost across assembly, reduce, solve and covariance, and decide which Tier-D
    /// scaling option (D1 selected inverse, D2 rate-split, ...) is worth building.
    ///
    /// Off by default: when @ref setPerfTiming is false, every instrumented method
    /// skips the clock reads entirely, so there is no measurable overhead and the
    /// output is byte-identical (the timers touch nothing the solution depends on).
    /// The accumulators sum across calls and are not cleared by @ref reset (they
    /// measure the whole run, independent of the per-epoch state); the driver
    /// snapshots them for per-epoch deltas and reads the run total at exit.
    /// @ref resetSectionTimers zeroes them.
    /// @{

    /// Per-section wall-clock accumulators + call counts (see @ref sectionTimers).
    struct SectionTimers {
        double add_station_ms{0.0};    ///< @ref addStationObservations: forming @f$A_s^\top W A_s@f$ etc. + the @f$N_{rr}@f$ LDLᵀ factorization (the assembly/scatter primitive).
        double reduce_ms{0.0};         ///< @ref reduce: the per-station Schur elimination onto the shared border (timed only on a real recompute).
        double solve_ms{0.0};          ///< @ref solveSharedConstrained: the constrained factorization + the point solve for @c x_s.
        double cov_inverse_ms{0.0};    ///< @ref solveSharedConstrained: the explicit full inverse for the shared covariance (@f$\tilde N_{ss}^{-1}@f$), the direct cost that the D1 selected-inverse option targets.
        double cov_consumers_ms{0.0};  ///< The per-station covariance back-propagations (the D1 per-station option): reconstructStation (+ ...Covariance / ...Diag / jointConditionalCovariance / rowRedundancy / stationRegression).  Timed at the @ref NetworkEstimator wrapper layer (via @ref accumulateCovConsumer), so the span also includes that layer's copy of the shared covariance and its datum-column zeroing: the per-call @f$O(n_s^2)@f$ overhead the D1 selected-inverse rewrite removes.
        std::size_t add_station_calls{0};   ///< #@ref addStationObservations calls timed.
        std::size_t reduce_recomputes{0};   ///< #real @ref reduce recomputes timed (mirrors @ref reduceRecomputeCount but gated on timing).
        std::size_t solve_calls{0};         ///< #@ref solveSharedConstrained calls timed.
        std::size_t cov_consumer_calls{0};  ///< #covariance-consumer calls timed (all methods above pooled).
        // Diagnostic for parallel_carry (P0b): the three parts of the per-block algebra
        // of @ref foldReducedDdPriors, so the dominant cost (per-station reduction, K×K
        // middle inverse, or Woodbury GMGᵀ) is measured before choosing the axis to
        // parallelize.  Accumulated only on the serial fold path (n_threads == 1, where
        // parallel_carry applies).
        double fold_loop_ms{0.0};      ///< the per-station B/G/z_free reduction loop (Σ_r CrTᵀ N_rr⁻¹ CrT etc.).
        double fold_minv_ms{0.0};      ///< the middle inverse M = (Pᶻ+B)⁻¹ (LDLᵀ factor + solve for the explicit K×K inverse).
        double fold_gmg_ms{0.0};       ///< the Woodbury fold onto the reduced shared system (G M Gᵀ, G M (z_free−ẑ)).
    };

    /// Arm or disarm the section timers (@c network.trace_section_timers).  Configuration
    /// level: set once before the epoch loop; it survives @ref reset (like
    /// @ref setStationPdMedianScale).  Default off: no clock reads, byte-identical output.
    void setPerfTiming(bool on) { perf_timing_ = on; }

    /// Zero the section-timer accumulators (see @ref SectionTimers).
    void resetSectionTimers() const { timers_ = SectionTimers{}; }

    /// Put back accumulators read earlier from @ref sectionTimers, discarding whatever was
    /// timed since.  For a caller whose solves are not the float path's own: the detached
    /// held solve (@ref NetworkEstimator::solveHeldDetached) restores them on exit,
    /// so the D0 run-end line and the driver's per-bracket accounting count the float solves
    /// alone, and the held solves' cost is read from the fixed-cycle seam's own timer.
    /// @param saved Accumulators previously returned by @ref sectionTimers.
    void restoreSectionTimers(const SectionTimers& saved) const { timers_ = saved; }

    /// @return The section-timer accumulators (cumulative since construction /
    ///         the last @ref resetSectionTimers).  Meaningful only when
    ///         @ref setPerfTiming was armed.
    const SectionTimers& sectionTimers() const { return timers_; }

    /// @return Whether the section timers are armed (@ref setPerfTiming).  Lets the
    ///         @ref NetworkEstimator covariance wrappers decide, per call, whether to
    ///         read the clock before accumulating via @ref accumulateCovConsumer.
    bool perfTiming() const { return perf_timing_; }

    /// Add @p ms to @ref SectionTimers::cov_consumers_ms and bump its call count.
    /// Called by the @ref NetworkEstimator covariance wrappers so the measured span
    /// includes their copy of the shared covariance and its datum-column zeroing, not
    /// only this kernel's propagation.  The caller guards on @ref perfTiming; this
    /// mutates only the (mutable) diagnostic accumulators, hence @c const.
    void accumulateCovConsumer(double ms) const {
        timers_.cov_consumers_ms += ms;
        ++timers_.cov_consumer_calls;
    }
    /// @}

    /**
     * @brief Use the median |LDLᵀ pivot| instead of the maximum as the reference
     *        scale of the local-block positive-definiteness test in
     *        @ref addStation (@c network.station_pd_pin_aware).
     *
     * The legacy test rejects a station when its smallest pivot is ≤ 0 or below
     * @c 1e-12 × the largest |pivot|.  A deliberate large-weight gauge-pin row
     * inside the local block (the prioritized phase-datum anchor's @c 1e10
     * ambiguity pin) raises that bar by about 10 orders of magnitude, so a
     * marginal station whose weakest genuine parameter has a usable pivot of about
     * 1e-3 is rejected, although the same block without the pin passes.  The
     * median |pivot| is robust to a minority of pin-scale pivots and still catches
     * a truly singular direction (a pivot many orders below the block's typical
     * scale).  Configuration level: set once; it survives @ref reset.  Default
     * off: the legacy max-pivot test, byte-identical.
     */
    void setStationPdMedianScale(bool on) { station_pd_median_scale_ = on; }

    /**
     * @brief Spectral read of the last positive-definiteness rejection in
     *        @ref addStation: the null direction that the pivot test reports only
     *        as a number.
     *
     * The rejection message gives a pivot magnitude, not a column.  LDLᵀ pivots are
     * permuted (@c transpositionsP), so a pivot index is not a column index and must
     * not be read as one.  This record instead holds a @c SelfAdjointEigenSolver
     * read of the same @f$N_{rr}^r@f$ the test rejected (including any folded
     * @ref StationPrior): its smallest eigenpair, the count of eigenvalues under the
     * relative cutoff, and the pivot triple the decision used.  The caller maps the
     * largest-|weight| entries of @ref null_vec to StateManager labels through its
     * own @c h2local map (the kernel has no label vocabulary), the same mapping
     * @c network.neq_export_epoch uses.
     *
     * Written only on a rejection.  With @ref setStationPdNullTrace armed (and a
     * successful eigen-decomposition) the record is filled and @ref valid is true;
     * with it off, a rejection sets @ref valid to false.  An accepted
     * @ref addStation and @ref reset leave the record unchanged, so read it in the
     * caller's @c catch, before a later rejection overwrites it.
     */
    struct StationPdFailure {
        bool          valid{false};      ///< this record describes a real rejection (armed + failed)
        Eigen::Index  n_local{0};        ///< the rejected block's dimension
        double        d_min{0.0};        ///< smallest LDLᵀ pivot (the test's statistic; may be < 0)
        double        d_ref{0.0};        ///< the test's reference scale (median |pivot| when pin-aware, else max)
        double        d_max{0.0};        ///< largest |LDLᵀ pivot|
        double        ev_min{0.0};       ///< smallest eigenvalue of N_rr_r (the null direction's)
        double        ev_max{0.0};       ///< largest eigenvalue (the block's spectral scale)
        int           n_eig_below{0};    ///< #eigenvalues with |λ| < 1e-12 * ev_max (the multiplicity of the null)
        Eigen::VectorXd null_vec;        ///< unit eigenvector of ev_min, in local column order
    };

    /**
     * @brief Arm the null-direction read on a PD rejection in @ref addStation
     *        (@c network.trace_station_pd_null).
     *
     * Off (the default), the rejection path is byte-identical: no
     * eigen-decomposition, no allocation, no record filled.  On, it costs one
     * symmetric eigen-decomposition of the rejected block only, so no accepted
     * station pays for it.  The rejection itself is the same either way: this names
     * the null direction and does not admit the station.  Configuration level: set
     * once before the epoch loop; it survives @ref reset (like
     * @ref setStationPdMedianScale).
     */
    void setStationPdNullTrace(bool on) { station_pd_null_trace_ = on; }

    /// @return The spectral record of the most recent @ref addStation PD rejection
    ///         (see @ref StationPdFailure).  @c valid is true only if
    ///         @ref setStationPdNullTrace was armed at that rejection.  An accepted
    ///         @ref addStation does not clear the record, so it describes the
    ///         preceding call only when read in that call's @c catch.
    const StationPdFailure& lastStationPdFailure() const { return last_pd_failure_; }

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
    std::vector<Station> stations_;  ///< per-station block pool (capacity persists across @ref reset)
    std::size_t n_active_{0};        ///< number of pool slots in use this epoch (≤ stations_.size())

    Eigen::MatrixXd reduced_N_ss_; ///< Ñ_ss (valid when reduced_)
    Eigen::VectorXd reduced_u_s_;  ///< ũ_s  (valid when reduced_)
    bool reduced_{false};

    /// A folded sum-to-zero gauge's cached data for the constrained reconstruction
    /// (@ref foldReducedSumZeroGauges).  Empty on the anchor / OFF path, so
    /// @ref reconstructStation is then byte-identical.
    struct FoldedSumZeroGauge {
        std::vector<std::pair<std::size_t, Eigen::Index>> terms;  ///< (station, local col)
        Eigen::VectorXd g;       ///< Σ_r N_sr n_r (n_shared)
        double beta{0.0};        ///< Σ_r (N_rr⁻¹)_{z_r z_r} (> 0)
        double gamma{0.0};       ///< Σ_r (N_rr⁻¹ u_r)_{z_r}
    };
    std::vector<FoldedSumZeroGauge> sumzero_gauges_;              ///< folded gauges (cleared by reduce()/reset())
    std::vector<std::vector<std::pair<std::size_t, Eigen::Index>>> station_sumzero_;  ///< station → list of (gauge idx, local col) for the reconstruction correction
    bool sumzero_folded_{false};                                 ///< guards double-folding into reduced_N_ss_ within one reduce (fold is applied in place; the gate + solve both trigger it)

    /// A folded rank-K DD-prior block's cached data (@ref foldReducedDdPriors) for
    /// the constrained reconstruction + the posterior-mean carry.  Empty on the OFF
    /// path, so @ref reconstructStation stays byte-identical.
    struct FoldedDdPrior {
        Eigen::MatrixXd G;       ///< Σ_r N_sr N_rr⁻¹ C_{L,r}ᵀ  (n_shared × K)
        Eigen::MatrixXd M;       ///< (Pᶻ + B)⁻¹                 (K × K)
        Eigen::MatrixXd Minv;    ///< Pᶻ + B = M⁻¹ (the matrix before factorization, K × K): the
                                 ///< innovation covariance under H0 before the shared-coupling
                                 ///< subtraction (@ref ddPriorInnovation).  Post-repair when an
                                 ///< eigenvalue clamp was applied; kept for a degenerate block too
                                 ///< (M=0 there, but Minv is the raw non-PD Pᶻ+B).
        Eigen::VectorXd z_free;  ///< Σ_r C_{L,r} N_rr⁻¹ u_r     (K)
        Eigen::VectorXd zhat;    ///< prior means                (K)
        Eigen::MatrixXd Pz;      ///< prior covariance           (K × K, for the posterior shrink)
        /// B = Σ_r C_{L,r} N_rr⁻¹ C_{L,r}ᵀ, the data information of this block's DDs, kept
        /// so that the posterior can be read back in product form (Pᶻ − PᶻMPᶻ ≡ PᶻMB).
        /// Filled only on the PD-safe path (INC-1a); empty unless @ref setDdFoldPdSafe.
        /// It is not recomputed as @c Minv − @c Pz at readback, because that subtraction
        /// reintroduces the cancellation between operands of order 1e6 that the product
        /// form avoids (docs/SPEC_pd_safe_dd_fold.md §4(a)).
        Eigen::MatrixXd B;
        // --- per-block PD-screen diagnostics (INC-0; always filled; solve-neutral) ---
        bool   degenerate{false};    ///< the PD screen tripped, so this block folded inertly (M = 0).
        double min_pivot_rel{0.0};   ///< d_min/d_max of the block's factor (0 for a K=0 slot or a failed factor).
        bool   repaired{false};      ///< an eigenvalue clamp repaired this block's factor (INC-1c).
        double repair_worst{0.0};    ///< |worst clamped eigenvalue| / λ_max (INC-1c; 0 when nothing was clamped).
    };
    std::vector<FoldedDdPrior> dd_priors_;   ///< folded DD-prior blocks (cleared by reduce()/reset())
    DdFoldStats dd_fold_stats_;              ///< the last fold's PD-screen aggregate (INC-0; per fold, not cumulative)
    bool dd_fold_pd_safe_{false};            ///< PD-safe fold arithmetic armed (@ref setDdFoldPdSafe); survives reset()
    /// One (block, row, col, coeff) term of a station's DD-prior reconstruction correction.
    struct DdStationTerm { std::size_t block; Eigen::Index row; Eigen::Index col; double coeff; };
    std::vector<std::vector<DdStationTerm>> station_ddprior_;  ///< station → its DD-prior terms
    /**
     * @brief Apply the sum-to-zero gauge correction of @ref reconstructStation to @p x_r in
     *        place: @f$x_r \mathrel{-}= n_r(\gamma - g^\top x_s)/\beta@f$ per folded gauge
     *        with a term at @p station.  No-op when no gauge is folded.
     * @param station A valid station index (checked by the caller).
     * @param x_s     The shared estimate (size checked by the caller).
     * @param x_r     [in,out] length @c n_local of the station.
     */
    void applySumZeroCorrection(std::size_t station, const Eigen::VectorXd& x_s,
                                Eigen::VectorXd& x_r) const;
    /**
     * @brief Apply the DD-prior correction of @ref reconstructStation to @p x_r in place:
     *        @f$x_r \mathrel{-}= N_{rr}^{-1}C_{L,r}^\top\kappa@f$.  No-op when no DD prior
     *        has a term at @p station.
     * @param station A valid station index (checked by the caller).
     * @param x_s     The shared estimate (size checked by the caller).
     * @param x_r     [in,out] length @c n_local of the station.
     */
    void applyDdPriorCorrection(std::size_t station, const Eigen::VectorXd& x_s,
                                Eigen::VectorXd& x_r) const;
    /**
     * @brief The batched rank-K carry correction for a request set
     *        (@ref hasCarryCovCorrection; docs/SPEC_carry_aware_covariance.md §5a).
     *
     * Given each request's station and its @f$n_i=N_{rr}^{-1}e_{c_i}@f$ (the solve the
     * blind path already performs), fills
     * @f$\texttt{local\_corr}(i,j)=\sum_b Y^{(i)\top}_bM_bY^{(j)}_b@f$ (@c k×k) and
     * @f$\texttt{m\_corr}(:,i)=\sum_b G_bM_bY^{(i)}_b@f$ (@c n_shared×k), both of which
     * the caller subtracts.  One sparse pass over @ref station_ddprior_ builds every
     * block's @f$Y@f$ columns, then three matrix products per block
     * (@f$M_bY_b@f$, @f$Y_b^\top(M_bY_b)@f$, @f$G_b(M_bY_b)@f$), so the cost is
     * @f$O(\sum_b K_b\,k\,(K_b+k+n_s))@f$ in BLAS-3 rather than @f$k@f$ separate
     * K-vector products.
     *
     * A chordless (K=0) or never-factored block is skipped.  A degenerate block (M=0,
     * the inert fold of INC-0) goes through the same arithmetic and contributes exactly
     * zero, so the inert-fold semantics carry through the readback.
     */
    void carryCovCorrection(const std::vector<std::size_t>& sta,
                            const std::vector<Eigen::VectorXd>& n,
                            Eigen::MatrixXd& local_corr,
                            Eigen::MatrixXd& m_corr) const;
    /// Fold one accepted (corrected, blind) variance pair into @ref CarryCovStats.  A
    /// pair with a non-positive or non-finite blind variance, or a non-finite corrected
    /// one, is skipped, so the ratio statistics never use a substitute denominator.
    void tallyCarryRatio(double corrected, double blind) const {
        if (!(blind > 0.0) || !std::isfinite(blind) || !std::isfinite(corrected)) return;
        const double r = corrected / blind;
        carry_cov_stats_.ratio_sum += r;
        ++carry_cov_stats_.ratio_n;
        if (r < carry_cov_stats_.ratio_min) carry_cov_stats_.ratio_min = r;
        if (r > carry_cov_stats_.ratio_max) carry_cov_stats_.ratio_max = r;
    }
    mutable CarryCovStats carry_cov_stats_;  ///< run-cumulative readback telemetry (survives reset(), like @ref timers_)
    bool ddprior_folded_{false};             ///< guards double-folding the DD priors within one reduce
    std::size_t reduce_recompute_count_{0}; ///< #full reduce() recomputes since reset() (reduce-once invariant)
    bool station_pd_median_scale_{false};   ///< pin-aware addStation PD test (median-|pivot| reference scale); survives reset()
    bool station_pd_null_trace_{false};     ///< eigen-read the rejected local block's null direction (RF-PD); survives reset()
    StationPdFailure last_pd_failure_;      ///< the last PD rejection's spectral record (RF-PD; see @ref lastStationPdFailure)
    bool perf_timing_{false};               ///< section timers armed (D0; survives reset(); set once by the driver)
    mutable SectionTimers timers_;          ///< wall-clock accumulators (D0; mutable: the const covariance/solve paths accumulate too)
};

} // namespace GnssProcessor
