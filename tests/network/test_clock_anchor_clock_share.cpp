/**
 * @file test_clock_anchor_clock_share.cpp
 * @brief The clock-share requirement on the clock anchor's phase-statistic trip
 *        (`network.clock_anchor_clock_k`).
 *
 * In phase mode the anchor guard trips an anchored satellite on its ionosphere-free phase
 * clock statistic w_phi.  That statistic moves when the clock moves and also when a phase-bias
 * key moves; the clock-space statistic w_clk computed beside it moves only with the clock.
 * With the requirement armed, a phase trip also needs |w_clk| >= clock_k, and a phase trip with
 * the clock still is withheld: the satellite stays anchored, and the withhold is an event and a
 * counter, with a closing event when the episode ends (standing invariants 4 and 7).
 *
 * Covered:
 *   - the verdict (`clockAnchorPhaseTripVerdict`): in band, trip, withheld, the boundary, the
 *     requirement off (the decision as before), a row without a statistic, a non-finite input;
 *     the keys' share of a phase statistic;
 *   - the loader: the key refused with the anchor off, outside phase mode (clock and shadow)
 *     and at the goldens' q (2.9e-8), each beside an accepting control; a negative value
 *     refused; the knob snapshot is 0 when the key is absent;
 *   - the guard on a synthetic network of clocks and two phase-bias bands with a dense prior:
 *     a shift carried by one phase-bias key alone (the clock still) trips without the
 *     requirement and is withheld with it, with its event row and counters; a true satellite
 *     clock step still trips with the requirement (standing invariant 1); the absolute
 *     backstop and the satellite clock event's forced trip are unchanged on a withheld row; a
 *     LOOSE row's re-validation is unchanged; an episode opens, extends and closes clear, or
 *     closes by a decoupling, each edge an event; the guard trace's decision column reads
 *     `withheld`.
 */

#include <gtest/gtest.h>

#include "network/state/RunContext.hpp"
#include "network/driver/Epoch.hpp"   // EpochEvents
#include "config/GnssConfig.hpp"
#include "network/solve/ClockAnchor.hpp"
#include "network/state/CarryDisposition.hpp"
#include "network/solve/Estimator.hpp"
#include "logging/LogManager.hpp"
#include "network/diag/RunTraces.hpp"
#include "math/TimeUtils.hpp"

#include <spdlog/spdlog.h>
#include <spdlog/sinks/null_sink.h>

#include <cmath>
#include <filesystem>
#include <fstream>
#include <limits>
#include <map>
#include <memory>
#include <set>
#include <sstream>
#include <string>
#include <vector>
#include "../support/TestTempDir.hpp"   // per-process, per-test scratch directory

namespace fs = std::filesystem;
using namespace GnssProcessor;

namespace {

// ---------------------------------------------------------------------------------------
// Configuration.
// ---------------------------------------------------------------------------------------

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
        "  \"filter\": {\n"
        "    \"process_noise\": {\n"
        "      \"sat_clock_q\": 1e-3\n"
        "    }\n"
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
    const fs::path p = gnss_test::tempDir() / ("cta_share_cfg_" + tag + ".json");
    std::ofstream f(p);
    f << body;
    return p;
}

/// A GnssConfig loaded from the network block @p extra; @c ok is the loader's verdict.
struct Loaded {
    LogManager log;
    GnssConfig cfg;
    fs::path   path;
    bool       ok{false};
    Loaded(const std::string& tag, const std::string& extra) {
        path = writeTempConfig(tag, extra);
        ok = cfg.loadFromJson(path, log);
    }
    ~Loaded() { std::error_code ec; fs::remove(path, ec); }
};

