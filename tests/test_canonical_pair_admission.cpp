/**
 * @file test_canonical_pair_admission.cpp
 * @brief Card CPA: the canonical-pair admission STATE MACHINE and its two knobs.
 *
 * (docs/refound/CPA_canonical_pair_admission_2026-09-20.md §1 and §2.4.)  The rule
 * the producer runs per (station, satellite) is a pure function of three things:
 * whether this epoch holds the station's canonical phase and code on band digits 1
 * and 2, the processed-epoch index, and the tolerance.  Everything else in the seam
 * (reading the latch, erasing the record, the events, the counters) is plumbing
 * around that function, so the function is what this file exercises:
 *
 *  * `canonicalPairStep`: never good; good from the first epoch; a dropout shorter
 *    than, equal to and longer than the tolerance; the return after a withhold; and
 *    a tolerance of zero.  Both EDGES (standing invariant 7) are checked where they
 *    are crossed and where they are NOT: an epoch admitted by the tolerance opens no
 *    withhold edge, and a withhold that continues raises no second one.
 *  * The loader: the two knobs' defaults, an armed value, and the negative-value
 *    clamp.
 *
 * Structural only (the suite does not validate estimation accuracy); the rule's
 * effect on the network is graded by the arms the card records.
 *
 * Suites: CanonicalPairStateMachine, CanonicalPairConfig (one file owns both; no
 * other file declares either name).
 */
#include <gtest/gtest.h>

#include "config/GnssConfig.hpp"
#include "filter/NetworkCanonicalPairAdmission.hpp"
#include "logging/LogManager.hpp"

#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

using namespace GnssProcessor;

namespace {

namespace fs = std::filesystem;

/// @brief True for the two verdicts that remove the record from the epoch.
bool withheld(const CanonicalPairStep& s) {
    return canonicalPairWithholds(s.action);
}

/**
 * @brief Drive one (station, satellite) through a scripted good/bad sequence.
 *
 * @param pattern one character per PROCESSED epoch: 'G' = the canonical pair is
 *                held, '.' = it is not.  The epoch index is the position, which is
 *                the seam's own clock (one increment per epoch that reaches the
 *                epoch body, whether or not this satellite was present).
 * @param keep    the tolerance, in processed epochs.
 * @return one step per character, in order.
 */
std::vector<CanonicalPairStep> run(const std::string& pattern, int keep) {
    CanonicalPairTrack tr;
    std::vector<CanonicalPairStep> out;
    out.reserve(pattern.size());
    for (std::size_t e = 0; e < pattern.size(); ++e)
        out.push_back(canonicalPairStep(tr, pattern[e] == 'G',
                                        static_cast<long long>(e), keep));
    return out;
}

/// @brief The admitted/withheld shape of a run, as a string in the same alphabet
///        (`A` admitted, `W` withheld) so a whole scenario reads on one line.
std::string shape(const std::vector<CanonicalPairStep>& steps) {
    std::string s;
    for (const auto& st : steps) s += withheld(st) ? 'W' : 'A';
    return s;
}

fs::path writeTempConfig(const std::string& tag, const std::string& body) {
    const fs::path p = fs::temp_directory_path() / ("cpa_cfg_" + tag + ".json");
    std::ofstream f(p);
    f << body;
    f.close();
    return p;
}

/// Minimum loadable PPP-RTK network config; @p extra is spliced into the network
/// section verbatim (must end with a comma when non-empty).
std::string networkConfig(const std::string& extra) {
    return std::string(
        "{\n"
        "  \"processing\": {\n"
        "    \"mode\": \"ppp-rtk-network\",\n"
        "    \"active_systems\": [ \"GPS\" ],\n"
        "    \"signal_priorities\": { \"GPS\": \"WC\" }\n"
        "  },\n"
        "  \"network\": {\n"
        "    \"clock_datum_station\": \"STA1\",\n")
        + extra +
        "    \"stations\": [\n"
        "      { \"name\": \"STA1\", \"position_ecef\": [ -2493304.99, -4655214.79, 3565497.67 ] },\n"
        "      { \"name\": \"STA2\", \"position_ecef\": [ -2281621.75, -1453585.08, 5756964.89 ] }\n"
        "    ]\n"
        "  }\n"
        "}\n";
}

bool loadWith(const std::string& extra, GnssConfig& cfg, LogManager& log,
              const std::string& tag) {
    const fs::path p = writeTempConfig(tag, networkConfig(extra));
    const bool ok = cfg.loadFromJson(p, log);
    std::error_code ec; fs::remove(p, ec);
    return ok;
}

} // namespace

