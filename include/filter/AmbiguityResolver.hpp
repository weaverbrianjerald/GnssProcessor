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
 * @brief Quality of one MW-resolved wide-lane arc, as consumed by the WL-only
 *        acceptance gate (@ref AmbiguityResolverConfig::wl_only_ffr).
 *
 * Mirrors @c MwSlipDetector::WlLockQuality without coupling this header to the
 * detector; the caller converts.  @c count travels with σ because a 2-sample arc
 * whose samples happen to agree reports σ ≈ 0, i.e. a success rate of ≈ 1 on one
 * degree of freedom.
 */
struct WlArcQuality {
    double      sigma_cycles{ 0.0 };   ///< σ of the MW arc mean (cycles).
    std::size_t count{ 0 };            ///< Welford sample count backing that σ.
    bool        exact{ false };        ///< SD-vs-self pivot: 0 cycles by construction, no uncertainty.
};

/**
 * @brief Result of an integer ambiguity resolution attempt.
 */
struct AmbiguityResolution {
    bool success{ false };             ///< True if ambiguities were successfully fixed.
    /// Ratio test value (s2/s1).  Higher = more confident.
    /// @warning `+inf` is a sentinel meaning "no LAMBDA ratio test ran".  The WL-only
    /// and MW-WL-constraint paths set it, because they fix by applying MW integers
    /// directly.  It is not a large ratio and must not be graded as one; an ungated
    /// 35 m wrong fix passed review that way (`docs/SPEC_clock_temporal_anchor.md`
    /// §8.11).  Grade those fixes on @ref p_succ, and see
    /// @ref AmbiguityResolverConfig::wl_only_ffr.
    double ratio{ 0.0 };
    int num_fixed{ 0 };                ///< Number of SD ambiguities fixed.
    int num_candidates{ 0 };           ///< Number of SD ambiguity candidates.
    Eigen::VectorXd fixed_state;       ///< Full state vector with fixed ambiguities applied.
    Eigen::MatrixXd fixed_covariance;  ///< Covariance after ambiguity fixing.
    /// Which early gate stopped AR, so that a run that never fixes can be diagnosed
    /// (T2c).  Set only at the early-return gates ahead of MLAMBDA; 0 means no early
    /// gate fired.  On the LAMBDA paths MLAMBDA was then reached, and the outcome
    /// (fixed, rejected by the ratio battery, or MLAMBDA failure) is read from
    /// @ref success and @ref ratio.  Codes: 0 = no early gate, 1 = disabled
    /// (ar_mode = 0), 2 = min_epochs guard, 3 = position-σ gate (see @ref gate_sigma_3d),
    /// 4 = no candidates (empty DD pairs, or no WL integers this epoch), 5 = the WL
    /// acceptance guards rejected every MW constraint
    /// (@ref AmbiguityResolverConfig::wl_only_ffr / @c wl_only_max_innov_cyc).
    /// Code 5 exists because MLAMBDA never runs on those paths: without it a guard
    /// rejection would read as `mlambda_fail` and attribute the guard's decision to the
    /// search, the same misreading as the `ratio = +inf` sentinel.
    int gate_reason{ 0 };
    double gate_sigma_3d{ -1.0 };      ///< 3D position σ (m) recorded when the σ-gate (gate_reason=3) fired; −1 otherwise.
    /// Bootstrap success rate of the accepted fix (the FFR quantity).  It is the only
    /// confidence figure the WL-only paths can report, since their @ref ratio is the
    /// `+inf` sentinel.  0 when no fix was accepted or the path did not compute one.
    double p_succ{ 0.0 };
    /// True when the accepted fix came from a WL-only / MW-WL-constraint path
    /// (MW integers applied directly, no LAMBDA search).  Lets a trace
    /// distinguish "fixed by ILS" from "fixed by asserting MW integers", which
    /// @ref ratio alone cannot.
    bool wl_only_fix{ false };

    /**
     * @brief One single-signal double-difference ambiguity that an accepted fix
     *        constrained to an integer: the state indices of the non-pivot
     *        ambiguity and of the pivot it was differenced against.
     */
    struct FixedDdRow {
        int state_idx_i{ -1 };     ///< State index of the non-pivot satellite's ambiguity.
        int state_idx_ref{ -1 };   ///< State index of the pivot satellite's ambiguity.
    };

