/**
 * @file test_network_clock_clone.cpp
 * @brief Unit gates of card SCL's clock clone (@c network.carry_coupling_clock_clone,
 *        docs/refound/SCL_self_contained_clock_level_2026-10-03.md, section 6, U2).
 *
 * 1. Estimator: a clone column is registered after every other column, keyed
 *    @c "sat|CLKC" and labelled @c "CLKC:<sat>", never emitted by @c toSolutionEpoch, and
 *    a clone prior folds by identity onto the clone column (dense and diagonal), while
 *    card Q3's refloat marginalizes the clock and keeps its clone.  The walk factor is the
 *    information form of @f$c(t) - c(t-1)@f$ and withdraws exactly.
 * 2. The posterior regression of the clones (@c cloneConditional) equals the covariance
 *    form @f$K=\Sigma_{CS'}\Sigma_{S'S'}^{-1}@f$, @f$B'=\Sigma_{CC}-K\Sigma_{S'C}@f$, and
 *    keeps a re-attributed row's identity along the declared gauge.
 * 3. The gate view: a referenced clone is marginalized and the declared gauge stays null;
 *    an idle clone is dropped (a principal sub-matrix would break the gauge).
 * 4. The harvest marginalization of one row (mode 5 of the unit network).
 * 5. The loader: default off; the requirements and refusals throw.
 * 6. Card SMH's row on the clone epoch (@c pbHoldCloneOnClones): the moved clock at Q = 0,
 *    its coefficient unshrunk, its revision from the clone's posterior, the walk share from
 *    card CTU's epoch; and its re-expression (@c pbHoldCloneMarginalizeClones), the
 *    harvest's rule in the row convention.
 *
 * The recursion-level gates (the clone with the joint harvest equals the batch, the
 * per-station harvest against the batch) are in test_coupling_exact_update.cpp.
 */

#include <gtest/gtest.h>

#include "app/NetworkRunContext.hpp"
#include "config/GnssConfig.hpp"
#include "filter/NetworkClockClone.hpp"
#include "filter/NetworkEstimator.hpp"
#include "filter/NetworkPbHoldClone.hpp"
#include "filter/NetworkRunTypes.hpp"
#include "logging/LogManager.hpp"

#include <Eigen/Dense>

#include <cmath>
#include <filesystem>
#include <fstream>
#include <limits>
#include <map>
#include <random>
#include <set>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using namespace GnssProcessor;

