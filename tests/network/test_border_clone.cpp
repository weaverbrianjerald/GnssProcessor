/**
 * @file test_border_clone.cpp
 * @brief Gate U0 of card BCL, the whole previous border on clones
 *        (@c network.carry_coupling_bias_clone, @c network.carry_coupling_departed_clone;
 *        docs/refound/BCL_previous_border_on_clones_2026-10-08.md, section 4).
 *
 * (a) A bias clone registers last with its key and label, idempotently, and is never
 *     emitted.
 * (b) Priors fold by identity onto a bias clone; a bias walk factor is the information form
 *     of b(t) - b(t-1) and withdraws exactly; a joint factor over a clock and a bias walk
 *     equals A' Qw^-1 A.
 * (c) The departed clone is exact: two rows sharing a departed key, folded on its clone with
 *     the stashed block, give the batch posterior, while card CTU's per-row re-attribution
 *     of the same rows does not (its cross-row term is dropped).
 * (d) The gate view marginalizes a referenced bias clone and keeps the declared gauge null,
 *     also with no clock clone registered.
 * (e) Both knobs load default off and refuse to load without card SCL's clock clone.
 */

#include <gtest/gtest.h>

#include "network/state/RunContext.hpp"
#include "config/GnssConfig.hpp"
#include "network/carry/ClockClone.hpp"
#include "network/carry/CouplingExact.hpp"
#include "network/solve/Estimator.hpp"
#include "logging/LogManager.hpp"

#include <Eigen/Dense>

#include <cmath>
#include <filesystem>
#include <fstream>
#include <memory>
#include <random>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using namespace GnssProcessor;

namespace {

/// Config text: a two-station network with card SCL's chain (the exact update, the dense
/// carry, the dense handover transform) and the given clone switches; @p extra is spliced
/// into the network section.
std::string bclConfig(bool clone, bool bias, bool departed, const std::string& extra = "") {
    const auto b = [](bool v) { return std::string(v ? "true" : "false"); };
    return std::string("{\n") +
        "  \"processing\": {\n"
        "    \"mode\": \"ppp-rtk-network\",\n"
        "    \"active_systems\": [ \"GPS\" ],\n"
        "    \"signal_priorities\": { \"GPS\": \"WC\" }\n"
        "  },\n"
        "  \"network\": {\n"
        "    \"carry_conditional_coupling\": true,\n"
        "    \"carry_coupling_time_update\": true,\n"
        "    \"scoped_handover_response\": true,\n"
        "    \"handover_stransform\": true,\n"
        "    \"handover_stransform_dense\": true,\n"
        "    \"shared_carry_full_cov\": true,\n"
        "    \"carry_coupling_exact_update\": true,\n"
        "    \"carry_coupling_clock_clone\": " + b(clone) + ",\n"
        "    \"carry_coupling_bias_clone\": " + b(bias) + ",\n"
        "    \"carry_coupling_departed_clone\": " + b(departed) + ",\n" + extra +
        "    \"stations\": [\n"
        "      { \"name\": \"AAAA\", \"position_ecef\": [ -2493304.99, -4655214.79, 3565497.67 ] },\n"
        "      { \"name\": \"BBBB\", \"position_ecef\": [ -2281621.75, -1453585.08, 5756964.89 ] }\n"
        "    ]\n"
        "  }\n"
        "}\n";
}

/// Load @p body; @p bias / @p departed receive the resolved knobs.
bool bclLoads(const std::string& body, const char* tag, bool* bias = nullptr,
              bool* departed = nullptr) {
    LogManager log_mgr;
    GnssConfig cfg;
    const fs::path p = fs::temp_directory_path() / (std::string("bcl_load_") + tag + ".json");
    { std::ofstream f(p); f << body; }
    const bool ok = cfg.loadFromJson(p, log_mgr);
    std::error_code ec; fs::remove(p, ec);
    if (bias) *bias = cfg.networkCarryCouplingBiasClone();
    if (departed) *departed = cfg.networkCarryCouplingDepartedClone();
    return ok;
}

/// The dimension of the numerical null space of a symmetric PSD matrix (relative 1e-10).
int nullCount(const Eigen::MatrixXd& N) {
    Eigen::SelfAdjointEigenSolver<Eigen::MatrixXd> es(N, Eigen::EigenvaluesOnly);
    const Eigen::VectorXd ev = es.eigenvalues();
    const double cut = 1e-10 * std::max(1.0, ev.cwiseAbs().maxCoeff());
    int n = 0;
    for (Eigen::Index i = 0; i < ev.size(); ++i) n += (std::abs(ev(i)) < cut) ? 1 : 0;
    return n;
}

}  // namespace

