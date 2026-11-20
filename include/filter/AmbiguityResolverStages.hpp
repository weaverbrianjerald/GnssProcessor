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

} // namespace GnssProcessor
