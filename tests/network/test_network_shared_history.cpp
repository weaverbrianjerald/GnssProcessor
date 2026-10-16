/**
 * @file test_network_shared_history.cpp
 * @brief Gate U3 of card SCL2's build X2, the shared history
 *        (@c network.carry_coupling_shared_history,
 *        docs/refound/SCL2_level_residue_under_the_clone_2026-10-06.md, sections 5, 6 and 9).
 *
 * 1. The compression (@c sharedHistoryCompress) equals the uncompressed shared term
 *    @f$R\,\Sigma\,R^\top@f$ to 1e-9 when its rank is within the cap; when the cap truncates,
 *    the kept and remainder terms still sum to it and the cross-station remainder is the
 *    counted loss.
 * 2. The estimator: a history column registers after every other column (the clones
 *    included), keyed @c "HIST|j" and labelled @c "HIST:j", folds its prior by identity,
 *    joins Y in the clones' regression, and is never emitted.
 * 3. The seam: registration needs the stash's generation and a referencing row, the fold
 *    is N(0, 1), and the harvest's end writes each row's history coefficients, its remainder
 *    and the within-station F3 term so that the rows' joint law equals the shared term
 *    within the cap.
 * 4. The row builder emits a row's history terms on card CTU's exact route, and a stale
 *    generation or card 1d's route marginalizes them into the row's variance.
 * 5. The loader: default off, requires card SCL's clone, refuses a rank outside [1, 8].
 * 6. The snapshot's write side stores the marginal, @c var plus the coefficients' squares.
 * 7. Gate U5 (amendment A1, build X2b): card SMH's clone rows projected onto the coupling rows'
 *    kept directions ("form p"): the projection's coefficients and remainder for given
 *    directions, the seam at the harvest's end and at the hold's re-harvest, the next epoch's
 *    revision or marginalization of a hold row's history terms, and both knobs' off paths.
 *
 * The recursion-level reference (mode 6 of the unit network against the batch) is
 * test_coupling_long_horizon.cpp.
 */

#include <gtest/gtest.h>

#include "network/state/NetworkRunContext.hpp"
#include "config/GnssConfig.hpp"
#include "config/GnssConfigTypes.hpp"
#include "network/carry/NetworkClockClone.hpp"
#include "network/carry/NetworkCoupling.hpp"
#include "network/carry/NetworkCouplingExact.hpp"
#include "network/solve/NetworkEstimator.hpp"
#include "network/emit/NetworkPbHoldClone.hpp"
#include "network/driver/NetworkRunTypes.hpp"
#include "network/carry/NetworkSharedHistory.hpp"
#include "logging/LogManager.hpp"

#include <Eigen/Dense>

#include <cmath>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <memory>
#include <random>
#include <string>
#include <tuple>
#include <vector>

namespace fs = std::filesystem;
using namespace GnssProcessor;

namespace {

constexpr double kStash = 1000.0;

/// Config text: a two-station network with card SCL's chain (the exact update, the dense
/// carry and B2) and the given clone / shared-history switches; @p extra is spliced into
/// the network section.
std::string shxConfig(bool clone, bool shx, const std::string& extra = "") {
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
        "    \"handover_stransform_dense\": " + b(clone) + ",\n"
        "    \"shared_carry_full_cov\": true,\n"
        "    \"carry_coupling_exact_update\": true,\n"
        "    \"carry_coupling_clock_clone\": " + b(clone) + ",\n"
        "    \"carry_coupling_shared_history\": " + b(shx) + ",\n" + extra +
        "    \"stations\": [\n"
        "      { \"name\": \"AAAA\", \"position_ecef\": [ -2493304.99, -4655214.79, 3565497.67 ] },\n"
        "      { \"name\": \"BBBB\", \"position_ecef\": [ -2281621.75, -1453585.08, 5756964.89 ] }\n"
        "    ]\n"
        "  }\n"
        "}\n";
}

/// Load @p body; @p armed and @p rank receive the resolved knobs.
bool shxLoads(const std::string& body, const char* tag, bool* armed = nullptr, int* rank = nullptr) {
    LogManager log_mgr;
    GnssConfig cfg;
    const fs::path p = fs::temp_directory_path() / (std::string("shx_load_") + tag + ".json");
    { std::ofstream f(p); f << body; }
    const bool ok = cfg.loadFromJson(p, log_mgr);
    std::error_code ec; fs::remove(p, ec);
    if (armed) *armed = cfg.networkCarryCouplingSharedHistory();
    if (rank) *rank = cfg.networkCarryCouplingSharedHistoryRank();
    return ok;
}

/// A run context from a loaded config with the shared history armed (and card SCL's
/// clone), for the seam functions.
struct ShxCtx {
    LogManager log;
    GnssConfig cfg;
    fs::path   path;
    std::unique_ptr<NetworkRunContext> ctx;
    ShxCtx(const char* tag, bool shx = true, const std::string& extra = "") {
        path = fs::temp_directory_path() / (std::string("shx_ctx_") + tag + ".json");
        { std::ofstream f(path); f << shxConfig(true, shx, extra); }
        EXPECT_TRUE(cfg.loadFromJson(path, log));
        ctx = std::make_unique<NetworkRunContext>(cfg, cfg.getNetworkConfig());
        EXPECT_EQ(ctx->knobs.carry_cpl_shx, shx);
    }
    ~ShxCtx() { std::error_code ec; fs::remove(path, ec); }
};

/// A random SPD matrix of size n.
Eigen::MatrixXd randomSpd(Eigen::Index n, std::mt19937& rng, double scale) {
    std::normal_distribution<double> nd(0.0, 1.0);
    Eigen::MatrixXd A(n, n);
    for (Eigen::Index i = 0; i < n; ++i)
        for (Eigen::Index j = 0; j < n; ++j) A(i, j) = nd(rng);
    return scale * (A * A.transpose() + 0.1 * Eigen::MatrixXd::Identity(n, n));
}

/// The kept term plus the remainder's within-group blocks: the law the rows carry.
Eigen::MatrixXd carriedLaw(const SharedHistoryCompression& X, const std::vector<std::size_t>& g) {
    const Eigen::Index m = X.coef.rows();
    Eigen::MatrixXd S = X.coef * X.coef.transpose();
    const Eigen::MatrixXd RR = X.rem * X.rem.transpose();
    for (Eigen::Index i = 0; i < m; ++i)
        for (Eigen::Index j = 0; j < m; ++j)
            if (g[static_cast<std::size_t>(i)] == g[static_cast<std::size_t>(j)]) S(i, j) += RR(i, j);
    return S;
}

}  // namespace

// The compression is exact within the cap: at full rank with the cap at the width of Y, and
// on a low-rank term with the cap at its rank, the kept term plus the within-station
// remainder equals R Sigma R' to 1e-9 and nothing crosses stations.  A binding cap keeps the
// leading directions, the kept and remainder masses sum to the total, the kept term plus the
// whole remainder is still R Sigma R', and the dropped cross-station remainder is positive.
TEST(NetworkSharedHistoryTest, CompressionEqualsTheSharedTermWithinTheCap) {
    std::mt19937 rng(20261006u);
    std::normal_distribution<double> nd(0.0, 1.0);
    const Eigen::Index m = 24, nY = 6;
    std::vector<std::size_t> g(static_cast<std::size_t>(m));
    for (Eigen::Index i = 0; i < m; ++i) g[static_cast<std::size_t>(i)] = static_cast<std::size_t>(i / 6);
    const Eigen::MatrixXd Sig = randomSpd(nY, rng, 1e-5);
    Eigen::MatrixXd R(m, nY);
    for (Eigen::Index i = 0; i < m; ++i)
        for (Eigen::Index j = 0; j < nY; ++j) R(i, j) = nd(rng);
    const Eigen::MatrixXd T = R * Sig * R.transpose();
    const double tn = T.cwiseAbs().maxCoeff();

    // Full rank, cap at the width of Y.
    const SharedHistoryCompression X6 = sharedHistoryCompress(R, Sig, 6, g);
    ASSERT_TRUE(X6.ok);
    EXPECT_FALSE(X6.chol_fallback);
    EXPECT_EQ(X6.rank, 6);
    EXPECT_EQ(X6.kt, 6);
    EXPECT_EQ(X6.rem.cols(), 0);
    EXPECT_LE((X6.coef * X6.coef.transpose() - T).cwiseAbs().maxCoeff(), 1e-9 * tn);
    EXPECT_EQ(X6.cross, 0.0);
    EXPECT_NEAR(X6.total, T.trace(), 1e-9 * T.trace());

    // A rank-2 term (the rows share two directions), cap 2: exact, the rest empty.
    Eigen::MatrixXd A(m, 2), Bm(2, nY);
    for (Eigen::Index i = 0; i < m; ++i)
        for (Eigen::Index j = 0; j < 2; ++j) A(i, j) = nd(rng);
    for (Eigen::Index i = 0; i < 2; ++i)
        for (Eigen::Index j = 0; j < nY; ++j) Bm(i, j) = nd(rng);
    const Eigen::MatrixXd R2 = A * Bm;
    const Eigen::MatrixXd T2 = R2 * Sig * R2.transpose();
    const double t2n = T2.cwiseAbs().maxCoeff();
    const SharedHistoryCompression X2 = sharedHistoryCompress(R2, Sig, 2, g);
    ASSERT_TRUE(X2.ok);
    EXPECT_EQ(X2.rank, 2);
    EXPECT_EQ(X2.kt, 2);
    EXPECT_LE((X2.coef * X2.coef.transpose() - T2).cwiseAbs().maxCoeff(), 1e-9 * t2n);
    EXPECT_LE((carriedLaw(X2, g) - T2).cwiseAbs().maxCoeff(), 1e-9 * t2n);
    EXPECT_LE(X2.remainder, 1e-9 * X2.total);
    EXPECT_LE(X2.cross, 1e-12 * X2.frob_total);
    // A cap above the rank keeps the rank, no more.
    EXPECT_EQ(sharedHistoryCompress(R2, Sig, 5, g).kt, 2);

    // A binding cap (1 of 6): the leading direction is kept.
    const SharedHistoryCompression X1 = sharedHistoryCompress(R, Sig, 1, g);
    ASSERT_TRUE(X1.ok);
    EXPECT_EQ(X1.kt, 1);
    EXPECT_EQ(X1.rem.cols(), 5);
    EXPECT_NEAR(X1.kept + X1.remainder, X1.total, 1e-12 * X1.total);
    EXPECT_NEAR(X1.kept, X1.coef.squaredNorm(), 1e-9 * X1.total);
    EXPECT_LE((X1.coef * X1.coef.transpose() + X1.rem * X1.rem.transpose() - T).cwiseAbs().maxCoeff(),
              1e-9 * tn);
    EXPECT_GT(X1.cross, 0.0);
    EXPECT_GT(X1.kept, X1.total / 6.0) << "the leading direction holds at least the mean share";
    const double tr_law = carriedLaw(X1, g).trace();
    EXPECT_NEAR(tr_law, T.trace(), 1e-9 * T.trace()) << "every row keeps its own variance";

    // A semi-definite Sigma (a zero direction) takes the eigen square root and stays exact.
    Eigen::MatrixXd Ssd = Sig;
    Ssd.row(0).setZero();
    Ssd.col(0).setZero();
    const SharedHistoryCompression Xs = sharedHistoryCompress(R, Ssd, 6, g);
    ASSERT_TRUE(Xs.ok);
    EXPECT_TRUE(Xs.chol_fallback);
    EXPECT_EQ(Xs.rank, 5);
    const Eigen::MatrixXd Ts = R * Ssd * R.transpose();
    EXPECT_LE((Xs.coef * Xs.coef.transpose() - Ts).cwiseAbs().maxCoeff(), 1e-9 * tn);

    // Mis-shaped input refuses.
    EXPECT_FALSE(sharedHistoryCompress(R, Eigen::MatrixXd::Identity(5, 5), 2, g).ok);
}