    /**
     * @brief The single-signal DD ambiguities the accepted fix constrained, one
     *        row per (non-pivot satellite, signal).
     *
     * Filled by the PPP / PPP-RTK paths that fix each signal's DD on its own: the
     * per-band LAMBDA search and the WL-constrained N_1 search, full set or partial
     * subset alike (the WL-constrained path adds both signals of each fixed pair).
     * Empty after a failed resolution, after the WL-only and NlWl paths (they
     * constrain the widelane combination, not any single-signal DD) and after
     * @c resolveDd (the baseline hold selects its own keys).  The rows carry the
     * epoch's candidate set with them: the elevation mask, the pivot election and
     * the exclusions were applied from the epoch's observations when the fix was
     * formed, so @ref AmbiguityResolver::holdAmbiguities needs neither the
     * observations nor a second pair election.
     */
    std::vector<FixedDdRow> fixed_dd_rows;
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
    /**
     * @brief Per-candidate maturity gate: minimum accumulated @f$\Sigma\sin^2(el)@f$
     *        on both constituent arcs before a candidate may enter the search.
     *
     * @c min_epochs is a global filter-epoch counter and does not gate individual
     * candidates, so a satellite that rose one minute ago is searched alongside one
     * tracked for four hours, and one immature candidate can fail the ratio test for
     * the whole set.  On a 24 h ALGO run the youngest arc was the worst NL direction
     * in 85.5 % of epochs, and the full-set ratio test failed in 98 % of epochs.
     *
     * Read only by the WL-constrained N_1 search
     * (@ref AmbiguityResolver::resolveWlConstrained), on the higher-frequency arcs of
     * the non-pivot satellite and the pivot.  The per-band, baseline DD and WL-only
     * paths do not apply it.
     *
     * Units are epoch-equivalents at zenith (an epoch at 30 deg contributes
     * 0.25).  0 (default) disables the gate and is byte-identical to the
     * pre-gate behaviour.
     *
     * @warning The gate guards against wrong fixes; it does not speed convergence.
     * It admits a candidate only once its arcs are well determined, which is when
     * the float solution gains least from a fix.
     */
    double min_arc_info{ 0.0 };
    double max_position_sigma_m{ 0.05 };  ///< Convergence guard: skip AR when 3D position σ exceeds this (m).
    double ratio_upper_bound{ 1e4 };   ///< Reject fixes whose ratio exceeds this (degenerate covariance).
    bool   wl_only{ false };           ///< If true, AR only applies MW-resolved WL integers as Kalman pseudo-observations; no LAMBDA NL search.
    double wl_only_pseudo_var{ 1e-4 }; ///< Pseudo-observation variance (cycles²) for WL-only constraints.  ~0.01 cycle σ.
    double wl_max_frac{ 0.40 };        ///< Max worst-case fractional residual (|frac|+3σ_mean/λ, cyc) for an MW WL to enter AR.
    double min_success_rate{ 0.999 };  ///< Min bootstrap success rate (FFR-style; 0.999 = 0.1% target failure rate).
    /**
     * @brief Apply the bootstrap FFR gate to the WL-only and MW-WL-constraint
     *        paths, which have no success-rate gate without it.
     *
     * `wl_max_frac` is a point test on each arc's running residual: it says the
     * locked integer looks self-consistent this epoch.  It cannot bound
     * P(the integer is wrong), which is the quantity an acceptance decision has to
     * control.  With this on, @ref AmbiguityResolver::resolveWlOnly and
     * @ref AmbiguityResolver::applyMwWlConstraintsNlWl_ evaluate the same
     * `erf(1/(2√2·σ))` bootstrap product as the LAMBDA tier
     * (@ref ils::computeBootstrapSuccessRate) against @ref min_success_rate,
     * dropping the weakest arc until the set passes.
     *
     * The product is taken over distinct arcs, not over DD constraints: many DDs
     * share the pivot arc, so a wrong pivot makes all of them wrong together, and
     * multiplying per-DD terms would count that shared failure once per DD.
     *
     * On by default.  With it off, an ungated 5-constraint WL-only fix put a
     * held-out consumer 35 m off while `ratio` reported the "no LAMBDA ran"
     * sentinel `+inf` (`docs/SPEC_clock_temporal_anchor.md` §8.11).  Set false to
     * reproduce the ungated behaviour exactly.
     */
    bool   wl_only_ffr{ true };
    /// Minimum MW arc sample count for an arc to carry a WL constraint under
    /// @ref wl_only_ffr; an arc below it contributes 0 to the product.  A 2-sample
    /// arc has a σ estimate with about one degree of freedom, and admitting it would
    /// let an unreliable variance buy a fix.
    int    wl_only_min_arc_count{ 10 };
    /**
     * @brief Max |asserted WL integer − predicted DD widelane| (cycles) for a
     *        WL-only or MW-WL constraint to be applied.  0 disables the check.
     *
     * The analogue of @ref nl_max_per_pair_frac, which the LAMBDA tier applies per
     * entry.  Without it the WL-only paths assert MW integers at
     * `wl_only_pseudo_var` (σ ≈ 0.01 cycle) however far the current float
     * disagrees.
     *
     * The FFR gate cannot provide this check.  A bootstrap success rate bounds the
     * random failure of rounding an arc; it is computed from the arc's own
     * dispersion and correctly reports a mature arc's widelane as well known.  It
     * cannot see a systematic disagreement between that integer and the filter's
     * frame, because the arc's dispersion does not depend on the frame.  The MW
     * widelane is in the .BIA frame, while a network-bias consumer's ambiguities
     * are in the consumed phase-bias frame (the conflict `setArIgnoreMwWl` exists
     * to avoid).  Asserting across that gap put a held-out consumer 35 m off with
     * `p_succ = 1.0` (`docs/SPEC_clock_temporal_anchor.md` §8.11).
     *
     * Default 0.5 cycle: half a widelane is the largest disagreement that can
     * still be a rounding question rather than a frame question.
     */
    double wl_only_max_innov_cyc{ 0.5 };
    double nl_max_per_pair_frac{ 0.15 }; ///< Max |N_1_float − N_1_fixed| per NL pair / DD; rejects marginal fixes.
    bool   nl_iono_free_row{ false };
        ///< Search functional for the WL-constrained N_1 step (@ref
        ///< AmbiguityResolver::resolveWlConstrained).  Both choices target the
        ///< same integer N_1 and differ only in which row of the (L1,L2) plane
        ///< is used to observe it:
        ///<
        ///< - @c false (default, legacy): the narrowlane row
        ///<   @f$(f_1,f_2)/(f_1{+}f_2)@f$.  Low noise, but not iono-free: its
        ///<   slant-iono sensitivity is @f$T\cdot\alpha = 5.907@f$ NL cycles per
        ///<   metre of L1 slant delay (0.959 cycles/TECU), so a 0.15-cycle
        ///<   @ref nl_max_per_pair_frac cap implicitly requires the ionosphere to
        ///<   0.156 TECU.
        ///< - @c true: the exact ionosphere-free row
        ///<   @f$(f_1^2,-f_2^2)/(f_1^2-f_2^2)@f$, for which
        ///<   @f$c_{IF}\cdot\alpha = 0@f$ exactly; the N_1 target carries no
        ///<   slant-iono error.
        ///<
        ///< @warning The iono-free row has a cost.  Both the row norm and the
        ///< widelane sensitivity grow by the factor
        ///< @f$(f_1{+}f_2)/(f_1{-}f_2) = 8.06@f$ (GPS L1/L2): a 1-cycle WL error
        ///< shifts the N_1 target by 3.53 cycles instead of 0.438, and the searched
        ///< covariance grows about 65× in variance.  The row trades an ionospheric
        ///< bias for amplified widelane error and noise, and pays off only with a
        ///< near-perfect widelane; arm it together with WL hardening, not alone.
        ///< Byte-identical to legacy when off.
    bool   enable_partial_ar{ false };   ///< If true, when the full-set LAMBDA fix fails its
        ///< acceptance gates, retry by greedy backward elimination (drop the least
        ///< fixable ambiguity, re-validate ratio/bootstrap/frac) and accept the first
        ///< subset that passes, so a few noisy satellites do not force the whole
        ///< epoch back to float (@ref AmbiguityResolver::selectFixableSubset_).
        ///< Complements (does not replace) the full-set FFR/ratio/Joseph-hold
        ///< validation; bit-identical to the legacy path when off.
    std::vector<std::string> ar_active_systems;
        ///< Constellations eligible for AR (e.g. {"GPS"}, {"GPS","Galileo"}).
        ///< Empty (default) = all active systems.  Keeps a constellation in the
        ///< float solution while excluding it from the LAMBDA candidate set, for
        ///< a system whose residual model bias lowers the ratio test even though
        ///< its float contribution is usable.
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
 *   2. Form DD ambiguities against one pivot per (constellation, signal) group,
 *      the highest-elevation satellite.
 *   3. Apply MLAMBDA (LDL decomposition + Z-decorrelation + ILS search).
 *   4. Validate: ratio s2/s1 >= threshold (and not above ratio_upper_bound),
 *      bootstrap success rate >= min_success_rate, and per-entry
 *      |float − fixed| <= nl_max_per_pair_frac.
 *   5. Compute the fixed-ambiguity state and covariance.
 *   6. Optionally apply fix-and-hold constraints (see @ref holdAmbiguities for
 *      what the SD path applies).
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
     * With @c wl_only set, or under the NlWl ambiguity basis, the MW integers are
     * applied as pseudo-observations and no LAMBDA search runs.
     *
     * @param state_manager   Current filter state (provides ambiguity indices).
     * @param observations    Modeled observations for this epoch (for elevation, satellite info).
     * @param wl_ambiguities  Resolved SD WL integers from the MW detector.
     *                        Keys are "SAT_SIG1_SIG2" (e.g. "G01_L1C_L2W"),
     *                        values are the rounded SD WL integer.  When the map
     *                        is non-empty, the two-step WL/NL search is used: the
     *                        WL is fixed from MW and only N_1 is searched with
     *                        LAMBDA, one search dimension per satellite pair
     *                        instead of two.  If that search fails, the result
     *                        stays float; there is no fallback to the
     *                        unconstrained per-band search.
     * @param wl_fractions    Per-arc WL fractional residuals (same keys as
     *                        @p wl_ambiguities), used by the @c wl_max_frac gate.
     * @param wl_quality      Per-arc MW quality, used by the @c wl_only_ffr gate.
     * @return AmbiguityResolution result.
     */
    AmbiguityResolution resolve(const StateManager& state_manager,
                                const std::vector<PppObservation>& observations,
                                const std::map<std::string, int>& wl_ambiguities,
                                const std::map<std::string, double>& wl_fractions = {},
                                const std::map<std::string, WlArcQuality>& wl_quality = {});

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
     * Unlike the PPP/PPP-RTK path, no SD subtraction is applied: the
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
     * Feeds the fixed DD ambiguities back as pseudo-observations (variance
     * @c hold_variance, Joseph-form update) to keep the float ambiguities from
     * drifting away from their integer values.  Runs only when @p resolution
     * succeeded and @c ar_mode is 3; a no-op under the NlWl basis.
     *
     * The held set is exactly @c resolution.fixed_dd_rows: the single-signal DD
     * ambiguities the fix constrained, formed from the epoch's observations with
     * their elevation mask and pivots.  A candidate the fix left out (below the
     * mask, outside a partial subset, without a widelane lock) stays float, and a
     * fix that constrained no single-signal DD (WL-only) holds nothing.
     *
     * The PPP callers install @c resolution.fixed_state in the filter before this
     * call, so the pseudo-observation innovation is zero and the hold tightens the
     * covariance of the fixed DD ambiguities without moving their means.
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
     * PPP / PPP-RTK SD path.  Every ambiguity whose key starts with
     * @c "DD|" and whose index falls within @c resolution.fixed_state is held
     * at its @c fixed_state value, including keys excluded from the fix.
     * Runs only when @p resolution succeeded and @c ar_mode is 3.
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
    /// Test seam for @ref nlRowCoefficients (tests/test_nl_search_row.cpp).
    friend struct NlRowTestAccess;
    /// Test seam for @ref wlArcSuccessRate_ (tests/test_wl_only_ffr.cpp).  A field
    /// run can show only that nothing went wrong, not that the WL-only acceptance
    /// gate fails safe, so a unit test pins that behaviour.
    friend struct WlFfrTestAccess;