/// The anchor at the broadcast regime's q in phase mode; @p tail is appended (the key under
/// test, a changed backstop).
std::string armed(const std::string& tail, const std::string& q = "5e-6",
                  const std::string& space = "phase", bool anchor = true,
                  const std::string& abs_m = "2.0") {
    return std::string("    \"clock_temporal_anchor\": ") + (anchor ? "true" : "false") + ",\n"
           "    \"clock_anchor_q\": " + q + ",\n"
           "    \"clock_anchor_var_floor_m2\": 1e-6,\n"
           "    \"clock_anchor_max_gap_s\": 120.0,\n"
           "    \"clock_anchor_k\": 6.0,\n"
           "    \"clock_anchor_abs_m\": " + abs_m + ",\n"
           "    \"clock_anchor_sigma_floor_m\": 0.02,\n"
           "    \"clock_anchor_phase_sigma_floor_m\": 0.02,\n"
           "    \"clock_anchor_recouple_epochs\": 3,\n"
           "    \"clock_anchor_guard_space\": \"" + space + "\",\n" + tail;
}

const char* const kShare = "    \"clock_anchor_clock_k\": 2.0,\n";

std::shared_ptr<spdlog::logger> nullLogger() {
    static auto sink = std::make_shared<spdlog::sinks::null_sink_mt>();
    return std::make_shared<spdlog::logger>("cta_share_test", sink);
}

// ---------------------------------------------------------------------------------------
// A synthetic network: S satellites with a clock and L1C / L2W phase biases, R stations.
// Station locals: [rx clock, N1_0 .. N1_{S-1}, N2_0 .. N2_{S-1}].  Code rows on band 1
// (rx - clk, sd 0.1 m), phase rows on both bands (rx - clk + pb_j + N_j, sd 0.01 m); station 0
// pins its ambiguities, so the phase fixes pb_j - clk and the code separates the clock.
// ---------------------------------------------------------------------------------------

struct Truth {
    std::vector<double> clk{0.0, 1.0, -0.5, 2.0};
    std::vector<double> pb1{0.30, -0.20, 0.10, 0.40};
    std::vector<double> pb2{-0.10, 0.25, 0.05, -0.30};
    std::vector<double> rx{0.7, -1.3, 0.2, 2.1, -0.4};
};

struct Net {
    int S{4}, R{5};
    std::vector<std::string> sats{"G01", "G02", "G03", "G04"};
    std::vector<Eigen::Index> clk, pb1, pb2;
};

void stationRows(const Net& n, const Truth& t, int r, const NetworkEstimator& est,
                 Eigen::MatrixXd& A_s, Eigen::MatrixXd& A_r, Eigen::VectorXd& w,
                 Eigen::VectorXd& y) {
    const bool pivot = (r == 0);
    const Eigen::Index nl = 1 + 2 * n.S;
    const Eigen::Index m = 3 * n.S + (pivot ? 2 * n.S : 0);
    A_s = Eigen::MatrixXd::Zero(m, est.numShared());
    A_r = Eigen::MatrixXd::Zero(m, nl);
    w = Eigen::VectorXd::Ones(m);
    y = Eigen::VectorXd::Zero(m);
    const double rx = t.rx[static_cast<std::size_t>(r)];
    Eigen::Index row = 0;
    for (int s = 0; s < n.S; ++s) {
        const auto us = static_cast<std::size_t>(s);
        A_s(row, n.clk[us]) = -1.0;
        A_r(row, 0) = 1.0;
        w(row) = 100.0;
        y(row) = rx - t.clk[us];
        ++row;
        for (int j = 0; j < 2; ++j) {
            A_s(row, n.clk[us]) = -1.0;
            A_s(row, j == 0 ? n.pb1[us] : n.pb2[us]) = 1.0;
            A_r(row, 0) = 1.0;
            A_r(row, 1 + j * n.S + s) = 1.0;
            w(row) = 1e4;
            y(row) = rx - t.clk[us] + (j == 0 ? t.pb1[us] : t.pb2[us]);
            ++row;
        }
    }
    if (pivot)
        for (Eigen::Index l = 1; l < nl; ++l) {
            A_r(row, l) = 1.0;
            w(row) = 1e10;
            ++row;
        }
}

/// The carried prior of one epoch: a value and a variance per (satellite, key), keys in the
/// order clock, L1C, L2W.  Independent keys (a diagonal dense block).
struct Prior {
    std::vector<double> clk, pb1, pb2;
    std::vector<double> var_clk, var_pb1, var_pb2;
};

