/**
 * @file NetworkEstimator.hpp
 * @brief Semantic layer over the arrowhead Schur kernel for the PPP-RTK network
 *        producer (W4-S2): shared satellite-parameter layout + explicit datum +
 *        mapping to the emittable @c NetworkSolutionEpoch.
 *
 * Where @ref NetworkNormalEquations is pure linear algebra over abstract
 * shared/local blocks, @c NetworkEstimator gives the shared columns GNSS
 * MEANING — each shared column is a (satellite, parameter-kind, signal) — and
 * drives one solution epoch:
 *
 *   1. register the shared satellite parameters (sat clock / code bias / phase
 *      bias) → fixes the shared column layout;
 *   2. @ref beginEpoch — open a fresh arrowhead system of that width;
 *   3. @ref addStation per station — pre-formed local-vs-shared design rows
 *      (built by the caller from each station's single-receiver
 *      `MeasurementModel`; the network applies no receiver-side differencing);
 *   4. @ref solve with an explicit minimum-constraint @ref Datum — the reduced
 *      shared satellite system is rank-deficient by the gauge freedoms (overall
 *      clock, the per-constellation phase-bias↔ambiguity datum, ISB), so the
 *      datum pins exactly those before the solve (realized through the kernel's
 *      @c solveSharedConstrained seam, leaving the kernel state pristine);
 *   5. @ref toSolutionEpoch — map the shared estimate to a @c NetworkSolutionEpoch
 *      that @c NetworkProductEmitter serializes in IGS-SSR convention.
 *
 * SCOPE (W4-S2a): this is the single-epoch estimator core, validated on a
 * synthetic noise-free network (truth recovery up to the declared S-transform).
 * Deliberately deferred:
 *   - S2b: the real `runNetwork()` epoch loop (MultiRinexSync `by_station`),
 *     building the per-station design rows from the live `MeasurementModel`, the
 *     between-epoch time update, and the config flags — those edit the shared
 *     core/config files and are sequenced after the concurrent sessions land.
 *   - W5: the full `SbasisDeclaration` / null-space rank gate.  The @ref Datum
 *     here is the minimal pin set the estimator needs to solve; W5 formalizes it.
 *   - QC / cold-start gating: no cross-station blunder isolation yet.
 *
 * The local datum elements that live on a station's own block (e.g. the pivot
 * receiver's ambiguity / clock pin that the emitted phase bias absorbs — Odijk
 * Eq. 4/9) are realized by the caller as tight priors folded into that station's
 * design rows; only the SHARED-block gauges go through @ref Datum.
 */

#pragma once

#include <Eigen/Dense>

#include <cstddef>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "filter/NetworkNormalEquations.hpp"
#include "products/NetworkProductEmitter.hpp"

namespace GnssProcessor {

/// Kind of shared (cross-station) parameter carried in the network solution.
enum class SharedParamKind {
    Clock,      ///< Estimable satellite clock (signal-independent).
    CodeBias,   ///< Per-signal satellite code bias (OSB).
    PhaseBias,  ///< Per-signal satellite phase bias (OSB; carries the pivot lump).
    VtecSh,     ///< Global single-layer SH-VTEC coefficient (a_nm/b_nm; network-wide, not per-satellite).
};

/**
 * @brief Joint multi-station network estimator for one solution epoch.
 *
 * Not thread-safe.  Register layout once, then per epoch: @ref beginEpoch →
 * @ref addStation × N → @ref solve → @ref reconstructStation / @ref toSolutionEpoch.
 */
class NetworkEstimator {
public:
    /// A hard pin of a shared column to a value — one element of the S-basis.
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
    /// @p trig is 'C' (cosine, m>=0) or 'S' (sine, m>=1).
    Eigen::Index registerVtecCoeff(int n, int m, char trig);

    /// @return The clock column for @p sat. @throws std::out_of_range if absent.
    Eigen::Index clockColumn(const std::string& sat) const;
    /// @return The code-bias column for (@p sat, @p signal). @throws if absent.
    Eigen::Index codeBiasColumn(const std::string& sat, const std::string& signal) const;
    /// @return The phase-bias column for (@p sat, @p signal). @throws if absent.
    Eigen::Index phaseBiasColumn(const std::string& sat, const std::string& signal) const;
    /// @return The SH-VTEC coefficient column for (n,m,trig). @throws if absent.
    Eigen::Index vtecCoeffColumn(int n, int m, char trig) const;