namespace {

/// Config text: a two-station network with the card CTU chain and the given card SCL
/// switches.  @p extra is spliced into the network section.
/// @p dense: network.handover_stransform_dense (card SCL B2, which B1 requires); -1 follows
/// @p clone.
std::string sclConfig(bool exact, bool fullcov, bool clone, const std::string& extra = "",
                      int dense = -1) {
    const auto b = [](bool v) { return std::string(v ? "true" : "false"); };
    const bool dn = dense < 0 ? clone : dense != 0;
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
        "    \"handover_stransform_dense\": " + b(dn) + ",\n"
        "    \"shared_carry_full_cov\": " + b(fullcov) + ",\n"
        "    \"carry_coupling_exact_update\": " + b(exact) + ",\n"
        "    \"carry_coupling_clock_clone\": " + b(clone) + ",\n" + extra +
        "    \"stations\": [\n"
        "      { \"name\": \"AAAA\", \"position_ecef\": [ -2493304.99, -4655214.79, 3565497.67 ] },\n"
        "      { \"name\": \"BBBB\", \"position_ecef\": [ -2281621.75, -1453585.08, 5756964.89 ] }\n"
        "    ]\n"
        "  }\n"
        "}\n";
}

/// Load @p body; @p armed receives the resolved knob.
bool sclLoads(const std::string& body, const char* tag, bool* armed = nullptr) {
    LogManager log_mgr;
    GnssConfig cfg;
    const fs::path p = fs::temp_directory_path() / (std::string("scl_load_") + tag + ".json");
    { std::ofstream f(p); f << body; }
    const bool ok = cfg.loadFromJson(p, log_mgr);
    std::error_code ec; fs::remove(p, ec);
    if (armed) *armed = cfg.networkCarryCouplingClockClone();
    return ok;
}

/// A run context from a loaded config with the clone knob armed, for the seam functions.
struct SclCtx {
    LogManager log;
    GnssConfig cfg;
    fs::path   path;
    std::unique_ptr<NetworkRunContext> ctx;
    explicit SclCtx(const char* tag, bool clone = true) {
        path = fs::temp_directory_path() / (std::string("scl_ctx_") + tag + ".json");
        { std::ofstream f(path); f << sclConfig(true, true, clone); }
        EXPECT_TRUE(cfg.loadFromJson(path, log));
        ctx = std::make_unique<NetworkRunContext>(cfg, cfg.getNetworkConfig());
        EXPECT_EQ(ctx->knobs.carry_cpl_clone, clone);
    }
    ~SclCtx() { std::error_code ec; fs::remove(path, ec); }
};

/// The dimension of the numerical null space of a symmetric PSD matrix (relative 1e-10).
int nullCount(const Eigen::MatrixXd& N) {
    Eigen::SelfAdjointEigenSolver<Eigen::MatrixXd> es(N, Eigen::EigenvaluesOnly);
    const Eigen::VectorXd ev = es.eigenvalues();
    const double cut = 1e-10 * std::max(1.0, ev.cwiseAbs().maxCoeff());
    int n = 0;
    for (Eigen::Index i = 0; i < ev.size(); ++i) n += (std::abs(ev(i)) < cut) ? 1 : 0;
    return n;
}

/// Border information H'H w over (clock A, clock B, clone of B, [idle clone of A]) with
/// the joint shift of every clock and clone as its one null direction: random rows
/// orthogonal to the all-ones vector over the first three columns.
Eigen::MatrixXd gaugeRows(int rows, unsigned seed, Eigen::Index width) {
    std::mt19937 rng(seed);
    std::normal_distribution<double> nd(0.0, 1.0);
    Eigen::MatrixXd H = Eigen::MatrixXd::Zero(rows, width);
    for (int r = 0; r < rows; ++r) {
        for (Eigen::Index c = 0; c < 3; ++c) H(r, c) = nd(rng);
        const double mean = H.row(r).head(3).mean();
        for (Eigen::Index c = 0; c < 3; ++c) H(r, c) -= mean;
    }
    return H;
}

}  // namespace

// The clone is the last column, keyed and labelled apart from the clock, idempotent, and
// registration after beginEpoch throws as for every column.
TEST(NetworkClockCloneTest, CloneRegistersLastWithItsKeyAndLabel) {
    NetworkEstimator est;
    const Eigen::Index c1 = est.registerClock("G01");
    const Eigen::Index p1 = est.registerPhaseBias("G01", "L1C");
    const Eigen::Index c2 = est.registerClock("G02");
    const Eigen::Index k2 = est.registerClockClone("G02");
    EXPECT_EQ(c1, 0);
    EXPECT_EQ(p1, 1);
    EXPECT_EQ(c2, 2);
    EXPECT_EQ(k2, 3);
    EXPECT_EQ(est.registerClockClone("G02"), k2) << "idempotent";
    EXPECT_EQ(est.clockColumn("G02"), c2) << "the clock keeps its column";
    EXPECT_EQ(est.clockCloneColumn("G02"), k2);
    EXPECT_THROW(est.clockCloneColumn("G01"), std::out_of_range);
    EXPECT_TRUE(est.isClockClone(k2));
    EXPECT_FALSE(est.isClockClone(c2));
    EXPECT_FALSE(est.isClockClone(99));
    EXPECT_EQ(est.numClockClones(), 1);
    EXPECT_EQ(est.numShared(), 4);
    EXPECT_EQ(est.sharedColumnLabel(k2), "CLKC:G02");
    EXPECT_EQ(est.sharedColumnLabel(c2), "CLK:G02");
    est.beginEpoch();
    EXPECT_THROW(est.registerClockClone("G01"), std::logic_error);
    est.reset();
    EXPECT_EQ(est.numClockClones(), 0);
    EXPECT_EQ(est.numShared(), 0);
}

