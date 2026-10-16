/**
 * @file test_phase_visible_monitor.cpp
 * @brief The phase-visible consistency instrument (
 *        docs/refound/PVN_phase_visible_nees_2026-09-29.md).
 *
 * Covered here:
 *   - phaseVisibleNis: e^2 / (S- - S+), refused when S- - S+ is not positive or an input
 *     is not finite;
 *   - the phase statistic's state fields (the contrast-projected prior and posterior
 *     variances S- / S+, the posterior value and its constellation median removal)
 *     against a brute-force computation on a synthetic network with two phase-bias bands
 *     per satellite and a dense prior;
 *   - the seam: knob off writes nothing; knob on forms one row per satellite per epoch,
 *     sums the NIS only after the 15th epoch, closes a window every window_ep epochs, and
 *     counts an epoch without a capture;
 *   - the loader requires network.raim_within_epoch and window_ep >= 2.
 */

#include <gtest/gtest.h>

#include "support/NetworkTestRun.hpp"   // the run and epoch networkPhaseVisibleMonitor's epoch form takes
#include "network/state/RunContext.hpp"
#include "common/SignalBand.hpp"
#include "config/GnssConfig.hpp"
#include "network/solve/ClockAnchor.hpp"
#include "network/solve/Estimator.hpp"
#include "network/diag/PhaseVisibleDiag.hpp"
#include "network/driver/RunTypes.hpp"
#include "logging/LogManager.hpp"
#include "network/diag/RunTraces.hpp"
#include "rinex/MultiRinexSync.hpp"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <limits>
#include <memory>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace GnssProcessor {
namespace {

// ---------------------------------------------------------------------------------------
// A synthetic network: S satellites with a clock and L1C / L2W phase biases, R stations.
// ---------------------------------------------------------------------------------------

struct PvNet {
    int S{0}, R{0};
    std::vector<std::string> sats;
    std::vector<Eigen::Index> clk, pb1, pb2;
    std::vector<NetworkEstimator::SharedPrior> keys;
    Eigen::MatrixXd cov;
    Eigen::VectorXd x;
};

/// Station r's rows.  Locals: [rx clock, N1_0 .. N1_{S-1}, N2_0 .. N2_{S-1}].  Code rows on
/// band 1 (rx - clk), phase rows on both bands (rx - clk + pb_j + N_j); station 0 pins its
/// ambiguities (the local half of the phase datum).
void pvStationRows(const PvNet& n, int r, const NetworkEstimator& est, Eigen::MatrixXd& A_s,
                   Eigen::MatrixXd& A_r, Eigen::VectorXd& w, Eigen::VectorXd& y) {
    const bool pivot = (r == 0);
    const Eigen::Index nl = 1 + 2 * n.S;
    const Eigen::Index m = 3 * n.S + (pivot ? 2 * n.S : 0);
    A_s = Eigen::MatrixXd::Zero(m, est.numShared());
    A_r = Eigen::MatrixXd::Zero(m, nl);
    w = Eigen::VectorXd::Ones(m);
    y = Eigen::VectorXd::Zero(m);
    Eigen::Index row = 0;
    for (int s = 0; s < n.S; ++s) {
        A_s(row, n.clk[s]) = -1.0;
        A_r(row, 0) = 1.0;
        y(row) = 0.07 * (r + 1) - 0.03 * s;
        ++row;
        for (int j = 0; j < 2; ++j) {
            A_s(row, n.clk[s]) = -1.0;
            A_s(row, j == 0 ? n.pb1[s] : n.pb2[s]) = 1.0;
            A_r(row, 0) = 1.0;
            A_r(row, 1 + j * n.S + s) = 1.0;
            w(row) = 1e4;
            y(row) = 0.11 * (r + 1) + 0.05 * s - 0.02 * j;
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

/// Build, fold a dense prior over every clock and phase bias, and solve.
PvNet pvSolve(NetworkEstimator& est, int S = 4, int R = 5) {
    PvNet n;
    n.S = S;
    n.R = R;
    for (int s = 0; s < S; ++s) {
        const std::string sat = std::string("G0") + std::to_string(s + 1);
        n.sats.push_back(sat);
        n.clk.push_back(est.registerClock(sat));
        n.pb1.push_back(est.registerPhaseBias(sat, "L1C"));
        n.pb2.push_back(est.registerPhaseBias(sat, "L2W"));
    }
    est.beginEpoch();
    for (int r = 0; r < R; ++r) {
        Eigen::MatrixXd A_s, A_r;
        Eigen::VectorXd w, y;
        pvStationRows(n, r, est, A_s, A_r, w, y);
        est.addStation(A_s, A_r, w, y);
    }
    // The dense prior: a level-correlated block (clock and phase biases of a satellite move
    // together, as the carry states them) plus independent parts.
    const int K = 3 * S;
    Eigen::MatrixXd B = Eigen::MatrixXd::Zero(K, K + S);
    for (int s = 0; s < S; ++s) {
        for (int k = 0; k < 3; ++k) B(3 * s + k, K + s) = 0.20;        // the level
        for (int k = 0; k < 3; ++k) B(3 * s + k, 3 * s + k) = 0.01 * (1 + k + s);
    }
    n.cov = B * B.transpose();
    for (int s = 0; s < S; ++s) {
        const char* sig[3] = { "", "L1C", "L2W" };
        const SharedParamKind kind[3] = { SharedParamKind::Clock, SharedParamKind::PhaseBias,
                                          SharedParamKind::PhaseBias };
        for (int k = 0; k < 3; ++k) {
            NetworkEstimator::SharedPrior p;
            p.kind = kind[k];
            p.sat = n.sats[static_cast<std::size_t>(s)];
            p.signal = sig[k];
            p.value = 0.3 * s - 0.1 * k + 0.05;
            p.info = 1.0 / n.cov(3 * s + k, 3 * s + k);
            n.keys.push_back(p);
        }
    }
    est.addSharedPriorDense(n.keys, n.cov);
    n.x = est.solve(NetworkEstimator::Datum{});
    return n;
}

double upperMedian(std::vector<double> v) {
    std::nth_element(v.begin(), v.begin() + static_cast<std::ptrdiff_t>(v.size() / 2), v.end());
    return v[v.size() / 2];
}

// --- 1. The NIS ------------------------------------------------------------------------

TEST(PhaseVisibleMonitorTest, NisIsTheSquaredUpdateOverThePredictedVariance) {
    double nis = -1.0;
    ASSERT_TRUE(phaseVisibleNis(0.02, 5e-4, 1e-4, nis));
    EXPECT_NEAR(nis, 0.0004 / 4e-4, 1e-12);
    nis = -1.0;
    EXPECT_FALSE(phaseVisibleNis(0.02, 1e-4, 1e-4, nis)) << "S- - S+ = 0 has no NIS";
    EXPECT_FALSE(phaseVisibleNis(0.02, 1e-4, 2e-4, nis)) << "a negative S- - S+ has no NIS";
    EXPECT_FALSE(phaseVisibleNis(std::numeric_limits<double>::quiet_NaN(), 5e-4, 1e-4, nis));
    EXPECT_DOUBLE_EQ(nis, -1.0) << "the out value is untouched when refused";
}

// --- 2. The state fields against a brute force ------------------------------------------

TEST(PhaseVisibleMonitorTest, StateFieldsMatchABruteForceContrastProjection) {
    NetworkEstimator est;
    const PvNet n = pvSolve(est);
    const auto rows = clockAnchorPhaseStatistic(est, n.x, n.keys, n.cov, 0.0);
    ASSERT_EQ(rows.size(), static_cast<std::size_t>(n.S));

    const double l1 = bandNominalWavelengthMeters(classifySignalForSat("G01", "L1C").band);
    const double l2 = bandNominalWavelengthMeters(classifySignalForSat("G01", "L2W").band);
    const std::vector<double> wv = clockAnchorPhaseClockWeights({ l1 * l1, l2 * l2 });
    ASSERT_EQ(wv.size(), 2u);
    EXPECT_NEAR(wv[0] + wv[1], 1.0, 1e-12);

    // The functional over the prior keys and over the shared columns.
    const Eigen::MatrixXd& Pp = est.sharedCovariance();
    const Eigen::Index K = static_cast<Eigen::Index>(n.keys.size());
    Eigen::MatrixXd Gk = Eigen::MatrixXd::Zero(n.S, K);
    Eigen::MatrixXd Gs = Eigen::MatrixXd::Zero(n.S, est.numShared());
    Eigen::VectorXd prior_val(K);
    for (Eigen::Index a = 0; a < K; ++a) prior_val(a) = n.keys[static_cast<std::size_t>(a)].value;
    for (int s = 0; s < n.S; ++s) {
        Gk(s, 3 * s) = 1.0;
        Gk(s, 3 * s + 1) = -wv[0];
        Gk(s, 3 * s + 2) = -wv[1];
        Gs(s, n.clk[s]) = 1.0;
        Gs(s, n.pb1[s]) = -wv[0];
        Gs(s, n.pb2[s]) = -wv[1];
    }
    const Eigen::MatrixXd Cm = Gk * n.cov * Gk.transpose();
    const Eigen::MatrixXd Cp = Gs * Pp * Gs.transpose();
    const Eigen::MatrixXd Kc = Eigen::MatrixXd::Identity(n.S, n.S)
                               - Eigen::MatrixXd::Constant(n.S, n.S, 1.0 / n.S);
    const Eigen::MatrixXd CmK = Kc * Cm * Kc, CpK = Kc * Cp * Kc;
    const Eigen::VectorXd d = Gs * n.x;
    const Eigen::VectorXd e = d - Gk * prior_val;
    const double dmed = upperMedian(std::vector<double>(d.data(), d.data() + d.size()));
    const double emed = upperMedian(std::vector<double>(e.data(), e.data() + e.size()));
    for (int s = 0; s < n.S; ++s) {
        const auto& r = rows.at(n.sats[static_cast<std::size_t>(s)]);
        ASSERT_TRUE(r.ok);
        EXPECT_EQ(r.n_pb, 2);
        EXPECT_EQ(r.sigs, "L1C+L2W") << "the signals the functional took, band order";
        EXPECT_NEAR(r.Sm, CmK(s, s), 1e-12 * std::max(1.0, CmK(s, s))) << n.sats[s];
        EXPECT_NEAR(r.Sp, CpK(s, s), 1e-9 * std::max(1e-6, CpK(s, s)) + 1e-15) << n.sats[s];
        EXPECT_NEAR(r.d_post, d(s), 1e-12) << n.sats[s];
        EXPECT_NEAR(r.d_post_hat, d(s) - dmed, 1e-12) << n.sats[s];
        EXPECT_NEAR(r.e_hat, e(s) - emed, 1e-12) << n.sats[s];
        EXPECT_GT(r.Sm - r.Sp, 0.0) << "the data must reduce the functional's variance";
        EXPECT_NEAR(r.S, std::max(0.0, r.Sm - r.Sp), 1e-15) << "the guard's S with no floor";
    }
}

// --- 3. The seam ------------------------------------------------------------------------

std::string pvConfig(bool on, bool raim_we, int window_ep) {
    return std::string("{\n") +
        "  \"processing\": {\n"
        "    \"mode\": \"ppp-rtk-network\",\n"
        "    \"active_systems\": [ \"GPS\" ],\n"
        "    \"signal_priorities\": { \"GPS\": \"WC\" }\n"
        "  },\n"
        "  \"network\": {\n"
        "    \"phase_visible_monitor\": " + (on ? "true" : "false") + ",\n"
        "    \"phase_visible_window_ep\": " + std::to_string(window_ep) + ",\n"
        "    \"raim_within_epoch\": " + (raim_we ? "true" : "false") + ",\n"
        "    \"stations\": [\n"
        "      { \"name\": \"AAAA\", \"position_ecef\": [ -2493304.99, -4655214.79, 3565497.67 ] },\n"
        "      { \"name\": \"BBBB\", \"position_ecef\": [ -2281621.75, -1453585.08, 5756964.89 ] }\n"
        "    ]\n"
        "  }\n"
        "}\n";
}

bool pvLoads(bool on, bool raim_we, int window_ep, const char* tag, GnssConfig* out = nullptr) {
    LogManager log_mgr;
    GnssConfig local;
    GnssConfig& cfg = out ? *out : local;
    const fs::path p = fs::temp_directory_path() / (std::string("pv_load_") + tag + ".json");
    { std::ofstream f(p); f << pvConfig(on, raim_we, window_ep); }
    const bool ok = cfg.loadFromJson(p, log_mgr);
    std::error_code ec; fs::remove(p, ec);
    return ok;
}

TEST(PhaseVisibleMonitorTest, LoaderRequiresTheWithinEpochCaptureAndAWindowOfTwo) {
    EXPECT_TRUE(pvLoads(true, true, 60, "ok"));
    EXPECT_FALSE(pvLoads(true, false, 60, "noraim"))
        << "the prior is the dense fold's capture, which only the within-epoch RAIM keeps";
    EXPECT_FALSE(pvLoads(true, true, 1, "win1"));
    EXPECT_TRUE(pvLoads(false, false, 60, "off")) << "off needs nothing";
}

TEST(PhaseVisibleMonitorTest, SeamCountsRowsSettlesAfterFifteenAndClosesWindows) {
    for (const bool on : { false, true }) {
        GnssConfig cfg;
        ASSERT_TRUE(pvLoads(on, true, 5, on ? "seam_on" : "seam_off", &cfg));
        const fs::path dir = fs::temp_directory_path() / "pv_traces";
        fs::create_directories(dir);
        NetworkTestRun fx(cfg, nullptr, dir);   // the run networkPhaseVisibleMonitor's epoch form takes
        NetworkRunContext& ctx = fx.ctx();
        ASSERT_EQ(ctx.knobs.pv_on, on);
        EXPECT_EQ(ctx.knobs.pv_window_ep, 5);
        NetworkEstimator& est = fx.est();
        const PvNet n = pvSolve(est);
        GnssTime t{ 2313, 259200.0 };
        for (int ep = 0; ep < 20; ++ep) {
            t.tow = 259200.0 + 30.0 * ep;
            NetworkEpoch& e = fx.newEpoch(t);
            e.solve.x_s = n.x;
            e.ws.cap_dense_keys = n.keys;   // the dense fold's capture
            e.ws.cap_dense_cov = n.cov;
            networkPhaseVisibleMonitor(fx.run(), e);
        }
        // An epoch without a capture is counted and forms nothing.
        NetworkEpoch& e = fx.newEpoch(t);
        e.solve.x_s = n.x;
        networkPhaseVisibleMonitor(fx.run(), e);
        const auto& k = ctx.counters;
        if (!on) {
            EXPECT_EQ(k.pv_epochs, 0);
            EXPECT_EQ(k.pv_rows, 0);
            EXPECT_TRUE(k.pv_win_nis.empty());
            continue;
        }
        EXPECT_EQ(k.pv_epochs, 21);
        EXPECT_EQ(k.pv_epochs_nocap, 1);
        EXPECT_EQ(k.pv_rows, 20 * n.S);
        EXPECT_EQ(k.pv_su_nonpos, 0);
        EXPECT_EQ(k.pv_nis_n, 5 * n.S) << "epochs 16 to 20 are settled";
        // Every epoch is the same solve, so each settled NIS is the per-satellite value
        // and the windows that close after the settling read their mean.
        const auto rows = clockAnchorPhaseStatistic(est, n.x, n.keys, n.cov, 0.0);
        double sum = 0.0;
        for (const auto& [sat, r] : rows) {
            double v = 0.0;
            ASSERT_TRUE(phaseVisibleNis(r.e_hat, r.Sm, r.Sp, v));
            sum += v;
        }
        EXPECT_NEAR(k.pv_nis_sum, 5.0 * sum, 1e-9 * std::max(1.0, sum));
        ASSERT_EQ(k.pv_win_nis.size(), 1u) << "windows 1 to 3 hold no settled NIS; window 4 closes at epoch 20";
        EXPECT_NEAR(k.pv_win_nis[0], sum / n.S, 1e-9 * std::max(1.0, sum));
        EXPECT_EQ(k.pv_abs_e.size(), static_cast<std::size_t>(5 * n.S));
    }
}

}  // namespace
}  // namespace GnssProcessor
