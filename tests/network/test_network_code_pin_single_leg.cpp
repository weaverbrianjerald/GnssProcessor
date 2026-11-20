/**
 * @file test_network_code_pin_single_leg.cpp
 * @brief The single-leg hold of the time-linked ionosphere-free code pin
 *        (@c network.code_pin_single_leg_hold).
 *
 * A satellite pinned on a dual-frequency pair whose registered code signals drop to one
 * frequency (one leg of the pair below two stations network-wide) takes, without the arm,
 * the all-ones row at target 0 in @c NetworkSbasis::buildCodeBiasDatumRows.  The surviving
 * column is not estimable against the satellite clock while the pair has one leg, so the
 * row pulls it toward zero at the gauge weight every epoch and it walks; at the return the
 * pair re-anchors against the moved column.  The arm holds the surviving column at its
 * pre-edge value (the decoupling edge), and at the return of the parked leg on the same
 * pair the time links target the pair's pre-edge combinations (the re-coupling edge).
 *
 * These cases pin: nothing changes with the key off or without the time link; the leg loss
 * opens the edge and writes one row on the surviving column at its pre-edge value; an open
 * edge keeps its target; the unarmed walk follows its rate law, step = x w / (w + 1/var);
 * the surviving column stays within the bound the row allows over 60 epochs of a biased
 * residual, while the unarmed row walks it; the return closes the
 * edge with no re-seed and both time links on the pre-edge combinations; a whole-satellite
 * departure does not open an edge; the kept-epoch site declares each edge once and
 * persists the open ones; the loader and the run-end line.
 *
 * Suite name @c NetworkCodePinSingleLeg is owned by this file alone.
 */

#include <gtest/gtest.h>

#include <Eigen/Dense>

#include <spdlog/spdlog.h>
#include <spdlog/sinks/ostream_sink.h>

#include <cmath>
#include <filesystem>
#include <fstream>
#include <map>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "common/GnssIdentity.hpp"
#include "config/GnssConfig.hpp"
#include "config/GnssConfigTypes.hpp"
#include "logging/LogManager.hpp"
#include "network/assembly/ContributionKind.hpp"
#include "network/driver/Epoch.hpp"   // EpochEvents
#include "network/qc/PriorResidual.hpp"
#include "network/solve/Estimator.hpp"
#include "network/solve/Sbasis.hpp"
#include "network/state/Entities.hpp"
#include "network/state/RunContext.hpp"
#include "../support/TestTempDir.hpp"   // per-process, per-test scratch directory

using namespace GnssProcessor;
namespace fs = std::filesystem;

namespace {

using Pair = std::pair<std::string, std::string>;
using Step = NetworkSbasis::CodePinLegStep;

constexpr double kE1  = 1575.42e6;
constexpr double kE5a = 1176.45e6;

double freqOf(const std::string& sat, const std::string& sig) {
    const std::optional<SignalId> id = SignalId::fromObsCode(systemOfSat(sat), sig);
    if (!id) return 0.0;
    switch (id->digit) {
        case '1': return kE1;
        case '5': return kE5a;
        default:  return 0.0;
    }
}

double a1() { return kE1 * kE1 / (kE1 * kE1 - kE5a * kE5a); }
double a2() { return -kE5a * kE5a / (kE1 * kE1 - kE5a * kE5a); }
double mu2MinusMu1() { return (kE1 * kE1) / (kE5a * kE5a) - 1.0; }

// The simulator case of the day program (E04 on the 60-station bed): pre-edge values.
constexpr double kHeldC1C   = -1.745022;
constexpr double kParkedC5Q = -3.129296;

/// The builder's inputs and side channels, cleared before each call.
struct Harness {
    bool cb_time_linked{true};
    NetworkSatGfDatum sat_gf_datum{NetworkSatGfDatum::NoneMode};
    bool sat_gf_datum_time_linked{false};
    NetworkSbasis::CodePinLegHoldArm leg;

    std::map<std::string, std::map<std::string, double>> code_bias_prev;
    std::map<std::string, Pair>                          code_bias_ifpin_pair;
    std::set<std::string>                                code_bias_sat_seen;

    std::map<std::string, Pair> if_pin_sel, if_pin_real, cb_ifpin;
    std::set<std::string>       cb_reseed;