// toSolutionEpoch maps the clocks and never the clone: a clone value differing from its
// clock's must not reach the product.
TEST(NetworkClockCloneTest, CloneIsNeverEmitted) {
    NetworkEstimator est;
    est.registerClock("G01");
    est.registerClock("G02");
    est.registerPhaseBias("G02", "L1C");
    est.registerClockClone("G01");
    est.registerClockClone("G02");
    Eigen::VectorXd x(5);
    x << 1.5, -2.5, 0.25, 99.0, -99.0;
    const NetworkSolutionEpoch sol = est.toSolutionEpoch(x, GnssTime{});
    ASSERT_EQ(sol.sats.size(), 2u);
    for (const auto& s : sol.sats) {
        EXPECT_TRUE(s.has_clock);
        if (s.sat == "G01") EXPECT_DOUBLE_EQ(s.clock_c0_m, 1.5);
        if (s.sat == "G02") EXPECT_DOUBLE_EQ(s.clock_c0_m, -2.5);
    }
}

// A clone prior folds by identity onto the clone column (dense and diagonal); the walk
// factor is (e_c - e_k)(e_c - e_k)'/q with the mean on the right-hand side and withdraws
// exactly; card Q3's refloat marginalizes the clock and keeps the clone.
TEST(NetworkClockCloneTest, PriorsFoldByIdentityOntoTheClone) {
    NetworkEstimator est;
    const Eigen::Index c = est.registerClock("G05");
    const Eigen::Index k = est.registerClockClone("G05");
    est.beginEpoch();
    const auto& neq = est.normals();
    NetworkEstimator::SharedPrior pk;
    pk.kind = SharedParamKind::Clock; pk.sat = "G05"; pk.clone = true; pk.value = 0.3;
    Eigen::MatrixXd cov(1, 1);
    cov << 0.04;
    est.addSharedPriorDense({pk}, cov);
    EXPECT_NEAR(neq.sharedNormals()(k, k), 25.0, 1e-9);
    EXPECT_EQ(neq.sharedNormals()(c, c), 0.0);
    EXPECT_NEAR(neq.sharedRhs()(k), 0.3 * 25.0, 1e-9);
    pk.info = 4.0;
    est.addSharedPrior({pk});
    EXPECT_NEAR(neq.sharedNormals()(k, k), 29.0, 1e-9);
    EXPECT_EQ(neq.sharedNormals()(c, c), 0.0);
    const Eigen::MatrixXd N0 = neq.sharedNormals();
    const Eigen::VectorXd u0 = neq.sharedRhs();
    NetworkEstimator::CloneWalk w;
    w.sat = "G05"; w.mean = 0.02; w.q = 0.5;
    EXPECT_EQ(est.addCloneWalk({w}), 1u);
    const Eigen::MatrixXd dN = neq.sharedNormals() - N0;
    const Eigen::VectorXd du = neq.sharedRhs() - u0;
    EXPECT_NEAR(dN(c, c), 2.0, 1e-12);
    EXPECT_NEAR(dN(k, k), 2.0, 1e-12);
    EXPECT_NEAR(dN(c, k), -2.0, 1e-12);
    EXPECT_NEAR(dN(k, c), -2.0, 1e-12);
    EXPECT_NEAR(du(c), 0.04, 1e-12);
    EXPECT_NEAR(du(k), -0.04, 1e-12);
    EXPECT_EQ(est.addCloneWalk({w}, -1.0), 1u);
    EXPECT_NEAR((neq.sharedNormals() - N0).cwiseAbs().maxCoeff(), 0.0, 1e-12);
    EXPECT_NEAR((neq.sharedRhs() - u0).cwiseAbs().maxCoeff(), 0.0, 1e-12);
    w.q = 0.0;
    EXPECT_EQ(est.addCloneWalk({w}), 0u) << "a factor with no walk is skipped";

    // Card Q3's refloat over a block with the clock and its clone: the clock alone is
    // marginalized.  The block re-folded without the clock equals the clone's own prior.
    NetworkEstimator e2;
    const Eigen::Index c2 = e2.registerClock("G05");
    const Eigen::Index k2 = e2.registerClockClone("G05");
    e2.beginEpoch();
    NetworkEstimator::SharedPrior pc;
    pc.kind = SharedParamKind::Clock; pc.sat = "G05"; pc.value = 0.1;
    Eigen::MatrixXd cv(2, 2);
    cv << 0.05, 0.03,
          0.03, 0.04;
    e2.addSharedPriorDense({pc, pk}, cv);
    EXPECT_EQ(e2.refoldSharedPriorDenseDroppingClocks({pc, pk}, cv, {"G05"}), 1u)
        << "the clock is dropped, the clone kept";
    const auto& n2 = e2.normals();
    EXPECT_NEAR(n2.sharedNormals()(c2, c2), 0.0, 1e-9);
    EXPECT_NEAR(n2.sharedNormals()(c2, k2), 0.0, 1e-9);
    EXPECT_NEAR(n2.sharedNormals()(k2, k2), 1.0 / 0.04, 1e-9);
    EXPECT_NEAR(n2.sharedRhs()(k2), 0.3 / 0.04, 1e-9);
}