// (a) The bias clone is the last column, keyed and labelled apart from the bias, idempotent,
// counted with the clones, and a clock clone through it is card SCL's own.
TEST(BorderCloneTest, BiasCloneRegistersLastWithItsKeyAndLabel) {
    NetworkEstimator est;
    const Eigen::Index c1 = est.registerClock("G01");
    const Eigen::Index p1 = est.registerPhaseBias("G01", "L1C");
    const Eigen::Index b1 = est.registerCodeBias("G01", "C1C");
    const Eigen::Index kp = est.registerClone(SharedParamKind::PhaseBias, "G01", "L1C");
    const Eigen::Index kb = est.registerClone(SharedParamKind::CodeBias, "G01", "C1C");
    const Eigen::Index kc = est.registerClone(SharedParamKind::Clock, "G01", "ignored");
    EXPECT_EQ(c1, 0);
    EXPECT_EQ(p1, 1);
    EXPECT_EQ(b1, 2);
    EXPECT_EQ(kp, 3);
    EXPECT_EQ(kb, 4);
    EXPECT_EQ(kc, 5);
    EXPECT_EQ(est.registerClone(SharedParamKind::PhaseBias, "G01", "L1C"), kp) << "idempotent";
    EXPECT_EQ(est.registerClockClone("G01"), kc) << "a clock clone is card SCL's";
    EXPECT_EQ(est.cloneColumn(SharedParamKind::PhaseBias, "G01", "L1C"), kp);
    EXPECT_EQ(est.cloneColumn(SharedParamKind::CodeBias, "G01", "C1C"), kb);
    EXPECT_EQ(est.cloneColumn(SharedParamKind::Clock, "G01", ""), kc);
    EXPECT_THROW(est.cloneColumn(SharedParamKind::PhaseBias, "G01", "L2W"), std::out_of_range);
    EXPECT_EQ(est.phaseBiasColumn("G01", "L1C"), p1) << "the bias keeps its column";
    EXPECT_EQ(est.sharedColumnLabel(kp), "PBC:G01:L1C");
    EXPECT_EQ(est.sharedColumnLabel(kb), "CBC:G01:C1C");
    EXPECT_EQ(est.sharedColumnLabel(kc), "CLKC:G01");
    EXPECT_TRUE(est.isClockClone(kp)) << "the flag marks a clone of any kind";
    EXPECT_EQ(est.numClockClones(), 3);
    SharedParamKind k;
    std::string sat, sig;
    EXPECT_FALSE(est.sharedColumnIdentity(kp, k, sat, sig)) << "a clone has no key identity";
    EXPECT_TRUE(est.sharedColumnIdentity(p1, k, sat, sig));
    EXPECT_THROW(est.registerClone(SharedParamKind::VtecSh, "G01", ""), std::invalid_argument);
    // Never emitted: the product's bias is its own column's value.
    Eigen::VectorXd x(6);
    x << 1.5, 0.25, -0.5, 99.0, -99.0, 77.0;
    const NetworkSolutionEpoch sol = est.toSolutionEpoch(x, GnssTime{});
    ASSERT_EQ(sol.sats.size(), 1u);
    EXPECT_DOUBLE_EQ(sol.sats[0].clock_c0_m, 1.5);
    ASSERT_EQ(sol.sats[0].phase_bias.count("L1C"), 1u);
    EXPECT_DOUBLE_EQ(sol.sats[0].phase_bias.at("L1C").bias_m, 0.25);
    ASSERT_EQ(sol.sats[0].code_bias_m.count("C1C"), 1u);
    EXPECT_DOUBLE_EQ(sol.sats[0].code_bias_m.at("C1C"), -0.5);
    est.beginEpoch();
    EXPECT_THROW(est.registerClone(SharedParamKind::PhaseBias, "G02", "L1C"), std::logic_error);
    est.reset();
    EXPECT_EQ(est.numClockClones(), 0);
}