    /// @return Number of registered shared columns.
    Eigen::Index numShared() const { return static_cast<Eigen::Index>(columns_.size()); }

    // --- One solution epoch -------------------------------------------------

    /**
     * @brief Clear the shared-column layout for a new epoch, REUSING allocations.
     *
     * Call at the top of each epoch (before re-registering the shared parameters)
     * when the estimator is hoisted out of the epoch loop.  Empties the layout
     * (@ref index_ / @ref columns_) and the per-epoch flags but RETAINS the
     * underlying @ref NetworkNormalEquations station-block pool, so the heavy
     * per-station Schur buffers are reused across epochs instead of being freed
     * and reallocated (the network producer's memory-churn fix).  Equivalent in
     * effect to default-constructing a new @c NetworkEstimator each epoch, minus
     * the large-block heap churn.
     */
    void reset();

    /// Open a fresh arrowhead system over the current shared layout.  Freezes the
    /// layout for this epoch (no registration after this until the next call).
    /// Reuses the kernel's retained allocations (see @ref NetworkNormalEquations::reset).
    void beginEpoch();

    /**
     * @brief Add one station's pre-formed linearized rows for this epoch.
     * @param A_s Jacobian wrt the shared columns (@c m × @ref numShared).
     * @param A_r Jacobian wrt this station's private nuisances (@c m × n_local).
     * @param W   SPD measurement weight (@c m × @c m).
     * @param y   Prefit residual (length @c m).  Any pivot-receiver datum pins
     *            local to this station are folded into @p A_r / @p W / @p y by
     *            the caller (tight prior rows).
     * @return The 0-based station index for @ref reconstructStation.
     * @throws std::logic_error if called before @ref beginEpoch or after
     *         @ref solve (start a new epoch with @ref beginEpoch first).
     */
    std::size_t addStation(const Eigen::MatrixXd& A_s, const Eigen::MatrixXd& A_r,
                           const Eigen::MatrixXd& W, const Eigen::VectorXd& y);

    /// Overload with a diagonal weight (per-observation variances⁻¹).
    std::size_t addStation(const Eigen::MatrixXd& A_s, const Eigen::MatrixXd& A_r,
                           const Eigen::VectorXd& w_diag, const Eigen::VectorXd& y);

    /**
     * @brief Set the per-station fan-out used by the Schur reduction (@ref
     *        NetworkNormalEquations::reduce), the dominant per-epoch serial cost.
     *
     * Default 1 (serial, byte-identical output).  The network driver sets this to
     * its per-station thread budget when station-parallelism is enabled so the
     * reduction runs across cores; values > 1 make reduce()'s output
     * reproducible-but-ULP-dependent on the thread count (see that method).  Takes
     * effect on the next @ref solve / @ref reducedSharedNormals.
     *
     * @param n_threads Thread count (clamped to ≥ 1).
     */
    void setReduceThreads(int n_threads) {
        reduce_threads_ = n_threads > 1 ? n_threads : 1;
    }

    /// Pin-aware station PD guard passthrough
    /// (@ref NetworkNormalEquations::setStationPdMedianScale;
    /// @c network.station_pd_pin_aware).  Configuration-level — set once before
    /// the epoch loop; survives @ref beginEpoch / @ref reset.
    void setStationPdMedianScale(bool on) { neq_.setStationPdMedianScale(on); }

    /**
     * @brief Solve the shared satellite parameters under the explicit @p datum.
     *
     * Reduces the arrowhead, applies the datum pins to the reduced shared system
     * (a large-weight constraint per pin on a COPY, via the kernel's
     * `solveSharedConstrained` seam), and returns the shared estimate @c x_s.
     * The internal kernel state is left pristine, so @ref reconstructStation is
     * valid afterwards.
     *
     * @param datum The shared-block gauges (≥ the rank deficiency of the reduced
     *        system).  An empty datum requires an already-full-rank reduced
     *        system (rare in practice).
     * @return The shared satellite-parameter estimate @c x_s.
     */
    Eigen::VectorXd solve(const Datum& datum);

