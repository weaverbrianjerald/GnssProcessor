/**
 * @file test_network_inject.cpp
 * @brief Event harness: the fault injector's parse contract and epoch semantics.
 *
 * Two halves, both structural (the suite does not validate estimation accuracy):
 *
 *  * `NetworkInjectConfig`: six of the seven accepted kinds parse (oracle_pin is
 *    not covered here), and every omitted required field fails the load.  The
 *    failure matters because a typo that fell through to another kind's defaults
 *    would arm a different event than the operator wrote, and the leg would be
 *    graded as though it were the intended one.
 *  * `NetworkInjectEpoch`: a step is applied once per epoch from `tow` on to that
 *    epoch's raw observables and does not accumulate; an outage drops exactly its
 *    window; an armed event that never fires is reported and fails the run; with
 *    nothing armed the call is a no-op that does not copy the epoch.
 *
 * The epoch half drives `networkInjectEpoch` directly against a hand-built
 * `SyncedEpoch`, so it needs no fixtures and no solve.
 */
#include <gtest/gtest.h>

#include "app/NetworkRunContext.hpp"
#include "config/GnssConfig.hpp"
#include "filter/NetworkInject.hpp"
#include "logging/LogManager.hpp"
#include "logging/NetworkRunTraces.hpp"
#include "rinex/MultiRinexSync.hpp"

#include <nlohmann/json.hpp>

#include <filesystem>
#include <fstream>
#include <memory>
#include <string>

using namespace GnssProcessor;

namespace {

    namespace fs = std::filesystem;

    /// Write @p body to a unique temp file and return its path (the loader takes a path).
    fs::path writeTempConfig(const std::string& tag, const std::string& body) {
        const fs::path p = fs::temp_directory_path() / ("netinj_cfg_" + tag + ".json");
        std::ofstream f(p);
        f << body;
        f.close();
        return p;
    }

    /// Minimum loadable PPP-RTK network config; @p inject_block is spliced into
    /// the network section verbatim (empty => network.inject absent).
    std::string networkConfig(const std::string& inject_block) {
        return std::string(
            "{\n"
            "  \"processing\": {\n"
            "    \"mode\": \"ppp-rtk-network\",\n"
            "    \"active_systems\": [ \"GPS\" ],\n"
            "    \"signal_priorities\": { \"GPS\": \"WC\" }\n"
            "  },\n"
            "  \"network\": {\n"
            "    \"clock_datum_station\": \"STA1\",\n"
            "    \"trace_events\": true,\n")
            + inject_block +
            "    \"stations\": [\n"
            "      { \"name\": \"STA1\", \"position_ecef\": [ -2493304.99, -4655214.79, 3565497.67 ] },\n"
            "      { \"name\": \"STA2\", \"position_ecef\": [ -2281621.75, -1453585.08, 5756964.89 ] }\n"
            "    ]\n"
            "  }\n"
            "}\n";
    }

    /// @c "inject": { ... }, ready to splice.  @p j is the inject object.
    std::string injectBlock(const nlohmann::json& j) {
        if (j.is_null()) return "";
        return "    \"inject\": " + j.dump() + ",\n";
    }

    bool loadWith(const nlohmann::json& inject, GnssConfig& cfg, LogManager& log,
                  const std::string& tag) {
        const fs::path p = writeTempConfig(tag, networkConfig(injectBlock(inject)));
        const bool ok = cfg.loadFromJson(p, log);
        std::error_code ec; fs::remove(p, ec);
        return ok;
    }

    /// One RINEX observation with one phase + one code observable.
    RinexObservation obs(const std::string& sat, double l1_cyc, double c1_m) {
        RinexObservation o;
        o.satellite = sat;
        o.system = System::GPS;
        o.measurements["L1C"] = l1_cyc;
        o.measurements["C1C"] = c1_m;
        return o;
    }

    SyncedEpoch epochAt(double tow) {
        SyncedEpoch se;
        se.time.week = 2313;
        se.time.tow = tow;
        se.by_station["STA1"] = { obs("G05", 1.0e8, 2.0e7), obs("G07", 1.1e8, 2.1e7) };
        se.by_station["STA2"] = { obs("G05", 1.0e8, 2.0e7) };
        return se;
    }