// A history column is the last column, keyed and labelled apart from every satellite key,
// idempotent, registration after beginEpoch throws; its prior folds by identity onto it
// (value 0, information 1, nothing on the clock); it joins Y after the clone in the
// regression; toSolutionEpoch never emits it.
TEST(NetworkSharedHistoryTest, HistoryColumnRegistersLastFoldsByIdentityAndIsNeverEmitted) {
    NetworkEstimator est;
    const Eigen::Index c1 = est.registerClock("G01");
    const Eigen::Index c2 = est.registerClock("G02");
    const Eigen::Index k2 = est.registerClockClone("G02");
    const Eigen::Index h0 = est.registerHistoryColumn(0);
    const Eigen::Index h1 = est.registerHistoryColumn(1);
    EXPECT_EQ(c1, 0);
    EXPECT_EQ(c2, 1);
    EXPECT_EQ(k2, 2);
    EXPECT_EQ(h0, 3);
    EXPECT_EQ(h1, 4);
    EXPECT_EQ(est.registerHistoryColumn(0), h0) << "idempotent";
    EXPECT_EQ(est.historyColumn(1), h1);
    EXPECT_THROW(est.historyColumn(2), std::out_of_range);
    EXPECT_TRUE(est.isHistoryColumn(h0));
    EXPECT_FALSE(est.isHistoryColumn(k2));
    EXPECT_FALSE(est.isClockClone(h0));
    EXPECT_FALSE(est.isHistoryColumn(99));
    EXPECT_EQ(est.numClockClones(), 1);
    EXPECT_EQ(est.numHistoryColumns(), 2);
    EXPECT_EQ(est.numBorderNuisance(), 3);
    EXPECT_EQ(est.sharedColumnLabel(h1), "HIST:1");
    EXPECT_EQ(est.sharedColumnLabel(k2), "CLKC:G02");
    SharedParamKind kind;
    std::string sat, sig;
    EXPECT_FALSE(est.sharedColumnIdentity(h0, kind, sat, sig));
    EXPECT_EQ(est.clockColumn("G02"), c2) << "the clock keeps its column";

    // toSolutionEpoch: a history value must not reach the product.
    Eigen::VectorXd x(5);
    x << 1.5, -2.5, 77.0, 99.0, -99.0;
    const NetworkSolutionEpoch sol = est.toSolutionEpoch(x, GnssTime{});
    ASSERT_EQ(sol.sats.size(), 2u);
    for (const auto& s : sol.sats) {
        EXPECT_FALSE(s.sat.empty());
        EXPECT_TRUE(s.has_clock);
        if (s.sat == "G01") EXPECT_DOUBLE_EQ(s.clock_c0_m, 1.5);
        if (s.sat == "G02") EXPECT_DOUBLE_EQ(s.clock_c0_m, -2.5);
    }

    est.beginEpoch();
    EXPECT_THROW(est.registerHistoryColumn(2), std::logic_error);
    const auto& neq = est.normals();
    NetworkEstimator::SharedPrior ph;
    ph.kind = SharedParamKind::Clock; ph.hist = true; ph.hist_idx = 1; ph.value = 0.0; ph.info = 1.0;
    NetworkEstimator::SharedPrior absent = ph;
    absent.hist_idx = 5;   // not in the layout: skipped
    absent.info = 3.0;
    est.addSharedPrior({ph, absent});
    EXPECT_EQ(neq.sharedNormals()(h1, h1), 1.0);
    EXPECT_EQ(neq.sharedNormals()(h0, h0), 0.0);
    EXPECT_EQ(neq.sharedNormals()(c1, c1), 0.0) << "the empty satellite never reaches a clock";
    EXPECT_EQ(neq.sharedNormals().cwiseAbs().sum(), 1.0);
    EXPECT_EQ(neq.sharedRhs().cwiseAbs().sum(), 0.0);

    // The regression over Y = [clone; history]: rows and B' over three columns.
    NetworkEstimator e2;
    e2.registerClock("G01");          // 0: pinned
    e2.registerClock("G02");          // 1
    e2.registerClockClone("G02");     // 2
    e2.registerHistoryColumn(0);      // 3
    e2.beginEpoch();
    Eigen::MatrixXd H(5, 4);
    H << 1.0, -0.5, -0.5, 0.3,
        -0.2, 0.9, -0.7, -0.4,
         0.4, 0.1, -0.5, 0.8,
         0.0, 1.0, -1.0, 0.0,
        -0.6, 0.2, 0.4, 0.5;
    e2.addSharedConstraintRows(H, Eigen::VectorXd::Zero(5), 4.0);
    NetworkEstimator::SharedPrior pk;
    pk.kind = SharedParamKind::Clock; pk.sat = "G02"; pk.clone = true; pk.info = 9.0;
    NetworkEstimator::SharedPrior p0 = ph;
    p0.hist_idx = 0;
    NetworkEstimator::SharedPrior pc;
    pc.kind = SharedParamKind::Clock; pc.sat = "G02"; pc.info = 2.0;
    e2.addSharedPrior({pk, p0, pc});
    const NetworkEstimator::Datum datum{{0, 0.0}};
    e2.solve(datum);
    Eigen::MatrixXd K, B;
    ASSERT_TRUE(e2.cloneConditional(datum, K, B));
    ASSERT_EQ(K.rows(), 2);
    ASSERT_EQ(B.rows(), 2);
    // Covariance form over the free non-Y column {1}: K = S_Y1 / S_11, B = S_YY - K S_1Y.
    const Eigen::MatrixXd& S = e2.sharedCovariance();
    for (int a = 0; a < 2; ++a) {
        const double Ka = S(2 + a, 1) / S(1, 1);
        EXPECT_NEAR(K(a, 1), Ka, 1e-9 * std::max(1.0, std::abs(Ka)));
        EXPECT_EQ(K(a, 0), 0.0) << "the pinned column is exactly known";
        EXPECT_EQ(K(a, 2), 0.0);
        EXPECT_EQ(K(a, 3), 0.0);
        for (int b = 0; b < 2; ++b)
            EXPECT_NEAR(B(a, b), S(2 + a, 2 + b) - S(2 + a, 1) * S(1, 2 + b) / S(1, 1),
                        1e-8 * std::sqrt(S(2 + a, 2 + a) * S(2 + b, 2 + b)));
    }

    est.reset();
    EXPECT_EQ(est.numHistoryColumns(), 0);
    EXPECT_EQ(est.numBorderNuisance(), 0);
}

