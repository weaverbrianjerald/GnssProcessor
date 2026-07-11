/**
 * @file AmbiguityResolver.hpp
 * @brief LAMBDA/MLAMBDA integer ambiguity resolution for PPP-RTK.
 *
 * Implements the Modified LAMBDA (MLAMBDA) method for integer least-squares
 * estimation of carrier-phase ambiguities.  The resolver operates on the
 * between-satellite single-difference (SD) ambiguity sub-vector extracted
 * from the Kalman filter state, resolves integer values, and optionally
 * constrains the float solution via a fix-and-hold pseudo-observation update.
 *
 * References:
 *   [1] P.J.G. Teunissen, "The least-squares ambiguity decorrelation
 *       adjustment: a method for fast GPS ambiguity estimation",
 *       J. Geodesy, 70, 65-82, 1995.
 *   [2] X.-W. Chang, X. Yang, T. Zhou, "MLAMBDA: A modified LAMBDA method
 *       for integer least-squares estimation", J. Geodesy, 79, 552-565, 2005.
 */
#pragma once

#include "config/GnssConfig.hpp"
#include "logging/LogManager.hpp"
#include "filter/StateManager.hpp"
#include "common/PppTypes.hpp"
#include <Eigen/Dense>
#include <string>
#include <vector>
#include <map>
#include <set>

namespace GnssProcessor {

/**
 * @brief Result of an integer ambiguity resolution attempt.
 */
struct AmbiguityResolution {
    bool success{ false };             ///< True if ambiguities were successfully fixed.
    double ratio{ 0.0 };               ///< Ratio test value (s2/s1).  Higher = more confident.
    int num_fixed{ 0 };                ///< Number of SD ambiguities fixed.
    int num_candidates{ 0 };           ///< Number of SD ambiguity candidates.
    Eigen::VectorXd fixed_state;       ///< Full state vector with fixed ambiguities applied.
    Eigen::MatrixXd fixed_covariance;  ///< Covariance after ambiguity fixing.
    /// T2c gate attribution — WHICH early gate zeroed AR (so a run that never fixes is
    /// diagnosable).  Set ONLY at the pre-MLAMBDA early-return gates; 0 means MLAMBDA was
    /// reached (the outcome — fixed / ratio-battery reject / MLAMBDA fail — is then read
    /// from @ref success + @ref ratio).  0=reached-MLAMBDA, 1=disabled(ar_mode=0),
    /// 2=min_epochs guard, 3=position-σ gate (see @ref gate_sigma_3d), 4=no candidates
    /// (empty DD pairs / no WL integers this epoch).
    int gate_reason{ 0 };
    double gate_sigma_3d{ -1.0 };      ///< 3D position σ (m) recorded when the σ-gate (gate_reason=3) fired; −1 otherwise.
};

/**
 * @brief Configuration parameters for ambiguity resolution.
 */
struct AmbiguityResolverConfig {
    double ratio_threshold{ 3.0 };     ///< Minimum ratio test threshold for acceptance.
    double elevation_mask_ar{ 20.0 };  ///< Elevation mask for AR candidates (degrees).
    double hold_variance{ 0.001 };     ///< Pseudo-observation variance for fix-and-hold (m^2).
    int    ar_mode{ 0 };               ///< 0: float-only, 1: continuous, 2: instantaneous, 3: fix-and-hold.
    int    min_epochs{ 10 };           ///< Convergence guard: skip AR for the first N invocations.
    double max_position_sigma_m{ 0.05 };  ///< Convergence guard: skip AR when 3D position σ exceeds this (m).
    double ratio_upper_bound{ 1e4 };   ///< Reject fixes whose ratio exceeds this (degenerate covariance).
    bool   wl_only{ false };           ///< If true, AR only applies MW-resolved WL integers as Kalman pseudo-observations; no LAMBDA NL search.
    double wl_only_pseudo_var{ 1e-4 }; ///< Pseudo-observation variance (cycles²) for WL-only constraints.  ~0.01 cycle σ.
    double wl_max_frac{ 0.40 };        ///< Max worst-case fractional residual (|frac|+3σ_mean/λ, cyc) for an MW WL to enter AR.
    double min_success_rate{ 0.999 };  ///< Min bootstrap success rate (FFR-style; 0.999 = 0.1% target failure rate).
    double nl_max_per_pair_frac{ 0.15 }; ///< Max |N_1_float − N_1_fixed| per NL pair / DD; rejects marginal fixes.
    bool   enable_partial_ar{ false };   ///< If true, when the full-set LAMBDA fix fails its
        ///< acceptance gates, retry on progressively smaller subsets (drop the
        ///< least-fixable ambiguity, re-validate ratio/bootstrap/frac) so a few
        ///< noisy satellites no longer force the whole epoch back to float.
        ///< Complements (does not replace) the full-set FFR/ratio/Joseph-hold
        ///< validation; bit-identical to the legacy path when off.
    std::vector<std::string> ar_active_systems;
        ///< Constellations eligible for AR (e.g. {"GPS"}, {"GPS","Galileo"}).
        ///< Empty (default) = all active systems.  Use to keep a constellation
        ///< in the float solution while excluding it from the LAMBDA candidate
        ///< set — useful when one system's residual model bias is dragging
        ///< down the ratio test even though its float contribution is healthy.
    StateAmbiguityBasis state_ambiguity_basis{ StateAmbiguityBasis::PerBand };
        ///< Ambiguity parameterization in the Kalman state.  Mirrors
        ///< @c GnssConfig::getStateAmbiguityBasis() and is read by
        ///< AR entry points to dispatch between per-band and NL/WL paths.
};

/**
 * @brief MLAMBDA integer ambiguity resolver.
 *
 * Workflow:
 *   1. Extract the SD ambiguity sub-vector and sub-covariance from the filter.
 *   2. Form DD ambiguities using a per-constellation pivot (highest-elevation).
 *   3. Apply MLAMBDA (LDL decomposition + Z-decorrelation + ILS search).
 *   4. Ratio test: accept if s2/s1 >= threshold.
 *   5. Compute the fixed-ambiguity state and covariance.
 *   6. Optionally apply fix-and-hold constraints.
 */
class AmbiguityResolver {
public:
    /**
     * @brief Construct the resolver.
     * @param config    AR configuration parameters.
     * @param log_manager  Logging facility.
     */
    AmbiguityResolver(const AmbiguityResolverConfig& config, LogManager& log_manager);

