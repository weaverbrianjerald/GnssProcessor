/**
 * @file test_clock_emit_rebase.cpp
 * @brief The clock emission re-base leaf (NetworkClockEmitRebase) and its loader
 *        contract (card CLK-R, docs/SPEC_clock_emit_rebase.md).
 *
 * What is pinned here:
 *   1. Stage 1 removes the per-epoch common-mode step exactly (the census's
 *      common-mode class) while preserving between-satellite differences to
 *      float rounding: the frame hold is the clock-ensemble gauge and nothing
 *      else.  New sightings join the held frame (no step against their peers).
 *   2. Stage 2 marks a genuine per-satellite step (common-removed statistic,
 *      aligned with the census) and passes the value through unsmoothed
 *      (invariant #1); it also works with the frame hold off (raw-wire marks,
 *      the card's "or" branch).
 *   3. The guards: forced keys neither vote nor double-mark; the μ vote is a
 *      median (minority outliers cannot drag the frame); the min_common floor
 *      freezes the frame instead of trusting a thin vote; the leash re-anchors
 *      and reports the constellation (so the caller can mark the datum change);
 *      a return beyond the horizon is a first sighting (no step test against a
 *      stale baseline) unless gap_jump_m is armed (stage 2b).
 *   4. Loader contract: both knobs default off and parse independently.
 */

#include <gtest/gtest.h>

#include "config/GnssConfig.hpp"
#include "logging/LogManager.hpp"
#include "network/emit/ClockEmitRebase.hpp"

#include <cmath>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using namespace GnssProcessor;

