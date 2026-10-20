/**
 * @file Estimator.hpp
 * @brief Semantic layer over the arrowhead Schur kernel for the PPP-RTK network
 *        producer (W4-S2): shared satellite-parameter layout, explicit datum, and
 *        mapping to the emittable @c NetworkSolutionEpoch.
 *
 * @ref NetworkNormalEquations is pure linear algebra over abstract shared and
 * local blocks.  @c NetworkEstimator gives each shared column its GNSS meaning, a
 * (satellite, parameter kind, signal) triple, and drives one solution epoch:
 *
 *   1. register the shared satellite parameters (satellite clock, code bias,
 *      phase bias), which fixes the shared column layout;
 *   2. @ref beginEpoch: open a fresh arrowhead system of that width;
 *   3. @ref addStation per station: pre-formed local-vs-shared design rows, built
 *      by the caller from each station's single-receiver row builder (the
 *      network applies no receiver-side differencing);
 *   4. @ref solve with an explicit minimum-constraint @ref Datum.  The reduced
 *      shared satellite system is rank-deficient by the gauge freedoms (overall
 *      clock, the per-constellation phase-bias↔ambiguity datum, ISB), and the
 *      datum pins exactly those.  The pins are applied to a copy of the reduced
 *      system through the kernel's @c solveSharedConstrained seam, so the
 *      kernel's own reduced normals never carry them;
 *   5. @ref toSolutionEpoch: map the shared estimate to a @c NetworkSolutionEpoch,
 *      which @c NetworkProductEmitter serializes in IGS-SSR convention.
 *
 * Scope (W4-S2a): the single-epoch estimator core, validated on a synthetic
 * noise-free network (truth recovery up to the declared S-transform).  Outside
 * this class: the `runNetwork()` epoch loop (S2b: MultiRinexSync `by_station`,
 * the per-station design rows from the station row builder, the between-epoch
 * time update, the config flags); the `SbasisDeclaration` and the null-space rank
 * gate in @c NetworkSbasis (W5), which formalize the minimal pin set that
 * @ref Datum carries here; and cross-station QC (network RAIM).
 *
 * Datum elements local to one station's block (for example the pivot receiver's
 * ambiguity or clock pin that the emitted phase bias absorbs, Odijk Eq. 4/9) are
 * realized by the caller as tight priors on that station's local block (prior
 * rows in the design passed to @ref addStation, or @ref addStationPrior).  Only
 * the shared-block gauges go through @ref Datum.
 */

#pragma once

#include <Eigen/Dense>

#include <algorithm>
#include <cstddef>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "network/solve/NormalEquations.hpp"
#include "network/assembly/ContributionKind.hpp"   // ContributionKind, SharedFoldLog
#include "products/NetworkProductEmitter.hpp"

namespace spdlog { class logger; }

namespace GnssProcessor {

/// Kind of shared (cross-station) parameter carried in the network solution.
enum class SharedParamKind {
    Clock,      ///< Estimable satellite clock (signal-independent).
    CodeBias,   ///< Per-signal satellite code bias (OSB).
    PhaseBias,  ///< Per-signal satellite phase bias (OSB; carries the pivot lump).
    VtecSh,     ///< Global SH-VTEC coefficient (a_nm/b_nm; network-wide, not per-satellite; layer 0 = legacy shell, layer 1 = the dual-layer second shell).
};

/**
 * @brief Joint multi-station network estimator for one solution epoch.
 *
 * Not thread-safe.  Register layout once, then per epoch: @ref beginEpoch →
 * @ref addStation × N → @ref solve → @ref reconstructStation / @ref toSolutionEpoch.
 */
class NetworkEstimator {
public:
    /// A hard pin of a shared column to a value: one element of the S-basis.
    struct Pin {
        Eigen::Index column;  ///< Shared-block column (from @ref clockColumn etc.).
        double value;         ///< Datum value the column is fixed to (usually 0).
    };
    /// The explicit minimum-constraint datum applied to the reduced shared system.
    using Datum = std::vector<Pin>;

    NetworkEstimator() = default;

    // --- Shared-parameter layout (call before beginEpoch) -------------------

    /// Register a satellite clock column (idempotent); returns its column index.
    Eigen::Index registerClock(const std::string& sat);
    /// Register a per-signal satellite code-bias column (idempotent).
    Eigen::Index registerCodeBias(const std::string& sat, const std::string& signal);
    /// Register a per-signal satellite phase-bias column (idempotent).
    Eigen::Index registerPhaseBias(const std::string& sat, const std::string& signal);
    /// Register a global SH-VTEC coefficient column (n,m,trig) (idempotent).
    /// @p trig is 'C' (cosine, m>=0) or 'S' (sine, m>=1); @p layer selects the
    /// shell: 0 = legacy single layer, keyed "SH|n|m|t"; 1 = the dual-layer second
    /// shell, keyed "SH2|n|m|t", so both shells coexist on the border.
    Eigen::Index registerVtecCoeff(int n, int m, char trig, int layer = 0);
    /**
     * @brief Register the one-epoch clock clone column of @p sat (
     *        @c network.carry_coupling_clock_clone); idempotent; returns its column.
     *
     * The clone is the satellite's previous-epoch clock @f$c_s(t-1)@f$ carried for one
     * epoch in the shared border: the coupling rows keep their harvested clock
     * coefficients on it, and the walk @f$c_s(t)-c_s(t-1)@f$ enters once, in the border
     * prior (@ref addCloneWalk), instead of once per row (the per-row form of the exact coupling update
     * drops the time-update covariance between rows).  Keyed @c "sat|CLKC" (disjoint
     * from the clock's @c "sat|CLK") and labelled @c "CLKC:<sat>".  Its descriptor
     * carries @ref SharedParamKind::Clock with a clone flag, so no switch over the kind
     * meets a new value; every reader that tests the kind for a satellite clock also
     * tests the flag.  The driver registers clones after every other column, so no
     * existing column index moves and the clones occupy the last
     * @ref numClockClones columns.  A clone is never emitted (@ref toSolutionEpoch),
     * never grouped as a satellite clock of the gauge-free downdate, and never
     * marginalized by the temporal clock anchor's clock refloat (@ref refoldSharedPriorDenseDroppingClocks).
     * @throws std::logic_error after @ref beginEpoch, like every registration.
     */
    Eigen::Index registerClockClone(const std::string& sat);
    /**
     * @brief Register the one-epoch clone of any satellite border key (
     *        docs/refound/BCL_previous_border_on_clones_2026-10-08.md); idempotent; returns
     *        its column.
     *
     * A clock is @ref registerClockClone exactly (key @c "sat|CLKC").  A phase bias is keyed
     * @c "sat|PBC|signal" and labelled @c "PBC:<sat>:<signal>", a code bias
     * @c "sat|CBC|signal" and @c "CBC:<sat>:<signal>": disjoint from the biases' own keys,
     * and no "PB:" or "CB:" prefix parser matches the label.  The descriptor carries the
     * key's own kind with the clone flag, so every reader that tests a kind for a bias also
     * meets the flag through @ref sharedColumnIdentity (false for a clone) or the label.
     * Like B1's clones it is registered after every other column, counted in
     * @ref numClockClones (the clones of every kind), never emitted, stashed or
     * snapshotted.
     * @param kind   Clock, PhaseBias or CodeBias.
     * @param sat    the satellite.
     * @param signal the signal (ignored for a clock).
     * @throws std::invalid_argument for an SH-VTEC kind; std::logic_error after
     *         @ref beginEpoch, like every registration.
     */
    Eigen::Index registerClone(SharedParamKind kind, const std::string& sat,
                               const std::string& signal);
    /**
     * @brief Register shared-history column @p j
     *        (@c network.carry_coupling_shared_history); idempotent; returns its column.
     *
     * A history column is one constant direction of the clones' shared conditional
     * carried across stations (docs/refound/SCL2_level_residue_under_the_clone_2026-10-06.md,
     * sections 4, 5 and 9): the harvest compresses the carried rows' terms on the clones
     * and on the previous history into at most k columns per cloned clock, each row keeps
     * its coefficients on them, and the next epoch folds each column with the prior
     * N(0, 1), independent of the border.  Keyed @c "HIST|j", labelled @c "HIST:j".  Its
     * descriptor carries @ref SharedParamKind::Clock with an empty satellite and a history
     * flag, so every reader that tests the kind for a satellite clock (and a non-empty
     * satellite) skips it without a new enum value.  The driver registers history columns
     * after every other column, the clones included, so no existing index moves and the
     * clones and the history occupy the last @ref numBorderNuisance columns.  A history
     * column is never emitted (@ref toSolutionEpoch), never stashed and never snapshotted.
     * @throws std::logic_error after @ref beginEpoch, like every registration.
     */
    Eigen::Index registerHistoryColumn(int j);

    /// @return The clock column for @p sat. @throws std::out_of_range if absent.
    Eigen::Index clockColumn(const std::string& sat) const;
    /// @return The code-bias column for (@p sat, @p signal). @throws if absent.
    Eigen::Index codeBiasColumn(const std::string& sat, const std::string& signal) const;
    /// @return The phase-bias column for (@p sat, @p signal). @throws if absent.
    Eigen::Index phaseBiasColumn(const std::string& sat, const std::string& signal) const;
    /// @return The SH-VTEC coefficient column for (n,m,trig) of @p layer.
    /// @throws if absent.
    Eigen::Index vtecCoeffColumn(int n, int m, char trig, int layer = 0) const;
    /// @return The clock clone column of @p sat (@ref registerClockClone).
    /// @throws std::out_of_range if absent.
    Eigen::Index clockCloneColumn(const std::string& sat) const;
    /// @return The clone column of (@p kind, @p sat, @p signal) (
    ///         @ref registerClone). @throws std::out_of_range if absent.
    Eigen::Index cloneColumn(SharedParamKind kind, const std::string& sat,
                             const std::string& signal) const;
    /// @return Whether shared column @p col is a clone of any kind;
    ///         false when
    ///         @p col is out of range.
    bool isClockClone(Eigen::Index col) const;
    /// @return The shared-history column @p j (@ref registerHistoryColumn).
    /// @throws std::out_of_range if absent.
    Eigen::Index historyColumn(int j) const;
    /// @return Whether shared column @p col is a shared-history column;
    ///         false when @p col is out of range.
    bool isHistoryColumn(Eigen::Index col) const;
    /**
     * @brief The identity of a satellite shared column: the reverse of
     *        @ref clockColumn / @ref phaseBiasColumn / @ref codeBiasColumn.
     *
     * Read by the smoothed-hold clone's marginalization, which spreads a row's clone coefficients
     * over the border through a column-indexed regression and re-keys them by identity.
     * @param col          the shared column.
     * @param[out] kind    its kind (Clock, PhaseBias or CodeBias).
     * @param[out] sat     its satellite.
     * @param[out] signal  its signal (empty for a clock).
     * @return false (outputs untouched) for a column out of range, an SH-VTEC column, a
     *         clock clone or a shared-history column.
     */
    bool sharedColumnIdentity(Eigen::Index col, SharedParamKind& kind, std::string& sat,
                              std::string& signal) const;
    /// @return The number of clone columns registered this epoch (the clock clone's clocks and
    ///         the border clones' biases and departed keys); they are the last columns of the
    ///         layout before the history.  0 whenever the knobs are off.
    Eigen::Index numClockClones() const { return n_clones_; }
    /// @return The number of shared-history columns registered this epoch;
    ///         they follow the clones.  0 whenever the knob is off.
    Eigen::Index numHistoryColumns() const { return n_hist_; }
    /// @return The border's nuisance columns, the clones and the history:
    ///         @ref numClockClones + @ref numHistoryColumns, the trailing columns of the
    ///         layout that the gate view marginalizes and the harvest regresses away.
    ///         Equals @ref numClockClones whenever the shared history is off.
    Eigen::Index numBorderNuisance() const { return n_clones_ + n_hist_; }

    /// @return Number of registered shared columns.
    Eigen::Index numShared() const { return static_cast<Eigen::Index>(columns_.size()); }