    /// Config + context + a traces bundle with no open streams (emitEvent no-ops).
    struct Harness {
        LogManager lm;
        GnssConfig cfg;
        std::unique_ptr<NetworkRunContext> ctx;
        std::unique_ptr<NetworkRunTraces> traces;
        std::shared_ptr<spdlog::logger> log;

        Harness(const nlohmann::json& inject, const std::string& tag) {
            EXPECT_TRUE(loadWith(inject, cfg, lm, tag));
            ctx = std::make_unique<NetworkRunContext>(cfg, cfg.getNetworkConfig());
            traces = std::make_unique<NetworkRunTraces>(fs::temp_directory_path());
            log = lm.getLogger("main_log");
            networkInjectArm(*ctx, cfg, lm, log);
        }
    };

    nlohmann::json slipEvent(double tow, double d, bool lli = false) {
        return {{"events", nlohmann::json::array({
            {{"kind", "slip"}, {"tow", tow}, {"station", "STA1"}, {"sat", "G05"},
             {"signal", "L1C"}, {"delta_cycles", d}, {"flag_lli", lli}}})}};
    }

} // namespace

// ---------------------------------------------------------------- parse contract
TEST(NetworkInjectConfig, AbsentIsOffAndByteIdentical) {
    LogManager lm;
    GnssConfig cfg;
    ASSERT_TRUE(loadWith(nlohmann::json(), cfg, lm, "t1"));
    EXPECT_FALSE(cfg.getNetworkConfig().inject.enable);
    EXPECT_TRUE(cfg.getNetworkConfig().inject.events.empty());
}

TEST(NetworkInjectConfig, EmptyEventsArrayIsInertNotArmed) {
    LogManager lm;
    GnssConfig cfg;
    ASSERT_TRUE(loadWith({{"events", nlohmann::json::array()}}, cfg, lm, "t2"));
    EXPECT_FALSE(cfg.getNetworkConfig().inject.enable);
}

TEST(NetworkInjectConfig, EachKindParses) {
    LogManager lm;
    GnssConfig cfg;
    nlohmann::json ij = {{"events", nlohmann::json::array({
        {{"kind", "slip"}, {"tow", 100.0}, {"station", "STA1"}, {"sat", "G05"},
         {"signal", "L1C"}, {"delta_cycles", 10.0}, {"flag_lli", true}},
        {{"kind", "outage"}, {"tow", 200.0}, {"station", "STA2"}, {"sat", "G07"},
         {"duration_s", 600.0}},
        {{"kind", "anchor_handover"}, {"tow", 300.0}, {"sat", "G05"}, {"mode", "cold"}},
        {{"kind", "arc_erase"}, {"tow", 400.0}, {"station", "STA1"}, {"sat", "G07"}},
        {{"kind", "sat_clock_step"}, {"tow", 500.0}, {"sat", "G05"}, {"step_m", 0.5}},
        {{"kind", "rx_clock_step"}, {"tow", 600.0}, {"station", "STA1"}, {"step_m", 0.5}},
    })}};
    ASSERT_TRUE(loadWith(ij, cfg, lm, "t3"));
    const auto& ic = cfg.getNetworkConfig().inject;
    ASSERT_EQ(ic.events.size(), 6u);
    EXPECT_TRUE(ic.enable);                       // implied by a non-empty events list
    EXPECT_EQ(ic.events[0].kind, "slip");
    EXPECT_TRUE(ic.events[0].flag_lli);
    EXPECT_DOUBLE_EQ(ic.events[0].delta_cycles, 10.0);
    EXPECT_DOUBLE_EQ(ic.events[1].duration_s, 600.0);
    EXPECT_EQ(ic.events[2].mode, "cold");
    EXPECT_EQ(ic.events[2].signal, "all");        // defaulted
    EXPECT_DOUBLE_EQ(ic.events[4].step_m, 0.5);
    // rx_clock_step, the dual of sat_clock_step: a station, no satellite, no signal.
    EXPECT_EQ(ic.events[5].kind, "rx_clock_step");
    EXPECT_EQ(ic.events[5].station, "STA1");
    EXPECT_TRUE(ic.events[5].sat.empty());
    EXPECT_TRUE(ic.events[5].signal.empty());
    EXPECT_DOUBLE_EQ(ic.events[5].step_m, 0.5);
}

