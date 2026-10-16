/**
 * @file test_signal_roster_sites.cpp
 * @brief The site census of card NOM step N3b (network/assembly/SignalRosterSites.cpp): the roster
 *        read that the latch readers moved onto, and the rule sites the census
 *        evaluates on a roster.  Suite SignalRosterSitesTest.
 */
#include <gtest/gtest.h>

#include "network/assembly/SignalRoster.hpp"

#include <string>

namespace GnssProcessor {
namespace {

std::size_t at(RosterN3bSite s) { return static_cast<std::size_t>(s); }

SignalRoster rosterWith(const std::string& cons, char band, const std::string& phase,
                        const std::string& code) {
    SignalRoster rs;
    rs.by_station.resize(1);
    BandRoster& cell = rs.by_station[0][cons][band];
    cell.modelled_phase = phase;
    cell.modelled_code  = code;
    return rs;
}

TEST(SignalRosterSitesTest, ReadFallsBackToTheLatchWhereTheRosterHasNoRole) {
    const SignalRoster rs;   // no station built yet (a constellation's first epoch)
    RosterN3bCensus c;
    EXPECT_EQ(signalRosterModelledOr(rs, "L1W", 0, "GPS", '1', 'L', &c,
                                     RosterN3bSite::LatchReadTracker), "L1W");
    EXPECT_EQ(c.fallback[at(RosterN3bSite::LatchReadTracker)], 1);
    EXPECT_EQ(c.count[at(RosterN3bSite::LatchReadTracker)].compared, 0);
    // No latch either: nothing is answered and nothing is counted.
    EXPECT_EQ(signalRosterModelledOr(rs, "", 0, "GPS", '1', 'L', &c,
                                     RosterN3bSite::LatchReadTracker), "");
    EXPECT_EQ(c.fallback[at(RosterN3bSite::LatchReadTracker)], 1);
}

TEST(SignalRosterSitesTest, ReadAnswersFromTheRosterAndComparesWithTheLatch) {
    const SignalRoster rs = rosterWith("GPS", '1', "L1W", "C1W");
    RosterN3bCensus c;
    const RosterN3bSite s = RosterN3bSite::LatchReadSecondPhase;
    EXPECT_EQ(signalRosterModelledOr(rs, "L1W", 0, "GPS", '1', 'L', &c, s), "L1W");
    EXPECT_EQ(c.count[at(s)].compared, 1);
    EXPECT_EQ(c.count[at(s)].disagree, 0);
    // A latch the roster contradicts: the roster answers, and the read is counted.
    EXPECT_EQ(signalRosterModelledOr(rs, "L1C", 0, "GPS", '1', 'L', &c, s), "L1W");
    EXPECT_EQ(c.count[at(s)].disagree, 1);
    EXPECT_EQ(c.detail[at(s)].size(), 1u);
    // The code role, and a station outside the roster.
    EXPECT_EQ(signalRosterModelledOr(rs, "C1W", 0, "GPS", '1', 'C', nullptr, s), "C1W");
    EXPECT_EQ(signalRosterModelledOr(rs, "L1C", 3, "GPS", '1', 'L', nullptr, s), "L1C");
}

TEST(SignalRosterSitesTest, CensusCountsTheCanonicalPairAndTheTwoFamilyRules) {
    SignalRoster rs;
    rs.by_station.resize(1);
    auto& gps = rs.by_station[0]["GPS"];
    gps['1'].modelled_phase = "L1C";
    gps['2'].modelled_phase = "L2W";
    auto& gal = rs.by_station[0]["Galileo"];
    gal['1'].modelled_phase = "L1C";
    gal['5'].modelled_phase = "L5Q";
    gal['1'].modelled_code  = "C1X";
    gal['5'].modelled_code  = "C5X";
    RosterN3bCensus c;
    signalRosterSiteCensus(rs, c);
    // GPS models bands 1 and 2, Galileo 1 and 5: the canonical pair misses Galileo.
    EXPECT_EQ(c.count[at(RosterN3bSite::CanonicalPair)].compared, 2);
    EXPECT_EQ(c.count[at(RosterN3bSite::CanonicalPair)].disagree, 1);
    // Phases L1C~L2W and L1C~L5Q: one family by "X against the rest", two by the
    // letter; codes C1X~C5X: one family by both.
    EXPECT_EQ(c.count[at(RosterN3bSite::ChannelFamily)].compared, 3);
    EXPECT_EQ(c.count[at(RosterN3bSite::ChannelFamily)].disagree, 2);
    // No datum this epoch: the datum sites compare nothing.
    EXPECT_EQ(c.count[at(RosterN3bSite::GdPairBands)].compared, 0);
    EXPECT_EQ(c.count[at(RosterN3bSite::TraitsPairBands)].compared, 0);
}

TEST(SignalRosterSitesTest, CensusComparesThePinnedPairsWithTheGroupDelayAndTraitsPairs) {
    SignalRoster rs;
    rs.datum_pin["E01"] = { "C1C", "C5Q" };   // Galileo (E1, E5a): the group-delay pair
    rs.datum_pin["G01"] = { "C1C", "C5Q" };   // GPS (L1, L5): not the (L1, L2) pair
    RosterN3bCensus c;
    signalRosterSiteCensus(rs, c);
    EXPECT_EQ(c.count[at(RosterN3bSite::GdPairBands)].compared, 2);
    EXPECT_EQ(c.count[at(RosterN3bSite::GdPairBands)].disagree, 1);
    // Traits pairs: Galileo (1, 5) agrees with its datum bands, GPS (1, 2) does not.
    EXPECT_EQ(c.count[at(RosterN3bSite::TraitsPairBands)].compared, 2);
    EXPECT_EQ(c.count[at(RosterN3bSite::TraitsPairBands)].disagree, 1);
}

TEST(SignalRosterSitesTest, SpecTableIsInSiteOrder) {
    const auto& specs = signalRosterN3bSpecs();
    for (std::size_t k = 0; k < kRosterN3bSites; ++k)
        EXPECT_EQ(at(specs[k].site), k);
}

}  // namespace
}  // namespace GnssProcessor