    /**
     * @brief Attempt to resolve integer ambiguities.
     *
     * @param state_manager   Current filter state (provides ambiguity indices).
     * @param observations    Modeled observations for this epoch (for elevation, satellite info).
     * @param wl_ambiguities  Resolved SD WL integers from the MW detector.
     *                        Keys are "SAT_SIG1_SIG2" (e.g. "G01_L1C_L2W"),
     *                        values are the rounded SD WL integer.  When
     *                        available for a DD pair, a two-step WL/NL approach
     *                        is used: the WL is fixed from MW, and only the NL
     *                        is searched with LAMBDA, halving the search space.
     * @return AmbiguityResolution result.
     */
    AmbiguityResolution resolve(const StateManager& state_manager,
                                const std::vector<PppObservation>& observations,
                                const std::map<std::string, int>& wl_ambiguities,
                                const std::map<std::string, double>& wl_fractions = {});

    /**
     * @brief Attempt to resolve integer ambiguities (no WL constraint).
     */
    AmbiguityResolution resolve(const StateManager& state_manager,
                                const std::vector<PppObservation>& observations);

    /**
     * @brief Attempt to resolve integer ambiguities for baseline (DD) mode.
     *
     * Sister entry point to @c resolve() that consumes ambiguity states
     * keyed in DD form (@c "DD|cons|sat_i|rov_code|pivot") directly.
     * Unlike the PPP/PPP-RTK path, no SD subtraction is applied — the
     * state values are already DD-differenced floats in meters.  The
     * meter-valued floats are converted to integer cycles using the
     * per-pair carrier wavelength before MLAMBDA, then the integer fix
     * is multiplied back to meters for the back-substitution step.
     *
     * Only DDs whose four observations (rov_i, rov_p, ref_i, ref_p)
     * all use the same RINEX code are integer-cycle.  Anything else
     * carries a residual receiver IFB or satellite DCB term that
     * prevents the float from rounding to integer.  Those DD ambiguity
     * keys must be passed in @p exclude_keys and are skipped from the
     * candidate set.
     *
     * @param state_manager  Filter state holding DD ambiguity entries.
     * @param observations   Rover-side modeled observations (used for
     *                       elevation lookups; reference-side elevations
     *                       are nearly identical on short baselines).
     * @param exclude_keys   DD ambiguity keys to skip (typically
     *                       MeasurementModel::getNonIntegerCapableDdAmbiguityKeys()).
     * @return AmbiguityResolution; @c success=false when there are too
     *         few candidates, the ratio test fails, or MLAMBDA cannot
     *         decompose the covariance.
     */
    AmbiguityResolution resolveDd(const StateManager& state_manager,
                                  const std::vector<PppObservation>& observations,
                                  const std::set<std::string>& exclude_keys);

