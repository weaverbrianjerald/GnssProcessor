/**
 * @file test_network_gross_los_cold_bound.cpp
 * @brief The gross line-of-sight screen's cold-start bound
 *        (@c network.gross_los_cold_bound_m;
 *        docs/refound/ICF_ignition_code_fault_2026-10-09.md).
 *
 * Inside the screen's maturity horizon the σ-bar declarations are held, and a faulty
 * receiver channel there (BSHM00ISR G07 on the field golden) moves a satellite clock by
 * metres.  The bound declares a line of sight whose largest post-fit code |v| exceeds it,
 * at once; while the declaration stands, each epoch over the bound erases the carried
 * ambiguity again, and the release re-seeds it.
 *
 * What is exercised here, without a network: the pure transition
 * (@ref networkGrossLosColdStep) and its interplay with the screen's own release step,
 * the act half's erase and sweep on hand-built assemblies, the publish half's fold of a
 * cold declaration, the loader and the knob resolve, and the run-end readout's INERT
 * clause (standing invariant #4).  What a declaration does to the solve is graded by the
 * validation gates on the goldens, not here.
 */
#include <gtest/gtest.h>

#include "network/state/RunContext.hpp"     // knobs, the per-LOS states, the act lists, the tallies
#include "network/driver/Epoch.hpp"   // EpochEvents
#include "config/GnssConfig.hpp"
#include "network/qc/GrossLos.hpp"
#include "network/driver/RunTypes.hpp"    // NetStation / StationAssembly / LocalCarry
#include "logging/LogManager.hpp"

#include <spdlog/spdlog.h>
#include <spdlog/sinks/ostream_sink.h>

#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <tuple>
#include <vector>
#include "../support/TestTempDir.hpp"   // per-process, per-test scratch directory

using namespace GnssProcessor;

namespace {

namespace fs = std::filesystem;

/// A verdict with @p n_rows code rows whose largest |v| is @p v_abs_max metres.
GrossLosVerdict verdict(double v_abs_max, int n_rows = 1) {
    GrossLosVerdict v;
    v.n_rows    = n_rows;
    v.v_abs_max = v_abs_max;
    v.r_max     = v_abs_max / 0.46;
    return v;
}

/// A minimal loadable network config, with @p extra spliced into the network block.
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
    const fs::path p = gnss_test::tempDir() / ("icf_cold_cfg_" + tag + ".json");
    {
        std::ofstream f(p);
        f << networkConfig(extra);
    }
    const bool ok = cfg.loadFromJson(p, log);
    std::error_code ec;
    fs::remove(p, ec);
    return ok;
}

/// One station assembly carrying @p sat on the phase signals @p sigs.
StationAssembly assemblyWith(const std::string& sat, const std::vector<std::string>& sigs) {
    StationAssembly R;
    R.ok = true;
    int col = 0;
    for (const auto& sig : sigs) {
        PlAmb pa;
        pa.sat      = sat;
        pa.signal   = sig;
        pa.amb_key  = sat + "_" + sig;
        pa.col      = col++;
        pa.elev_deg = 30.0;
        R.pl_amb.push_back(pa);
    }
    return R;
}

/// A logger writing into @p oss, unregistered (so no global name is taken).
std::shared_ptr<spdlog::logger> captureLogger(std::ostringstream& oss) {
    auto sink = std::make_shared<spdlog::sinks::ostream_sink_mt>(oss);
    return std::make_shared<spdlog::logger>("icf_cold_bound_test", sink);
}

}  // namespace

// ---------------------------------------------------------------------------
// The transition: networkGrossLosColdStep
// ---------------------------------------------------------------------------

// A bound of 0 is the off state: nothing changes, whatever the residual (invariant #6).
TEST(NetworkGrossLosColdBound, OffIsInertWhateverTheResidual) {
    GrossLosState s;
    EXPECT_EQ(networkGrossLosColdStep(s, verdict(343.0), 0.0, true, true, 259410.0),
              GrossLosColdEdge::None);
    EXPECT_EQ(networkGrossLosColdStep(s, verdict(343.0), -5.0, true, true, 259410.0),
              GrossLosColdEdge::None);
    EXPECT_FALSE(s.gross);
    EXPECT_FALSE(s.cold);
    EXPECT_EQ(s.n_declared, 0);
}