// Registration: armed, with a compression of the stash's generation, a direction referenced
// by a valid row is registered after the clones and an unreferenced one is not; a different
// generation registers nothing.  The fold is N(0, 1) on each registered column; off, nothing.
TEST(NetworkSharedHistoryTest, RegistrationNeedsTheGenerationAndAReferenceAndFoldsUnitPriors) {
    ShxCtx S("reg");
    NetworkRunContext& ctx = *S.ctx;
    ctx.carry.cxu_stash_valid = true;
    ctx.carry.cxu_stash_s     = kStash;
    ctx.carry.shx.valid   = true;
    ctx.carry.shx.stash_s = kStash;
    ctx.carry.shx.kt      = 3;
    LocalCarry a{2.0, 1e-4, kStash};
    a.shx_coef = {0.01, 0.0, 0.0};
    a.shx_stash_s = kStash;
    LocalCarry z{0.1, 1e-4, kStash};
    z.shx_coef = {0.0, 0.0, -0.02};
    z.shx_stash_s = kStash;
    LocalCarry stale{1.0, 1e-4, kStash};   // re-stashed without history terms: ignored
    stale.shx_coef = {0.0, 0.5, 0.0};
    stale.shx_stash_s = kStash - 30.0;
    net::putArc(ctx.entities, std::make_tuple(0, std::string("G02"), std::string("L1C")), &net::ArcState::amb) = a;
    net::put(ctx.entities.stations, StationId{1}, &net::StationState::zwd) = z;
    net::putArc(ctx.entities, std::make_tuple(1, std::string("G02"), std::string("L1C")), &net::ArcState::amb) = stale;

    NetworkEstimator est;
    est.registerClock("G01");
    est.registerClock("G02");
    ctx.carry.ccl.col["G02"] = est.registerClockClone("G02");
    networkSharedHistoryRegister(ctx, est);
    ASSERT_EQ(ctx.carry.shx.col.size(), 3u);
    EXPECT_EQ(ctx.carry.shx.col[0], 3) << "after the clone";
    EXPECT_EQ(ctx.carry.shx.col[1], -1) << "only a stale row referenced it";
    EXPECT_EQ(ctx.carry.shx.col[2], 4);
    EXPECT_TRUE(ctx.carry.shx.on);
    EXPECT_EQ(est.numHistoryColumns(), 2);
    EXPECT_EQ(ctx.counters.shx.unreferenced, 1);
    EXPECT_EQ(ctx.counters.shx.registered, 2);

    est.beginEpoch();
    EpochWorkspace ws;
    networkSharedHistoryFold(ctx, ws, est, /*within_epoch_armed=*/true);
    const Eigen::MatrixXd& N = est.normals().sharedNormals();
    EXPECT_EQ(N(3, 3), 1.0);
    EXPECT_EQ(N(4, 4), 1.0);
    EXPECT_EQ(N.cwiseAbs().sum(), 2.0);
    EXPECT_EQ(est.normals().sharedRhs().cwiseAbs().sum(), 0.0);
    EXPECT_EQ(ws.cap_shx.size(), 2u);
    networkSharedHistoryReplayFold(ctx, ws, est);
    EXPECT_EQ(est.normals().sharedNormals()(3, 3), 2.0) << "the replay folds the capture again";

    // Another generation: nothing registered.
    ctx.carry.cxu_stash_s = kStash + 30.0;
    NetworkEstimator e2;
    e2.registerClock("G01");
    networkSharedHistoryRegister(ctx, e2);
    EXPECT_FALSE(ctx.carry.shx.on);
    EXPECT_EQ(e2.numHistoryColumns(), 0);
    EXPECT_EQ(ctx.counters.shx.gen_mismatch, 1);

    // Off: nothing, whatever the carry holds.
    ShxCtx off("reg_off", false);
    off.ctx->carry.cxu_stash_valid = true;
    off.ctx->carry.cxu_stash_s = kStash;
    off.ctx->carry.shx = ctx.carry.shx;
    net::assignArc(off.ctx->entities, &net::ArcState::amb, net::arcMap(ctx.entities.arcs, &net::ArcState::amb));
    NetworkEstimator e3;
    e3.registerClock("G01");
    networkSharedHistoryRegister(*off.ctx, e3);
    EXPECT_EQ(e3.numHistoryColumns(), 0);
}

// The harvest's end: each noted row gets its history coefficients (stamped with the harvest)
// and its remainder on its variance, the F3 block its within-station remainder, and the
// rows' joint law (kept term plus within-station remainder) equals R Sigma R' when the rank
// is within the cap; a binding cap keeps every row's variance and drops only cross-station
// remainder.
TEST(NetworkSharedHistoryTest, HarvestEndKeepsTheSharedTermAcrossStations) {
    std::mt19937 rng(7u);
    std::normal_distribution<double> nd(0.0, 1.0);
    for (const int ncl : {2, 1}) {   // cap = rank x clones: 2 (exact, rank 2) and 1 (binding)
        ShxCtx S(ncl == 2 ? "end2" : "end1");
        NetworkRunContext& ctx = *S.ctx;
        ctx.carry.ccl.col["G02"] = 5;
        if (ncl == 2) ctx.carry.ccl.col["G03"] = 6;
        ClockCloneHarvest h;
        h.on = true;
        h.shx = true;
        h.n_shared = 7;
        h.n0 = 5;
        h.B = randomSpd(2, rng, 1e-5);
        // Two stations, two ambiguities each, in one F3 block per station.
        std::vector<std::tuple<int, std::string, Eigen::Index>> rows = {
            {0, "G02", 10}, {0, "G03", 11}, {1, "G02", 20}, {1, "G03", 21}};
        SharedHistoryHarvest H = networkSharedHistoryHarvestBegin(ctx, h);
        ASSERT_TRUE(H.on);
        Eigen::MatrixXd R(4, 2);
        std::vector<double> var0;
        for (int i = 0; i < 4; ++i) {
            const auto& [sta, sat, col] = rows[static_cast<std::size_t>(i)];
            Eigen::Vector2d rc(nd(rng), nd(rng));
            R.row(i) = rc.transpose();
            h.rc[{static_cast<std::size_t>(sta), col}] = rc;
            LocalCarry lc{1.0 + i, 2e-4 + 1e-5 * i, kStash};
            var0.push_back(lc.var);
            net::putArc(ctx.entities, std::make_tuple(sta, sat, std::string("L1C")), &net::ArcState::amb) = lc;
            networkSharedHistoryNoteEntry(H, static_cast<std::size_t>(sta), col,
                                          SharedHistoryMap::Amb, sta, sat, "L1C");
        }
        // A noted row with no Y term and a lost row (its entry erased).
        networkSharedHistoryNoteEntry(H, 0, 12, SharedHistoryMap::Zwd, 0);
        h.rc[{1, 22}] = Eigen::Vector2d(0.3, 0.1);
        networkSharedHistoryNoteEntry(H, 1, 22, SharedHistoryMap::Iono, 1, "G09");
        std::vector<std::shared_ptr<PlHarvestBlock>> blk(2);
        for (int s = 0; s < 2; ++s) {
            blk[static_cast<std::size_t>(s)] = std::make_shared<PlHarvestBlock>();
            blk[static_cast<std::size_t>(s)]->C = Eigen::Matrix2d::Zero();
            networkClockCloneHarvestBlock(h, static_cast<std::size_t>(s), blk[static_cast<std::size_t>(s)],
                                          {static_cast<Eigen::Index>(10 * (s + 1)),
                                           static_cast<Eigen::Index>(10 * (s + 1) + 1)});
        }
        networkClockCloneHarvestEnd(h, ctx);   // returns at once under X2
        EXPECT_EQ(blk[0]->C.cwiseAbs().sum(), 0.0);
        networkSharedHistoryHarvestEnd(H, h, ctx, kStash);
        EXPECT_TRUE(ctx.carry.shx.valid);
        EXPECT_EQ(ctx.carry.shx.stash_s, kStash);
        EXPECT_EQ(ctx.carry.shx.kt, ncl);
        EXPECT_EQ(ctx.counters.shx.rows_compressed, 4);
        EXPECT_EQ(ctx.counters.shx.rows_lost, 1);
        const Eigen::MatrixXd T = R * h.B * R.transpose();
        Eigen::MatrixXd coef(4, ncl), law = Eigen::MatrixXd::Zero(4, 4);
        for (int i = 0; i < 4; ++i) {
            const auto& [sta, sat, col] = rows[static_cast<std::size_t>(i)];
            (void)col;
            const LocalCarry& lc = (*net::getArc(ctx.entities.arcs, std::make_tuple(sta, sat, std::string("L1C")), &net::ArcState::amb));
            ASSERT_TRUE(sharedHistoryCoefValid(lc));
            ASSERT_EQ(lc.shx_coef.size(), static_cast<std::size_t>(ncl));
            for (int j = 0; j < ncl; ++j) coef(i, j) = lc.shx_coef[static_cast<std::size_t>(j)];
            law(i, i) = lc.var - var0[static_cast<std::size_t>(i)];   // the remainder on the diagonal
        }
        law += coef * coef.transpose();
        for (int s = 0; s < 2; ++s) {
            law(2 * s, 2 * s + 1) += blk[static_cast<std::size_t>(s)]->C(0, 1);
            law(2 * s + 1, 2 * s) += blk[static_cast<std::size_t>(s)]->C(1, 0);
            EXPECT_EQ(blk[static_cast<std::size_t>(s)]->C(0, 1), blk[static_cast<std::size_t>(s)]->C(1, 0));
        }
        const double tn = T.cwiseAbs().maxCoeff();
        for (int i = 0; i < 4; ++i) {
            EXPECT_NEAR(law(i, i), T(i, i), 1e-12 * tn) << "every row keeps its own variance";
            for (int j = 0; j < 4; ++j) {
                const bool same = (i / 2) == (j / 2);
                if (ncl == 2 || same) EXPECT_NEAR(law(i, j), T(i, j), 1e-9 * tn) << i << "," << j;
            }
        }
        if (ncl == 1) {
            ASSERT_EQ(ctx.counters.shx.cross_share.size(), 1u);
            EXPECT_GT(ctx.counters.shx.cross_share[0], 0.0);
            EXPECT_EQ(ctx.counters.shx.kt_capped, 1);
        } else {
            EXPECT_EQ(ctx.counters.shx.kt_capped, 0);
            EXPECT_GT(ctx.counters.shx.f3_pairs, -1);
        }
    }
    // Inert record (no regression): the held compression is dropped.
    ShxCtx S("end_inert");
    S.ctx->carry.shx.valid = true;
    ClockCloneHarvest h0;
    SharedHistoryHarvest H0 = networkSharedHistoryHarvestBegin(*S.ctx, h0);
    EXPECT_FALSE(H0.on);
    networkSharedHistoryHarvestEnd(H0, h0, *S.ctx, kStash);
    EXPECT_FALSE(S.ctx->carry.shx.valid);
}

