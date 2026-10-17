/**
 * @file test_zwd_seam_reseed_q.cpp
 * @brief @c network.zwd_seam_reseed_q: the random-walk process noise of the seam
 *        window, separate from the fingerprinted continuous carry parameter
 *        (docs/SPEC_event_harness.md §21.21).
 *
 * The knob resolves a policy conflict, not an arithmetic one.
 * @c network.zwd_carry_q is in the warm-resume fingerprint ("carry q's … must
 * COLD-start": a continuous carry is solve-affecting on both sides of a seam),
 * while @c network.zwd_seam_reseed, the resume response, is deliberately not.
 * When the response took its only weight from @c zwd_carry_q, that weight could
 * not be varied on a resume leg: the fingerprint check refused the bracket
 * pre-registered in §21.21 of SPEC_event_harness before its first epoch.
 *
 * The cases pin:
 *
 *  1. **Unset is byte-identical.**  The default is a negative sentinel, and the
 *     resolver returns @c zwd_carry_q for it in both window states.  If that
 *     stopped holding, an unarmed knob would invalidate every stored snapshot
 *     and golden hash.
 *  2. **The override is seam-scoped.**  An armed value governs only while the
 *     post-resume window is live; a continuous @c carry_zwd row keeps
 *     @c zwd_carry_q.  This containment lives in one expression
 *     (@c NetworkKnobs::zwdQEff), which this file tests.
 *  3. **Zero is refused at load.**  A q of 0 arms a window whose variance never
 *     inflates while every counter and log line reports it armed: an inert
 *     mechanism that looks live (standing invariant #4).
 *  4. **It is not in the warm-resume fingerprint.**  Tested against the real
 *     builder (@c networkSnapshotFingerprint), with @c zwd_carry_q as the
 *     positive control, so a fingerprint that hashed nothing could not pass as a
 *     null result.  Adding the knob to the fingerprint would bring back the
 *     conflict described above.
 *
 * The seam window's mechanics (arm at resume, roll, retire) are covered by
 * test_zwd_seam_reseed_config.cpp and the field ladder; this file covers only the
 * q contract.
 */

#include <gtest/gtest.h>

#include "network/state/RunContext.hpp"
#include "config/GnssConfig.hpp"
#include "network/driver/RunTypes.hpp"
#include "logging/LogManager.hpp"

#include <filesystem>
#include <fstream>
#include <string>
#include <vector>
#include "../support/TestTempDir.hpp"   // per-process, per-test scratch directory

namespace fs = std::filesystem;
using namespace GnssProcessor;

namespace {

/// Write @p body to a unique temp file and return its path.
fs::path writeTempConfig(const std::string& tag, const std::string& body) {
    const fs::path p = gnss_test::tempDir() / ("zwdseamq_cfg_" + tag + ".json");
    std::ofstream f(p);
    f << body;
    f.close();
    return p;
}

/// A minimal loadable PPP-RTK network config; @p extra splices into the network
/// block (test_zwd_seam_reseed_config.cpp's pattern, same two-station roster so
/// the two files' fingerprints are comparable by eye).
std::string networkConfig(const std::string& extra = "") {
    return std::string("{\n")
        + "  \"processing\": {\n"
          "    \"mode\": \"ppp-rtk-network\",\n"
          "    \"active_systems\": [ \"GPS\", \"Galileo\" ],\n"
          "    \"signal_priorities\": { \"GPS\": \"WC\", \"Galileo\": \"CB\" }\n"
          "  },\n"
          "  \"network\": {\n"
          "    \"clock_datum_station\": \"AAAA\",\n"
        + extra +
          "    \"stations\": [\n"
          "      { \"name\": \"AAAA\", \"position_ecef\": [ -2493304.99, -4655214.79, 3565497.67 ] },\n"
          "      { \"name\": \"BBBB\", \"position_ecef\": [ -2281621.75, -1453585.08, 5756964.89 ] }\n"
          "    ]\n"
          "  }\n"
          "}\n";
}

bool loadInto(const std::string& tag, const std::string& body, GnssConfig& cfg) {
    LogManager log;
    const fs::path p = writeTempConfig(tag, body);
    const bool ok = cfg.loadFromJson(p, log);
    std::error_code ec; fs::remove(p, ec);
    return ok;
}

/// The armed seam posture every case below starts from: leveling (the seam
/// inject rides the carry_warm gate), a snapshot path (a resume must be
/// reachable), and the response itself.
const char* const kArmedSeam =
    "    \"phase_leveling\": true,\n"
    "    \"state_snapshot_path\": \"state_snapshot.snap\",\n"
    "    \"zwd_seam_reseed\": true,\n";

/// The roster the fingerprint hashes (name + pivot flag are all it reads).
std::vector<NetStation> roster() {
    std::vector<NetStation> s;
    s.resize(2);
    s[0].name = "AAAA"; s[0].is_pivot = true;
    s[1].name = "BBBB"; s[1].is_pivot = false;
    return s;
}

/// Load @p extra, build the run context a real run would build, and return the
/// warm-resume fingerprint it computes.
uint64_t fingerprintOf(const std::string& tag, const std::string& extra) {
    GnssConfig cfg;
    EXPECT_TRUE(loadInto(tag, networkConfig(extra), cfg));
    NetworkRunContext ctx(cfg, cfg.getNetworkConfig());
    // The builder is a no-op on an empty snapshot path.  This is the only field it
    // needs beyond the knob snapshot the context already took.
    ctx.carry.snap_path = "state_snapshot.snap";
    const auto st = roster();
    networkSnapshotFingerprint(ctx, cfg, st);
    return ctx.carry.live_fingerprint;
}

}  // namespace