// Inside the horizon a line of sight over the bound is declared on its first epoch, with
// no persistence and no row count; after the horizon the bound declares nothing (the σ
// bar owns that window).
TEST(NetworkGrossLosColdBound, DeclaresAtOnceInsideTheHorizonOnly) {
    GrossLosState s;
    EXPECT_EQ(networkGrossLosColdStep(s, verdict(24.2), 12.0, /*in_hold=*/true, true, 259410.0),
              GrossLosColdEdge::Declare);
    EXPECT_TRUE(s.gross);
    EXPECT_TRUE(s.cold);
    EXPECT_EQ(s.n_declared, 1);
    EXPECT_DOUBLE_EQ(s.declared_tow, 259410.0);
    EXPECT_EQ(s.over_streak, 0);
    EXPECT_EQ(s.clear_streak, 0);

    GrossLosState late;
    EXPECT_EQ(networkGrossLosColdStep(late, verdict(24.2), 12.0, /*in_hold=*/false, true,
                                      262800.0),
              GrossLosColdEdge::None);
    EXPECT_FALSE(late.gross);
}

// The bound is strict, and an empty row set is never evidence (the screen's rule).
TEST(NetworkGrossLosColdBound, EqualityIsNotOverAndAbsenceIsNotEvidence) {
    GrossLosState s;
    EXPECT_EQ(networkGrossLosColdStep(s, verdict(12.0), 12.0, true, true, 0.0),
              GrossLosColdEdge::None);
    EXPECT_EQ(networkGrossLosColdStep(s, verdict(99.0, /*n_rows=*/0), 12.0, true, true, 0.0),
              GrossLosColdEdge::None);
    EXPECT_FALSE(s.gross);
}

// The cold station guard holds the declaration and says so; the state is untouched.
TEST(NetworkGrossLosColdBound, TheStationGuardHoldsTheDeclaration) {
    GrossLosState s;
    EXPECT_EQ(networkGrossLosColdStep(s, verdict(40.0), 12.0, true, /*allow=*/false, 0.0),
              GrossLosColdEdge::Held);
    EXPECT_FALSE(s.gross);
    EXPECT_FALSE(s.cold);
    EXPECT_EQ(s.n_declared, 0);
}

// While a cold declaration stands, each epoch over the bound asks for the erase again,
// inside the horizon and after it; an epoch under the bound does not.  A declaration
// taken on the σ bar is never re-erased by the cold path.
TEST(NetworkGrossLosColdBound, AStandingColdDeclarationIsReErasedWhileTheBoundIsExceeded) {
    GrossLosState s;
    ASSERT_EQ(networkGrossLosColdStep(s, verdict(24.2), 12.0, true, true, 259410.0),
              GrossLosColdEdge::Declare);
    EXPECT_EQ(networkGrossLosColdStep(s, verdict(0.0), 12.0, true, true, 259440.0),
              GrossLosColdEdge::None);       // the epoch after the erase: no redundancy
    EXPECT_EQ(networkGrossLosColdStep(s, verdict(43.0), 12.0, true, true, 259470.0),
              GrossLosColdEdge::Refloat);
    EXPECT_EQ(networkGrossLosColdStep(s, verdict(43.0), 12.0, false, false, 262800.0),
              GrossLosColdEdge::Refloat);    // outlives the horizon; the guard does not apply
    EXPECT_EQ(s.n_declared, 1);              // a re-erase is not a new episode

    GrossLosState sigma_declared;
    sigma_declared.gross = true;             // declared on the σ bar
    EXPECT_EQ(networkGrossLosColdStep(sigma_declared, verdict(500.0), 12.0, true, true, 0.0),
              GrossLosColdEdge::None);
}