namespace {

/// Two registered clocks and a phase bias, two history columns after them, the layout
/// frozen, and card CTU's exact epoch over the three keys stashed at kStash (the template
/// of test_wsx_within_station_cross.cpp).
struct RowFixture {
    NetworkEstimator est;
    GlobalVtecShConfig shc{};
    net::SatSignalTable tu_reinit_hist;   // the reinit history (card NOM step N4d)
    StationAssembly R;
    CouplingExactEpoch ep;
    SharedHistoryState shx;
    Eigen::Index h0{-1}, h1{-1};

    RowFixture() {
        const Eigen::Index c1 = est.registerClock("G01");
        const Eigen::Index p1 = est.registerPhaseBias("G01", "L1C");
        const Eigen::Index c2 = est.registerClock("G02");
        h0 = est.registerHistoryColumn(0);
        h1 = est.registerHistoryColumn(1);
        est.beginEpoch();
        R.A_s = Eigen::MatrixXd::Zero(0, est.numShared());
        R.A_r = Eigen::MatrixXd::Zero(0, 10);
        R.w.resize(0);
        R.y.resize(0);
        const std::vector<std::string> keys = {sharedCarryKey(SharedParamKind::Clock, "G01", ""),
                                               sharedCarryKey(SharedParamKind::PhaseBias, "G01", "L1C"),
                                               sharedCarryKey(SharedParamKind::Clock, "G02", "")};
        ep.stash_s = kStash;
        ep.P.resize(3, 3);
        ep.P << 0.20, 0.15, 0.05,
                0.15, 0.18, 0.04,
                0.05, 0.04, 0.12;
        for (int p = 0; p < 3; ++p) ep.p_of_key[keys[static_cast<std::size_t>(p)]] = p;
        ep.s_of_p   = {0, 1, 2};
        ep.p_of_s   = {0, 1, 2};
        ep.route_p  = std::vector<int>(3, static_cast<int>(CouplingExactDRoute::Present));
        ep.clk_p    = {1, 0, 1};
        ep.cc_p     = {'G', 'G', 'G'};
        ep.col_of_s = {c1, p1, c2};
        ep.q_s.resize(3);
        ep.q_s << 0.03, 0.0, 0.03;
        ep.shat_s.resize(3);
        ep.shat_s << 1.5, 0.3, -0.7;
        EXPECT_TRUE(couplingExactFactor(ep));
        shx.valid   = true;
        shx.stash_s = kStash;
        shx.kt      = 2;
        shx.col     = {h0, h1};
        shx.on      = true;
    }

    /// One row through buildCplRow, with or without the shared history.
    PendCplRow row(const LocalCarry& lc, bool with_shx) {
        CouplingRowBuilder cb{est, shc, 1e-4, 1e-4, 1e-4, /*tu=*/true, /*gauge=*/false,
                              /*reinit=*/false, tu_reinit_hist, R};
        cb.cxu = &ep;
        if (with_shx) cb.shx = &shx;
        cb.buildCplRow(4, 0.1, lc, 0.02, 30.0);
        EXPECT_EQ(cb.crows.size(), 1u);
        return cb.crows.empty() ? PendCplRow{} : cb.crows.back();
    }
};

LocalCarry rowCarry(double seen_s) {
    LocalCarry lc{2.0, 0.02, seen_s};
    const SharedParamKind kinds[3] = {SharedParamKind::Clock, SharedParamKind::PhaseBias,
                                      SharedParamKind::Clock};
    const char* sats[3] = {"G01", "G01", "G02"};
    const char* sigs[3] = {"", "L1C", ""};
    const double m[3] = {-1.2, 0.9, 0.1};
    const double shat[3] = {1.5, 0.3, -0.7};
    const double pp[3] = {0.20, 0.18, 0.12};
    for (int p = 0; p < 3; ++p)
        lc.coupling.push_back(CoupledShared{kinds[p], sats[p], sigs[p], 0, 0, 'C', 0, m[p], shat[p], pp[p]});
    lc.shx_coef    = {0.004, -0.003};
    lc.shx_stash_s = seen_s;
    return lc;
}

double coefOn(const PendCplRow& r, Eigen::Index col) {
    for (const auto& [c, m] : r.shared)
        if (c == col) return m;
    return 0.0;
}

}  // namespace

// The row builder: on the exact route a row emits its history coefficients on the registered
// columns, with the same right-hand side and weight as without them; a stale generation, a
// column not registered and card 1d's route marginalize them (variance += their squares);
// a re-stash without history terms (stale stamp) ignores them.
TEST(NetworkSharedHistoryTest, RowBuilderEmitsHistoryTermsAndAStaleGenerationMarginalizes) {
    const double m2 = 0.004 * 0.004 + 0.003 * 0.003;
    {
        RowFixture f;
        const LocalCarry lc = rowCarry(kStash);
        const PendCplRow r0 = f.row(lc, false);
        const PendCplRow r1 = f.row(lc, true);
        EXPECT_EQ(coefOn(r0, f.h0), 0.0);
        EXPECT_EQ(coefOn(r1, f.h0), 0.004);
        EXPECT_EQ(coefOn(r1, f.h1), -0.003);
        EXPECT_EQ(r1.shared.size(), r0.shared.size() + 2);
        EXPECT_EQ(r1.y, r0.y) << "the prior mean is 0";
        EXPECT_EQ(r1.w, r0.w) << "emitted, not marginalized";
        EXPECT_EQ(f.R.cxu.rows_hist, 1);
        EXPECT_EQ(f.R.cxu.terms_hist, 2);
        EXPECT_EQ(f.R.cxu.rows_hist_marg, 0);
    }
    {   // A stale generation: the registered columns belong to another compression.
        RowFixture f;
        f.shx.stash_s = kStash + 30.0;
        const LocalCarry lc = rowCarry(kStash);
        const PendCplRow r0 = f.row(lc, false);
        const PendCplRow r1 = f.row(lc, true);
        EXPECT_EQ(coefOn(r1, f.h0), 0.0);
        EXPECT_EQ(coefOn(r1, f.h1), 0.0);
        EXPECT_NEAR(1.0 / r1.w, 1.0 / r0.w + m2, 1e-15);
        EXPECT_EQ(f.R.cxu.rows_hist_marg, 1);
        EXPECT_EQ(f.R.cxu.terms_hist_marg, 2);
    }
    {   // One column not registered: that term alone is marginalized.
        RowFixture f;
        f.shx.col[1] = -1;
        const LocalCarry lc = rowCarry(kStash);
        const PendCplRow r0 = f.row(lc, false);
        const PendCplRow r1 = f.row(lc, true);
        EXPECT_EQ(coefOn(r1, f.h0), 0.004);
        EXPECT_EQ(coefOn(r1, f.h1), 0.0);
        EXPECT_NEAR(1.0 / r1.w, 1.0 / r0.w + 0.003 * 0.003, 1e-15);
    }
    {   // Card 1d's route (the row was harvested before the stash).
        RowFixture f;
        const LocalCarry lc = rowCarry(kStash - 30.0);
        const PendCplRow r0 = f.row(lc, false);
        const PendCplRow r1 = f.row(lc, true);
        EXPECT_EQ(coefOn(r1, f.h0), 0.0);
        EXPECT_NEAR(1.0 / r1.w, 1.0 / r0.w + m2, 1e-15);
        EXPECT_EQ(f.R.cxu.rows_stale, 2);
        EXPECT_EQ(f.R.cxu.rows_hist_marg, 1);
    }
    {   // Re-stashed without history terms: the old coefficients are ignored.
        RowFixture f;
        LocalCarry lc = rowCarry(kStash);
        lc.shx_stash_s = kStash - 30.0;
        const PendCplRow r0 = f.row(lc, false);
        const PendCplRow r1 = f.row(lc, true);
        EXPECT_EQ(r1.shared, r0.shared);
        EXPECT_EQ(r1.w, r0.w);
        EXPECT_EQ(f.R.cxu.rows_hist + f.R.cxu.rows_hist_marg, 0);
    }
}