    /**
     * @brief Stable human-readable label for shared column @p col.
     *
     * Format by kind: @c "CLK:<sat>" (satellite clock), @c "CB:<sat>:<signal>"
     * (satellite code bias), @c "PB:<sat>:<signal>" (satellite phase bias),
     * @c "SH:<C|S>:<n>:<m>" (SH-VTEC coefficient; @c "SH2:<C|S>:<n>:<m>" on the
     * second shell), @c "CLKC:<sat>" (a clock clone; no "CLK:" prefix
     * parser matches it), @c "PBC:<sat>:<signal>" and @c "CBC:<sat>:<signal>" (a phase-
     * or code-bias clone; no "PB:" or "CB:" prefix parser matches them),
     * @c "HIST:<j>" (a shared-history column).  Intended for the NEQ export and other offline attribution
     * tooling; labels are stable across epochs for an unchanged registration order.
     *
     * @throws std::out_of_range if @p col is not a registered column.
     */
    std::string sharedColumnLabel(Eigen::Index col) const;

    /// @return Read-only access to the current epoch's arrowhead kernel (for the
    ///         NEQ export; see @ref NetworkNormalEquations accessors).
    const NetworkNormalEquations& normals() const { return neq_; }

    // --- One solution epoch -------------------------------------------------

    /**
     * @brief Clear the shared-column layout for a new epoch, reusing allocations.
     *
     * Call at the top of each epoch, before re-registering the shared parameters,
     * when the estimator is hoisted out of the epoch loop.  Empties the layout
     * (@ref index_ / @ref columns_), the shared covariance, the sum-to-zero phase
     * gauges, the DD priors and the per-epoch flags, and keeps the underlying
     * @ref NetworkNormalEquations station-block pool, so the per-station Schur
     * buffers are reused across epochs instead of being freed and reallocated.
     * For the layout and the per-epoch state the effect equals default-constructing
     * a new @c NetworkEstimator.  Everything else is kept: the settings made
     * through the setters and passthroughs (thread budgets, rank-gate tolerance,
     * kernel options) and the attribution recorder.
     */
    void reset();

    /// Open a fresh arrowhead system over the current shared layout.  Freezes the
    /// layout for this epoch: registration throws from here until the next
    /// @ref reset.  Reuses the kernel's retained allocations (see
    /// @ref NetworkNormalEquations::reset).
    void beginEpoch();

    /**
     * @brief Add one station's pre-formed linearized rows for this epoch.
     * @param A_s Jacobian wrt the shared columns (@c m × @ref numShared).
     * @param A_r Jacobian wrt this station's private nuisances (@c m × n_local).
     * @param W   SPD measurement weight (@c m × @c m).
     * @param y   Prefit residual (length @c m).  Any pivot-receiver datum pins
     *            local to this station are folded into @p A_r / @p W / @p y by
     *            the caller (tight prior rows).
     * @param prior Optional diagonal prior rows folded into this station's
     *            @f$N_{rr}/u_r@f$ before the positive-definiteness test, instead
     *            of afterwards through @ref addStationPrior
     *            (@c network.station_prior_before_pd).  See
     *            @ref NetworkNormalEquations::StationPrior.  Empty (the default)
     *            keeps the legacy path, byte-identical.  A row passed here must
     *            not also be passed to @ref addStationPrior: the two paths are
     *            alternatives for one prior.
     * @return The 0-based station index for @ref reconstructStation.
     * @throws std::logic_error if called before @ref beginEpoch or after
     *         @ref solve (start a new epoch with @ref beginEpoch first).
     */
    std::size_t addStation(const Eigen::MatrixXd& A_s, const Eigen::MatrixXd& A_r,
                           const Eigen::MatrixXd& W, const Eigen::VectorXd& y,
                           const NetworkNormalEquations::StationPrior& prior =
                               NetworkNormalEquations::StationPrior{});

    /// Overload with a diagonal weight (per-observation variances⁻¹).
    std::size_t addStation(const Eigen::MatrixXd& A_s, const Eigen::MatrixXd& A_r,
                           const Eigen::VectorXd& w_diag, const Eigen::VectorXd& y,
                           const NetworkNormalEquations::StationPrior& prior =
                               NetworkNormalEquations::StationPrior{});

    /**
     * @brief Set the thread budget for the deterministic-parallel DD carry-fold
     *        (@c network.parallel_seam): the per-block fan-out of
     *        @ref NetworkNormalEquations::foldReducedDdPriors.
     *
     * Default 1 (serial fold, byte-identical).  Unlike the threaded branch of
     * @ref NetworkNormalEquations::reduce, this keeps the output byte-identical at any
     * thread count: the per-block algebra
     * is computed in parallel but accumulated serially in block-index order.
     * Takes effect on the next @ref solve.
     *
     * @param n_threads Thread count (clamped to ≥ 1).
     */
    void setSeamThreads(int n_threads) {
        seam_threads_ = n_threads > 1 ? n_threads : 1;
    }

    /**
     * @brief Arm @c network.parallel_carry: run @ref solve's DD-prior fold
     *        (@ref NetworkNormalEquations::foldReducedDdPriors) serially over blocks,
     *        with per-station threading inside each block (@p nthreads).
     *
     * The fold's dominant cost is the per-station reduction (measured at about 44 %
     * of the epoch), which Eigen's GEMM threading does not speed up.
     * @ref foldReducedDdPriors fans it across threads instead, with per-thread
     * partials combined in ascending order (ULP-level differences against the
     * serial fold), and column-parallelizes the K×K middle inverse (byte-identical).
     * Eigen stays pinned to one thread (the driver does not un-pin it around
     * @ref solve when this is armed), so the per-station OpenMP does not nest with
     * Eigen's threading.  When armed, @ref solve passes block_threads=1 and
     * station_threads=@p nthreads to the fold, in place of the @ref setSeamThreads
     * budget.
     * @param nthreads Within-block thread budget (≤ 1 turns it off: the serial or
     *                 seam fold path).
     */
    void setCarryParallelThreads(int nthreads) { carry_par_threads_ = nthreads > 1 ? nthreads : 1; }

    /**
     * @brief Set the resolved relative eigenvalue tolerance of the shared-border
     *        null-space rank gate (@c network.rank_gate_tol): the one
     *        value every consumer reads.
     *
     * Default @ref kNullSpaceTol (1e-9), so an unset config is byte-identical: the
     * same double goes through the same arithmetic as the former compile-time
     * constant.  The driver calls this once, next to the other estimator knobs,
     * before the epoch loop.
     *
     * @par One value, three consumers
     * The gate (@c NetworkSbasis::assertNullSpaceRank), the estimability dump
     * (@ref dumpEstimabilitySpectrum) and the prioritized drop-and-heal
     * (@c healNearNullPhaseBias) must agree: the dump describes the gate's
     * decision, and the drop-and-heal soft-anchors only directions the gate itself
     * counts as null.  At a looser cutoff than the gate, it would put prior
     * information on weak but real parameters, the near-cutoff fragility that the
     * CB-ADM rejected-alternatives list rules out.  One resolved member
     * makes that agreement structural rather than a convention.
     *
     * @par Placement rule (binding for any non-default value)
     * A non-default tolerance is justified from that config's own estimability
     * dump: the cutoff @p tol · max(λmax, 1) sits at least 2 decades above the
     * config's gauge depth and at least 2 decades below its weakest real
     * direction.  The gate is an exact-equality test that throws in both
     * directions, so a tolerance below the config's own gauge depth stops
     * counting the declared gauges and withholds a correctly specified config
     * ("over-specified").  A value chosen without a measured spectrum violates
     * the spec: see docs/SPEC_beidou_w6_datum.md §2 (DG3).
     *
     * @param tol Relative eigenvalue tolerance (> 0).  The loader rejects a value
     *            that is not > 0 and raises one below 1e-16 to 1e-16, since a
     *            deeper cutoff counts no direction as null and turns the gate off.
     */
    void setRankGateTol(double tol) { rank_gate_tol_ = tol; }

    /// The resolved rank-gate tolerance (@ref setRankGateTol); @ref kNullSpaceTol
    /// until the driver sets it.  Read by the gate call sites, the estimability
    /// dump and the drop-and-heal; no consumer keeps a private tolerance.
    double rankGateTol() const { return rank_gate_tol_; }

    /// Relative-pivot bound of the dense border fold (@ref addSharedPriorDense and the
    /// refolds that share its terms): the correlation-scaled carried block is accepted
    /// when its smallest LDLT pivot is at least this fraction of the largest
    /// (@c network.dense_fold_kappa; the class default is the historical 1e-6, the config
    /// default 1e-12).  A consistent carry whose level is loose
    /// while its phase-visible combinations are phase-precise has correlations within
    /// about 1e-8 of one between a clock and its phase biases, which 1e-6 rejects (the fold
    /// then falls back to the diagonal carry for that epoch).  The
    /// absolute folded-information budget is unchanged.
    void setDenseFoldKappa(double k) { dense_fold_kappa_ = k; }
    /// The resolved dense-fold pivot bound (@ref setDenseFoldKappa).
    double denseFoldKappa() const { return dense_fold_kappa_; }

    /// Pin-aware station PD guard passthrough
    /// (@ref NetworkNormalEquations::setStationPdMedianScale;
    /// @c network.station_pd_pin_aware).  Configuration-level: set once before
    /// the epoch loop; survives @ref beginEpoch / @ref reset.
    void setStationPdMedianScale(bool on) { neq_.setStationPdMedianScale(on); }

    /// Sparse station normals passthrough
    /// (@ref NetworkNormalEquations::setSparseStationNormals;
    /// @c network.sparse_station_normals).  Configuration-level: set once before
    /// the epoch loop; survives @ref beginEpoch / @ref reset.
    void setSparseStationNormals(bool on) { neq_.setSparseStationNormals(on); }

    /// Passthrough for the station PD null-direction read
    /// (@ref NetworkNormalEquations::setStationPdNullTrace;
    /// @c network.trace_station_pd_null).  Configuration-level: set
    /// once before the epoch loop; survives @ref beginEpoch / @ref reset.  The
    /// record itself is read through @ref normals() →
    /// @ref NetworkNormalEquations::lastStationPdFailure in the caller's @c catch.
    void setStationPdNullTrace(bool on) { neq_.setStationPdNullTrace(on); }

    /**
     * @brief Solve the shared satellite parameters under the explicit @p datum.
     *
     * Reduces the arrowhead, folds the sum-to-zero phase gauges and DD priors set
     * for this epoch (if any), then applies the datum pins to a copy of the
     * reduced shared system and solves it through the kernel's
     * `solveSharedConstrained` seam.  Each pin is exact (Dirichlet elimination):
     * the pinned value's contribution moves to the right-hand side, the pinned row
     * and column are zeroed, and the diagonal is set to the largest diagonal
     * magnitude of the reduced normals (at least 1).  That scale keeps
     * x_col = value exact without distorting the relative pivots the
     * positive-definiteness guard inspects.  The kernel's own reduced normals
     * never carry the pins, so @ref reconstructStation is valid afterwards.
     *
     * @param datum The shared-block gauges (≥ the rank deficiency of the reduced
     *        system).  An empty datum requires an already-full-rank reduced
     *        system (rare in practice).
     * @return The shared satellite-parameter estimate @c x_s.
     */
    Eigen::VectorXd solve(const Datum& datum);

    /**
     * @brief Apply the datum pins to a reduced shared system exactly as @ref solve does
     *        (Dirichlet elimination at the max-diagonal scale).
     *
     * Each pin's known contribution moves to the right-hand side (with the normals as
     * passed), then the pinned row and column are zeroed and the diagonal set to the
     * pin scale @f$s=\max(1,\max_i|N_{ii}|)@f$ with @f$u_{col}=s\cdot\mathrm{value}@f$, so
     * @f$x_{col}=\mathrm{value}@f$ exactly.  @ref solve and @ref solveHeldDetached share
     * this one implementation (it was moved verbatim out of @ref solve), so the held
     * solve is pinned exactly as the float one.
     *
     * @param[in,out] N     Reduced shared normals (square); pinned in place.
     * @param[in,out] u     Matching right-hand side; pinned in place.
     * @param         datum The pins.
     * @throws std::out_of_range when a pin column is outside @p N.
     */
    static void applyDatumPins(Eigen::MatrixXd& N, Eigen::VectorXd& u, const Datum& datum);

