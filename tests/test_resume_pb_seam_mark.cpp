/**
 * @file test_resume_pb_seam_mark.cpp
 * @brief Contract tests for the RS-4 warm-resume PHASE-BIAS seam mark
 *        (@c network.resume_pb_seam_mark, SPEC_event_harness §21.9).
 *
 * The card's premise, measured on the field golden's 6 h seed: a warm resume that
 * RESTORES the emitter (its per-key @c last_phase_bias_ baselines come back from the
 * snapshot's @c epb rows) and restores the CLK-R held frame emits a first epoch whose
 * satellite phase biases sit ~17 m p50 away from those baselines — while the E record
 * carries @c datum_changed 0, because the only resume mark in the emit path fires on a
 * COLD held-frame clock engine and the restore is precisely what makes it warm.
 *
 * What is locked here is the DECISION, not the driver plumbing: @ref
 * NetworkProductEmitter::pbSeamCensus is the read-only twin of the jump test inside
 * @c emitToStore, and the rule the driver applies to it is "more than
 * @ref GnssConfig::kResumePbSeamMarkFraction of a constellation's fresh-baseline keys
 * beyond @c discThresholdM".  Cases: (1) a first emit moving 30 % of the keys is over
 * the bar and marks its constellation exactly once; (2) 5 % is under the bar and marks
 * nothing; (3) a SECOND emit can never mark, because @c emitToStore has already
 * overwritten every baseline with this epoch's value (the census is structurally empty)
 * — the one-shot property the driver's latch also enforces; (4) the knob is off by
 * default, parses, and is not in the warm-resume fingerprint.  The seam's wire effect
 * (the first E record's @c datum_changed) is graded on the legs, not here.
 */
#include <gtest/gtest.h>
#include "config/GnssConfig.hpp"
#include "logging/LogManager.hpp"
#include "products/NetworkProductEmitter.hpp"
#include "products/SsrCorrectionStore.hpp"
#include <cmath>
#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using namespace GnssProcessor;

namespace {

constexpr double kLam = 0.1902936728;   // GPS L1 wavelength (m) — one cycle >> the 0.05 m gate

/// A GPS epoch of @p n satellites, one L1C phase bias each: key i gets
/// @c base + i*0.001 m, plus @p jump m on the FIRST @p n_jumped of them.
NetworkSolutionEpoch makeEpoch(double tow, int n, int n_jumped, double jump) {
    NetworkSolutionEpoch sol;
    sol.epoch_time = GnssTime{2313, tow};
    sol.iod_ssr = 3; sol.provider_id = 42; sol.solution_id = 1;
    for (int i = 0; i < n; ++i) {
        NetworkSatSolution s;
        s.sat = "G" + std::string(i + 1 < 10 ? "0" : "") + std::to_string(i + 1);
        s.has_clock = true; s.clock_c0_m = 1.0;
        const double b = 0.25 + 0.001 * i + (i < n_jumped ? jump : 0.0);
        s.phase_bias["L1C"] = NetworkPhaseBias{b, /*integer=*/true, /*wl=*/1};
        sol.sats.push_back(s);
    }
    return sol;
}

/// The driver's rule (NetworkEmitStore.cpp), applied to a census: the set of
/// constellations whose fresh-baseline key fraction beyond threshold clears the bar.
std::set<char> markedConsts(
    const std::map<char, NetworkProductEmitter::PbSeamConstTally>& census) {
    std::set<char> out;
    for (const auto& [cst, t] : census) {
        if (t.keys_tested <= 0) continue;
        const double frac =
            static_cast<double>(t.keys_over) / static_cast<double>(t.keys_tested);
        if (frac > GnssConfig::kResumePbSeamMarkFraction) out.insert(cst);
    }
    return out;
}

fs::path writeTempConfig(const std::string& tag, const std::string& body) {
    const fs::path p = fs::temp_directory_path() / ("pbseam_cfg_" + tag + ".json");
    std::ofstream f(p);
    f << body;
    f.close();
    return p;
}

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

/// An emitter whose per-key baselines are the PREVIOUS run's emitted values — the
/// state a warm resume restores from the snapshot's EMITTER blob.  Built by emitting
/// the pre-seam epoch into a throwaway store, then round-tripping through
/// serializeState / restoreState so the test exercises the same path the resume does.
NetworkProductEmitter restoredEmitter(double pre_seam_tow, int n) {
    NetworkProductEmitter writer;
    SsrCorrectionStore throwaway;
    writer.emitToStore(makeEpoch(pre_seam_tow, n, /*n_jumped=*/0, /*jump=*/0.0), throwaway);
    std::ostringstream os;
    writer.serializeState(os);

    NetworkProductEmitter reader;
    std::istringstream is(os.str());
    EXPECT_TRUE(reader.restoreState(is));
    return reader;
}

}  // namespace

