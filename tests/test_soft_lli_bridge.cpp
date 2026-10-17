/**
 * @file test_soft_lli_bridge.cpp
 * @brief Unit tests for SoftLliBridge, the shared soft-LLI and gap-bridge
 *        hold orchestration: the per-signal-dropout path
 *        (registerGapHolds) and the restore and fallback contracts of
 *        resolveHolds.
 *
 * Scenario under test: a phase signal drops for a few epochs with no
 * receiver LLI.  The hold is registered at the first missing epoch (so the
 * caller can protect the ambiguity from the prune and exclude it from the
 * KF), survives the gap, and resolves to a snapshot + dN·λ integer repair
 * when the TurboEdit gap bridge adjudicates the outage.  When the restore
 * cannot be applied, it falls back to a reset and logs a warning.
 */

#include <gtest/gtest.h>

#include "common/PppObservation.hpp"
#include "config/GnssConfig.hpp"
#include "filter/StateManager.hpp"
#include "logging/LogManager.hpp"
#include "slip/SoftLliBridge.hpp"
#include "slip/TurboEditResolver.hpp"

#include <Eigen/Dense>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <string>
#include <vector>
#include "support/TestTempDir.hpp"   // per-process, per-test scratch directory

namespace GnssProcessor {
namespace {

constexpr double kC  = 299792458.0;
const double kL2 = kC / 1227.60e6;   // GPS L2 wavelength ≈ 0.24421 m

constexpr size_t kPppCoreSize = 5;

/// Load a GnssConfig with the TurboEdit gap bridge enabled through the real
/// JSON path (turbo_edit + turbo_edit_bridge_lli + default 15 s cap).
void loadBridgeConfig(GnssConfig& config, LogManager& log_manager,
                      const std::string& tag) {
    namespace fs = std::filesystem;
    const fs::path p = gnss_test::tempDir() / ("gnss_soft_lli_" + tag + ".json");
    {
        std::ofstream f(p);
        f << R"({ "estimation": { "cycle_slip": {
                "turbo_edit": true, "turbo_edit_bridge_lli": true } } })";
    }
    ASSERT_TRUE(config.loadFromJson(p, log_manager));
    std::error_code ec;
    fs::remove(p, ec);
}

/// Test harness: bridge-enabled config, a StateManager to hold ambiguity
/// states, and a resolver to receive the held-continuous marks.  Member
/// order keeps the refs alive for the dependents.
struct BridgeHarness {
    LogManager log_manager;
    GnssConfig config;
    StateManager      sm{ kPppCoreSize, log_manager, config };
    SoftLliBridge     bridge{ config, log_manager };
    TurboEditResolver te{ config, log_manager };

    explicit BridgeHarness(const std::string& tag) {
        loadBridgeConfig(config, log_manager, tag);
        bridge.setSampleInterval(1.0);  // high-rate gate
    }

    /// Add an ambiguity state at a converged variance (well under the
    /// snapshot ceiling) so recordSnapshot captures it.
    size_t addConvergedAmbiguity(const std::string& key, double value,
                                 double variance = 0.01) {
        const size_t idx = sm.addAmbiguity(key, value);
        Eigen::MatrixXd P = sm.getCovariance();
        P(static_cast<Eigen::Index>(idx), static_cast<Eigen::Index>(idx)) = variance;
        sm.setCovariance(P);
        return idx;
    }
};

/// Observation carrying the given raw phase keys in @c observed (the map
/// registerGapHolds scans for this epoch's tracked signals).
PppObservation makeObs(const std::string& sat,
                       const std::vector<std::string>& phase_signals) {
    PppObservation obs{};
    obs.satellite = sat;
    obs.system = (sat[0] == 'R') ? System::GLONASS : System::GPS;
    obs.elevation = 45.0;
    for (const auto& sig : phase_signals)
        obs.observed[sig] = 100.0;
    return obs;
}

// ---------------------------------------------------------------------------
// registerGapHolds: registration and gates
// ---------------------------------------------------------------------------