    /**
     * @brief The held solve of one epoch, beside the float one.
     *
     * Filled by @ref solveHeldDetached.  Per-block vectors are in input block order;
     * a degenerate block has an empty @c kappa and a NaN @c chi2 and @c chi2_expect, an
     * empty block an empty @c kappa and zeros.
     *
     * @par The contract on x_s
     * @c x_s is the held estimate only when @c ok; otherwise it is @c x_float, bitwise,
     * whatever was computed (a refused float reproduction, a held estimate that is not
     * finite, nothing folded).  A reader of @c x_s therefore never receives a held vector
     * built on a cache that did not produce the float.  When @c ok is false the
     * statistics are diagnostic only.
     *
     * @par The two statistics
     * The blocks touch disjoint stations, but their float cycle values are correlated
     * through the shared border, so the identity @f$\kappa=S^{-1}v@f$ holds over all the
     * folded rows together, with @f$S=\mathrm{Var}(\text{float cycles})+P^z@f$ the joint
     * matrix: @c kappa is that joint vector, block by block, and @c chi2_joint is
     * @f$v^\top S^{-1}v@f$ over every folded row.  A block's own statistic
     * @f$v_b^\top S_{bb}^{-1}v_b@f$ is not @f$v_b^\top\kappa_b@f$ when another block
     * folds; @c chi2 holds the own statistic, from a held solve of the block alone
     * (one more shared solve per folded block, taken only when two or more fold).
     *
     * @par Their mean under right targets
     * With right targets @f$v\sim N(0,S-P^z)@f$, so each statistic has the mean
     * @f$K-\mathrm{tr}(S^{-1}P^z)@f$, which is @f$K@f$ only where the hold variance is
     * small against the float variance in every direction of the rows (at a hold of
     * 0.01 cycle on a settled L1/L2 pair it is about 0.6 K).  @c chi2_expect and
     * @c chi2_joint_expect state it every epoch, from
     * @f$S^{-1}=A^{-1}-X\Sigma_hX^\top@f$ (@f$X=A^{-1}G^\top@f$, @f$\Sigma_h@f$ the held
     * shared covariance with the pinned columns exact, @f$\mathrm{diag}(A^{-1})@f$ by
     * selected inversion), so a calibration is graded against it, not against
     * @f$K@f$.  Each statistic is an overall test: one wrong unit among @f$K@f$ rows
     * shifts its mean by that unit's non-centrality alone, while the bar grows with
     * @f$K@f$, so a single error that keeps the wide lane (a (1, 1) pair) is
     * invisible to it at network size.  A test per unit is the caller's (
     * continuation FCFB).
     */
    struct HeldSolve {
        bool   ok{false};                    ///< The held estimate is accepted: at least one block folded (non-degenerate, non-empty), the held estimate is finite and the float reproduced (@c float_repro_rel ≤ @ref kHeldFloatReproTol).
        Eigen::VectorXd x_s;                 ///< The held shared estimate when @c ok; otherwise @c x_float itself, bitwise.
        std::vector<Eigen::VectorXd> v;      ///< Per block: @f$z_{\mathrm{free}}-G^\top x_{\mathrm{float}}-\hat z@f$, the float cycle values minus their targets (m).
        std::vector<Eigen::VectorXd> kappa;  ///< Per block: @f$A^{-1}(z_{\mathrm{free}}-G^\top x_{\mathrm{held}}-\hat z)@f$, the block's part of the joint @f$S^{-1}v@f$ (m⁻¹).  The posterior cycle values are @f$\hat z+P^z\kappa@f$.
        std::vector<double> chi2;            ///< Per block: the block's own statistic @f$v_b^\top S_{bb}^{-1}v_b@f$ (@f$v_b^\top\kappa_b@f$ when it is the only block that folds; otherwise from a held solve of the block alone).
        std::vector<double> chi2_expect;     ///< Per block: the mean of @c chi2 under right targets, @f$K_b-\mathrm{tr}(S_{bb}^{-1}P^z_b)@f$.
        double chi2_joint{0.0};              ///< @f$v^\top S^{-1}v=\sum_b v_b^\top\kappa_b@f$ over every folded row (0 when nothing folded).
        double chi2_joint_expect{0.0};       ///< The mean of @c chi2_joint under right targets, @f$K-\mathrm{tr}(S^{-1}P^z)@f$ (0 when nothing folded).
        long long dof_joint{0};              ///< The number of folded rows, the degrees of freedom of @c chi2_joint.
        std::vector<char>   degenerate;      ///< Per block: 1 when it folded nothing (dependent rows, or the pivot screen on its hold refused it).
        std::vector<char>   dependent;       ///< Per block: 1 when its rows are linearly dependent (@ref NetworkNormalEquations::DetachedFoldBlock::dependent); implies @c degenerate.
        double float_repro_rel{0.0};         ///< The normwise backward error of @c x_float on the pinned unfolded copy of the cached reduced system, @f$\|N_px-u_p\|_\infty/(\|N_p\|_\infty\|x\|_\infty+\|u_p\|_\infty)@f$: round-off for the float this cache produced, whatever the conditioning; large when the cache no longer produced @c x_float.  Infinite when @c x_float is not finite.
        double max_shift_m{0.0};             ///< @f$\max|x_{\mathrm{held}}-x_{\mathrm{float}}|@f$ over the shared columns of the computed held estimate, also when @c ok is false (0 when nothing folded; infinite when the held estimate is not finite).
        long long factor_nnz{0};             ///< Sum of the blocks' factor nonzeros (cost telemetry).
    };

    /// Bar on @ref HeldSolve::float_repro_rel above which the held solve is not @c ok:
    /// the cached reduced system did not produce the float passed in.  A
    /// backward-stable solve leaves a normwise backward error of order the dimension
    /// times the unit round-off; the bar leaves six orders of magnitude above that.
    static constexpr double kHeldFloatReproTol = 1e-8;

    /**
     * @brief Fold blocks of fixed integer cycles onto a copy of the last solve's reduced
     *        normals, pin with @p datum and solve once; the estimator is not modified
     *.
     *
     * The steps: (1) copy the cached reduced normals and RHS, pin the unfolded copy with
     * @ref applyDatumPins and measure @c float_repro_rel from @p x_float; (2) form the
     * blocks' fold operands from the cached per-station factors
     * (@ref NetworkNormalEquations::foldCycleRowsDetached); (3) add @c dN and @c du to the
     * copy, pin it the same way and solve once through
     * @ref NetworkNormalEquations::solveSharedConstrained, with the held covariance for
     * the statistics' means; (4) per block, @c v and @c kappa, the joint statistic, each
     * block's own statistic (a solve of each folded block alone when two or more fold;
     * see @ref HeldSolve), their means under right targets, and the largest shift from
     * the float; (5) acceptance: @c ok, and @c x_s the held estimate only then.
     *
     * Fold-then-pin is exactly what @ref solve does with DD priors set
     * (@ref setDdPriors), so for rows with a diagonal @f$P^z@f$ the held estimate and the
     * posterior cycles @f$\hat z+P^z\kappa@f$ equal that path's solve and
     * @ref ddPriorPosteriorMean.  The reduced normals, the shared covariance and every
     * @ref reconstructStation stay those of the float solve: nothing here writes to the
     * estimator or its kernel.  The kernel's section timers
     * (@ref NetworkNormalEquations::sectionTimers) are restored on every exit, so with the
     * timers armed the held solves are neither timed nor counted there;
     * their cost is the caller's to time.
     *
     * @param datum   The datum passed to the float @ref solve.
     * @param x_float The float shared estimate that @ref solve returned.
     * @param blocks  Blocks of cycle rows over disjoint station sets (one per connected
     *                station component).
     * @return The held solve; @c x_s is @p x_float, bitwise, and @c ok false when
     *         @p blocks is empty, no block folds, the held estimate is not finite or the
     *         float is not reproduced.
     * @throws std::logic_error before a @ref solve, when the kernel's reduction is not
     *         current, or when the kernel holds folded DD priors or sum-to-zero
     *         gauges (read from the kernel's state, not from the request lists, which
     *         may change after the solve); std::invalid_argument on an @p x_float size
     *         mismatch;
     *         whatever @ref NetworkNormalEquations::foldCycleRowsDetached throws (two
     *         blocks touching one station, a malformed row); std::invalid_argument when
     *         the folded, pinned system is not positive definite.
     */
    HeldSolve solveHeldDetached(const Datum& datum, const Eigen::VectorXd& x_float,
                                const std::vector<std::vector<NetworkNormalEquations::CycleRow>>& blocks) const;

    /**
     * @brief Inject a carried-ambiguity (phase-leveling) prior onto one station's
     *        local block; forwards to @ref NetworkNormalEquations::addStationPrior.
     *
     * Call after @ref addStation (the station must already be in this epoch's
     * arrowhead) and before @ref solve / @ref reducedSharedNormals: a local-block
     * change must be folded before the Schur reduction, and there is no
     * post-reduce fast path.  @p local_cols are columns within that station's
     * @c A_r; @p delta_Nrr_diag are the per-column information weights
     * (@f$1/\text{var}_\text{eff}@f$) and @p delta_ur the matching information-RHS
     * increments.  See the kernel method for the ordering/determinism contract.
     *
     * @throws std::logic_error if called before @ref beginEpoch or after @ref solve.
     */
    void addStationPrior(std::size_t station,
                         const std::vector<Eigen::Index>& local_cols,
                         const Eigen::VectorXd& delta_Nrr_diag,
                         const Eigen::VectorXd& delta_ur);

    /// Back-substitute a station's nuisance estimate given the shared solution.
    Eigen::VectorXd reconstructStation(std::size_t station,
                                       const Eigen::VectorXd& x_s) const;

    /**
     * @brief Conditional covariance of a station's nuisance block in the
     *        datum-as-known frame (producer-side network AR, Phase 1).
     *
     * Takes the most recent @ref solve's shared covariance (@ref
     * sharedCovariance), zeros the rows and columns of every @p datum-pinned
     * shared column and of every ensemble phase-gauge column
     * (@ref zeroPhaseGaugeColumns), since those gauges are exact and carry no
     * random uncertainty, and propagates the result into station @p station's
     * locals via @ref NetworkNormalEquations::reconstructStationCovariance.  The
     * returned block is @f$\mathrm{Cov}(x_r\mid x_s^{\text{free}})@f$: the
     * covariance the between-station DD ambiguities are differenced from before
     * the integer search.
     *
     * @param station Index from @ref addStation.
     * @param datum   The same datum passed to @ref solve (its pinned columns).
     * @return The station's local covariance (@c n_local × @c n_local); empty if
     *         the station has no locals.  Requires a prior @ref solve.
     */
    Eigen::MatrixXd conditionalLocalCovariance(std::size_t station,
                                               const Datum& datum,
                                               bool carry_aware = false) const;

    /**
     * @brief Joint conditional covariance of local parameters drawn from one or
     *        more stations, in the datum-as-known frame: the between-station
     *        double-difference ambiguity covariance primitive (network AR, Phase 2).
     *
     * Zeros the @p datum-pinned columns (and any ensemble phase-gauge columns) of
     * the last @ref solve's @ref sharedCovariance, then forwards to
     * @ref NetworkNormalEquations::jointConditionalCovariance.  The @c (a,b) entry
     * is @f$\mathrm{Cov}(x^{(i)}_{c_a}, x^{(j)}_{c_b}\mid x_s^{\text{free}})@f$.
     * Without @p carry_aware, cross-station entries carry only the shared-parameter
     * propagation, the coupling that makes a between-station DD covariance correct.
     *
     * @param requests @c (station, local_col) pairs.
     * @param datum    The same datum passed to @ref solve.
     * @param carry_aware Include the folded DD priors' rank-K term
     *                 (@ref NetworkNormalEquations::hasCarryCovCorrection).  @c false
     *                 (the default) is the earlier expression, unchanged.  With it
     *                 on, cross-station entries also carry the rank-K downdate, which
     *                 couples every column inside a DD block; a chord spans two
     *                 stations by construction.
     * @return The @c k×k joint conditional covariance; empty if @p requests is
     *         empty.  Requires a prior @ref solve.
     */
    Eigen::MatrixXd jointConditionalCovariance(
        const std::vector<std::pair<std::size_t, Eigen::Index>>& requests,
        const Datum& datum, bool carry_aware = false) const;

