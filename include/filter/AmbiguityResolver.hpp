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
#include "filter/AmbiguityResolverStages.hpp"
#include <Eigen/Dense>
#include <string>
#include <vector>
#include <map>
#include <optional>
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
    /// 35 m wrong fix passed review that way (`docs/record/SPEC_clock_temporal_anchor.md`
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

    /**
     * @brief Satellites whose ambiguities the caller refloats after this epoch's
     *        fix is applied (@ref AmbiguityResolver::refloatSatellites).
     *
     * Filled by @ref AmbiguityResolverConfig::hold_release_cyc (a held pair the
     * float contradicts) and @ref AmbiguityResolverConfig::refloat_partner_signal
     * (one signal of a satellite restarted while the other carried on), whether or
     * not a fix was accepted.  Empty with both off.
     */
    std::vector<std::string> release_satellites;

    /**
     * @brief Satellites whose receiver-satellite code-bias states the caller returns
     *        to their prior (@ref StateManager::resetRxSatCodeBiasStatesToPrior).
     *
     * Filled by the wide-lane decision of
     * @ref AmbiguityResolverConfig::wl_branch_check: a satellite pair on which the
     * code, with the bias states it carries, admitted one wide-lane integer while
     * the ionosphere-free phase was decisive for the neighbouring one is a
     * contradiction, and the satellites at fault are chosen as the held-pair
     * release does (the reference when it is contradicted in at least two pairs that
     * are the majority of its code-admitted pairs this epoch, else the other
     * satellite).  Filled whether or not a fix was accepted.  The callers act on it
     * only when the receiver-satellite code-bias family is on.  Empty with the key off.
     */
    std::vector<std::string> bias_reset_satellites;

    /// One extra signal of a held satellite pair
    /// (@ref AmbiguityResolverConfig::extra_signals): its integer and its extra-wide lane
    /// against the lower primary signal.
    struct HeldExtraFix {
        std::string signal;     ///< The extra signal (for example "L5Q").
        double lam{ 0.0 };      ///< Wavelength of the extra signal [m].
        int ewl{ 0 };           ///< Extra-wide-lane integer E = N_lo - N_x (cycles).
        int n{ 0 };             ///< DD integer of the extra signal, N_x = N_lo - E.
    };

    /// One satellite pair the accepted WL-constrained fix held, as the integers
    /// @ref AmbiguityResolver remembers for @ref AmbiguityResolverConfig::hold_release_cyc.
    struct HeldPairFix {
        std::string sat_i;      ///< Non-reference satellite.
        std::string sat_ref;    ///< Reference satellite.
        std::string sig_hi;     ///< Signal on the higher frequency.
        std::string sig_lo;     ///< Signal on the lower frequency.
        double f1{ 0.0 };       ///< Frequency of @c sig_hi [Hz].
        double f2{ 0.0 };       ///< Frequency of @c sig_lo [Hz].
        double lam1{ 0.0 };     ///< Wavelength of @c sig_hi [m].
        double lam2{ 0.0 };     ///< Wavelength of @c sig_lo [m].
        int n1{ 0 };            ///< DD integer on the higher frequency.
        int wl{ 0 };            ///< DD widelane integer (N_1 - N_2).
        /// The extra signals fixed with the pair (empty with @c extra_signals off).
        std::vector<HeldExtraFix> extras;
    };
    /// Pairs the accepted fix held (filled by the WL-constrained path only when
    /// @ref AmbiguityResolverConfig::hold_release_cyc > 0; committed to memory by
    /// @ref AmbiguityResolver::resolve once the fix is final).
    std::vector<HeldPairFix> held_pairs;

    /// How the call ended (@ref ArEnding): set on the paths that reach the WL-constrained
    /// search, @c None elsewhere.  A failed search keeps its ending although its other
    /// fields are those of an empty result.
    ArEnding ending{ ArEnding::None };
    /// The ratio s2/s1 the WL-constrained search computed, also when the search failed;
    /// 0 when no search computed one.  On a partial fix it is the full set's ratio, while
    /// @ref ratio is the accepted subset's.
    double search_ratio{ 0.0 };
    /// Satellite pairs among the epoch's DD pairs (held census; 0 when the census did not run).
    int n_sat_pairs{ 0 };
    /// Of them, held: at least two signals' DD ambiguities at hold scale (@ref isHeldDdVariance).
    int n_held_pairs{ 0 };
    /// Satellites that joined the held set at this call (held census).
    int held_joined{ 0 };
    /// Satellites that left the held set at this call, still observed or absent (held census).
    int held_left{ 0 };
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
     * (AmbiguityResolver::resolveWlConstrained (@c GnssProcessor::AmbiguityResolver::resolveWlConstrained)), on the higher-frequency arcs of
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
     * control.  With this on, AmbiguityResolver::resolveWlOnly (@c GnssProcessor::AmbiguityResolver::resolveWlOnly) and
     * AmbiguityResolver::applyMwWlConstraintsNlWl_ (@c GnssProcessor::AmbiguityResolver::applyMwWlConstraintsNlWl_) evaluate the same
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
     * sentinel `+inf` (`docs/record/SPEC_clock_temporal_anchor.md` §8.11).  Set false to
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
     * `p_succ = 1.0` (`docs/record/SPEC_clock_temporal_anchor.md` §8.11).
     *
     * Default 0.5 cycle: half a widelane is the largest disagreement that can
     * still be a rounding question rather than a frame question.
     */
    double wl_only_max_innov_cyc{ 0.5 };
    double nl_max_per_pair_frac{ 0.15 }; ///< Max |N_1_float − N_1_fixed| per NL pair / DD; rejects marginal fixes.
    bool   nl_iono_free_row{ false };
        ///< Search functional for the WL-constrained N_1 step (@c
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
    /**
     * @brief Take the WL-constrained search's widelane integers from the float
     *        states instead of the MW detector.
     *
     * With this off (default) @ref AmbiguityResolver::resolveWlConstrained imposes
     * the MW detector's DD widelane integer.  The MW detector works on raw
     * observables corrected only by the satellite biases, so it omits every term
     * that differs between the two frequencies and is not in the bias file: the
     * satellite antenna offset per frequency above all.  For a GPS Block III
     * satellite (L1 and L2 z offsets 1.116 m and 0.612 m in igs20.atx) that term
     * moves the MW widelane by about 2.3 cycles, so the imposed integer is 2
     * cycles wrong on every pair with such a satellite, and the narrow-lane search
     * cannot see it (a 2-cycle WL error moves the N_1 target by 7.06 cycles on the
     * iono-free row, a fraction of 0.06).
     *
     * With this on, each candidate's widelane is the float states' own
     * @f$w = DD_1/\lambda_1 - DD_2/\lambda_2@f$, with its formal sigma
     * @f$\sigma_w@f$ from the same covariance; it carries every model term the
     * filter applies.  A candidate enters only if
     * @f$|w - \mathrm{round}(w)| + 3\sigma_w \le@f$ @ref wl_max_frac (the same
     * worst-case form the MW path uses), and then with @f$\mathrm{round}(w)@f$.
     * The route also runs when the MW detector supplies no integers (no .BIA
     * file, as on correction streams).  Byte-identical when off.
     */
    bool   wl_from_float{ false };
    /**
     * @brief Let the ionosphere-free phase decide the wide-lane integer where the
     *        code cannot, and name the pairs on which the two disagree.  Off by
     *        default.
     *
     * Needs @ref wl_from_float and @ref nl_iono_free_row; the resolver treats the
     * key as off when either is missing.  For each satellite pair the float wide
     * lane @c w has a nearest integer W0 and a second nearest W1.  One wide-lane
     * cycle moves the N_1 target of the iono-free row by a shift whose distance
     * from an integer (the branch gap, 0.471 cycle for GPS L1/L2) says how far the
     * two integers are apart in the phase.  When the phase of the iono-free row,
     * with its formal sigma and the narrow-lane error floor, lies within half a gap
     * of an integer N_1 for one of the two wide-lane integers, it is decisive for
     * that one.  The decision of one pair at one epoch is:
     *
     * - three integers within reach of the code sigma (@c 1 + |d| <= 3 sigma_w):
     *   not admitted;
     * - the code admits W0 (@c |d| + 3 sigma_w <= @ref wl_max_frac) and the phase is
     *   decisive for W1: a contradiction.  Neither integer is taken and the pair is
     *   not admitted;
     * - the code admits and the phase is decisive for W0 or for neither: W0, as
     *   without the key;
     * - the code does not admit and the phase is decisive for W0: W0;
     * - the code does not admit, the phase is decisive for W1 and W1 is a candidate
     *   (@c 1 - |d| <= 3 sigma_w, the code cannot exclude the neighbour): W1;
     * - otherwise not admitted.
     *
     * A decision by the phase (W0 or W1) and a contradiction only take effect once
     * the phase has been decisive for the same integer of the same pair for at least
     * @ref wl_branch_persist_s.  A persisted contradiction is the event: the
     * satellites at fault are returned in
     * @ref AmbiguityResolution::bias_reset_satellites and the pair stays not admitted.
     *
     * For a frequency pair with a small gap (Galileo E1/E5a, 0.051 cycle) the phase
     * is never decisive and the decision is the one without the key.  A pair
     * admitted by any row enters the maturity gate, the N_1 search, the ratio test,
     * the per-pair cap and the hold as before.  Each evaluated pair logs a
     * @c WLBRANCH line.
     */
    bool   wl_branch_check{ false };
    /**
     * @brief Fix the signals of a satellite pair beyond its two primary signals.
     *        Off by default.
     *
     * Today a satellite pair tracked on more than two signals is skipped whole.  With
     * this on, the two signals of the pair on the constellation's @ref primary_bands go
     * through the wide-lane and N_1 step exactly as a two-signal pair does (admission,
     * wide-lane branch check, search, validation).  Every other signal x of the pair is
     * an extra signal: its extra-wide lane against the lower-frequency primary signal,
     * @f$e = n_{lo} - n_x@f$ in cycles of the float between-satellite ambiguities, is
     * admitted when @f$|e - \mathrm{round}(e)| + 3\sigma_e \le@f$ @ref wl_max_frac.
     * Extra signals do not enter the N_1 search.  When the primary pair is fixed by an
     * accepted search (full or partial), each admitted extra signal gets
     * @f$N_x = N_{lo} - \mathrm{round}(e)@f$ with @f$N_{lo} = N_1 - W@f$, and its
     * between-satellite ambiguity is held with the same pseudo-observation variance as
     * the primary signals.  With @ref hold_release_cyc the held extra-wide lane is
     * compared with the float as the wide lane is, and a contradiction releases the
     * pair's satellite through the existing path.  A constellation without a
     * @ref primary_bands entry keeps the rule that a pair needs exactly two signals.
     *
     * Needs @ref wl_from_float; the loader treats the key as off without it.  Each
     * evaluated extra signal logs an @c EWLFLOAT line.
     */
    bool   extra_signals{ false };
    /**
     * @brief Per constellation, the two RINEX band digits of the primary signals.
     *
     * Key: constellation name as @c systemName spells it ("GPS", "Galileo", "BeiDou").
     * Value: two distinct digits, for example "15" for Galileo E1 and E5a.  The primary
     * signals are the signals of a satellite pair whose band digit (the second
     * character of the signal name) is one of the two; they carry the wide-lane and N_1
     * step, and are the bands the phase-bias products give biases for.  Read only with
     * @ref extra_signals.
     */
    std::map<std::string, std::string> primary_bands{ defaultArPrimaryBands() };
    /**
     * @brief Seconds the iono-free phase must stay decisive for one wide-lane integer
     *        of one satellite pair before @ref wl_branch_check acts on it.  Default 120.
     *
     * The float of a pair can leave its true value in one epoch (a change of
     * reference satellite, a hold on other pairs) with a formal sigma that does not
     * show it, and the phase then looks decisive for the wrong integer.  Requiring
     * the same integer over this time lets such a transient pass.  The run is kept
     * per pair (satellite, reference, higher and lower signal) and restarts when the
     * integer changes, when the phase is decisive for neither integer, or when the
     * time since the last decisive epoch exceeds the larger of this value and 1 s.
     * 0 acts at once.  Used only with @ref wl_branch_check; must be >= 0.
     */
    double wl_branch_persist_s{ 120.0 };
    /**
     * @brief Error floor (narrow-lane cycles) added to the covariance the
     *        WL-constrained N_1 search uses.  0 disables it (default).
     *
     * The filter's formal narrow-lane sigma keeps shrinking as if every epoch
     * brought independent information, while the actual miss levels off at the
     * size of the time-correlated phase errors (multipath, troposphere and
     * antenna mismodelling, product residuals).  On CODE final products the miss
     * levels at about 0.045 cycle with the position estimated while the formal
     * sigma falls to 0.022, so the ratio test, the bootstrap success rate and the
     * per-pair cap all see an over-confident covariance.
     *
     * With a floor f > 0 the searched covariance becomes
     * @f$Q + \tfrac{f^2}{2}(I + M)@f$, where @f$M_{kl} = 1@f$ when candidates k
     * and l share their reference satellite: one per-satellite error of variance
     * @f$f^2/2@f$, so each DD gets @f$f^2@f$ and DDs on a common reference
     * correlate as the reference's error does.  Applied to the search only; the
     * filter state and the fix's pseudo-observation are unchanged.
     */
    double nl_sigma_floor_cyc{ 0.0 };
    /**
     * @brief Release threshold for a held satellite pair [cycles].  0 disables it
     *        (default).
     *
     * An accepted WL-constrained fix is held: its pseudo-observation (variance
     * @ref hold_variance) stays in the filter state, and nothing released it when
     * the data later disagreed.  On BAKE00CAN, 25 May 2026, a pair fixed to the
     * wrong widelane after a single-signal refloat stayed held while the float sat
     * 0.7 to 0.9 widelane cycle from it for half an hour.
     *
     * With a threshold t > 0 the resolver remembers each held pair's integers
     * (N_1 and the widelane) and, every epoch the search runs, compares them with
     * the float state: the widelane DD_1/lambda_1 - DD_2/lambda_2 against the held
     * widelane, and the ionosphere-free combination against the held one in
     * narrow-lane cycles (lambda_NL = c/(f_1+f_2)).  Either difference above t
     * contradicts the hold.  The satellite at fault is refloated (both signals,
     * through @ref AmbiguityResolution::release_satellites): the reference when at
     * least two of its held pairs and more than half of them contradict, else the
     * pair's non-reference satellite.  Its pairs leave this epoch's search and their
     * holds are forgotten.  A hold is also forgotten when an arc of either
     * satellite on either signal restarts.  Each release is logged (HOLDREL) and
     * counted.
     */
    double hold_release_cyc{ 0.0 };
    /**
     * @brief Refloat the other signal of a satellite when one signal's arc
     *        restarts while the other carries on.  Off by default.
     *
     * The filter's phase-outlier response refloats the flagged row only.  With a
     * free slant ionosphere and the satellite's other signal held tight, the next
     * epochs re-determine the refloated ambiguity through the ionosphere from the
     * kept signal, so a slip on the kept signal (or one common to both) lands in
     * the refloated ambiguity as a non-integer offset with a millimetre sigma.  On
     * BAKE00CAN, 25 May 2026 03:37, G31 L1W was refloated alone and re-settled
     * 0.155 m away at a 3 mm sigma one epoch later; both runs then fixed the pair
     * G06-G31 to widelane 3 against an independent widelane of 4.
     *
     * With this on, a signal of a satellite whose ambiguity begins anew (its
     * continuity generation changes: birth, a refloat, or a discontinuity that the
     * post-fit screen records on its own copies of the state) while another signal
     * of that satellite has kept its generation for at least three epochs puts the
     * satellite in
     * @ref AmbiguityResolution::release_satellites, so both signals restart
     * together.  Satellites whose signals start together (a rising satellite, the
     * start of the run) are untouched.  Each refloat is logged (PARTNERREL) and
     * counted.
     */
    bool   refloat_partner_signal{ false };
    bool   enable_partial_ar{ false };   ///< If true, when the full-set LAMBDA fix fails its
        ///< acceptance gates, retry by greedy backward elimination (drop the least
        ///< fixable ambiguity, re-validate ratio/bootstrap/frac) and accept the first
        ///< subset that passes, so a few noisy satellites do not force the whole
        ///< epoch back to float (@c AmbiguityResolver::selectFixableSubset_).
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