TEST(NetworkSharedHistoryTest, LoaderDefaultOffRequiresTheCloneAndRefusesARankOutsideItsRange) {
    bool armed = true;
    int rank = 0;
    EXPECT_TRUE(shxLoads(shxConfig(true, false), "off", &armed, &rank));
    EXPECT_FALSE(armed) << "default off";
    EXPECT_EQ(rank, 1) << "gate U2's decision";
    std::string absent = shxConfig(true, false);
    const std::string line = "    \"carry_coupling_shared_history\": false,\n";
    const auto pos = absent.find(line);
    ASSERT_NE(pos, std::string::npos);
    absent.erase(pos, line.size());
    EXPECT_TRUE(shxLoads(absent, "absent", &armed));
    EXPECT_FALSE(armed);
    EXPECT_TRUE(shxLoads(shxConfig(true, true), "on", &armed, &rank));
    EXPECT_TRUE(armed);
    EXPECT_EQ(rank, 1);
    EXPECT_TRUE(shxLoads(shxConfig(true, true, "    \"carry_coupling_shared_history_rank\": 8,\n"),
                         "rank8", &armed, &rank));
    EXPECT_EQ(rank, 8);
    EXPECT_FALSE(shxLoads(shxConfig(false, true), "noclone"))
        << "the history is the clones' shared conditional";
    EXPECT_FALSE(shxLoads(shxConfig(true, true, "    \"carry_coupling_shared_history_rank\": 0,\n"),
                          "rank0"));
    EXPECT_FALSE(shxLoads(shxConfig(true, true, "    \"carry_coupling_shared_history_rank\": 9,\n"),
                          "rank9"));
    EXPECT_FALSE(shxLoads(shxConfig(true, false, "    \"carry_coupling_shared_history_rank\": 9,\n"),
                          "rank9off"))
        << "the range is checked off too";
}

// The snapshot's write side under X2 stores the marginal variance: var plus the squares of
// valid history coefficients; a stale stamp or no coefficient writes var.
TEST(NetworkSharedHistoryTest, SnapshotWriteStoresTheMarginal) {
    LocalCarry lc{1.0, 4e-4, kStash};
    EXPECT_EQ(sharedHistoryMarginalVar(lc), 4e-4);
    lc.shx_coef = {0.01, -0.02};
    lc.shx_stash_s = kStash;
    EXPECT_TRUE(sharedHistoryCoefValid(lc));
    EXPECT_NEAR(sharedHistoryMarginalVar(lc), 4e-4 + 1e-4 + 4e-4, 1e-18);
    lc.shx_stash_s = kStash - 30.0;
    EXPECT_FALSE(sharedHistoryCoefValid(lc));
    EXPECT_EQ(sharedHistoryMarginalVar(lc), 4e-4);
}

// ---------------------------------------------------------------------------------------------
// Gate U5 (card SCL2 section 11, amendment A1, build X2b): card SMH's clone rows in the history.
// ---------------------------------------------------------------------------------------------

namespace {

/// The smoothed hold's prerequisites, with the hold itself on or off (spliced into shxConfig).
std::string holdExtra(bool smoothed) {
    const auto b = [](bool v) { return std::string(v ? "true" : "false"); };
    return "    \"emit_pb_hold\": true,\n"
           "    \"emit_pb_hold_smoothed\": " + b(smoothed) + ",\n"
           "    \"raim\": true,\n"
           "    \"raim_within_epoch\": true,\n"
           "    \"clock_temporal_anchor\": true,\n";
}

/// A random orthonormal n x n basis.
Eigen::MatrixXd randomBasis(Eigen::Index n, std::mt19937& rng) {
    std::normal_distribution<double> nd(0.0, 1.0);
    Eigen::MatrixXd A(n, n);
    for (Eigen::Index i = 0; i < n; ++i)
        for (Eigen::Index j = 0; j < n; ++j) A(i, j) = nd(rng);
    const Eigen::HouseholderQR<Eigen::MatrixXd> qr(A);
    return qr.householderQ() * Eigen::MatrixXd::Identity(n, n);
}

/// A live clone row with a pending Y term at epoch @p ep.
PbHoldClone pendingClone(const std::string& sat, const Eigen::VectorXd& rc, double rc_var,
                         long long ep, double var) {
    PbHoldClone c;
    c.sat        = sat;
    c.signal     = "L1C";
    c.alive      = true;
    c.var        = var;
    c.shx.rc     = rc;
    c.shx.rc_var = rc_var;
    c.shx.rc_ep  = ep;
    return c;
}

}  // namespace

// U5 (a): a hold row's projection for given directions.  The history coefficients are
// F_T V_kt and the remainder ||F_T V_rest||^2 with F_T = r' L, to 1e-12; the two sum to the
// row's whole term r' Sigma r; a coupling row projected onto its own compression reproduces
// the compression's coefficients and remainder; a width mismatch refuses.
TEST(NetworkSharedHistoryTest, HoldRowProjectionKeepsItsCoordinatesOnTheKeptDirections) {
    std::mt19937 rng(115u);
    std::normal_distribution<double> nd(0.0, 1.0);
    const Eigen::Index nY = 5;
    const Eigen::MatrixXd Sig = randomSpd(nY, rng, 2e-5);
    const Eigen::MatrixXd L = Sig.llt().matrixL();
    const Eigen::MatrixXd V = randomBasis(nY, rng);
    Eigen::VectorXd r(nY);
    for (Eigen::Index i = 0; i < nY; ++i) r(i) = nd(rng);
    for (const int kt : {0, 2, 5}) {
        const SharedHistoryProjection P = sharedHistoryProject(r, L, V, kt);
        ASSERT_TRUE(P.ok);
        ASSERT_EQ(P.coef.size(), kt);
        const Eigen::RowVectorXd F = r.transpose() * L;
        const Eigen::RowVectorXd ck = F * V.leftCols(kt);
        const double rem = (F * V.rightCols(nY - kt)).squaredNorm();
        const double tot = r.dot(Sig * r);
        for (int j = 0; j < kt; ++j) EXPECT_NEAR(P.coef(j), ck(j), 1e-12 * std::sqrt(tot));
        EXPECT_NEAR(P.rem, rem, 1e-12 * tot);
        EXPECT_NEAR(P.total, tot, 1e-12 * tot);
        EXPECT_NEAR(P.coef.squaredNorm() + P.rem, tot, 1e-12 * tot) << "the row keeps its own variance";
    }
    EXPECT_FALSE(sharedHistoryProject(Eigen::VectorXd::Ones(4), L, V, 2).ok);
    EXPECT_FALSE(sharedHistoryProject(r, L, V, 6).ok);

    // A row of the compression itself, projected on its factor and directions, is the
    // compression's own row (the hold row's form is the coupling rows' form).
    const Eigen::Index m = 12;
    std::vector<std::size_t> g(static_cast<std::size_t>(m));
    Eigen::MatrixXd R(m, nY);
    for (Eigen::Index i = 0; i < m; ++i) {
        g[static_cast<std::size_t>(i)] = static_cast<std::size_t>(i / 4);
        for (Eigen::Index j = 0; j < nY; ++j) R(i, j) = nd(rng);
    }
    const SharedHistoryCompression X = sharedHistoryCompress(R, Sig, 2, g);
    ASSERT_TRUE(X.ok);
    ASSERT_EQ(X.L.rows(), nY);
    ASSERT_EQ(X.V.cols(), nY);
    for (Eigen::Index i = 0; i < m; ++i) {
        const SharedHistoryProjection P = sharedHistoryProject(R.row(i).transpose(), X.L, X.V, X.kt);
        ASSERT_TRUE(P.ok);
        const double tot = R.row(i).dot(Sig * R.row(i).transpose());
        for (int j = 0; j < X.kt; ++j) EXPECT_NEAR(P.coef(j), X.coef(i, j), 1e-12 * std::sqrt(tot));
        EXPECT_NEAR(P.rem, X.rem.row(i).squaredNorm(), 1e-12 * tot);
    }
}