    /**
     * @brief Only the blocks of @ref jointConditionalCovariance (uncorrected form), in
     *        the same datum-as-known frame (block search).  See
     *        @ref NetworkNormalEquations::blockConditionalCovariances.
     * @param requests (station, local column) pairs.
     * @param datum    The S-basis pins.
     * @param blocks   Each a list of indices into @p requests.
     * @return One symmetric matrix per block.  Requires a prior @ref solve.
     */
    std::vector<Eigen::MatrixXd> blockConditionalCovariances(
        const std::vector<std::pair<std::size_t, Eigen::Index>>& requests,
        const Datum& datum, const std::vector<std::vector<int>>& blocks) const;

    /**
     * @brief Diagonal conditional variances for a subset of a station's local
     *        columns in the datum-as-known frame (the carried-ambiguity harvest).
     *
     * Like @ref conditionalLocalCovariance but returns only the diagonal entries
     * for @p local_cols, computed without the full per-station inverse: the cheap
     * per-epoch harvest the phase-leveling carry needs to seed each ambiguity's
     * next-epoch prior variance.  Zeros the @p datum-pinned shared columns and the
     * ensemble phase-gauge columns of the last solve's @ref sharedCovariance
     * (exact gauges carry no random uncertainty), then forwards to
     * @ref NetworkNormalEquations::reconstructStationCovarianceDiag.
     *
     * @param station    Index from @ref addStation.
     * @param local_cols Local columns whose conditional variance is wanted.
     * @param datum      The same datum passed to @ref solve (its pinned columns).
     * @return One conditional variance per requested column (empty if the station
     *         has no locals or @p local_cols is empty).  Requires a prior @ref solve.
     */
    Eigen::VectorXd conditionalLocalVarianceDiag(
        std::size_t station, const std::vector<Eigen::Index>& local_cols,
        const Datum& datum, bool carry_aware = false) const;

    /**
     * @brief Border-exact conditional variance diagonal: @f$\mathrm{diag}
     *        (N_{rr}^{-1})@f$ restricted to @p local_cols, i.e. the local columns'
     *        uncertainty relative to the shared frame, with the entire border
     *        treated as known (@f$\Sigma_{ss}\equiv 0@f$).
     *
     * The anchor-handover ledger re-validation uses this form
     * (SPEC_pivot_visibility_gauge_defect §5.09.1): the shared phase-bias
     * column's own uncertainty moves the pin target and the realized frame
     * together, so it is common-mode in the handover step, and a gate on the
     * border-inclusive marginal (@ref conditionalLocalVarianceDiag) measures the
     * wrong quantity in both directions.  Requires a prior @ref solve.
     */
    Eigen::VectorXd pureConditionalLocalVarianceDiag(
        std::size_t station, const std::vector<Eigen::Index>& local_cols) const;

    /**
     * @brief Regression of one local column on the shared border and its pure
     *        conditional variance: the ingredients of the rigorous between-epoch
     *        conditional carry (@c network.carry_conditional_coupling).
     *
     * Thin forward to @ref NetworkNormalEquations::stationRegression: returns
     * @f$M_r=N_{sr}N_{rr}^{-1}e_r@f$ (the shared-border regression row) and
     * @f$(N_{rr}^{-1})_{rr}=\mathrm{Var}(x_r\mid x_s)@f$ (the conditional variance
     * given the whole shared border, distinct from the marginal that
     * @ref conditionalLocalVarianceDiag returns).  Both are pure functions of the
     * accumulated normals and do not depend on the datum, so no @ref Datum
     * argument is taken; the driver restricts @f$M_r@f$ to the free
     * (non-datum-pinned) shared columns itself.  Requires a station already added
     * this epoch; a solve is not required, since the cached @f$N_{rr}@f$ factor
     * exists from @ref addStation.
     *
     * @param station   Index from @ref addStation.
     * @param local_col Local column within that station's @c A_r block.
     * @param cross_cols Optional local columns at which the same
     *                  @f$N_{rr}^{-1}e_r@f$ solve is also read (the conditional cross
     *                  covariances; F3, network.phase_leveling_local_block), forwarded
     *                  unchanged; see @ref NetworkNormalEquations::stationRegression.
     * @return @ref NetworkNormalEquations::StationRegression.
     */
    NetworkNormalEquations::StationRegression stationRegression(
        std::size_t station, Eigen::Index local_col,
        bool carry_aware = false,
        const std::vector<Eigen::Index>* cross_cols = nullptr) const;

    /**
     * @brief Baarda per-row redundancy numbers for one station's measurement rows
     *        (B2 network RAIM standardization), in the datum-as-known frame.
     *
     * Zeros the @p datum-pinned columns and the ensemble phase-gauge columns of the
     * last @ref solve's @ref sharedCovariance (exact gauges carry no random
     * uncertainty), then forwards to @ref NetworkNormalEquations::rowRedundancy.
     * Returns @f$r_i = 1 - w_i\,a_i^\top\mathrm{Cov}(\theta)\,a_i \in [0,1]@f$ per
     * row, with @f$a_i = [\,@c A_s_rows.row(i)\mid@c A_r_rows.row(i)\,]@f$.
     * Requires a prior @ref solve.  A low value marks a lightly checked measurement
     * whose residual barely responds to a fault; such a row needs the
     * @f$/\sqrt{r_i}@f$ standardization to be flaggable.
     */
    Eigen::VectorXd rowRedundancy(std::size_t station,
                                  const Eigen::MatrixXd& A_s_rows,
                                  const Eigen::MatrixXd& A_r_rows,
                                  const Eigen::VectorXd& w_rows,
                                  const Datum& datum) const;

    /// @return The kernel's reduce-once instrumentation
    ///         (@ref NetworkNormalEquations::reduceRecomputeCount) for the current
    ///         epoch, which shows whether the carried prior forced a second reduction.
    std::size_t reduceRecomputeCount() const { return neq_.reduceRecomputeCount(); }

    /// @name Tier D0 section-timer passthroughs (@c network.trace_section_timers)
    ///
    /// Arm/reset/read the kernel's per-section wall-clock accumulators (assembly /
    /// reduce / solve / shared-covariance inverse / per-station covariance
    /// consumers).  Configuration-level: @ref setPerfTiming once before the epoch
    /// loop (survives @ref reset / @ref beginEpoch, like @ref setStationPdMedianScale).
    /// Off (default) ⇒ no clock reads, numerically byte-identical.  See
    /// @ref NetworkNormalEquations::SectionTimers.
    /// @{
    void setPerfTiming(bool on) { neq_.setPerfTiming(on); }
    void resetSectionTimers() const { neq_.resetSectionTimers(); }
    const NetworkNormalEquations::SectionTimers& sectionTimers() const {
        return neq_.sectionTimers();
    }
    /// @}

    /**
     * @brief Covariance of the shared estimate from the most recent @ref solve.
     *
     * The inverse of the datum-constrained reduced shared normals, i.e. the formal
     * covariance of @c x_s.  A datum-pinned column carries only the small
     * artificial variance 1/s of the pin scale s used by @ref solve, and no
     * cross-covariance; the datum-frame readouts zero it.  Used by the network driver's between-epoch
     * carry-forward (the W4-S2b D2 prior): the posterior (value, variance) per
     * shared parameter seeds the next epoch's prior via @ref
     * NetworkNormalEquations::addSharedInfo.  Empty until @ref solve is called.
     */
    const Eigen::MatrixXd& sharedCovariance() const { return shared_cov_; }

    /**
     * @brief Zero the rows and columns of every folded ensemble phase-bias gauge
     *        column in a shared-covariance matrix, treating each
     *        @f$\sum_r z^s_r=0@f$ gauge as an exact datum (variance 0).
     *
     * The anchor-free ensemble datum fixes each satellite phase bias after the
     * reduction through the rank-1 @f$g g^\top/\beta@f$ fold
     * (@ref NetworkNormalEquations::foldReducedSumZeroGauges; Fix A,
     * covariance-carry consistency).  A gauge's shared column is the gauge (null)
     * direction of the un-reduced normals, so its slot of @ref sharedCovariance
     * holds the fold's artificial gauge-direction variance and phase↔clock/code
     * cross terms, not a physical uncertainty.  Any consumer that propagates the
     * shared covariance across epochs (the driver's D2 carry) or into the station
     * locals (the datum-frame conditional covariance) treats those columns as
     * exact, in the same way as the datum-pinned columns.  Otherwise the gauge
     * covariance over-tightens the D2 phase-bias prior, which biases the coupled
     * clock, and inflates the conditional ambiguity variance the phase-leveling
     * carry seeds from, which makes the carry inert.  The ensemble reference then
     * re-realizes every epoch, the emitted phase bias churns, and the co-estimated
     * SH-VTEC ionosphere absorbs the churn and feeds it back, driving the clocks
     * and biases into exponential runaway.  No-op (byte-identical) when no
     * ensemble gauge is folded (the anchor / off path).
     *
     * @param cov In/out shared covariance (@c numShared × @c numShared).  A gauge
     *            column outside its bounds is skipped, so a matrix with no gauge
     *            column in range is left untouched.
     */
    void zeroPhaseGaugeColumns(Eigen::MatrixXd& cov) const;

    /**
     * @brief The reduced shared normals @f$\tilde N_{ss}@f$ after Schur reduction
     *        (lazily reduces).  The W5 null-space-gate seam: inspect the reduced
     *        system before @ref solve folds the DD priors and applies the datum
     *        pins (any sum-to-zero phase gauges are already folded).  Non-const:
     *        updates the kernel's internal reduction cache (idempotent; @ref solve
     *        reuses it).
     */
    const Eigen::MatrixXd& reducedSharedNormals();

    /// Default relative eigenvalue tolerance of the shared-border null-space rank
    /// gate (relative to the largest λ of the reduced shared normals).  The
    /// reduced system is ill-conditioned by construction: phase pins the
    /// clock/phase-bias sum at ~1e5 weight while code alone separates the split,
    /// so the weakest real direction sits about 7 decades above the true gauges
    /// (gauges σ ~1e-10 of σmax, weakest real direction σ ~1e-2).  The 1e-6
    /// library default of NetworkSbasis::assertNullSpaceRank sits above the weak
    /// but real directions and miscounts them; 1e-9 lands in the GPS+Galileo gap.
    ///
    /// The tolerance is a calibration parameter.  This class constant
    /// (INC-8) is the single source of the default, so an unset
    /// @c network.rank_gate_tol is byte-identical, and the three consumers read
    /// @ref rankGateTol.  BeiDou is the reason: BDS adds a weak but real family
    /// layer at ≈1.7e-10 of λmax, about three decades deeper (relatively) than the
    /// GPS+Galileo weakest real direction, so 1e-9 miscounts it as 1e-6 did
    /// before.  The measured 305-station spectrum has a six-decade gap between
    /// that layer (~1e-3 absolute) and the declared clock gauges (~1e-9 absolute);
    /// a per-config value placed in that gap again counts only the gauges.  See
    /// @ref setRankGateTol for the binding placement rule and
    /// docs/SPEC_beidou_w6_datum.md §0.2.
    static constexpr double kNullSpaceTol = 1e-9;

    /// One reported near-null direction of an @ref EstimabilityDump: its
    /// eigenvalue plus the (column label, |component|) pairs of the ≤ 5
    /// heaviest-spanning shared columns; the machine-readable twin of the
    /// "dir spanned by [...]" warn line.
    struct EstimabilityDirection {
        double ev{0.0};                                    ///< eigenvalue of the direction
        std::vector<std::pair<std::string, double>> top;   ///< ≤ 5 (col_desc label, |component|), heaviest first
    };

    /// Machine-readable mirror of one @ref dumpEstimabilitySpectrum call, so the
    /// storm analyzer (scripts/characterize_storm.py) can read a CSV instead of
    /// re-parsing warn text.  Observational only: the struct is filled from the
    /// same numbers the log lines print, in the same order, and filling it changes
    /// neither the log text nor the solution.
    struct EstimabilityDump {
        bool   valid{false};        ///< false ⇒ the eigensolve did not converge (nothing was logged either)
        double tow{0.0};            ///< epoch GPS time-of-week
        int    n_shared{0};         ///< reduced shared-normal dimension
        int    declared_dim{0};     ///< declared S-basis dimension for this epoch
        double ev_max{0.0};         ///< largest eigenvalue λmax
        double cutoff{0.0};         ///< the absolute cutoff applied = rankGateTol() · max(λmax, 1)
        int    n_below{0};          ///< directions with ev < cutoff (what the gate counts)
        /// The dumped tail (n_below + 2 directions, at most n_shared), largest ev
        /// first: the order of the warn lines, so a CSV row and a log line match 1:1.
        std::vector<EstimabilityDirection> dirs;
    };