// The release is the screen's own test (two epochs under the clear bar); the cold flag
// is what tells the seam to re-seed at that edge.
TEST(NetworkGrossLosColdBound, TheReleaseIsTheScreensOwnTest) {
    GrossLosState s;
    ASSERT_EQ(networkGrossLosColdStep(s, verdict(24.2), 12.0, true, true, 259410.0),
              GrossLosColdEdge::Declare);
    const std::vector<GrossLosSample> big   = { GrossLosSample{ "C1C", 90.0, 41.4 } };
    const std::vector<GrossLosSample> clean = { GrossLosSample{ "C1C", 0.4, 0.2 } };
    const GrossLosVerdict vb = networkGrossLosAssess(big, 8.0, 2, 3.0, true);
    const GrossLosVerdict vc = networkGrossLosAssess(clean, 8.0, 2, 3.0, true);
    EXPECT_EQ(networkGrossLosStep(s, vb, 2, 2, 259440.0, false), GrossLosEdge::None);
    EXPECT_EQ(networkGrossLosStep(s, vc, 2, 2, 259770.0, false), GrossLosEdge::None);
    EXPECT_EQ(networkGrossLosStep(s, vc, 2, 2, 259800.0, false), GrossLosEdge::Clear);
    EXPECT_FALSE(s.gross);
    EXPECT_TRUE(s.cold) << "the step leaves the flag to the seam, which re-seeds and clears it";
}

// ---------------------------------------------------------------------------
// The act half and the publish half
// ---------------------------------------------------------------------------

// The cold erase removes every carried ambiguity of the line of sight, including one
// of a signal absent from this epoch's assembly, and its iono carry; another line of
// sight at the station is untouched.
TEST(NetworkGrossLosColdBound, TheColdEraseSweepsEveryCarriedAmbiguityOfTheLineOfSight) {
    LogManager log; GnssConfig cfg;
    ASSERT_TRUE(loadWith("    \"gross_los_screen\": true,\n"
                         "    \"gross_los_cold_bound_m\": 12.0,\n", cfg, log, "act"));
    NetworkRunContext ctx(cfg, cfg.getNetworkConfig());
    EpochEvents events;
    ASSERT_DOUBLE_EQ(ctx.knobs.gl_cold_bound_m, 12.0);

    std::vector<NetStation> stations(1);
    stations[0].name = "STA1";
    std::vector<StationAssembly> asm_res{ assemblyWith("G07", { "L1C" }) };

    const auto l1 = std::make_tuple(0, std::string("G07"), std::string("L1C"));
    const auto l2 = std::make_tuple(0, std::string("G07"), std::string("L2W"));
    const auto other = std::make_tuple(0, std::string("G05"), std::string("L1C"));
    net::putArc(ctx.entities, l1, &net::ArcState::amb)    = LocalCarry{ 178.0, 1.0e-4, 100.0 };
    net::putArc(ctx.entities, l2, &net::ArcState::amb)    = LocalCarry{ -3.0, 1.0e-4, 100.0 };   // not tracked this epoch
    net::putArc(ctx.entities, other, &net::ArcState::amb) = LocalCarry{ 12.0, 1.0e-4, 100.0 };
    net::putLos(ctx.entities, 0, "G07", &net::LosState::iono) = LocalCarry{ -91.0, 1.0e-2, 100.0 };

    events.gross.gl_act_cold_refloat.push_back({ 0, "G07" });
    networkGrossLosAct(ctx, nullptr, stations, asm_res, events);

    EXPECT_EQ((net::getArc(ctx.entities.arcs, l1, &net::ArcState::amb) != nullptr), 0u);
    EXPECT_EQ((net::getArc(ctx.entities.arcs, l2, &net::ArcState::amb) != nullptr), 0u);
    EXPECT_EQ((net::getArc(ctx.entities.arcs, other, &net::ArcState::amb) != nullptr), 1u);
    EXPECT_EQ(static_cast<std::size_t>(net::getLos(ctx.entities.los, 0, "G07", &net::LosState::iono) != nullptr), 0u);
    EXPECT_EQ(ctx.counters.gross_los.cold_amb_erased, 2);
    EXPECT_TRUE((net::present(ctx.entities.arcs, &net::ArcState::gl_hold_amb) == 0)) << "the cold path never holds";
}