/// The wide-lane integer the decision of @ref AmbiguityResolverConfig::wl_branch_check chose.
enum class WlBranchChoice {
    None,   ///< The pair is not admitted.
    W0,     ///< The nearest integer of the float wide lane.
    W1      ///< The second nearest integer (one cycle away, on the side of the float).
};

/// Why @ref decideWlBranch chose what it chose (the @c reason word of the @c WLBRANCH line).
enum class WlBranchReason {
    NotFinite,      ///< The float wide lane or its sigma is not finite.
    Reach,          ///< Three integers lie within three sigma of the float wide lane.
    Code,           ///< The code admits and the phase does not contradict it: W0.
    Contradiction,  ///< The code admits W0 and the phase is decisive for W1: not admitted; a contradiction once persisted.
    PhaseW0,        ///< The code does not admit; the phase is decisive for W0.
    PhaseW1,        ///< The code does not admit; the phase is decisive for W1 and W1 is a candidate.
    Undecided,      ///< The code does not admit and the phase is not decisive (or decisive for a W1 that is no candidate).
    Waiting         ///< A decision by the phase is held back until it has persisted (never returned by @ref decideWlBranch).
};

/// The outcome of @ref decideWlBranch for one satellite pair.
struct WlBranchDecision {
    WlBranchChoice integer{ WlBranchChoice::None };   ///< The chosen integer.
    WlBranchReason reason{ WlBranchReason::Undecided }; ///< The row of the decision table that fired.
    bool           code_admits{ false };              ///< The code's own test (|d| + 3 sigma_w <= wl_max_frac) passed.
    WlBranchChoice phase{ WlBranchChoice::None };     ///< The integer the phase is decisive for, or None.
    bool           w1_candidate{ false };             ///< The code does not admit and cannot exclude W1 (@c 1 - |d| <= 3 sigma_w).
};