// U5 (b): the seam at the harvest's end.  Armed with the smoothed hold, every live clone row
// pending from this epoch's update (card SMH's clock row; card SMD's two rows) takes its
// projection's coefficients (stamped with the compression) and its remainder on its variance;
// a row pending from another epoch or dead is not touched; with no compression the row takes
// the per-row term (counted as a failure); the hold's re-harvest gives any row still pending
// the per-row term.
TEST(NetworkSharedHistoryTest, HarvestEndProjectsTheHoldRowsOntoTheCompression) {
    ShxCtx S("hold_proj", true, holdExtra(true));
    NetworkRunContext& ctx = *S.ctx;
    ASSERT_TRUE(ctx.knobs.emit_pb_hold_smoothed);
    std::mt19937 rng(9u);
    std::normal_distribution<double> nd(0.0, 1.0);
    ctx.counters.n_loop_epochs = 7;   // so the previous epoch is a valid pending stamp
    const long long ep = static_cast<long long>(ctx.counters.n_loop_epochs);
    const Eigen::Index nY = 4;
    const Eigen::MatrixXd Sig = randomSpd(nY, rng, 1e-5);
    Eigen::MatrixXd R(10, nY);
    std::vector<std::size_t> g(10);
    for (Eigen::Index i = 0; i < 10; ++i) {
        g[static_cast<std::size_t>(i)] = static_cast<std::size_t>(i / 5);
        for (Eigen::Index j = 0; j < nY; ++j) R(i, j) = nd(rng);
    }
    const SharedHistoryCompression X = sharedHistoryCompress(R, Sig, 2, g);
    ASSERT_TRUE(X.ok);
    ASSERT_EQ(X.kt, 2);
    Eigen::VectorXd r1(nY), r2(nY), r3(nY);
    for (Eigen::Index i = 0; i < nY; ++i) {
        r1(i) = nd(rng);
        r2(i) = nd(rng);
        r3(i) = nd(rng);
    }
    const double v1 = r1.dot(Sig * r1), v2 = r2.dot(Sig * r2), v3 = r3.dot(Sig * r3);
    net::putSig(ctx.entities, "G05|L1C", &net::SatSignalState::smh_clone) = pendingClone("G05", r1, v1, ep, 1e-6);
    net::putSig(ctx.entities, "G07|L1C", &net::SatSignalState::smh_clone) = pendingClone("G07", r2, v2, ep - 1, 2e-6);   // another epoch
    PbHoldClone dead = pendingClone("G09", r2, v2, ep, 3e-6);
    dead.alive = false;
    net::putSig(ctx.entities, "G09|L1C", &net::SatSignalState::smh_clone) = dead;
    PbHoldClone blk = pendingClone("G11", r2, v2, ep, 4e-6);
    blk.block        = true;
    blk.pv_var       = 5e-6;
    blk.pv_shx.rc    = r3;
    blk.pv_shx.rc_var = v3;
    blk.pv_shx.rc_ep = ep;
    net::putSig(ctx.entities, "G11|L1C", &net::SatSignalState::smh_block) = blk;

    networkSharedHistoryProjectHoldRows(ctx, &X, kStash);
    const auto check = [&](const PbHoldCloneShx& sh, double var, double var0, const Eigen::VectorXd& r) {
        const SharedHistoryProjection P = sharedHistoryProject(r, X.L, X.V, X.kt);
        ASSERT_EQ(sh.coef.size(), 2u);
        for (int j = 0; j < 2; ++j) EXPECT_EQ(sh.coef[static_cast<std::size_t>(j)], P.coef(j));
        EXPECT_EQ(sh.stash_s, kStash);
        EXPECT_EQ(var, var0 + P.rem);
        EXPECT_EQ(sh.rc_ep, -1) << "no longer pending";
        EXPECT_EQ(sh.rc.size(), 0);
    };
    check((*net::getSig(ctx.entities.sat_signals, "G05|L1C", &net::SatSignalState::smh_clone)).shx, (*net::getSig(ctx.entities.sat_signals, "G05|L1C", &net::SatSignalState::smh_clone)).var, 1e-6, r1);
    check((*net::getSig(ctx.entities.sat_signals, "G11|L1C", &net::SatSignalState::smh_block)).shx, (*net::getSig(ctx.entities.sat_signals, "G11|L1C", &net::SatSignalState::smh_block)).var, 4e-6, r2);
    check((*net::getSig(ctx.entities.sat_signals, "G11|L1C", &net::SatSignalState::smh_block)).pv_shx, (*net::getSig(ctx.entities.sat_signals, "G11|L1C", &net::SatSignalState::smh_block)).pv_var, 5e-6, r3);
    EXPECT_EQ((*net::getSig(ctx.entities.sat_signals, "G07|L1C", &net::SatSignalState::smh_clone)).shx.rc_ep, ep - 1) << "not this epoch's pending";
    EXPECT_EQ((*net::getSig(ctx.entities.sat_signals, "G07|L1C", &net::SatSignalState::smh_clone)).var, 2e-6);
    EXPECT_EQ((*net::getSig(ctx.entities.sat_signals, "G09|L1C", &net::SatSignalState::smh_clone)).var, 3e-6) << "a dead row is not touched";
    EXPECT_EQ(ctx.counters.shx.hold_projected, 3);
    EXPECT_EQ(ctx.counters.shx.hold_proj_failed, 0);
    ASSERT_EQ(ctx.counters.shx.hold_kept_share.size(), 3u);
    EXPECT_NEAR(ctx.counters.shx.hold_kept_share[0] + ctx.counters.shx.hold_rem_share[0], 1.0, 1e-12);

    // The re-harvest's finalize: the row pending from the previous epoch takes the per-row term.
    networkSharedHistoryFinalizeHoldRows(ctx, ep);
    EXPECT_EQ((*net::getSig(ctx.entities.sat_signals, "G07|L1C", &net::SatSignalState::smh_clone)).var, 2e-6 + v2);
    EXPECT_EQ((*net::getSig(ctx.entities.sat_signals, "G07|L1C", &net::SatSignalState::smh_clone)).shx.rc_ep, -1);
    EXPECT_TRUE((*net::getSig(ctx.entities.sat_signals, "G07|L1C", &net::SatSignalState::smh_clone)).shx.coef.empty());
    EXPECT_EQ(ctx.counters.shx.hold_nocomp_unreached, 1);
    EXPECT_EQ(ctx.counters.shx.hold_proj_failed, 0);
    EXPECT_EQ((*net::getSig(ctx.entities.sat_signals, "G05|L1C", &net::SatSignalState::smh_clone)).var, 1e-6 + sharedHistoryProject(r1, X.L, X.V, 2).rem)
        << "a projected row is final";

    // No compression this epoch (a harvest without the regression, a failed compression):
    // the per-row term, counted.
    net::putSig(ctx.entities, "G13|L1C", &net::SatSignalState::smh_clone) = pendingClone("G13", r3, v3, ep, 6e-6);
    networkSharedHistoryProjectHoldRows(ctx, nullptr, kStash);
    EXPECT_EQ((*net::getSig(ctx.entities.sat_signals, "G13|L1C", &net::SatSignalState::smh_clone)).var, 6e-6 + v3);
    EXPECT_TRUE((*net::getSig(ctx.entities.sat_signals, "G13|L1C", &net::SatSignalState::smh_clone)).shx.coef.empty());
    EXPECT_EQ(ctx.counters.shx.hold_nocomp_end, 1);
    EXPECT_EQ(ctx.counters.shx.hold_proj_failed, 0);
    // A width mismatch (the hold's Y is not the compression's) is a failure too.
    net::putSig(ctx.entities, "G15|L1C", &net::SatSignalState::smh_clone) = pendingClone("G15", Eigen::VectorXd::Ones(3), 0.5, ep, 0.0);
    networkSharedHistoryProjectHoldRows(ctx, &X, kStash);
    EXPECT_EQ((*net::getSig(ctx.entities.sat_signals, "G15|L1C", &net::SatSignalState::smh_clone)).var, 0.5);
    EXPECT_EQ(ctx.counters.shx.hold_proj_failed, 1);
}

// U5 (c): the next epoch.  A hold row's history coefficients of the compression whose columns
// are registered become terms on those columns (p_prev 1, s_prev 0) and revise its value by
// their posterior; a stale generation marginalizes every coefficient into the row's variance
// (h^2 each); a column not registered marginalizes that coefficient alone; the coefficients
// are consumed.
TEST(NetworkSharedHistoryTest, HoldRowHistoryTermsAreRevisedOrMarginalizedOnAStaleGeneration) {
    SharedHistoryState S;
    S.valid   = true;
    S.stash_s = kStash;
    S.kt      = 2;
    S.col     = {5, 6};
    S.on      = true;
    Eigen::VectorXd x(8);
    x << 0.1, 0.2, 0.3, 0.4, 0.5, 0.7, -0.9, 0.0;
    const auto make = [](double stamp) {
        PbHoldCloneShx sh;
        sh.coef    = {0.004, -0.003};
        sh.stash_s = stamp;
        return sh;
    };
    const double m2 = 0.004 * 0.004 + (-0.003) * (-0.003);
    {   // The registered generation: revised with the history columns.
        PbHoldCloneShx sh = make(kStash);
        std::vector<PbHoldCloneTerm> terms(1);
        double value = 1.0, var = 1e-6;
        const PbHoldCloneHistAttach a = pbHoldCloneAttachHistory(S, sh, x, terms, value, var);
        EXPECT_EQ(a.n_terms, 2);
        EXPECT_EQ(a.n_marg, 0);
        ASSERT_EQ(terms.size(), 3u);
        EXPECT_EQ(terms[1].col, 5);
        EXPECT_EQ(terms[1].coeff, 0.004);
        EXPECT_EQ(terms[1].p_prev, 1.0);
        EXPECT_EQ(terms[1].s_prev, 0.0);
        EXPECT_TRUE(terms[1].sat.empty()) << "no clock family: no deficit";
        EXPECT_EQ(terms[2].col, 6);
        double v = 1.0;
        v -= 0.004 * 0.7;
        v -= -0.003 * -0.9;
        EXPECT_EQ(value, v);
        EXPECT_EQ(var, 1e-6);
        EXPECT_TRUE(sh.coef.empty()) << "consumed";
    }
    {   // A stale generation: every coefficient marginalized.
        PbHoldCloneShx sh = make(kStash - 30.0);
        std::vector<PbHoldCloneTerm> terms;
        double value = 1.0, var = 1e-6;
        const PbHoldCloneHistAttach a = pbHoldCloneAttachHistory(S, sh, x, terms, value, var);
        EXPECT_EQ(a.n_terms, 0);
        EXPECT_EQ(a.n_marg, 2);
        EXPECT_TRUE(terms.empty());
        EXPECT_EQ(value, 1.0);
        EXPECT_NEAR(var, 1e-6 + m2, 1e-18);
        EXPECT_NEAR(a.marg_var, m2, 1e-18);
        EXPECT_TRUE(sh.coef.empty()) << "consumed";
    }
    {   // No history registered this epoch (shx.on false): marginalized too.
        SharedHistoryState S0 = S;
        S0.on = false;
        PbHoldCloneShx sh = make(kStash);
        std::vector<PbHoldCloneTerm> terms;
        double value = 1.0, var = 0.0;
        EXPECT_EQ(pbHoldCloneAttachHistory(S0, sh, x, terms, value, var).n_marg, 2);
        EXPECT_NEAR(var, m2, 1e-18);
    }
    {   // One column not registered: that coefficient alone.
        SharedHistoryState S1 = S;
        S1.col[1] = -1;
        PbHoldCloneShx sh = make(kStash);
        std::vector<PbHoldCloneTerm> terms;
        double value = 1.0, var = 0.0;
        const PbHoldCloneHistAttach a = pbHoldCloneAttachHistory(S1, sh, x, terms, value, var);
        EXPECT_EQ(a.n_terms, 1);
        EXPECT_EQ(a.n_marg, 1);
        EXPECT_EQ(var, 0.003 * 0.003);
        EXPECT_EQ(value, 1.0 - 0.004 * 0.7);
    }
    {   // No coefficient: nothing.
        PbHoldCloneShx sh;
        std::vector<PbHoldCloneTerm> terms;
        double value = 1.0, var = 2.0;
        const PbHoldCloneHistAttach a = pbHoldCloneAttachHistory(S, sh, x, terms, value, var);
        EXPECT_EQ(a.n_terms + a.n_marg, 0);
        EXPECT_EQ(value, 1.0);
        EXPECT_EQ(var, 2.0);
        EXPECT_TRUE(terms.empty());
    }
}

