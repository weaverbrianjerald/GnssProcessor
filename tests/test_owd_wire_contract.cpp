/**
 * @file test_owd_wire_contract.cpp
 * @brief The S contract line is part of every wire (card OWD, O1).
 *
 * Two pure functions, tested without a producer run:
 *   - @c NetworkProductReader::admitsWire, the replay's refusal of a wire without an S
 *     contract line (such a wire replays with the default emitter, so it cannot
 *     reproduce the producer's clock declarations) unless the consumer opts in
 *     explicitly through @c network.replay_allow_undeclared_wire;
 *   - @c aprioriIdentityForWindow, the S line's a-priori identity naming the product files
 *     in force during the processing window (the resolver merges the previous, the
 *     observation and the next day, so the first merged SP3 is the previous day's and
 *     does not identify the product in force).
 */
#include <gtest/gtest.h>

#include "math/TimeUtils.hpp"
#include "products/NetworkProductIO.hpp"

#include <filesystem>
#include <string>
#include <vector>

using namespace GnssProcessor;
namespace fs = std::filesystem;

namespace {

/// Continuous GPS seconds of a GPS-scale calendar instant.
double gpsSeconds(int y, int mo, int d, int h, int mi) {
    return TimeUtils::continuousGpsSeconds(TimeUtils::gregorianGpsToGpsTime(y, mo, d, h, mi, 0.0));
}

/// The DOY-128/129/130 COD merge the path resolver builds for a DOY-129 run, plus the
/// non-orbit products that must never enter the identity.
std::vector<fs::path> threeDayMerge() {
    return {
        "data/2024-128/products/COD0MGXFIN_20241280000_01D_05M_ORB.SP3",
        "data/2024-128/products/COD0MGXFIN_20241280000_01D_30S_CLK.CLK",
        "data/2024-129/products/COD0MGXFIN_20241290000_01D_05M_ORB.SP3",
        "data/2024-129/products/COD0MGXFIN_20241290000_01D_30S_CLK.CLK",
        "data/2024-129/products/COD0MGXFIN_20241290000_01D_01D_OSB.BIA",
        "data/2024-130/products/COD0MGXFIN_20241300000_01D_05M_ORB.SP3",
        "data/2024-130/products/COD0MGXFIN_20241300000_01D_30S_CLK.CLK",
        "data/static/atx/igs20.atx",
    };
}

}  // namespace

// A wire with a contract is admitted and needs no reason.
TEST(UndeclaredWireAdmission, DeclaredWireIsAdmitted) {
    SbasisContract c;
    c.present = true;
    std::string why = "stale";
    EXPECT_TRUE(NetworkProductReader::admitsWire(c, /*allow_undeclared=*/false, &why));
    EXPECT_TRUE(why.empty());
    EXPECT_TRUE(NetworkProductReader::admitsWire(c, /*allow_undeclared=*/true, &why));
    EXPECT_TRUE(why.empty());
}

// A wire without a contract is refused by default, and the reason names the cause and
// both fixes (a current build, or the knob).
TEST(UndeclaredWireAdmission, UndeclaredWireIsRefusedByDefault) {
    SbasisContract c;            // present = false: a pre-OWD, contract-less wire
    std::string why;
    EXPECT_FALSE(NetworkProductReader::admitsWire(c, /*allow_undeclared=*/false, &why));
    EXPECT_NE(why.find("no S-basis contract line"), std::string::npos) << why;
    EXPECT_NE(why.find("clock-discontinuity channel"), std::string::npos) << why;
    EXPECT_NE(why.find("Re-produce the wire"), std::string::npos) << why;
    EXPECT_NE(why.find("network.replay_allow_undeclared_wire"), std::string::npos) << why;
}

// The knob admits a contract-less wire on purpose, and the reason still says what it is.
TEST(UndeclaredWireAdmission, KnobAdmitsALegacyLegOnPurpose) {
    SbasisContract c;
    std::string why;
    EXPECT_TRUE(NetworkProductReader::admitsWire(c, /*allow_undeclared=*/true, &why));
    EXPECT_NE(why.find("replaying it anyway"), std::string::npos) << why;
    EXPECT_EQ(why.find("Re-produce the wire"), std::string::npos) << why;
    // The out-parameter is optional.
    EXPECT_TRUE(NetworkProductReader::admitsWire(c, true));
    EXPECT_FALSE(NetworkProductReader::admitsWire(c, false));
}

// A 00:00 to 06:00 window of DOY 129 names the DOY-129 pair only, in the pre-OWD shape:
// the previous day's files end exactly at the window start (half-open spans), the next
// day's start after its end.
TEST(UndeclaredWireAdmission, IdentityNamesTheFilesInForce) {
    const double t0 = gpsSeconds(2024, 5, 8, 0, 0);
    bool named_all = true;
    const std::string id = aprioriIdentityForWindow(threeDayMerge(), t0, t0 + 6 * 3600.0,
                                                    &named_all);
    EXPECT_EQ(id, "COD0MGXFIN_20241290000_01D_05M_ORB.SP3+COD0MGXFIN_20241290000_01D_30S_CLK.CLK");
    EXPECT_FALSE(named_all);
}

// A window across midnight names both days of each kind, SP3s first, resolver order.
TEST(UndeclaredWireAdmission, WindowAcrossMidnightNamesBothDays) {
    const double t0 = gpsSeconds(2024, 5, 8, 18, 0);
    const std::string id = aprioriIdentityForWindow(threeDayMerge(), t0, t0 + 12 * 3600.0);
    EXPECT_EQ(id,
              "COD0MGXFIN_20241290000_01D_05M_ORB.SP3+COD0MGXFIN_20241300000_01D_05M_ORB.SP3+"
              "COD0MGXFIN_20241290000_01D_30S_CLK.CLK+COD0MGXFIN_20241300000_01D_30S_CLK.CLK");
}

// A name without an IGS long-name span makes its kind name every merged file (flagged);
// the other kind still names the file in force.
TEST(UndeclaredWireAdmission, UnparsableNameNamesEveryFileOfItsKind) {
    std::vector<fs::path> paths = threeDayMerge();
    paths.push_back("data/2024-129/products/MYORBIT.SP3");
    const double t0 = gpsSeconds(2024, 5, 8, 0, 0);
    bool named_all = false;
    const std::string id = aprioriIdentityForWindow(paths, t0, t0 + 2 * 3600.0, &named_all);
    EXPECT_TRUE(named_all);
    EXPECT_EQ(id,
              "COD0MGXFIN_20241280000_01D_05M_ORB.SP3+COD0MGXFIN_20241290000_01D_05M_ORB.SP3+"
              "COD0MGXFIN_20241300000_01D_05M_ORB.SP3+MYORBIT.SP3+"
              "COD0MGXFIN_20241290000_01D_30S_CLK.CLK");
}

// No SP3 and no CLK: an empty identity, so the caller keeps its configuration intent.
TEST(UndeclaredWireAdmission, NoOrbitOrClockFileGivesAnEmptyIdentity) {
    const std::vector<fs::path> paths = {"data/static/atx/igs20.atx",
                                         "data/2024-129/products/COD0MGXFIN_20241290000_01D_01D_OSB.BIA"};
    const double t0 = gpsSeconds(2024, 5, 8, 0, 0);
    EXPECT_TRUE(aprioriIdentityForWindow(paths, t0, t0 + 3600.0).empty());
}