// A cold declaration folds the line of sight's code rows exactly as a σ-bar one does,
// and the release lifts the fold.
TEST(NetworkGrossLosColdBound, AColdDeclarationFoldsTheCodeRows) {
    LogManager log; GnssConfig cfg;
    ASSERT_TRUE(loadWith("    \"gross_los_screen\": true,\n"
                         "    \"gross_los_cold_bound_m\": 12.0,\n", cfg, log, "publish"));
    NetworkRunContext ctx(cfg, cfg.getNetworkConfig());
    EpochEvents events;
    std::vector<NetStation> stations(1);
    stations[0].name = "STA1";

    GrossLosState& st = net::putLos(ctx.entities, 0, "G07", &net::LosState::gross_los);
    ASSERT_EQ(networkGrossLosColdStep(st, verdict(24.2), ctx.knobs.gl_cold_bound_m, true, true,
                                      259410.0),
              GrossLosColdEdge::Declare);
    networkGrossLosPublishScales(ctx, stations);
    ASSERT_EQ(stations[0].gl_code_sigma_scale.count("G07"), 1u);
    EXPECT_DOUBLE_EQ(stations[0].gl_code_sigma_scale["G07"], 100.0);

    st.gross = false;
    networkGrossLosPublishScales(ctx, stations);
    EXPECT_TRUE(stations[0].gl_code_sigma_scale.empty());
}

// ---------------------------------------------------------------------------
// The loader and the knob resolve
// ---------------------------------------------------------------------------

TEST(NetworkGrossLosColdBound, ParsesClampsAndResolves) {
    {   // default: off
        LogManager log; GnssConfig cfg;
        ASSERT_TRUE(loadWith("    \"gross_los_screen\": true,\n", cfg, log, "def"));
        EXPECT_DOUBLE_EQ(cfg.networkGrossLosColdBoundM(), 0.0);
        NetworkRunContext ctx(cfg, cfg.getNetworkConfig());
    EpochEvents events;
        EXPECT_DOUBLE_EQ(ctx.knobs.gl_cold_bound_m, 0.0);
    }
    {   // armed
        LogManager log; GnssConfig cfg;
        ASSERT_TRUE(loadWith("    \"gross_los_screen\": true,\n"
                             "    \"gross_los_cold_bound_m\": 12.0,\n", cfg, log, "on"));
        EXPECT_DOUBLE_EQ(cfg.networkGrossLosColdBoundM(), 12.0);
        EXPECT_DOUBLE_EQ(cfg.getNetworkConfig().gross_los_cold_bound_m, 12.0);
        NetworkRunContext ctx(cfg, cfg.getNetworkConfig());
    EpochEvents events;
        EXPECT_DOUBLE_EQ(ctx.knobs.gl_cold_bound_m, 12.0);
    }
    {   // the screen off: parsed, resolved off
        LogManager log; GnssConfig cfg;
        ASSERT_TRUE(loadWith("    \"gross_los_cold_bound_m\": 12.0,\n", cfg, log, "noscreen"));
        EXPECT_DOUBLE_EQ(cfg.networkGrossLosColdBoundM(), 12.0);
        NetworkRunContext ctx(cfg, cfg.getNetworkConfig());
    EpochEvents events;
        EXPECT_FALSE(ctx.knobs.gl_on);
        EXPECT_DOUBLE_EQ(ctx.knobs.gl_cold_bound_m, 0.0);
    }
    {   // under the hold: resolved off (the hold would keep the level the response erases)
        LogManager log; GnssConfig cfg;
        ASSERT_TRUE(loadWith("    \"gross_los_screen\": true,\n"
                             "    \"gross_los_bound\": true,\n"
                             "    \"gross_los_cold_bound_m\": 12.0,\n", cfg, log, "hold"));
        NetworkRunContext ctx(cfg, cfg.getNetworkConfig());
    EpochEvents events;
        EXPECT_TRUE(ctx.knobs.gl_bound);
        EXPECT_DOUBLE_EQ(ctx.knobs.gl_cold_bound_m, 0.0);
    }
    {   // a negative value cannot express a bound: clamped to off
        LogManager log; GnssConfig cfg;
        ASSERT_TRUE(loadWith("    \"gross_los_screen\": true,\n"
                             "    \"gross_los_cold_bound_m\": -3.0,\n", cfg, log, "neg"));
        EXPECT_DOUBLE_EQ(cfg.networkGrossLosColdBoundM(), 0.0);
    }
}

