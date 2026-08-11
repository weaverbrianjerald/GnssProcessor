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
/**
 * @brief Quality of one MW-resolved wide-lane arc, as consumed by the WL-only
 *        acceptance gate (@ref AmbiguityResolverConfig::wl_only_ffr).
 *
 * Mirrors @c MwSlipDetector::WlLockQuality without coupling this header to the
 * detector — the caller converts.  @c count travels with σ because a 2-sample
 * arc whose samples happen to agree reports σ ≈ 0, i.e. a success rate of ≈ 1
 * on one degree of freedom.
 */
struct WlArcQuality {
    double      sigma_cycles{ 0.0 };   ///< σ of the MW arc mean (cycles).
    std::size_t count{ 0 };            ///< Welford sample count backing that σ.
    bool        exact{ false };        ///< SD-vs-self pivot: 0 cycles by construction, no uncertainty.
};

struct AmbiguityResolution {
    bool success{ false };             ///< True if ambiguities were successfully fixed.
    /// Ratio test value (s2/s1).  Higher = more confident.
    /// @warning `+inf` is a SENTINEL meaning "no LAMBDA ratio test ran" — it is
    /// set by the WL-only / MW-WL-constraint paths, which fix by applying MW
    /// integers directly.  It is NOT a spectacular ratio, and reading it as one
    /// is how an ungated 35 m wrong fix passed review
    /// (`docs/SPEC_clock_temporal_anchor.md` §8.11).  Grade those fixes on
    /// @ref p_succ, and see @ref AmbiguityResolverConfig::wl_only_ffr.
    double ratio{ 0.0 };
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
    /// Bootstrap success rate of the ACCEPTED fix — the FFR quantity, and the
    /// only confidence number the WL-only paths can report (their @ref ratio is
    /// the `+inf` sentinel).  0 when no fix was accepted or the path did not
    /// compute one.
    double p_succ{ 0.0 };
    /// True when the accepted fix came from a WL-only / MW-WL-constraint path
    /// (MW integers applied directly, no LAMBDA search).  Lets a trace
    /// distinguish "fixed by ILS" from "fixed by asserting MW integers", which
    /// @ref ratio alone cannot.
    bool wl_only_fix{ false };
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
     *        on BOTH constituent arcs before a candidate may enter the search.
     *
     * @c min_epochs is a GLOBAL filter-epoch counter and does not gate individual
     * candidates, so a satellite that rose one minute ago is searched alongside
     * one tracked for four hours.  Measured on a 24 h ALGO run: the worst NL
     * direction is the youngest arc in 85.5% of epochs, and the full-set ratio
     * test consequently fails 98% of the time — one immature candidate spoils
     * the set.
     *
     * Units are epoch-equivalents at zenith (an epoch at 30 deg contributes
     * 0.25).  0 (default) disables the gate and is byte-identical to the
     * pre-gate behaviour.
     *
     * @warning This gate makes AR SAFE, not USEFUL.  It admits candidates only
     * once they are already well determined, which is precisely when the float
     * needs no help — see the caveat in @ref AmbiguityResolver.  It is a
     * wrong-fix guard, not a convergence accelerator.
     */
    double min_arc_info{ 0.0 };
    double max_position_sigma_m{ 0.05 };  ///< Convergence guard: skip AR when 3D position σ exceeds this (m).
    double ratio_upper_bound{ 1e4 };   ///< Reject fixes whose ratio exceeds this (degenerate covariance).
    bool   wl_only{ false };           ///< If true, AR only applies MW-resolved WL integers as Kalman pseudo-observations; no LAMBDA NL search.
    double wl_only_pseudo_var{ 1e-4 }; ///< Pseudo-observation variance (cycles²) for WL-only constraints.  ~0.01 cycle σ.
    double wl_max_frac{ 0.40 };        ///< Max worst-case fractional residual (|frac|+3σ_mean/λ, cyc) for an MW WL to enter AR.
    double min_success_rate{ 0.999 };  ///< Min bootstrap success rate (FFR-style; 0.999 = 0.1% target failure rate).
    /**
     * @brief Apply the bootstrap FFR gate to the WL-ONLY / MW-WL constraint
     *        paths, which historically had no success-rate gate at all.
     *
     * `wl_max_frac` is a POINT test on each arc's running residual — it says
     * the locked integer looks self-consistent this epoch.  It cannot bound
     * P(the integer is wrong), and that is the quantity an acceptance decision
     * has to control.  With this armed, @ref AmbiguityResolver::resolveWlOnly
     * and @ref AmbiguityResolver::applyMwWlConstraintsNlWl_ run the SAME
     * `erf(1/(2√2·σ))` bootstrap product the LAMBDA tier runs
     * (@ref ils::computeBootstrapSuccessRate) against
     * @ref min_success_rate, dropping the weakest arc until the set passes.
     *
     * The product is taken over DISTINCT ARCS, not over DD constraints: many
     * DDs share the pivot arc, so a wrong pivot makes all of them wrong
     * together, and multiplying per-DD terms would double-count the shared
     * failure.
     *
     * Default ON.  This closes a fail-unsafe path — measured on
     * `runs/cta4_q1e7`, an ungated 5-constraint WL-only fix put the held-out
     * consumer 35 m off with `ratio` reporting the "no LAMBDA ran" sentinel
     * `+inf` (`docs/SPEC_clock_temporal_anchor.md` §8.11).  Set false to
     * reproduce the pre-fix behaviour exactly.
     */
    bool   wl_only_ffr{ true };
    /// Minimum MW arc sample count for an arc to carry a WL constraint under
    /// @ref wl_only_ffr.  An arc with 2 samples has a σ estimate with ~1 degree
    /// of freedom; admitting it would let a meaningless variance buy a fix.
    int    wl_only_min_arc_count{ 10 };
    /**
     * @brief Max |asserted WL integer − predicted DD widelane| (cycles) for a
     *        WL-only constraint to be applied.  0 disables the check.
     *
     * The direct analogue of @ref nl_max_per_pair_frac, which the LAMBDA tier
     * applies per entry and the WL-only path had no equivalent of: it asserted
     * MW integers at `wl_only_pseudo_var` (σ ≈ 0.01 cycle) no matter how far the
     * current float disagreed.
     *
     * **This is the gate the FFR gate cannot be.**  A bootstrap success rate
     * bounds the RANDOM failure of rounding an arc — it is computed from the
     * arc's own dispersion and is entirely correct in saying a mature arc's
     * widelane is precisely known.  It is blind to a SYSTEMATIC disagreement
     * between that integer and the filter's frame, because nothing about the
     * arc's dispersion knows which frame the filter is in.  The MW widelane
     * lives in the .BIA frame while a network-bias consumer's ambiguities live
     * in the consumed phase-bias frame (the conflict `setArIgnoreMwWl` exists
     * to avoid), and asserting across that gap is what put a held-out consumer
     * 35 m off with `p_succ = 1.0` — a perfectly confident, perfectly wrong fix
     * (`docs/SPEC_clock_temporal_anchor.md` §8.11).
     *
     * Default 0.5 cycle: half a widelane is the largest disagreement that can
     * still be a rounding question rather than a frame question.
     */
    double wl_only_max_innov_cyc{ 0.5 };
    double nl_max_per_pair_frac{ 0.15 }; ///< Max |N_1_float − N_1_fixed| per NL pair / DD; rejects marginal fixes.
    bool   nl_iono_free_row{ false };
        ///< Search functional for the WL-constrained N_1 step (@ref
        ///< AmbiguityResolver::resolveWlConstrained).  Both choices target the
        ///< SAME integer N_1 and differ only in which row of the (L1,L2) plane
        ///< is used to observe it:
        ///<
        ///< - @c false (default, legacy): the NARROWLANE row
        ///<   @f$(f_1,f_2)/(f_1{+}f_2)@f$.  Cheap in noise, but it is NOT
        ///<   iono-free — its slant-iono sensitivity is
        ///<   @f$T\cdot\alpha = 5.907@f$ NL cyc per metre of slant iono
        ///<   (0.959 cyc/TECU), so a 0.15-cycle @ref nl_max_per_pair_frac cap
        ///<   implicitly demands the ionosphere to 0.156 TECU.
        ///< - @c true: the exact IONOSPHERE-FREE row
        ///<   @f$(f_1^2,-f_2^2)/(f_1^2-f_2^2)@f$, for which
        ///<   @f$c_{IF}\cdot\alpha = 0@f$ EXACTLY — the N_1 target carries no
        ///<   slant-iono error at all.
        ///<
        ///< @warning Not free.  Both the row norm and the widelane sensitivity
        ///< grow by exactly @f$(f_1{+}f_2)/(f_1{-}f_2) = 8.06@f$ (GPS L1/L2):
        ///< a 1-cycle WL error shifts the N_1 target by 3.53 cyc instead of
        ///< 0.438, and the searched covariance inflates ~65× in variance.  This
        ///< trades an ionospheric BIAS for widelane/noise AMPLIFICATION and is
        ///< only a win with a near-perfect widelane — arm it together with WL
        ///< hardening, never alone.  Byte-identical to legacy when off.
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
    /// Test seam for @ref nlRowCoefficients (tests/test_nl_search_row.cpp).
    friend struct NlRowTestAccess;
    /// Test seam for @ref wlArcSuccessRate_ (tests/test_wl_only_ffr.cpp) — the
    /// WL-only acceptance gate's fail-safe behaviour is exactly the property a
    /// field run cannot demonstrate (it can only show that nothing went wrong),
    /// so it is pinned here.
    friend struct WlFfrTestAccess;