    // -----------------------------------------------------------------------
    // WL-constrained N_1 search functional
    // -----------------------------------------------------------------------

    /**
     * @brief Coefficients that define which row of the (L1,L2) ambiguity plane
     *        observes the narrow-lane integer N_1, once the widelane is known.
     *
     * All four are exact functions of the frequency pair.  @ref t_1 / @ref t_2
     * carry DD ambiguities from metres into search cycles; @ref shift and
     * @ref bt_2 undo the known-widelane displacement in each direction.
     */
    struct NlRowCoefficients {
        double t_1;    ///< Row entry on the higher-frequency DD [1/m].
        double t_2;    ///< Row entry on the lower-frequency DD [1/m].
        double shift;  ///< y_search = y_reduced + shift·WL ≡ N_1; also N_1 = y_reduced_fixed + shift·WL.
        double bt_2;   ///< N_2 = y_reduced_fixed + bt_2·WL.  Algebraically shift − 1.
    };

    /**
     * @brief Build the N_1 search-row coefficients for one dual-frequency pair.
     *
     * @param f1        Higher frequency [Hz].
     * @param f2        Lower frequency [Hz].
     * @param lam1      Wavelength on @p f1 [m].
     * @param lam2      Wavelength on @p f2 [m].
     * @param iono_free Select the exact iono-free row @f$(f_1^2,-f_2^2)/(f_1^2-f_2^2)@f$
     *                  instead of the narrowlane row @f$(f_1,f_2)/(f_1+f_2)@f$.
     *                  See @ref AmbiguityResolverConfig::nl_iono_free_row.
     * @return The four coefficients; both rows target the same integer N_1.
     *
     * @note Caller must reject a pair with @f$f_1 \le f_2@f$ before requesting
     *       the iono-free row, which divides by @f$f_1-f_2@f$.
     */
    static NlRowCoefficients nlRowCoefficients(double f1, double f2,
                                               double lam1, double lam2,
                                               bool iono_free);

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
     * Used as a Fixed-Failure-Rate-style acceptance criterion: a fix
     * is rejected when @c P_s,boot is below a configured threshold
     * (e.g. 0.999 for 0.1% failure rate target), in addition to the
     * ratio test.  The criterion follows the actual problem geometry
     * rather than a heuristic ratio.
     *
     * @param d Post-reduction LDL diagonal (positive entries; n entries
     *          correspond to n decorrelated ambiguities).
     * @return Bootstrap success rate ∈ [0, 1].  Zero if @p d is empty or any
     *         d_i ≤ 0.
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
     * Ambiguities that are unseeded (value 0), frozen (variance 0), below
     * @c elevation_mask_ar, flagged unresolvable, or outside
     * @c ar_active_systems are excluded first.  A satellite absent from
     * @p observations gets elevation 0.
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
        bool success{ false };       ///< True if a subset (≥1 entry) passed every gate.
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
     * full-set fix: ratio (lower and upper bound), bootstrap success rate
     * (FFR), and per-entry |float − fixed| cap.  On any failure the least
     * fixable entry is dropped (the largest |float − fixed| when the fractional
     * cap is the failing gate, otherwise the largest formal float variance)
     * and the search retries on the smaller subset, down to a single entry.
     * The first subset that passes is accepted; it is not necessarily the
     * largest passing subset.
     *
     * The accepted subset is re-validated by the full gate battery on its own
     * covariance sub-block, so partial AR fixes fewer ambiguities than the full
     * set but never accepts a subset under weaker gates.  PAR therefore
     * complements the existing validation rather than weakening it: a poor
     * elimination order costs fixes, not integrity.
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

