/**
 * @file test_zwd_seam_reseed_q.cpp
 * @brief Card RS-10b (docs/SPEC_event_harness.md §21.21) — @c
 *        network.zwd_seam_reseed_q, the seam window's OWN random-walk process
 *        noise, decoupled from the fingerprinted continuous carry parameter it
 *        used to borrow.
 *
 * The defect this knob closes is a POLICY collision, not an arithmetic one:
 * @c network.zwd_carry_q is IN the warm-resume fingerprint ("carry q's … must
 * COLD-start" — a continuous carry is solve-affecting on both sides of a seam),
 * while @c network.zwd_seam_reseed, the resume RESPONSE whose only weight was
 * borrowed from it, deliberately is NOT.  So the one knob that response exists
 * to tune could not be varied on a resume leg at all: §21.21's pre-registered
 * bracket was REFUSED at the fingerprint check before it ran an epoch.
 *
 * What is pinned here, and why each is worth a case:
 *
 *  1. **Unset is byte-identical.**  The default is a negative sentinel and the
 *     resolver returns @c zwd_carry_q for it in BOTH window states — if that
 *     ever stops holding, every stored snapshot and golden hash is invalidated
 *     by a knob nobody armed.
 *  2. **The override is SEAM-SCOPED.**  An armed value governs only while the
 *     post-resume window is live; a continuous @c carry_zwd row keeps
 *     @c zwd_carry_q.  This is the whole containment claim of the card, and it
 *     lives in exactly one expression (@c NetworkKnobs::zwdQEff), which is what
 *     this file tests.
 *  3. **Zero is refused at load.**  A q of 0 arms a window whose variance never
 *     inflates while every counter and log line reads ARMED — an inert
 *     mechanism that looks live is the standing-invariant-#4 failure.
 *  4. **It is NOT in the warm-resume fingerprint.**  Tested against the real
 *     builder (@c networkSnapshotFingerprint), with @c zwd_carry_q as the
 *     positive control so a fingerprint that stopped hashing ANYTHING cannot
 *     pass this as a null result.  A knob that landed in the fingerprint would
 *     re-create the exact defect the card removes.
 *
 * The seam window's mechanics (arm at resume, roll, retire) belong to
 * test_zwd_seam_reseed_config.cpp and the field ladder; only the q contract is
 * here.
 */

#include <gtest/gtest.h>

#include "app/NetworkRunContext.hpp"
#include "config/GnssConfig.hpp"
#include "filter/NetworkRunTypes.hpp"
#include "logging/LogManager.hpp"

#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using namespace GnssProcessor;

namespace {

/// Write @p body to a unique temp file and return its path.
fs::path writeTempConfig(const std::string& tag, const std::string& body) {
    const fs::path p = fs::temp_directory_path() / ("zwdseamq_cfg_" + tag + ".json");
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
    // The builder is a no-op on an empty snapshot path — this is the only field
    // it needs beyond the knob snapshot it already took.
    ctx.carry.snap_path = "state_snapshot.snap";
    const auto st = roster();
    networkSnapshotFingerprint(ctx, cfg, st);
    return ctx.carry.live_fingerprint;
}

}  // namespace

// ===========================================================================
// 1. Unset is the inherit sentinel — byte-identical to the pre-card build
// ===========================================================================

TEST(ZwdSeamReseedQ, DefaultIsTheInheritSentinel) {
    GnssConfig cfg;
    ASSERT_TRUE(loadInto("default", networkConfig(), cfg));
    EXPECT_LT(cfg.networkZwdSeamReseedQ(), 0.0);
    EXPECT_DOUBLE_EQ(cfg.networkZwdSeamReseedQ(), -1.0);
    // The parameter it defers to is untouched by this card.
    EXPECT_DOUBLE_EQ(cfg.networkZwdCarryQ(), 1e-9);
}

TEST(ZwdSeamReseedQ, UnsetResolvesToCarryQInBothWindowStates) {
    GnssConfig cfg;
    ASSERT_TRUE(loadInto("unset_resolve",
        networkConfig(std::string(kArmedSeam) + "    \"zwd_carry_q\": 3.5e-7,\n"), cfg));
    const NetworkKnobs k = NetworkKnobs::snapshot(cfg, cfg.getNetworkConfig());
    ASSERT_TRUE(k.zwd_seam_on);
    ASSERT_LT(k.zwd_seam_q, 0.0);
    // BIT equality on both branches: an unset knob must not perturb the fold by
    // so much as a rounding step, in or out of a seam window.
    EXPECT_EQ(k.zwdQEff(/*seam_live=*/true),  k.zwd_q);
    EXPECT_EQ(k.zwdQEff(/*seam_live=*/false), k.zwd_q);
    EXPECT_DOUBLE_EQ(k.zwd_q, 3.5e-7);
}