// The information form of the clones' regression equals the covariance form over the
// free columns; the datum-pinned column takes no coefficient.
TEST(NetworkClockCloneTest, CloneRegressionIsTheGaussianConditional) {
    NetworkEstimator est;
    est.registerClock("G01");                 // 0: the pinned pivot
    est.registerClock("G02");                 // 1
    est.registerClockClone("G02");            // 2
    est.beginEpoch();
    const Eigen::MatrixXd H = gaugeRows(6, 7u, 3);
    est.addSharedConstraintRows(H, Eigen::VectorXd::Zero(H.rows()), 4.0);
    // A prior on the clone alone, as the stand-in fold gives it (keeps J_CC definite).
    NetworkEstimator::SharedPrior pk;
    pk.kind = SharedParamKind::Clock; pk.sat = "G02"; pk.clone = true; pk.info = 9.0;
    est.addSharedPrior({pk});
    const NetworkEstimator::Datum datum{{0, 0.0}};
    est.solve(datum);
    Eigen::MatrixXd K, B;
    ASSERT_TRUE(est.cloneConditional(datum, K, B));
    ASSERT_EQ(K.rows(), 1);
    ASSERT_EQ(K.cols(), 3);
    EXPECT_EQ(K(0, 0), 0.0) << "the pinned column is exactly known";
    EXPECT_EQ(K(0, 2), 0.0);
    // Covariance form over the free columns {1, 2}: K_1 = S_21 / S_11, B = S_22 - K_1 S_12.
    const Eigen::MatrixXd& S = est.sharedCovariance();
    const double K1 = S(2, 1) / S(1, 1);
    EXPECT_NEAR(K(0, 1), K1, 1e-9 * std::max(1.0, std::abs(K1)));
    EXPECT_NEAR(B(0, 0), S(2, 2) - K1 * S(1, 2), 1e-9 * S(2, 2));
    EXPECT_GT(B(0, 0), 0.0);
    // No clone, no regression.
    NetworkEstimator e0;
    e0.registerClock("G01");
    e0.beginEpoch();
    Eigen::MatrixXd K0, B0;
    EXPECT_FALSE(e0.cloneConditional({}, K0, B0));
}