    /**
     * @brief Per-arc maturity for an ambiguity key (@c "SAT|SIGNAL").
     *
     * Nothing else in the tree tracks this: @c StateManager registers
     * ambiguities by (satellite, signal) with no birth epoch, and the MW
     * detector's arc state is per-(satellite, channel-pair) and private to the
     * slip layer.
     *
     * @c epochs is the raw age; @c info is the maturity measure the gate uses.
     * An ambiguity's precision is set by accumulated code information, and under
     * the elevation stochastic model each epoch contributes weight
     * @f$\sin^2(\mathrm{el})@f$, so two arcs of equal age are not equally mature:
     * one climbing from the mask and one at zenith differ by ~5x per epoch.
     * Measured: the hold-free log-log slope of sigma against raw age is -0.816
     * rather than the -0.5 of white-noise averaging, and an elevation-weighted
     * limb (@f$\Sigma \propto N^{1.4-1.5}@f$) predicts a slope of about -0.7,
     * most of that gap.  @c info is therefore the maturity axis and @c epochs
     * only a proxy.
     */
    struct ArcState {
        int    epochs{ 0 };   ///< Consecutive epochs since arc birth / last refloat.
        double info{ 0.0 };   ///< Sum of sin^2(elevation) over those epochs.
    };

    /// Per-ambiguity-key arc maturity, maintained by @ref refreshArcAges_.
    std::map<std::string, ArcState> arcs_;