// ===========================================================================
// 1. Unset is the inherit sentinel, byte-identical to the build before RS-10b
// ===========================================================================

TEST(ZwdSeamReseedQ, DefaultIsTheInheritSentinel) {
    GnssConfig cfg;
    ASSERT_TRUE(loadInto("default", networkConfig(), cfg));
    EXPECT_LT(cfg.networkZwdSeamReseedQ(), 0.0);
    EXPECT_DOUBLE_EQ(cfg.networkZwdSeamReseedQ(), -1.0);
    // The parameter it defers to keeps its default of 1e-9.
    EXPECT_DOUBLE_EQ(cfg.networkZwdCarryQ(), 1e-9);
}

TEST(ZwdSeamReseedQ, UnsetResolvesToCarryQInBothWindowStates) {
    GnssConfig cfg;
    ASSERT_TRUE(loadInto("unset_resolve",
        networkConfig(std::string(kArmedSeam) + "    \"zwd_carry_q\": 3.5e-7,\n"), cfg));
    const NetworkKnobs k = NetworkKnobs::snapshot(cfg, cfg.getNetworkConfig());
    ASSERT_TRUE(k.zwd_seam_on);
    ASSERT_LT(k.zwd_seam_q, 0.0);
    // Bit equality on both branches: an unset knob does not perturb the fold by
    // even a rounding step, in or out of a seam window.
    EXPECT_EQ(k.zwdQEff(/*seam_live=*/true),  k.zwd_q);
    EXPECT_EQ(k.zwdQEff(/*seam_live=*/false), k.zwd_q);
    EXPECT_DOUBLE_EQ(k.zwd_q, 3.5e-7);
}

TEST(ZwdSeamReseedQ, AnyNegativeValueIsTheSentinel) {
    // The sentinel is any negative value, not only -1: a config that writes -1.0
    // explicitly, or any other negative, means the same thing and loads.
    for (const char* v : { "-1.0", "-1", "-2.5e-3" }) {
        GnssConfig cfg;
        ASSERT_TRUE(loadInto(std::string("neg_") + v,
            networkConfig(std::string(kArmedSeam)
                          + "    \"zwd_seam_reseed_q\": " + v + ",\n"), cfg))
            << "value " << v;
        const NetworkKnobs k = NetworkKnobs::snapshot(cfg, cfg.getNetworkConfig());
        EXPECT_EQ(k.zwdQEff(/*seam_live=*/true), k.zwd_q) << "value " << v;
    }
}

// ===========================================================================
// 2. An armed value governs the seam rows and no others
// ===========================================================================

TEST(ZwdSeamReseedQ, ArmedValueParsesAndIsReadable) {
    GnssConfig cfg;
    ASSERT_TRUE(loadInto("armed",
        networkConfig(std::string(kArmedSeam)
                      + "    \"zwd_seam_reseed_q\": 0.017,\n"), cfg));
    EXPECT_DOUBLE_EQ(cfg.networkZwdSeamReseedQ(), 0.017);
    // It leaves the continuous carry parameter at its default and does not arm the carry.
    EXPECT_DOUBLE_EQ(cfg.networkZwdCarryQ(), 1e-9);
    EXPECT_FALSE(cfg.networkCarryZwd());
}

TEST(ZwdSeamReseedQ, ArmedValueGovernsOnlyTheLiveSeamWindow) {
    GnssConfig cfg;
    ASSERT_TRUE(loadInto("seam_scoped",
        networkConfig(std::string(kArmedSeam)
                      + "    \"zwd_carry_q\": 1e-9,\n"
                        "    \"zwd_seam_reseed_q\": 0.017,\n"), cfg));
    const NetworkKnobs k = NetworkKnobs::snapshot(cfg, cfg.getNetworkConfig());
    ASSERT_DOUBLE_EQ(k.zwd_seam_q, 0.017);
    ASSERT_DOUBLE_EQ(k.zwd_q, 1e-9);
    // Inside the window: the seam's own stiffness.
    EXPECT_DOUBLE_EQ(k.zwdQEff(/*seam_live=*/true), 0.017);
    // Outside it (a continuous carry_zwd row, or the window retired): the
    // fingerprinted continuous parameter, unchanged.  The override does not reach
    // a non-seam fold.
    EXPECT_DOUBLE_EQ(k.zwdQEff(/*seam_live=*/false), 1e-9);
}