    /**
     * @brief Inject a carried-ambiguity (phase-leveling) prior onto one station's
     *        local block — forwards to @ref NetworkNormalEquations::addStationPrior.
     *
     * Call AFTER @ref addStation (the station must already be in this epoch's
     * arrowhead) and BEFORE @ref solve / @ref reducedSharedNormals (a local-block
     * change must be folded before the Schur reduction; there is no post-reduce
     * fast-path).  @p local_cols are columns within that station's @c A_r;
     * @p delta_Nrr_diag are the per-column information weights
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
     * sharedCovariance), zeros the rows/columns of every @p datum-pinned shared
     * column (the S-basis gauges are EXACT, so they carry no random
     * uncertainty), and propagates the result into station @p station's locals
     * via @ref NetworkNormalEquations::reconstructStationCovariance.  The
     * returned block is @f$\mathrm{Cov}(x_r\mid x_s^{\text{free}})@f$ — the
     * covariance the between-station DD ambiguities are differenced from before
     * the integer search.
     *
     * @param station Index from @ref addStation.
     * @param datum   The same datum passed to @ref solve (its pinned columns).
     * @return The station's local covariance (@c n_local × @c n_local); empty if
     *         the station has no locals.  Requires a prior @ref solve.
     */
    Eigen::MatrixXd conditionalLocalCovariance(std::size_t station,
                                               const Datum& datum) const;

    /**
     * @brief Joint conditional covariance of local parameters drawn from one or
     *        more stations, in the datum-as-known frame — the between-station
     *        double-difference ambiguity covariance primitive (network AR, Phase 2).
     *
     * Zeros the @p datum-pinned columns (and any ensemble phase-gauge columns) of
     * the last @ref solve's @ref sharedCovariance, then forwards to
     * @ref NetworkNormalEquations::jointConditionalCovariance.  The @c (a,b) entry
     * is @f$\mathrm{Cov}(x^{(i)}_{c_a}, x^{(j)}_{c_b}\mid x_s^{\text{free}})@f$;
     * cross-station entries carry ONLY the shared-parameter propagation (the
     * coupling that makes a between-station DD covariance correct).
     *
     * @param requests @c (station, local_col) pairs.
     * @param datum    The same datum passed to @ref solve.
     * @return The @c k×k joint conditional covariance; empty if @p requests is
     *         empty.  Requires a prior @ref solve.
     */
    Eigen::MatrixXd jointConditionalCovariance(
        const std::vector<std::pair<std::size_t, Eigen::Index>>& requests,
        const Datum& datum) const;

    /**
     * @brief Diagonal conditional variances for a SUBSET of a station's local
     *        columns in the datum-as-known frame (the carried-ambiguity harvest).
     *
     * Like @ref conditionalLocalCovariance but returns only the diagonal entries
     * for @p local_cols, computed without the full per-station inverse — the cheap
     * per-epoch harvest the phase-leveling carry needs to seed each ambiguity's
     * next-epoch prior variance.  Zeros the @p datum-pinned shared columns of the
     * last solve's @ref sharedCovariance (exact gauges carry no random
     * uncertainty), then forwards to
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
        const Datum& datum) const;

    /**
     * @brief Regression of one local column on the shared border and its pure
     *        conditional variance — the ingredients of the rigorous between-epoch
     *        conditional carry (Card 1, @c network.carry_conditional_coupling).
     *
     * Thin forward to @ref NetworkNormalEquations::stationRegression: returns
     * @f$M_r=N_{sr}N_{rr}^{-1}e_r@f$ (the shared-border regression row) and
     * @f$(N_{rr}^{-1})_{rr}=\mathrm{Var}(x_r\mid x_s)@f$ (the CONDITIONAL variance
     * given the whole shared border, distinct from the MARGINAL
     * @ref conditionalLocalVarianceDiag returns).  Both are pure functions of the
     * accumulated normals — datum-independent — so no @ref Datum argument is taken;
     * the driver restricts @f$M_r@f$ to the FREE (non-datum-pinned) shared columns
     * itself.  Requires a station already added this epoch (a solve is not
     * required — the cached @f$N_{rr}@f$ factor exists from @ref addStation).
     *
     * @param station   Index from @ref addStation.
     * @param local_col Local column within that station's @c A_r block.
     * @return @ref NetworkNormalEquations::StationRegression.
     */
    NetworkNormalEquations::StationRegression stationRegression(
        std::size_t station, Eigen::Index local_col) const;