TEST(NetworkInjectConfig, EnableFalseParksTheSchedule) {
    LogManager lm;
    GnssConfig cfg;
    nlohmann::json ij = slipEvent(100.0, 3.0);
    ij["enable"] = false;
    ASSERT_TRUE(loadWith(ij, cfg, lm, "t4"));
    EXPECT_FALSE(cfg.getNetworkConfig().inject.enable);
    EXPECT_EQ(cfg.getNetworkConfig().inject.events.size(), 1u);
}

/// A missing required field fails the load instead of falling through to a default
/// that arms a different event than the operator wrote.
class NetworkInjectRejects : public ::testing::TestWithParam<nlohmann::json> {};

TEST_P(NetworkInjectRejects, MalformedEventFailsTheLoad) {
    LogManager lm;
    GnssConfig cfg;
    EXPECT_FALSE(loadWith({{"events", nlohmann::json::array({GetParam()})}}, cfg, lm, "t5"));
}

INSTANTIATE_TEST_SUITE_P(
    Malformed, NetworkInjectRejects,
    ::testing::Values(
        // unknown kind
        nlohmann::json{{"kind", "wobble"}, {"tow", 1.0}},
        // missing tow
        nlohmann::json{{"kind", "slip"}, {"station", "STA1"}, {"sat", "G05"},
                       {"signal", "L1C"}, {"delta_cycles", 1.0}},
        // tow out of range
        nlohmann::json{{"kind", "sat_clock_step"}, {"tow", 999999.0}, {"sat", "G05"},
                       {"step_m", 1.0}},
        // slip with no signal
        nlohmann::json{{"kind", "slip"}, {"tow", 1.0}, {"station", "STA1"},
                       {"sat", "G05"}, {"delta_cycles", 1.0}},
        // slip on a CODE observable
        nlohmann::json{{"kind", "slip"}, {"tow", 1.0}, {"station", "STA1"},
                       {"sat", "G05"}, {"signal", "C1C"}, {"delta_cycles", 1.0}},
        // slip with zero delta (an event that cannot be detected is a typo)
        nlohmann::json{{"kind", "slip"}, {"tow", 1.0}, {"station", "STA1"},
                       {"sat", "G05"}, {"signal", "L1C"}, {"delta_cycles", 0.0}},
        // outage with no duration
        nlohmann::json{{"kind", "outage"}, {"tow", 1.0}, {"station", "STA1"},
                       {"sat", "G05"}},
        // arc_erase with no station
        nlohmann::json{{"kind", "arc_erase"}, {"tow", 1.0}, {"sat", "G05"}},
        // arc_erase with an unknown Study-1 diagnostic mode
        nlohmann::json{{"kind", "arc_erase"}, {"tow", 1.0}, {"station", "STA1"},
                       {"sat", "G05"}, {"mode", "nuke_it"}},
        // arc_erase whose band scope names a CODE observable
        nlohmann::json{{"kind", "arc_erase"}, {"tow", 1.0}, {"station", "STA1"},
                       {"sat", "G05"}, {"signal", "C1C"}},
        // anchor_handover with a bad mode
        nlohmann::json{{"kind", "anchor_handover"}, {"tow", 1.0}, {"sat", "G05"},
                       {"mode", "warm"}},
        // sat_clock_step with a zero step
        nlohmann::json{{"kind", "sat_clock_step"}, {"tow", 1.0}, {"sat", "G05"},
                       {"step_m", 0.0}},
        // rx_clock_step with no station (the whole event is the station)
        nlohmann::json{{"kind", "rx_clock_step"}, {"tow", 1.0}, {"step_m", 0.5}},
        // rx_clock_step with a zero step
        nlohmann::json{{"kind", "rx_clock_step"}, {"tow", 1.0}, {"station", "STA1"},
                       {"step_m", 0.0}},
        // rx_clock_step with a `sat`: it reads as a scope but is not one.  A receiver
        // clock jump is station-common, so accepting it would arm a different event
        // than the operator wrote
        nlohmann::json{{"kind", "rx_clock_step"}, {"tow", 1.0}, {"station", "STA1"},
                       {"sat", "G05"}, {"step_m", 0.5}},
        // rx_clock_step with a `signal`: the same reasoning on the dispersive axis
        nlohmann::json{{"kind", "rx_clock_step"}, {"tow", 1.0}, {"station", "STA1"},
                       {"signal", "L1C"}, {"step_m", 0.5}}));