    /**
     * @brief Bump/expire @ref arcs_ against the state vector.  Call once per
     *        epoch, before any candidate is built.
     *
     * An arc ends in two ways, and each resets the counter:
     *   - the key leaves the state vector (set-out / prune), detected by absence;
     *     the entry is dropped, so a re-acquired key starts a new arc;
     *   - the key is refloated in place by @c StateManager::resetAmbiguity, which
     *     zeroes the covariance row/col and re-inflates the variance but keeps
     *     the key.  Counting presence alone would report a freshly refloated arc
     *     as mature; @c isAmbiguityPendingReseed is the marker that restarts it.
     */
    void refreshArcAges_(const StateManager& state_manager,
                         const std::vector<PppObservation>& observations);

    /// Arc age (epochs) of @c "SAT|SIGNAL"; 0 if unknown.
    int arcAgeOf_(const std::string& sat, const std::string& signal) const;

    /// Accumulated sin^2(el) information of @c "SAT|SIGNAL"; 0 if unknown.
    double arcInfoOf_(const std::string& sat, const std::string& signal) const;
    /// Last DD WL integer applied per (sat_i, sat_ref, sig_hi, sig_lo) key.
    /// resolveWlOnly and applyMwWlConstraintsNlWl_ use it to detect when MW has
    /// re-locked at a different integer; the affected ambiguity covariance (the
    /// L1/L2 SD ambiguities, or the non-pivot WL columns under NlWl) is then
    /// inflated so the new constraint takes effect in a single epoch rather than
    /// drifting toward the new integer slowly.
    std::map<std::string, int> previous_dd_wl_;