// Main case: G01 tracked on L1C+L2W with converged states; L2W produces no
// observation this epoch (no LLI).  Only the dropped signal is held.
TEST(SoftLliGapHolds, DroppedSignalIsHeldObservedSignalIsNot) {
    BridgeHarness h("drop");
    h.addConvergedAmbiguity("G01_L1C", 12.5);
    h.addConvergedAmbiguity("G01_L2W", -3.25);
    h.bridge.recordSnapshot(h.sm, /*epoch=*/100.0);

    const std::vector<PppObservation> epoch = { makeObs("G01", { "L1C" }) };
    h.bridge.registerGapHolds(epoch, h.sm, /*is_bssd_pivot=*/nullptr,
                              h.te, /*epoch_now_s=*/101.0, "");

    EXPECT_EQ(h.bridge.heldKeys(), std::set<std::string>{ "G01_L2W" });

    // Idempotent across the gap: re-registration on later missing epochs
    // must not duplicate or disturb the hold.
    h.bridge.registerGapHolds(epoch, h.sm, nullptr, h.te, 102.0, "");
    EXPECT_EQ(h.bridge.heldKeys().size(), 1u);
}

// Gates: disabled bridge, GLONASS (not AR-capable), BSSD pivot, stale
// snapshot, and never-converged (no snapshot) keys all fall through to the
// normal prune, with no hold.
TEST(SoftLliGapHolds, GatesRefuseIneligibleKeys) {
    // Disabled (default config: turbo_edit_bridge_lli off).
    {
        LogManager lm;
        GnssConfig cfg;
        StateManager sm{ kPppCoreSize, lm, cfg };
        SoftLliBridge bridge{ cfg, lm };
        TurboEditResolver te{ cfg, lm };
        bridge.setSampleInterval(1.0);
        sm.addAmbiguity("G01_L2W", 1.0);
        bridge.recordSnapshot(sm, 100.0);
        bridge.registerGapHolds({ makeObs("G01", { "L1C" }) }, sm, nullptr,
                                te, 101.0, "");
        EXPECT_TRUE(bridge.heldKeys().empty());
    }
    // GLONASS: FDMA is excluded by qualifies().
    {
        BridgeHarness h("glo");
        h.addConvergedAmbiguity("R05_L2C", 4.0);
        h.bridge.recordSnapshot(h.sm, 100.0);
        h.bridge.registerGapHolds({ makeObs("R05", { "L1C" }) }, h.sm, nullptr,
                                  h.te, 101.0, "");
        EXPECT_TRUE(h.bridge.heldKeys().empty());
    }
    // BSSD pivot: the caller's predicate wins.
    {
        BridgeHarness h("pivot");
        h.addConvergedAmbiguity("G01_L2W", 4.0);
        h.bridge.recordSnapshot(h.sm, 100.0);
        h.bridge.registerGapHolds({ makeObs("G01", { "L1C" }) }, h.sm,
                                  [](const std::string&) { return true; },
                                  h.te, 101.0, "");
        EXPECT_TRUE(h.bridge.heldKeys().empty());
    }
    // Stale snapshot: the last converged value is older than the bridge cap
    // (15 s), so the satellite was not actively tracking and the prune
    // reclaims the state.
    {
        BridgeHarness h("stale");
        h.addConvergedAmbiguity("G01_L2W", 4.0);
        h.bridge.recordSnapshot(h.sm, 100.0);
        h.bridge.registerGapHolds({ makeObs("G01", { "L1C" }) }, h.sm, nullptr,
                                  h.te, 100.0 + 20.0, "");
        EXPECT_TRUE(h.bridge.heldKeys().empty());
    }
    // Never converged: no snapshot anchor exists, so a snapshot+dN·λ repair
    // has nothing to restore, and no hold is registered.
    {
        BridgeHarness h("uncvg");
        h.addConvergedAmbiguity("G01_L2W", 4.0, /*variance=*/1.0e6);
        h.bridge.recordSnapshot(h.sm, 100.0);
        h.bridge.registerGapHolds({ makeObs("G01", { "L1C" }) }, h.sm, nullptr,
                                  h.te, 101.0, "");
        EXPECT_TRUE(h.bridge.heldKeys().empty());
    }
}

// ---------------------------------------------------------------------------
// resolveHolds: restore and fallback contracts
// ---------------------------------------------------------------------------

