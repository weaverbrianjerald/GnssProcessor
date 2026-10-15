/**
 * @file test_network_we_choice.cpp
 * @brief The within-epoch tier's culprit choice (card NOM3 step SG2): weChooseCulprit, the pure
 *        function the adaptation loop calls once per iteration.  The gate on effect / sigma0, the
 *        surgical-first tie-break, the station de-rating, the consensus escalation and the
 *        exclusion of culprits already dropped.
 */
#include <gtest/gtest.h>

#include "filter/NetworkRaimWithinEpoch.hpp"
#include "filter/NetworkWithinEpochCulprits.hpp"

#include <map>
#include <string>
#include <utility>

using namespace GnssProcessor;

namespace {
using ArcEff = std::map<std::pair<int, std::string>, double>;
using SatEff = std::map<std::string, double>;
using StaEff = std::map<int, double>;
constexpr double kK = 3.0;     // raim_k
constexpr int    kMinSta = 3;  // raim_min_sta
}  // namespace

TEST(WeChooseCulprit, NothingOverTheGateIsNone) {
    const ArcEff arcs{{{0, "G01"}, 2.0}};
    const SatEff sats{{"G01", 2.5}};
    const StaEff stas{{0, 2.9}};
    const WeChoice c = weChooseCulprit(arcs, sats, stas, WithinEpochCulprits{}, 1.0, true, 1.0, kK, kMinSta);
    EXPECT_EQ(c.kind, WeChoice::Kind::None);
}

TEST(WeChooseCulprit, EqualScoresPreferTheArc) {
    const ArcEff arcs{{{2, "G05"}, 6.0}};
    const SatEff sats{{"G07", 6.0}};
    const StaEff stas{{4, 6.0}};
    const WeChoice c = weChooseCulprit(arcs, sats, stas, WithinEpochCulprits{}, 1.0, true, 1.0, kK, kMinSta);
    ASSERT_EQ(c.kind, WeChoice::Kind::Arc);
    EXPECT_EQ(c.arc_sta, 2);
    EXPECT_EQ(c.arc_sat, "G05");
}

TEST(WeChooseCulprit, WithoutSurgicalTheArcsAreNotCandidates) {
    const ArcEff arcs{{{2, "G05"}, 9.0}};
    const SatEff sats{{"G07", 6.0}};
    const WeChoice c = weChooseCulprit(arcs, sats, StaEff{}, WithinEpochCulprits{}, 1.0, false, 1.0, kK, kMinSta);
    ASSERT_EQ(c.kind, WeChoice::Kind::Sat);
    EXPECT_EQ(c.sat, "G07");
}

TEST(WeChooseCulprit, AStationIsDeratedBySta_bias) {
    const SatEff sats{{"E11", 6.0}};
    const StaEff stas{{7, 10.0}};
    const WeChoice derated = weChooseCulprit(ArcEff{}, sats, stas, WithinEpochCulprits{}, 1.0, true, 2.0, kK, kMinSta);
    EXPECT_EQ(derated.kind, WeChoice::Kind::Sat);   // 10 / 2 = 5 < 6
    const WeChoice dominant = weChooseCulprit(ArcEff{}, sats, stas, WithinEpochCulprits{}, 1.0, true, 1.5, kK, kMinSta);
    ASSERT_EQ(dominant.kind, WeChoice::Kind::Sta);   // 10 / 1.5 > 6
    EXPECT_EQ(dominant.sta, 7);
}

TEST(WeChooseCulprit, ConsensusEscalatesTheArcToItsSatellite) {
    // G09 is over the gate at three stations: a network-wide fault, so the whole satellite.
    const ArcEff arcs{{{0, "G09"}, 8.0}, {{1, "G09"}, 5.0}, {{2, "G09"}, 4.0}, {{3, "G12"}, 1.0}};
    const SatEff sats{{"G09", 4.5}};
    const WeChoice c = weChooseCulprit(arcs, sats, StaEff{}, WithinEpochCulprits{}, 1.0, true, 1.0, kK, kMinSta);
    ASSERT_EQ(c.kind, WeChoice::Kind::Sat);
    EXPECT_EQ(c.sat, "G09");
    EXPECT_EQ(c.arc_sta, -1);
    // Two stations over the gate stay an arc.
    const ArcEff two{{{0, "G09"}, 8.0}, {{1, "G09"}, 5.0}, {{2, "G09"}, 1.0}};
    const WeChoice a = weChooseCulprit(two, sats, StaEff{}, WithinEpochCulprits{}, 1.0, true, 1.0, kK, kMinSta);
    ASSERT_EQ(a.kind, WeChoice::Kind::Arc);
    EXPECT_EQ(a.arc_sta, 0);
}

TEST(WeChooseCulprit, CulpritsAlreadyDroppedAreSkipped) {
    const ArcEff arcs{{{0, "G01"}, 9.0}, {{1, "G02"}, 7.0}};
    const SatEff sats{{"G03", 8.0}, {"G04", 4.0}};
    const StaEff stas{{5, 20.0}};
    WithinEpochCulprits dropped;
    dropped.sats = {"G03"};
    dropped.arcs[0] = {"G01"};
    dropped.stations = {5};
    const WeChoice c = weChooseCulprit(arcs, sats, stas, dropped, 1.0, true, 1.0, kK, kMinSta);
    ASSERT_EQ(c.kind, WeChoice::Kind::Arc);
    EXPECT_EQ(c.arc_sta, 1);
    EXPECT_EQ(c.arc_sat, "G02");
}

TEST(WeChooseCulprit, TheGateScalesWithSigma0) {
    const SatEff sats{{"G01", 6.0}};
    EXPECT_EQ(weChooseCulprit(ArcEff{}, sats, StaEff{}, WithinEpochCulprits{}, 1.0, true, 1.0, kK, kMinSta).kind,
              WeChoice::Kind::Sat);
    EXPECT_EQ(weChooseCulprit(ArcEff{}, sats, StaEff{}, WithinEpochCulprits{}, 2.5, true, 1.0, kK, kMinSta).kind,
              WeChoice::Kind::None);   // 6 / 2.5 < 3
}
