/**
 * @file AmbiguityResolverStages.hpp
 * @brief The stage table of the WL-constrained user resolver and the outcome of the
 *        held-pair test: plain value types read by a person and by the unit tests.
 *
 * @c AmbiguityResolver::resolveWlConstrained fixes the narrow-lane integers of the
 * user's satellite pairs once their wide lanes are known.  It runs as nine stages in a
 * fixed order; @ref kWlcStages lists them with what each reads, what it writes and the
 * decision rules it owns.  The rule names (W1 to W18, with W3a) are those of the rule
 * table of the user fixing audit (docs/refound/UFA_user_fixing_audit_2026-11-18.md,
 * section 2), so a rule in the audit maps to one stage here and to one function in
 * src/core/filter/AmbiguityResolver.cpp.
 *
 * The table is declaration only: no code walks it, and changing a row changes no
 * output.  The order of the rows is the order of the calls.
 */
#pragma once

#include <cstddef>

namespace GnssProcessor {

/// One stage of the WL-constrained resolver (@ref kWlcStages).
struct WlcStageRow {
    const char* id;        ///< Stage id, S1 to S9, in call order.
    const char* function;  ///< The member function of AmbiguityResolver that runs the stage.
    const char* reads;     ///< What the stage reads.
    const char* writes;    ///< What the stage writes (fields of AmbiguityResolver::WlcEpoch, the result, members).
    const char* rules;     ///< The decision rules it owns (UFA section 2 names), or "-".
};

/**
 * @brief The stages of AmbiguityResolver::resolveWlConstrained, in call order.
 *
 * S3 and S4 run once per satellite pair inside one loop, in the order of the pair
 * keys, so their log lines interleave pair by pair.  An early return leaves after S6
 * (no candidate) or inside S7 (search failed, or every gate refused and partial
 * fixing is off or found no subset).
 */
inline constexpr WlcStageRow kWlcStages[] = {
    { "S1", "groupSatellitePairs_",
      "the epoch's DD pairs, observations",
      "WlcEpoch::groups (signals of each satellite pair and their primary/extra split), tow",
      "W1 pair usable, W2 primary split" },
    { "S2", "testHeldPairs_",
      "groups, float state x; held_pairs_",
      "WlcEpoch::released, WlcEpoch::hold_checks, result.release_satellites; erases released holds",
      "W3 hold test, W3a silent hold skips" },
    { "S3", "decideWideLane_",
      "one group, x, P, released, branch epoch time; wide-lane map",
      "the pair's DD wide-lane integer or a skip; WlcEpoch::branch",
      "W4 released, W5a branch check, W5b float wide lane, W5c MW wide lane" },
    { "S4", "gateCandidate_",
      "one group and its wide lane, x, P",
      "one entry of WlcEpoch::candidates or a skip; WlcEpoch::n_skipped_immature",
      "W6 maturity, W7 IF-row amplification, W8 extra lanes" },
    { "S5", "settleContradictions_",
      "WlcEpoch::branch",
      "result.bias_reset_satellites",
      "W9 contradiction response" },
    { "S6", "buildSearchProblem_",
      "candidates, pairs, x, P",
      "the search problem: T, y, nl_shift, floored Q, conditioning before the fix",
      "W10 no candidates, W11 search problem" },
    { "S7", "searchAndAccept_",
      "the search problem",
      "integers, distances, ratio, P_s, worst pair; verdict full, partial (kept subset) or reject",
      "W12 search, W13 ratio low, W14 ratio high, W15 success rate, W16 per-pair cap, W17 partial" },
    { "S8", "writeBack_",
      "candidates, kept indices, integers, x, P",
      "result.fixed_state, fixed_covariance, fixed_dd_rows, held_pairs",
      "W18 write-back" },
    { "S9", "reportFix_",
      "conditioning before and after the fix, ratio, P_s, worst fraction",
      "the summary and QBCOND lines",
      "-" },
};

/// Number of rows of @ref kWlcStages.
inline constexpr std::size_t kWlcStageCount = sizeof(kWlcStages) / sizeof(kWlcStages[0]);

/**
 * @brief Outcome of the held-pair test (stage S2) for one remembered hold.
 *
 * Kept in memory for the epoch only; nothing is written from it.  The cases that are
 * not @c Ok or @c Contradicted are the silent skips of rule W3a: the hold is not tested
 * this epoch and stays in the memory.
 */
enum class HoldCheck {
    Ok,             ///< Tested: the float agrees with the held integers within hold_release_cyc.
    Contradicted,   ///< Tested: the wide lane, the iono-free combination or a held extra-wide lane disagrees.
    UnusableGroup,  ///< The satellite pair is present but has no usable primary split (rule W2).
    KeyMiss,        ///< No satellite pair of this epoch has the hold's key: the reference, a satellite or a signal changed.
    BadIndex,       ///< The pair's state indices lie outside the state vector.
};

/// Name of a @ref HoldCheck value, for tests and trace lines.
constexpr const char* holdCheckName(HoldCheck c) {
    switch (c) {
        case HoldCheck::Ok:            return "ok";
        case HoldCheck::Contradicted:  return "contradicted";
        case HoldCheck::UnusableGroup: return "unusable_group";
        case HoldCheck::KeyMiss:       return "key_miss";
        case HoldCheck::BadIndex:      return "bad_index";
    }
    return "?";
}

/**
 * @brief How one call of the user resolver ended: an early gate, or the verdict of the
 *        WL-constrained search.
 *
 * Recorded in @c AmbiguityResolution::ending by @c AmbiguityResolver::resolve on the
 * paths that reach the WL-constrained search (the user's path); the per-band, WL-only,
 * NlWl and baseline DD paths leave @c None.  A failed search reports its own cause, never
 * a generic failure: the trace column @c ar_gate and the run-end tally read it.  The
 * search verdicts are tested in the order of the acceptance battery (ratio low, ratio
 * high, success rate, per-pair cap), so a search refused for two reasons reports the
 * first.
 */
enum class ArEnding {
    None,           ///< No ending recorded (a path that does not set one).
    Disabled,       ///< ar_mode 0.
    MinEpochs,      ///< The min_epochs convergence guard.
    SigmaGate,      ///< The formal 3D position sigma is above max_position_sigma_m.
    NoPairs,        ///< No DD ambiguity pair this epoch.
    NoNlCandidate,  ///< No narrow-lane candidate after the wide-lane and maturity rules (W10).
    SearchFailed,   ///< MLAMBDA failed or returned invalid distances (W12).
    RatioLow,       ///< The ratio is below ratio_threshold (W13).
    RatioHigh,      ///< The ratio is above ratio_upper_bound (W14).
    SuccessRate,    ///< The bootstrap success rate is below min_success_rate (W15).
    PairFrac,       ///< One pair lies farther than nl_max_per_pair_frac from its integer (W16).
    AllHeld,        ///< search_unheld_only: every candidate is already held, nothing is searched.
    Degenerate,     ///< search_unheld_only: the conditioned covariance has a direction at hold scale.
    Fixed,          ///< The full candidate set was accepted.
    FixedPartial,   ///< A subset was accepted by partial fixing (W17).
};

/// Number of values of @ref ArEnding.
inline constexpr std::size_t kArEndingCount = 15;

/**
 * @brief Name of an @ref ArEnding value: the word of the run-end tally and, for a failed
 *        search, of the trace column @c ar_gate.
 *
 * The early gates keep the words the trace used before the endings existed
 * (`no_candidates` is the gate of an epoch without DD pairs); a search that found no
 * narrow-lane candidate is `no_nl_candidate`.
 */
constexpr const char* arEndingName(ArEnding e) {
    switch (e) {
        case ArEnding::None:          return "none";
        case ArEnding::Disabled:      return "disabled";
        case ArEnding::MinEpochs:     return "min_epochs";
        case ArEnding::SigmaGate:     return "sigma_gate";
        case ArEnding::NoPairs:       return "no_candidates";
        case ArEnding::NoNlCandidate: return "no_nl_candidate";
        case ArEnding::SearchFailed:  return "search_failed";
        case ArEnding::RatioLow:      return "ratio_low";
        case ArEnding::RatioHigh:     return "ratio_high";
        case ArEnding::SuccessRate:   return "success_rate";
        case ArEnding::PairFrac:      return "pair_frac";
        case ArEnding::AllHeld:       return "all_held";
        case ArEnding::Degenerate:    return "degenerate";
        case ArEnding::Fixed:         return "fixed";
        case ArEnding::FixedPartial:  return "fixed_partial";
    }
    return "?";
}

/// True for the endings a search reaches after the early gates (the search ran, or found
/// nothing to search); false for @c None and the early gates.
constexpr bool arEndingIsSearchVerdict(ArEnding e) {
    return e != ArEnding::None && e != ArEnding::Disabled && e != ArEnding::MinEpochs
        && e != ArEnding::SigmaGate && e != ArEnding::NoPairs;
}

/**
 * @brief Held-scale factor: a DD ambiguity is held when its variance is at most this many
 *        times the hold variance.
 *
 * An accepted fix writes each fixed DD as a pseudo-observation of variance hold_variance,
 * so the DD's variance afterwards is at most that variance and grows only by the
 * ambiguity process noise; a float DD never comes near it.  On a 24 h observed user
 * (hold variance 1e-7 m2) the DDs of pairs held by a fix sit at or below 1.5e-7 m2 at the
 * 99th percentile and the float DDs at or above 2.4e-4 m2 at the 0.1st percentile: every
 * factor from 4 to 1000 separates them the same way.  Ten leaves a decade for the growth
 * of a hold that is not re-applied.
 */
inline constexpr double kHeldVarianceFactor = 10.0;

/**
 * @brief Is a DD ambiguity of variance @p var_m2 held by a fix?
 *
 * @param var_m2            The DD's variance [m^2].
 * @param hold_variance_m2  The variance of the fix's pseudo-observation [m^2]
 *                          (AmbiguityResolverConfig::hold_variance).
 * @return True when @p var_m2 <= @ref kHeldVarianceFactor x @p hold_variance_m2
 *         (false when the hold variance is not positive).
 */
constexpr bool isHeldDdVariance(double var_m2, double hold_variance_m2) {
    return hold_variance_m2 > 0.0 && var_m2 <= kHeldVarianceFactor * hold_variance_m2;
}

/**
 * @brief Run counters of the held census and of the resolver's endings (measure only).
 *
 * The census runs at every resolver call that reaches the epoch's DD pairs.  A satellite
 * pair is held when at least two of its signals' DD ambiguities are held
 * (@ref isHeldDdVariance); a satellite is held when one of its pairs is.  The held
 * satellites of the previous census epoch give the edges: a satellite joins the held set,
 * or leaves it while still in the epoch's pairs (refloated or released) or by its
 * absence (set, or no DD pair).  Satellite keys survive a change of reference satellite,
 * pair keys do not.
 */
struct FixQualityCounters {
    long long epochs{ 0 };            ///< Census epochs.
    long long epochs_any_held{ 0 };   ///< Census epochs with at least one held pair.
    long long pair_epochs{ 0 };       ///< Satellite pair-epochs seen by the census.
    long long held_pair_epochs{ 0 };  ///< Of them, held.
    long long joins{ 0 };             ///< Satellites that joined the held set.
    long long leaves_present{ 0 };    ///< Satellites that left it while still in the pairs.
    long long leaves_absent{ 0 };     ///< Satellites that left it by their absence.
    long long endings[kArEndingCount]{};   ///< Resolver calls by @ref ArEnding.
};

} // namespace GnssProcessor