    /**
     * @brief Apply fix-and-hold constraints to the filter.
     *
     * Feeds back the fixed DD ambiguities as pseudo-observations to
     * constrain the float solution, preventing the ambiguities from
     * drifting away from their integer values.
     *
     * @param state_manager   Filter state to update (modified in place).
     * @param resolution      Result from a previous successful resolve() call.
     */
    void holdAmbiguities(StateManager& state_manager,
                         const AmbiguityResolution& resolution);

    /**
     * @brief Baseline-mode (DD) variant of @c holdAmbiguities.
     *
     * The state ambiguities are already DD-valued, so the
     * pseudo-observation matrix is a simple selector
     * (@c H(k, dd_idx)=1) rather than the (i,-1,ref) row used by the
     * PPP / PPP-RTK SD path.  Only ambiguities whose key starts with
     * @c "DD|" are eligible for the hold update.
     *
     * @param state_manager   Filter state to update (modified in place).
     * @param resolution      Result from a previous successful @c resolveDd() call.
     */
    void holdAmbiguitiesDd(StateManager& state_manager,
                           const AmbiguityResolution& resolution);

    /// Get the current AR configuration.
    const AmbiguityResolverConfig& getConfig() const { return config_; }

    /// Update the AR configuration.
    void setConfig(const AmbiguityResolverConfig& config) { config_ = config; }

private:
    // Test seam: the MLAMBDA unit tests (tests/test_estimation_core.cpp)
    // exercise the private static numerical kernels below in isolation with
    // synthetic matrices.  Befriending the access shim grants that access
    // without widening the public API; it has no effect on production
    // translation units (the type is only ever defined in the test binary).
    friend struct MlambdaTestAccess;

    // -----------------------------------------------------------------------
    // MLAMBDA core routines
    // -----------------------------------------------------------------------

    /**
     * @brief LDL^T decomposition of a positive-definite matrix.
     * @param Q  Input covariance matrix (n x n).
     * @param L  Output lower-triangular unit matrix.
     * @param d  Output diagonal vector.
     * @return True on success, false if matrix is not positive definite.
     */
    static bool ldlDecompose(const Eigen::MatrixXd& Q,
                             Eigen::MatrixXd& L, Eigen::VectorXd& d);

    /**
     * @brief MLAMBDA lattice reduction (decorrelation).
     * @param[in,out] L  Lower-triangular matrix (modified in place).
     * @param[in,out] d  Diagonal vector (modified in place).
     * @param[out]    Z  Integer transformation matrix.
     */
    static void reduction(Eigen::MatrixXd& L, Eigen::VectorXd& d,
                          Eigen::MatrixXd& Z);