/// The prior at the truth: clock sd 3 cm, phase-bias keys sd 1 cm, except G03's L2W key whose
/// sd is 1 m (a key born or re-realized with a metre-level sd, card BRD3 section 5.2).
Prior priorAtTruth(const Truth& t) {
    Prior p{t.clk, t.pb1, t.pb2, {}, {}, {}};
    p.var_clk.assign(4, 9e-4);
    p.var_pb1.assign(4, 1e-4);
    p.var_pb2.assign(4, 1e-4);
    p.var_pb2[2] = 1.0;
    return p;
}

/// One guarded epoch: the anchor's fold for every satellite (anchored, or LOOSE), the dense
/// prior captured as the border fold captures it, the solve, the guard.
struct ShareFixture {
    Loaded            L;
    NetworkRunContext ctx;
    fs::path          dir;
    NetworkRunTraces  traces;
    Net               net;
    Truth             truth;
    NetworkEstimator  est;
    NetworkSbasis::Plan plan;
    double            now_s{1000.0};

    ShareFixture(const std::string& tag, const std::string& extra)
        : L(tag, extra), ctx(L.cfg, L.cfg.getNetworkConfig()),
          dir(gnss_test::tempDir() / ("cta_share_traces_" + tag)),
          traces((fs::create_directories(dir), dir)) {
        EXPECT_TRUE(L.ok);
        for (const auto& s : net.sats) {
            net.clk.push_back(est.registerClock(s));
            net.pb1.push_back(est.registerPhaseBias(s, "L1C"));
            net.pb2.push_back(est.registerPhaseBias(s, "L2W"));
        }
        traces.events_trace.open(traces.events_trace_path);
        traces.anchor_guard_trace.open(traces.anchor_guard_trace_path);
        // Every satellite anchored from an earlier engagement.
        clockAnchorBeginEpoch(ctx);
        for (const auto& s : net.sats) {
            ctx.carry.clk_anchor.sat[s].anchored = true;
            ctx.carry.clk_anchor.sat[s].ever_anchored = true;
        }
    }
    ~ShareFixture() { std::error_code ec; fs::remove_all(dir, ec); }

    /// One epoch on @p data (the observations' truth) against the carried @p prior.
    /// @p loose names satellites whose fold is LOOSE this epoch; @p events is the slip layer's.
    Eigen::VectorXd epoch(const Truth& data, const Prior& prior,
                          const std::set<std::string>& loose = {},
                          const EpochEvents& events = {}) {
        now_s += 30.0;
        clockAnchorBeginEpoch(ctx);
        std::vector<NetworkEstimator::SharedPrior> keys;
        std::vector<double> var;
        for (int s = 0; s < net.S; ++s) {
            const auto us = static_cast<std::size_t>(s);
            const std::string& sat = net.sats[us];
            ClockAnchorFold f;
            f.anchored = loose.count(sat) == 0;
            f.var_eff  = prior.var_clk[us];
            clockAnchorFoldCommit(ctx, sat, f, now_s, prior.clk[us]);
            const char* sig[3] = {"", "L1C", "L2W"};
            const SharedParamKind kind[3] = {SharedParamKind::Clock, SharedParamKind::PhaseBias,
                                             SharedParamKind::PhaseBias};
            const double val[3] = {prior.clk[us], prior.pb1[us], prior.pb2[us]};
            const double vv[3]  = {prior.var_clk[us], prior.var_pb1[us], prior.var_pb2[us]};
            for (int k = 0; k < 3; ++k) {
                NetworkEstimator::SharedPrior p;
                p.kind   = kind[k];
                p.sat    = sat;
                p.signal = sig[k];
                p.value  = val[k];
                p.info   = 1.0 / vv[k];
                keys.push_back(p);
                var.push_back(vv[k]);
            }
        }
        Eigen::MatrixXd cov = Eigen::MatrixXd::Zero(static_cast<Eigen::Index>(var.size()),
                                                    static_cast<Eigen::Index>(var.size()));
        for (std::size_t a = 0; a < var.size(); ++a)
            cov(static_cast<Eigen::Index>(a), static_cast<Eigen::Index>(a)) = var[a];
        est.beginEpoch();
        for (int r = 0; r < net.R; ++r) {
            Eigen::MatrixXd A_s, A_r;
            Eigen::VectorXd w, y;
            stationRows(net, data, r, est, A_s, A_r, w, y);
            est.addStation(A_s, A_r, w, y);
        }
        est.addSharedPriorDense(keys, cov);
        EpochWorkspace ws;
        ws.cap_dense_keys = keys;
        ws.cap_dense_cov  = cov;
        Eigen::VectorXd x = est.solve(plan.shared_datum);
        std::vector<std::string> reg(net.sats.begin(), net.sats.end());
        GnssTime tm;
        tm.week = 2300;
        tm.tow  = now_s;
        networkClockAnchorGuard(ctx, L.cfg, nullLogger(), traces, tm, est, plan, x, ws, reg, {},
                                false, {}, events);
        return x;
    }