// ---------------------------------------------------------------- epoch semantics
TEST(NetworkInjectEpoch, UnarmedReturnsTheCallersOwnEpoch) {
    Harness h{nlohmann::json(), "h1"};
    EXPECT_FALSE(h.ctx->inject.armed);
    SyncedEpoch se = epochAt(1000.0), scratch;
    const SyncedEpoch& out =
            networkInjectEpoch(*h.ctx, h.log, *h.traces, se, scratch);
    EXPECT_EQ(&out, &se);                 // no copy taken at all
    EXPECT_TRUE(scratch.by_station.empty());
}

TEST(NetworkInjectEpoch, SlipIsAStepNotARamp) {
    Harness h{slipEvent(1000.0, 10.0), "h2"};
    ASSERT_TRUE(h.ctx->inject.armed);
    const double l0 = 1.0e8;

    // Before the tow: untouched, and no copy.
    SyncedEpoch pre = epochAt(970.0), s0;
    const SyncedEpoch& o0 = networkInjectEpoch(*h.ctx, h.log, *h.traces, pre, s0);
    EXPECT_EQ(&o0, &pre);
    EXPECT_DOUBLE_EQ(o0.by_station.at("STA1")[0].measurements.at("L1C"), l0);

    // Onset and three more epochs: the same +10 cyc every time.
    for (int k = 0; k < 4; ++k) {
        SyncedEpoch se = epochAt(1000.0 + 30.0 * k), scratch;
        const SyncedEpoch& out =
                networkInjectEpoch(*h.ctx, h.log, *h.traces, se, scratch);
        EXPECT_EQ(&out, &scratch) << "epoch " << k;
        EXPECT_DOUBLE_EQ(out.by_station.at("STA1")[0].measurements.at("L1C"), l0 + 10.0)
                << "epoch " << k << ": the shift accumulated instead of holding";
        // The other satellite, the other station and the code channel are untouched.
        EXPECT_DOUBLE_EQ(out.by_station.at("STA1")[1].measurements.at("L1C"), 1.1e8);
        EXPECT_DOUBLE_EQ(out.by_station.at("STA2")[0].measurements.at("L1C"), l0);
        EXPECT_DOUBLE_EQ(out.by_station.at("STA1")[0].measurements.at("C1C"), 2.0e7);
    }
    EXPECT_EQ(h.ctx->inject.n_applied[0], 4);
}

TEST(NetworkInjectEpoch, SlipLliIsRaisedOnTheOnsetEpochOnly) {
    Harness h{slipEvent(1000.0, 10.0, /*lli=*/true), "h3"};
    SyncedEpoch a = epochAt(1000.0), sa;
    const SyncedEpoch& oa = networkInjectEpoch(*h.ctx, h.log, *h.traces, a, sa);
    EXPECT_EQ(oa.by_station.at("STA1")[0].lli_flags.at("L1C") & 0x1, 1);
    SyncedEpoch b = epochAt(1030.0), sb;
    const SyncedEpoch& ob = networkInjectEpoch(*h.ctx, h.log, *h.traces, b, sb);
    EXPECT_EQ(ob.by_station.at("STA1")[0].lli_flags.count("L1C") == 0
                      ? 0
                      : (ob.by_station.at("STA1")[0].lli_flags.at("L1C") & 0x1),
              0);
}

TEST(NetworkInjectEpoch, OutageDropsExactlyTheWindow) {
    nlohmann::json ij = {{"events", nlohmann::json::array({
        {{"kind", "outage"}, {"tow", 1000.0}, {"station", "STA1"}, {"sat", "G05"},
         {"duration_s", 90.0}}})}};   // 3 epochs at 30 s
    Harness h{ij, "h4"};
    const double tows[] = { 970.0, 1000.0, 1030.0, 1060.0, 1090.0, 1120.0 };
    const bool  gone[]  = { false,  true,   true,   true,   false,  false };
    for (int k = 0; k < 6; ++k) {
        SyncedEpoch se = epochAt(tows[k]), scratch;
        const SyncedEpoch& out =
                networkInjectEpoch(*h.ctx, h.log, *h.traces, se, scratch);
        std::size_t n_g05 = 0;
        for (const auto& o : out.by_station.at("STA1"))
            if (o.satellite == "G05") ++n_g05;
        EXPECT_EQ(n_g05, gone[k] ? 0u : 1u) << "tow " << tows[k];
        // The station's other satellite is never dropped.
        std::size_t n_g07 = 0;
        for (const auto& o : out.by_station.at("STA1"))
            if (o.satellite == "G07") ++n_g07;
        EXPECT_EQ(n_g07, 1u) << "tow " << tows[k];
        // The other station's G05 is never touched.
        EXPECT_EQ(out.by_station.at("STA2").size(), 1u);
    }
    EXPECT_EQ(h.ctx->inject.n_applied[0], 3);
}

