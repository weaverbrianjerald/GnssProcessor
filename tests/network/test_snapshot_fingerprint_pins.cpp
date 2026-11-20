/**
 * @file test_snapshot_fingerprint_pins.cpp
 * @brief Pins the snapshot fingerprint of the three goldens and four archived arm configurations.
 *
 * A drifted fingerprint cold-starts every snapshot that was saved before the drift, so the
 * value networkSnapshotFingerprint computes for a fixed configuration and a fixed station roster
 * is pinned here.  The three goldens live in the repository; the four arm configurations live
 * under the machine's rapid-run folder and their tests skip when the file is absent (fixture
 * gated).  A change that moves a pin is a deliberate fingerprint migration and is never a
 * side effect of a refactor.
 *
 * The roster is three fixed names with the first as the pivot: the pin guards the token
 * logic, not a particular run's roster.
 */
#include <gtest/gtest.h>

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

#include "config/GnssConfig.hpp"
#include "logging/LogManager.hpp"
#include "network/state/RunContext.hpp"

using namespace GnssProcessor;

namespace {

/// The fingerprint of @p cfg as 16 lower-case hex digits (three fixed stations, the first the pivot).
std::string fingerprintHex(const GnssConfig& cfg) {
    NetworkRunContext ctx(cfg, cfg.getNetworkConfig());
    ctx.carry.snap_path = "state_snapshot.snap";   // the builder does nothing without a path
    std::vector<NetStation> st(3);
    st[0].name = "AAAA"; st[0].is_pivot = true;
    st[1].name = "BBBB"; st[1].is_pivot = false;
    st[2].name = "CCCC"; st[2].is_pivot = false;
    networkSnapshotFingerprint(ctx, cfg, st);
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%016llx",
                  static_cast<unsigned long long>(ctx.carry.live_fingerprint));
    return buf;
}

/// Loads @p path and compares its fingerprint with @p pin; the value is printed so a deliberate migration can re-pin.
/// @param skip_when_absent  true for a configuration outside the repository.
void checkPin(const std::filesystem::path& path, const std::string& pin, bool skip_when_absent) {
    if (!std::filesystem::exists(path)) {
        if (skip_when_absent) GTEST_SKIP() << "fixture absent: " << path.string();
        FAIL() << "missing configuration " << path.string();
    }
    GnssConfig cfg;
    LogManager log;
    ASSERT_TRUE(cfg.loadFromJson(path, log)) << path.string();
    const std::string got = fingerprintHex(cfg);
    std::printf("FINGERPRINT %s %s\n", path.string().c_str(), got.c_str());
    EXPECT_EQ(got, pin) << path.string();
}

std::filesystem::path golden(const char* name) {
    return std::filesystem::path(GNSS_PROJECT_ROOT) / "config" / "golden" / name;
}

std::filesystem::path armConfig(const char* run) {
    return std::filesystem::path("D:/GnssProcessorSupport/runs_nom") / run / "arm.json";
}

} // namespace

TEST(SnapshotFingerprintPins, FieldGolden) {
    checkPin(golden("golden_field_refound.json"), "5d78e8a43c273007", false);
}
TEST(SnapshotFingerprintPins, SimGolden) {
    checkPin(golden("golden_sim_refound.json"), "c67cf69145345c5d", false);
}
TEST(SnapshotFingerprintPins, BeiDouGolden) {
    checkPin(golden("golden_field_bds_refound.json"), "ec139101e59f8ba3", false);
}
TEST(SnapshotFingerprintPins, ArmAdN146) {
    checkPin(armConfig("_rap_ad_n146"), "5d78e8a43c273007", true);
}
TEST(SnapshotFingerprintPins, ArmM1Off37) {
    checkPin(armConfig("_rap_m1_off37"), "e133cb5d927f7347", true);
}
TEST(SnapshotFingerprintPins, ArmH40Off) {
    checkPin(armConfig("_rap_h40_off"), "e133cb5d927f7347", true);
}
TEST(SnapshotFingerprintPins, ArmD24c32N147) {
    checkPin(armConfig("_rap_d24c32_n147"), "edb9c2a39b9c6dde", true);
}