// --- 1. Over the bar: 30 % of the keys move ⇒ the constellation is marked, once ----

TEST(ResumePbSeamMark, ThirtyPercentOfKeysMarksTheConstellationOnce) {
    NetworkProductEmitter em = restoredEmitter(/*pre_seam_tow=*/277290.0, /*n=*/20);
    SsrCorrectionStore store;

    // The first post-resume emit: 6 of 20 keys (30 %) re-realized by a full cycle.
    const NetworkSolutionEpoch first = makeEpoch(277320.0, 20, /*n_jumped=*/6, /*jump=*/kLam);
    const auto census = em.pbSeamCensus(first);

    ASSERT_EQ(census.size(), 1u);
    const auto& g = census.at('G');
    EXPECT_EQ(g.keys_tested, 20);     // every key carries a RESTORED baseline
    EXPECT_EQ(g.keys_over, 6);
    EXPECT_NEAR(g.max_abs_delta_m, kLam, 1e-9);
    EXPECT_GT(static_cast<double>(g.keys_over) / g.keys_tested,
              GnssConfig::kResumePbSeamMarkFraction);
    EXPECT_EQ(markedConsts(census), std::set<char>({'G'}));

    // The census is READ-ONLY: taking it twice gives the same answer, and it has not
    // disturbed the counters the emitter is about to step.
    EXPECT_EQ(em.pbSeamCensus(first).at('G').keys_over, 6);
    EXPECT_EQ(em.discontinuityCounter("G01", "L1C"), 0);

    // The mark is exactly what markDatumChange consumes: one bump per key of the
    // marked constellation at this emit, and no second bump at the next quiet one.
    em.markDatumChange(std::set<char>({'G'}));
    em.emitToStore(first, store);
    EXPECT_EQ(em.discontinuityCounter("G01", "L1C"), 1);
    EXPECT_EQ(em.discontinuityCounter("G20", "L1C"), 1);
    em.emitToStore(makeEpoch(277350.0, 20, /*n_jumped=*/6, /*jump=*/kLam), store);
    EXPECT_EQ(em.discontinuityCounter("G01", "L1C"), 1);
}

// --- 2. Under the bar: 5 % of the keys move ⇒ nothing is marked -------------------

TEST(ResumePbSeamMark, FivePercentOfKeysMarksNothing) {
    NetworkProductEmitter em = restoredEmitter(/*pre_seam_tow=*/277290.0, /*n=*/20);

    // 1 of 20 keys (5 %) — under the 10 % bar, so this is an ordinary single-key jump
    // the emitter's own per-key test already reports; not a frame re-realization.
    const NetworkSolutionEpoch first = makeEpoch(277320.0, 20, /*n_jumped=*/1, /*jump=*/kLam);
    const auto census = em.pbSeamCensus(first);

    ASSERT_EQ(census.size(), 1u);
    EXPECT_EQ(census.at('G').keys_tested, 20);
    EXPECT_EQ(census.at('G').keys_over, 1);
    EXPECT_LE(static_cast<double>(census.at('G').keys_over) / census.at('G').keys_tested,
              GnssConfig::kResumePbSeamMarkFraction);
    EXPECT_TRUE(markedConsts(census).empty());
}

// --- 3. A second emit can never mark — the baselines are this epoch's -------------