    NetworkSbasis::CodeBiasDatumRows build(const NetworkEstimator& est,
                                           const std::vector<Pair>& keys) {
        if_pin_sel.clear(); if_pin_real.clear(); cb_ifpin.clear(); cb_reseed.clear();
        const std::set<std::string> none;
        const std::map<std::string, double> no_targets;
        return NetworkSbasis::buildCodeBiasDatumRows(
            est, keys, freqOf, cb_time_linked, /*emit_clock_disc=*/false,
            sat_gf_datum, no_targets, sat_gf_datum_time_linked, /*sat_isc_state=*/false, none,
            /*sat_gf_extra_band_free=*/false, none,
            code_bias_prev, code_bias_ifpin_pair, code_bias_sat_seen,
            /*code_bias_datum_resumed=*/false,
            if_pin_sel, if_pin_real, cb_ifpin, cb_reseed,
            NetworkSbasis::CodeDatumFamilyArms{}, NetworkSbasis::CodeBiasCarryMap{}, leg);
    }
};

std::vector<Pair> registerRoster(NetworkEstimator& est,
                                 const std::vector<std::pair<std::string, std::vector<std::string>>>& r) {
    std::vector<Pair> keys;
    for (const auto& [sat, sigs] : r)
        for (const auto& s : sigs) {
            est.registerCodeBias(sat, s);
            keys.emplace_back(sat, s);
        }
    return keys;
}

/// The last kept epoch: E04 and E05 both pinned on (C1C, C5Q), both veterans.
void lastKeptBothLegs(Harness& h) {
    h.code_bias_prev["E|C1C"] = {{"E04", kHeldC1C}, {"E05", 0.31}};
    h.code_bias_prev["E|C5Q"] = {{"E04", kParkedC5Q}, {"E05", 0.72}};
    h.code_bias_ifpin_pair = {{"E04", {"C1C", "C5Q"}}, {"E05", {"C1C", "C5Q"}}};
    h.code_bias_sat_seen = {"E04", "E05"};
}

/// The ionosphere-free pin row (kind CbIfPin) touching column @p col, or -1.
Eigen::Index ifPinRow(const NetworkSbasis::CodeBiasDatumRows& rows, Eigen::Index col) {
    for (Eigen::Index r = 0; r < rows.H.rows(); ++r)
        if (rows.row_kinds[static_cast<std::size_t>(r)] == ContributionKind::CbIfPin
            && std::abs(rows.H(r, col)) > 1e-12)
            return r;
    return -1;
}

/// The geometry-free pair row (kind CbSatGfPair) touching column @p col, or -1.
Eigen::Index gfRow(const NetworkSbasis::CodeBiasDatumRows& rows, Eigen::Index col) {
    for (Eigen::Index r = 0; r < rows.H.rows(); ++r)
        if (rows.row_kinds[static_cast<std::size_t>(r)] == ContributionKind::CbSatGfPair
            && std::abs(rows.H(r, col)) > 1e-12)
            return r;
    return -1;
}

std::string networkConfig(const std::string& extra) {
    return std::string(
        "{\n"
        "  \"processing\": {\n"
        "    \"mode\": \"ppp-rtk-network\",\n"
        "    \"active_systems\": [ \"GPS\" ],\n"
        "    \"signal_priorities\": { \"GPS\": \"WC\" }\n"
        "  },\n"
        "  \"estimation\": { \"estimate_ifb\": true },\n"
        "  \"network\": {\n"
        "    \"clock_datum_station\": \"AAAA\",\n")
        + extra +
        "    \"stations\": [\n"
        "      { \"name\": \"AAAA\", \"position_ecef\": [ -2493304.99, -4655214.79, 3565497.67 ] },\n"
        "      { \"name\": \"BBBB\", \"position_ecef\": [ -2281621.75, -1453585.08, 5756964.89 ] }\n"
        "    ]\n"
        "  }\n"
        "}\n";
}

bool loadWith(const std::string& extra, GnssConfig& cfg, LogManager& log, const std::string& tag) {
    const fs::path p = gnss_test::tempDir() / ("cpsl_cfg_" + tag + ".json");
    {
        std::ofstream f(p);
        f << networkConfig(extra);
    }
    const bool ok = cfg.loadFromJson(p, log);
    std::error_code ec;
    fs::remove(p, ec);
    return ok;
}

std::shared_ptr<spdlog::logger> captureLogger(std::ostringstream& oss, const std::string& name) {
    auto sink = std::make_shared<spdlog::sinks::ostream_sink_mt>(oss);
    return std::make_shared<spdlog::logger>(name, sink);
}

struct Event {
    std::string tier, event, sat, action, detail;
    std::optional<double> statistic;
};

}  // namespace