// The gate view marginalizes a referenced clone and drops an idle one; the declared gauge
// stays null, which a principal sub-matrix over the non-clone columns does not keep.
TEST(NetworkClockCloneTest, GateViewKeepsTheGaugeAndDropsIdleClones) {
    SclCtx S("gate");
    NetworkRunContext& ctx = *S.ctx;
    NetworkEstimator est;
    est.registerClock("G01");                         // 0
    est.registerClock("G02");                         // 1
    ctx.carry.ccl.col["G02"] = est.registerClockClone("G02");   // 2: referenced
    ctx.carry.ccl.col["G01"] = est.registerClockClone("G01");   // 3: idle
    est.beginEpoch();
    const Eigen::MatrixXd H = gaugeRows(5, 11u, 4);
    est.addSharedConstraintRows(H, Eigen::VectorXd::Zero(H.rows()), 2.0);
    const Eigen::MatrixXd N = est.reducedSharedNormals();
    EXPECT_EQ(nullCount(N), 2) << "the gauge plus the idle clone";
    Eigen::MatrixXd V;
    ASSERT_TRUE(networkClockCloneGateView(ctx, est, V));
    ASSERT_EQ(V.rows(), 2);
    EXPECT_EQ(nullCount(V), 1) << "the declared gauge alone";
    EXPECT_NEAR((V * Eigen::Vector2d(1.0, 1.0)).norm(), 0.0, 1e-9 * N.norm());
    EXPECT_EQ(nullCount(N.topLeftCorner(2, 2)), 0)
        << "a principal sub-matrix would read the gauge as observed (over-specified)";
    EXPECT_EQ(ctx.counters.ccl.gate_referenced, 1);
    EXPECT_EQ(ctx.counters.ccl.gate_idle, 1);
    // Off: no view, whatever the carry state holds.
    SclCtx off("gate_off", false);
    off.ctx->carry.ccl.col = ctx.carry.ccl.col;
    Eigen::MatrixXd V2;
    EXPECT_FALSE(networkClockCloneGateView(*off.ctx, est, V2));
    EXPECT_EQ(V2.size(), 0);
}

// The harvest marginalization of one row: r_S' += K' r_C, cond_var += r_C' B' r_C, r_C = 0,
// and the clock-family deficit onto the constellation's pinned clock, so the row's sum
// over the clock family (its identity along the declared gauge) is the harvest's; a row
// with no clone term is untouched.
TEST(NetworkClockCloneTest, HarvestRowMarginalizesTheClones) {
    SclCtx S("harvest");
    NetworkRunContext& ctx = *S.ctx;
    // Columns: 0 G01 clock (pinned), 1 G02 clock, 2 G03 clock, 3 clone of G02, 4 clone of G03.
    ClockCloneHarvest h;
    h.on = true;
    h.n_shared = 5;
    h.n0 = 3;
    h.K.resize(2, 5);
    h.K << 0.0, 0.9, 0.1, 0.0, 0.0,
           0.0, 0.2, 0.7, 0.0, 0.0;
    h.B.resize(2, 2);
    h.B << 4e-4, 1e-4,
           1e-4, 9e-4;
    h.col_cc   = {'G', 'G', 'G'};
    h.clone_cc = {'G', 'G'};
    h.pin_col['G'] = 0;
    Eigen::VectorXd row(5);
    row << 0.3, 1.0, -1.0, 0.5, -0.25;
    const double sum0 = row.sum();
    double cv = 1e-6;
    networkClockCloneHarvestRow(h, ctx, 0, 7, row, cv);
    const Eigen::Vector2d rc(0.5, -0.25);
    EXPECT_NEAR(row(1), 1.0 + 0.9 * 0.5 + 0.2 * -0.25, 1e-15);
    EXPECT_NEAR(row(2), -1.0 + 0.1 * 0.5 + 0.7 * -0.25, 1e-15);
    EXPECT_EQ(row(3), 0.0);
    EXPECT_EQ(row(4), 0.0);
    EXPECT_NEAR(row.head(3).sum(), sum0, 1e-15) << "the clock-family sum is the harvest's";
    EXPECT_NEAR(row(0), 0.3 - 0.025, 1e-15) << "the deficit sits on the pinned clock";
    EXPECT_NEAR(cv, 1e-6 + rc.dot(h.B * rc), 1e-18);
    EXPECT_EQ(ctx.counters.ccl.harvest_rows, 1);
    EXPECT_EQ(ctx.counters.ccl.harvest_gauge_held, 1);
    ASSERT_EQ(h.rc.size(), 1u);
    Eigen::VectorXd plain(5);
    plain << 0.0, 1.0, 2.0, 0.0, 0.0;
    double cv2 = 3e-6;
    networkClockCloneHarvestRow(h, ctx, 0, 8, plain, cv2);
    EXPECT_EQ(plain(1), 1.0);
    EXPECT_EQ(plain(2), 2.0);
    EXPECT_EQ(cv2, 3e-6);
    EXPECT_EQ(ctx.counters.ccl.harvest_rows, 1);
}