    /// The rows of network_events.csv whose event column is @p event.
    std::vector<std::string> events(const std::string& event) {
        traces.events_trace.flush();
        std::ifstream f(traces.events_trace_path);
        std::vector<std::string> out;
        std::string line;
        while (std::getline(f, line))
            if (line.find("," + event + ",") != std::string::npos) out.push_back(line);
        return out;
    }

    /// The guard trace's decision column for @p sat (all rows, in order).
    std::vector<std::string> decisions(const std::string& sat) {
        traces.anchor_guard_trace.flush();
        std::ifstream f(traces.anchor_guard_trace_path);
        std::vector<std::string> out;
        std::string line;
        while (std::getline(f, line)) {
            std::vector<std::string> c;
            std::stringstream ss(line);
            std::string cell;
            while (std::getline(ss, cell, ',')) c.push_back(cell);
            if (c.size() > 11 && c[2] == sat) out.push_back(c[11]);
        }
        return out;
    }
};

/// G03's L2W key carried 20 m off the data (a metre-sd key's first innovation), the clock and
/// every other key at the truth: the phase statistic reads the key's move, the clock stays.
Prior keyShifted(const Truth& t) {
    Prior p = priorAtTruth(t);
    p.pb2[2] += 20.0;
    return p;
}

/// The prior at the truth with every phase-bias key tight (sd 1 cm): a clock step then moves
/// the phase statistic (with G03's metre-sd L2W key the step would hide in that key's variance).
Prior priorTight(const Truth& t) {
    Prior p = priorAtTruth(t);
    p.var_pb2[2] = 1e-4;
    return p;
}

/// The data with G03's clock stepped by @p step_m, the prior at the old truth.
Truth clockStepped(Truth t, double step_m) {
    t.clk[2] += step_m;
    return t;
}

}  // namespace

// --- the verdict ---------------------------------------------------------------------------

TEST(ClockAnchorClockShare, TheVerdictWithholdsAPhaseTripWithTheClockStill) {
    // Card BRD3 section 2.3: a key-carried trip of DOY 132 (|w_phi| 8.0, |w_clk| 0.57).
    EXPECT_EQ(clockAnchorPhaseTripVerdict(true, 8.0, 0.57, 6.0, 2.0),
              ClockAnchorPhaseVerdict::Withheld);
    EXPECT_EQ(clockAnchorPhaseTripVerdict(true, -8.0, -1.99, 6.0, 2.0),
              ClockAnchorPhaseVerdict::Withheld) << "both statistics are read in magnitude";
}

TEST(ClockAnchorClockShare, TheVerdictKeepsAClockCarriedTrip) {
    // The injected 0.5 m step at q 5e-6 reads w_clk -7.94 (card SCK's leg on posture c).
    EXPECT_EQ(clockAnchorPhaseTripVerdict(true, 7.0, -7.94, 6.0, 2.0),
              ClockAnchorPhaseVerdict::Trip);
    EXPECT_EQ(clockAnchorPhaseTripVerdict(true, 7.0, 2.0, 6.0, 2.0),
              ClockAnchorPhaseVerdict::Trip) << "the bar is |w_clk| >= clock_k";
}