/**
 * @brief The wide-lane decision of one satellite pair by the code and the
 *        ionosphere-free phase (@ref AmbiguityResolverConfig::wl_branch_check).
 *
 * A pure function of the pair's numbers, so the decision table is testable without a
 * filter and without state.  With @c W0 = round(w), @c d = w - W0 and @c W1 = W0 + 1 for
 * d >= 0 (else W0 - 1), evaluated in this order:
 *
 *  1. @p w or @p sigma_w not finite: not admitted (@c NotFinite).
 *  2. @c 1 + |d| <= 3 sigma_w: not admitted (@c Reach).
 *  3. The code admits (@c |d| + 3 sigma_w <= @p wl_max_frac); W1 is then never a candidate:
 *     a. the phase is decisive for W1: no integer this epoch (@c Contradiction);
 *     b. otherwise W0 (@c Code).
 *  4. The code does not admit:
 *     a. the phase is decisive for W0: W0 (@c PhaseW0);
 *     b. the phase is decisive for W1 and W1 is a candidate (@c 1 - |d| <= 3 sigma_w):
 *        W1 (@c PhaseW1);
 *     c. otherwise not admitted (@c Undecided).
 *
 * The result is the provisional decision of one epoch.  Rows 3a, 4a and 4b take effect
 * only after the phase has persisted (@ref stepWlBranchPersistence).
 *
 * The phase is decisive for an integer when @c |r| + 3 sigma_n <= gap / 2, with @c r
 * the fraction of its N_1 float.  For fractions that belong to one float (they differ
 * by the wide-lane shift) it cannot be decisive for both integers; for inputs that
 * claim it is, the function treats the phase as decisive for neither.
 *
 * @param w           Float wide lane of the pair [cycles].
 * @param sigma_w     Formal sigma of @p w [cycles].
 * @param r0          Fraction (N_1 float minus its nearest integer) for W0 [cycles].
 * @param r1          The same for W1 [cycles].
 * @param sigma_n     Sigma of the N_1 float of the iono-free row, with the
 *                    narrow-lane error floor in quadrature [cycles].
 * @param gap         Branch gap, the distance of the wide-lane shift of the N_1
 *                    float from an integer [cycles].
 * @param wl_max_frac The code's admission limit (@ref AmbiguityResolverConfig::wl_max_frac).
 * @return The chosen integer, the reason, the code's own verdict and the phase's integer.
 */