// (b) A bias clone prior folds by identity onto the clone; a bias walk factor is
// (e_b - e_k)(e_b - e_k)'/q with the mean on the right-hand side and withdraws exactly; the
// joint factor over a clock walk and a bias walk is A' Qw^-1 A with A' Qw^-1 mean.
TEST(BorderCloneTest, BiasPriorsAndWalksFoldByIdentity) {
    NetworkEstimator est;
    const Eigen::Index c  = est.registerClock("G05");
    const Eigen::Index b  = est.registerPhaseBias("G05", "L2W");
    const Eigen::Index kc = est.registerClockClone("G05");
    const Eigen::Index kb = est.registerClone(SharedParamKind::PhaseBias, "G05", "L2W");
    est.beginEpoch();
    const auto& neq = est.normals();
    NetworkEstimator::SharedPrior pk;
    pk.kind = SharedParamKind::PhaseBias; pk.sat = "G05"; pk.signal = "L2W";
    pk.clone = true; pk.value = 0.004;
    Eigen::MatrixXd cov(1, 1);
    cov << 1e-6;
    est.addSharedPriorDense({pk}, cov);
    EXPECT_NEAR(neq.sharedNormals()(kb, kb), 1e6, 1e-3);
    EXPECT_EQ(neq.sharedNormals()(b, b), 0.0) << "the bias's own column takes nothing";
    EXPECT_NEAR(neq.sharedRhs()(kb), 0.004 * 1e6, 1e-6);
    const Eigen::MatrixXd N0 = neq.sharedNormals();
    const Eigen::VectorXd u0 = neq.sharedRhs();
    NetworkEstimator::CloneWalk wb;
    wb.sat = "G05"; wb.kind = SharedParamKind::PhaseBias; wb.signal = "L2W";
    wb.mean = 1e-5; wb.q = 3e-11;
    EXPECT_EQ(est.addCloneWalk({wb}), 1u);
    const Eigen::MatrixXd dN = neq.sharedNormals() - N0;
    const Eigen::VectorXd du = neq.sharedRhs() - u0;
    const double w = 1.0 / 3e-11;
    EXPECT_NEAR(dN(b, b) / w, 1.0, 1e-12);
    EXPECT_NEAR(dN(kb, kb) / w, 1.0, 1e-12);
    EXPECT_NEAR(dN(b, kb) / w, -1.0, 1e-12);
    EXPECT_EQ(dN(c, c), 0.0) << "the clock is untouched by a bias walk";
    EXPECT_EQ(dN(kc, kc), 0.0);
    EXPECT_NEAR(du(b) / (w * 1e-5), 1.0, 1e-12);
    EXPECT_NEAR(du(kb) / (w * 1e-5), -1.0, 1e-12);
    EXPECT_EQ(est.addCloneWalk({wb}, -1.0), 1u);
    EXPECT_LE((neq.sharedNormals() - N0).cwiseAbs().maxCoeff(), 1e-12 * w);
    EXPECT_LE((neq.sharedRhs() - u0).cwiseAbs().maxCoeff(), 1e-12 * w * 1e-5);

    // The joint factor over a clock walk and the bias walk against the brute force.
    NetworkEstimator::CloneWalk wc;
    wc.sat = "G05"; wc.mean = 0.02; wc.q = 1.5e-4;
    Eigen::Matrix2d Qw;
    Qw << 1.5e-4, 2e-8,
          2e-8,   3e-11 + 1e-11;
    const Eigen::MatrixXd N1 = neq.sharedNormals();
    const Eigen::VectorXd u1 = neq.sharedRhs();
    EXPECT_EQ(est.addCloneWalkJoint({wc, wb}, Qw), 2u);
    Eigen::MatrixXd A = Eigen::MatrixXd::Zero(2, est.numShared());
    A(0, c) = 1.0; A(0, kc) = -1.0;
    A(1, b) = 1.0; A(1, kb) = -1.0;
    const Eigen::Matrix2d W = Qw.inverse();
    const Eigen::MatrixXd Nref = A.transpose() * W * A;
    const Eigen::VectorXd uref = A.transpose() * W * Eigen::Vector2d(0.02, 1e-5);
    const Eigen::MatrixXd dJ = neq.sharedNormals() - N1;
    EXPECT_LE((dJ - Nref).cwiseAbs().maxCoeff(), 1e-9 * Nref.cwiseAbs().maxCoeff());
    EXPECT_LE((neq.sharedRhs() - u1 - uref).cwiseAbs().maxCoeff(), 1e-9 * uref.cwiseAbs().maxCoeff());
}