// Card SMH's row on the clone epoch (pbHoldCloneOnClones): the stood-in clock moves onto its
// clone at Q = 0 (the seam pair touching it dropped, the other keys unchanged), a row's clock
// coefficient comes through the time update unshrunk, its revision reads the clone's
// posterior against the stashed mean, and the walk share still reads card CTU's Q.
TEST(NetworkClockCloneTest, SmoothedRowAdvancesOnTheClones) {
    // P rows: 0 G02 clock, 1 G02 L1C phase bias, 2 G03 clock; all in S at columns 1, 5, 2.
    CouplingExactEpoch xe;
    xe.P.resize(3, 3);
    xe.P << 4e-4, 3e-4, 1e-4,
            3e-4, 5e-4, 1e-4,
            1e-4, 1e-4, 6e-4;
    xe.s_of_p   = {0, 1, 2};
    xe.route_p.assign(3, static_cast<int>(CouplingExactDRoute::Present));
    xe.clk_p    = {1, 0, 1};
    xe.cc_p     = {'G', 'G', 'G'};
    xe.p_of_s   = {0, 1, 2};
    xe.col_of_s = {1, 5, 2};
    xe.q_s      = Eigen::Vector3d(1.5e-4, 0.0, 2e-4);
    xe.shat_s   = Eigen::Vector3d(0.10, 0.20, 0.30);
    xe.q_off    = {{0, 1, 1e-6}, {1, 0, 1e-6}};
    xe.p_of_key[sharedCarryKey(SharedParamKind::Clock, "G02", "")]         = 0;
    xe.p_of_key[sharedCarryKey(SharedParamKind::PhaseBias, "G02", "L1C")] = 1;
    xe.p_of_key[sharedCarryKey(SharedParamKind::Clock, "G03", "")]         = 2;
    ASSERT_TRUE(couplingExactFactor(xe));

    const CouplingExactEpoch same = pbHoldCloneOnClones(xe, {});
    EXPECT_TRUE(same.clone_s.empty());
    EXPECT_EQ(same.col_of_s, xe.col_of_s);

    const CouplingExactEpoch xc = pbHoldCloneOnClones(xe, {{0, 10}});
    ASSERT_TRUE(xc.ok);
    EXPECT_EQ(xc.col_of_s, (std::vector<Eigen::Index>{10, 5, 2}));
    EXPECT_EQ(xc.q_s(0), 0.0);
    EXPECT_EQ(xc.q_s(2), 2e-4);
    EXPECT_EQ(xc.clone_s, (std::vector<char>{1, 0, 0}));
    EXPECT_TRUE(xc.q_off.empty()) << "the seam pair touching the moved clock";

    // The time update: on the clone the coefficient is the harvested one, on card CTU's
    // epoch the walk shrinks it.
    const CouplingExactRow on_clone = couplingExactRow(xc, {{0, -1.0}});
    const CouplingExactRow on_ctu   = couplingExactRow(xe, {{0, -1.0}});
    ASSERT_TRUE(on_clone.ok);
    EXPECT_EQ(on_clone.m_new(0), -1.0);
    EXPECT_EQ(on_clone.var_add, 0.0);
    EXPECT_GT(on_ctu.m_new(0), -0.9);

    // The row through pbHoldCloneAdvanceRow on the clone epoch.
    PbHoldCloneInputs in;
    in.xe      = &xc;
    in.xe_walk = &xe;
    in.desc_of_p.resize(3);
    in.desc_of_p[0].sat = "G02";
    in.desc_of_p[1].kind = SharedParamKind::PhaseBias;
    in.desc_of_p[1].sat = "G02";
    in.desc_of_p[1].signal = "L1C";
    in.desc_of_p[2].sat = "G03";
    in.post_s  = Eigen::Vector3d(0.13, 0.21, 0.29);   // the clone's posterior, then the others'
    in.prior_s = xc.shat_s;
    in.col_of      = [](const PbHoldCloneTerm&) { return Eigen::Index{-1}; };
    in.post_of_col = [](Eigen::Index) { return std::numeric_limits<double>::quiet_NaN(); };
    in.prior_of    = [](const PbHoldCloneTerm&) { return std::numeric_limits<double>::quiet_NaN(); };
    PbHoldCloneTerm t;
    t.sat = "G02"; t.coeff = -1.0; t.s_prev = 0.10; t.p_prev = 4e-4; t.col = 1;
    const PbHoldCloneStep st = pbHoldCloneAdvanceRow(in, "G02", {PbHoldCloneOwn{}}, {t}, 0.10,
                                                     0.0, 0.0);
    ASSERT_TRUE(st.ok);
    ASSERT_EQ(st.terms.size(), 1u);
    EXPECT_EQ(st.terms[0].col, 10);
    EXPECT_EQ(st.terms[0].coeff, -1.0);
    EXPECT_NEAR(st.value, 0.13, 1e-15) << "x0 = the clone's posterior";
    EXPECT_EQ(st.var, 0.0);
    EXPECT_EQ(st.dW, 1.5e-4) << "the walk share reads card CTU's Q";
    in.xe_walk = nullptr;
    EXPECT_EQ(pbHoldCloneAdvanceRow(in, "G02", {PbHoldCloneOwn{}}, {t}, 0.10, 0.0, 0.0).dW, 0.0);
}