    /**
     * @brief Integer least-squares search (modified Schnorr-Euchner).
     * @param L     Reduced lower-triangular matrix.
     * @param d     Reduced diagonal vector.
     * @param zs    Float DD ambiguity in Z-transformed space.
     * @param zn    Output: best m integer candidates (n x m).
     * @param sq    Output: squared distances for each candidate.
     * @param m     Number of candidates to find (default 2 for ratio test).
     * @return Number of candidates found.
     */
    static int search(const Eigen::MatrixXd& L, const Eigen::VectorXd& d,
                      const Eigen::VectorXd& zs,
                      Eigen::MatrixXd& zn, Eigen::VectorXd& sq, int m = 2);

    /**
     * @brief Full MLAMBDA: decompose, reduce, search, back-transform.
     * @param a     Float DD ambiguity vector.
     * @param Q     DD ambiguity covariance matrix.
     * @param afix  Output: best integer candidate.
     * @param s     Output: squared distances [s1, s2] for ratio test.
     * @param d_out Optional output: post-reduction diagonal of the LDL
     *              decomposition (i.e. the variance of each ambiguity
     *              in the decorrelated basis).  Used by the caller to
     *              compute the bootstrap success rate via
     *              @c computeBootstrapSuccessRate.  Pass nullptr to
     *              skip.
     * @return True on success.
     */
    static bool mlambda(const Eigen::VectorXd& a, const Eigen::MatrixXd& Q,
                        Eigen::VectorXd& afix, Eigen::VectorXd& s,
                        Eigen::VectorXd* d_out = nullptr);

    /**
     * @brief Bootstrap-rounding success rate for an ILS problem.
     *
     * Given the post-reduction LDL diagonal @p d (one variance per
     * ambiguity in the decorrelated basis), returns the probability
     * that simple sequential rounding would land on the correct
     * integer set:
     *
     *     P_s,boot = Π_i  erf( 1 / (2·√(2 · d_i)) )
     *
     * This is a strict lower bound on the actual ILS success rate.
     * Used as a Fixed-Failure-Rate-style acceptance criterion: AR
     * commits a fix iff @c P_s,boot exceeds a configured threshold
     * (e.g. 0.999 for 0.1% failure rate target).  Self-tuning: the
     * threshold reflects actual problem geometry rather than a
     * heuristic ratio.
     *
     * @param d Post-reduction LDL diagonal (positive entries; n entries
     *          correspond to n decorrelated ambiguities).
     * @return Bootstrap success rate ∈ [0, 1].  Zero if any d_i ≤ 0.
     */
    static double computeBootstrapSuccessRate(const Eigen::VectorXd& d);

    // -----------------------------------------------------------------------
    // DD index management
    // -----------------------------------------------------------------------

    /// Information about one DD ambiguity pair.
    struct DdPair {
        int    state_idx_i;    ///< State index of the non-pivot SD ambiguity.
        int    state_idx_ref;  ///< State index of the pivot (reference) SD ambiguity.
        std::string sat_i;     ///< Non-pivot satellite ID.
        std::string sat_ref;   ///< Pivot satellite ID.
        std::string signal;    ///< Signal type (e.g. "L1C").
    };

    /**
     * @brief Build DD ambiguity pairs from the current state.
     *
     * For each (constellation, signal) group, selects the highest-elevation
     * satellite as pivot and pairs all other ambiguities against it.
     *
     * @param state_manager  Filter state.
     * @param observations   Observations for elevation info.
     * @return Vector of DD pairs.
     */
    std::vector<DdPair> buildDdPairs(const StateManager& state_manager,
                                     const std::vector<PppObservation>& observations) const;

    // -----------------------------------------------------------------------
    // Partial ambiguity resolution (subset selection)
    // -----------------------------------------------------------------------

    /// Outcome of a partial-subset search over a cycle-domain integer set.
    struct PartialFix {
        bool success{ false };       ///< True if a fixable subset (≥1 entry) was found.
        std::vector<int> kept;       ///< Indices (into the caller's set) that survived and were fixed.
        Eigen::VectorXd afix_cycles; ///< Integer fix (cycles) for the kept entries, in @c kept order.
        double ratio{ 0.0 };         ///< Ratio test value of the accepted subset.
        double p_s{ 0.0 };           ///< Bootstrap success rate of the accepted subset.
        double max_frac{ 0.0 };      ///< Worst |float − fixed| (cycles) over the accepted subset.
    };