WlBranchDecision decideWlBranch(double w, double sigma_w, double r0, double r1,
                                double sigma_n, double gap, double wl_max_frac);

/// The reason of a decision as the one short word of the @c WLBRANCH line.
const char* wlBranchReasonWord(WlBranchReason reason);

/// The run of one satellite pair over which the phase has been decisive for one integer.
struct WlBranchRun {
    long   integer{ 0 };      ///< The wide-lane integer the phase is decisive for.
    double began_tow{ 0.0 };  ///< Epoch time the current run began [s].
    double last_tow{ 0.0 };   ///< Epoch time of the last decisive evaluation [s].
};

/// The outcome of one @ref stepWlBranchPersistence.
struct WlBranchPersistStep {
    bool        keep{ false };       ///< False: erase the entry; true: store @ref run.
    WlBranchRun run;                 ///< The updated entry (valid when @ref keep).
    bool        ripe{ false };       ///< The run has reached the persistence time.
    double      persisted_s{ 0.0 };  ///< Seconds the phase has been decisive for this integer (0 when not).
};

/**
 * @brief One step of the persistence of the phase's decision for a satellite pair.
 *
 * A pure function of the stored run, this epoch's evaluation and the clock.  When the
 * phase is not decisive the entry is erased.  When it is decisive for @p integer the run
 * continues if the stored entry holds the same integer and the time since its last
 * decisive evaluation lies in [0, max(@p persist_s, 1 s)], otherwise it restarts at
 * @p tow (a negative time since the last evaluation, as at a week rollover, restarts it
 * too).  The step is ripe when the run has lasted @p persist_s; 0 is ripe at once.
 *
 * @param entry     The pair's stored run, or none.
 * @param decisive  True when the phase is decisive for an integer this epoch.
 * @param integer   That integer (used only when @p decisive).
 * @param tow       Epoch time [s].
 * @param persist_s Persistence time [s], >= 0.
 * @return The updated entry or its erasure, whether it is ripe, and the run length.
 */
WlBranchPersistStep stepWlBranchPersistence(const std::optional<WlBranchRun>& entry,
                                            bool decisive, long integer,
                                            double tow, double persist_s);

/// The run counters of the wide-lane decision (@ref AmbiguityResolverConfig::wl_branch_check).
struct WlBranchCounters {
    long long evaluated{ 0 };            ///< Pairs the decision evaluated.
    long long by_phase{ 0 };             ///< Pairs admitted only by the phase, after it persisted (rows PhaseW0 and PhaseW1).
    long long contradictions{ 0 };       ///< Persisted contradictions (the code admitted W0, the phase stayed decisive for W1).
    long long waiting{ 0 };              ///< Pair-epochs held back because the phase had not yet persisted.
    long long refused_reach{ 0 };        ///< Pairs refused because three integers lie within reach.
    long long satellites_returned{ 0 };  ///< Satellite returns for a code-bias reset (one per satellite per epoch).
};

/// The outcome of the admission of one extra-wide lane (@ref admitExtraWideLane).
struct ExtraWideLaneAdmission {
    bool admitted{ false };   ///< The extra-wide lane is close enough to an integer.
    long integer{ 0 };        ///< round(e); meaningful only when @c admitted.
};

/**
 * @brief Admission of one extra-wide lane (@ref AmbiguityResolverConfig::extra_signals).
 *
 * The extra-wide lane @p e of a signal against the lower primary signal is a difference
 * of two cycle counts, so it is an integer in the absence of errors and needs no
 * division by a frequency difference: unlike the iono-free N_1 row, two nearby
 * frequencies cannot amplify an error here, and no amplification guard is applied.
 * Admitted when both numbers are finite and @c |e - round(e)| + 3 sigma_e <= wl_max_frac.
 *
 * @param e            Float extra-wide lane [cycles].
 * @param sigma_e      Its formal sigma [cycles].
 * @param wl_max_frac  The admission limit (@ref AmbiguityResolverConfig::wl_max_frac).
 * @return Whether it is admitted, and round(e).
 */
ExtraWideLaneAdmission admitExtraWideLane(double e, double sigma_e, double wl_max_frac);

/// One signal of a satellite pair, as @ref splitPrimaryAndExtra reads it.
struct PairSignal {
    std::string signal;   ///< Signal name; its second character is the RINEX band digit ("L5Q" is band 5).
    double      freq{ 0.0 }; ///< Carrier frequency [Hz].
};

/// How @ref splitPrimaryAndExtra divides the signals of one satellite pair.
struct PrimaryExtraSplit {
    bool             usable{ false };  ///< The pair has its primary two signals and is processed.
    int              hi{ -1 };         ///< Index (in the input) of the higher-frequency primary signal.
    int              lo{ -1 };         ///< Index of the lower-frequency primary signal.
    std::vector<int> extras;           ///< Indices of every other signal, in input order.
};

/**
 * @brief Split the signals of one satellite pair into its primary two and the extras
 *        (@ref AmbiguityResolverConfig::extra_signals).
 *
 * With a @p primary_bands entry for @p system_name, the primary signals are the first
 * signal on each of the entry's two band digits; every other signal is an extra.  The
 * pair is usable only when both primary signals exist.  Without an entry for the
 * constellation (or with an empty table, the key-off call) the pair is usable only when
 * it has exactly two signals, both primary, as before the key existed.  The higher
 * frequency of the two primary signals is @c hi (on equal frequencies the second input).
 *
 * @param system_name    Constellation name as @c systemName spells it.
 * @param signals        The pair's signals.
 * @param primary_bands  Constellation name to two band digits.
 * @return The split; @c usable false when the pair is skipped.
 */
PrimaryExtraSplit splitPrimaryAndExtra(const std::string& system_name,
                                       const std::vector<PairSignal>& signals,
                                       const std::map<std::string, std::string>& primary_bands);

/// The run counters of the extra signals (@ref AmbiguityResolverConfig::extra_signals).
struct ExtraSignalCounters {
    long long evaluated{ 0 };         ///< Extra-wide lanes evaluated (one per extra signal per pair-epoch).
    long long admitted{ 0 };          ///< Of those, admitted.
    long long fixed{ 0 };             ///< Extra signals given an integer by an accepted fix.
    long long releases_caused{ 0 };   ///< Held pairs contradicted by a held extra-wide lane (counted per pair-epoch).
};

// ---------------------------------------------------------------------------
// Conditioning of a search covariance (the QBCOND diagnostic)
// ---------------------------------------------------------------------------

/**
 * @brief Eigenvalue spread of a cycle-domain ambiguity covariance (the QBCOND line).
 *
 * Every fix site of the resolver evaluates it on the matrix MLAMBDA searches, before the
 * fix and on the matrix rebuilt from the fixed covariance.  The comparison shows whether
 * the Joseph-form pseudo-observation leaves the matrix well-conditioned for the next epoch
 * or collapses it as a back-substitution would.
 */