// Key off, or armed without the time link: the same rows, targets and side channels as the
// unarmed builder, and no record.
TEST(NetworkCodePinSingleLeg, KeyOffWritesNothing) {
    NetworkEstimator est;
    const auto keys = registerRoster(est, {{"E04", {"C1C"}}, {"E05", {"C1C", "C5Q"}}});
    Harness off;
    lastKeptBothLegs(off);
    const auto r_off = off.build(est, keys);

    Harness unlinked = off;
    unlinked.leg.armed = true;
    unlinked.cb_time_linked = false;
    Harness off_unlinked = off;
    off_unlinked.cb_time_linked = false;
    const auto r_unlinked = unlinked.build(est, keys);
    const auto r_off_unlinked = off_unlinked.build(est, keys);

    EXPECT_TRUE(r_off.pin_leg.empty());
    EXPECT_TRUE(r_unlinked.pin_leg.empty());
    ASSERT_EQ(r_unlinked.H.rows(), r_off_unlinked.H.rows());
    EXPECT_TRUE(r_unlinked.H == r_off_unlinked.H);
    EXPECT_TRUE(r_unlinked.z == r_off_unlinked.z);
    EXPECT_EQ(unlinked.cb_ifpin, off_unlinked.cb_ifpin);
    // The unarmed single-frequency row: all ones on the surviving column, target 0.
    const Eigen::Index c1 = est.codeBiasColumn("E04", "C1C");
    const Eigen::Index r = ifPinRow(r_off, c1);
    ASSERT_GE(r, 0);
    EXPECT_DOUBLE_EQ(r_off.H(r, c1), 1.0);
    EXPECT_DOUBLE_EQ(r_off.z(r), 0.0);
    EXPECT_EQ(off.cb_ifpin.count("E04"), 0u);
}

// The decoupling edge: one leg of the last kept pair left, both had values.  One row on the
// surviving column at its pre-edge value, the pair record kept, no re-seed, record Opened.
TEST(NetworkCodePinSingleLeg, LegLossOpensTheEdgeAndWritesTheRow) {
    NetworkEstimator est;
    const auto keys = registerRoster(est, {{"E04", {"C1C"}}, {"E05", {"C1C", "C5Q"}}});
    Harness h;
    lastKeptBothLegs(h);
    h.leg.armed = true;
    const auto rows = h.build(est, keys);

    ASSERT_EQ(rows.pin_leg.size(), 1u);
    const NetworkSbasis::CodePinLegRecord& rec = rows.pin_leg.at("E04");
    EXPECT_EQ(rec.step, Step::Opened);
    EXPECT_EQ(rec.edge.surviving, "C1C");
    EXPECT_EQ(rec.edge.parked, "C5Q");
    EXPECT_DOUBLE_EQ(rec.edge.held_m, kHeldC1C);
    EXPECT_DOUBLE_EQ(rec.edge.parked_m, kParkedC5Q);
    EXPECT_EQ(rec.edge.pair, Pair("C1C", "C5Q"));

    const Eigen::Index c1 = est.codeBiasColumn("E04", "C1C");
    const Eigen::Index r = ifPinRow(rows, c1);
    ASSERT_GE(r, 0);
    EXPECT_DOUBLE_EQ(rows.H(r, c1), 1.0);
    EXPECT_DOUBLE_EQ(rows.H.row(r).cwiseAbs().sum(), 1.0);   // the surviving column alone
    EXPECT_DOUBLE_EQ(rows.z(r), kHeldC1C);
    EXPECT_EQ(h.cb_ifpin.at("E04"), Pair("C1C", "C5Q"));
    EXPECT_EQ(h.cb_reseed.count("E04"), 0u);
    EXPECT_EQ(h.if_pin_real.count("E04"), 0u);   // no pair realized: not declared
}