TEST(NetworkInjectEpoch, SatClockStepMovesEveryStationAndBothChannels) {
    nlohmann::json ij = {{"events", nlohmann::json::array({
        {{"kind", "sat_clock_step"}, {"tow", 1000.0}, {"sat", "G05"},
         {"step_m", 0.5}}})}};
    Harness h{ij, "h5"};
    SyncedEpoch se = epochAt(1000.0), scratch;
    const SyncedEpoch& out = networkInjectEpoch(*h.ctx, h.log, *h.traces, se, scratch);
    for (const char* sta : { "STA1", "STA2" }) {
        const auto& o = out.by_station.at(sta)[0];
        ASSERT_EQ(o.satellite, "G05");
        EXPECT_DOUBLE_EQ(o.measurements.at("C1C"), 2.0e7 + 0.5);
        // Phase moved by step/lambda cycles: L1 lambda ~ 0.1903 m, so ~2.63 cyc.
        const double d_cyc = o.measurements.at("L1C") - 1.0e8;
        EXPECT_GT(d_cyc, 2.5);
        EXPECT_LT(d_cyc, 2.7);
    }
    // A different satellite is untouched on both channels.
    EXPECT_DOUBLE_EQ(out.by_station.at("STA1")[1].measurements.at("C1C"), 2.1e7);
    EXPECT_DOUBLE_EQ(out.by_station.at("STA1")[1].measurements.at("L1C"), 1.1e8);
}

TEST(NetworkInjectEpoch, RxClockStepMovesEverySignalOfOneStationOnly) {
    // The dual of sat_clock_step: one station, every satellite, both channels.
    nlohmann::json ij = {{"events", nlohmann::json::array({
        {{"kind", "rx_clock_step"}, {"tow", 1000.0}, {"station", "STA1"},
         {"step_m", 0.5}}})}};
    Harness h{ij, "h5r"};
    SyncedEpoch se = epochAt(1000.0), scratch;
    const SyncedEpoch& out = networkInjectEpoch(*h.ctx, h.log, *h.traces, se, scratch);

    // Every satellite of the stepped station moved, by the same metres on code.
    const auto& s1 = out.by_station.at("STA1");
    ASSERT_EQ(s1.size(), 2u);
    EXPECT_DOUBLE_EQ(s1[0].measurements.at("C1C"), 2.0e7 + 0.5);   // G05
    EXPECT_DOUBLE_EQ(s1[1].measurements.at("C1C"), 2.1e7 + 0.5);   // G07
    // Phase moved by step/lambda cycles (L1 lambda ~ 0.1903 m => ~2.63 cyc), the same
    // cycle count on both satellites because the band is the same (non-dispersive).
    const double d05 = s1[0].measurements.at("L1C") - 1.0e8;
    const double d07 = s1[1].measurements.at("L1C") - 1.1e8;
    EXPECT_GT(d05, 2.5);
    EXPECT_LT(d05, 2.7);
    EXPECT_NEAR(d05, d07, 1e-9);

    // A different station is untouched on both channels; this is the half that
    // separates a receiver-clock jump from a satellite-clock jump.
    const auto& s2 = out.by_station.at("STA2");
    ASSERT_EQ(s2.size(), 1u);
    EXPECT_DOUBLE_EQ(s2[0].measurements.at("C1C"), 2.0e7);
    EXPECT_DOUBLE_EQ(s2[0].measurements.at("L1C"), 1.0e8);

    // 2 satellites x (1 code + 1 phase) = 4 observables touched, fired once.
    EXPECT_EQ(h.ctx->inject.n_applied[0], 1);
    EXPECT_TRUE(h.ctx->inject.onset_logged[0]);
    EXPECT_EQ(h.ctx->inject.fired_by_kind["rx_clock_step"], 1);
    EXPECT_EQ(networkInjectRunEnd(*h.ctx, h.log), 0);
}