    /**
     * @brief Baarda per-row redundancy numbers for one station's measurement rows
     *        (B2 network RAIM standardization), in the datum-as-known frame.
     *
     * Zeros the @p datum-pinned columns of the last @ref solve's @ref sharedCovariance
     * (the S-basis gauges are EXACT — no random uncertainty), then forwards to
     * @ref NetworkNormalEquations::rowRedundancy.  Returns
     * @f$r_i = 1 - w_i\,a_i^\top\mathrm{Cov}(\theta)\,a_i \in [0,1]@f$ per row, with
     * @f$a_i = [\,@c A_s_rows.row(i)\mid@c A_r_rows.row(i)\,]@f$.  Requires a prior
     * @ref solve.  A low value ⇒ a lightly-checked measurement whose residual barely
     * responds to a fault (needs @f$/\sqrt{r_i}@f$ standardization to be flaggable).
     */
    Eigen::VectorXd rowRedundancy(std::size_t station,
                                  const Eigen::MatrixXd& A_s_rows,
                                  const Eigen::MatrixXd& A_r_rows,
                                  const Eigen::VectorXd& w_rows,
                                  const Datum& datum) const;

    /// @return The kernel's reduce-once instrumentation
    ///         (@ref NetworkNormalEquations::reduceRecomputeCount) for the current
    ///         epoch — asserts the carried prior did not force a second reduction.
    std::size_t reduceRecomputeCount() const { return neq_.reduceRecomputeCount(); }

    /// @name Tier D0 section-timer passthroughs (@c network.trace_section_timers)
    ///
    /// Arm/reset/read the kernel's per-section wall-clock accumulators (assembly /
    /// reduce / solve / shared-covariance inverse / per-station covariance
    /// consumers).  Configuration-level: @ref setPerfTiming once before the epoch
    /// loop (survives @ref reset / @ref beginEpoch, like @ref setStationPdMedianScale).
    /// OFF (default) ⇒ no clock reads, numerically byte-identical.  See
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
     * The inverse of the datum-constrained reduced shared normals — i.e. the
     * formal covariance of @c x_s (datum-pinned columns carry a tiny variance,
     * being the fixed S-basis gauges).  Used by the network driver's between-epoch
     * carry-forward (the W4-S2b D2 prior): the posterior (value, variance) per
     * shared parameter seeds the next epoch's prior via @ref
     * NetworkNormalEquations::addSharedInfo.  Empty until @ref solve is called.
     */
    const Eigen::MatrixXd& sharedCovariance() const { return shared_cov_; }

    /**
     * @brief Zero the ROWS AND COLUMNS of every folded ensemble phase-bias gauge
     *        column in a shared-covariance matrix — treat each @f$\sum_r z^s_r=0@f$
     *        gauge as an EXACT datum (variance 0).
     *
     * Fix A (covariance-carry consistency): the anchor-free ensemble datum fixes each
     * satellite phase bias POST-reduce via the rank-1 @f$g g^\top/\beta@f$ fold
     * (@ref NetworkNormalEquations::foldReducedSumZeroGauges).  A gauge's shared
     * column is the gauge (null) direction of the un-reduced normals, so its slot of
     * @ref sharedCovariance carries the fold's ARTIFICIAL gauge-direction variance +
     * phase↔clock/code cross terms — not a physical uncertainty.  Any consumer that
     * propagates the shared covariance across epochs (the driver's D2 carry) or into
     * the station locals (the datum-frame conditional covariance) must treat those
     * columns as exact, EXACTLY as the datum-pinned columns are zeroed.  Without this
     * the fold's over-tight/cross-coupled gauge covariance (i) over-tightens the D2
     * phase-bias prior → biases the coupled clock, and (ii) INFLATES the
     * conditional-ambiguity variance the phase-leveling carry seeds from → the carry
     * goes inert → the ensemble reference re-realizes every epoch → the emitted phase
     * bias churns → the co-estimated iono (SH-VTEC) absorbs the churn and feeds it
     * back → exponential clock/bias runaway.  No-op (byte-identical) when no ensemble
     * gauge is folded (anchor / OFF path).
     *
     * @param cov In/out shared covariance (@c numShared × @c numShared); untouched
     *            when it is not that size or no gauge column is in range.
     */
    void zeroPhaseGaugeColumns(Eigen::MatrixXd& cov) const;