    /**
     * @brief One-shot estimability diagnostic (network.trace_estimability):
     *        dump the tail of the reduced shared-normal eigen-spectrum and name
     *        the parameters spanning each near-zero (unpinned-gauge) direction
     *        (INC-8).
     *
     * Shows where the null space sits relative to the declared datum dimension.
     * A clean gap of about 7 decades after exactly @p declared_dim eigenvalues
     * points to a mis-set tolerance (@ref rankGateTol); one or more extra tiny
     * eigenvalues above the gap point to a genuinely unpinned direction (a
     * missing datum constraint).  The reduced shared normals are symmetric
     * positive-semidefinite (a Schur complement of weighted Gram normals), so
     * their eigenvalues equal their singular values: a self-adjoint eigensolve
     * yields the exact spectrum the rank gate tests at a small fraction of a full
     * JacobiSVD's cost.  Naming the gauge parameters still needs the
     * eigenvectors, so this is a full decomposition; the caller keeps it behind
     * the trace_estimability knob and its first-N latch.  Non-const:
     * @ref reducedSharedNormals lazily reduces (idempotent).
     *
     * @param tow          Epoch GPS time-of-week (log stamp).
     * @param declared_dim Declared datum dimension (NetworkSbasis::Plan).
     * @param col_desc     Driver-format column labels ("CLK|sat", "PB|sat|sig",
     *                     "CB|sat|sig", "SH|n|m|trig") built by the Assembly
     *                     column registration when armed; missing or empty
     *                     entries fall back to "colN".  Intentionally not
     *                     @ref sharedColumnLabel: the formats differ, and the
     *                     trace text is golden-gated.
     * @param log          Main logger: one warn line with the spectrum tail, then
     *                     one per dumped direction.
     * @return The same spectrum in machine-readable form (@ref EstimabilityDump;
     *         @c valid=false iff the eigensolve failed, in which case nothing was
     *         logged either).  The caller writes it to @c network_estimability.csv
     *         under @c network.trace_estimability (DG5).  Filling it leaves the
     *         log text unchanged; the estim arm golden-gates that text.
     */
    EstimabilityDump dumpEstimabilitySpectrum(double tow, int declared_dim,
                                              const std::vector<std::string>& col_desc,
                                              const std::shared_ptr<spdlog::logger>& log);

    /// A carried-forward / regularizing prior on one shared parameter (W4-S2b D2
    /// time update; also the SH-VTEC Kaula/temporal prior).
    struct SharedPrior {
        SharedParamKind kind;     ///< parameter family
        std::string     sat;      ///< satellite id (empty for VtecSh)
        std::string     signal;   ///< signal (empty for Clock / VtecSh)
        int             sh_n{-1};    ///< VtecSh degree (VtecSh only)
        int             sh_m{-1};    ///< VtecSh order  (VtecSh only)
        char            sh_trig{0};  ///< VtecSh 'C'/'S' (VtecSh only)
        int             sh_layer{0}; ///< VtecSh shell index (VtecSh only; 0 = legacy layer)
        double          value{0.0};  ///< prior mean
        double          info{0.0};   ///< information weight = 1 / prior-variance (>0)
        /// Clock clone: the prior is on the clock clone column of @c sat
        /// (@ref registerClockClone, key @c "sat|CLKC"), with @c kind Clock; border clone: on the
        /// clone of (kind, sat, signal) (@ref registerClone).  False (the default) = the
        /// column of (kind, sat, signal), as before.
        bool            clone{false};
        /// Shared history: the prior is on shared-history column @c hist_idx
        /// (@ref registerHistoryColumn, key @c "HIST|j"), with @c kind Clock and an empty
        /// @c sat.  False (the default) = as before.
        bool            hist{false};
        int             hist_idx{-1};   ///< the history column's index j (with @c hist)
    };

    /**
     * @brief Inject diagonal carried-forward priors onto the shared border (the
     *        D2 between-epoch time update; forwards to
     *        @ref NetworkNormalEquations::addSharedInfo).
     *
     * Identity-keyed by (sat, kind, signal): a prior whose parameter is not in the
     * current epoch's layout (the satellite set) is skipped.  Call after
     * @ref beginEpoch and before @ref solve.
     *
     * @see addSharedPriorDense for the C1 full-covariance form of the same carry.
     */
    void addSharedPrior(const std::vector<SharedPrior>& priors);

    /**
     * @brief Inject one dense full-covariance carried-forward prior onto the
     *        shared border (the C1 cross-epoch carry; forwards to
     *        @ref NetworkNormalEquations::addSharedInfo as a single symmetric
     *        information block).
     *
     * Folds @f$\Lambda = P^{-1}@f$ and @f$\eta = \Lambda\,v@f$ over the carried
     * keys, where @p cov is the previous epoch's posterior covariance block plus
     * process noise (P_prev + Q, per-key surgeries already applied by the caller)
     * and @c v the carried values: the information-form Kalman time and prior
     * update on the shared border.  Identity-keyed like @ref addSharedPrior, except
     * for the absent-key policy: a key not present in this epoch's layout is
     * marginalized, i.e. its rows and columns of @p cov are dropped before the
     * inversion.  Dropping rows of Λ instead would condition on
     * the departed key and bias every surviving parameter, which is why the API
     * takes a covariance rather than an information matrix.
     *
     * Numerics: the retained block is symmetrized and inverted in correlation
     * scaling (@f$P = D C D,\ \Lambda = D^{-1} C^{-1} D^{-1}@f$, @f$D=\sqrt{\mathrm
     * {diag}\,P}@f$) via LDLT with a relative-pivot PD guard and one @c 1e-9 ridge
     * retry on the unit diagonal, so blocks mixing stiff zero-q phase-bias carries
     * (~1e-9 m²) with loose clock carries (~1e-2 m²) stay well-scaled.  The folded
     * Λ is explicitly symmetrized so the pre-reduce (within-epoch RAIM replay) and
     * post-reduce (primary fold fast path) kernel paths agree bit-for-bit.
     *
     * @param keys        Identity + carried value per entry (@ref SharedPrior;
     *                    the @c info field is ignored, the weight comes from
     *                    @p cov).  Order defines the row/column order of @p cov.
     * @param cov         @c keys.size() × @c keys.size() prior covariance
     *                    (P_prev + Q).  Must be finite with positive diagonal on
     *                    every retained key (caller pre-screens per-key variance).
     * @param lambda_diag Optional out: resized to @c keys.size(); the folded
     *                    @f$\Lambda(c,c)@f$ per input key (0 for marginalized
     *                    keys), which feeds the B7 anchor-information trace.
     * @return Number of keys retained (present in this epoch's layout).
     * @throws std::invalid_argument on a dimension mismatch.
     * @throws std::logic_error unless called between @ref beginEpoch and
     *         @ref solve (guarded, unlike the legacy diagonal overload).
     * @throws std::runtime_error when the retained block is non-finite, has a
     *         non-positive retained variance, stays non-PD after the ridge retry,
     *         or folds more information than the conditioning budget allows (a
     *         diagonal of Λ above 1e13 m⁻²).  The caller (driver) owns the
     *         fallback policy: degrade to the diagonal carry for the epoch.
     */
    /// @param clock_gauge_free Marginalize the per-constellation satellite-clock
    ///        common direction out of the folded block before it is applied
    ///        (the temporal clock anchor, §2.1; see @ref addSharedPriorClockGaugeFree for the algebra
    ///        and the rationale).  Applied to the joint Λ, so the clock↔code-bias
    ///        off-diagonals that the C1 dense carry exists to retain are preserved;
    ///        only the gauge direction's information is removed.
    std::size_t addSharedPriorDense(const std::vector<SharedPrior>& keys,
                                    const Eigen::MatrixXd& cov,
                                    Eigen::VectorXd* lambda_diag = nullptr,
                                    bool clock_gauge_free = false);

    /// One clone's walk factor (a border clone for a bias): the pseudo-observation
    /// @f$x(t) - x(t-1) = \text{mean}@f$ with variance @c q, between the key's own column
    /// and its clone column.  The defaults (@c kind Clock, empty @c signal) are a clock's.
    struct CloneWalk {
        std::string sat;    ///< satellite id (its key and its clone must both be registered)
        SharedParamKind kind{SharedParamKind::Clock};   ///< border clone: the key's kind (Clock, PhaseBias or CodeBias)
        std::string signal; ///< border clone: the key's signal (empty for a clock)
        double mean{0.0};   ///< the factor's mean (m): the fold-time carried clock minus the clone's prior mean, 0 unless the carry moved after the rows were built
        double q{0.0};      ///< the walk's variance over the epoch (m^2): the fold's var_eff minus the stashed variance
        /// Across a clock-pivot handover the dense S-transform re-expressed, the
        /// clock column is in the incoming frame and the clones in the outgoing one, so the
        /// factor is @f$c_s(t) - c_s(t-1) + c_q(t-1) = \text{mean}@f$ with @c ref the incoming
        /// pivot q (its clone must be registered).  Empty (the default): no third term.
        std::string ref;
    };

    /**
     * @brief Fold (or withdraw) the walk factors of the clock clones.
     *
     * In information form: for each factor, @f$w=1/q@f$ is added to
     * @f$N_{cc}@f$ and @f$N_{kk}@f$ and subtracted from @f$N_{ck}@f$, @f$N_{kc}@f$ (clock
     * column c, clone column k), with @f$\pm w\,\text{mean}@f$ on the right-hand side.
     * Together with the stashed block folded over the stand-ins (each cloned clock stood
     * in for by its clone at the stashed variance, @ref addSharedPriorDense with
     * @ref SharedPrior::clone), this is the joint prior of the moved border and the clones
     * with covariance @f$[[P+Q, P], [P, P]]@f$ on each clock and its clone, formed without
     * inverting that ill-conditioned block.  A factor with a @c ref adds the
     * reference clone with coefficient +1: @f$a = e_c - e_k + e_r@f$, information
     * @f$a a^\top/q@f$, right-hand side @f$a\,\text{mean}/q@f$.  A factor whose clock, clone
     * or reference clone is absent from the layout, or whose @c q is not positive and
     * finite, is skipped.
     * @param sign +1 folds, -1 withdraws exactly what +1 folded (the temporal clock anchor's refloat, which
     *             may call it after @ref solve, as it calls @ref withdrawSharedPrior, and
     *             folds a factor back with +1 when its re-solve throws).
     * @return The number of factors applied.
     * @throws std::logic_error before @ref beginEpoch.
     */
    std::size_t addCloneWalk(const std::vector<CloneWalk>& walks, double sign = 1.0);

    /**
     * @brief Fold (or withdraw) the clones' walk factors as one joint factor with a full
     *        walk covariance (pivot-frame process noise on the clock clone,
     *        docs/refound/PFQ_pivot_frame_process_noise_2026-10-08.md section 6).
     *
     * With the pivot-frame clock noise (@c network.clock_pivot_frame_q) the walks of two
     * clocks of a constellation share the pinned pivot's walk, so the walk vector
     * @f$w = A x@f$ (row i of @f$A@f$ is @ref addCloneWalk's @f$a_i@f$ of walk i) has the
     * covariance @p Qw, not a diagonal.  The factor is the pseudo-observation
     * @f$A x = \text{mean}@f$ with that covariance: information @f$A^\top Q_w^{-1} A@f$ and
     * right-hand side @f$A^\top Q_w^{-1}\,\text{mean}@f$, times @p sign.  A walk that
     * @ref addCloneWalk would skip is dropped from the factor together with its row and
     * column of @p Qw.  With an empty @p Qw this is @ref addCloneWalk exactly, and with a
     * diagonal @p Qw equal to the walks' @c q it folds the same information.
     * @param walks the factors (their @c q is ignored when @p Qw is given).
     * @param Qw    the walks' covariance (m^2), symmetric positive definite over the walks
     *              that apply; empty selects the per-walk form.
     * @param sign  +1 folds, -1 withdraws exactly what +1 folded.
     * @return The number of walks in the factor.
     * @throws std::logic_error before @ref beginEpoch; std::runtime_error when the
     *         applicable block of @p Qw is not positive definite (nothing is folded).
     */
    std::size_t addCloneWalkJoint(const std::vector<CloneWalk>& walks, const Eigen::MatrixXd& Qw,
                                  double sign = 1.0);