    /**
     * @brief Two-step WL/NL resolution using known WL integers.
     *
     * For each satellite pair that has DD pairs on two frequencies and a
     * known DD WL integer, the (L1, L2) DD pair is transformed to the
     * NL domain.  LAMBDA runs on the NL-only set: unpaired DD
     * ambiguities without a matching second frequency or WL integer are
     * excluded to avoid contaminating the ratio test.  The fixed NL
     * integers are back-transformed to the original signal domain.  The
     * @c min_arc_info maturity gate applies here.
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
     * float, not a full integer fix.  Useful as a conservative AR mode
     * before enabling the full WL+NL LAMBDA pipeline: WL via MW is robust
     * even when float NL covariance hasn't converged enough for safe
     * integer rounding.
     *
     * Skipped under both convergence gates, as the LAMBDA paths are.
     *
     * Each DD WL integer combines two zero-difference MW arcs (N_i − N_ref);
     * the per-fix confidence gate @c wl_max_frac is applied to both arcs, so
     * a pair is left float this epoch if either arc's running fractional
     * residual exceeds the cap.  This mirrors the gate in
     * @c applyMwWlConstraintsNlWl_ but is stronger by construction (the NlWl
     * counterpart gates a single arc).  The @c wl_only_ffr gate and the
     * @c wl_only_max_innov_cyc screen follow when armed; if they reject every
     * constraint, the result stays float with @c gate_reason 5.
     *
     * @param pairs           DD pairs (from buildDdPairs).
     * @param x               Full state vector.
     * @param P               Full covariance matrix.
     * @param wl_ambiguities  Resolved SD WL integers from the MW detector.
     * @param wl_fractions    Per-arc WL fractional residuals (same keys as
     *                        @p wl_ambiguities); used by the @c wl_max_frac gate.
     * @param observations    Observations for system/frequency lookup.
     * @param wl_quality      Per-arc MW quality; used by the @c wl_only_ffr gate.
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
        const std::vector<PppObservation>& observations,
        const std::map<std::string, WlArcQuality>& wl_quality);

    /**
     * @brief Bootstrap FFR success rate of a set of MW WL constraints, taken
     *        over the distinct arcs backing them.
     *
     * The failure event belongs to the arc, not to the DD: many DD constraints
     * share the pivot arc, so a wrong pivot integer makes every one of them
     * wrong together.  Multiplying a per-DD term would count that shared
     * failure once per DD and understate the joint success, so the product
     * runs over distinct arcs.  Each arc contributes the same per-component
     * term the LAMBDA tier uses, `erf(1/(2√2·σ))`
     * (@ref ils::computeBootstrapSuccessRate), so the two tiers report one
     * comparable quantity.
     *
     * Fail-safe: an arc with no quality entry, an immature arc
     * (`count < wl_only_min_arc_count`), or a non-finite or negative σ
     * contributes 0, so an arc whose failure rate cannot be bounded never
     * reads as reliable.  A pivot marked @c exact contributes 1.  An empty
     * @p arc_keys returns 0.
     *
     * @param arc_keys  distinct MW arc keys backing the constraint set
     * @param wl_quality per-arc quality from the caller
     * @param worst_key  OUT (optional): the arc with the lowest contribution,
     *                   which backward elimination drops next
     * @return Π over arcs of the per-arc rounding success rate, in [0,1]
     */
    double wlArcSuccessRate_(const std::set<std::string>& arc_keys,
                             const std::map<std::string, WlArcQuality>& wl_quality,
                             std::string* worst_key) const;