    /**
     * @brief The reduced shared normals @f$\tilde N_{ss}@f$ after Schur reduction
     *        (lazily reduces).  The W5 null-space-gate seam — inspect the
     *        pre-constraint reduced system before @ref solve.  Non-const: updates
     *        the kernel's internal reduction cache (idempotent; @ref solve reuses it).
     */
    const Eigen::MatrixXd& reducedSharedNormals();

    /// A carried-forward / regularizing prior on one shared parameter (W4-S2b D2
    /// time update; also the SH-VTEC Kaula/temporal prior).
    struct SharedPrior {
        SharedParamKind kind;     ///< parameter family
        std::string     sat;      ///< satellite id (empty for VtecSh)
        std::string     signal;   ///< signal (empty for Clock / VtecSh)
        int             sh_n{-1};    ///< VtecSh degree (VtecSh only)
        int             sh_m{-1};    ///< VtecSh order  (VtecSh only)
        char            sh_trig{0};  ///< VtecSh 'C'/'S' (VtecSh only)
        double          value{0.0};  ///< prior mean
        double          info{0.0};   ///< information weight = 1 / prior-variance (>0)
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
     * @brief Inject ONE dense full-covariance carried-forward prior onto the
     *        shared border (the C1 cross-epoch carry; forwards to
     *        @ref NetworkNormalEquations::addSharedInfo as a single symmetric
     *        information block).
     *
     * Folds @f$\Lambda = P^{-1}@f$ and @f$\eta = \Lambda\,v@f$ over the carried
     * keys, where @p cov is the previous epoch's posterior covariance block plus
     * process noise (P_prev + Q, per-key surgeries already applied by the caller)
     * and @c v the carried values — the information-form Kalman time+prior update
     * on the shared border.  Identity-keyed like @ref addSharedPrior, with one
     * load-bearing difference in the absent-key policy: a key not present in this
     * epoch's layout is MARGINALIZED — its rows/columns of @p cov are dropped
     * BEFORE the inversion.  (Dropping rows of Λ instead would CONDITION on the
     * departed key and bias every surviving parameter; this is why the API takes
     * a covariance, not an information matrix.)
     *
     * Numerics: the retained block is symmetrized and inverted in correlation
     * scaling (@f$P = D C D,\ \Lambda = D^{-1} C^{-1} D^{-1}@f$, @f$D=\sqrt{\mathrm
     * {diag}\,P}@f$) via LDLT with a relative-pivot PD guard and one @c 1e-9 ridge
     * retry on the unit diagonal, so blocks mixing stiff zero-q phase-bias carries
     * (~1e-9 m²) with loose clock carries (~1e-2 m²) stay well-scaled.  The folded
     * Λ is explicitly symmetrized so the pre-reduce (within-epoch RAIM replay) and
     * post-reduce (primary fold fast-path) kernel paths agree bit-for-bit.
     *
     * @param keys        Identity + carried value per entry (@ref SharedPrior;
     *                    the @c info field is IGNORED — the weight is @p cov).
     *                    Order defines the row/column order of @p cov.
     * @param cov         @c keys.size() × @c keys.size() prior covariance
     *                    (P_prev + Q).  Must be finite with positive diagonal on
     *                    every retained key (caller pre-screens per-key variance).
     * @param lambda_diag Optional out: resized to @c keys.size(); the folded
     *                    @f$\Lambda(c,c)@f$ per INPUT key (0 for marginalized
     *                    keys) — feeds the B7 anchor-information trace.
     * @return Number of keys retained (present in this epoch's layout).
     * @throws std::invalid_argument on a dimension mismatch.
     * @throws std::logic_error unless called between @ref beginEpoch and
     *         @ref solve (guarded, unlike the legacy diagonal overload).
     * @throws std::runtime_error when the retained block is non-finite, has a
     *         non-positive retained variance, or stays non-PD after the ridge
     *         retry — the caller (driver) owns the fallback policy (degrade to
     *         the diagonal carry for the epoch).
     */
    std::size_t addSharedPriorDense(const std::vector<SharedPrior>& keys,
                                    const Eigen::MatrixXd& cov,
                                    Eigen::VectorXd* lambda_diag = nullptr);

    /**
     * @brief Fold an explicit linear gauge constraint onto the shared border.
     *
     * Adds the information form of @p weight·‖@p H x − @p z‖² to the shared
     * normals: @f$N_{ss} \mathrel{+}= w\,H^\top H,\ u_s \mathrel{+}= w\,H^\top z@f$
     * (via @ref NetworkNormalEquations::addSharedInfo).  Used by the network driver
     * to pin the satellite code-bias gauges the data leaves free — the per-
     * satellite ionosphere-free combination (so the emitted OSB is pure-DCB, its IF
     * lump staying in the estimable clock) and the per-constellation DCB
     * sum-to-zero — as tight-but-finite rows folded BEFORE the null-space gate, so
     * the reduced shared system carries only the per-constellation clock deficiency
     * the explicit @ref Datum pins.
     *
     * Because these rows pin pure GAUGE directions (zero data information), any
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
     *        (Fix A, @c network.phase_datum_ensemble).
     *
     * The ensemble realization of the phase-bias↔ambiguity datum: instead of
     * pinning ONE anchor receiver's ambiguity per (satellite, signal), fix the
     * gauge by @f$\sum_r z^s_{r,\sigma}=0@f$ over the RECEIVERS tracking it — no
     * pivot, no handover, no single-anchor SPOF, and every satellite referenced to
     * the same receiver ensemble (so the user's between-satellite fractional bias
     * cancels and its DD ambiguity stays integer; Odijk S-system).  Each gauge's
     * terms are (station index, local ambiguity column) — the caller (driver)
     * builds them from its own @c A_r layout.  Stored and folded through the kernel
     * (@ref NetworkNormalEquations::foldReducedSumZeroGauges) at @ref solve /
     * @ref reducedSharedNormals, AFTER the Schur reduction and BEFORE the datum
     * pins / null-space gate.  Pass an empty vector (the default) for the
     * legacy anchor-pin datum — then @ref reconstructStation and the reduced system
     * are byte-identical.  Cleared by @ref reset.
     *
     * @param gauges One sum-to-zero gauge per (satellite, signal) shared phase bias.
     */
    void setPhaseSumZeroGauges(std::vector<NetworkNormalEquations::SumZeroGauge> gauges) {
        phase_sumzero_gauges_ = std::move(gauges);
    }

    /**
     * @brief Card-4 (M1) invariant carry: set the rank-K double-difference priors the
     *        z-store folds onto the reduced system this epoch
     *        (@ref NetworkNormalEquations::foldReducedDdPriors).
     *
     * One @c DdPriorBlock per connected (const,signal) component — the z-store's
     * carried DD ambiguities (mean + dense covariance) as gauge-free constraints on
     * the local ambiguity columns.  Folded at @ref solve AFTER the Schur reduction,
     * BEFORE the W5 datum constraint.  MUTUALLY EXCLUSIVE with the sum-to-zero
     * ensemble gauges (both are post-reduce Woodbury folds reading the pristine
     * per-station @f$N_{rr}@f$; composing them over coupled columns drops the cross
     * term — @ref solve throws if both are set).  The Card-4 phase datum is the
     * PRE-reduce tree pins (@ref addStationPrior), which compose exactly with this
     * fold.  Empty (the default) → byte-identical OFF path.  Cleared per epoch (like
     * the sum-to-zero gauges), so the driver re-sets them each epoch from the z-store.
     *
     * @param blocks One rank-K DD-prior block per component.
     */
    void setDdPriors(std::vector<NetworkNormalEquations::DdPriorBlock> blocks) {
        dd_priors_ = std::move(blocks);
    }

    /// Posterior mean of DD-prior block @p b given the solved shared estimate @p x_s
    /// — the value the z-store carries forward (forwards to the kernel).
    Eigen::VectorXd ddPriorPosteriorMean(std::size_t b, const Eigen::VectorXd& x_s) const {
        return neq_.ddPriorPosteriorMean(b, x_s);
    }
    /// @return Number of DD-prior blocks folded this epoch.
    std::size_t numDdPriorBlocks() const { return neq_.numDdPriorBlocks(); }

    /**
     * @brief Map a shared estimate to an emittable @c NetworkSolutionEpoch.
     * @param x_s        The shared estimate from @ref solve.
     * @param epoch_time SSR content reference epoch stamped on the solution.
     * @param iod_ssr / provider_id / solution_id IGS-SSR stamps (pass-through).
     * @param ambiguity_datum_ensemble T3a: true when the phase-bias gauge is the
     *        anchor-free receiver ENSEMBLE (@c network.phase_datum_ensemble,
     *        @c SbasisDeclaration::AmbDatum::SumToZeroPerReceiver).  The ensemble
     *        references the satellite phase bias to the mean of the tracking
     *        receivers' float ambiguities, so the emitted bias is NON-integer — the
     *        @c signal_integer flag is forced false so no consumer treats it as an
     *        AR candidate.  Default false = the pivot-receiver datum (byte-identical).
     * @return One epoch of satellite-side solutions (clock C0 + per-signal code /
     *         phase bias) keyed by RINEX sat id, ready for `NetworkProductEmitter`.
     */
    NetworkSolutionEpoch toSolutionEpoch(const Eigen::VectorXd& x_s,
                                         const GnssTime& epoch_time, int iod_ssr = 0,
                                         int provider_id = 0, int solution_id = 0,
                                         bool ambiguity_datum_ensemble = false) const;

    // --- RTA-10: per-constellation σ̂0 carve-out decision --------------------

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
     * Pure policy — no estimator state.  A constellation is PUBLISHABLE iff its
     * σ̂0_c is at or below the SAME withhold ceiling the global σ̂0 tripped
     * (@p ceiling = @c network.raim_withhold_sigma0) AND it has at least
     * @p min_rows rows in its pool (fewer ⇒ too little redundancy to certify
     * cleanliness under a contaminated epoch — withheld as UNJUDGEABLE, not
     * published unjudged).  Everything else lands in @c withhold.  The caller maps
     * the sets onto the emission: an empty @c publish means NO healthy subset
     * exists and the whole-epoch withhold applies unchanged; a constellation
     * absent from @p stats (zero rows) is likewise unjudgeable and must not be
     * published by the caller.
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
    };