// An open edge keeps its pre-edge target although the column's previous value moved.
TEST(NetworkCodePinSingleLeg, AnOpenEdgeKeepsItsTarget) {
    NetworkEstimator est;
    const auto keys = registerRoster(est, {{"E04", {"C1C"}}, {"E05", {"C1C", "C5Q"}}});
    Harness h;
    lastKeptBothLegs(h);
    h.leg.armed = true;
    h.code_bias_prev["E|C5Q"].erase("E04");      // the harvest dropped the parked leg
    h.code_bias_prev["E|C1C"]["E04"] = kHeldC1C + 0.004;
    h.leg.open["E04"] = NetworkSbasis::CodePinLegEdge{"C1C", "C5Q", kHeldC1C, kParkedC5Q,
                                                      {"C1C", "C5Q"}};
    const auto rows = h.build(est, keys);
    ASSERT_EQ(rows.pin_leg.at("E04").step, Step::Held);
    const Eigen::Index r = ifPinRow(rows, est.codeBiasColumn("E04", "C1C"));
    ASSERT_GE(r, 0);
    EXPECT_DOUBLE_EQ(rows.z(r), kHeldC1C);
}

// Over 60 epochs of a biased residual on the surviving column (b = 5 cm at weight wb), the
// held column stays inside the bound the row allows, b * wb / (w + wb), while the unarmed
// row walks it toward zero.  The recursion folds the builder's own row each epoch into a
// stiff scalar prior (2 mm, the observed formal sigma).
TEST(NetworkCodePinSingleLeg, TheHeldColumnStaysInsideTheRowsBound) {
    NetworkEstimator est;
    const auto keys = registerRoster(est, {{"E04", {"C1C"}}, {"E05", {"C1C", "C5Q"}}});
    const Eigen::Index c1 = est.codeBiasColumn("E04", "C1C");
    const double w = NetworkSbasis::kCodeBiasGaugeWeight;
    const double b = 0.05, wb = 20.0 / (0.3 * 0.3);   // 20 stations at 0.3 m code
    const auto run = [&](bool armed) {
        Harness h;
        lastKeptBothLegs(h);
        h.leg.armed = armed;
        double x = kHeldC1C, var = 0.002 * 0.002;
        for (int k = 0; k < 60; ++k) {
            const auto rows = h.build(est, keys);
            const Eigen::Index r = ifPinRow(rows, c1);
            EXPECT_GE(r, 0);
            const double hz = rows.H(r, c1), z = rows.z(r);
            const double info = 1.0 / var + w * hz * hz + wb;
            x = (x / var + w * hz * z + wb * (kHeldC1C + b)) / info;
            var = 1.0 / info + 1e-9;   // the carried prior with a small process noise
            // The harvest of this kept epoch.
            h.code_bias_prev["E|C1C"]["E04"] = x;
            h.code_bias_prev["E|C5Q"].erase("E04");
            h.code_bias_ifpin_pair = h.cb_ifpin;
            h.code_bias_ifpin_pair["E05"] = {"C1C", "C5Q"};
            h.leg.open.clear();
            for (const auto& [sat, rec] : rows.pin_leg)
                if (rec.step == Step::Opened || rec.step == Step::Held) h.leg.open[sat] = rec.edge;
        }
        return x;
    };
    const double held = run(true);
    const double walked = run(false);
    EXPECT_LE(std::abs(held - kHeldC1C), b * wb / (w + wb) + 1e-9);
    EXPECT_GT(std::abs(walked - kHeldC1C), 0.10);   // the unarmed row walks it by decimetres
}