// (c) Two rows share a departed key d.  Folded on d's clone with the stashed block (the clone
// of the continuing clock s standing in, its walk folded once), they give the batch
// posterior of s(t) exactly; card CTU's per-row re-attribution of the same rows, each with
// its own conditional variance, does not.
TEST(BorderCloneTest, DepartedCloneIsExactWhereThePerRowFormIsNot) {
    const double Pss = 0.04, Psd = 0.012, Pdd = 0.01, Q = 0.02;
    const double shs = 0.1, shd = -0.05;
    const double m1s = 0.7, m1d = 1.2, c1 = 1e-3, y1 = 0.03;
    const double m2s = -0.4, m2d = 0.9, c2 = 2e-3, y2 = -0.08;

    // The batch over z = [s(t), s(t-1), d(t-1)].
    Eigen::Matrix3d S0;
    S0 << Pss + Q, Pss, Psd,
          Pss,     Pss, Psd,
          Psd,     Psd, Pdd;
    const Eigen::Vector3d z0(shs, shs, shd);
    Eigen::Matrix<double, 2, 3> H;
    H << 0.0, m1s, m1d,
         0.0, m2s, m2d;
    const Eigen::Vector2d y(y1, y2);
    const Eigen::Matrix2d Rinv = Eigen::Vector2d(1.0 / c1, 1.0 / c2).asDiagonal();
    const Eigen::Matrix3d Ipost = S0.inverse() + H.transpose() * Rinv * H;
    const Eigen::Matrix3d Spost = Ipost.inverse();
    const Eigen::Vector3d xpost = Spost * (S0.inverse() * z0 + H.transpose() * Rinv * y);

    // Card BCL-d: s on its clone (stand-in, walk once), d on its clone (the stashed row, no walk).
    NetworkEstimator est;
    const Eigen::Index cs = est.registerClock("G01");
    const Eigen::Index ks = est.registerClockClone("G01");
    const Eigen::Index kd = est.registerClone(SharedParamKind::PhaseBias, "G02", "L1C");   // departed: no own column
    est.beginEpoch();
    NetworkEstimator::SharedPrior ps;
    ps.kind = SharedParamKind::Clock; ps.sat = "G01"; ps.clone = true; ps.value = shs;
    NetworkEstimator::SharedPrior pd;
    pd.kind = SharedParamKind::PhaseBias; pd.sat = "G02"; pd.signal = "L1C"; pd.clone = true; pd.value = shd;
    Eigen::Matrix2d P;
    P << Pss, Psd,
         Psd, Pdd;
    est.addSharedPriorDense({ps, pd}, P);
    NetworkEstimator::CloneWalk w;
    w.sat = "G01"; w.mean = 0.0; w.q = Q;
    ASSERT_EQ(est.addCloneWalk({w}), 1u);
    Eigen::MatrixXd H1 = Eigen::MatrixXd::Zero(1, est.numShared());
    H1(0, ks) = m1s; H1(0, kd) = m1d;
    Eigen::MatrixXd H2 = Eigen::MatrixXd::Zero(1, est.numShared());
    H2(0, ks) = m2s; H2(0, kd) = m2d;
    est.addSharedConstraintRows(H1, Eigen::VectorXd::Constant(1, y1), 1.0 / c1);
    est.addSharedConstraintRows(H2, Eigen::VectorXd::Constant(1, y2), 1.0 / c2);
    const Eigen::VectorXd x = est.solve({});
    const Eigen::MatrixXd& Sx = est.sharedCovariance();
    EXPECT_NEAR(x(cs), xpost(0), 1e-9 * std::max(1.0, std::abs(xpost(0))));
    EXPECT_NEAR(Sx(cs, cs) / Spost(0, 0), 1.0, 1e-9);
    EXPECT_NEAR(x(kd), xpost(2), 1e-9 * std::max(1.0, std::abs(xpost(2))));

    // Card CTU's per-row form on the same rows: S = {s} with Q, D = {d} departed.
    CouplingExactEpoch ep;
    ep.P = P;
    ep.p_of_key = {{"s", 0}, {"d", 1}};
    ep.s_of_p   = {0, -1};
    ep.route_p  = {static_cast<int>(CouplingExactDRoute::Present),
                   static_cast<int>(CouplingExactDRoute::Departed)};
    ep.clk_p    = {1, 0};
    ep.cc_p     = {'G', 'G'};
    ep.p_of_s   = {0};
    ep.col_of_s = {0};
    ep.q_s      = Eigen::VectorXd::Constant(1, Q);
    ep.shat_s   = Eigen::VectorXd::Constant(1, shs);
    ASSERT_TRUE(couplingExactFactor(ep));
    const CouplingExactRow r1 = couplingExactRow(ep, {{0, m1s}, {1, m1d}});
    const CouplingExactRow r2 = couplingExactRow(ep, {{0, m2s}, {1, m2d}});
    ASSERT_TRUE(r1.ok && r2.ok);
    // Each row as an observation of s(t): y - m_s shat_s - m_d shat_d + m_new shat_s = m_new s(t).
    const double a1 = r1.m_new(0), a2 = r2.m_new(0);
    const double o1 = y1 - m1s * shs - m1d * shd + a1 * shs;
    const double o2 = y2 - m2s * shs - m2d * shd + a2 * shs;
    const double v1 = c1 + r1.var_add, v2 = c2 + r2.var_add;
    const double I = 1.0 / (Pss + Q) + a1 * a1 / v1 + a2 * a2 / v2;
    const double xr = (shs / (Pss + Q) + a1 * o1 / v1 + a2 * o2 / v2) / I;
    // With the dropped cross-row term restored the per-row form is the batch (the check
    // that the comparison above is the right one).
    const double cx = couplingExactCross(ep, r1, r2);
    Eigen::Matrix2d V;
    V << v1, cx,
         cx, v2;
    const Eigen::Vector2d av(a1, a2), ov(o1, o2);
    const double Ij = 1.0 / (Pss + Q) + av.dot(V.inverse() * av);
    const double xj = (shs / (Pss + Q) + av.dot(V.inverse() * ov)) / Ij;
    EXPECT_NEAR(xj, xpost(0), 1e-9 * std::max(1.0, std::abs(xpost(0))));
    EXPECT_NEAR((1.0 / Ij) / Spost(0, 0), 1.0, 1e-9);
    // The per-row form itself differs from the batch in both value and variance.
    std::printf("[U0c] s(t): batch %.6f var %.4e; departed clone %.6f var %.4e; per-row %.6f var %.4e "
                "(cross-row term %.3e m^2)\n", xpost(0), Spost(0, 0), x(cs), Sx(cs, cs), xr, 1.0 / I, cx);
    EXPECT_GT(std::abs((1.0 / I) / Spost(0, 0) - 1.0), 1e-3);
    EXPECT_GT(std::abs(xr - xpost(0)), 1e-4);
}