// Card SMH's re-expression (pbHoldCloneMarginalizeClones): the coupling rows' harvest rule on
// the row convention, with the same K, B' and gauge restore as HarvestRowMarginalizesTheClones.
TEST(NetworkClockCloneTest, SmoothedRowMarginalizesTheClones) {
    // Columns: 0 G01 clock (pinned), 1 G02 clock, 2 G03 clock, 3 clone of G02, 4 clone of G03.
    Eigen::MatrixXd K(2, 5), B(2, 2);
    K << 0.0, 0.9, 0.1, 0.0, 0.0,
         0.0, 0.2, 0.7, 0.0, 0.0;
    B << 4e-4, 1e-4,
         1e-4, 9e-4;
    const char* sats[] = {"G01", "G02", "G03"};
    const auto term_of_col = [&](Eigen::Index j, PbHoldCloneTerm& t) {
        if (j < 0 || j > 2) return false;
        t.kind   = SharedParamKind::Clock;
        t.sat    = sats[j];
        t.s_prev = 10.0 + static_cast<double>(j);
        t.p_prev = j == 0 ? 0.0 : 1e-4;
        return true;
    };
    PbHoldCloneTerm pin;
    pin.sat = "G01"; pin.col = 0; pin.s_prev = 10.0; pin.p_prev = 0.0;
    const std::map<char, PbHoldCloneTerm> pins{{'G', pin}};
    const auto mk = [](const char* sat, double c, double pp, Eigen::Index col) {
        PbHoldCloneTerm t;
        t.sat = sat; t.coeff = c; t.s_prev = 0.0; t.p_prev = pp; t.col = col;
        return t;
    };
    std::vector<PbHoldCloneTerm> row{mk("G01", 0.3, 0.0, 0), mk("G02", 1.0, 1e-4, 1),
                                     mk("G03", -1.0, 1e-4, 2), mk("G02", 0.5, 1e-4, 3),
                                     mk("G03", -0.25, 1e-4, 4)};
    double v = 1e-6;
    const PbHoldCloneMarg mg = pbHoldCloneMarginalizeClones(row, v, K, B, 3, term_of_col, pins);
    ASSERT_TRUE(mg.ok);
    EXPECT_EQ(mg.n_clone, 2);
    EXPECT_EQ(mg.n_moved, 0) << "merged into the row's own clock terms";
    EXPECT_EQ(mg.n_held, 1);
    ASSERT_EQ(row.size(), 3u);
    double sum = 0.0;
    for (const PbHoldCloneTerm& t : row) {
        EXPECT_LT(t.col, 3) << "no clone term is carried";
        sum += t.coeff;
        if (t.col == 1) EXPECT_NEAR(t.coeff, 1.0 + 0.9 * 0.5 + 0.2 * -0.25, 1e-15);
        if (t.col == 2) EXPECT_NEAR(t.coeff, -1.0 + 0.1 * 0.5 + 0.7 * -0.25, 1e-15);
        if (t.col == 0) EXPECT_NEAR(t.coeff, 0.3 - 0.025, 1e-15) << "the deficit on the pin";
    }
    EXPECT_NEAR(sum, 0.3 + 1.0 - 1.0 + 0.5 - 0.25, 1e-15) << "the clock-family sum is kept";
    const Eigen::Vector2d rc(0.5, -0.25);
    EXPECT_NEAR(v, 1e-6 + rc.dot(B * rc), 1e-18);

    // A column with no identity: its coefficient is dropped and counted.
    std::vector<PbHoldCloneTerm> row2{mk("G02", 0.5, 1e-4, 3)};
    double v2 = 0.0;
    const auto refuse2 = [&](Eigen::Index j, PbHoldCloneTerm& t) {
        return j == 2 ? false : term_of_col(j, t);
    };
    const PbHoldCloneMarg mg2 = pbHoldCloneMarginalizeClones(row2, v2, K, B, 3, refuse2, pins);
    EXPECT_EQ(mg2.n_dropped, 1);
    EXPECT_NEAR(mg2.mass_dropped, 0.1 * 0.5, 1e-15);
    EXPECT_EQ(mg2.n_moved, 1);

    // A row with no clone term is untouched, and a mis-shaped regression refuses.
    std::vector<PbHoldCloneTerm> plain{mk("G02", 1.0, 1e-4, 1)};
    double v3 = 2e-6;
    const PbHoldCloneMarg mg3 = pbHoldCloneMarginalizeClones(plain, v3, K, B, 3, term_of_col, pins);
    EXPECT_TRUE(mg3.ok);
    EXPECT_EQ(mg3.n_clone, 0);
    EXPECT_EQ(plain.size(), 1u);
    EXPECT_EQ(v3, 2e-6);
    std::vector<PbHoldCloneTerm> bad{mk("G02", 0.5, 1e-4, 3)};
    const PbHoldCloneMarg mg4 = pbHoldCloneMarginalizeClones(bad, v3, K, B, 2, term_of_col, pins);
    EXPECT_FALSE(mg4.ok);
    EXPECT_EQ(bad.size(), 1u);
}