// U5 (d): off is bitwise unchanged.  With the shared history off the amendment's hold seams
// touch nothing (a pending record left in place, the variance untouched), and the clone
// marginalization without the deferral is the shipped form bit for bit (the deferred form
// differs only by the per-row term it reports instead of adding).  With the smoothed hold
// off, the harvest's end writes the coupling rows exactly as it does with no hold row in the
// carry, and leaves a stray pending hold row alone.
TEST(NetworkSharedHistoryTest, AmendmentIsBitwiseInertWithTheHistoryOrTheSmoothedHoldOff) {
    // The shared history off, the smoothed hold on.
    {
        ShxCtx S("hold_off_x2", false, holdExtra(true));
        NetworkRunContext& ctx = *S.ctx;
        ASSERT_TRUE(ctx.knobs.emit_pb_hold_smoothed);
        const long long ep = static_cast<long long>(ctx.counters.n_loop_epochs);
        net::putSig(ctx.entities, "G05|L1C", &net::SatSignalState::smh_clone) = pendingClone("G05", Eigen::VectorXd::Ones(2), 0.25, ep, 1e-6);
        networkSharedHistoryProjectHoldRows(ctx, nullptr, kStash);
        networkSharedHistoryFinalizeHoldRows(ctx, ep);
        EXPECT_EQ((*net::getSig(ctx.entities.sat_signals, "G05|L1C", &net::SatSignalState::smh_clone)).var, 1e-6);
        EXPECT_EQ((*net::getSig(ctx.entities.sat_signals, "G05|L1C", &net::SatSignalState::smh_clone)).shx.rc_ep, ep);
        EXPECT_EQ(ctx.counters.shx.hold_proj_failed + ctx.counters.shx.hold_projected
                      + ctx.counters.shx.hold_nocomp_end + ctx.counters.shx.hold_nocomp_unreached, 0);
    }
    // The clone marginalization: no deferral (the default) is the shipped form.
    {
        std::vector<PbHoldCloneTerm> t0(3);
        t0[0].kind = SharedParamKind::Clock;
        t0[0].sat = "G05";
        t0[0].coeff = -0.8;
        t0[0].col = 1;
        t0[0].p_prev = 0.1;
        t0[1].kind = SharedParamKind::Clock;
        t0[1].sat = "G05";
        t0[1].coeff = -0.2;
        t0[1].col = 3;
        t0[1].p_prev = 0.1;
        t0[2].kind = SharedParamKind::Clock;   // a history term (no satellite)
        t0[2].coeff = 0.01;
        t0[2].col = 4;
        t0[2].p_prev = 1.0;
        Eigen::MatrixXd K(2, 5), B(2, 2);
        K << 0.0, 0.7, 0.1, 0.0, 0.0,
             0.0, 0.05, 0.2, 0.0, 0.0;
        B << 4e-6, 1e-6,
             1e-6, 3e-6;
        const std::function<bool(Eigen::Index, PbHoldCloneTerm&)> term_of_col =
            [](Eigen::Index j, PbHoldCloneTerm& t) {
                t.kind   = j == 1 ? SharedParamKind::Clock : SharedParamKind::PhaseBias;
                t.sat    = "G05";
                t.signal = j == 1 ? "" : "L1C";
                t.s_prev = 0.0;
                t.p_prev = 0.1;
                return true;
            };
        std::map<char, PbHoldCloneTerm> pin;
        pin['G'].sat = "G01";
        pin['G'].col = 0;
        std::vector<PbHoldCloneTerm> ta = t0, tb = t0, tc = t0;
        double va = 1e-6, vb = 1e-6, vc = 1e-6;
        const PbHoldCloneMarg ma = pbHoldCloneMarginalizeClones(ta, va, K, B, 3, term_of_col, pin);
        const PbHoldCloneMarg mb = pbHoldCloneMarginalizeClones(tb, vb, K, B, 3, term_of_col, pin, nullptr);
        Eigen::VectorXd rc;
        const PbHoldCloneMarg mc = pbHoldCloneMarginalizeClones(tc, vc, K, B, 3, term_of_col, pin, &rc);
        ASSERT_TRUE(ma.ok && mb.ok && mc.ok);
        EXPECT_EQ(va, vb);
        ASSERT_EQ(ta.size(), tb.size());
        ASSERT_EQ(ta.size(), tc.size());
        for (std::size_t i = 0; i < ta.size(); ++i) {
            EXPECT_EQ(ta[i].coeff, tb[i].coeff);
            EXPECT_EQ(ta[i].col, tb[i].col);
            EXPECT_EQ(ta[i].coeff, tc[i].coeff) << "the deferral moves the same coefficients";
            EXPECT_EQ(ta[i].col, tc[i].col);
        }
        EXPECT_EQ(vc, 1e-6) << "deferred: the per-row term is not added";
        EXPECT_EQ(mc.var_add, ma.var_add);
        EXPECT_EQ(va, 1e-6 + ma.var_add);
        ASSERT_EQ(rc.size(), 2);
        EXPECT_EQ(rc(0), -0.2);
        EXPECT_EQ(rc(1), 0.01);
        EXPECT_EQ(ma.var_add, rc.dot(B * rc));
    }
    // The smoothed hold off, the shared history on: the harvest's end with and without a hold
    // row in the carry writes the same bits.
    {
        std::mt19937 rng(31u);
        std::normal_distribution<double> nd(0.0, 1.0);
        ClockCloneHarvest h;
        h.on       = true;
        h.shx      = true;
        h.n_shared = 6;
        h.n0       = 4;
        h.B        = randomSpd(2, rng, 1e-5);
        std::vector<Eigen::VectorXd> rcs;
        for (int i = 0; i < 4; ++i) rcs.push_back(Eigen::Vector2d(nd(rng), nd(rng)));
        const auto runEnd = [&](const char* tag, bool with_hold) {
            auto S = std::make_unique<ShxCtx>(tag, true, holdExtra(false));
            NetworkRunContext& ctx = *S->ctx;
            EXPECT_FALSE(ctx.knobs.emit_pb_hold_smoothed);
            ctx.carry.ccl.col["G02"] = 4;
            ctx.carry.ccl.col["G03"] = 5;
            ClockCloneHarvest hh = h;
            SharedHistoryHarvest H = networkSharedHistoryHarvestBegin(ctx, hh);
            for (int i = 0; i < 4; ++i) {
                const int sta = i / 2;
                const std::string sat = (i % 2) ? "G03" : "G02";
                const Eigen::Index col = 10 * (sta + 1) + (i % 2);
                hh.rc[{static_cast<std::size_t>(sta), col}] = rcs[static_cast<std::size_t>(i)];
                net::putArc(ctx.entities, std::make_tuple(sta, sat, std::string("L1C")), &net::ArcState::amb) = LocalCarry{1.0, 1e-4, kStash};
                networkSharedHistoryNoteEntry(H, static_cast<std::size_t>(sta), col, SharedHistoryMap::Amb, sta,
                                              sat, "L1C");
            }
            if (with_hold)
                net::putSig(ctx.entities, "G05|L1C", &net::SatSignalState::smh_clone) = pendingClone(
                    "G05", Eigen::VectorXd::Ones(2), 0.25, static_cast<long long>(ctx.counters.n_loop_epochs), 1e-6);
            networkSharedHistoryHarvestEnd(H, hh, ctx, kStash);
            if (with_hold) {
                EXPECT_EQ((*net::getSig(ctx.entities.sat_signals, "G05|L1C", &net::SatSignalState::smh_clone)).var, 1e-6) << "the hold off: not projected";
                EXPECT_TRUE((*net::getSig(ctx.entities.sat_signals, "G05|L1C", &net::SatSignalState::smh_clone)).shx.coef.empty());
            }
            return S;
        };
        const auto A  = runEnd("hold_off_a", false);
        const auto Bc = runEnd("hold_off_b", true);
        ASSERT_EQ(net::present(A->ctx->entities.arcs, &net::ArcState::amb), 4u);
        for (const auto& [key, lc] : net::arcMap(A->ctx->entities.arcs, &net::ArcState::amb)) {
            const LocalCarry& lb = (*net::getArc(Bc->ctx->entities.arcs, key, &net::ArcState::amb));
            EXPECT_EQ(lc.var, lb.var);
            EXPECT_EQ(lc.shx_coef, lb.shx_coef);
            EXPECT_EQ(lc.shx_stash_s, lb.shx_stash_s);
            EXPECT_FALSE(lc.shx_coef.empty());
        }
        EXPECT_EQ(A->ctx->carry.shx.kt, Bc->ctx->carry.shx.kt);
        EXPECT_EQ(Bc->ctx->counters.shx.hold_projected + Bc->ctx->counters.shx.hold_proj_failed
                      + Bc->ctx->counters.shx.hold_nocomp_end, 0);
    }
}