    Eigen::Index registerColumn_(const std::string& sat, SharedParamKind kind,
                                 const std::string& signal);
    static std::string key_(const std::string& sat, SharedParamKind kind,
                            const std::string& signal);
    /// Throw unless the epoch is open and not yet solved (guards @ref addStation).
    void requireAccumulating_() const;

    std::map<std::string, Eigen::Index> index_;  ///< layout key → column
    std::vector<ColumnDesc> columns_;             ///< column → descriptor
    NetworkNormalEquations neq_{0};               ///< current-epoch arrowhead
    Eigen::MatrixXd shared_cov_;                  ///< covariance of x_s from the last solve() (for the D2 carry-forward prior)
    std::vector<NetworkNormalEquations::SumZeroGauge> phase_sumzero_gauges_;  ///< Fix A: anchor-free phase-bias gauges (empty = anchor-pin datum, byte-identical)
    std::vector<NetworkNormalEquations::DdPriorBlock> dd_priors_;             ///< Card 4 (M1): per-component invariant-carry DD priors (empty = OFF, byte-identical)
    int reduce_threads_{1};                       ///< per-station Schur-loop fan-out for reduce() (1 = serial; set by the driver)
    bool epoch_open_{false};                      ///< between beginEpoch() and the next layout change
    bool solved_{false};                          ///< set by solve(); blocks further addStation until beginEpoch()
};

} // namespace GnssProcessor
