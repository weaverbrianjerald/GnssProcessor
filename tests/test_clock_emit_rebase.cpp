/**
 * @file test_clock_emit_rebase.cpp
 * @brief Card CLK-R (docs/SPEC_clock_emit_rebase.md) — the clock emission
 *        re-base leaf (NetworkClockEmitRebase) + its loader contract.
 *
 * What is pinned here:
 *   1. Stage 1 removes the per-epoch common-mode step EXACTLY (the 613/1440
 *      census class) while preserving between-satellite differences to
 *      float-rounding — the frame hold is the clock-ensemble gauge, nothing
 *      else.  New sights join the held frame (no step vs their peers).
 *   2. Stage 2 marks a genuine per-sat step (common-removed, census-aligned
 *      statistic) and passes the value through UNSMOOTHED — never laundered
 *      (invariant #1); it works with the frame hold OFF too (raw-wire marks,
 *      the card's "or" branch).
 *   3. The guards: forced keys neither vote nor double-mark; the μ vote is a
 *      median (minority outliers cannot drag the frame); the min_common floor
 *      freezes the frame instead of trusting a thin vote; the leash re-anchors
 *      WITH a reported constellation (the caller's datum-mark obligation); a
 *      beyond-horizon return is a first sighting (no stale-baseline step test).
 *   4. Loader contract: both knobs default OFF and parse independently.
 */

#include <gtest/gtest.h>

#include "config/GnssConfig.hpp"
#include "logging/LogManager.hpp"
#include "products/NetworkClockEmitRebase.hpp"

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
// stage 1 — the frame hold
// ===========================================================================

TEST(ClockEmitRebase, FrameHoldRemovesCommonModeExactly) {
    NetworkClockEmitRebase eng(holdCfg());
    auto e0 = epoch(eng, 0.0, kBase);
    // Two epochs with pure common-mode steps (the census's 613-epoch class,
    // incl. a 17.7 m-scale spike): the emitted series must be CONTINUOUS.
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
    // G07 first-sights while the frame is displaced: it must join the HELD
    // frame (its SD vs peers = the raw SD), not re-import the solve frame.
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
    // Only 3 fresh keys < min_common: the μ vote is void, the frame must NOT
    // move (a thin median is noise), the step passes to the wire.
    EXPECT_NEAR(eng.frameAccForTest('G'), 0.0, 1e-12);
    EXPECT_NEAR(valueOf(e1, "G01"), 2.0, 1e-12);
}

TEST(ClockEmitRebase, LeashReanchorsAndReportsTheConstellation) {
    NetworkClockEmitRebase::Config c = holdCfg();
    c.leash_m = 0.5;
    c.jump_test = true;   // must be suppressed on the re-anchor epoch
    NetworkClockEmitRebase eng(c);
    epoch(eng, 0.0, kBase);
    std::set<char> ra1;
    epoch(eng, 30.0, shifted(kBase, 0.4), &ra1);
    EXPECT_TRUE(ra1.empty());
    EXPECT_NEAR(eng.frameAccForTest('G'), 0.4, 1e-12);
    // Second 0.4 m common step crosses the 0.5 m leash: re-anchor to the solve
    // frame (raw passthrough, accumulator cleared), constellation reported so
    // the caller can markDatumChange — and NO step marks on that epoch, even
    // for a satellite with a genuine event (the datum mark covers everything).
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
// stage 2 — the step marks
// ===========================================================================

TEST(ClockEmitRebase, GenuineStepPassesThroughAndMarks) {
    NetworkClockEmitRebase::Config c = holdCfg();
    c.jump_test = true;   // jump_m default 1.5
    NetworkClockEmitRebase eng(c);
    epoch(eng, 0.0, kBase);
    auto e1 = epoch(eng, 30.0, shifted(kBase, 0.0, {{"G03", 2.0}}));
    EXPECT_TRUE(markedOf(e1, "G03"));
    EXPECT_FALSE(markedOf(e1, "G01"));
    // Never laundered: the marked value carries the full step (held frame).
    EXPECT_NEAR(valueOf(e1, "G03"), kBase.at("G03") + 2.0, 1e-9);
    for (const auto& e : e1)
        if (e.sat == "G03") EXPECT_NEAR(e.step_m, 2.0, 1e-9);
    EXPECT_EQ(eng.counters().marks, 1);
    // Below threshold: not an event.
    auto e2 = epoch(eng, 60.0, shifted(kBase, 0.0, {{"G03", 2.0 + 1.0}}));
    EXPECT_FALSE(markedOf(e2, "G03"));
    EXPECT_EQ(eng.counters().marks, 1);
}

TEST(ClockEmitRebase, JumpTestAloneMarksTheRawWire) {
    NetworkClockEmitRebase::Config c;   // frame_hold OFF
    c.jump_test = true;
    NetworkClockEmitRebase eng(c);
    epoch(eng, 0.0, kBase);
    // A 5 m COMMON step with the hold off: values pass through untouched
    // (mediation not armed) and nobody is marked — the statistic is
    // common-removed (the census's own), so the common class never
    // storms the marks.  The one satellite with an extra 2 m IS marked.
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
    // G04 is forced (a retro bump / datum change is already pending): its 10 m
    // step must not drag the μ vote, must not double-mark, and its value still
    // rides the held frame (one shared subtrahend — SD preserved).
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
    // Return with a huge offset: a first sighting (stale baseline suppressed —
    // the emitter's own RTA-11 rule), never a mark.
    auto raw = five;
    raw["G06"] = kBase.at("G06") + 50.0;
    auto e = epoch(eng, 690.0, raw);
    EXPECT_FALSE(markedOf(e, "G06"));
    EXPECT_EQ(eng.counters().gaps, 1);
    EXPECT_EQ(eng.counters().marks, 0);
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