TEST(NetworkInjectEpoch, RxClockStepPersistsAndDoesNotAccumulate) {
    // A receiver that stepped its clock does not step it back: the same delta on
    // every later epoch, applied to that epoch's own raw observables (neither a
    // ramp nor a one-epoch spike).  Nothing before tow moves.
    nlohmann::json ij = {{"events", nlohmann::json::array({
        {{"kind", "rx_clock_step"}, {"tow", 1000.0}, {"station", "STA1"},
         {"step_m", 0.5}}})}};
    Harness h{ij, "h5s"};

    SyncedEpoch before = epochAt(970.0), sb;
    const SyncedEpoch& o0 = networkInjectEpoch(*h.ctx, h.log, *h.traces, before, sb);
    EXPECT_DOUBLE_EQ(o0.by_station.at("STA1")[0].measurements.at("C1C"), 2.0e7);
    EXPECT_EQ(h.ctx->inject.n_applied[0], 0);

    double first = 0.0;
    for (int i = 0; i < 3; ++i) {
        SyncedEpoch se = epochAt(1000.0 + 30.0 * i), scratch;
        const SyncedEpoch& out = networkInjectEpoch(*h.ctx, h.log, *h.traces, se, scratch);
        const double d = out.by_station.at("STA1")[0].measurements.at("C1C") - 2.0e7;
        if (i == 0) first = d;
        EXPECT_DOUBLE_EQ(d, 0.5);
        EXPECT_DOUBLE_EQ(d, first);        // constant, not accumulating
    }
    EXPECT_EQ(h.ctx->inject.n_applied[0], 3);
}

TEST(NetworkInjectEpoch, RxClockStepOnAnAbsentStationNeverFires) {
    // The harness rule: an event whose target does not exist fails the run at the
    // end; otherwise the leg would be graded as a null result.
    nlohmann::json ij = {{"events", nlohmann::json::array({
        {{"kind", "rx_clock_step"}, {"tow", 1000.0}, {"station", "NOPE"},
         {"step_m", 0.5}}})}};
    Harness h{ij, "h5t"};
    SyncedEpoch se = epochAt(1000.0), scratch;
    (void)networkInjectEpoch(*h.ctx, h.log, *h.traces, se, scratch);
    EXPECT_FALSE(h.ctx->inject.anyFired());
    EXPECT_EQ(networkInjectRunEnd(*h.ctx, h.log), 1);
}

TEST(NetworkInjectEpoch, DecisionKindsStageAOneEpochDirective) {
    nlohmann::json ij = {{"events", nlohmann::json::array({
        {{"kind", "anchor_handover"}, {"tow", 1000.0}, {"sat", "G05"},
         {"mode", "cold"}},
        {{"kind", "arc_erase"}, {"tow", 1000.0}, {"station", "STA1"},
         {"sat", "G07"}}})}};
    Harness h{ij, "h6"};
    SyncedEpoch a = epochAt(1000.0), sa;
    (void)networkInjectEpoch(*h.ctx, h.log, *h.traces, a, sa);
    ASSERT_EQ(h.ctx->inject.anchor_now.size(), 1u);
    EXPECT_TRUE(h.ctx->inject.anchor_now[0].cold);
    EXPECT_TRUE(h.ctx->inject.anchor_now[0].matches("G05_L1C"));
    EXPECT_FALSE(h.ctx->inject.anchor_now[0].matches("G07_L1C"));
    ASSERT_EQ(h.ctx->inject.arc_erase_now.size(), 1u);
    EXPECT_EQ(h.ctx->inject.arc_erase_now[0].station, "STA1");
    EXPECT_EQ(h.ctx->inject.arc_erase_now[0].sat, "G07");
    // Study 1: no mode named => the production erase, whole satellite.
    EXPECT_EQ(h.ctx->inject.arc_erase_now[0].mode, "erase");
    EXPECT_TRUE(h.ctx->inject.arc_erase_now[0].isErase());
    EXPECT_TRUE(h.ctx->inject.arc_erase_now[0].signals.empty());
    EXPECT_TRUE(h.ctx->inject.arc_erase_now[0].coversSignal("L5Q"));

    // The next epoch clears them, so a directive fires once.  In a real leg the
    // consumers close the firing ledger by setting onset_logged; no consumer runs
    // here, so the test sets it by hand, since otherwise the directives restage.
    h.ctx->inject.onset_logged[0] = true;
    h.ctx->inject.onset_logged[1] = true;
    SyncedEpoch b = epochAt(1030.0), sb;
    (void)networkInjectEpoch(*h.ctx, h.log, *h.traces, b, sb);
    EXPECT_TRUE(h.ctx->inject.anchor_now.empty());
    EXPECT_TRUE(h.ctx->inject.arc_erase_now.empty());
}