// A bridge repair arrives for a held key: the ambiguity is overwritten with
// anchor + dN·λ at the snapshot variance, the hold is released, and the
// satellite is reported repaired (so the caller skips its own reset).
TEST(SoftLliGapHolds, BridgeRepairRestoresAnchorPlusIntegerJump) {
    BridgeHarness h("repair");
    const double anchor = -3.25;
    const size_t idx = h.addConvergedAmbiguity("G01_L2W", anchor, 0.01);
    h.bridge.recordSnapshot(h.sm, 100.0);
    h.bridge.registerGapHolds({ makeObs("G01", { "L1C" }) }, h.sm, nullptr,
                              h.te, 101.0, "");
    ASSERT_EQ(h.bridge.heldKeys().size(), 1u);

    // Simulate the post-gap KF reseed that the hold guards against: the live
    // value has drifted, and the restore has to come from the snapshot.
    {
        Eigen::VectorXd x = h.sm.getStateVector();
        x(static_cast<Eigen::Index>(idx)) = 999.0;
        h.sm.setStateVector(x);
    }

    TurboEditResolver::EpochResolution te_res;
    te_res.repairs.push_back({ "G01", "L2W", /*dN=*/+3, kL2, 0.0 });

    std::set<std::string> repaired_sats;
    int n_repaired = 0, n_reset = 0;
    int reset_calls = 0;
    h.bridge.resolveHolds(te_res, h.te, { makeObs("G01", { "L1C", "L2W" }) },
                          h.sm, /*epoch_now_s=*/106.0, "", repaired_sats,
                          n_repaired, n_reset,
                          [&](const std::string&, const std::string&,
                              const std::string&) { ++reset_calls; });

    EXPECT_EQ(repaired_sats.count("G01"), 1u);
    EXPECT_EQ(n_repaired, 1);
    EXPECT_EQ(reset_calls, 0);
    EXPECT_TRUE(h.bridge.heldKeys().empty());
    EXPECT_NEAR(h.sm.getStateVector()(static_cast<Eigen::Index>(idx)),
                anchor + 3.0 * kL2, 1e-12);
    EXPECT_NEAR(h.sm.getCovariance()(static_cast<Eigen::Index>(idx),
                                     static_cast<Eigen::Index>(idx)),
                0.01, 1e-12);
}

// A restore failure is terminal: when the held state has vanished (a gap in
// prune protection), the fallback reset fires once and only once (the dN = 0
// partner loop skips a signal already acted on), and the satellite is not
// reported repaired.
TEST(SoftLliGapHolds, RestoreFailureFallsBackToSingleReset) {
    BridgeHarness h("fallback");
    h.addConvergedAmbiguity("G01_L2W", -3.25);
    h.bridge.recordSnapshot(h.sm, 100.0);
    h.bridge.registerGapHolds({ makeObs("G01", { "L1C" }) }, h.sm, nullptr,
                              h.te, 101.0, "");
    ASSERT_EQ(h.bridge.heldKeys().size(), 1u);

    // The failure under test: the held state is deleted while the hold is
    // active (what an unprotected prune does).
    ASSERT_TRUE(h.sm.removeAmbiguity("G01_L2W"));

    TurboEditResolver::EpochResolution te_res;
    te_res.repairs.push_back({ "G01", "L2W", +3, kL2, 0.0 });

    std::set<std::string> repaired_sats;
    int n_repaired = 0, n_reset = 0;
    int reset_calls = 0;
    h.bridge.resolveHolds(te_res, h.te, {}, h.sm, 106.0, "", repaired_sats,
                          n_repaired, n_reset,
                          [&](const std::string& key, const std::string&,
                              const std::string&) {
                              EXPECT_EQ(key, "G01_L2W");
                              ++reset_calls;
                          });

    EXPECT_EQ(reset_calls, 1);
    EXPECT_EQ(n_reset, 1);
    EXPECT_EQ(n_repaired, 0);
    EXPECT_TRUE(repaired_sats.empty());
    EXPECT_TRUE(h.bridge.heldKeys().empty());
}