    // -----------------------------------------------------------------------
    // WL-constrained N_1 search functional
    // -----------------------------------------------------------------------

    /**
     * @brief Coefficients that define which row of the (L1,L2) ambiguity plane
     *        observes the narrow-lane integer N_1, once the widelane is known.
     *
     * All four are exact functions of the frequency pair.  @ref t_1 / @ref t_2
     * carry DD ambiguities from METRES into search cycles; @ref shift and
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
     *       the iono-free row — it divides by @f$f_1-f_2@f$.
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

    /**
     * @brief Per-arc maturity for an ambiguity key (@c "SAT|SIGNAL").
     *
     * Nothing else in the tree tracks this — @c StateManager registers
     * ambiguities by (satellite, signal) with no birth epoch, and the MW
     * detector's arc state is per-(satellite, channel-pair) and private to the
     * slip layer.
     *
     * @c epochs is the raw age.  @c info is the load-bearing one: an
     * ambiguity's precision is set by ACCUMULATED CODE INFORMATION, and under
     * the elevation stochastic model each epoch contributes weight
     * @f$\sin^2(\mathrm{el})@f$.  Two arcs of equal age are NOT equally mature —
     * one climbing from the mask and one sitting at zenith differ by ~5x per
     * epoch.  Measured: the hold-free log-log slope of sigma vs raw AGE is
     * -0.816 rather than the -0.5 of white-noise averaging, and an
     * elevation-weighted limb (@f$\Sigma \propto N^{1.4-1.5}@f$) predicts
     * about -0.7 of that gap.  So @c info is the right maturity axis and
     * @c epochs is only a proxy.
     */
    struct ArcState {
        int    epochs{ 0 };   ///< Consecutive epochs since arc birth / last refloat.
        double info{ 0.0 };   ///< Sum of sin^2(elevation) over those epochs.
    };