    /**
     * @brief The posterior regression of the clock clones on the rest of the border
     *, the ingredient of the harvest's clone marginalization.
     *
     * With shared-history columns registered, the regressed set is
     * Y = [clones; history] (@ref numBorderNuisance rows, in column order), so @c K is
     * @f$K_Y = -J_{YY}^{-1}J_{YS'}@f$ and @c Bc is @f$\Sigma_{Y\mid s} = J_{YY}^{-1}@f$, the
     * ingredients of the history compression.  Off, Y is the clones alone (unchanged).
     *
     * After @ref solve, with @f$J@f$ the reduced shared normals the solve factored (every
     * prior folded, before the datum pins), @f$C@f$ the clone columns and @f$S'@f$ the
     * other columns outside @p datum: @f$B' = J_{CC}^{-1} = \mathrm{Var}(c\mid s',
     * \text{data})@f$ and @f$K = -B'J_{CS'}@f$, the coefficients of @f$E[c\mid s']@f$.
     * This is the information form of @f$K=\Sigma_{CS'}\Sigma_{S'S'}^{-1}@f$,
     * @f$B'=\Sigma_{CC}-K\Sigma_{S'C}@f$ over the solve's covariance, free of the
     * cancellation in the latter (B' is orders of magnitude below @f$\Sigma_{CC}@f$).  A
     * datum-pinned column is exactly known in the solve's frame: it gets no coefficient
     * (the conditional mean does not move with it), and the caller restores a row's
     * identity along the declared clock gauge on it (the harvest's clock-family deficit).
     * @param datum The datum the solve applied.
     * @param[out] K  @c numBorderNuisance × @ref numShared, zero on the clone, history and
     *                pinned columns.
     * @param[out] Bc @c numBorderNuisance square.
     * @return false (outputs untouched) when no clone or history column is registered, the epoch is not
     *         solved, or @f$J_{CC}@f$ is not positive definite.
     */
    bool cloneConditional(const Datum& datum, Eigen::MatrixXd& K, Eigen::MatrixXd& Bc) const;

    /**
     * @brief Fold diagonal shared priors with the per-constellation
     *        satellite-clock gauge direction marginalized out (temporal clock anchor, §2.1).
     *
     * ## Why
     *
     * The per-constellation clock common mode is a gauge: the S-basis fixes it
     * (the pivot-satellite pin), not the carried prior, and a consumer's
     * per-epoch receiver clock absorbs it identically.  A carry that puts
     * information on it competes with the datum, and the conflict lands on the
     * pivot's observation rows rather than on the estimable between-satellite
     * differences.  Measured on the wire (3 h, 63 stations), the anchored arm
     * reduced CLK-R's drained common mode Σ|μ| from 49.4 m to 27.8 m against its
     * control: the carry was doing 44 % of the gauge's work.
     *
     * ## The algebra
     *
     * Removing the prior's information along a direction (equivalently, giving
     * that direction infinite prior variance) is the rank-1 downdate
     * @f[ \Lambda_{gf} = \Lambda - \Lambda u (u^\top \Lambda u)^{-1} u^\top \Lambda,
     *     \qquad \eta_{gf} = \Lambda_{gf} v @f]
     * (the Sherman–Morrison limit of @f$(P + \sigma^2 uu^\top)^{-1}@f$ as
     * @f$\sigma^2 \to \infty@f$), with @f$u@f$ the indicator of one
     * constellation's carried clock keys.  @f$\Lambda_{gf} u = 0@f$ exactly, and
     * @f$\Lambda_{gf}@f$ stays PSD with rank one lower.  For the diagonal case
     * this reduces to anchoring each clock to its carried value minus the
     * information-weighted mean of the carried values, i.e. it anchors the
     * differences, which are the estimable content (Odijk datum-invariance; the
     * GKF "carry on functions, not states" form).
     *
     * A constellation contributing fewer than two carried clock keys has no
     * estimable content to anchor (its single key is the gauge), so nothing is
     * folded for it.  That is a valid outcome, not an error, and the caller
     * counts it.
     *
     * Non-clock priors in @p priors are folded exactly as @ref addSharedPrior
     * folds them.
     *
     * @return Number of clock keys whose information was downdated (0 ⇒ the
     *         gauge-free step was inert for this fold).
     */
    std::size_t addSharedPriorClockGaugeFree(const std::vector<SharedPrior>& priors);

    /**
     * @brief Gauge-free diagonal analogue of
     *        @ref refoldSharedPriorDenseDroppingClocks: re-fold a gauge-free
     *        clock prior set with @p drop_sats refloated.
     *
     * A gauge-free block is not diagonal, so @ref withdrawSharedPrior cannot undo
     * one key of it: removing a key also moves the group's information-weighted
     * mean, against which every survivor is anchored.  This rebuilds both blocks
     * (with and without the refloated keys) and folds the difference.  Only
     * @ref SharedParamKind::Clock entries of @p priors are touched.
     *
     * @param restore Fold the inverse delta: the exact undo, for the path where
     *                the re-solve throws and the anchored solve must survive.
     * @return Number of clock keys refloated (0 ⇒ nothing folded).
     */
    std::size_t refoldSharedPriorClockGaugeFreeDropping(
        const std::vector<SharedPrior>& priors,
        const std::set<std::string>& drop_sats, bool restore = false);

    /**
     * @brief Withdraw already-folded diagonal shared priors
     *        (docs/SPEC_clock_temporal_anchor.md §2.4) so the epoch can be
     *        re-solved with those states refloated.
     *
     * The exact inverse of @ref addSharedPrior for the listed (identity, value,
     * info) triples: it folds the negated increment through
     * @ref NetworkNormalEquations::addSharedInfo.  Intended to be called after
     * @ref solve.  A shared-border increment carries through the Schur reduction
     * unchanged, so the kernel's reduction-preserving fast path keeps the cached
     * reduced system current, and a following @ref solve costs the datum
     * application and one factorization of the reduced shared system, with no
     * re-reduction and no re-assembly.
     *
     * The caller passes the values and infos it folded, not a recomputation: the
     * withdrawal is arithmetic, not a lookup, and a mismatched info would leave
     * residual prior information behind.
     *
     * Withdrawing information can only make the reduced normals less definite.
     * The intended population (a data-supported satellite clock column carried by
     * tens of stations) cannot be driven singular this way, and the solve's own
     * relative-pivot guard is the backstop; the caller owns the policy on a throw
     * (restore the pre-withdraw solution).
     *
     * @param priors Identity + the folded value + the folded info of each prior
     *               to remove.  Entries with @c info <= 0, or whose parameter is
     *               absent from this epoch's layout, are skipped exactly as
     *               @ref addSharedPrior skips them.
     */
    void withdrawSharedPrior(const std::vector<SharedPrior>& priors);

    /**
     * @brief Re-fold a dense carried block with some keys marginalized, after
     *        @ref solve, so the epoch can be re-solved with those states refloated
     *        (the temporal clock anchor's refloat).
     *
     * The dense (C1) form of @ref withdrawSharedPrior.  Folds
     * @f$(\Lambda_{new} - \Lambda_{old})@f$ and
     * @f$(\eta_{new} - \eta_{old})@f$, where @f$\Lambda_{old}@f$ is the block
     * @ref addSharedPriorDense folded for @p keys / @p cov and @f$\Lambda_{new}@f$
     * is the same construction over the surviving keys, with the
     * @ref SharedParamKind::Clock rows and columns of @p drop_sats dropped from
     * @p cov before the inversion.  Marginalization, not conditioning, is the
     * exact treatment of a refloated key, the same policy this class applies to a
     * key that has left the layout.
     *
     * Both Λ are built by the private path @ref addSharedPriorDense uses, so a
     * re-fold that drops nothing is an exact algebraic no-op.
     *
     * @param keys      The keys the original fold used (order defines @p cov).
     * @param cov       The covariance the original fold used (P_prev + Q).
     * @param drop_sats Satellites whose clock key is to be marginalized (a clock clone
     *                  key of the same satellite is kept: the rows still
     *                  reference the clone).
     * @param restore   Fold the inverse delta (Λ_old − Λ_new) instead: the exact
     *                  undo of a previous call with the same arguments, used when
     *                  the re-solve throws and the caller must put the epoch's
     *                  shared system back the way it solved.
     * @return Number of clock keys actually marginalized (0 ⇒ nothing folded).
     * @throws std::invalid_argument on a dimension mismatch, and
     *         std::runtime_error under the block conditions of
     *         @ref addSharedPriorDense (either block non-finite, non-PD, or over
     *         the information budget).  Nothing is folded when it throws.
     */
    /// @param clock_gauge_free Must match the flag the original fold used, or the
    ///        withdrawn block is not the block that was applied.
    std::size_t refoldSharedPriorDenseDroppingClocks(
        const std::vector<SharedPrior>& keys, const Eigen::MatrixXd& cov,
        const std::set<std::string>& drop_sats, bool restore = false,
        bool clock_gauge_free = false);

    /**
     * @brief Fold an explicit linear gauge constraint onto the shared border.
     *
     * Adds the information form of @p weight·‖@p H x − @p z‖² to the shared
     * normals: @f$N_{ss} \mathrel{+}= w\,H^\top H,\ u_s \mathrel{+}= w\,H^\top z@f$
     * (via @ref NetworkNormalEquations::addSharedInfo).  The network driver uses it
     * to pin the satellite code-bias gauges the data leaves free: the
     * per-satellite ionosphere-free combination (so the emitted OSB is pure DCB
     * and its IF lump stays in the estimable clock) and the per-constellation DCB
     * sum-to-zero.  They go in as tight but finite rows folded before the
     * null-space gate, so the reduced shared system carries only the
     * per-constellation clock deficiency that the explicit @ref Datum pins.
     *
     * Because these rows pin pure gauge directions (zero data information), any
     * positive @p weight fixes them exactly without biasing the estimable
     * (between-satellite) DCB structure.  Call after @ref beginEpoch and before
     * @ref solve / the rank gate.
     *
     * @param H      Constraint Jacobian (@c k × @ref numShared).
     * @param z      Constraint target (length @c k; usually 0).
     * @param weight Row information weight (= 1/σ²) applied to every row.
     * @throws std::invalid_argument on a dimension mismatch.
     */
    void addSharedConstraintRows(const Eigen::MatrixXd& H, const Eigen::VectorXd& z,
                                 double weight);

    /**
     * @brief Set the anchor-free sum-to-zero phase-bias gauges for this epoch
     *        (Fix A; kept as a tested kernel after the removal of
     *        @c network.phase_datum_ensemble, its only production caller).
     *
     * The ensemble realization of the phase-bias↔ambiguity datum: instead of
     * pinning one anchor receiver's ambiguity per (satellite, signal), the gauge
     * is fixed by @f$\sum_r z^s_{r,\sigma}=0@f$ over the receivers tracking it.
     * There is no pivot, no handover and no single-anchor point of failure, and
     * every satellite is referenced to the same receiver ensemble, so the user's
     * between-satellite fractional bias cancels and its DD ambiguity stays integer
     * (Odijk S-system).  Each gauge's terms are (station index, local ambiguity
     * column); the caller (driver) builds them from its own @c A_r layout.  The
     * gauges are stored and folded through the kernel
     * (@ref NetworkNormalEquations::foldReducedSumZeroGauges) at @ref solve /
     * @ref reducedSharedNormals, after the Schur reduction and before the datum
     * pins / null-space gate.  An empty vector (the default state) selects the
     * legacy anchor-pin datum, and @ref reconstructStation and the reduced system
     * are then byte-identical.  Cleared by @ref reset.
     *
     * @param gauges One sum-to-zero gauge per (satellite, signal) shared phase bias.
     */
    void setPhaseSumZeroGauges(std::vector<NetworkNormalEquations::SumZeroGauge> gauges) {
        const ContributionKind ck = takeContributionKind_();
        const long long n = static_cast<long long>(gauges.size());
        phase_sumzero_gauges_ = std::move(gauges);
        if (n > 0) noteFold_(ck, n);
    }