// The clock gauge with the pivot a Dirichlet-pinned border column that receives the deficits
// (card SCL2 section 11, reading 2; the guard of the attribution's T2 note, parts (a) to (c)).
// (a) After the harvest's row step and its end, each carried row's clock-family sum over the
// clocks (the pinned pivot included) equals its sum over the clocks and the clones before,
// with the history at weight 0, to 1e-12; the Y terms are gone and the conditional variance is
// deferred.  (b) The unpinned reduced normals of gauge-blind rows plus the history's N(0, 1)
// prior annihilate the gauge direction (every clock and clone of the constellation, the
// history at 0), relative 1e-10: the history prior adds nothing along the gauge.  (c) With the
// pivot pinned, the solution does not move when every row's pivot coefficient takes a
// constant.
TEST(NetworkSharedHistoryTest, KeepsTheClockGaugeWithAPinnedPivotColumn) {
    ShxCtx S("gauge");
    NetworkRunContext& ctx = *S.ctx;
    std::mt19937 rng(424242u);
    std::normal_distribution<double> nd(0.0, 1.0);
    // The layout: three clocks (G01 the pivot), a phase bias, the two clones, one history column.
    const auto layout = [&](NetworkEstimator& est) {
        EXPECT_EQ(est.registerClock("G01"), 0);
        EXPECT_EQ(est.registerClock("G02"), 1);
        EXPECT_EQ(est.registerClock("G03"), 2);
        EXPECT_EQ(est.registerPhaseBias("G02", "L1C"), 3);
        EXPECT_EQ(est.registerClockClone("G02"), 4);
        EXPECT_EQ(est.registerClockClone("G03"), 5);
        EXPECT_EQ(est.registerHistoryColumn(0), 6);
    };
    const Eigen::Index n = 7;
    const std::vector<Eigen::Index> fam = {0, 1, 2, 4, 5};   // the G clock family: clocks and clones
    // Gauge-blind rows: each row's family coefficients sum to zero; the history column is free.
    const Eigen::Index k = 14;
    Eigen::MatrixXd H(k, n);
    for (Eigen::Index i = 0; i < k; ++i) {
        for (Eigen::Index j = 0; j < n; ++j) H(i, j) = nd(rng);
        double s = 0.0;
        for (const Eigen::Index j : fam) s += H(i, j);
        for (const Eigen::Index j : fam) H(i, j) -= s / static_cast<double>(fam.size());
    }
    Eigen::VectorXd z(k);
    for (Eigen::Index i = 0; i < k; ++i) z(i) = 0.01 * nd(rng);
    ctx.carry.ccl.col["G02"] = 4;
    ctx.carry.ccl.col["G03"] = 5;
    ctx.carry.shx.valid   = true;
    ctx.carry.shx.stash_s = kStash;
    ctx.carry.shx.kt      = 1;
    ctx.carry.shx.col     = {6};
    ctx.carry.shx.on      = true;
    const auto build = [&](NetworkEstimator& est, const Eigen::MatrixXd& Hr) {
        layout(est);
        est.beginEpoch();
        est.addSharedConstraintRows(Hr, z, 1e4);
        EpochWorkspace ws;
        networkSharedHistoryFold(ctx, ws, est, false);   // N(0, 1) on the history column
    };

    // (b) The unpinned reduced normals annihilate the gauge direction.
    NetworkEstimator est;
    build(est, H);
    const Eigen::MatrixXd N = est.reducedSharedNormals();
    ASSERT_EQ(N.rows(), n);
    EXPECT_NEAR(N(6, 6), H.col(6).squaredNorm() * 1e4 + 1.0, 1e-12 * N(6, 6)) << "the history prior is N(0, 1)";
    Eigen::VectorXd gvec = Eigen::VectorXd::Zero(n);
    for (const Eigen::Index j : fam) gvec(j) = 1.0;
    EXPECT_LE((N * gvec).norm(), 1e-10 * N.norm() * gvec.norm());

    // (c) The pivot pinned: a constant on every row's pivot coefficient moves nothing.
    const NetworkEstimator::Datum datum{{0, 0.0}};
    const Eigen::VectorXd x0 = est.solve(datum);
    Eigen::MatrixXd H2 = H;
    H2.col(0).array() += 0.37;
    NetworkEstimator est2;
    build(est2, H2);
    const Eigen::VectorXd x2 = est2.solve(datum);
    ASSERT_EQ(x0.size(), n);
    EXPECT_EQ(x0(0), 0.0);
    EXPECT_LE((x2 - x0).cwiseAbs().maxCoeff(), 1e-10 * std::max(1e-12, x0.cwiseAbs().maxCoeff()));

    // (a) The harvest on the pinned pivot: family sums kept, the history at weight 0.
    std::map<Eigen::Index, CoupledShared> col2desc;
    for (int c = 0; c < 3; ++c) {
        CoupledShared d;
        d.kind = SharedParamKind::Clock;
        d.sat  = c == 0 ? "G01" : (c == 1 ? "G02" : "G03");
        col2desc[c] = d;
    }
    CoupledShared pb;
    pb.kind = SharedParamKind::PhaseBias;
    pb.sat = "G02";
    pb.signal = "L1C";
    col2desc[3] = pb;
    ClockCloneHarvest h = networkClockCloneHarvestBegin(ctx, est, datum, col2desc);
    ASSERT_TRUE(h.on);
    ASSERT_TRUE(h.shx);
    ASSERT_EQ(h.n0, 4);
    ASSERT_EQ(h.B.rows(), 3) << "Y = the two clones and the history column";
    ASSERT_EQ(h.pin_col.at('G'), 0);
    SharedHistoryHarvest Hh = networkSharedHistoryHarvestBegin(ctx, h);
    ASSERT_TRUE(Hh.on);
    std::vector<Eigen::VectorXd> rows_after;
    std::vector<double> fam_before;
    for (int i = 0; i < 6; ++i) {
        Eigen::VectorXd row(n);
        for (Eigen::Index j = 0; j < n; ++j) row(j) = nd(rng);
        double fb = 0.0;
        for (const Eigen::Index j : fam) fb += row(j);   // history (col 6) at weight 0
        fam_before.push_back(fb);
        double cond_var = 2e-4;
        const int sta = i / 3;
        const Eigen::Index col = 10 + i;
        networkClockCloneHarvestRow(h, ctx, static_cast<std::size_t>(sta), col, row, cond_var);
        EXPECT_EQ(cond_var, 2e-4) << "X2 defers the conditional variance";
        EXPECT_EQ(row.segment(4, 3).cwiseAbs().maxCoeff(), 0.0) << "the Y terms are gone";
        rows_after.push_back(row);
        net::putArc(ctx.entities, std::make_tuple(sta, std::string(i % 3 == 0 ? "G02" : "G03"),
                                              std::string("L") + std::to_string(i)), &net::ArcState::amb) = LocalCarry{1.0, 2e-4, kStash};
        networkSharedHistoryNoteEntry(Hh, static_cast<std::size_t>(sta), col, SharedHistoryMap::Amb, sta,
                                      i % 3 == 0 ? "G02" : "G03", std::string("L") + std::to_string(i));
    }
    networkClockCloneHarvestEnd(h, ctx);
    networkSharedHistoryHarvestEnd(Hh, h, ctx, kStash);
    EXPECT_EQ(ctx.counters.shx.rows_compressed, 6);
    for (std::size_t i = 0; i < rows_after.size(); ++i) {
        const Eigen::VectorXd& row = rows_after[i];
        const double fa = row(0) + row(1) + row(2);   // the clocks, the pinned pivot included
        EXPECT_NEAR(fa, fam_before[i], 1e-12 * std::max(1.0, std::abs(fam_before[i]))) << "row " << i;
    }
}
