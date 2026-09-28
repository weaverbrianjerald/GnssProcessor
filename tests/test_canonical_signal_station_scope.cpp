/**
 * @file test_canonical_signal_station_scope.cpp
 * @brief Contract tests for `processing.canonical_signal_priority_stations`
 *        (card CSL section 6, the four-station split).
 *
 * Per CSL section 4, four of the six stations that move to L1W carry the whole
 * consumer gain, and three of them have equal first-epoch margins, so a coverage
 * floor cannot split them further and the next split needs a per-station
 * override.  @c PppModel::canonicalRuleForStation is that override: it decides
 * which rule each station hands to @c PppModel::decideCanonicalPicks.
 *
 * Covered here:
 *   (a) an empty list is the default: the configured rule applies at every
 *       station, under either rule, so an absent key changes nothing;
 *   (b) a non-empty list is a scope that decides on its own: a listed station
 *       runs `priority` and every other station runs `count`, whatever the
 *       global rule says, so the list alone arms the scope and the global rule
 *       cannot widen it;
 *   (c) the comparison is case-insensitive and whole-string (no prefix match);
 *   (d) the empty station label (the off-network path, which has no label) is
 *       never a member of a non-empty scope.
 *
 * The rule is pure (no config, no logger, no observations), so these cases carry
 * no fixture.  Whether a run applies it per station is shown by the run's own
 * "Canonical latch for ..." line, graded on the arms, not here.
 */
#include <gtest/gtest.h>

#include "model/PppModel.hpp"

#include <string>
#include <vector>

using GnssProcessor::CanonicalSignalRule;
using GnssProcessor::PppModel;

namespace {

/// The four stations of the CSL section 6 split (`lE_cov085`'s L1W set).
const std::vector<std::string> kFour{ "KOKV00USA", "STHL00GBR", "ULAB00MNG", "WIND00NAM" };
const std::vector<std::string> kNone{};

CanonicalSignalRule ruleAt(CanonicalSignalRule global,
                           const std::vector<std::string>& scope,
                           const std::string& station) {
    return PppModel::canonicalRuleForStation(global, scope, station);
}

}  // namespace

// --------------------------------------------------------------------------
// (a) no scope: the global rule everywhere
// --------------------------------------------------------------------------

TEST(CanonicalSignalStationScope, EmptyListLeavesTheGlobalRuleEverywhere) {
    EXPECT_EQ(ruleAt(CanonicalSignalRule::Count, kNone, "KOKV00USA"),
              CanonicalSignalRule::Count);
    EXPECT_EQ(ruleAt(CanonicalSignalRule::Count, kNone, "MBAR00UGA"),
              CanonicalSignalRule::Count);
    EXPECT_EQ(ruleAt(CanonicalSignalRule::Priority, kNone, "KOKV00USA"),
              CanonicalSignalRule::Priority);
    EXPECT_EQ(ruleAt(CanonicalSignalRule::Priority, kNone, "MBAR00UGA"),
              CanonicalSignalRule::Priority);
}

TEST(CanonicalSignalStationScope, EmptyListAndEmptyLabelStillTakesTheGlobalRule) {
    // The off-network path carries no station label and keeps the global rule.
    EXPECT_EQ(ruleAt(CanonicalSignalRule::Priority, kNone, ""),
              CanonicalSignalRule::Priority);
    EXPECT_EQ(ruleAt(CanonicalSignalRule::Count, kNone, ""),
              CanonicalSignalRule::Count);
}

// --------------------------------------------------------------------------
// (b) a scope: priority inside, count outside, whatever the global rule is
// --------------------------------------------------------------------------

TEST(CanonicalSignalStationScope, ListedStationsRunPriorityAndTheRestRunCount) {
    for (const auto& s : kFour)
        EXPECT_EQ(ruleAt(CanonicalSignalRule::Priority, kFour, s),
                  CanonicalSignalRule::Priority) << s;
    // The two of the six that the split leaves out, and an unrelated station.
    EXPECT_EQ(ruleAt(CanonicalSignalRule::Priority, kFour, "BOGT00COL"),
              CanonicalSignalRule::Count);
    EXPECT_EQ(ruleAt(CanonicalSignalRule::Priority, kFour, "URUM00CHN"),
              CanonicalSignalRule::Count);
    EXPECT_EQ(ruleAt(CanonicalSignalRule::Priority, kFour, "MBAR00UGA"),
              CanonicalSignalRule::Count);
}

TEST(CanonicalSignalStationScope, TheScopeDecidesOnItsOwnWhateverTheGlobalRule) {
    // A list with the global rule left at `count` still puts the listed stations
    // on `priority`: the list is the arming, which is what makes an arm of this
    // knob a one-line delta against its base.
    EXPECT_EQ(ruleAt(CanonicalSignalRule::Count, kFour, "WIND00NAM"),
              CanonicalSignalRule::Priority);
    EXPECT_EQ(ruleAt(CanonicalSignalRule::Count, kFour, "MBAR00UGA"),
              CanonicalSignalRule::Count);
}

TEST(CanonicalSignalStationScope, ASingleStationScopeIsTheLeaveOneInArm) {
    const std::vector<std::string> one{ "STHL00GBR" };
    EXPECT_EQ(ruleAt(CanonicalSignalRule::Priority, one, "STHL00GBR"),
              CanonicalSignalRule::Priority);
    for (const char* other : { "KOKV00USA", "ULAB00MNG", "WIND00NAM" })
        EXPECT_EQ(ruleAt(CanonicalSignalRule::Priority, one, other),
                  CanonicalSignalRule::Count) << other;
}

// --------------------------------------------------------------------------
// (c) / (d) matching
// --------------------------------------------------------------------------

TEST(CanonicalSignalStationScope, MatchingIsCaseInsensitive) {
    const std::vector<std::string> lower{ "kokv00usa" };
    EXPECT_EQ(ruleAt(CanonicalSignalRule::Priority, lower, "KOKV00USA"),
              CanonicalSignalRule::Priority);
    const std::vector<std::string> upper{ "KOKV00USA" };
    EXPECT_EQ(ruleAt(CanonicalSignalRule::Priority, upper, "kokv00usa"),
              CanonicalSignalRule::Priority);
    EXPECT_EQ(ruleAt(CanonicalSignalRule::Priority, upper, "KoKv00UsA"),
              CanonicalSignalRule::Priority);
}

TEST(CanonicalSignalStationScope, MatchingIsWholeStringNotAPrefix) {
    const std::vector<std::string> four_char{ "KOKV" };
    EXPECT_EQ(ruleAt(CanonicalSignalRule::Priority, four_char, "KOKV00USA"),
              CanonicalSignalRule::Count);
    const std::vector<std::string> nine_char{ "KOKV00USA" };
    EXPECT_EQ(ruleAt(CanonicalSignalRule::Priority, nine_char, "KOKV"),
              CanonicalSignalRule::Count);
}

TEST(CanonicalSignalStationScope, AnUnlabelledStationIsNeverInsideAScope) {
    EXPECT_EQ(ruleAt(CanonicalSignalRule::Priority, kFour, ""),
              CanonicalSignalRule::Count);
    // The same holds if the list itself carries an empty entry (the loader drops
    // those, so this is a second guard).
    const std::vector<std::string> with_empty{ "", "KOKV00USA" };
    EXPECT_EQ(ruleAt(CanonicalSignalRule::Priority, with_empty, ""),
              CanonicalSignalRule::Count);
    EXPECT_EQ(ruleAt(CanonicalSignalRule::Priority, with_empty, "KOKV00USA"),
              CanonicalSignalRule::Priority);
}