TEST(ClockAnchorClockShare, TheVerdictOffIsTheDecisionWithoutTheRequirement) {
    for (double w_clk : {0.0, 0.57, 7.94})
        EXPECT_EQ(clockAnchorPhaseTripVerdict(true, 8.0, w_clk, 6.0, 0.0),
                  ClockAnchorPhaseVerdict::Trip) << w_clk;
    EXPECT_EQ(clockAnchorPhaseTripVerdict(true, 5.9, 9.0, 6.0, 0.0),
              ClockAnchorPhaseVerdict::InBand);
}

TEST(ClockAnchorClockShare, TheVerdictIsInBandWithoutAPhaseTrip) {
    EXPECT_EQ(clockAnchorPhaseTripVerdict(true, 5.9, 9.0, 6.0, 2.0),
              ClockAnchorPhaseVerdict::InBand) << "the clock statistic alone never trips here";
    EXPECT_EQ(clockAnchorPhaseTripVerdict(true, 6.0, 9.0, 6.0, 2.0),
              ClockAnchorPhaseVerdict::InBand) << "the phase bar is |w_phi| > k";
    EXPECT_EQ(clockAnchorPhaseTripVerdict(false, 100.0, 100.0, 6.0, 2.0),
              ClockAnchorPhaseVerdict::InBand) << "no phase statistic: the backstop decides";
    const double nan = std::numeric_limits<double>::quiet_NaN();
    EXPECT_EQ(clockAnchorPhaseTripVerdict(true, nan, 9.0, 6.0, 2.0),
              ClockAnchorPhaseVerdict::InBand);
    EXPECT_EQ(clockAnchorPhaseTripVerdict(true, 8.0, nan, 6.0, 2.0),
              ClockAnchorPhaseVerdict::Withheld) << "a non-finite clock statistic carries no share";
}

TEST(ClockAnchorClockShare, TheKeysShareIsTheirPartOfTheMedianRemovedStatistic) {
    ClockAnchorPhiRow r;
    r.ok    = true;
    r.e_hat = 1.0;
    r.w     = {2.5457, -1.5457};
    r.dpb   = {0.0, 0.6};              // -w2 * dpb2 = +0.927
    EXPECT_NEAR(clockAnchorPhaseKeysShare(r), 0.92742, 1e-5);
    r.e_hat = 0.0;
    EXPECT_DOUBLE_EQ(clockAnchorPhaseKeysShare(r), 0.0);
    r.e_hat = 1.0;
    r.dpb.clear();
    EXPECT_DOUBLE_EQ(clockAnchorPhaseKeysShare(r), 0.0);
}

// --- the loader ----------------------------------------------------------------------------

TEST(ClockAnchorClockShare, TheLoaderAcceptsTheKeyAtTheBroadcastRegimesQInPhaseMode) {
    Loaded L("accept", armed(kShare));
    ASSERT_TRUE(L.ok);
    EXPECT_DOUBLE_EQ(L.cfg.networkClockAnchorClockK(), 2.0);
    NetworkRunContext ctx(L.cfg, L.cfg.getNetworkConfig());
    EXPECT_DOUBLE_EQ(ctx.knobs.cta_clock_k, 2.0);
}

TEST(ClockAnchorClockShare, TheLoaderRefusesTheKeyAtTheGoldensQ) {
    Loaded refused("q_golden", armed(kShare, "2.9e-8"));
    EXPECT_FALSE(refused.ok) << "at 2.9e-8 a 0.5 m step reads w_clk 0.15";
    Loaded control("q_golden_ctl", armed("", "2.9e-8"));
    EXPECT_TRUE(control.ok) << "the same posture without the key loads";
    Loaded edge("q_edge", armed(kShare, "1e-6"));
    EXPECT_TRUE(edge.ok) << "the bar is q >= 1e-6";
}

TEST(ClockAnchorClockShare, TheLoaderRefusesTheKeyOutsidePhaseMode) {
    Loaded clock("space_clock", armed(kShare, "5e-6", "clock"));
    EXPECT_FALSE(clock.ok);
    Loaded shadow("space_shadow", armed(kShare, "5e-6", "phase_shadow"));
    EXPECT_FALSE(shadow.ok);
    Loaded control("space_clock_ctl", armed("", "5e-6", "clock"));
    EXPECT_TRUE(control.ok);
}