    /**
     * @brief Set this epoch's rank-K double-difference priors from the z-store,
     *        folded onto the reduced system by
     *        @ref NetworkNormalEquations::foldReducedDdPriors (the M1
     *        invariant carry).
     *
     * One @c DdPriorBlock per connected (const,signal) component: the z-store's
     * carried DD ambiguities (mean + dense covariance) as gauge-free constraints on
     * the local ambiguity columns.  Folded at @ref solve after the Schur reduction
     * and before the W5 datum constraint.  Mutually exclusive with the sum-to-zero
     * ensemble gauges: both are post-reduce Woodbury folds reading the unmodified
     * per-station @f$N_{rr}@f$, and composing them over coupled columns drops the
     * cross term, so @ref solve throws std::logic_error if both are set.  The
     * phase datum is the pre-reduce tree pins (@ref addStationPrior), which
     * compose exactly with this fold.  Empty (the default) keeps the off path
     * byte-identical.  Cleared by @ref beginEpoch and by @ref reset, so the driver
     * sets them from the z-store after each @ref beginEpoch.
     *
     * @param blocks One rank-K DD-prior block per component.
     */
    void setDdPriors(std::vector<NetworkNormalEquations::DdPriorBlock> blocks) {
        const ContributionKind ck = takeContributionKind_();
        const long long n = static_cast<long long>(blocks.size());
        dd_priors_ = std::move(blocks);
        if (n > 0) noteFold_(ck, n);
    }

    /// Posterior mean of DD-prior block @p b given the solved shared estimate
    /// @p x_s: the value the z-store carries forward (forwards to the kernel).
    Eigen::VectorXd ddPriorPosteriorMean(std::size_t b, const Eigen::VectorXd& x_s) const {
        return neq_.ddPriorPosteriorMean(b, x_s);
    }

    /**
     * @brief Posterior covariance of DD-prior block @p b: the @f$P^z@f$ the z-store
     *        carries forward (M1 phase C3b).
     *
     * Builds the datum-as-known shared covariance @c cov_free from the last solve's
     * @c shared_cov_ (@ref datumFrameSharedCov: the datum-pinned columns zeroed as
     * exact gauges, the same frame as @ref conditionalLocalCovariance) and forwards
     * to @ref NetworkNormalEquations::ddPriorPosteriorCovariance.  The DD is
     * gauge-free, so the K×K result is datum-invariant.
     *
     * @param b     DD-prior block index (input order; see @ref ddPriorPosteriorMean).
     * @param datum The same minimum-constraint datum passed to @ref solve.
     */
    Eigen::MatrixXd ddPriorPosteriorCovariance(std::size_t b, const Datum& datum) const;

    /**
     * @brief Innovation + its H0 covariance for DD-prior block @p b (D5 gate).
     *
     * Builds the datum-as-known shared covariance @c cov_free from the last solve's
     * @c shared_cov_ (identical to @ref ddPriorPosteriorCovariance) and forwards to
     * @ref NetworkNormalEquations::ddPriorInnovation.  The caller (the invariant-carry
     * driver) standardizes @f$|v_i|/\sqrt{\mathrm{Cov}(v)_{ii}}@f$ to flag a slipped
     * carried DD and re-tap it fresh.
     *
     * @param b     DD-prior block index (input order; see @ref ddPriorPosteriorMean).
     * @param x_s   The solved shared estimate.
     * @param datum The same minimum-constraint datum passed to @ref solve.
     * @param[out] v     The K innovations (empty for a K=0 slot).
     * @param[out] cov_v The K×K H0 innovation covariance (empty for a K=0 slot).
     */
    void ddPriorInnovation(std::size_t b, const Eigen::VectorXd& x_s, const Datum& datum,
                           Eigen::VectorXd& v, Eigen::MatrixXd& cov_v) const;

    /**
     * @brief Build the datum-as-known shared covariance @c cov_free once.
     *
     * The @f$\Sigma_{ss}@f$ frame every DD-carry covariance readout consumes: the
     * last solve's @c shared_cov_ with the datum-pinned columns treated as exact
     * (variance 0) and the phase-gauge columns zeroed.  A per-epoch caller (the
     * invariant-carry driver's @ref NetworkInvariantCarryDriver::installPosterior)
     * builds it once and passes it to the matrix overloads below instead of
     * rebuilding the n_shared² copy per block per readout.  Byte-identical to the
     * per-call builds.
     */
    Eigen::MatrixXd datumFrameSharedCov(const Datum& datum) const;

    /// Matrix overloads that take a pre-built @ref datumFrameSharedCov (skip the copy).
    Eigen::MatrixXd ddPriorPosteriorCovariance(std::size_t b, const Eigen::MatrixXd& cov_free) const {
        return neq_.ddPriorPosteriorCovariance(b, cov_free);
    }
    void ddPriorInnovation(std::size_t b, const Eigen::VectorXd& x_s,
                           const Eigen::MatrixXd& cov_free,
                           Eigen::VectorXd& v, Eigen::MatrixXd& cov_v) const {
        neq_.ddPriorInnovation(b, x_s, cov_free, v, cov_v);
    }

    /// @return Number of DD-prior blocks folded this epoch.
    std::size_t numDdPriorBlocks() const { return neq_.numDdPriorBlocks(); }

    // --- PD-safe invariant-DD fold forwarders (docs/SPEC_pd_safe_dd_fold.md) ---

    /// Arm the PD-safe DD-prior fold arithmetic (@c network.carry_invariant_pd_safe;
    /// @ref NetworkNormalEquations::setDdFoldPdSafe).  Set once before the epoch loop;
    /// it survives @ref reset, like @ref setPerfTiming.  Off ⇒ byte-identical.
    void setDdFoldPdSafe(bool on) { neq_.setDdFoldPdSafe(on); }
    /// @return Whether the PD-safe fold arithmetic is armed.
    bool ddFoldPdSafe() const { return neq_.ddFoldPdSafe(); }
    /// @return Diagnostics of the last DD-prior fold's PD screen (INC-0).  They are
    ///         per fold, so a within-epoch RAIM re-solve overwrites them, and the
    ///         install seam reads the accepted solve.  @ref NetworkNormalEquations::DdFoldStats.
    const NetworkNormalEquations::DdFoldStats& ddFoldStats() const { return neq_.ddFoldStats(); }
    /// @return Whether DD-prior block @p b folded inertly (PD screen tripped ⇒ M = 0).
    bool ddPriorBlockDegenerate(std::size_t b) const { return neq_.ddPriorBlockDegenerate(b); }
    /// @return DD-prior block @p b's relative pivot d_min/d_max at the fold.
    double ddPriorBlockMinPivotRel(std::size_t b) const { return neq_.ddPriorBlockMinPivotRel(b); }

    // --- Carry-aware covariance readback (docs/SPEC_carry_aware_covariance.md) ---

    /// @return Whether this epoch's fold left a DD-prior block for a @c carry_aware
    ///         readback to correct (@ref NetworkNormalEquations::hasCarryCovCorrection).
    bool hasCarryCovCorrection() const { return neq_.hasCarryCovCorrection(); }
    /// @return Run-cumulative carry-aware readback telemetry
    ///         (@ref NetworkNormalEquations::CarryCovStats): the run-end readout,
    ///         which also measures the fold's cross-block approximation error.
    const NetworkNormalEquations::CarryCovStats& carryCovStats() const {
        return neq_.carryCovStats();
    }

    // --- Reduced-RHS class attribution (diagnostics; filter/NetworkRhsAttrib) ---

    /// One captured shared-info RHS increment: the contribution kind the fold site named
    /// (@ref contributionKind; the attribution's class is a function
    /// of it) plus the delta the call folded into the shared RHS.
    struct AttribCapture {
        ContributionKind kind{ContributionKind::Unset};  ///< the kind the site named; @c Unset when it named none
        Eigen::VectorXd  delta_rhs;                      ///< the folded shared-RHS increment (n_shared)
    };

    /**
     * @brief Arm / disarm the shared-info capture recorder for this epoch.
     *
     * Armed, every shared-info fold (@ref addSharedPrior, @ref addSharedPriorDense,
     * @ref addSharedPriorClockGaugeFree, @ref addSharedConstraintRows, and the
     * withdraw and re-fold calls) appends its RHS increment under the kind its site
     * named (@ref contributionKind); a constraint-row fold whose site named a kind per
     * row (@ref contributionRowKinds), and a diagonal-prior fold whose site named a kind
     * per prior (@ref contributionPriorKinds), append one increment per kind.  @ref beginEpoch
     * clears the captures but keeps the armed flag, so a within-epoch re-solve leaves
     * only the final build's captures (matching the kernel state the attribution
     * reads).  Disarmed (the default), the recorder costs one branch per call.
     */
    void attribArm(bool on) {
        attrib_armed_ = on;
        if (on) attrib_shared_.clear();
    }
    /// @return Whether the capture recorder is armed this epoch.
    bool attribArmed() const { return attrib_armed_; }
    /// @return This epoch's captured shared-info RHS increments (armed builds only).
    const std::vector<AttribCapture>& attribCaptures() const { return attrib_shared_; }

    // --- Contribution kinds of the shared folds ( filter/NetworkContributions) ---

    /**
     * @brief Name the kind of the next shared fold.
     *
     * Every shared fold (@ref addSharedPrior, @ref addSharedPriorDense,
     * @ref addSharedPriorClockGaugeFree, the refold and withdraw calls, the clone walks,
     * @ref addSharedConstraintRows, @ref setPhaseSumZeroGauges, @ref setDdPriors) takes the
     * kind named here when it starts, and records it with its dimension in the epoch's
     * @ref sharedFoldLog when it folds.  The kind is consumed by that one call, so a fold
     * whose site named none is recorded as @c ContributionKind::Unset and counted on the
     * table's integrity line rather than under a stale kind.  The same kind names the
     * fold's RHS capture (@ref attribArm), so the RHS attribution's class of a shared fold
     * is the kind's name (one name per fold site).
     *
     * @param k The kind of the next fold.
     */
    void contributionKind(ContributionKind k) { contrib_kind_ = k; }

    /**
     * @brief Name one kind per row of the next @ref addSharedConstraintRows (the W6
     *        code-bias datum stack, whose rows are of several kinds).
     *
     * Consumed by that call.  A vector whose size is not the call's row count is ignored,
     * and the rows are recorded under the kind of @ref contributionKind.
     *
     * @param kinds One kind per row of @c H (not owned; must outlive the call), or null.
     */
    void contributionRowKinds(const std::vector<ContributionKind>* kinds) {
        contrib_row_kinds_ = kinds;
    }

    /**
     * @brief Name one kind per prior of the next @ref addSharedPrior or
     *        @ref addSharedPriorClockGaugeFree (the within-epoch replay's re-fold of the
     *        captured priors, which are of several kinds and re-fold in one call).
     *
     * Consumed by that call.  The fold itself is unchanged: the call accumulates and folds
     * the same information and RHS as without the kinds.  What the kinds change is the
     * record: the fold is entered in @ref sharedFoldLog once per kind present (in the order
     * a kind first appears, each with its number of priors), and an armed capture
     * (@ref attribArm) holds one RHS increment per kind, the part of the call's increment
     * that the priors of that kind contribute (their sum is the call's increment; under the
     * gauge-free downdate a prior's part is its column of the downdated information times
     * its value).  A vector whose size is not the call's prior count is ignored, and the
     * fold is recorded under the kind of @ref contributionKind.
     *
     * @param kinds One kind per entry of the call's @c priors (not owned; must outlive the
     *              call), or null.
     */
    void contributionPriorKinds(const std::vector<ContributionKind>* kinds) {
        contrib_prior_kinds_ = kinds;
    }

    /**
     * @brief Mark the folds that follow as a within-epoch replay's re-folds.
     *
     * A replay rebuilds the epoch on a fresh arrowhead; its re-folds are counted apart
     * (@c SharedFoldLog::replay_folds) so that the log describes the primary build, and
     * its solve does not replace the primary solve's pin count.  Each switch on counts one
     * replay build.  @ref reset switches it off.
     *
     * @param on True at the start of a replay build, false at its end.
     */
    void contributionReplay(bool on) {
        contrib_replay_ = on;
        if (on) ++contrib_log_.replay_builds;
    }