TEST(NetworkInjectEpoch, ArcEraseStudy1ModeAndBandScope) {
    // docs/SPEC_event_harness.md §9: the diagnostic sub-knobs are parsed onto
    // the staged directive, free_epoch is routed away from the T4 block
    // (isErase() false), and a ";"-separated `signal` narrows the band scope.
    nlohmann::json ij = {{"events", nlohmann::json::array({
        {{"kind", "arc_erase"}, {"tow", 1000.0}, {"station", "STA1"},
         {"sat", "G07"}, {"mode", "free_epoch"}, {"signal", "L1C;L2W"}},
        {{"kind", "arc_erase"}, {"tow", 1000.0}, {"station", "STA1"},
         {"sat", "G08"}, {"mode", "erase_keep_iono"}, {"signal", "all"}}})}};
    Harness h{ij, "h_study1"};
    SyncedEpoch a = epochAt(1000.0), sa;
    (void)networkInjectEpoch(*h.ctx, h.log, *h.traces, a, sa);
    ASSERT_EQ(h.ctx->inject.arc_erase_now.size(), 2u);

    const auto& d0 = h.ctx->inject.arc_erase_now[0];
    EXPECT_EQ(d0.mode, "free_epoch");
    EXPECT_FALSE(d0.isErase());          // the PASS-A drain owns it, not T4
    EXPECT_EQ(d0.signals.size(), 2u);
    EXPECT_TRUE(d0.coversSignal("L1C"));
    EXPECT_TRUE(d0.coversSignal("L2W"));
    EXPECT_FALSE(d0.coversSignal("L5Q")); // the anchor band stays pinned
    EXPECT_EQ(d0.ev_index, 0u);

    const auto& d1 = h.ctx->inject.arc_erase_now[1];
    EXPECT_EQ(d1.mode, "erase_keep_iono");
    EXPECT_TRUE(d1.isErase());
    EXPECT_TRUE(d1.signals.empty());     // "all" == every signal
    EXPECT_TRUE(d1.coversSignal("L5Q"));
    EXPECT_EQ(d1.ev_index, 1u);
}

TEST(NetworkInjectEpoch, ArmedButNeverFiredIsAHardRunEndError) {
    // A station that is not in any epoch: the event can never fire.
    nlohmann::json ij = {{"events", nlohmann::json::array({
        {{"kind", "slip"}, {"tow", 1000.0}, {"station", "NOPE"}, {"sat", "G05"},
         {"signal", "L1C"}, {"delta_cycles", 5.0}}})}};
    Harness h{ij, "h7"};
    SyncedEpoch se = epochAt(1000.0), scratch;
    (void)networkInjectEpoch(*h.ctx, h.log, *h.traces, se, scratch);
    EXPECT_EQ(h.ctx->inject.n_applied[0], 0);
    EXPECT_FALSE(h.ctx->inject.anyFired());
    EXPECT_EQ(networkInjectRunEnd(*h.ctx, h.log), 1);
}

TEST(NetworkInjectEpoch, FiredScheduleReportsCleanAtRunEnd) {
    Harness h{slipEvent(1000.0, 10.0), "h8"};
    SyncedEpoch se = epochAt(1000.0), scratch;
    (void)networkInjectEpoch(*h.ctx, h.log, *h.traces, se, scratch);
    EXPECT_TRUE(h.ctx->inject.anyFired());
    EXPECT_EQ(networkInjectRunEnd(*h.ctx, h.log), 0);
}

TEST(NetworkInjectEpoch, UnarmedRunEndIsAlwaysClean) {
    Harness h{nlohmann::json(), "h9"};
    EXPECT_EQ(networkInjectRunEnd(*h.ctx, h.log), 0);
}