// The rate law of the unarmed walk: with one leg left and nothing else informing the
// surviving column, folding the all-ones row at target 0 into the carried prior moves the
// column toward zero by step = x * w / (w + 1/var) each epoch, where var = sigma_prev^2 + q*dt
// is the predicted prior variance (q = 1e-10 m^2/s, dt = 30 s, the code-bias carry).  The
// observed DOY 146 steps sit at 1.0025 to 1.0062 times this law (964 of 964 toward zero);
// the recursion here, with no data rows, reproduces it exactly.  Armed, the step is zero.
TEST(NetworkCodePinSingleLeg, TheUnarmedWalkFollowsTheRateLaw) {
    NetworkEstimator est;
    const auto keys = registerRoster(est, {{"E04", {"C1C"}}, {"E05", {"C1C", "C5Q"}}});
    const Eigen::Index c1 = est.codeBiasColumn("E04", "C1C");
    const double w = NetworkSbasis::kCodeBiasGaugeWeight;
    ASSERT_DOUBLE_EQ(w, 1.0e3);
    const double q = 1e-10, dt = 30.0;
    const auto run = [&](bool armed, std::vector<double>& steps) {
        Harness h;
        lastKeptBothLegs(h);
        h.leg.armed = armed;
        double x = kHeldC1C, sigma2 = 0.0051 * 0.0051;   // the simulator's carried sigma
        for (int k = 0; k < 60; ++k) {
            const auto rows = h.build(est, keys);
            const Eigen::Index r = ifPinRow(rows, c1);
            EXPECT_GE(r, 0);
            const double var = sigma2 + q * dt;
            const double hz = rows.H(r, c1), z = rows.z(r);
            const double info = 1.0 / var + w * hz * hz;
            const double x_new = (x / var + w * hz * z) / info;
            if (!armed) {
                const double law = x * w / (w + 1.0 / var);
                EXPECT_NEAR(x - x_new, law, 1e-12 * std::abs(law) + 1e-15) << "epoch " << k;
            }
            steps.push_back(x - x_new);
            x = x_new;
            sigma2 = 1.0 / info;
            h.code_bias_prev["E|C1C"]["E04"] = x;
            h.code_bias_prev["E|C5Q"].erase("E04");
            h.code_bias_ifpin_pair = h.cb_ifpin;
            h.code_bias_ifpin_pair["E05"] = {"C1C", "C5Q"};
            h.leg.open.clear();
            for (const auto& [sat, rec] : rows.pin_leg)
                if (rec.step == Step::Opened || rec.step == Step::Held) h.leg.open[sat] = rec.edge;
        }
        return x;
    };
    std::vector<double> off_steps, on_steps;
    const double walked = run(false, off_steps);
    const double held = run(true, on_steps);
    // First step at the simulator's 5.1 mm sigma: 2.5 % of the value (the 2.4 % seen on E04).
    EXPECT_NEAR(off_steps.front() / kHeldC1C, 0.0254, 0.0005);
    for (const double s : off_steps) EXPECT_LT(s, 0.0);   // every step toward zero (x < 0)
    EXPECT_GT(walked, kHeldC1C);
    EXPECT_LT(walked, 0.0);
    for (const double s : on_steps) EXPECT_NEAR(s, 0.0, 1e-12);   // rounding of the fold only
    EXPECT_NEAR(held, kHeldC1C, 1e-10);
}