TEST(ZwdSeamReseedQ, AnyNegativeValueIsTheSentinel) {
    // The sentinel is "negative", not "exactly -1" — a config that writes -1.0
    // explicitly, or any other negative, means the same thing and must load.
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
// 2. An armed value governs the SEAM rows, and only those
// ===========================================================================

TEST(ZwdSeamReseedQ, ArmedValueParsesAndIsReadable) {
    GnssConfig cfg;
    ASSERT_TRUE(loadInto("armed",
        networkConfig(std::string(kArmedSeam)
                      + "    \"zwd_seam_reseed_q\": 0.017,\n"), cfg));
    EXPECT_DOUBLE_EQ(cfg.networkZwdSeamReseedQ(), 0.017);
    // It does NOT touch the continuous carry parameter or arm the carry itself.
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
    // fingerprinted continuous parameter, untouched.  This is the containment
    // claim — the override must never leak onto a non-seam fold.
    EXPECT_DOUBLE_EQ(k.zwdQEff(/*seam_live=*/false), 1e-9);
}

TEST(ZwdSeamReseedQ, OverrideStaysSeamScopedWithContinuousCarryAlsoArmed) {
    // carry_zwd + the seam knob is a legal (subsumed) posture; the resolver's
    // answer for a non-seam epoch must still be zwd_carry_q there.
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
    // 0 would inflate nothing while every ARMED log line and counter reads live
    // — an inert mechanism that looks wired (standing invariant #4).
    GnssConfig cfg;
    EXPECT_FALSE(loadInto("zero",
        networkConfig(std::string(kArmedSeam)
                      + "    \"zwd_seam_reseed_q\": 0.0,\n"), cfg));
}

TEST(ZwdSeamReseedQ, ZeroIsRejectedEvenWithoutTheSeamArmed) {
    // The check is at the PARSE site, not inside the `if (seam_reseed)` block:
    // a config that pins a q but forgets the response must not load silently
    // and then read as "0 is fine".
    GnssConfig cfg;
    EXPECT_FALSE(loadInto("zero_unarmed",
        networkConfig("    \"zwd_seam_reseed_q\": 0,\n"), cfg));
}

// ===========================================================================
// 4. NOT in the warm-resume fingerprint (with zwd_carry_q as positive control)
// ===========================================================================

TEST(ZwdSeamReseedQ, NotInTheWarmResumeFingerprint) {
    const uint64_t fp_unset = fingerprintOf("fp_unset", kArmedSeam);
    const uint64_t fp_q4    = fingerprintOf("fp_q4",
        std::string(kArmedSeam) + "    \"zwd_seam_reseed_q\": 0.00038,\n");
    const uint64_t fp_q2    = fingerprintOf("fp_q2",
        std::string(kArmedSeam) + "    \"zwd_seam_reseed_q\": 0.017,\n");
    ASSERT_NE(fp_unset, 0u);
    // The whole point of the card: one snapshot, three legs.
    EXPECT_EQ(fp_unset, fp_q4);
    EXPECT_EQ(fp_unset, fp_q2);
}

TEST(ZwdSeamReseedQ, CarryQIsStillFingerprinted) {
    // Positive control for the case above — a fingerprint that had stopped
    // hashing the ZWD terms entirely would pass it as a false null.
    const uint64_t fp_base = fingerprintOf("fp_base",
        std::string(kArmedSeam) + "    \"zwd_carry_q\": 1e-9,\n");
    const uint64_t fp_moved = fingerprintOf("fp_moved",
        std::string(kArmedSeam) + "    \"zwd_carry_q\": 3.8e-4,\n");
    EXPECT_NE(fp_base, fp_moved);
}

TEST(ZwdSeamReseedQ, SeamResponseKnobsStayExemptTogether) {
    // The exemption is a FAMILY: the response, its horizon and now its q.  If a
    // later edit fingerprints any of them the bracket stops running off one
    // snapshot, which is the failure §21.21 documents.
    const uint64_t fp_off = fingerprintOf("fp_off",
        "    \"phase_leveling\": true,\n"
        "    \"state_snapshot_path\": \"state_snapshot.snap\",\n");
    const uint64_t fp_on = fingerprintOf("fp_on",
        std::string(kArmedSeam) + "    \"zwd_seam_reseed_epochs\": 720,\n"
                                  "    \"zwd_seam_reseed_q\": 0.017,\n");
    EXPECT_EQ(fp_off, fp_on);
}