// (d) The gate view marginalizes a referenced bias clone with no clock clone registered:
// the declared gauge (the clocks' joint shift) stays null, and the bias clone, which is no
// member of it, leaves no null direction behind.
TEST(BorderCloneTest, GateViewMarginalizesABiasClone) {
    LogManager log;
    GnssConfig cfg;
    const fs::path path = fs::temp_directory_path() / "bcl_ctx_gate.json";
    { std::ofstream f(path); f << bclConfig(true, true, false); }
    ASSERT_TRUE(cfg.loadFromJson(path, log));
    auto ctx = std::make_unique<NetworkRunContext>(cfg, cfg.getNetworkConfig());
    std::error_code ec; fs::remove(path, ec);
    ASSERT_TRUE(ctx->knobs.carry_cpl_bclone);
    NetworkEstimator est;
    est.registerClock("G01");                                   // 0
    est.registerClock("G02");                                   // 1
    est.registerPhaseBias("G02", "L1C");                        // 2
    const std::string key = sharedCarryKey(SharedParamKind::PhaseBias, "G02", "L1C");
    ctx->carry.ccl.xcol[key] = est.registerClone(SharedParamKind::PhaseBias, "G02", "L1C");   // 3
    est.beginEpoch();
    // Rows over (clock A, clock B, PB B, PB clone B), orthogonal to the clocks' joint shift.
    std::mt19937 rng(17u);
    std::normal_distribution<double> nd(0.0, 1.0);
    Eigen::MatrixXd Hr(6, 4);
    for (int r = 0; r < 6; ++r) {
        for (int c = 0; c < 4; ++c) Hr(r, c) = nd(rng);
        const double mean = 0.5 * (Hr(r, 0) + Hr(r, 1));
        Hr(r, 0) -= mean;
        Hr(r, 1) -= mean;
    }
    est.addSharedConstraintRows(Hr, Eigen::VectorXd::Zero(6), 3.0);
    const Eigen::MatrixXd N = est.reducedSharedNormals();
    EXPECT_EQ(nullCount(N), 1) << "the clocks' joint shift";
    Eigen::MatrixXd Vw;
    ASSERT_TRUE(networkClockCloneGateView(*ctx, est, Vw)) << "a bias clone alone forms the view";
    ASSERT_EQ(Vw.rows(), 3);
    EXPECT_EQ(nullCount(Vw), 1) << "the declared gauge alone";
    EXPECT_NEAR((Vw * Eigen::Vector3d(1.0, 1.0, 0.0)).norm(), 0.0, 1e-9 * N.norm());
}