// The re-coupling edge: the parked leg back on the same pair with no previous value of its
// own.  The ionosphere-free pin and the geometry-free time link target the pre-edge
// combinations, nothing is re-seeded, and the record is Recoupled.  Without an open edge the
// same return re-seeds the pin at 0 and re-anchors the geometry-free row.
TEST(NetworkCodePinSingleLeg, TheReturnRecouplesWithoutAReseed) {
    NetworkEstimator est;
    const auto keys = registerRoster(est, {{"E04", {"C1C", "C5Q"}}, {"E05", {"C1C", "C5Q"}}});
    const Eigen::Index c1 = est.codeBiasColumn("E04", "C1C");
    const Eigen::Index c5 = est.codeBiasColumn("E04", "C5Q");
    const auto returning = [&](bool open_edge) {
        Harness h;
        lastKeptBothLegs(h);
        h.sat_gf_datum = NetworkSatGfDatum::Zero;
        h.sat_gf_datum_time_linked = true;
        h.leg.armed = true;
        h.code_bias_prev["E|C5Q"].erase("E04");
        if (open_edge)
            h.leg.open["E04"] = NetworkSbasis::CodePinLegEdge{"C1C", "C5Q", kHeldC1C, kParkedC5Q,
                                                              {"C1C", "C5Q"}};
        else
            h.code_bias_ifpin_pair.erase("E04");   // the unarmed hold-less path: no record kept
        auto rows = h.build(est, keys);
        return std::make_pair(std::move(rows), h);
    };
    const auto [on, h_on] = returning(true);
    ASSERT_EQ(on.pin_leg.at("E04").step, Step::Recoupled);
    const Eigen::Index r = ifPinRow(on, c1);
    ASSERT_GE(r, 0);
    EXPECT_NEAR(on.z(r), a1() * kHeldC1C + a2() * kParkedC5Q, 1e-12);
    EXPECT_EQ(h_on.cb_reseed.count("E04"), 0u);
    const Eigen::Index g = gfRow(on, c5);
    ASSERT_GE(g, 0);
    EXPECT_NEAR(on.z(g), (kParkedC5Q - kHeldC1C) / mu2MinusMu1(), 1e-12);
    EXPECT_EQ(on.n_sat_gf_reanchored, 0);

    const auto [off, h_off] = returning(false);
    EXPECT_TRUE(off.pin_leg.empty());
    const Eigen::Index r0 = ifPinRow(off, c1);
    ASSERT_GE(r0, 0);
    EXPECT_DOUBLE_EQ(off.z(r0), 0.0);
    EXPECT_EQ(h_off.cb_reseed.count("E04"), 1u);
    EXPECT_EQ(off.n_sat_gf_reanchored, 1);
}

// A return on another pair, or the surviving leg leaving too, abandons the edge and the
// unarmed path runs.
TEST(NetworkCodePinSingleLeg, OtherEndingsAbandonTheEdge) {
    NetworkEstimator est;
    const NetworkSbasis::CodePinLegEdge edge{"C1C", "C5Q", kHeldC1C, kParkedC5Q, {"C1C", "C5Q"}};
    {
        const auto keys = registerRoster(est, {{"E04", {"C5Q"}}, {"E05", {"C1C", "C5Q"}}});
        Harness h;
        lastKeptBothLegs(h);
        h.leg.armed = true;
        h.leg.open["E04"] = edge;
        const auto rows = h.build(est, keys);
        ASSERT_EQ(rows.pin_leg.at("E04").step, Step::Abandoned);
        const Eigen::Index r = ifPinRow(rows, est.codeBiasColumn("E04", "C5Q"));
        ASSERT_GE(r, 0);
        EXPECT_DOUBLE_EQ(rows.z(r), 0.0);   // the all-ones row
    }
    {
        NetworkEstimator est2;
        const auto keys = registerRoster(est2, {{"E04", {"C1X", "C5Q"}}, {"E05", {"C1C", "C5Q"}}});
        Harness h;
        lastKeptBothLegs(h);
        h.leg.armed = true;
        h.leg.open["E04"] = edge;
        const auto rows = h.build(est2, keys);
        ASSERT_EQ(rows.pin_leg.at("E04").step, Step::Abandoned);
        EXPECT_NE(rows.pin_leg.at("E04").why.find("C1X+C5Q"), std::string::npos);
    }
}

// A whole-satellite departure opens no edge, and a satellite returning on one leg after an
// absence (no pair record of the last kept epoch) opens none either.
TEST(NetworkCodePinSingleLeg, AWholeSatelliteDepartureDoesNotOpenIt) {
    NetworkEstimator est;
    const auto keys = registerRoster(est, {{"E05", {"C1C", "C5Q"}}});
    Harness h;
    lastKeptBothLegs(h);
    h.leg.armed = true;
    EXPECT_TRUE(h.build(est, keys).pin_leg.empty());

    NetworkEstimator est2;
    const auto keys2 = registerRoster(est2, {{"E04", {"C1C"}}, {"E05", {"C1C", "C5Q"}}});
    Harness back;
    lastKeptBothLegs(back);
    back.leg.armed = true;
    back.code_bias_ifpin_pair.erase("E04");
    EXPECT_TRUE(back.build(est2, keys2).pin_leg.empty());
}