TEST(ClockAnchorClockShare, TheLoaderRefusesTheKeyWithTheAnchorOff) {
    Loaded refused("anchor_off", armed(kShare, "5e-6", "phase", false));
    EXPECT_FALSE(refused.ok);
    Loaded control("anchor_off_ctl", armed("", "5e-6", "phase", false));
    EXPECT_TRUE(control.ok);
}

TEST(ClockAnchorClockShare, TheLoaderRefusesANegativeValueAndDefaultsToOff) {
    Loaded neg("negative", armed("    \"clock_anchor_clock_k\": -1.0,\n"));
    EXPECT_FALSE(neg.ok);
    Loaded absent("absent", armed(""));
    ASSERT_TRUE(absent.ok);
    EXPECT_DOUBLE_EQ(absent.cfg.networkClockAnchorClockK(), 0.0);
    NetworkRunContext ctx(absent.cfg, absent.cfg.getNetworkConfig());
    EXPECT_DOUBLE_EQ(ctx.knobs.cta_clock_k, 0.0);
}

// --- the guard -----------------------------------------------------------------------------

/// The premise: a shift carried by one phase-bias key alone, with the clock still, trips the
/// phase statistic when no clock share is required (the decision as before the key).
TEST(ClockAnchorClockShare, AKeyCarriedShiftTripsWithoutTheRequirement) {
    ShareFixture f("key_off", armed(""));
    ASSERT_EQ(f.ctx.knobs.cta_space, 2);
    ASSERT_DOUBLE_EQ(f.ctx.knobs.cta_clock_k, 0.0);
    f.epoch(f.truth, keyShifted(f.truth));
    EXPECT_EQ(f.ctx.counters.cta_trip_phi, 1);
    EXPECT_FALSE(f.ctx.carry.clk_anchor.sat.at("G03").anchored);
    EXPECT_EQ(f.ctx.carry.clk_anchor.stepped,
              (std::map<std::string, ClockStepCause>{{"G03", ClockStepCause::TestTrip}}));
    EXPECT_EQ(f.ctx.counters.cta_phi_withheld, 0);
    EXPECT_TRUE(f.events("phi_withheld").empty());
    EXPECT_EQ(f.decisions("G03"), (std::vector<std::string>{"trip"}));
}

/// The same epoch with the requirement: the key moved and the clock did not (|w_clk| far below
/// 2), so the trip is withheld; G03 stays anchored, nothing is marked or re-solved, the hold's
/// trip set is empty, and the withhold is an event row and a counter.
TEST(ClockAnchorClockShare, AKeyCarriedShiftIsWithheldWithTheRequirement) {
    ShareFixture f("key_on", armed(kShare));
    ASSERT_DOUBLE_EQ(f.ctx.knobs.cta_clock_k, 2.0);
    f.epoch(f.truth, keyShifted(f.truth));
    const auto& C = f.ctx.counters;
    EXPECT_EQ(C.cta_trip_phi, 0);
    EXPECT_EQ(C.cta_trip_nis + C.cta_trip_abs, 0);
    EXPECT_EQ(C.cta_phi_withheld, 1);
    EXPECT_EQ(C.cta_phi_withheld_episodes, 1);
    EXPECT_GT(C.cta_phi_withheld_max_w_phi, 6.0);
    EXPECT_LT(C.cta_phi_withheld_max_w_clk, 2.0);
    EXPECT_EQ(C.cta_resolves, 0);
    EXPECT_EQ(C.cta_marks, 0);
    EXPECT_TRUE(f.ctx.carry.clk_anchor.sat.at("G03").anchored);
    EXPECT_TRUE(f.ctx.carry.clk_anchor.stepped.empty());
    EXPECT_TRUE(f.ctx.carry.clk_anchor.tripped.empty());
    EXPECT_EQ(f.ctx.carry.clk_anchor.sat.at("G03").phi_withheld_run, 1);
    const auto rows = f.events("phi_withheld");
    ASSERT_EQ(rows.size(), 1u);
    EXPECT_NE(rows[0].find("G03"), std::string::npos);
    EXPECT_NE(rows[0].find("pair=L1C+L2W"), std::string::npos);
    EXPECT_NE(rows[0].find("keys_share="), std::string::npos);
    EXPECT_NE(rows[0].find("withhold"), std::string::npos);
    EXPECT_EQ(f.decisions("G03"), (std::vector<std::string>{"withheld"}));
    EXPECT_EQ(f.decisions("G02"), (std::vector<std::string>{"ok"}));
}