// (e) Both knobs are off by default and require card SCL's clock clone; armed with it, they
// load and are read into the run context.
TEST(BorderCloneTest, LoaderDefaultOffAndRequiresTheClockClone) {
    bool bias = true, departed = true;
    EXPECT_TRUE(bclLoads(bclConfig(true, false, false), "off", &bias, &departed));
    EXPECT_FALSE(bias);
    EXPECT_FALSE(departed);
    std::string absent = bclConfig(true, false, false);
    for (const char* k : {"    \"carry_coupling_bias_clone\": false,\n",
                          "    \"carry_coupling_departed_clone\": false,\n"}) {
        const auto pos = absent.find(k);
        ASSERT_NE(pos, std::string::npos);
        absent.erase(pos, std::string(k).size());
    }
    EXPECT_TRUE(bclLoads(absent, "absent", &bias, &departed));
    EXPECT_FALSE(bias);
    EXPECT_FALSE(departed);
    EXPECT_TRUE(bclLoads(bclConfig(true, true, true), "on", &bias, &departed));
    EXPECT_TRUE(bias);
    EXPECT_TRUE(departed);
    EXPECT_FALSE(bclLoads(bclConfig(false, true, false), "bias_noclone"))
        << "the bias clone is a mode of card SCL's clone";
    EXPECT_FALSE(bclLoads(bclConfig(false, false, true), "dep_noclone"))
        << "the departed clone is a mode of card SCL's clone";
}