// ---------------------------------------------------------------------------
// The state machine: canonicalPairStep
// ---------------------------------------------------------------------------

TEST(CanonicalPairStateMachine, NeverGoodIsWithheldForever) {
    // A receiver that tracks the satellite on its civil signals and NEVER holds the
    // semi-codeless pair: every epoch is `before_first`, whatever the tolerance.
    // A tolerance licenses carrying a good epoch forward, and there is none here.
    const auto s = run("........", 3);
    EXPECT_EQ(shape(s), "WWWWWWWW");
    for (const auto& st : s)
        EXPECT_EQ(st.action, CanonicalPairAction::WithholdBeforeFirst);
    EXPECT_STREQ(canonicalPairReason(s.front().action), "before_first");
    // The DECOUPLING edge is raised ONCE, on the first withheld epoch, not once per
    // withheld epoch (the card's event contract).
    EXPECT_TRUE(s.front().withhold_edge);
    for (std::size_t i = 1; i < s.size(); ++i) EXPECT_FALSE(s[i].withhold_edge);
    // Nothing was ever admitted, so no re-coupling edge exists to report.
    for (const auto& st : s) EXPECT_FALSE(st.readmit_edge);
}

TEST(CanonicalPairStateMachine, GoodFromTheFirstEpochIsNeverWithheld) {
    const auto s = run("GGGGGG", 3);
    EXPECT_EQ(shape(s), "AAAAAA");
    for (const auto& st : s) {
        EXPECT_EQ(st.action, CanonicalPairAction::AdmitGood);
        EXPECT_FALSE(st.withhold_edge);
        EXPECT_FALSE(st.readmit_edge);   // nothing was withheld, so nothing returns
    }
    EXPECT_STREQ(canonicalPairReason(s.front().action), "");
}

TEST(CanonicalPairStateMachine, DropoutShorterThanTheToleranceIsAdmitted) {
    // Two missed epochs under a tolerance of 3: both are carried on the last good
    // epoch and neither leaves the solve, so no edge is crossed at all.
    const auto s = run("G..G", 3);
    EXPECT_EQ(shape(s), "AAAA");
    EXPECT_EQ(s[1].action, CanonicalPairAction::AdmitWithinKeep);
    EXPECT_EQ(s[2].action, CanonicalPairAction::AdmitWithinKeep);
    EXPECT_EQ(s[1].gap_ep, 1);
    EXPECT_EQ(s[2].gap_ep, 2);
    for (const auto& st : s) {
        EXPECT_FALSE(st.withhold_edge);
        EXPECT_FALSE(st.readmit_edge);
    }
}

TEST(CanonicalPairStateMachine, DropoutExactlyTheToleranceIsStillAdmitted) {
    // The boundary the emulation drew (`e - last > keep` kills): the third missed
    // epoch is the last one carried, the fourth would not be.
    const auto s = run("G...G", 3);
    EXPECT_EQ(shape(s), "AAAAA");
    EXPECT_EQ(s[3].action, CanonicalPairAction::AdmitWithinKeep);
    EXPECT_EQ(s[3].gap_ep, 3);
}

TEST(CanonicalPairStateMachine, DropoutLongerThanTheToleranceIsWithheld) {
    // The fourth missed epoch is one past the tolerance, so the record leaves the
    // solve there and stays out until the pair comes back.
    const auto s = run("G.....", 3);
    EXPECT_EQ(shape(s), "AAAAWW");
    EXPECT_EQ(s[3].action, CanonicalPairAction::AdmitWithinKeep);
    EXPECT_EQ(s[4].action, CanonicalPairAction::WithholdBeyondKeep);
    EXPECT_STREQ(canonicalPairReason(s[4].action), "beyond_keep");
    EXPECT_EQ(s[4].gap_ep, 4);            // processed epochs since the last good one
    EXPECT_TRUE(s[4].withhold_edge);      // once, on the transition
    EXPECT_FALSE(s[5].withhold_edge);
}

TEST(CanonicalPairStateMachine, ReturnAfterAWithholdRaisesTheReCouplingEdge) {
    // The edge invariant 7 asks for on the way back in, with the length of what was
    // withheld: two epochs here, not the six since the last good one.
    const auto s = run("G.....GG", 3);
    EXPECT_EQ(shape(s), "AAAAWWAA");
    ASSERT_EQ(s[6].action, CanonicalPairAction::AdmitGood);
    EXPECT_TRUE(s[6].readmit_edge);
    EXPECT_EQ(s[6].gap_ep, 2);            // the run of withheld epochs that just ended
    EXPECT_FALSE(s[7].readmit_edge);      // the epoch after a return is ordinary
    // The clock restarts from the RETURN: a second dropout is measured against it.
    const auto t = run("G.....GG.....", 3);
    EXPECT_EQ(shape(t), "AAAAWWAAAAAWW");
}