/// Standing invariant 1: a true satellite clock step (0.5 m in the data, the prior at the old
/// value) moves the anchored clock, reads a clock share far above 2 and still trips.
TEST(ClockAnchorClockShare, ATrueSatelliteClockStepStillTripsWithTheRequirement) {
    ShareFixture f("step_on", armed(kShare));
    f.epoch(clockStepped(f.truth, 0.5), priorTight(f.truth));
    const auto& C = f.ctx.counters;
    EXPECT_EQ(C.cta_trip_phi, 1);
    EXPECT_EQ(C.cta_phi_withheld, 0);
    EXPECT_FALSE(f.ctx.carry.clk_anchor.sat.at("G03").anchored);
    EXPECT_EQ(f.ctx.carry.clk_anchor.stepped,
              (std::map<std::string, ClockStepCause>{{"G03", ClockStepCause::TestTrip}}));
    EXPECT_EQ(f.decisions("G03"), (std::vector<std::string>{"trip"}));
    const auto rows = f.events("cta_decouple");
    ASSERT_EQ(rows.size(), 1u);
    EXPECT_NE(rows[0].find("reason=phi"), std::string::npos);
}

/// The absolute backstop is unchanged on a withheld row: with the backstop's rail below the
/// clock's own (millimetre) move, the key-carried epoch decouples G03 on the backstop.
TEST(ClockAnchorClockShare, TheAbsoluteBackstopIsUnchangedOnAWithheldRow) {
    // A rail of 0.1 mm: the clock moves by more than that under the 20 m key shift.
    ShareFixture f("abs_on", armed(kShare, "5e-6", "phase", true, "1e-4"));
    ASSERT_DOUBLE_EQ(f.ctx.knobs.cta_abs_m, 1e-4);
    f.epoch(f.truth, keyShifted(f.truth));
    const auto& C = f.ctx.counters;
    EXPECT_GE(C.cta_trip_abs, 1);
    EXPECT_FALSE(f.ctx.carry.clk_anchor.sat.at("G03").anchored);
    EXPECT_EQ(C.cta_phi_withheld, 0) << "a row the backstop trips is not a withhold";
}

/// The satellite clock event's forced trip is unchanged on a withheld row: the slip layer names
/// G03 and the fold did not read it, so the backstop of the event trips it.
TEST(ClockAnchorClockShare, TheForcedTripOfASatelliteClockEventIsUnchanged) {
    ShareFixture f("forced_on", armed(kShare));
    EpochEvents ev;
    ev.slip.sat_clock_step["G03"] = 0.5;
    f.epoch(f.truth, keyShifted(f.truth), {}, ev);
    const auto& C = f.ctx.counters;
    EXPECT_EQ(C.cta_trip_slip_clk_late, 1);
    EXPECT_EQ(C.cta_phi_withheld, 0);
    EXPECT_FALSE(f.ctx.carry.clk_anchor.sat.at("G03").anchored);
    EXPECT_EQ(f.ctx.carry.clk_anchor.stepped.count("G03"), 1u);
}

/// A LOOSE row is not a trip: its re-validation reads the phase statistic as before, so the
/// key-carried epoch resets its in-band run with or without the requirement.
TEST(ClockAnchorClockShare, ALooseRowsReValidationIsUnchanged) {
    for (const bool share : {false, true}) {
        ShareFixture f(share ? "loose_on" : "loose_off", armed(share ? kShare : ""));
        f.ctx.carry.clk_anchor.sat["G03"].anchored = false;
        f.ctx.carry.clk_anchor.sat["G03"].in_band  = 2;   // one more in-band epoch re-couples
        f.epoch(f.truth, keyShifted(f.truth), {"G03"});
        const auto& st = f.ctx.carry.clk_anchor.sat.at("G03");
        EXPECT_EQ(st.in_band, 0) << share;
        EXPECT_FALSE(st.anchored) << share;
        EXPECT_EQ(f.ctx.counters.cta_phi_withheld, 0) << share;
        EXPECT_EQ(f.decisions("G03"), (std::vector<std::string>{"out"})) << share;
    }
}