TEST(NetworkClockCloneTest, LoaderDefaultOffRequiresAndRefuses) {
    bool armed = true;
    EXPECT_TRUE(sclLoads(sclConfig(true, true, false), "off", &armed));
    EXPECT_FALSE(armed) << "default off";
    // Absent key: off as well.
    std::string absent = sclConfig(true, true, false);
    const auto pos = absent.find("    \"carry_coupling_clock_clone\": false,\n");
    ASSERT_NE(pos, std::string::npos);
    absent.erase(pos, std::string("    \"carry_coupling_clock_clone\": false,\n").size());
    EXPECT_TRUE(sclLoads(absent, "absent", &armed));
    EXPECT_FALSE(armed);
    EXPECT_TRUE(sclLoads(sclConfig(true, true, true), "on", &armed));
    EXPECT_TRUE(armed);
    EXPECT_FALSE(sclLoads(sclConfig(true, true, true, "", /*dense=*/0), "nodense"))
        << "a diagonal handover transform would leave the clones free and referenced";
    EXPECT_FALSE(sclLoads(sclConfig(false, true, true), "noexact"))
        << "the clone is a mode of the exact update";
    EXPECT_FALSE(sclLoads(sclConfig(true, false, true), "nofullcov"))
        << "the clones stand in for the dense block's clocks";
    EXPECT_FALSE(sclLoads(sclConfig(true, true, true, "    \"clock_model\": true,\n"), "clkmodel"))
        << "the clock model carries no clock in the border";
    EXPECT_FALSE(sclLoads(sclConfig(true, true, true, "    \"clock_anchor_gauge_free\": true,\n"),
                          "gaugefree"))
        << "the gauge-free direction would have to include the clones";
    EXPECT_FALSE(sclLoads(sclConfig(true, true, true, "    \"shared_carry_gauge_free\": true,\n"),
                          "sharedgf"))
        << "the same downdate, armed by the dense carry";
}