// Outage exceeding the bridge cap with no adjudication: the hold times out
// to the path-specific reset and is released.
TEST(SoftLliGapHolds, TimeoutFallsBackToReset) {
    BridgeHarness h("timeout");
    h.addConvergedAmbiguity("G01_L2W", -3.25);
    h.bridge.recordSnapshot(h.sm, 100.0);
    h.bridge.registerGapHolds({ makeObs("G01", { "L1C" }) }, h.sm, nullptr,
                              h.te, 101.0, "");
    ASSERT_EQ(h.bridge.heldKeys().size(), 1u);

    std::set<std::string> repaired_sats;
    int n_repaired = 0, n_reset = 0;
    int reset_calls = 0;
    const TurboEditResolver::EpochResolution empty_res;

    // Within the cap: the hold persists (measurements stay excluded).
    h.bridge.resolveHolds(empty_res, h.te, {}, h.sm, /*epoch=*/110.0, "",
                          repaired_sats, n_repaired, n_reset,
                          [&](const std::string&, const std::string&,
                              const std::string&) { ++reset_calls; });
    EXPECT_EQ(h.bridge.heldKeys().size(), 1u);
    EXPECT_EQ(reset_calls, 0);

    // Beyond the 15 s cap: fresh reset, hold released.
    h.bridge.resolveHolds(empty_res, h.te, {}, h.sm, /*epoch=*/101.0 + 16.0, "",
                          repaired_sats, n_repaired, n_reset,
                          [&](const std::string&, const std::string&,
                              const std::string&) { ++reset_calls; });
    EXPECT_EQ(reset_calls, 1);
    EXPECT_EQ(n_reset, 1);
    EXPECT_TRUE(h.bridge.heldKeys().empty());
}

// The timeout is per key, not per satellite: a signal that joins an existing
// outage late gets the full cap measured from its own hold start.  A
// per-satellite timer anchored at the earliest hold would reset the late
// signal a few seconds into its own window (field case: L1C+L2W held at T,
// L2L joining at T+8).
TEST(SoftLliGapHolds, TimeoutIsPerKeyNotPerSatellite) {
    BridgeHarness h("perkey");
    h.addConvergedAmbiguity("G01_L1C", 12.5);
    h.addConvergedAmbiguity("G01_L2W", -3.25);
    h.addConvergedAmbiguity("G01_L2L", 7.75);
    h.bridge.recordSnapshot(h.sm, /*epoch=*/100.0);

    // T=101: L1C + L2W drop (L2L still tracked) -> two holds start.
    h.bridge.registerGapHolds({ makeObs("G01", { "L2L" }) }, h.sm, nullptr,
                              h.te, 101.0, "");
    EXPECT_EQ(h.bridge.heldKeys(),
              (std::set<std::string>{ "G01_L1C", "G01_L2W" }));

    // T=109: L2L drops too, joining the outage 8 s late.
    h.bridge.registerGapHolds({}, h.sm, nullptr, h.te, 109.0, "");
    EXPECT_EQ(h.bridge.heldKeys().size(), 3u);

    std::set<std::string> repaired_sats;
    int n_repaired = 0, n_reset = 0;
    std::set<std::string> reset_keys;
    const TurboEditResolver::EpochResolution empty_res;
    const auto count_resets = [&](const std::string& key, const std::string&,
                                  const std::string&) { reset_keys.insert(key); };

    // T=117: L1C/L2W are 16 s old (> 15 s cap) -> expire; L2L is 8 s old
    // and remains held with its own window intact.
    h.bridge.resolveHolds(empty_res, h.te, {}, h.sm, /*epoch=*/117.0, "",
                          repaired_sats, n_repaired, n_reset, count_resets);
    EXPECT_EQ(reset_keys,
              (std::set<std::string>{ "G01_L1C", "G01_L2W" }));
    EXPECT_EQ(n_reset, 2);
    EXPECT_EQ(h.bridge.heldKeys(), std::set<std::string>{ "G01_L2L" });

    // T=125: L2L's own 15 s window (start 109) is now exceeded -> expires.
    h.bridge.resolveHolds(empty_res, h.te, {}, h.sm, /*epoch=*/125.0, "",
                          repaired_sats, n_repaired, n_reset, count_resets);
    EXPECT_EQ(n_reset, 3);
    EXPECT_EQ(reset_keys.count("G01_L2L"), 1u);
    EXPECT_TRUE(h.bridge.heldKeys().empty());
    EXPECT_TRUE(repaired_sats.empty());
}

} // namespace
} // namespace GnssProcessor