/// The episode's two edges: withheld on two epochs (one episode, two withholds), then a clean
/// epoch closes it clear with an event naming its length.
TEST(ClockAnchorClockShare, AWithholdEpisodeOpensExtendsAndClosesClear) {
    ShareFixture f("episode", armed(kShare));
    f.epoch(f.truth, keyShifted(f.truth));
    f.epoch(f.truth, keyShifted(f.truth));
    const auto& C = f.ctx.counters;
    EXPECT_EQ(C.cta_phi_withheld, 2);
    EXPECT_EQ(C.cta_phi_withheld_episodes, 1);
    EXPECT_EQ(f.ctx.carry.clk_anchor.sat.at("G03").phi_withheld_run, 2);
    EXPECT_TRUE(f.events("phi_withheld_end").empty());
    f.epoch(f.truth, priorAtTruth(f.truth));
    EXPECT_EQ(C.cta_phi_withheld, 2);
    EXPECT_EQ(C.cta_phi_withheld_end_band, 1);
    EXPECT_EQ(C.cta_phi_withheld_end_trip, 0);
    EXPECT_EQ(C.cta_phi_withheld_max_run, 2);
    EXPECT_EQ(f.ctx.carry.clk_anchor.sat.at("G03").phi_withheld_run, 0);
    EXPECT_TRUE(f.ctx.carry.clk_anchor.sat.at("G03").anchored);
    const auto ends = f.events("phi_withheld_end");
    ASSERT_EQ(ends.size(), 1u);
    EXPECT_NE(ends[0].find("epochs=2;end=clear"), std::string::npos);
}

/// An episode closed by a decoupling: withheld, then the clock really steps; the trip is taken
/// and the episode's closing event names the decoupling.
TEST(ClockAnchorClockShare, AWithholdEpisodeClosedByATripNamesTheDecoupling) {
    ShareFixture f("episode_trip", armed(kShare));
    f.epoch(f.truth, keyShifted(f.truth));
    ASSERT_EQ(f.ctx.counters.cta_phi_withheld, 1);
    f.epoch(clockStepped(f.truth, 0.5), priorTight(f.truth));
    const auto& C = f.ctx.counters;
    EXPECT_EQ(C.cta_trip_phi, 1);
    EXPECT_EQ(C.cta_phi_withheld_end_trip, 1);
    EXPECT_EQ(C.cta_phi_withheld_end_band, 0);
    EXPECT_FALSE(f.ctx.carry.clk_anchor.sat.at("G03").anchored);
    const auto ends = f.events("phi_withheld_end");
    ASSERT_EQ(ends.size(), 1u);
    EXPECT_NE(ends[0].find("epochs=1;end=decoupled"), std::string::npos);
}

/// Off, a quiet epoch and a key-carried one leave every counter of the requirement at 0 and no
/// satellite with an open episode.
TEST(ClockAnchorClockShare, OffTheRequirementWritesNothing) {
    ShareFixture f("off_quiet", armed(""));
    f.epoch(f.truth, priorAtTruth(f.truth));
    f.epoch(f.truth, keyShifted(f.truth));
    const auto& C = f.ctx.counters;
    EXPECT_EQ(C.cta_phi_withheld + C.cta_phi_withheld_episodes + C.cta_phi_withheld_end_band
                  + C.cta_phi_withheld_end_trip, 0);
    EXPECT_EQ(C.cta_phi_withheld_max_run, 0);
    for (const auto& [sat, st] : f.ctx.carry.clk_anchor.sat) EXPECT_EQ(st.phi_withheld_run, 0) << sat;
    EXPECT_TRUE(f.events("phi_withheld").empty());
    EXPECT_TRUE(f.events("phi_withheld_end").empty());
}