// ---------------------------------------------------------------------------
// The run-end readout (invariant #4)
// ---------------------------------------------------------------------------

// Off prints nothing; armed with nothing declared prints an INERT clause naming the
// case; armed with a declaration names the line of sight.
TEST(NetworkGrossLosColdBound, RunEndReadoutSaysInertOrNamesTheLineOfSight) {
    {
        LogManager log; GnssConfig cfg;
        ASSERT_TRUE(loadWith("    \"gross_los_screen\": true,\n", cfg, log, "re_off"));
        NetworkRunContext ctx(cfg, cfg.getNetworkConfig());
    EpochEvents events;
        std::ostringstream oss;
        networkGrossLosColdBoundRunEnd(ctx, cfg, captureLogger(oss));
        EXPECT_TRUE(oss.str().empty());
    }
    {
        LogManager log; GnssConfig cfg;
        ASSERT_TRUE(loadWith("    \"gross_los_screen\": true,\n"
                             "    \"gross_los_cold_bound_m\": 12.0,\n", cfg, log, "re_inert"));
        NetworkRunContext ctx(cfg, cfg.getNetworkConfig());
    EpochEvents events;
        ctx.counters.gross_los.cold_los_screened = 57000;
        ctx.counters.gross_los.cold_v_under_max_m = 6.7;
        std::ostringstream oss;
        auto lg = captureLogger(oss);
        networkGrossLosColdBoundRunEnd(ctx, cfg, lg);
        lg->flush();
        EXPECT_NE(oss.str().find("INERT"), std::string::npos);
        EXPECT_NE(oss.str().find("ZERO OPPORTUNITY"), std::string::npos);
    }
    {
        LogManager log; GnssConfig cfg;
        ASSERT_TRUE(loadWith("    \"gross_los_screen\": true,\n"
                             "    \"gross_los_cold_bound_m\": 12.0,\n", cfg, log, "re_on"));
        NetworkRunContext ctx(cfg, cfg.getNetworkConfig());
    EpochEvents events;
        auto& g = ctx.counters.gross_los;
        g.cold_los_screened = 57000;
        g.cold_los_over = 8;
        g.cold_declared = 1;
        g.cold_declared_by_los["BSHM00ISR:G07"] = 1;
        std::ostringstream oss;
        auto lg = captureLogger(oss);
        networkGrossLosColdBoundRunEnd(ctx, cfg, lg);
        lg->flush();
        EXPECT_EQ(oss.str().find("INERT"), std::string::npos);
        EXPECT_NE(oss.str().find("BSHM00ISR:G07=1"), std::string::npos);
    }
    {   // set but resolved off: a warning, so the run is not read as an armed null
        LogManager log; GnssConfig cfg;
        ASSERT_TRUE(loadWith("    \"gross_los_cold_bound_m\": 12.0,\n", cfg, log, "re_miswired"));
        NetworkRunContext ctx(cfg, cfg.getNetworkConfig());
    EpochEvents events;
        std::ostringstream oss;
        auto lg = captureLogger(oss);
        networkGrossLosColdBoundRunEnd(ctx, cfg, lg);
        lg->flush();
        EXPECT_NE(oss.str().find("INERT"), std::string::npos);
        EXPECT_NE(oss.str().find("resolved off"), std::string::npos);
    }
}
