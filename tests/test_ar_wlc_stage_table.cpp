/**
 * @file test_ar_wlc_stage_table.cpp
 * @brief Unit tests of the declared stage table of the WL-constrained user resolver
 *        (@ref kWlcStages) and of the held-pair test outcomes (@ref HoldCheck).
 *
 * The table is read by people: a rule of the user fixing audit (section 2, rows W) is
 * found in one stage row and one function.  The tests keep it whole:
 *   - nine rows, S1 to S9, in call order;
 *   - every row names a function, what it reads and what it writes;
 *   - every function name is distinct;
 *   - every rule of the audit (W1 to W18 with W5 as W5a to W5c, and W3a) is owned by exactly
 *     one row, and no other W rule appears;
 *   - every outcome of the held-pair test has its own name.
 *
 * Suite name @c ArWlcStageTable is owned solely by this file.
 */

#include <gtest/gtest.h>

#include "filter/AmbiguityResolverStages.hpp"

#include <map>
#include <regex>
#include <set>
#include <string>

using namespace GnssProcessor;

namespace {

TEST(ArWlcStageTable, NineStagesInCallOrder) {
    ASSERT_EQ(kWlcStageCount, 9u);
    for (std::size_t k = 0; k < kWlcStageCount; ++k)
        EXPECT_EQ(std::string(kWlcStages[k].id), "S" + std::to_string(k + 1));
}

TEST(ArWlcStageTable, EveryRowNamesAFunctionWhatItReadsAndWhatItWrites) {
    for (const auto& row : kWlcStages) {
        EXPECT_FALSE(std::string(row.function).empty()) << row.id;
        EXPECT_FALSE(std::string(row.reads).empty()) << row.id;
        EXPECT_FALSE(std::string(row.writes).empty()) << row.id;
        EXPECT_FALSE(std::string(row.rules).empty()) << row.id;
    }
}

TEST(ArWlcStageTable, EveryFunctionNameIsDistinct) {
    std::set<std::string> names;
    for (const auto& row : kWlcStages) EXPECT_TRUE(names.insert(row.function).second) << row.function;
}

TEST(ArWlcStageTable, EveryRuleOfTheAuditIsOwnedByExactlyOneStage) {
    std::map<std::string, int> owners;
    const std::regex rule(R"(\bW([0-9]+[a-c]?)\b)");
    for (const auto& row : kWlcStages) {
        const std::string text = row.rules;
        for (auto it = std::sregex_iterator(text.begin(), text.end(), rule); it != std::sregex_iterator(); ++it)
            ++owners["W" + (*it)[1].str()];
    }
    // The audit's rows: W1 to W18, with W5 split into W5a, W5b and W5c, and W3a.
    std::set<std::string> want = { "W3a", "W5a", "W5b", "W5c" };
    for (int k = 1; k <= 18; ++k)
        if (k != 5) want.insert("W" + std::to_string(k));
    for (const auto& w : want) EXPECT_EQ(owners[w], 1) << w;
    for (const auto& [w, n] : owners) EXPECT_TRUE(want.count(w)) << "unknown rule " << w;
}

TEST(ArWlcStageTable, EveryHoldCheckOutcomeHasItsOwnName) {
    const HoldCheck all[] = { HoldCheck::Ok, HoldCheck::Contradicted, HoldCheck::UnusableGroup,
                              HoldCheck::KeyMiss, HoldCheck::BadIndex };
    std::set<std::string> names;
    for (const HoldCheck c : all) {
        const std::string n = holdCheckName(c);
        EXPECT_NE(n, "?");
        EXPECT_TRUE(names.insert(n).second) << n;
    }
    EXPECT_STREQ(holdCheckName(HoldCheck::KeyMiss), "key_miss");
}

} // namespace