TEST(CanonicalPairStateMachine, ReturnBeforeTheFirstGoodEpochAlsoReCouples) {
    // The `before_first` stretch is the population GRS §6 says dominates ("long
    // stretches in which a receiver tracks a satellite without ever holding the
    // semi-codeless pair"), so its re-coupling edge must be reported too.
    const auto s = run("...G", 3);
    EXPECT_EQ(shape(s), "WWWA");
    EXPECT_TRUE(s[0].withhold_edge);
    ASSERT_EQ(s[3].action, CanonicalPairAction::AdmitGood);
    EXPECT_TRUE(s[3].readmit_edge);
    EXPECT_EQ(s[3].gap_ep, 3);
}

TEST(CanonicalPairStateMachine, ToleranceZeroAdmitsOnlyTheGoodEpochs) {
    // N = 0 is the card's other measured arm: every partial epoch is withheld, so
    // the shape is the pattern itself.  Note it is NOT the same rule as "never
    // good": the returns still readmit.
    const auto s = run("G.G..G", 0);
    EXPECT_EQ(shape(s), "AWAWWA");
    EXPECT_EQ(s[1].action, CanonicalPairAction::WithholdBeyondKeep);
    EXPECT_EQ(s[1].gap_ep, 1);
    EXPECT_TRUE(s[1].withhold_edge);
    EXPECT_TRUE(s[2].readmit_edge);
    EXPECT_EQ(s[2].gap_ep, 1);
    EXPECT_TRUE(s[3].withhold_edge);
    EXPECT_FALSE(s[4].withhold_edge);
    EXPECT_EQ(s[5].gap_ep, 2);
}

TEST(CanonicalPairStateMachine, AbsentEpochsCountTowardTheGap) {
    // The epoch index is the RUN's clock, not this satellite's: a satellite absent
    // from the epoch entirely contributes no step, and the gap still grows.  Driven
    // here by stepping the index past the epochs the satellite did not appear in.
    CanonicalPairTrack tr;
    EXPECT_EQ(canonicalPairStep(tr, true, 10, 3).action, CanonicalPairAction::AdmitGood);
    // Epochs 11..14 hold no record for this satellite at all; the next one it does
    // appear in is 15, four epochs past the tolerance.
    const auto s = canonicalPairStep(tr, false, 15, 3);
    EXPECT_EQ(s.action, CanonicalPairAction::WithholdBeyondKeep);
    EXPECT_EQ(s.gap_ep, 5);
    EXPECT_TRUE(s.withhold_edge);
}

// ---------------------------------------------------------------------------
// The knobs: network.canonical_pair_admission / _keep_epochs
// ---------------------------------------------------------------------------

TEST(CanonicalPairConfig, DefaultsAreOffWithAToleranceOfThree) {
    LogManager log; GnssConfig cfg;
    ASSERT_TRUE(loadWith("", cfg, log, "default"));
    EXPECT_FALSE(cfg.networkCanonicalPairAdmission());
    EXPECT_EQ(cfg.networkCanonicalPairKeepEpochs(), 3);
}

TEST(CanonicalPairConfig, ArmedValuesAreRead) {
    LogManager log; GnssConfig cfg;
    ASSERT_TRUE(loadWith("    \"canonical_pair_admission\": true,\n"
                         "    \"canonical_pair_keep_epochs\": 0,\n",
                         cfg, log, "armed"));
    EXPECT_TRUE(cfg.networkCanonicalPairAdmission());
    EXPECT_EQ(cfg.networkCanonicalPairKeepEpochs(), 0);
}

TEST(CanonicalPairConfig, NegativeToleranceIsClampedNotRefused) {
    // Clamped rather than refused: 0 is a meaningful arm and a negative value has
    // no other reading, so the run proceeds on the nearest legal posture.
    LogManager log; GnssConfig cfg;
    ASSERT_TRUE(loadWith("    \"canonical_pair_admission\": true,\n"
                         "    \"canonical_pair_keep_epochs\": -4,\n",
                         cfg, log, "clamp"));
    EXPECT_TRUE(cfg.networkCanonicalPairAdmission());
    EXPECT_EQ(cfg.networkCanonicalPairKeepEpochs(), 0);
}