struct QbCond {
    double cond   = 0.0;   ///< lambda_max / lambda_min [-]; infinite when lambda_min <= 0.
    double lo     = 0.0;   ///< Smallest eigenvalue [cycles^2].
    double hi     = 0.0;   ///< Largest eigenvalue [cycles^2].
    double sig_lo = 0.0;   ///< sqrt(lo): sigma of the tightest direction [cycles]; 0 when lo <= 0.
    double sig_hi = 0.0;   ///< sqrt(hi): sigma of the loosest direction [cycles]; 0 when hi <= 0.
};

/**
 * @brief Eigen-decompose a cycle-domain ambiguity covariance (read through its symmetric part).
 *
 * Read-only: the eigenvalues are computed on a copy.
 *
 * @param Qb_cycles The covariance [cycles^2], n x n.
 * @return The spread; all zeros on an empty or non-finite matrix or a failed decomposition.
 */
QbCond qbCondition(const Eigen::MatrixXd& Qb_cycles);

/**
 * @brief Rebuild the cycle-domain DD ambiguity covariance from a full state covariance.
 *
 * The differencing and wavelength scaling of the per-band search: entry (i, j) is
 * @f$(P_{ii'} - P_{ir'} - P_{ri'} + P_{rr'}) / (\lambda_i \lambda_j)@f$ with i, r the two
 * state indices of pair i and i', r' those of pair j.  The result is symmetrized.
 *
 * @tparam PairVec      A sequence of items with @c state_idx_i and @c state_idx_ref.
 * @param P             Full state covariance [m^2].
 * @param pairs         The DD pairs.
 * @param pair_lambda   Wavelength of each pair [m].
 * @return The covariance [cycles^2], pairs.size() square.
 */
template <typename PairVec>
Eigen::MatrixXd rebuildQbCycles(const Eigen::MatrixXd& P,
                                const PairVec& pairs,
                                const std::vector<double>& pair_lambda) {
    const int nb = static_cast<int>(pairs.size());
    Eigen::MatrixXd Q(nb, nb);
    for (int i = 0; i < nb; ++i) {
        for (int j = 0; j < nb; ++j) {
            const int ii = pairs[i].state_idx_i,  ir = pairs[i].state_idx_ref;
            const int ji = pairs[j].state_idx_i,  jr = pairs[j].state_idx_ref;
            Q(i, j) = (P(ii, ji) - P(ii, jr) - P(ir, ji) + P(ir, jr))
                      / (pair_lambda[i] * pair_lambda[j]);
        }
    }
    return 0.5 * (Q + Q.transpose());
}

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
     *
     * @param state_manager   Current filter state (provides ambiguity indices).
     * @param observations    Modeled observations for this epoch (for elevation, satellite info).
     * @return AmbiguityResolution; @c success=false (the caller keeps the float
     *         state) when AR is disabled, a convergence guard is still active,
     *         there are no candidates, or an acceptance gate fails.
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

    /**
     * @brief Refloat every ambiguity of the listed satellites.
     *
     * Calls @c StateManager::resetAmbiguity on each ambiguity key whose satellite is
     * in @p satellites (all signals), so the filter re-seeds them as new arcs.  The
     * PPP callers apply it to @ref AmbiguityResolution::release_satellites after
     * the epoch's fix is installed, so the refloat is not overwritten by
     * @c fixed_covariance.  A satellite without ambiguity states (a pivot) is
     * skipped.
     *
     * @param state_manager  Filter state (modified in place).
     * @param satellites     Satellites to refloat.
     * @param std_m          Ambiguity sigma after the refloat [m].
     * @return Number of ambiguity keys reset.
     */
    int refloatSatellites(StateManager& state_manager,
                          const std::vector<std::string>& satellites,
                          double std_m) const;

    /// Satellites refloated so far by @ref AmbiguityResolverConfig::hold_release_cyc.
    int holdReleaseCount() const { return n_hold_releases_; }
    /// Satellites refloated so far by @ref AmbiguityResolverConfig::refloat_partner_signal.
    int partnerReleaseCount() const { return n_partner_releases_; }

    /// Run counters of the wide-lane decision (all zero with the key off).
    const WlBranchCounters& wlBranchCounters() const { return wl_branch_counters_; }

    /**
     * @brief The run-end line of the wide-lane decision.
     * @return One line with the pairs evaluated, decided by the phase, persisted
     *         contradictions, pair-epochs waiting for persistence, pairs refused for reach
     *         and satellites returned for a reset.
     */
    std::string wlBranchRunEndLine() const;

    /// Run counters of the extra signals (all zero with the key off).
    const ExtraSignalCounters& extraSignalCounters() const { return extra_counters_; }

    /**
     * @brief The run-end line of the extra signals.
     * @return One line with the extra-wide lanes evaluated, admitted, fixed and the held
     *         pairs released because of one.
     */
    std::string extraSignalsRunEndLine() const;

    /// Run counters of the held census and of the endings (measure only).
    const FixQualityCounters& fixQualityCounters() const { return fix_quality_; }

    /**
     * @brief The run-end line of the resolver's endings and of the held census.
     * @return One line with the resolver calls by ending (non-zero ones, in the order of
     *         @ref ArEnding), the census epochs with a held pair and their share, the held
     *         pair-epochs, and the held set's joins and leaves by cause.
     */
    std::string fixQualityRunEndLine() const;

    /**
     * @brief True when the extra signals are in force.
     * @return @c extra_signals together with @c wl_from_float, which the extra-wide lanes need.
     */
    bool extraSignalsActive() const { return config_.extra_signals && config_.wl_from_float; }

    /**
     * @brief True when the wide-lane decision is in force.
     * @return @c wl_branch_check together with @c wl_from_float and @c nl_iono_free_row,
     *         which the decision needs.
     */
    bool wlBranchActive() const {
        return config_.wl_branch_check && config_.wl_from_float && config_.nl_iono_free_row;
    }

    /**
     * @brief Get the current AR configuration.
     * @return Reference to the stored configuration; valid for the lifetime of this resolver.
     */
    const AmbiguityResolverConfig& getConfig() const { return config_; }

    /**
     * @brief Update the AR configuration.
     * @param config New AR configuration; copied, and used from the next resolve call on.
     */
    void setConfig(const AmbiguityResolverConfig& config) { config_ = config; }