    /// @return This epoch's shared folds, with their kinds (cleared by @ref reset).
    const SharedFoldLog& sharedFoldLog() const { return contrib_log_; }
    /// @return The input DD-prior blocks set by @ref setDdPriors this epoch: the
    ///         (station, local column, coefficient) structure the attribution
    ///         recompute needs, which the kernel's folded factors do not retain.
    const std::vector<NetworkNormalEquations::DdPriorBlock>& ddPriorInputs() const {
        return dd_priors_;
    }

    /**
     * @brief Map a shared estimate to an emittable @c NetworkSolutionEpoch.
     * @param x_s        The shared estimate from @ref solve.
     * @param epoch_time SSR content reference epoch stamped on the solution.
     * @param iod_ssr / provider_id / solution_id IGS-SSR stamps (pass-through).
     * @return One epoch of satellite-side solutions (clock C0 + per-signal code /
     *         phase bias) keyed by RINEX sat id, ready for `NetworkProductEmitter`.
     *         SH-VTEC columns are not mapped, nor are clock clone columns (
     *         a border-internal state, never a product) or shared-history columns
     *         (the same).
     */
    NetworkSolutionEpoch toSolutionEpoch(const Eigen::VectorXd& x_s,
                                         const GnssTime& epoch_time, int iod_ssr = 0,
                                         int provider_id = 0, int solution_id = 0) const;

    // --- Per-constellation σ̂0 carve-out decision (RTA-10) -------------------

    /// One constellation's restriction of the network σ̂0 statistic
    /// (@c network.per_const_withhold): σ̂0_c over that constellation's
    /// carried-ambiguity phase rows, plus how many rows backed it.
    struct PerConstSigma0 {
        double sigma0_c{1.0};  ///< max(1, 1.4826·median r) over the constellation's rows.
        int    rows{0};        ///< Standardized-residual rows in the constellation's pool.
    };

    /// The carve-out's publish/withhold partition of the judged constellations.
    struct PerConstWithholdDecision {
        std::set<char> publish;   ///< Constellations individually publishable.
        std::set<char> withhold;  ///< Constellations to prune from the emission.
    };

    /**
     * @brief Decide, per constellation, whether its slice of a σ̂0-withheld epoch is
     *        individually publishable (RTA-10, @c network.per_const_withhold).
     *
     * Pure policy with no estimator state.  A constellation is publishable iff its
     * σ̂0_c is at or below the withhold ceiling that the global σ̂0 tripped
     * (@p ceiling = @c network.raim_withhold_sigma0) and it has at least
     * @p min_rows rows in its pool.  With fewer rows there is too little
     * redundancy to certify it clean in a contaminated epoch, so it is withheld as
     * unjudgeable rather than published unjudged.  Everything else lands in
     * @c withhold.  The caller maps the sets onto the emission: an empty
     * @c publish means no publishable subset exists and the whole-epoch withhold
     * applies unchanged; a constellation absent from @p stats (zero rows) is
     * likewise unjudgeable, and the caller must not publish it.
     *
     * @param stats    Per-constellation σ̂0_c + row count (see the driver's pool
     *                 partition of the within-epoch RAIM standardized residuals).
     * @param ceiling  The A4 withhold ceiling (@c network.raim_withhold_sigma0).
     * @param min_rows Minimum rows to be judgeable
     *                 (@c network.per_const_withhold_min_rows).
     * @return The publish/withhold partition of exactly the keys of @p stats.
     */
    static PerConstWithholdDecision decidePerConstWithhold(
        const std::map<char, PerConstSigma0>& stats, double ceiling, int min_rows);

private:
    /// Descriptor of one shared column (for reverse mapping in toSolutionEpoch).
    struct ColumnDesc {
        std::string sat;       ///< satellite id (empty for VtecSh)
        SharedParamKind kind;
        std::string signal;    ///< empty for Clock / VtecSh
        int  sh_n{-1};         ///< VtecSh degree (VtecSh only)
        int  sh_m{-1};         ///< VtecSh order  (VtecSh only)
        char sh_trig{0};       ///< VtecSh 'C'/'S' (VtecSh only)
        int  sh_layer{0};      ///< VtecSh shell index (VtecSh only; 0 = legacy layer)
        bool clone{false};     ///< a clock clone column (kind Clock, @ref registerClockClone); or a bias clone (its own kind, @ref registerClone)
        bool hist{false};      ///< a shared-history column (kind Clock, empty sat, @ref registerHistoryColumn)
        int  hist_idx{-1};     ///< its history index j
    };

    Eigen::Index registerColumn_(const std::string& sat, SharedParamKind kind,
                                 const std::string& signal);
    static std::string key_(const std::string& sat, SharedParamKind kind,
                            const std::string& signal);
    /// The layout key of a prior: the SH key for VtecSh, @c "HIST|j" for a shared-history
    /// column, @c "sat|CLKC" for a clock clone, @ref key_
    /// otherwise (the same string as before for every other prior).
    static std::string priorKey_(const SharedPrior& p);
    /// The layout key of a clone (clock clone or border clone): @c "sat|CLKC" for a clock,
    /// @c "sat|PBC|signal" and @c "sat|CBC|signal" for the biases.
    static std::string cloneKey_(SharedParamKind kind, const std::string& sat,
                                 const std::string& signal);
    /// Throw unless the epoch is open and not yet solved (guards @ref addStation).
    void requireAccumulating_() const;

    /// @internal Shared arithmetic of @ref addSharedPriorDense and
    /// @ref refoldSharedPriorDenseDroppingClocks: map @p keys onto this epoch's
    /// layout (absent keys and @p skip keys are marginalized: their rows and
    /// columns of @p cov are dropped before the inversion), then form Λ = P⁻¹ and
    /// η = Λv in correlation scaling with the PD / ridge / information-budget
    /// guards.  Both callers use this one path, so they fold bit-identical blocks.
    /// @param skip   Satellites whose @ref SharedParamKind::Clock key is
    ///               additionally marginalized (empty for the ordinary fold).
    /// @param col    out: shared-border column of each retained key.
    /// @param in_idx out: index into @p keys of each retained key.
    /// @return false when nothing is retained (Λ / η left untouched).
    bool denseFoldTerms_(const std::vector<SharedPrior>& keys,
                         const Eigen::MatrixXd& cov,
                         const std::set<std::string>& skip,
                         std::vector<Eigen::Index>& col,
                         std::vector<Eigen::Index>& in_idx,
                         Eigen::MatrixXd& Lambda, Eigen::VectorXd& eta,
                         bool clock_gauge_free = false,
                         std::size_t* gauge_free_keys = nullptr) const;

    std::map<std::string, Eigen::Index> index_;  ///< layout key → column
    std::vector<ColumnDesc> columns_;             ///< column → descriptor
    Eigen::Index n_clones_{0};                    ///< clock clone columns registered this epoch; cleared by reset()
    Eigen::Index n_hist_{0};                      ///< shared-history columns registered this epoch; cleared by reset()
    NetworkNormalEquations neq_{0};               ///< current-epoch arrowhead
    Eigen::MatrixXd shared_cov_;                  ///< covariance of x_s from the last solve() (for the D2 carry-forward prior)
    std::vector<NetworkNormalEquations::SumZeroGauge> phase_sumzero_gauges_;  ///< Fix A: anchor-free phase-bias gauges (empty = anchor-pin datum, byte-identical)
    std::vector<NetworkNormalEquations::DdPriorBlock> dd_priors_;             ///< per-component invariant-carry DD priors (M1; empty = off, byte-identical)
    /// Take the kind named by @ref contributionKind for this fold, and clear it:
    /// @c Unset when the site named none.
    ContributionKind takeContributionKind_() {
        const ContributionKind k = contrib_kind_;
        contrib_kind_ = ContributionKind::Unset;
        return k;
    }
    /// Capture one shared fold's RHS increment under its kind, when the recorder is armed
    /// (@ref attribArm).
    void noteCapture_(ContributionKind k, const Eigen::VectorXd& delta_rhs) {
        if (attrib_armed_) attrib_shared_.push_back(AttribCapture{k, delta_rhs});
    }
    /// Record one shared fold of dimension @p dim in @ref contrib_log_ (a replay's re-fold
    /// counts apart).
    void noteFold_(ContributionKind k, long long dim) {
        if (contrib_replay_) {
            ++contrib_log_.replay_folds;
            contrib_log_.replay_dim += dim;
            return;
        }
        contrib_log_.folds.push_back(SharedFoldRecord{k, dim});
    }
    /// The kinds present in @p kinds, each once, in the order a kind first appears.
    static std::vector<ContributionKind> kindsInOrder_(const std::vector<ContributionKind>& kinds) {
        std::vector<ContributionKind> order;
        for (const ContributionKind k : kinds)
            if (std::find(order.begin(), order.end(), k) == order.end()) order.push_back(k);
        return order;
    }
    /// Record a fold whose site named a kind per part (a row or a prior) as one fold per kind
    /// present, in the order a kind first appears, each of dimension the number of its parts.
    void noteFoldsPerKind_(const std::vector<ContributionKind>& kinds) {
        for (const ContributionKind k : kindsInOrder_(kinds))
            noteFold_(k, static_cast<long long>(std::count(kinds.begin(), kinds.end(), k)));
    }
    ContributionKind contrib_kind_{ContributionKind::Unset};  ///< Kind named for the next shared fold; @c Unset when none.
    const std::vector<ContributionKind>* contrib_row_kinds_{nullptr};  ///< Per-row kinds for the next addSharedConstraintRows (not owned).
    const std::vector<ContributionKind>* contrib_prior_kinds_{nullptr};  ///< Per-prior kinds for the next addSharedPrior / addSharedPriorClockGaugeFree (not owned).
    bool contrib_replay_{false};                                 ///< Folds are a within-epoch replay's re-folds.
    SharedFoldLog contrib_log_;                                  ///< This epoch's shared folds (cleared by reset()).
    bool attrib_armed_{false};                   ///< RHS-attribution capture recorder armed (diagnostics)
    std::vector<AttribCapture> attrib_shared_;   ///< captured shared-info RHS increments (armed epochs only)
    int seam_threads_{1};                         ///< per-block DD carry-fold fan-out for foldReducedDdPriors() (1 = serial; byte-identical at any count; set by the driver)
    int carry_par_threads_{1};                    ///< network.parallel_carry: within-block per-station fan-out for the fold (1 = off; > 1 gives ULP-level differences on the reduction and stays byte-identical on the middle inverse; set by the driver)
    double dense_fold_kappa_{1e-6};               ///< relative-pivot bound of the dense border fold (network.dense_fold_kappa); 1e-6 = the historical constant, byte-identical (@ref setDenseFoldKappa)
    double rank_gate_tol_{kNullSpaceTol};         ///< resolved null-space rank-gate tolerance every consumer reads (gate / dump / drop-and-heal; network.rank_gate_tol); defaults to kNullSpaceTol so an unset config is byte-identical (@ref setRankGateTol)
    bool epoch_open_{false};                      ///< set by beginEpoch(), cleared by reset(); registration throws while set
    bool solved_{false};                          ///< set by solve(); blocks further addStation until beginEpoch()
};

// ---- Run-end reports (generated by scripts/nom_td_cut.py) ----

struct NetworkRunEndView;

/**
 * @brief Run-end report: the carry-aware covariance mode.
 *
 * The carry-aware covariance readback on every carry_invariant run, armed or not: the
 * covariance mode the published sigma was computed in.
 *
 * Gate: returns 0 at once unless `ctx.knobs.carry_invariant`.
 *
 * @param v The run at its end (network/runend/RunEnd.hpp).
 * @return 0 (this report does not set the exit code).
 */
int networkCarryAwareCovarianceRunEnd(const NetworkRunEndView& v);

// ---- End of the run-end reports ----

struct NetworkRun;
struct NetworkEngineSpec;

/**
 * @brief Configures the joint estimator once for the run.
 *
 * Thread counts, rank-gate tolerance, dense-fold kappa, the station PD guard and its
 * null-direction trace, and the PD-safe DD fold; the estimator keeps them across its per-epoch
 * reset().  Setup builder: called once from Application::runNetwork, in
 * the order scripts/nom3_designs/EP4_design.md section 2 states.
 *
 * @param run The network run; fills run.engines.est.
 * @param engine_spec The engine specification the run was built from (its carry-driver configuration).
 */
void networkEstimatorConfigure(NetworkRun& run, const NetworkEngineSpec& engine_spec);

} // namespace GnssProcessor