// The kept-epoch site: one CB_PIN event per edge (open, recouple with the census verdict
// of the returning column, abandon on departure), the counters, and the open edges
// persisted.  The re-coupling verdict is the prior-residual census row, not a distance: a
// 20 cm move the census does not report is "kept", a census row above its level is
// "moved" whatever its size in metres, and no census is "untested".
TEST(NetworkCodePinSingleLeg, KeptEpochDeclaresBothEdgesOnce) {
    LogManager logman;
    GnssConfig cfg;
    ASSERT_TRUE(loadWith("", cfg, logman, "kept"));
    NetworkRunContext ctx(cfg, cfg.getNetworkConfig());
    EpochEvents ev;
    std::vector<Event> events;
    const auto sink = [&](const char* tier, const char* event, const std::string& sat,
                          const std::string&, std::optional<double> stat, std::optional<double>,
                          const char* action, const std::string& detail) {
        events.push_back({tier, event, sat, action, detail, stat});
    };
    NetworkEstimator est;
    est.registerCodeBias("E04", "C1C");
    est.registerCodeBias("E04", "C5Q");
    Eigen::VectorXd x = Eigen::VectorXd::Zero(est.numShared());
    const NetworkSbasis::CodePinLegEdge edge{"C1C", "C5Q", kHeldC1C, kParkedC5Q, {"C1C", "C5Q"}};

    NetworkPriorResidual census({"cb"}, /*window_epochs=*/0.0, /*min_rows=*/0.0, /*report_sigma=*/4.0);
    NetworkPriorResidual no_census({}, 0.0);
    census.beginEpoch(0.0, 0.0);

    networkCodePinLegKeptEpoch(ctx, sink, ev, est, x, census);   // nothing open, no record
    EXPECT_TRUE(events.empty());

    ev.code_datum.this_epoch_cb_pin_leg = {{"E04", {edge, Step::Opened, ""}}};
    networkCodePinLegKeptEpoch(ctx, sink, ev, est, x, census);
    ASSERT_EQ(events.size(), 1u);
    EXPECT_EQ(events[0].tier, "CB_PIN");
    EXPECT_EQ(events[0].event, "single_leg");
    EXPECT_EQ(events[0].action, "open");
    EXPECT_EQ(events[0].sat, "E04");
    ASSERT_NE(net::getSat(ctx.entities.sats, "E04", &net::SatelliteState::code_pin_leg_edge), nullptr);

    ev.code_datum.this_epoch_cb_pin_leg = {{"E04", {edge, Step::Held, ""}}};
    networkCodePinLegKeptEpoch(ctx, sink, ev, est, x, census);
    EXPECT_EQ(events.size(), 1u);   // a held epoch declares nothing

    // The return moved 20 cm and the census, at a loose dispersion, does not report it: kept.
    x(est.codeBiasColumn("E04", "C5Q")) = kParkedC5Q + 0.20;
    ev.code_datum.this_epoch_cb_pin_leg = {{"E04", {edge, Step::Recoupled, ""}}};
    NetworkPriorResidual::Input loose;
    loose.family = "cb"; loose.sat = "E04"; loose.signal = "C5Q";
    loose.e = 0.20; loose.fold_var = 1.0; loose.post_var = 0.5;
    census.addRow(loose);
    networkCodePinLegKeptEpoch(ctx, sink, ev, est, x, census);
    ASSERT_EQ(events.size(), 2u);
    EXPECT_EQ(events[1].action, "recouple");
    EXPECT_FALSE(events[1].statistic.has_value());
    EXPECT_NE(events[1].detail.find("level kept"), std::string::npos);
    EXPECT_EQ(net::getSat(ctx.entities.sats, "E04", &net::SatelliteState::code_pin_leg_edge), nullptr);

    // A 2 cm move at a tight dispersion is reported by the census: moved.
    census.beginEpoch(30.0, 30.0);
    x(est.codeBiasColumn("E04", "C5Q")) = kParkedC5Q + 0.02;
    NetworkPriorResidual::Input tight = loose;
    tight.e = 0.02; tight.fold_var = 2.0e-5; tight.post_var = 1.0e-5;
    census.addRow(tight);
    networkCodePinLegKeptEpoch(ctx, sink, ev, est, x, census);
    ASSERT_EQ(events.size(), 3u);
    ASSERT_TRUE(events[2].statistic.has_value());
    EXPECT_GT(std::abs(*events[2].statistic), 4.0);
    EXPECT_NE(events[2].detail.find("level moved"), std::string::npos);

    // No census: untested, never "kept".
    networkCodePinLegKeptEpoch(ctx, sink, ev, est, x, no_census);
    ASSERT_EQ(events.size(), 4u);
    EXPECT_NE(events[3].detail.find("level untested"), std::string::npos);

    // An edge open at the last kept epoch whose satellite has no record now: departed.
    ev.code_datum.this_epoch_cb_pin_leg = {{"E04", {edge, Step::Opened, ""}}};
    networkCodePinLegKeptEpoch(ctx, sink, ev, est, x, census);
    ev.code_datum.this_epoch_cb_pin_leg.clear();
    networkCodePinLegKeptEpoch(ctx, sink, ev, est, x, census);
    ASSERT_EQ(events.size(), 6u);
    EXPECT_EQ(events[5].action, "abandon");
    EXPECT_NE(events[5].detail.find("departed"), std::string::npos);

    const NetworkSbasis::CodePinLegCounts& c = ctx.counters.cb_pin_leg;
    EXPECT_EQ(c.open, 2);
    EXPECT_EQ(c.held_epochs, 3);
    EXPECT_EQ(c.recouple, 3);
    EXPECT_EQ(c.recouple_moved, 1);
    EXPECT_EQ(c.recouple_untested, 1);
    EXPECT_EQ(c.abandoned, 1);
    EXPECT_NEAR(c.max_recouple_move_m, 0.20, 1e-9);
    EXPECT_GT(c.max_recouple_t, 4.0);
}