private:
    // Test seam: the MLAMBDA unit tests (tests/test_estimation_core.cpp)
    // exercise the private static numerical kernels below in isolation with
    // synthetic matrices.  Befriending the access shim grants that access
    // without widening the public API; it has no effect on production
    // translation units (the type is only ever defined in the test binary).
    friend struct MlambdaTestAccess;
    /// Test seam for nlRowCoefficients (@c GnssProcessor::AmbiguityResolver::nlRowCoefficients) (tests/test_nl_search_row.cpp).
    friend struct NlRowTestAccess;
    /// Test seam for wlArcSuccessRate_ (@c GnssProcessor::AmbiguityResolver::wlArcSuccessRate_) (tests/test_wl_only_ffr.cpp).  A field
    /// run can show only that nothing went wrong, not that the WL-only acceptance
    /// gate fails safe, so a unit test pins that behaviour.
    friend struct WlFfrTestAccess;
    /// Test seam for @ref floatWidelane_ and @ref addSharedReferenceFloor_
    /// (tests/test_ar_float_widelane.cpp).
    friend struct ArFloatWlTestAccess;
    /// Test seam for the held-pair release (tests/test_ar_hold_release.cpp).
    friend struct ArHoldReleaseTestAccess;
    /// Test seam for the wide-lane decision through @ref resolveWlConstrained
    /// (tests/test_ar_wl_branch.cpp).
    friend struct ArWlBranchTestAccess;
    /// Test seam for the extra signals through @ref resolveWlConstrained
    /// (tests/test_ar_extra_signals.cpp).
    friend struct ArExtraSignalsTestAccess;
    /// Test seam for the stages of @ref resolveWlConstrained (tests/support/ArWlcStageAccess.hpp).
    friend struct ArWlcStageTestAccess;
    /// Test seam for the held census and the endings (tests/test_ar_fix_quality.cpp).
    friend struct ArFixQualityTestAccess;

    // -----------------------------------------------------------------------
    // WL-constrained N_1 search functional
    // -----------------------------------------------------------------------

    /**
     * @brief Coefficients that define which row of the (L1,L2) ambiguity plane
     *        observes the narrow-lane integer N_1, once the widelane is known.
     *
     * All four are exact functions of the frequency pair.  t_1 (@c GnssProcessor::AmbiguityResolver::NlRowCoefficients::t_1) / t_2 (@c GnssProcessor::AmbiguityResolver::NlRowCoefficients::t_2)
     * carry DD ambiguities from metres into search cycles; @c shift and
     * bt_2 (@c GnssProcessor::AmbiguityResolver::NlRowCoefficients::bt_2) undo the known-widelane displacement in each direction.
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
    // Float widelane and the N_1 search covariance floor
    // -----------------------------------------------------------------------

    /// The float states' DD widelane of one satellite pair and its formal sigma.
    struct FloatWidelane {
        double w;      ///< DD_1/λ_1 − DD_2/λ_2 [WL cycles].
        double sigma;  ///< Formal sigma of @ref w from the filter covariance [WL cycles].
    };

    /**
     * @brief Float DD widelane of one satellite pair
     *        (@ref AmbiguityResolverConfig::wl_from_float).
     *
     * @param p1    DD pair on the higher frequency.
     * @param p2    DD pair on the lower frequency (same satellites as @p p1).
     * @param lam1  Wavelength of @p p1's signal [m].
     * @param lam2  Wavelength of @p p2's signal [m].
     * @param x     Filter state (DD ambiguities in metres).
     * @param P     Filter covariance.
     * @return The widelane and its sigma; NaN in both when an index is out of
     *         range or the variance is negative.
     */
    static FloatWidelane floatWidelane_(const DdPair& p1, const DdPair& p2,
                                        double lam1, double lam2,
                                        const Eigen::VectorXd& x,
                                        const Eigen::MatrixXd& P);

    /// What the wide-lane decision collects over one epoch's satellite pairs.
    struct WlBranchEpoch {
        /// (sat_i, sat_ref) of every pair the code admitted this epoch.
        std::vector<std::pair<std::string, std::string>> code_admitted;
        /// (sat_i, sat_ref) of every pair with a persisted contradiction of the code by the phase.
        std::vector<std::pair<std::string, std::string>> contradicted;
    };

    /**
     * @brief Decide the wide-lane integer of one satellite pair by the code and the
     *        ionosphere-free phase (@ref AmbiguityResolverConfig::wl_branch_check).
     *
     * Builds the fractions and the sigma of the iono-free N_1 float for the two
     * candidate wide-lane integers (the row is always the iono-free one, whichever row
     * the search uses), calls @ref decideWlBranch, applies the persistence of the pair
     * (@ref stepWlBranchPersistence, state in @c wl_branch_runs_), counts the outcome, logs
     * one @c WLBRANCH line and records code-admitted pairs and persisted contradictions
     * in @p epoch.
     *
     * @param p_hi      DD pair on the higher frequency.
     * @param p_lo      DD pair on the lower frequency (same satellites).
     * @param lam1      Wavelength on the higher frequency [m].
     * @param lam2      Wavelength on the lower frequency [m].
     * @param f1        Higher frequency [Hz].
     * @param f2        Lower frequency [Hz].
     * @param fw        Float wide lane of the pair (@ref floatWidelane_).
     * @param x         Filter state.
     * @param P         Filter covariance.
     * @param tow       Epoch time of week, for the log line and the persistence [s].
     * @param[in,out] epoch  The epoch's code-admitted pairs and persisted contradictions.
     * @param[out] dd_wl     The chosen DD wide-lane integer (set when admitted).
     * @return True when the pair is admitted with @p dd_wl.
     */
    bool decideWlBranchOfPair_(const DdPair& p_hi, const DdPair& p_lo,
                               double lam1, double lam2, double f1, double f2,
                               const FloatWidelane& fw,
                               const Eigen::VectorXd& x, const Eigen::MatrixXd& P,
                               double tow, WlBranchEpoch& epoch, int& dd_wl);

    /// One extra signal of a satellite pair on its way to a fix
    /// (@ref AmbiguityResolverConfig::extra_signals).
    struct ExtraCandidate {
        int         pair_idx{ -1 };   ///< Index of the extra signal's DD pair in the epoch's pair list.
        std::string signal;           ///< The extra signal.
        double      lam{ 0.0 };       ///< Its wavelength [m].
        double      e{ 0.0 };         ///< Float extra-wide lane n_lo - n_x [cycles] (set by the evaluation).
        int         ewl{ 0 };         ///< round(e) (set by the evaluation).
    };

    /**
     * @brief Evaluate the extra-wide lanes of one satellite pair and keep the admitted.
     *
     * For each extra signal x the float extra-wide lane @c e = n_lo - n_x and its sigma
     * come from @ref floatWidelane_ (lower primary signal first), the admission from
     * @ref admitExtraWideLane.  Counts every evaluation and admission and logs one
     * @c EWLFLOAT line per evaluated signal when @p log_lines is true.
     *
     * @param pairs        The epoch's DD pairs.
     * @param lo_pair_idx  Index of the lower-frequency primary signal's DD pair.
     * @param lam_lo       Its wavelength [m].
     * @param extras       The pair's extra signals (pair index, signal and wavelength set).
     * @param x            Filter state.
     * @param P            Filter covariance.
     * @param tow          Epoch time of week, for the log line [s].
     * @param log_lines    Write the @c EWLFLOAT lines.
     * @return The admitted extra signals with @c e and @c ewl filled in.
     */
    std::vector<ExtraCandidate> evaluateExtraWideLanes_(
        const std::vector<DdPair>& pairs, int lo_pair_idx, double lam_lo,
        const std::vector<ExtraCandidate>& extras, const Eigen::VectorXd& x,
        const Eigen::MatrixXd& P, double tow, bool log_lines);

    /**
     * @brief Drop the persistence runs of the wide-lane decision that can no longer continue.
     *
     * Erases the entries of @c wl_branch_runs_ whose last decisive epoch is older than
     * @c 10 * max(wl_branch_persist_s, 30 s) or lies after @p tow (a week rollover), so the
     * map holds only the pairs in play.
     *
     * @param tow Epoch time of week [s].
     */
    void pruneWlBranchRuns_(double tow);

    /**
     * @brief Add the error floor of @ref AmbiguityResolverConfig::nl_sigma_floor_cyc
     *        to a search covariance.
     *
     * Adds @f$\tfrac{f^2}{2}(I + M)@f$ with @f$M_{kl} = 1@f$ when entries k and l
     * share a reference satellite (including k = l): each entry's variance grows
     * by @f$f^2@f$, and entries on a common reference correlate by @f$f^2/2@f$.
     *
     * @param[in,out] Q        Search covariance [cycles²], n × n.
     * @param ref_of           Reference satellite of each entry (size n).
     * @param floor_cyc        The floor f [cycles], > 0.
     */
    static void addSharedReferenceFloor_(Eigen::MatrixXd& Q,
                                         const std::vector<std::string>& ref_of,
                                         double floor_cyc);

    /**
     * @brief The body of @ref resolve: gates, pair election and the searches.
     *
     * @ref resolve wraps it to commit an accepted fix to the held-pair memory and
     * to append the partner-refloat satellites, which every return path must carry.
     * Parameters as for @ref resolve.
     */
    AmbiguityResolution resolveCore_(const StateManager& state_manager,
                                     const std::vector<PppObservation>& observations,
                                     const std::map<std::string, int>& wl_ambiguities,
                                     const std::map<std::string, double>& wl_fractions,
                                     const std::map<std::string, WlArcQuality>& wl_quality);

    // -----------------------------------------------------------------------
    // Held-pair release (AmbiguityResolverConfig::hold_release_cyc)
    // -----------------------------------------------------------------------

    /// How far the float state of one satellite pair sits from its held integers.
    struct HoldMisfit {
        double d_wl;   ///< Float widelane minus the held widelane [WL cycles].
        double d_if;   ///< Float minus held ionosphere-free combination [NL cycles].
    };

    /**
     * @brief Misfit of a float DD pair against a held fix.
     *
     * @param h    The held fix (integers, frequencies, wavelengths).
     * @param dd1  Float DD on the higher frequency [m].
     * @param dd2  Float DD on the lower frequency [m].
     * @return Widelane and ionosphere-free differences; the held DDs are
     *         N_1 lambda_1 and (N_1 - WL) lambda_2.
     */
    static HoldMisfit holdMisfit_(const AmbiguityResolution::HeldPairFix& h,
                                  double dd1, double dd2);

    /**
     * @brief Choose the satellites to refloat from the contradicted held pairs.
     *
     * A reference is chosen when at least two of its held pairs and more than half
     * of them are contradicted; otherwise each contradicted pair's non-reference
     * satellite.
     *
     * @param held          (sat_i, sat_ref) of every held pair checked this epoch.
     * @param contradicted  (sat_i, sat_ref) of the contradicted ones.
     * @return The satellites to refloat, sorted, without duplicates.
     */
    static std::vector<std::string> chooseReleaseSatellites_(
        const std::vector<std::pair<std::string, std::string>>& held,
        const std::vector<std::pair<std::string, std::string>>& contradicted);

    /// Held pairs by "sat_i|sat_ref|sig_hi|sig_lo" (hold_release_cyc > 0 only).
    std::map<std::string, AmbiguityResolution::HeldPairFix> held_pairs_;
    /// Satellites queued by refloat_partner_signal in this epoch's refreshArcAges_.
    std::vector<std::string> partner_release_;
    /// Per ambiguity key: the continuity generation seen last epoch and the epochs
    /// since it last changed (kept only while a release rule is on).
    std::map<std::string, std::pair<uint64_t, int>> key_gen_;
    int n_hold_releases_{ 0 };      ///< Satellites refloated by hold_release_cyc.
    int n_partner_releases_{ 0 };   ///< Satellites refloated by refloat_partner_signal.
    WlBranchCounters wl_branch_counters_;   ///< Run counters of the wide-lane decision.
    ExtraSignalCounters extra_counters_;    ///< Run counters of the extra signals.
    /// Per pair "sat_i|sat_ref|sig_hi|sig_lo": the run of the phase's decisive integer (wl_branch_check only).
    std::map<std::string, WlBranchRun> wl_branch_runs_;

    // -----------------------------------------------------------------------
    // Held census and endings (measure only)
    // -----------------------------------------------------------------------

    /**
     * @brief Count the epoch's held pairs and the held set's edges (measure only).
     *
     * Groups the DD pairs by satellite pair; a pair is held when at least two of its
     * signals' DD variances are at hold scale (@ref isHeldDdVariance).  Compares the held
     * satellites with those of the previous census epoch and counts each join and leave
     * (a leave while the satellite is still in the pairs, or by its absence); each edge is
     * a debug line `HELDSET`.  Writes nothing the solution reads.
     *
     * @param pairs         The epoch's DD pairs.
     * @param P             The state covariance (read only when @p pairs is not empty).
     * @param observations  The epoch's observations (epoch time of the debug lines).
     * @param[out] result   n_sat_pairs, n_held_pairs, held_joined and held_left are written.
     */
    void censusHeldPairs_(const std::vector<DdPair>& pairs, const Eigen::MatrixXd& P,
                          const std::vector<PppObservation>& observations,
                          AmbiguityResolution& result);

    FixQualityCounters fix_quality_;           ///< Run counters of the census and the endings.
    std::set<std::string> held_sats_prev_;     ///< Held satellites at the previous census epoch.

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

    /// Per-ambiguity-key arc maturity, maintained by refreshArcAges_ (@c GnssProcessor::AmbiguityResolver::refreshArcAges_).
    std::map<std::string, ArcState> arcs_;

    /**
     * @brief Bump/expire @c arcs_ against the state vector.  Call once per
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

    // -----------------------------------------------------------------------
    // WL-constrained resolution: what crosses its stages.  The stages, their
    // order and the rules each owns are listed in kWlcStages
    // (filter/AmbiguityResolverStages.hpp).
    // -----------------------------------------------------------------------

    /// One signal of a satellite pair, as stage S1 collects it.
    struct WlcSignal {
        int         pair_idx;     ///< Index of the signal's DD pair in the epoch's pair list.
        std::string signal;       ///< Signal name (e.g. "L1C").
        double      freq;         ///< Carrier frequency [Hz].
        double      wavelength;   ///< Wavelength [m].
    };

    /// One satellite pair of the epoch: its signals and their primary/extra split (stage S1).
    struct WlcGroup {
        std::string            key;       ///< "sat_i|sat_ref".
        std::vector<WlcSignal> signals;   ///< The pair's signals, in the order of the epoch's pair list.
        PrimaryExtraSplit      split;     ///< Its two primary signals and its extras (@ref splitPrimaryAndExtra).
    };

    /// The outcome of the held-pair test for one remembered hold (stage S2).
    struct HoldCheckRecord {
        std::string key;                              ///< The hold's key "sat_i|sat_ref|sig_hi|sig_lo".
        HoldCheck   outcome{ HoldCheck::KeyMiss };    ///< What the test did with it.
        double      d_wl{ 0.0 };   ///< Float minus held wide lane [WL cycles]; tested holds only.
        double      d_if{ 0.0 };   ///< Float minus held iono-free combination [NL cycles]; tested holds only.
    };

    /// The DD wide-lane integer of one satellite pair (stage S3).
    struct WlcWideLane {
        int dd_wl{ 0 };        ///< DD wide-lane integer N_1 - N_2 [cycles].
        int wl_int_i{ 0 };     ///< Per-satellite MW integer of sat_i (MW source only, else 0).
        int wl_int_ref{ 0 };   ///< Per-satellite MW integer of sat_ref (MW source only, else 0).
    };

    /// One narrow-lane candidate: a satellite pair on two primary signals with a known DD
    /// wide lane (stage S4), and the coefficients of its search row.
    struct NlCandidate {
        int dd_idx_1;         ///< Index of the higher-frequency DD in the epoch's pairs.
        int dd_idx_2;         ///< Index of the lower-frequency DD.
        double f1, f2;        ///< Frequencies [Hz].
        double lam1, lam2;    ///< Wavelengths [m].
        int dd_wl;            ///< DD wide-lane integer (known).
        double lambda_nl;     ///< Narrow-lane wavelength [m].
        // --- Search-functional coefficients (see the T-matrix block of the search problem) ---
        // Filled per candidate so every downstream expression keeps one form
        // whichever row (narrowlane or iono-free) is in force.
        double t_coef_1;      ///< T row entry on dd_idx_1 [1/m]: DD metres → search cycles.
        double t_coef_2;      ///< T row entry on dd_idx_2 [1/m].
        double shift_coef;    ///< y_for_mlambda = y_reduced + shift_coef·WL (≡ N_1),
                              ///< and, identically, N_1 = n_reduced_fixed + shift_coef·WL.
        double bt_coef_2;     ///< N_2 = n_reduced_fixed + bt_coef_2·WL (= shift_coef − 1).
        /// The pair's admitted extra signals (extra_signals); empty with the key off.
        std::vector<ExtraCandidate> extras;
    };

    /// What crosses the stages of @ref resolveWlConstrained in one epoch.
    struct WlcEpoch {
        std::shared_ptr<spdlog::logger> log;   ///< The ambiguity_resolver logger, fetched once by S1.
        double tow{ 0.0 };           ///< Epoch time of week [s]; 0 without observations.
        bool   log_lines{ false };   ///< Observations are present: the per-pair info lines are written.
        std::vector<WlcGroup> groups;               ///< S1: the satellite pairs, in key order.
        std::set<std::string> released;             ///< S2: satellites that sit out this epoch's search.
        std::vector<HoldCheckRecord> hold_checks;   ///< S2: one record per remembered hold.
        bool branch_check{ false };                 ///< The wide-lane branch check is active this epoch.
        WlBranchEpoch branch;                       ///< S3: code-admitted pairs and persisted contradictions.
        std::vector<NlCandidate> candidates;        ///< S4: the narrow-lane candidates, in key order.
        int n_skipped_immature{ 0 };                ///< S4: candidates dropped by the min_arc_info gate.
    };

    /// The logger of the epoch's stages: WlcEpoch::log, or a fresh fetch when S1 did not run.
    std::shared_ptr<spdlog::logger> wlcLog_(const WlcEpoch& ep) const;

    /// Which outcome of the search stage S9 reports.
    enum class WlcFixPath {
        Full,      ///< The full candidate set passed the acceptance battery and was written back.
        Partial,   ///< A subset found by partial fixing was written back.
        Reject,    ///< Partial fixing found no subset: the epoch stays float.
    };

    /**
     * @brief Stage S1: gather the epoch's DD pairs by satellite pair (rules W1, W2).
     *
     * A pair is kept when its satellite is observed and its signal has a finite positive
     * frequency and wavelength; the kept pairs are grouped by "sat_i|sat_ref" in key order,
     * each group with its signals in pair order and their split into two primary signals and
     * extras (@ref splitPrimaryAndExtra; with the extra signals off, exactly two signals).
     * Also sets the epoch time and the log flag.
     *
     * @param pairs         The epoch's DD pairs.
     * @param observations  The epoch's observations (system, satellite, time).
     * @param[out] ep       Its groups, tow and log_lines are written.
     */
    void groupSatellitePairs_(const std::vector<DdPair>& pairs,
                              const std::vector<PppObservation>& observations,
                              WlcEpoch& ep) const;

    /**
     * @brief Stage S9: write the summary line and the QBCOND line of a search outcome.
     *
     * @param ep        The epoch (its logger).
     * @param path      Full fix, partial fix or refused partial search.
     * @param n_nl      Number of narrow-lane candidates searched.
     * @param n_fixed   Number fixed (n_nl for a full fix, the kept count for a partial one).
     * @param ratio     The ratio reported (the full set's, or the accepted subset's).
     * @param p_s       Bootstrap success rate of the fix (unused for Reject).
     * @param max_frac  Worst |float - integer| of the fix [cycles] (unused for Reject).
     * @param qb_pre    Conditioning of the search covariance before the fix.
     * @param qb_post   Conditioning rebuilt from the fixed covariance (unused for Reject,
     *                  whose line carries literal zeros).
     */
    void reportFix_(const WlcEpoch& ep, WlcFixPath path, int n_nl, int n_fixed, double ratio, double p_s,
                    double max_frac, const QbCond& qb_pre, const QbCond& qb_post);

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
     * in resolveWlOnly (@c GnssProcessor::AmbiguityResolver::resolveWlOnly).
     *
     * @param state_manager   Filter state holding NL/WL ambiguity entries.
     * @param x               Float state vector (working copy).
     * @param P               Float covariance matrix (working copy).
     * @param wl_ambiguities  MW-resolved SD WL integers keyed by
     *                        @c "<sat>_<sig_a>_<sig_b>" stats keys.
     * @param wl_fractions    Worst-case fractional WL residual (WL cycles) of each
     *                        locked arc, same keys; an arc without an entry reads as 0.
     *                        Compared with @c wl_max_frac by the single-arc gate.
     * @param observations    Per-satellite observations (for elevation
     *                        and constellation lookup).
     * @param wl_quality      Per-arc WL lock quality (sigma of the arc mean and sample
     *                        count) from the MW detector, same keys; bounds the joint
     *                        rounding success rate of the constraint set.
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