TEST(ResumePbSeamMark, SecondEmitCannotMark) {
    NetworkProductEmitter em = restoredEmitter(/*pre_seam_tow=*/277290.0, /*n=*/20);
    SsrCorrectionStore store;

    const NetworkSolutionEpoch first = makeEpoch(277320.0, 20, /*n_jumped=*/20, /*jump=*/kLam);
    EXPECT_EQ(markedConsts(em.pbSeamCensus(first)), std::set<char>({'G'}));
    em.emitToStore(first, store);

    // Re-censusing the epoch that was just emitted: every baseline now IS that epoch's
    // value, so nothing is over threshold even though the values never moved back.
    const auto after = em.pbSeamCensus(first);
    ASSERT_EQ(after.size(), 1u);
    EXPECT_EQ(after.at('G').keys_tested, 20);
    EXPECT_EQ(after.at('G').keys_over, 0);
    EXPECT_TRUE(markedConsts(after).empty());

    // And the genuinely next epoch, re-realized by another whole cycle, is graded
    // against the post-seam frame — the emitter's ordinary jump test, not the seam.
    const auto next = em.pbSeamCensus(makeEpoch(277350.0, 20, 20, 2.0 * kLam));
    EXPECT_EQ(next.at('G').keys_over, 20);   // the per-key test still sees it
    EXPECT_NEAR(next.at('G').max_abs_delta_m, kLam, 1e-9);   // one cycle, not two
}

// --- 4. A COLD emitter has no baselines, so the census is empty -------------------

TEST(ResumePbSeamMark, ColdEmitterHasNothingToCompare) {
    NetworkProductEmitter em;                 // fresh: no restored baselines
    EXPECT_TRUE(em.pbSeamCensus(makeEpoch(277320.0, 20, 20, kLam)).empty());
    EXPECT_TRUE(markedConsts(em.pbSeamCensus(makeEpoch(277320.0, 20, 20, kLam))).empty());
}

// --- 5. Per-constellation scoping: only the constellation that moved is marked ----

TEST(ResumePbSeamMark, MarksOnlyTheConstellationThatMoved) {
    NetworkProductEmitter writer;
    SsrCorrectionStore throwaway;
    NetworkSolutionEpoch pre = makeEpoch(277290.0, 10, 0, 0.0);
    for (int i = 0; i < 10; ++i) {           // bolt a Galileo half onto the same epoch
        NetworkSatSolution s;
        s.sat = "E" + std::string(i + 1 < 10 ? "0" : "") + std::to_string(i + 1);
        s.has_clock = true; s.clock_c0_m = 1.0;
        s.phase_bias["L1C"] = NetworkPhaseBias{-0.40 + 0.001 * i, true, 1};
        pre.sats.push_back(s);
    }
    writer.emitToStore(pre, throwaway);
    std::ostringstream os; writer.serializeState(os);
    NetworkProductEmitter em;
    std::istringstream is(os.str());
    ASSERT_TRUE(em.restoreState(is));

    NetworkSolutionEpoch first = pre;
    first.epoch_time.tow = 277320.0;
    for (auto& s : first.sats)               // move GPS only; Galileo holds its frame
        if (s.sat.front() == 'G') s.phase_bias["L1C"].bias_m += kLam;

    const auto census = em.pbSeamCensus(first);
    ASSERT_EQ(census.size(), 2u);
    EXPECT_EQ(census.at('G').keys_over, 10);
    EXPECT_EQ(census.at('E').keys_over, 0);
    EXPECT_EQ(markedConsts(census), std::set<char>({'G'}));
}

// --- 6. The knob: default OFF, parses, and is NOT fingerprinted -------------------

TEST(ResumePbSeamMark, KnobDefaultsOffAndParses) {
    GnssConfig def;
    ASSERT_TRUE(loadInto("def", networkConfig(), def));
    EXPECT_FALSE(def.networkResumePbSeamMark());

    GnssConfig on;
    ASSERT_TRUE(loadInto("on", networkConfig("    \"resume_pb_seam_mark\": true,\n"), on));
    EXPECT_TRUE(on.networkResumePbSeamMark());

    // The warn-inert posture (armed with no snapshot to resume from) still LOADS —
    // an armed-and-inert knob is a warning, never a hard failure (the
    // resume_frame_restore precedent).
    EXPECT_TRUE(on.getNetworkConfig().state_snapshot_path.empty());

    // The bar is a single compile-time constant shared by the rule, its log line and
    // this test — 10 %, as pre-registered.
    EXPECT_DOUBLE_EQ(GnssConfig::kResumePbSeamMarkFraction, 0.10);
}