    /// Per-ambiguity-key arc maturity, maintained by @ref refreshArcAges_.
    std::map<std::string, ArcState> arcs_;

    /**
     * @brief Bump/expire @ref arcs_ against the state vector.  Call ONCE per
     *        epoch, before any candidate is built.
     *
     * Two ways an arc ends, and BOTH must reset the counter:
     *   - the key leaves the state vector (set-out / prune) — detected by absence;
     *   - the key is REFLOATED IN PLACE by @c StateManager::resetAmbiguity, which
     *     zeroes the covariance row/col and re-inflates the variance but KEEPS
     *     the key (StateManager.cpp:1150-1160).  Presence-counting alone misses
     *     this and logs a freshly refloated arc as fully mature — the defect this
     *     signature exists to close.  @c isAmbiguityPendingReseed is the marker.
     */
    void refreshArcAges_(const StateManager& state_manager,
                         const std::vector<PppObservation>& observations);

    /// Arc age (epochs) of @c "SAT|SIGNAL"; 0 if unknown.
    int arcAgeOf_(const std::string& sat, const std::string& signal) const;

    /// Accumulated sin^2(el) information of @c "SAT|SIGNAL"; 0 if unknown.
    double arcInfoOf_(const std::string& sat, const std::string& signal) const;
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
        const std::vector<PppObservation>& observations,
        const std::map<std::string, WlArcQuality>& wl_quality);

    /**
     * @brief Bootstrap FFR success rate of a set of MW WL constraints, taken
     *        over the DISTINCT ARCS backing them.
     *
     * The failure event lives on the ARC, not on the DD: many DD constraints
     * share the pivot arc, so a wrong pivot integer makes every one of them
     * wrong together.  Multiplying a per-DD term would count that shared
     * failure once per DD and understate the joint success — the product must
     * run over distinct arcs.  Each arc contributes the same per-component
     * term the LAMBDA tier uses, `erf(1/(2√2·σ))`
     * (@ref ils::computeBootstrapSuccessRate), so the two tiers report one
     * comparable quantity.
     *
     * FAIL-SAFE: an arc with no quality entry, an immature arc
     * (`count < wl_only_min_arc_count`), or a non-finite σ contributes 0 —
     * "we cannot bound this arc's failure rate" must never read as "it is
     * fine".  A pivot marked @c exact contributes 1.
     *
     * @param arc_keys  distinct MW arc keys backing the constraint set
     * @param wl_quality per-arc quality from the caller
     * @param worst_key  OUT (optional): the arc with the LOWEST contribution —
     *                   the one backward elimination should drop next
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
        const std::vector<PppObservation>& observations,
        const std::map<std::string, WlArcQuality>& wl_quality);
};

} // namespace GnssProcessor