namespace {

/// Run one epoch through the engine and return the (mutated) entries.
std::vector<NetworkClockEmitRebase::Entry>
epoch(NetworkClockEmitRebase& eng, double t,
      const std::map<std::string, double>& raw,
      std::set<char>* reanchored = nullptr,
      const std::set<std::string>& forced = {}) {
    std::vector<NetworkClockEmitRebase::Entry> es;
    es.reserve(raw.size());
    for (const auto& [sat, v] : raw) {
        NetworkClockEmitRebase::Entry e;
        e.sat = sat;
        e.c0_m = v;
        e.forced = forced.count(sat) != 0;
        es.push_back(std::move(e));
    }
    std::set<char> ra;
    eng.rebaseEpoch(es, t, reanchored ? *reanchored : ra);
    return es;
}

double valueOf(const std::vector<NetworkClockEmitRebase::Entry>& es,
               const std::string& sat) {
    for (const auto& e : es)
        if (e.sat == sat) return e.c0_m;
    ADD_FAILURE() << "satellite " << sat << " not in epoch";
    return 0.0;
}

bool markedOf(const std::vector<NetworkClockEmitRebase::Entry>& es,
              const std::string& sat) {
    for (const auto& e : es)
        if (e.sat == sat) return e.marked;
    ADD_FAILURE() << "satellite " << sat << " not in epoch";
    return false;
}

NetworkClockEmitRebase::Config holdCfg() {
    NetworkClockEmitRebase::Config c;
    c.frame_hold = true;
    return c;
}

const std::map<std::string, double> kBase = {
    {"G01", 1.0}, {"G02", -2.0}, {"G03", 0.5},
    {"G04", 3.0}, {"G05", -1.5}, {"G06", 2.5}};

std::map<std::string, double> shifted(const std::map<std::string, double>& base,
                                      double common,
                                      const std::map<std::string, double>& extra = {}) {
    std::map<std::string, double> out;
    for (const auto& [s, v] : base) {
        const auto it = extra.find(s);
        out[s] = v + common + (it != extra.end() ? it->second : 0.0);
    }
    return out;
}

// ===========================================================================
// stage 1: the frame hold
// ===========================================================================

TEST(ClockEmitRebase, FrameHoldRemovesCommonModeExactly) {
    NetworkClockEmitRebase eng(holdCfg());
    auto e0 = epoch(eng, 0.0, kBase);
    // Two epochs with pure common-mode steps (the census's common-mode class,
    // including a 17.7 m-scale spike): the emitted series stays continuous.
    auto e1 = epoch(eng, 30.0, shifted(kBase, 0.8));
    auto e2 = epoch(eng, 60.0, shifted(kBase, 0.8 + 17.7));
    for (const auto& [sat, v] : kBase) {
        EXPECT_NEAR(valueOf(e0, sat), v, 1e-12);
        EXPECT_NEAR(valueOf(e1, sat), v, 1e-9) << sat;
        EXPECT_NEAR(valueOf(e2, sat), v, 1e-9) << sat;
    }
    // The accumulator carries exactly the drained common mode.
    EXPECT_NEAR(eng.frameAccForTest('G'), 0.8 + 17.7, 1e-9);
    EXPECT_NEAR(eng.counters().sum_abs_mu, 0.8 + 17.7, 1e-9);
    EXPECT_EQ(eng.counters().reanchors, 0);
}

TEST(ClockEmitRebase, BetweenSatDifferencesPreservedToRounding) {
    NetworkClockEmitRebase eng(holdCfg());
    epoch(eng, 0.0, kBase);
    const auto raw = shifted(kBase, -3.7, {{"G04", 0.02}, {"G05", -0.01}});
    auto e1 = epoch(eng, 30.0, raw);
    for (const auto& [si, vi] : raw)
        for (const auto& [sj, vj] : raw)
            EXPECT_NEAR(valueOf(e1, si) - valueOf(e1, sj), vi - vj, 1e-9)
                << si << "-" << sj;
}

TEST(ClockEmitRebase, NewSatelliteJoinsTheHeldFrame) {
    NetworkClockEmitRebase eng(holdCfg());
    epoch(eng, 0.0, kBase);
    epoch(eng, 30.0, shifted(kBase, 5.0));   // acc -> 5.0
    // G07 is first sighted while the frame is displaced: it joins the held
    // frame (its SD against peers = the raw SD) instead of re-importing the
    // solve frame.
    auto raw = shifted(kBase, 5.0);
    raw["G07"] = 4.0;
    auto e2 = epoch(eng, 60.0, raw);
    EXPECT_NEAR(valueOf(e2, "G07") - valueOf(e2, "G01"), 4.0 - raw["G01"], 1e-9);
    EXPECT_NEAR(valueOf(e2, "G07"), 4.0 - 5.0, 1e-9);   // held frame = raw - acc
    EXPECT_EQ(eng.counters().inits, 7);   // 6 first epoch + G07
}

TEST(ClockEmitRebase, MinCommonFloorFreezesTheFrame) {
    NetworkClockEmitRebase::Config c = holdCfg();
    c.min_common = 4;
    NetworkClockEmitRebase eng(c);
    const std::map<std::string, double> three = {
        {"G01", 1.0}, {"G02", 2.0}, {"G03", 3.0}};
    epoch(eng, 0.0, three);
    auto e1 = epoch(eng, 30.0, shifted(three, 1.0));
    // Only 3 fresh keys < min_common: the μ vote is void, the frame does not
    // move (a thin median is noise), and the step passes to the wire.
    EXPECT_NEAR(eng.frameAccForTest('G'), 0.0, 1e-12);
    EXPECT_NEAR(valueOf(e1, "G01"), 2.0, 1e-12);
}

TEST(ClockEmitRebase, LeashReanchorsAndReportsTheConstellation) {
    NetworkClockEmitRebase::Config c = holdCfg();
    c.leash_m = 0.5;
    c.jump_test = true;   // suppressed on the re-anchor epoch
    NetworkClockEmitRebase eng(c);
    epoch(eng, 0.0, kBase);
    std::set<char> ra1;
    epoch(eng, 30.0, shifted(kBase, 0.4), &ra1);
    EXPECT_TRUE(ra1.empty());
    EXPECT_NEAR(eng.frameAccForTest('G'), 0.4, 1e-12);
    // A second 0.4 m common step crosses the 0.5 m leash: re-anchor to the solve
    // frame (raw passthrough, accumulator cleared) and report the constellation
    // so the caller can markDatumChange.  No step marks are set on that epoch,
    // even for a satellite with a genuine event (the datum mark covers it).
    std::set<char> ra2;
    auto e2 = epoch(eng, 60.0, shifted(kBase, 0.8, {{"G06", 5.0}}), &ra2);
    EXPECT_EQ(ra2, std::set<char>{'G'});
    EXPECT_NEAR(eng.frameAccForTest('G'), 0.0, 1e-12);
    EXPECT_NEAR(valueOf(e2, "G01"), kBase.at("G01") + 0.8, 1e-12);  // raw
    EXPECT_FALSE(markedOf(e2, "G06"));
    EXPECT_EQ(eng.counters().reanchors, 1);
    EXPECT_EQ(eng.counters().marks, 0);
}

// ===========================================================================
// stage 2: the step marks
// ===========================================================================

TEST(ClockEmitRebase, GenuineStepPassesThroughAndMarks) {
    NetworkClockEmitRebase::Config c = holdCfg();
    c.jump_test = true;   // jump_m default 1.5
    NetworkClockEmitRebase eng(c);
    epoch(eng, 0.0, kBase);
    auto e1 = epoch(eng, 30.0, shifted(kBase, 0.0, {{"G03", 2.0}}));
    EXPECT_TRUE(markedOf(e1, "G03"));
    EXPECT_FALSE(markedOf(e1, "G01"));
    // Not smoothed: the marked value carries the full step (held frame).
    EXPECT_NEAR(valueOf(e1, "G03"), kBase.at("G03") + 2.0, 1e-9);
    for (const auto& e : e1)
        if (e.sat == "G03") EXPECT_NEAR(e.step_m, 2.0, 1e-9);
    EXPECT_EQ(eng.counters().marks, 1);
    // A 1 m step, below the 1.5 m threshold: not an event.
    auto e2 = epoch(eng, 60.0, shifted(kBase, 0.0, {{"G03", 2.0 + 1.0}}));
    EXPECT_FALSE(markedOf(e2, "G03"));
    EXPECT_EQ(eng.counters().marks, 1);
}

TEST(ClockEmitRebase, JumpTestAloneMarksTheRawWire) {
    NetworkClockEmitRebase::Config c;   // frame_hold off
    c.jump_test = true;
    NetworkClockEmitRebase eng(c);
    epoch(eng, 0.0, kBase);
    // A 5 m common step with the hold off: values pass through untouched
    // (mediation not armed) and the common step marks no satellite, because
    // the statistic is common-removed (the census's own).  The one satellite
    // with an extra 2 m is marked.
    auto e1 = epoch(eng, 30.0, shifted(kBase, 5.0, {{"G02", 2.0}}));
    EXPECT_NEAR(valueOf(e1, "G01"), kBase.at("G01") + 5.0, 1e-12);  // raw wire
    EXPECT_NEAR(eng.frameAccForTest('G'), 0.0, 1e-12);              // no hold
    EXPECT_FALSE(markedOf(e1, "G01"));
    EXPECT_TRUE(markedOf(e1, "G02"));
}

TEST(ClockEmitRebase, ForcedKeysNeitherVoteNorMark) {
    NetworkClockEmitRebase::Config c = holdCfg();
    c.jump_test = true;
    NetworkClockEmitRebase eng(c);
    epoch(eng, 0.0, kBase);
    // G04 is forced (a retro bump or datum change is already pending): its 10 m
    // step does not drag the μ vote and is not marked a second time, and its
    // value still rides the held frame (one shared subtrahend, so SD is preserved).
    auto e1 = epoch(eng, 30.0, shifted(kBase, 0.0, {{"G04", 10.0}}),
                    nullptr, {"G04"});
    EXPECT_FALSE(markedOf(e1, "G04"));
    EXPECT_NEAR(eng.frameAccForTest('G'), 0.0, 1e-12);
    EXPECT_NEAR(valueOf(e1, "G04"), kBase.at("G04") + 10.0, 1e-9);
    EXPECT_EQ(eng.counters().marks, 0);
}

TEST(ClockEmitRebase, MedianVoteIgnoresMinorityOutliers) {
    NetworkClockEmitRebase::Config c = holdCfg();
    c.jump_test = true;
    NetworkClockEmitRebase eng(c);
    epoch(eng, 0.0, kBase);
    // One 10 m outlier among six: the median μ stays 0 (the frame does not
    // move), and the outlier is marked as the per-sat event it is.
    auto e1 = epoch(eng, 30.0, shifted(kBase, 0.0, {{"G05", 10.0}}));
    EXPECT_NEAR(eng.frameAccForTest('G'), 0.0, 1e-12);
    EXPECT_TRUE(markedOf(e1, "G05"));
    EXPECT_FALSE(markedOf(e1, "G02"));
}

TEST(ClockEmitRebase, BeyondHorizonReturnIsAFirstSighting) {
    NetworkClockEmitRebase::Config c = holdCfg();
    c.jump_test = true;
    c.max_gap_s = 600.0;
    NetworkClockEmitRebase eng(c);
    epoch(eng, 0.0, kBase);
    // G06 vanishes; the others run on (30 s cadence) past the horizon.
    std::map<std::string, double> five = kBase;
    five.erase("G06");
    for (double t = 30.0; t <= 660.0; t += 30.0) epoch(eng, t, five);
    // Return with a 50 m offset: a first sighting (the stale baseline is
    // suppressed, the emitter's own RTA-11 rule), not a mark.
    auto raw = five;
    raw["G06"] = kBase.at("G06") + 50.0;
    auto e = epoch(eng, 690.0, raw);
    EXPECT_FALSE(markedOf(e, "G06"));
    EXPECT_EQ(eng.counters().gaps, 1);
    EXPECT_EQ(eng.counters().marks, 0);
}

TEST(ClockEmitRebase, GapAwareReturnMarksAgainstPreGapBaseline) {
    // Stage 2b: the same beyond-horizon return with gap_jump_m armed, so the
    // retained pre-gap baseline tests the return.  This targets the
    // eclipse-readmission re-realization (measured 6.3-6.7 m steps), which the
    // fresh-only test of stage 2 cannot see by construction (E1).
    NetworkClockEmitRebase::Config c = holdCfg();
    c.jump_test  = true;
    c.max_gap_s  = 600.0;
    c.gap_jump_m = 3.0;
    NetworkClockEmitRebase eng(c);
    epoch(eng, 0.0, kBase);
    std::map<std::string, double> five = kBase;
    five.erase("G06");
    for (double t = 30.0; t <= 660.0; t += 30.0) epoch(eng, t, five);
    auto raw = five;
    raw["G06"] = kBase.at("G06") + 50.0;      // far beyond the 3 m gap band
    auto e = epoch(eng, 690.0, raw);
    EXPECT_TRUE(markedOf(e, "G06"));
    EXPECT_EQ(eng.counters().gap_marks, 1);
    EXPECT_EQ(eng.counters().marks, 0);       // the fresh-path counter is untouched
    EXPECT_EQ(eng.counters().gaps, 1);        // still counted as a gap re-init
}

TEST(ClockEmitRebase, GapAwareReturnWithinBandStaysUnmarked) {
    // A quiet return inside the gap band: no mark and no false positive, so the
    // knob does not turn every readmission into a consumer reset.
    NetworkClockEmitRebase::Config c = holdCfg();
    c.jump_test  = true;
    c.max_gap_s  = 600.0;
    c.gap_jump_m = 3.0;
    NetworkClockEmitRebase eng(c);
    epoch(eng, 0.0, kBase);
    std::map<std::string, double> five = kBase;
    five.erase("G06");
    for (double t = 30.0; t <= 660.0; t += 30.0) epoch(eng, t, five);
    auto raw = five;
    raw["G06"] = kBase.at("G06") + 0.5;       // inside the 3 m band
    auto e = epoch(eng, 690.0, raw);
    EXPECT_FALSE(markedOf(e, "G06"));
    EXPECT_EQ(eng.counters().gap_marks, 0);
    EXPECT_EQ(eng.counters().gaps, 1);
}

// ===========================================================================
// loader contract
// ===========================================================================

fs::path writeTempConfig(const std::string& tag, const std::string& network_extra) {
    const std::string body = std::string("{\n") +
        "  \"processing\": {\n"
        "    \"mode\": \"ppp-rtk-network\",\n"
        "    \"active_systems\": [ \"GPS\" ],\n"
        "    \"signal_priorities\": { \"GPS\": \"WC\" }\n"
        "  },\n"
        "  \"estimation\": {\n"
        "    \"estimate_ifb\": true\n"
        "  },\n"
        "  \"network\": {\n"
        "    \"clock_datum_station\": \"AAAA\",\n"
        + network_extra +
        "    \"stations\": [\n"
        "      { \"name\": \"AAAA\", \"position_ecef\": [ -2493304.99, -4655214.79, 3565497.67 ] },\n"
        "      { \"name\": \"BBBB\", \"position_ecef\": [ -2281621.75, -1453585.08, 5756964.89 ] }\n"
        "    ]\n"
        "  }\n"
        "}\n";
    const fs::path p = fs::temp_directory_path() / ("clk_rebase_cfg_" + tag + ".json");
    std::ofstream f(p);
    f << body;
    return p;
}

TEST(ClockEmitRebaseLoader, DefaultsOff) {
    LogManager log;
    const fs::path p = writeTempConfig("default", "");
    GnssConfig cfg;
    ASSERT_TRUE(cfg.loadFromJson(p, log));
    EXPECT_FALSE(cfg.networkEmitClockFrameHold());
    EXPECT_FALSE(cfg.networkClockDiscJumpTest());
    EXPECT_DOUBLE_EQ(cfg.networkClockFrameLeashM(), 50.0);
    EXPECT_DOUBLE_EQ(cfg.networkClockDiscJumpM(), 1.5);
    std::error_code ec; fs::remove(p, ec);
}

TEST(ClockEmitRebaseLoader, KnobsParseIndependently) {
    LogManager log;
    {   // stage 1 alone
        const fs::path p = writeTempConfig("hold",
            "    \"emit_clock_frame_hold\": true,\n"
            "    \"clock_frame_leash_m\": 30.0,\n");
        GnssConfig cfg;
        ASSERT_TRUE(cfg.loadFromJson(p, log));
        EXPECT_TRUE(cfg.networkEmitClockFrameHold());
        EXPECT_FALSE(cfg.networkClockDiscJumpTest());
        EXPECT_DOUBLE_EQ(cfg.networkClockFrameLeashM(), 30.0);
        std::error_code ec; fs::remove(p, ec);
    }
    {   // stage 2 alone (the raw-wire "or" branch)
        const fs::path p = writeTempConfig("jump",
            "    \"clock_disc_jump_test\": true,\n"
            "    \"clock_disc_jump_m\": 2.0,\n");
        GnssConfig cfg;
        ASSERT_TRUE(cfg.loadFromJson(p, log));
        EXPECT_FALSE(cfg.networkEmitClockFrameHold());
        EXPECT_TRUE(cfg.networkClockDiscJumpTest());
        EXPECT_DOUBLE_EQ(cfg.networkClockDiscJumpM(), 2.0);
        std::error_code ec; fs::remove(p, ec);
    }
}

}  // namespace