    /**
     * @brief Greedy backward-elimination partial ambiguity resolution.
     *
     * Starting from the full candidate set, repeatedly runs MLAMBDA on the
     * current subset and tests the same acceptance battery used by the
     * full-set fix — ratio (lower and upper bound), bootstrap success rate
     * (FFR), and per-entry |float − fixed| cap.  On any failure the least
     * fixable entry is dropped (the largest |float − fixed| when the fractional
     * cap is the failing gate, otherwise the largest formal float variance)
     * and the search retries on the smaller subset, down to a single entry.
     *
     * The accepted subset is therefore always re-validated by the full gate
     * battery on its own covariance sub-block, so subset selection can only
     * fix @e fewer ambiguities than the full set, never a worse-conditioned
     * set.  This is the safety property that lets PAR complement, rather than
     * weaken, the existing validation: a wrong elimination order costs fixes,
     * not integrity.
     *
     * Operates purely in the decorrelation-agnostic cycle domain, so it serves
     * the per-band SD, baseline DD, and WL-constrained NL paths uniformly; each
     * caller maps the returned @c kept indices back to its own state rows.
     *
     * @param y_cycles  Float ambiguity vector (cycles) for the full set.
     * @param Q_cycles  Covariance of @p y_cycles (cycles²); symmetric PD.
     * @return PartialFix; @c success=false if no subset down to size 1 passes.
     */
    PartialFix selectFixableSubset_(const Eigen::VectorXd& y_cycles,
                                    const Eigen::MatrixXd& Q_cycles) const;

    AmbiguityResolverConfig config_;
    LogManager& log_manager_;
    PppUtils ppp_utils_;   ///< For frequency/wavelength lookups in NL computation.
    int      epoch_count_{ 0 };  ///< Counts resolve()/resolveDd() invocations for the min-epochs convergence gate.
    /// Last DD WL integer applied per (sat_i, sat_ref, sig_hi, sig_lo) key,
    /// used by resolveWlOnly to detect when MW has re-locked at a different
    /// integer and the L1/L2 SD ambiguity covariance must be inflated so
    /// the new constraint can take effect in a single epoch rather than
    /// drifting toward the new integer slowly.
    std::map<std::string, int> previous_dd_wl_;

    /**
     * @brief Two-step WL/NL resolution using known WL integers.
     *
     * For each satellite pair that has DD pairs on two frequencies AND a
     * known DD WL integer, the (L1, L2) DD pair is transformed to the
     * NL domain.  LAMBDA runs on the NL-only set � unpaired DD
     * ambiguities without a matching second frequency or WL integer are
     * excluded to avoid contaminating the ratio test.  The fixed NL
     * integers are back-transformed to the original signal domain.
     *
     * @param pairs            DD pairs (from buildDdPairs).
     * @param x                Full state vector.
     * @param P                Full covariance matrix.
     * @param wl_ambiguities   Resolved SD WL integers from MW detector.
     * @param observations     Observations (for system/frequency info).
     * @return AmbiguityResolution result, or empty if the method is not applicable.
     */
    AmbiguityResolution resolveWlConstrained(
        const std::vector<DdPair>& pairs,
        const Eigen::VectorXd& x,
        const Eigen::MatrixXd& P,
        const std::map<std::string, int>& wl_ambiguities,
        const std::vector<PppObservation>& observations);

