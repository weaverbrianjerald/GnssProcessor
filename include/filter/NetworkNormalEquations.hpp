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

    /// One anchor-free sum-to-zero-over-stations gauge (Fix A, the ensemble
    /// phase-bias↔ambiguity datum): @f$\sum_{\text{terms}} x_{\text{local}} = 0@f$,
    /// each term a (station, local column) — e.g. every receiver's ambiguity to
    /// one (satellite, signal).  Realizes the datum WITHOUT a pivot receiver, so
    /// the emitted satellite phase bias references the whole receiver ensemble
    /// (no handover, no single-anchor SPOF, and a consistent between-satellite
    /// reference that keeps the user's DD ambiguity integer — Odijk S-system).
    struct SumZeroGauge {
        std::vector<std::pair<std::size_t, Eigen::Index>> terms;  ///< (station index, local column)
        /// The SHARED (border) column this gauge fixes — the satellite phase-bias
        /// column referenced by the receiver ensemble.  Set by the driver.  It is
        /// the gauge (null) DIRECTION of the unreduced normals, so the rank-1 fold
        /// leaves its slot of the reduced covariance carrying an ARTIFICIAL
        /// gauge-direction variance + phase↔clock/code cross terms (not a physical
        /// uncertainty).  Consumers of the datum-frame conditional covariance must
        /// treat it as EXACT (variance 0) — @ref conditionalLocalVarianceDiag /
        /// @ref conditionalLocalCovariance / @ref rowRedundancy zero it exactly as
        /// they zero the datum-pinned columns.  -1 = unset (e.g. the unit tests that
        /// exercise only the point solve, which does not read this).
        Eigen::Index shared_col{-1};
    };

    /**
     * @brief Fold anchor-free sum-to-zero gauges onto the reduced shared system as
     *        Schur-projected rank-1 corrections (the ensemble phase-bias datum).
     *
     * Each gauge @f$\sum_r z_r = 0@f$ couples LOCAL ambiguity columns across
     * stations, so it cannot be a shared-border row.  As the hard limit
     * (@f$w\to\infty@f$) of the soft constraint @f$w(\sum_r z_r)^2@f$, Woodbury
     * through the block-diagonal local normals yields a rank-1 update of the reduced
     * shared normals computed entirely from the cached per-station LDLᵀ factors:
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
     * The stored @f$(g,\beta,\gamma)@f$ + per-station term index let
     * @ref reconstructStation return the CONSTRAINED locals
     * (@f$x_r \mathrel{-}= n_r(\gamma - g^\top x_s)/\beta@f$), so the carried /
     * QC ambiguities live in the same sum-to-zero gauge as the emitted bias.
     *
     * @par Ordering
     * Lazily @ref reduce()s, then folds.  Call AFTER any @ref reduce that rebuilt
     * the reduced system and BEFORE the null-space gate reads
     * @ref reducedSharedNormals (the fold fixes the phase gauge, so the gate's
     * declared dimension stays = the clock deficiency count, exactly as the anchor
     * mode whose local pins were pre-eliminated).  A subsequent @ref reduce
     * recompute or @ref reset CLEARS the stored gauges — the rebuilt reduced system
     * no longer carries the fold, so the driver MUST re-call this after any
     * re-reduce (mirrors the code-bias / clock-constraint re-apply on the
     * within-epoch RAIM re-solve).  A gauge with @f$\beta\le0@f$ (no valid terms)
     * is skipped.
     *
     * @param gauges The sum-to-zero gauges to fold.  A term's station must be
     *        active this epoch and its column within that station's local block.
     * @throws std::out_of_range / std::invalid_argument on a bad station/column.
     */
    void foldReducedSumZeroGauges(const std::vector<SumZeroGauge>& gauges);

    /// One connected (const,signal) component's rank-K double-difference PRIOR — the
    /// Card-4 invariant-carry z-store's contribution to the producer solve (M1
    /// phase C).  Each of the K rows is a DD ambiguity constraint
    /// @f$z_k=\sum(\text{coeff}\cdot x_{\text{local}})@f$ with signed coefficients on
    /// four (or, for higher-order fundamental cycles, more) LOCAL ambiguity columns
    /// spanning up to four stations — a GAUGE-FREE quantity (the receiver + satellite
    /// phase biases cancel, so it touches NO shared column).  The K carried DDs have
    /// prior mean @c zhat and a DENSE prior covariance @c Pz (fundamental cycles share
    /// tree edges ⇒ correlated ⇒ a JOINT rank-K fold, NOT K independent rank-1 folds —
    /// the phase-A scout's adversarial correction).
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
     *        per-component JOINT Woodbury corrections — the ONE structural change of
     *        Card 4 (M1 phase C, spec D3 option (i)).
     *
     * A DD prior @f$z\sim N(\hat z,P^z),\ z=C_L x_L@f$ on LOCAL ambiguity columns
     * couples columns ACROSS stations, so it cannot be a per-station @c N_rr
     * increment (that would break the block-diagonal arrowhead).  As the soft
     * generalization of @ref foldReducedSumZeroGauges (which is exactly the hard-
     * constraint @f$P^z\!\to\!0,\ K\!=\!1,\ \hat z\!=\!0@f$ limit of this), Woodbury
     * through the block-diagonal local normals yields, per component,
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
     * @f$r@f$'s columns; @f$B,G,z_{\text{free}}@f$ accumulate from the CACHED
     * per-station LDLᵀ factors (no re-reduce, factors left pristine), and the
     * @f$K\times K@f$ middle inverse @f$M@f$ is per-component and bounded (no
     * @f$O(n^3)@f$ border blow-up — the rejected option (ii) cost).  The stored
     * @f$(G,M,z_{\text{free}},\hat z)@f$ drive the matching @ref reconstructStation
     * correction
     * @f$x_r\mathrel{-}=N_{rr}^{-1}C_{L,r}^\top M\,(z_{\text{free}}-G^\top x_s-\hat z)@f$,
     * so the reconstructed locals are the CONSTRAINED (prior-folded) solution.
     *
     * @par Ordering / mutual exclusivity with the sum-to-zero gauge fold
     * Lazily @ref reduce()s, then folds ONCE (a second call is a no-op until a
     * @ref reduce recompute / @ref reset clears the stored blocks — mirrors
     * @ref foldReducedSumZeroGauges).  Call AFTER @ref reduce and BEFORE the solve.
     * @warning This and @ref foldReducedSumZeroGauges are BOTH post-reduce Woodbury
     * folds that read the PRISTINE per-station @f$N_{rr}@f$ factor.  Composing the two
     * over COUPLED local columns is INEXACT — it drops the cross term
     * @f$C_L N_{rr}^{-1} C_{L,\text{gauge}}^\top@f$ of the joint middle inverse (each
     * fold sees the base @f$N_{rr}@f$, not the other's augmentation).  They are
     * therefore MUTUALLY EXCLUSIVE by design: under @c carry_invariant the DD priors
     * SUPERSEDE the sum-to-zero phase-bias ensemble datum (spec D6), and the Card-4
     * per-epoch phase datum is instead the PRE-REDUCE tree pins (@ref addStationPrior),
     * which fold into @f$N_{rr}@f$ BEFORE the reduce and so compose EXACTLY with this
     * post-reduce DD fold (it reads the already-pinned factor).
     * @ref NetworkEstimator::solve throws if both are set.  A block whose
     * @f$P^z+B@f$ is not positive-definite folds INERTLY (@f$M=0@f$, an index-stable
     * no-op) rather than being dropped, so @ref ddPriorPosteriorMean / @ref numDdPriorBlocks
     * keys stay aligned with the INPUT block order (a chordless @f$K=0@f$ block is an
     * inert slot too).
     *
     * @param blocks One rank-K prior block per connected component.
     * @param n_threads Optional deterministic fan-out of the per-block fold
     *        (@c network.parallel_seam; default 1 = the serial fold, byte-identical to
     *        the historical loop).  With @p n_threads > 1 and OpenMP, each block's
     *        algebra + its @f$G M G^\top@f$ / @f$G M(z_{\text{free}}-\hat z)@f$ Woodbury
     *        contribution is computed into a per-block slot in parallel, then the slots
     *        are ACCUMULATED SERIALLY in block-index order — the floating-point
     *        summation order is unchanged, so the reduced system is BIT-IDENTICAL for
     *        ANY thread count (unlike @ref reduce's opt-in ULP-reassociating fan-out).
     *        The dense kernels must run single-threaded (the driver pins Eigen to one
     *        thread for the epoch) to avoid nested oversubscription.
     * @param station_threads Optional WITHIN-BLOCK fan-out (@c network.parallel_carry;
     *        default 1 = serial).  Takes effect only on the SERIAL fold path
     *        (@p n_threads == 1) — the driver runs the carry serial-over-blocks and asks
     *        for within-block parallelism instead.  With @p station_threads > 1 the
     *        per-station reduction (@f$\Sigma_r C_{L,r}^\top N_{rr}^{-1} C_{L,r}@f$ etc.)
     *        is fanned across threads via per-thread partials combined in ascending thread
     *        order — reproducible per thread-count but ULP-different from the serial fold
     *        (the parallel_carry relaxation, mirroring @ref reduce), and the K×K middle
     *        inverse's @c solve is column-parallelized (each RHS column independent ⇒
     *        byte-identical given @f$P^z+B@f$; the LDLT factor stays serial).  Eigen must
     *        stay pinned to one thread (the driver does NOT un-pin it around the fold when
     *        this is armed) so the per-station OpenMP does not nest with Eigen's GEMM.
     * @throws std::out_of_range / std::invalid_argument on a bad station, column,
     *         or a @c Pz / @c zhat dimension mismatch.
     */
    void foldReducedDdPriors(const std::vector<DdPriorBlock>& blocks, int n_threads = 1,
                             int station_threads = 1);

    /**
     * @brief Posterior mean of a folded block's K double differences given the
     *        solved shared estimate — the value the z-store carries forward.
     *
     * @f$\hat z_{\text{post}}=\hat z+P^z M\,(z_{\text{free}}-G^\top x_s-\hat z)@f$
     * (equivalently @f$C_L\hat x_L@f$ of the constrained locals): the shrinkage of
     * the prior @f$\hat z@f$ toward the data-implied DD @f$C_L x^0@f$.
     *
     * @param block_index INPUT block index — @ref foldReducedDdPriors preserves the
     *        input order (a degenerate/chordless block is an inert slot), so the index
     *        stays aligned with the caller's z-store block list.
     * @param x_s The solved shared estimate (length @c n_shared).
     * @return The K posterior DD means (empty for a chordless @f$K=0@f$ slot; the prior
     *         @c zhat unchanged for an inert degenerate block, since @f$M=0@f$).
     * @throws std::out_of_range on a bad index; std::invalid_argument on an x_s size mismatch.
     */
    Eigen::VectorXd ddPriorPosteriorMean(std::size_t block_index,
                                         const Eigen::VectorXd& x_s) const;

    /**
     * @brief Posterior COVARIANCE of a folded block's K double differences — the
     *        @f$P^z@f$ the z-store carries forward (the Card-4 phase-C posterior
     *        readout the kernel's mean-only fold left open).
     *
     * The block's DD @f$z=C_L x_L@f$ is a gauge-free function of the local
     * ambiguities, whose full posterior covariance (prior + all network data,
     * including the shared-parameter coupling) has the closed form
     * @f[
     *   P^z_{\text{post}}
     *     = \underbrace{(P^z - P^z M P^z)}_{=\,(P^{z-1}+B^{-1})^{-1}=C_L N_{LL}'^{-1}C_L^\top}
     *     + (P^z M G^\top)\,\Sigma_{ss}\,(G M P^z),
     * @f]
     * built entirely from the cached fold factors @f$(P^z,M,G)@f$ (see
     * @ref foldReducedDdPriors) and the datum-as-known shared covariance
     * @f$\Sigma_{ss}@f$.  Derivation (@f$N_{LL}'=N_{LL}+C_L^\top P^{z-1}C_L@f$,
     * @f$\Sigma_{LL}=N_{LL}'^{-1}+N_{LL}'^{-1}N_{Ls}\Sigma_{ss}N_{sL}N_{LL}'^{-1}@f$,
     * Woodbury): the first term is @f$C_L N_{LL}'^{-1} C_L^\top = B-BMB = P^z-P^zMP^z@f$
     * (the conditional-given-@f$x_s@f$ posterior) and the coupling term uses
     * @f$C_L N_{LL}'^{-1} N_{Ls} = (I-BM)G^\top = P^z M G^\top@f$ (since
     * @f$P^zM+BM=(P^z+B)M=I@f$).  The DD is estimable, so the result is
     * datum-invariant despite @f$\Sigma_{ss}@f$ being datum-constrained.
     *
     * @param block_index INPUT block index (input order is preserved; see
     *        @ref ddPriorPosteriorMean).
     * @param cov_free_xs The shared covariance with the datum-pinned columns treated
     *        as EXACT (variance 0) — the same @c cov_free the
     *        @ref reconstructStationCovariance consumers build (n_shared × n_shared).
     * @return The K×K posterior DD covariance (the prior @c Pz unchanged for an inert
     *         degenerate block since @f$M=0@f$; empty for a chordless @f$K=0@f$ slot).
     * @throws std::out_of_range on a bad index; std::invalid_argument on a
     *         @p cov_free_xs size mismatch.
     */
    Eigen::MatrixXd ddPriorPosteriorCovariance(std::size_t block_index,
                                               const Eigen::MatrixXd& cov_free_xs) const;

    /**
     * @brief Innovation (measurement-minus-prediction) of a folded block's K double
     *        differences and its exact H0 covariance — the estimation-side fine-slip
     *        test the Card-4 D5 gate standardizes (spec D5 Def-1).
     *
     * The DD carry's coarse slip layer (MW / LLI / IF-TD) is deliberately relaxed; the
     * finer slips the estimator must catch show up as an inflated innovation on the
     * carried DD.  Per block, using ONLY the cached fold factors (@ref FoldedDdPrior)
     * plus the datum-as-known shared covariance @f$\Sigma_{ss}@f$ (@p cov_free_xs, the
     * SAME frame @ref ddPriorPosteriorCovariance consumes):
     * @f[
     *   v = z_{\text{free}} - G^\top x_s - \hat z, \qquad
     *   \mathrm{Cov}(v) = (P^z + B) - G^\top \Sigma_{ss} G = M^{-1} - G^\top\Sigma_{ss}G.
     * @f]
     * The @f$-G^\top\Sigma_{ss}G@f$ (NOT @f$+@f$) accounts for @f$x_s@f$ being
     * prior-influenced — the same border coupling that made @ref ddPriorPosteriorCovariance
     * datum-invariant.  Consistency (hand-verified): @f$P^zM\,\mathrm{Cov}(v)\,MP^z =
     * P^z - P^{z+}@f$, i.e. @f$\mathrm{Cov}(\hat z^+-\hat z)@f$ matches
     * @ref ddPriorPosteriorMean's @f$\hat z^+ - \hat z = P^zMv@f$.  @f$v@f$ is exactly the
     * @c innov of @ref ddPriorPosteriorMean.
     *
     * @param block_index INPUT block index (input order preserved; see @ref ddPriorPosteriorMean).
     * @param x_s         The solved shared estimate (length @c n_shared).
     * @param cov_free_xs The datum-as-known shared covariance (n_shared × n_shared), the
     *        same @c cov_free @ref ddPriorPosteriorCovariance builds.
     * @param[out] v      The K innovations (empty for a chordless K=0 slot).
     * @param[out] cov_v  The K×K innovation covariance under H0 (empty for a K=0 slot;
     *        for an inert degenerate block M=0 this is the raw non-PD Pᶻ+B − GᵀΣG — the
     *        caller must skip a chord whose @c cov_v diagonal is not strictly positive).
     * @throws std::out_of_range on a bad index; std::invalid_argument on an x_s /
     *         cov_free_xs size mismatch.
     */
    void ddPriorInnovation(std::size_t block_index, const Eigen::VectorXd& x_s,
                           const Eigen::MatrixXd& cov_free_xs,
                           Eigen::VectorXd& v, Eigen::MatrixXd& cov_v) const;

    /// @return Number of DD-prior blocks (== the input count of the last
    ///         @ref foldReducedDdPriors; degenerate/chordless blocks are inert slots).
    std::size_t numDdPriorBlocks() const { return dd_priors_.size(); }

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

    /// The regression of one local column on the shared border plus that column's
    /// PURE conditional variance — the two ingredients of a rigorous between-epoch
    /// conditional carry (Card 1, @c network.carry_conditional_coupling).
    struct StationRegression {
        Eigen::VectorXd row;   ///< @f$M_r^\top = N_{sr}N_{rr}^{-1}e_r@f$ (length @c n_shared): row @c r of @f$N_{rr}^{-1}N_{rs}@f$, i.e. the coefficients of the local's linear regression on the shared border.  Empty when @c n_shared is 0.
        double cond_var{0.0};  ///< @f$(N_{rr}^{-1})_{rr}=\mathrm{Var}(x_r\mid x_s)@f$: the local's variance CONDITIONAL on the whole shared border (NOT the marginal @ref reconstructStationCovarianceDiag returns).
    };

    /**
     * @brief Regression row @f$M_r=(N_{rr}^{-1}e_r)^\top N_{rs}@f$ and the pure
     *        conditional variance @f$(N_{rr}^{-1})_{rr}@f$ for one local column.
     *
     * The exact recursion carries a persistent local parameter (a phase-leveling
     * ambiguity or ZWD) forward as the conditional distribution @f$r\mid x_s@f$
     * rather than as an independent marginal.  That conditional is a
     * pseudo-observation ROW spanning @f$(x_s\mid x_r)@f$ with coefficient @c +1 on
     * the local column, coefficients @f$M_{r}@f$ on the shared border (so the prior
     * says "the local moves WITH the border per its posterior regression"), and
     * measurement variance = the conditional variance @f$(N_{rr}^{-1})_{rr}@f$.
     * Combined with the dense border carry (which supplies the border's marginal
     * information), this reproduces the exact joint recursion — no double count of
     * the border information the local value also encodes.
     *
     * Both outputs reuse the cached @f$N_{rr}@f$ LDLᵀ factor via ONE local solve
     * @f$n_c=N_{rr}^{-1}e_r@f$: @c cond_var @f$=n_c(r)@f$ and
     * @c row @f$=N_{sr}\,n_c@f$.  Deterministic S-basis matters not at all here —
     * both are pure functions of the accumulated normals, independent of any datum
     * or covariance frame (the datum-driven FREE-column restriction of @c row lives
     * in the caller).
     *
     * @param station   Index from @ref addStation (must have local state).
     * @param local_col Local column within this station's @c A_r block.
     * @return @ref StationRegression (empty @c row when @c n_shared is 0).
     * @throws std::out_of_range / std::invalid_argument on a bad index or column.
     */
    StationRegression stationRegression(std::size_t station,
                                        Eigen::Index local_col) const;

    /**
     * @brief Joint conditional covariance of an arbitrary set of local parameters
     *        drawn from ONE OR MORE stations, in the datum-as-known frame — the
     *        primitive for between-station double-difference ambiguity covariance
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
     * for EVERY pair — it is the only coupling between different stations (they
     * correlate solely through the shared satellite parameters); the local
     * @f$N_{ii}^{-1}@f$ term is added only for same-station pairs.  This equals
     * @ref reconstructStationCovariance restricted to a column subset on the
     * diagonal blocks, plus the cross-station blocks
     * @f$M_i\,\mathrm{Cov}_\text{free}\,M_j^\top@f$ the DD covariance needs.  Costs
     * one local solve @f$N_{ii}^{-1}e_c@f$ per request.
     *
     * @param requests    @c (station, local_col) pairs; each column must be valid
     *                    in that station's local block.
     * @param cov_free_xs Shared-parameter covariance (@c n_shared × n_shared) with
     *                    the datum-pinned rows/cols zeroed (the conditional frame).
     * @return The @c k×k joint conditional covariance (symmetric); empty if
     *         @p requests is empty.
     * @throws std::out_of_range / std::invalid_argument on a bad station, column,
     *         or @p cov_free_xs size.
     */
    Eigen::MatrixXd jointConditionalCovariance(
        const std::vector<std::pair<std::size_t, Eigen::Index>>& requests,
        const Eigen::MatrixXd& cov_free_xs) const;

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
     *        (@ref numLocal × @ref numLocal), REBUILT from the cached LDLᵀ factor
     *        (the raw matrix is not retained).  Exact up to factorization
     *        round-off; intended for the offline NEQ export, not per-epoch use.
     * @throws std::out_of_range if @p station ≥ @ref numStations.
     */
    Eigen::MatrixXd stationLocalNormals(std::size_t station) const;

    /// @return How many times @ref reduce has actually RECOMPUTED the per-station
    ///         Schur loop since the last @ref reset (idempotent no-op returns do
    ///         not count).  Reset to 0 by @ref reset; used to assert the
    ///         "reduce-once-per-epoch" invariant when the carried prior is on (a
    ///         stray post-gate local injection would force a second full reduce).
    std::size_t reduceRecomputeCount() const { return reduce_recompute_count_; }

    /// @name Tier D0 section timers (@c network.trace_section_timers)
    ///
    /// Wall-clock accumulators (milliseconds) over the primitive kernel operations
    /// the network per-epoch cost decomposes into, so the driver can attribute that
    /// cost across assembly / reduce / solve / covariance and decide which Tier-D
    /// scaling lever (D1 selected inverse, D2 rate-split, ...) is worth pulling.
    ///
    /// OFF by default: when @ref setPerfTiming is false every instrumented method
    /// skips the clock reads entirely — no measurable overhead and numerically
    /// byte-identical (the timers touch nothing the solution depends on).  The
    /// accumulators sum CONTINUOUSLY across calls (they are never auto-reset by
    /// @ref reset — they are a whole-run measurement, orthogonal to the per-epoch
    /// state churn); the driver snapshots them for per-epoch deltas and reads the
    /// run total at exit.  @ref resetSectionTimers zeroes them.
    /// @{

    /// Per-section wall-clock accumulators + call counts (see @ref sectionTimers).
    struct SectionTimers {
        double add_station_ms{0.0};    ///< @ref addStationObservations: forming @f$A_s^\top W A_s@f$ etc. + the @f$N_{rr}@f$ LDLᵀ factorization (the assembly/scatter primitive).
        double reduce_ms{0.0};         ///< @ref reduce: the per-station Schur elimination onto the shared border (timed only on a real recompute).
        double solve_ms{0.0};          ///< @ref solveSharedConstrained: the constrained factorization + the point solve for @c x_s.
        double cov_inverse_ms{0.0};    ///< @ref solveSharedConstrained: the explicit shared-covariance FULL inverse (@f$\tilde N_{ss}^{-1}@f$) — the D1 selected-inverse lever's direct cost.
        double cov_consumers_ms{0.0};  ///< The per-station covariance back-propagations (the D1 per-station lever): reconstructStation (+ ...Covariance / ...Diag / jointConditionalCovariance / rowRedundancy / stationRegression).  Timed at the @ref NetworkEstimator wrapper layer (via @ref accumulateCovConsumer), so the span ALSO includes that layer's shared-covariance COPY + datum-column zeroing — the per-call @f$O(n_s^2)@f$ overhead the D1 selected-inverse rewrite removes.
        std::size_t add_station_calls{0};   ///< #@ref addStationObservations calls timed.
        std::size_t reduce_recomputes{0};   ///< #real @ref reduce recomputes timed (mirrors @ref reduceRecomputeCount but gated on timing).
        std::size_t solve_calls{0};         ///< #@ref solveSharedConstrained calls timed.
        std::size_t cov_consumer_calls{0};  ///< #covariance-consumer calls timed (all methods above pooled).
        // parallel_carry diagnostic (P0b): the three sub-parts of @ref foldReducedDdPriors's
        // per-block algebra, so the dominant cost (per-station reduction vs the K×K middle
        // inverse vs the Woodbury GMGᵀ) is measured before choosing the parallelization axis.
        // Accumulated only on the SERIAL fold path (parallel_carry / n_threads==1).
        double fold_loop_ms{0.0};      ///< the per-station B/G/z_free reduction loop (Σ_r CrTᵀ N_rr⁻¹ CrT etc.).
        double fold_minv_ms{0.0};      ///< the middle inverse M = (Pᶻ+B)⁻¹ (LDLT factor + solve for the explicit K×K inverse).
        double fold_gmg_ms{0.0};       ///< the Woodbury fold onto the reduced shared system (G M Gᵀ, G M (z_free−ẑ)).
    };

    /// Arm/disarm the section timers (@c network.trace_section_timers).  Configuration
    /// level — set once before the epoch loop; survives @ref reset (like
    /// @ref setStationPdMedianScale).  Default off = no clock reads, byte-identical.
    void setPerfTiming(bool on) { perf_timing_ = on; }

    /// Zero the section-timer accumulators (see @ref SectionTimers).
    void resetSectionTimers() const { timers_ = SectionTimers{}; }

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
    /// includes their shared-covariance COPY + datum-column zeroing, not only this
    /// kernel's propagation.  The caller guards on @ref perfTiming; this only mutates
    /// the (mutable) diagnostic accumulators, hence @c const.
    void accumulateCovConsumer(double ms) const {
        timers_.cov_consumers_ms += ms;
        ++timers_.cov_consumer_calls;
    }
    /// @}

    /**
     * @brief Use the MEDIAN |LDLᵀ pivot| (instead of the maximum) as the
     *        reference scale of @ref addStation's local-block positive-
     *        definiteness test (@c network.station_pd_pin_aware).
     *
     * The legacy test rejects a station when its smallest pivot falls below
     * @c 1e-12 × the LARGEST pivot.  A deliberate huge-weight gauge-pin row
     * inside the local block — the prioritized phase-datum anchor's @c 1e10
     * ambiguity pin — inflates that bar by ~10 orders, ejecting a healthy
     * marginal station whose weakest genuine parameter sits at a perfectly
     * usable ~1e-3 (the TWTF/VOIM every-epoch non-PD class; same relative
     * floor a pin-free station passes easily).  The median |pivot| is robust
     * to any minority of pin-scale pivots while still catching a truly
     * singular direction (a pivot collapsing many orders below the block's
     * typical scale).  Survives @ref reset (configuration-level, set once).
     * Default off = the legacy max-pivot test, byte-identical.
     */
    void setStationPdMedianScale(bool on) { station_pd_median_scale_ = on; }

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
        Eigen::MatrixXd Minv;    ///< Pᶻ + B = M⁻¹ (the pre-factor matrix, K × K) — the innovation
                                 ///< covariance under H0 before the shared-coupling subtraction
                                 ///< (@ref ddPriorInnovation).  Kept for a degenerate block too
                                 ///< (M=0 there, but Minv is the raw non-PD Pᶻ+B).
        Eigen::VectorXd z_free;  ///< Σ_r C_{L,r} N_rr⁻¹ u_r     (K)
        Eigen::VectorXd zhat;    ///< prior means                (K)
        Eigen::MatrixXd Pz;      ///< prior covariance           (K × K, for the posterior shrink)
    };
    std::vector<FoldedDdPrior> dd_priors_;   ///< folded DD-prior blocks (cleared by reduce()/reset())
    /// One (block, row, col, coeff) term of a station's DD-prior reconstruction correction.
    struct DdStationTerm { std::size_t block; Eigen::Index row; Eigen::Index col; double coeff; };
    std::vector<std::vector<DdStationTerm>> station_ddprior_;  ///< station → its DD-prior terms
    bool ddprior_folded_{false};             ///< guards double-folding the DD priors within one reduce
    std::size_t reduce_recompute_count_{0}; ///< #full reduce() recomputes since reset() (reduce-once invariant)
    bool station_pd_median_scale_{false};   ///< pin-aware addStation PD test (median-|pivot| reference scale); survives reset()
    bool perf_timing_{false};               ///< D0: section timers armed (survives reset(); set once by the driver)
    mutable SectionTimers timers_;          ///< D0: wall-clock accumulators (mutable — the const covariance/solve paths accumulate too)
};

} // namespace GnssProcessor