// The loader reads the key (default off), and the run-end line: nothing when off, INERT
// without the time link, a warning with no edge, the counts otherwise.
TEST(NetworkCodePinSingleLeg, LoaderAndRunEndLine) {
    {
        LogManager log; GnssConfig cfg;
        ASSERT_TRUE(loadWith("", cfg, log, "off"));
        EXPECT_FALSE(cfg.networkCodePinSingleLegHold());
        NetworkRunContext ctx(cfg, cfg.getNetworkConfig());
        std::ostringstream oss;
        auto lg = captureLogger(oss, "cpsl_off");
        networkCodePinLegRunEnd(ctx, cfg, lg);
        lg->flush();
        EXPECT_TRUE(oss.str().empty());
    }
    {
        LogManager log; GnssConfig cfg;
        ASSERT_TRUE(loadWith("    \"estimate_sat_code_bias\": true,\n"
                             "    \"code_pin_single_leg_hold\": true,\n", cfg, log, "inert"));
        EXPECT_TRUE(cfg.networkCodePinSingleLegHold());
        NetworkRunContext ctx(cfg, cfg.getNetworkConfig());
        std::ostringstream oss;
        auto lg = captureLogger(oss, "cpsl_inert");
        networkCodePinLegRunEnd(ctx, cfg, lg);
        lg->flush();
        EXPECT_NE(oss.str().find("INERT without network.code_datum_time_linked."), std::string::npos);
    }
    {
        LogManager log; GnssConfig cfg;
        ASSERT_TRUE(loadWith("    \"estimate_sat_code_bias\": true,\n"
                             "    \"code_datum_time_linked\": true,\n"
                             "    \"code_pin_single_leg_hold\": true,\n", cfg, log, "armed"));
        NetworkRunContext ctx(cfg, cfg.getNetworkConfig());
        {
            std::ostringstream oss;
            auto lg = captureLogger(oss, "cpsl_zero");
            networkCodePinLegRunEnd(ctx, cfg, lg);
            lg->flush();
            EXPECT_NE(oss.str().find("ZERO decoupling edges"), std::string::npos);
        }
        ctx.counters.cb_pin_leg.open = 1;
        ctx.counters.cb_pin_leg.held_epochs = 60;
        ctx.counters.cb_pin_leg.recouple = 1;
        std::ostringstream oss;
        auto lg = captureLogger(oss, "cpsl_counts");
        networkCodePinLegRunEnd(ctx, cfg, lg);
        lg->flush();
        EXPECT_NE(oss.str().find("1 decoupling edge(s)"), std::string::npos);
        EXPECT_NE(oss.str().find("60 held satellite-epoch(s)"), std::string::npos);
    }
}