    /**
     * @brief WL-only resolution: apply MW-resolved WL integers as Kalman
     *        pseudo-observations on the SD ambiguity state, no LAMBDA NL search.
     *
     * For each pair of DD ambiguities (L1, L2) on the same satellite pair where
     * the DD WL integer is known from MW slip detection, constrains the cycle-
     * domain combination
     *     (DD_L1 / λ_L1) − (DD_L2 / λ_L2) = WL_int
     * via a Kalman update with @c wl_only_pseudo_var (cycles²) variance.  The
     * fixed_state and fixed_covariance returned reflect the WL-constrained
     * float — *not* a full integer fix.  Useful as a conservative AR mode
     * before enabling the full WL+NL LAMBDA pipeline: WL via MW is robust
     * even when float NL covariance hasn't converged enough for safe
     * integer rounding.
     *
     * Skipped under both convergence gates same as the LAMBDA paths.
     *
     * Each DD WL integer combines two zero-difference MW arcs (N_i − N_ref);
     * the per-fix confidence gate @c wl_max_frac is applied to *both* arcs, so
     * a pair is left float this epoch if either arc's running fractional
     * residual exceeds the cap.  This mirrors the gate in
     * @c applyMwWlConstraintsNlWl_ but is stronger by construction (the NlWl
     * counterpart gates a single arc).
     *
     * @param pairs           DD pairs (from buildDdPairs).
     * @param x               Full state vector.
     * @param P               Full covariance matrix.
     * @param wl_ambiguities  Resolved SD WL integers from the MW detector.
     * @param wl_fractions    Per-arc WL fractional residuals (same keys as
     *                        @p wl_ambiguities); used by the @c wl_max_frac gate.
     * @param observations    Observations for system/frequency lookup.
     * @return AmbiguityResolution with success=true if at least one WL
     *         constraint was applied; ratio is set to +∞ as a sentinel
     *         (no LAMBDA ratio test ran).
     */
    AmbiguityResolution resolveWlOnly(
        const std::vector<DdPair>& pairs,
        const Eigen::VectorXd& x,
        const Eigen::MatrixXd& P,
        const std::map<std::string, int>& wl_ambiguities,
        const std::map<std::string, double>& wl_fractions,
        const std::vector<PppObservation>& observations);

    /**
     * @brief Apply MW-resolved WL integers in the @c StateAmbiguityBasis::NlWl
     *        basis: one Kalman pseudo-observation row per (sat, band-pair).
     *
     * The WL state column under NlWl is, by construction, the SD wide-lane
     * ambiguity (post-BSSD).  For each pair of WL states sharing a
     * (constellation, band-pair) — picking the highest-elevation eligible
     * satellite as the AR-internal pivot — this method forms the DD WL
     * integer @c MW(sat_i) − MW(sat_pivot) and applies the constraint
     * @code{.unparsed}
     *     wl_state(sat_i) − wl_state(sat_pivot) = DD_WL_int  cycles
     * @endcode
     * with measurement noise @c config_.wl_only_pseudo_var (cycles²) on
     * a single Kalman row.  When the AR pivot happens to coincide with the
     * BSSD pivot, that pivot's WL column is zero-covariance and the row
     * effectively reduces to a single state (the canonical "constrain
     * the WL diagonal" form the user-visible Phase 4 design called out).
     *
     * @param state_manager   Filter state holding NL/WL ambiguity entries.
     * @param x               Float state vector (working copy).
     * @param P               Float covariance matrix (working copy).
     * @param wl_ambiguities  MW-resolved SD WL integers keyed by
     *                        @c "<sat>_<sig_a>_<sig_b>" stats keys.
     * @param observations    Per-satellite observations (for elevation
     *                        and constellation lookup).
     * @return AmbiguityResolution containing the WL-constrained float
     *         state and covariance; @c success=true when at least one
     *         constraint was applied.  @c ratio is set to +∞ as a
     *         sentinel — no LAMBDA ratio test ran.
     */
    AmbiguityResolution applyMwWlConstraintsNlWl_(
        const StateManager& state_manager,
        const Eigen::VectorXd& x,
        const Eigen::MatrixXd& P,
        const std::map<std::string, int>& wl_ambiguities,
        const std::map<std::string, double>& wl_fractions,
        const std::vector<PppObservation>& observations);
};

} // namespace GnssProcessor
