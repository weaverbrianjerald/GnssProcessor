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
#include <string>
#include <vector>

#include "filter/NetworkNormalEquations.hpp"
#include "products/NetworkProductEmitter.hpp"

namespace GnssProcessor {

/// Kind of shared satellite-side parameter carried in the network solution.
enum class SharedParamKind {
    Clock,      ///< Estimable satellite clock (signal-independent).
    CodeBias,   ///< Per-signal satellite code bias (OSB).
    PhaseBias,  ///< Per-signal satellite phase bias (OSB; carries the pivot lump).
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

    /// @return The clock column for @p sat. @throws std::out_of_range if absent.
    Eigen::Index clockColumn(const std::string& sat) const;
    /// @return The code-bias column for (@p sat, @p signal). @throws if absent.
    Eigen::Index codeBiasColumn(const std::string& sat, const std::string& signal) const;
    /// @return The phase-bias column for (@p sat, @p signal). @throws if absent.
    Eigen::Index phaseBiasColumn(const std::string& sat, const std::string& signal) const;

    /// @return Number of registered shared columns.
    Eigen::Index numShared() const { return static_cast<Eigen::Index>(columns_.size()); }

    // --- One solution epoch -------------------------------------------------

    /// Open a fresh arrowhead system over the current shared layout.  Freezes the
    /// layout for this epoch (no registration after this until the next call).
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

    /// Back-substitute a station's nuisance estimate given the shared solution.
    Eigen::VectorXd reconstructStation(std::size_t station,
                                       const Eigen::VectorXd& x_s) const;

    /**
     * @brief Map a shared estimate to an emittable @c NetworkSolutionEpoch.
     * @param x_s        The shared estimate from @ref solve.
     * @param epoch_time SSR content reference epoch stamped on the solution.
     * @param iod_ssr / provider_id / solution_id IGS-SSR stamps (pass-through).
     * @return One epoch of satellite-side solutions (clock C0 + per-signal code /
     *         phase bias) keyed by RINEX sat id, ready for `NetworkProductEmitter`.
     */
    NetworkSolutionEpoch toSolutionEpoch(const Eigen::VectorXd& x_s,
                                         const GnssTime& epoch_time, int iod_ssr = 0,
                                         int provider_id = 0, int solution_id = 0) const;

private:
    /// Descriptor of one shared column (for reverse mapping in toSolutionEpoch).
    struct ColumnDesc {
        std::string sat;
        SharedParamKind kind;
        std::string signal;  ///< empty for Clock
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
    bool epoch_open_{false};                      ///< between beginEpoch() and the next layout change
    bool solved_{false};                          ///< set by solve(); blocks further addStation until beginEpoch()
};

} // namespace GnssProcessor