    /**
     * @brief Apply MW-resolved WL integers in the @c StateAmbiguityBasis::NlWl
     *        basis: one Kalman pseudo-observation row per (sat, band-pair).
     *
     * The WL state column under NlWl is, by construction, the SD wide-lane
     * ambiguity (post-BSSD).  For each (constellation, band-pair) group, the
     * highest-elevation eligible satellite is the AR-internal pivot; for every
     * other satellite this method forms the DD WL integer
     * @c MW(sat_i) − MW(sat_pivot) and applies the constraint
     * @code{.unparsed}
     *     wl_state(sat_i) − wl_state(sat_pivot) = DD_WL_int  cycles
     * @endcode
     * with measurement noise @c config_.wl_only_pseudo_var (cycles²) on
     * a single Kalman row.  The row is expressed through the NL and WL state
     * columns and has 1 to 4 nonzero entries, depending on the band pair, the
     * reference band and whether the AR pivot's columns are BSSD-frozen.  When
     * the AR pivot coincides with the BSSD pivot, that pivot's WL column is
     * zero-covariance and the row reduces to a single state (the diagonal WL
     * constraint of the Phase 4 design).  The @c wl_max_frac gate (one arc),
     * the @c wl_only_ffr gate and the @c wl_only_max_innov_cyc screen apply as
     * in @ref resolveWlOnly.
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
     *         sentinel: no LAMBDA ratio test ran.
     */
    AmbiguityResolution applyMwWlConstraintsNlWl_(
        const StateManager& state_manager,
        const Eigen::VectorXd& x,
        const Eigen::MatrixXd& P,
        const std::map<std::string, int>& wl_ambiguities,
        const std::map<std::string, double>& wl_fractions,
        const std::vector<PppObservation>& observations,
        const std::map<std::string, WlArcQuality>& wl_quality);
};

} // namespace GnssProcessor