TEST(ZwdSeamReseedQ, OverrideStaysSeamScopedWithContinuousCarryAlsoArmed) {
    // carry_zwd together with the seam knob is a legal (subsumed) posture; for a
    // non-seam epoch the resolver still returns zwd_carry_q there.
    GnssConfig cfg;
    ASSERT_TRUE(loadInto("with_b9",
        networkConfig(std::string(kArmedSeam)
                      + "    \"carry_zwd\": true,\n"
                        "    \"zwd_carry_q\": 2e-9,\n"
                        "    \"zwd_seam_reseed_q\": 3.8e-4,\n"), cfg));
    const NetworkKnobs k = NetworkKnobs::snapshot(cfg, cfg.getNetworkConfig());
    ASSERT_TRUE(k.zwd_on);
    EXPECT_DOUBLE_EQ(k.zwdQEff(/*seam_live=*/false), 2e-9);
    EXPECT_DOUBLE_EQ(k.zwdQEff(/*seam_live=*/true),  3.8e-4);
}

// ===========================================================================
// 3. Zero is refused
// ===========================================================================

TEST(ZwdSeamReseedQ, ZeroIsRejected) {
    // A q of 0 would inflate nothing while every armed log line and counter reads
    // live: an inert mechanism that looks wired (standing invariant #4).
    GnssConfig cfg;
    EXPECT_FALSE(loadInto("zero",
        networkConfig(std::string(kArmedSeam)
                      + "    \"zwd_seam_reseed_q\": 0.0,\n"), cfg));
}

TEST(ZwdSeamReseedQ, ZeroIsRejectedEvenWithoutTheSeamArmed) {
    // The check sits at the parse site, not inside the `if (seam_reseed)` block,
    // so a config that pins a q of 0 without arming the response also fails to load.
    GnssConfig cfg;
    EXPECT_FALSE(loadInto("zero_unarmed",
        networkConfig("    \"zwd_seam_reseed_q\": 0,\n"), cfg));
}

// ===========================================================================
// 4. Not in the warm-resume fingerprint (with zwd_carry_q as positive control)
// ===========================================================================

TEST(ZwdSeamReseedQ, NotInTheWarmResumeFingerprint) {
    const uint64_t fp_unset = fingerprintOf("fp_unset", kArmedSeam);
    const uint64_t fp_q4    = fingerprintOf("fp_q4",
        std::string(kArmedSeam) + "    \"zwd_seam_reseed_q\": 0.00038,\n");
    const uint64_t fp_q2    = fingerprintOf("fp_q2",
        std::string(kArmedSeam) + "    \"zwd_seam_reseed_q\": 0.017,\n");
    ASSERT_NE(fp_unset, 0u);
    // One snapshot serves all three legs.
    EXPECT_EQ(fp_unset, fp_q4);
    EXPECT_EQ(fp_unset, fp_q2);
}

TEST(ZwdSeamReseedQ, CarryQIsStillFingerprinted) {
    // Positive control for the case above: a fingerprint that hashed none of the
    // ZWD terms would pass that case as a false null.
    const uint64_t fp_base = fingerprintOf("fp_base",
        std::string(kArmedSeam) + "    \"zwd_carry_q\": 1e-9,\n");
    const uint64_t fp_moved = fingerprintOf("fp_moved",
        std::string(kArmedSeam) + "    \"zwd_carry_q\": 3.8e-4,\n");
    EXPECT_NE(fp_base, fp_moved);
}

TEST(ZwdSeamReseedQ, SeamResponseKnobsStayExemptTogether) {
    // The exemption covers a family: the response, its horizon and its q.  If any
    // of them entered the fingerprint, the bracket could not run from one
    // snapshot, which is the failure §21.21 of SPEC_event_harness documents.
    const uint64_t fp_off = fingerprintOf("fp_off",
        "    \"phase_leveling\": true,\n"
        "    \"state_snapshot_path\": \"state_snapshot.snap\",\n");
    const uint64_t fp_on = fingerprintOf("fp_on",
        std::string(kArmedSeam) + "    \"zwd_seam_reseed_epochs\": 720,\n"
                                  "    \"zwd_seam_reseed_q\": 0.017,\n");
    EXPECT_EQ(fp_off, fp_on);
}
