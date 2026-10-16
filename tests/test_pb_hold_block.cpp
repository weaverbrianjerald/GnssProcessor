/**
 * @file test_pb_hold_block.cpp
 * @brief Unit gates of the smoothed hold's (c, p_1, p_2) block clone, an
 *        instrument of @c network.emit_pb_hold_smoothed (its wire selection, the
 *        removed key @c emit_pb_hold_smoothed_block, is gone).
 *
 * 1. The phase-visible row, advanced by @ref pbHoldCloneAdvanceRow and re-harvested by
 *    @ref pbHoldCloneRebase, equals the fixed-point smoother of D = c - sum_j w_j p_j (an
 *    augmented filter carrying D(t0) as a zero-noise state) at every epoch;
 *    @ref pbHoldCloneRowValueVar is that filter's variance of D(t0), non-increasing.
 * 2. The row is the functional of the three single-key rows, its time-update variance the
 *    block's with every cross term (@ref couplingExactCross), and its fold inflation g'Qg
 *    with the seam's off-diagonal noise.
 * 3. The pair: one phase bias per band by the smallest stashed variance, weights blind to
 *    the level and to the ionosphere.
 * 4. The block clone retires at a re-reference, a new anchor tenure and an emitter mark of
 *    its pair; it is born with its pair from the stash.  The clock clone writes the wire,
 *    and a switch to the bridge keeps the epoch's clock step.
 * 5. The S line: an older product file's pbhold=4 still reads as a smoothed hold.
 */

#include <gtest/gtest.h>

#include "support/NetworkTestRun.hpp"   // the run and epoch the clone seams' epoch forms take
#include "network/state/RunContext.hpp"
#include "common/SignalBand.hpp"
#include "config/GnssConfig.hpp"
#include "config/GnssConfigTypes.hpp"
#include "network/carry/CouplingExact.hpp"
#include "network/emit/PbHold.hpp"
#include "network/emit/PbHoldClone.hpp"
#include "network/driver/RunTypes.hpp"
#include "logging/LogManager.hpp"
#include "network/diag/RunTraces.hpp"
#include "products/NetworkProductEmitter.hpp"
#include "products/NetworkProductIO.hpp"
#include "products/SsrCorrectionStore.hpp"
#include "rinex/MultiRinexSync.hpp"

#include <spdlog/spdlog.h>
#include <spdlog/sinks/null_sink.h>

#include <Eigen/Dense>

#include <cmath>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <random>
#include <sstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace GnssProcessor {
namespace {

// --- The dense walk model (with the level walking on G01) -----------------

/// The five border keys: G01's clock and its L1C and L2W phase biases, G02's clock and L1C.
std::vector<PbHoldCloneTerm> modelKeys() {
    std::vector<PbHoldCloneTerm> k(5);
    k[0].kind = SharedParamKind::Clock;     k[0].sat = "G01";
    k[1].kind = SharedParamKind::PhaseBias; k[1].sat = "G01"; k[1].signal = "L1C";
    k[2].kind = SharedParamKind::PhaseBias; k[2].sat = "G01"; k[2].signal = "L2W";
    k[3].kind = SharedParamKind::Clock;     k[3].sat = "G02";
    k[4].kind = SharedParamKind::PhaseBias; k[4].sat = "G02"; k[4].signal = "L1C";
    for (int i = 0; i < 5; ++i) k[static_cast<std::size_t>(i)].col = i;
    return k;
}

std::string keyOf(const PbHoldCloneTerm& t) { return sharedCarryKey(t.kind, t.sat, t.signal); }

/// The per-epoch walk: clocks 1e-3 m2, phase biases 1e-6 m2.
Eigen::VectorXd walkQ() {
    Eigen::VectorXd q(5);
    q << 1e-3, 1e-6, 1e-6, 1e-3, 1e-6;
    return q;
}

/// One epoch's rows: code on each clock, phase c - p, a code-like ionosphere row per band
/// pair of G01, and a random row.
void epochRows(std::mt19937& rng, Eigen::MatrixXd& H, Eigen::VectorXd& r) {
    std::normal_distribution<double> nd(0.0, 1.0);
    H.setZero(6, 5);
    r.resize(6);
    H(0, 0) = 1.0;                 r(0) = 0.25;
    H(1, 3) = 1.0;                 r(1) = 0.25;
    H(2, 0) = 1.0; H(2, 1) = -1.0; r(2) = 1e-4;
    H(3, 0) = 1.0; H(3, 2) = -1.0; r(3) = 1e-4;
    H(4, 3) = 1.0; H(4, 4) = -1.0; r(4) = 1e-4;
    for (int j = 0; j < 5; ++j) H(5, j) = 0.3 * nd(rng);
    r(5) = 1e-2;
}

void kfUpdate(Eigen::VectorXd& x, Eigen::MatrixXd& P, const Eigen::MatrixXd& H,
              const Eigen::VectorXd& r, const Eigen::VectorXd& y) {
    const Eigen::MatrixXd R = r.asDiagonal();
    const Eigen::MatrixXd S = H * P * H.transpose() + R;
    const Eigen::MatrixXd K = P * H.transpose() * S.ldlt().solve(Eigen::MatrixXd::Identity(S.rows(), S.cols()));
    x = x + K * (y - H * x);
    const Eigen::MatrixXd IKH = Eigen::MatrixXd::Identity(P.rows(), P.cols()) - K * H;
    P = IKH * P * IKH.transpose() + K * R * K.transpose();
    P = 0.5 * (P + P.transpose()).eval();
}

::testing::AssertionResult nearRel(double a, double b, double tol) {
    if (std::abs(a - b) <= tol * std::max(1.0, std::abs(b))) return ::testing::AssertionSuccess();
    return ::testing::AssertionFailure() << a << " vs " << b << " (tol " << tol << " relative)";
}

/// The exact-coupling epoch inputs over the stash (x, P), every key in S with the walk as its
/// inflation, and optionally the seam's off-diagonal noise @p off between G01's clock and
/// its L1C phase bias.
CouplingExactEpoch exactEpoch(const Eigen::VectorXd& x, const Eigen::MatrixXd& P,
                              const Eigen::VectorXd& q, double stash_s, double off = 0.0) {
    const auto keys = modelKeys();
    CouplingExactEpoch ep;
    ep.P       = P;
    ep.stash_s = stash_s;
    const int np = static_cast<int>(P.rows());
    ep.s_of_p.assign(static_cast<std::size_t>(np), -1);
    ep.route_p.assign(static_cast<std::size_t>(np), static_cast<int>(CouplingExactDRoute::Present));
    ep.clk_p.assign(static_cast<std::size_t>(np), 0);
    ep.cc_p.assign(static_cast<std::size_t>(np), 'G');
    std::vector<double> qs, sh;
    for (int p = 0; p < np; ++p) {
        const auto& k = keys[static_cast<std::size_t>(p)];
        ep.p_of_key[keyOf(k)] = p;
        ep.clk_p[static_cast<std::size_t>(p)] = (k.kind == SharedParamKind::Clock) ? 1 : 0;
        ep.s_of_p[static_cast<std::size_t>(p)] = static_cast<int>(ep.p_of_s.size());
        ep.p_of_s.push_back(p);
        ep.col_of_s.push_back(p);
        qs.push_back(q(p));
        sh.push_back(x(p));
    }
    ep.q_s    = Eigen::Map<const Eigen::VectorXd>(qs.data(), static_cast<Eigen::Index>(qs.size()));
    ep.shat_s = Eigen::Map<const Eigen::VectorXd>(sh.data(), static_cast<Eigen::Index>(sh.size()));
    if (off != 0.0) {
        ep.q_off.emplace_back(0, 1, off);
        ep.q_off.emplace_back(1, 0, off);
    }
    EXPECT_TRUE(couplingExactFactor(ep));
    return ep;
}

PbHoldCloneInputs cloneInputs(const CouplingExactEpoch& ep, const Eigen::VectorXd& x_post,
                              const Eigen::VectorXd& x_prior) {
    PbHoldCloneInputs in;
    in.xe        = &ep;
    in.desc_of_p = modelKeys();
    in.post_s.resize(ep.nS());
    in.prior_s.resize(ep.nS());
    for (Eigen::Index s = 0; s < ep.nS(); ++s) {
        const int p = ep.p_of_s[static_cast<std::size_t>(s)];
        in.post_s(s)  = x_post(p);
        in.prior_s(s) = x_prior(p);
    }
    in.col_of = [](const PbHoldCloneTerm& t) -> Eigen::Index {
        for (const auto& k : modelKeys())
            if (keyOf(k) == keyOf(t)) return k.col;
        return -1;
    };
    in.post_of_col = [x_post](Eigen::Index c) { return x_post(c); };
    in.prior_of    = [x_prior](const PbHoldCloneTerm& t) {
        for (const auto& k : modelKeys())
            if (keyOf(k) == keyOf(t)) return x_prior(k.col);
        return std::numeric_limits<double>::quiet_NaN();
    };
    return in;
}

std::map<std::string, std::pair<double, double>> stashOf(const Eigen::VectorXd& x,
                                                         const Eigen::MatrixXd& P) {
    std::map<std::string, std::pair<double, double>> m;
    for (const auto& k : modelKeys()) m[keyOf(k)] = {x(k.col), P(k.col, k.col)};
    return m;
}

/// G01's pair (L1C, L2W) and its functional.
PbHoldPair g01Pair() {
    const PbHoldPair pr = pbHoldClonePickPair("G01", {{"L1C", 0.01}, {"L2W", 0.01}});
    EXPECT_EQ(pr.sig.size(), 2u);
    return pr;
}

/// The D row born on the posterior (x, P): {c: -1, L1C: +w1, L2W: +w2}.
std::vector<PbHoldCloneTerm> dBirth(const Eigen::VectorXd& x, const Eigen::MatrixXd& P,
                                    const PbHoldPair& pr, double& value) {
    const auto k = modelKeys();
    std::vector<PbHoldCloneTerm> t = {k[0], k[1], k[2]};
    t[0].coeff = -1.0;     t[0].s_prev = x(0); t[0].p_prev = P(0, 0);
    t[1].coeff = pr.w[0];  t[1].s_prev = x(1); t[1].p_prev = P(1, 1);
    t[2].coeff = pr.w[1];  t[2].s_prev = x(2); t[2].p_prev = P(2, 2);
    value = x(0) - pr.w[0] * x(1) - pr.w[1] * x(2);
    return t;
}

// --- 1. The smoother -------------------------------------------------------------------

TEST(PbHoldBlockTest, ThePhaseVisibleRowIsTheFixedPointSmootherOfD) {
    const PbHoldPair pr = g01Pair();
    const std::vector<PbHoldCloneOwn> own = pbHoldCloneOwnPv(pr);
    Eigen::VectorXd g = Eigen::VectorXd::Zero(5);   // D = g'x
    g(0) = 1.0; g(1) = -pr.w[0]; g(2) = -pr.w[1];
    std::mt19937 rng(20260930u);
    std::normal_distribution<double> nd(0.0, 1.0);
    const Eigen::VectorXd q = walkQ();
    Eigen::VectorXd truth = Eigen::VectorXd::Zero(5);
    Eigen::VectorXd x     = Eigen::VectorXd::Zero(5);
    Eigen::MatrixXd P     = Eigen::MatrixXd::Identity(5, 5);
    Eigen::VectorXd xa;
    Eigen::MatrixXd Pa;
    std::vector<PbHoldCloneTerm> terms;
    double value = 0.0, var = 0.0, stamp = 0.0;
    double v_prev = 0.0;   // the row's V at its previous update
    bool born = false;
    const int T = 40, t0 = 3;
    for (int t = 1; t <= T; ++t) {
        for (int j = 0; j < 5; ++j) truth(j) += std::sqrt(q(j)) * nd(rng);
        const double lvl = 0.02 * nd(rng);          // a level walk on G01: c and both biases
        truth(0) += lvl; truth(1) += lvl; truth(2) += lvl;
        Eigen::MatrixXd H;
        Eigen::VectorXd r;
        epochRows(rng, H, r);
        Eigen::VectorXd y = H * truth;
        for (Eigen::Index i = 0; i < y.size(); ++i) y(i) += std::sqrt(r(i)) * nd(rng);
        const CouplingExactEpoch ep = exactEpoch(x, P, q, t - 1.0);
        const Eigen::VectorXd x_prior = x;
        P += Eigen::MatrixXd(q.asDiagonal());
        kfUpdate(x, P, H, r, y);
        if (born) {
            Eigen::VectorXd qa = Eigen::VectorXd::Zero(6);
            qa.head(5) = q;
            Pa += Eigen::MatrixXd(qa.asDiagonal());
            Eigen::MatrixXd Ha = Eigen::MatrixXd::Zero(H.rows(), 6);
            Ha.leftCols(5) = H;
            kfUpdate(xa, Pa, Ha, r, y);
            const PbHoldCloneStep st = pbHoldCloneAdvanceRow(cloneInputs(ep, x, x_prior), "G01",
                                                             own, terms, value, var, stamp);
            ASSERT_TRUE(st.ok) << "epoch " << t;
            EXPECT_TRUE(st.parent_in_s);
            EXPECT_NEAR(st.value, xa(5), 1e-8) << "epoch " << t << ": D(t0|t)";
            Eigen::VectorXd m = Eigen::VectorXd::Zero(5);
            for (const auto& tm : st.terms)
                if (tm.col >= 0 && tm.p_prev > 0.0) m(tm.col) += tm.coeff;
            EXPECT_TRUE(nearRel(st.var + m.dot(P * m), Pa(5, 5), 1e-8)) << "epoch " << t;
            // The helper the trace's V comes from is that variance, and the
            // smoother never raises it (the freeze's "rise" cause is not the smoother's).
            const double v_rev = pbHoldCloneRowValueVar(st.terms, st.var, P);
            EXPECT_TRUE(nearRel(v_rev, Pa(5, 5), 1e-8)) << "epoch " << t << ": the clone row variance V";
            EXPECT_LE(v_rev, v_prev * (1.0 + 1e-10)) << "epoch " << t << ": V non-increasing";
            v_prev = v_rev;
            // The increment D(t) - D(t0) = (g + m)'x + const.
            const Eigen::VectorXd gi = g + m;
            const double vinc = g.dot(Pa.topLeftCorner(5, 5) * g) + Pa(5, 5)
                                - 2.0 * g.dot(Pa.block(0, 5, 5, 1).col(0));
            EXPECT_TRUE(nearRel(st.var + gi.dot(P * gi), vinc, 1e-8)) << "epoch " << t;
            // The fold's inflation along D is g'Qg.
            EXPECT_NEAR(st.dW, g.dot(Eigen::VectorXd(q.cwiseProduct(g))), 1e-15);
            value = st.value;
            var   = st.var;
            terms = pbHoldCloneRebase(st.terms, stashOf(x, P), {}, nullptr);
            stamp = t;
        }
        if (t == t0) {
            terms = dBirth(x, P, pr, value);
            var   = 0.0;
            stamp = t;
            born  = true;
            xa.resize(6);
            xa.head(5) = x;
            xa(5) = g.dot(x);
            Pa = Eigen::MatrixXd::Zero(6, 6);
            Pa.topLeftCorner(5, 5) = P;
            const Eigen::VectorXd Pg = P * g;
            Pa.block(0, 5, 5, 1) = Pg;
            Pa.block(5, 0, 1, 5) = Pg.transpose();
            Pa(5, 5) = g.dot(Pg);
            // V at birth is the filtered variance of D at the stash.
            v_prev = pbHoldCloneRowValueVar(terms, 0.0, P);
            EXPECT_TRUE(nearRel(v_prev, Pa(5, 5), 1e-12)) << "V at birth";
        }
    }
    ASSERT_TRUE(born);
}

// --- 2. The block and its cross terms --------------------------------------------------

TEST(PbHoldBlockTest, TheRowIsTheBlocksFunctionalWithEveryCrossTerm) {
    const PbHoldPair pr = g01Pair();
    Eigen::VectorXd x(5);
    x << 0.3, 0.1, -0.2, 0.5, 0.05;
    Eigen::MatrixXd A = Eigen::MatrixXd::Random(5, 5);
    const Eigen::MatrixXd P = A * A.transpose() + Eigen::MatrixXd::Identity(5, 5);
    const double off = 2e-4;   // the seam's level noise between G01's clock and its L1C
    const CouplingExactEpoch ep = exactEpoch(x, P, walkQ(), 2.0, off);
    // The three single-key rows of the (c, p_1, p_2) block and the functional's row.
    const CouplingExactRow rc = couplingExactRow(ep, {{0, -1.0}});
    const CouplingExactRow r1 = couplingExactRow(ep, {{1, -1.0}});
    const CouplingExactRow r2 = couplingExactRow(ep, {{2, -1.0}});
    const CouplingExactRow rd = couplingExactRow(ep, {{0, -1.0}, {1, pr.w[0]}, {2, pr.w[1]}});
    ASSERT_TRUE(rc.ok && r1.ok && r2.ok && rd.ok);
    const double g[3] = {1.0, -pr.w[0], -pr.w[1]};
    const CouplingExactRow* rows[3] = {&rc, &r1, &r2};
    const Eigen::VectorXd m_comb = g[0] * rc.m_new + g[1] * r1.m_new + g[2] * r2.m_new;
    EXPECT_TRUE(rd.m_new.isApprox(m_comb, 1e-12)) << "the exact rule is linear in the row";
    double v = 0.0;
    for (int a = 0; a < 3; ++a)
        for (int b = 0; b < 3; ++b)
            v += g[a] * g[b] * couplingExactCross(ep, *rows[a], *rows[b]);
    EXPECT_TRUE(nearRel(rd.var_raw, v, 1e-10)) << "the block's variance with every cross term";
    // The row advance: its value moves by the functional of the three rows' revisions, and
    // its fold inflation is g'Qg with the seam's cross term.
    double value = 0.0;
    std::vector<PbHoldCloneTerm> terms = dBirth(x, P, pr, value);
    Eigen::VectorXd post = x;
    post(0) += 0.01; post(1) -= 0.02; post(3) += 0.005;
    const PbHoldCloneStep st = pbHoldCloneAdvanceRow(cloneInputs(ep, post, x), "G01",
                                                     pbHoldCloneOwnPv(pr), terms, value, 0.0, 2.0);
    ASSERT_TRUE(st.ok);
    const Eigen::VectorXd q = walkQ();
    const double dW = q(0) + g[1] * g[1] * q(1) + g[2] * g[2] * q(2) + 2.0 * g[0] * g[1] * off;
    EXPECT_NEAR(st.dW, dW, 1e-15);
    EXPECT_NEAR(st.var, rd.var_add, 1e-15);
    // A key of the functional outside the block is named.
    CouplingExactEpoch out = ep;
    out.p_of_key.erase("G01|PB|L2W");
    const PbHoldCloneStep so = pbHoldCloneAdvanceRow(cloneInputs(out, post, x), "G01",
                                                     pbHoldCloneOwnPv(pr), terms, value, 0.0, 2.0);
    EXPECT_FALSE(so.ok);
    EXPECT_EQ(so.death, PbHoldCloneDeath::ParentOutside);
    EXPECT_EQ(so.outside_own, 2) << "the L2W phase bias, own key 2 (the clock is 0)";
}

// --- 3. The pair -------------------------------------------------------------------------

TEST(PbHoldBlockTest, ThePairIsOnePhaseBiasPerBandBlindToTheLevelAndTheIonosphere) {
    const PbHoldPair a = pbHoldClonePickPair("G05", {{"L1C", 0.02}, {"L1W", 0.01}, {"L2W", 0.03}});
    ASSERT_EQ(a.sig.size(), 2u);
    EXPECT_EQ(a.sig[0], "L1W") << "the smaller stashed variance on L1";
    EXPECT_EQ(a.sig[1], "L2W");
    EXPECT_NEAR(a.w[0], 2.5457, 1e-4);
    EXPECT_NEAR(a.w[1], -1.5457, 1e-4);
    EXPECT_NEAR(a.w[0] + a.w[1], 1.0, 1e-12) << "blind to the level";
    const double l1 = bandNominalWavelengthMeters(classifySignalForSat("G05", "L1W").band);
    const double l2 = bandNominalWavelengthMeters(classifySignalForSat("G05", "L2W").band);
    EXPECT_NEAR(a.w[0] * l1 * l1 + a.w[1] * l2 * l2, 0.0, 1e-12) << "blind to the ionosphere";
    // A level step of the clock and both biases moves D by nothing; a clock step passes.
    const double lvl = 0.37;
    EXPECT_NEAR(lvl - a.w[0] * lvl - a.w[1] * lvl, 0.0, 1e-12);
    const PbHoldPair tie = pbHoldClonePickPair("G05", {{"L1W", 0.01}, {"L1C", 0.01}, {"L2W", 0.03}});
    ASSERT_EQ(tie.sig.size(), 2u);
    EXPECT_EQ(tie.sig[0], "L1C") << "a tie keeps the lexically first signal";
    EXPECT_TRUE(pbHoldClonePickPair("G05", {{"L1C", 0.01}, {"L1W", 0.02}}).sig.empty())
        << "one band has no phase-visible functional";
    EXPECT_TRUE(pbHoldClonePickPair("G05", {{"L1C", 0.0}, {"L2W", 0.01}}).sig.empty())
        << "a key with no stated variance is not stashed";
    const auto own = pbHoldCloneOwnPv(a);
    ASSERT_EQ(own.size(), 3u);
    EXPECT_EQ(own[0].kind, SharedParamKind::Clock);
    EXPECT_DOUBLE_EQ(own[0].g, 1.0);
    EXPECT_EQ(own[1].signal, "L1W");
    EXPECT_DOUBLE_EQ(own[1].g, -a.w[0]);
}

// --- 4. The lifetime and the hold ----------------------------------------------------------

std::shared_ptr<spdlog::logger> smdLogger() {
    static auto sink = std::make_shared<spdlog::sinks::null_sink_mt>();
    return std::make_shared<spdlog::logger>("smd_test", sink);
}

std::string smdConfig() {
    return std::string("{\n") +
        "  \"processing\": {\n"
        "    \"mode\": \"ppp-rtk-network\",\n"
        "    \"active_systems\": [ \"GPS\" ],\n"
        "    \"signal_priorities\": { \"GPS\": \"WC\" }\n"
        "  },\n"
        "  \"network\": {\n"
        "    \"emit_pb_hold\": true,\n"
        "    \"emit_pb_hold_smoothed\": true,\n"
        "    \"carry_conditional_coupling\": true,\n"
        "    \"carry_coupling_time_update\": true,\n"
        "    \"shared_carry_full_cov\": true,\n"
        "    \"carry_coupling_exact_update\": true,\n"
        "    \"raim\": true,\n"
        "    \"raim_within_epoch\": true,\n"
        "    \"clock_temporal_anchor\": true,\n"
        "    \"stations\": [\n"
        "      { \"name\": \"AAAA\", \"position_ecef\": [ -2493304.99, -4655214.79, 3565497.67 ] },\n"
        "      { \"name\": \"BBBB\", \"position_ecef\": [ -2281621.75, -1453585.08, 5756964.89 ] }\n"
        "    ]\n"
        "  }\n"
        "}\n";
}

std::unique_ptr<NetworkRunContext> smdContext(GnssConfig& cfg, const char* tag) {
    LogManager log_mgr;
    const fs::path p = fs::temp_directory_path() / (std::string("smd_ctx_") + tag + ".json");
    { std::ofstream f(p); f << smdConfig(); }
    EXPECT_TRUE(cfg.loadFromJson(p, log_mgr));
    std::error_code ec; fs::remove(p, ec);
    return std::make_unique<NetworkRunContext>(cfg, cfg.getNetworkConfig());
}

/// The run the clone seams' epoch forms take (tests/support/NetworkTestRun.hpp), over the
/// configuration smdContext loads.
std::unique_ptr<NetworkTestRun> smdRun(GnssConfig& cfg, const char* tag) {
    (void)smdContext(cfg, tag);   // loads cfg
    return std::make_unique<NetworkTestRun>(cfg, smdLogger());
}

/// A live block clone on the pair (sig1, L2W), advanced at epoch @p ep.
PbHoldClone liveBlock(const std::string& sat, const std::string& sig1, long long ep) {
    PbHoldClone c;
    c.sat        = sat;
    c.signal     = "L1W";
    c.generation = 1;
    c.alive      = true;
    c.block      = true;
    c.t0_s       = static_cast<double>(ep);
    c.updated_ep = ep;
    c.pv_pair    = PbHoldPair{{sig1, "L2W"}, {2.5457, -1.5457}};
    c.phi_ok     = true;
    c.pv_phi_ok  = true;
    return c;
}

TEST(PbHoldBlockTest, ABlockRetiresAtAReReferenceOrANewTenureOfItsPair) {
    GnssConfig cfg;
    auto fx = smdRun(cfg, "follow");
    NetworkRunContext* ctx = &fx->ctx();
    ASSERT_TRUE(ctx->knobs.emit_pb_hold_smoothed);
    ctx->counters.n_loop_epochs = 7;
    auto& carry = ctx->carry;
    net::putSig(ctx->entities, "G32|L1W", &net::SatSignalState::smh_block) = liveBlock("G32", "L1W", 6);
    net::putSig(ctx->entities, "G12|L1W", &net::SatSignalState::smh_block) = liveBlock("G12", "L1C", 6);
    net::putSig(ctx->entities, "G05|L1W", &net::SatSignalState::smh_block) = liveBlock("G05", "L1W", 6);
    net::putSig(ctx->entities, "G07|L1W", &net::SatSignalState::smh_block) = liveBlock("G07", "L1W", 6);
    for (const char* s : {"G32", "G12", "G05", "G07"}) {
        carry.clk_anchor.sat[s].anchored        = true;
        carry.clk_anchor.sat[s].folded_anchored = true;
    }
    carry.phase_anchor_reref.insert({"G32", "L2W"});         // a cold handover of G32's L2W
    net::putSig(ctx->entities, "G12_L1C", &net::SatSignalState::phase_basis).elected_ep = 7;             // a continuous handover of G12's L1C
    net::putSig(ctx->entities, "G05_L1C", &net::SatSignalState::phase_basis).elected_ep = 7;             // outside G05's pair: no step of D
    net::putSig(ctx->entities, "G07_L1W", &net::SatSignalState::phase_basis).elected_ep = 6;             // an earlier tenure
    NetworkEpoch& e = fx->newEpoch(GnssTime{});   // an empty workspace and datum, the run's estimator
    e.solve.x_s = Eigen::VectorXd::Zero(1);
    networkPbHoldCloneUpdate(fx->run(), e);
    const auto b = net::sigMap(ctx->entities.sat_signals, &net::SatSignalState::smh_block, '|');
    EXPECT_EQ(b.at("G32|L1W").death, PbHoldCloneDeath::Followed);
    EXPECT_EQ(b.at("G32|L1W").follow, PbHoldFollow::Reref);
    EXPECT_EQ(b.at("G12|L1W").death, PbHoldCloneDeath::Followed);
    EXPECT_EQ(b.at("G12|L1W").follow, PbHoldFollow::Tenure);
    // The others go on to the exact route, which this empty epoch cannot give them.
    EXPECT_EQ(b.at("G05|L1W").death, PbHoldCloneDeath::Unfactored);
    EXPECT_EQ(b.at("G07|L1W").death, PbHoldCloneDeath::Unfactored);
    EXPECT_EQ(ctx->counters.smh_pv_follow[static_cast<int>(PbHoldFollow::Reref)], 1);
    EXPECT_EQ(ctx->counters.smh_pv_follow[static_cast<int>(PbHoldFollow::Tenure)], 1);
    const auto pend = net::sigMap(ctx->entities.sat_signals, &net::SatSignalState::smh_block_pending, '|');
    ASSERT_EQ(pend.size(), 2u);
    EXPECT_TRUE(pend.at("G32|L1W").rebirth);
    EXPECT_TRUE(pend.at("G32|L1W").block);
    EXPECT_EQ(pend.at("G12|L1W").queued_ep, 7);
    EXPECT_TRUE((net::present(ctx->entities.sat_signals, &net::SatSignalState::smh_clone) == 0)) << "the smoothed hold's clock clones are untouched";
}

TEST(PbHoldBlockTest, ABlockIsBornWithItsPairFromTheStash) {
    GnssConfig cfg;
    auto fx = smdRun(cfg, "birth");
    NetworkRunContext* ctx = &fx->ctx();
    auto& carry = ctx->carry;
    for (const char* s : {"G32", "G12"}) {
        net::PbHoldKey k;
        k.generation = 1;
        net::putSig(ctx->entities, std::string(s) + "|L1W", &net::SatSignalState::pb_hold) = k;
        PbHoldClone pc;
        pc.sat = s; pc.signal = "L1W"; pc.generation = 1; pc.queued_ep = 1; pc.block = true;
        net::putSig(ctx->entities, std::string(s) + "|L1W", &net::SatSignalState::smh_block_pending) = pc;
        carry.clk_anchor.sat[s].anchored = true;
    }
    // G32: a clock and three phase biases; G12: a clock and L1C alone (one band).
    struct K { SharedParamKind kind; const char* sat; const char* sig; double v; double var; };
    const std::vector<K> ks = {{SharedParamKind::Clock, "G32", "", 3.0, 0.04},
                               {SharedParamKind::PhaseBias, "G32", "L1C", 1.0, 0.02},
                               {SharedParamKind::PhaseBias, "G32", "L1W", 2.0, 0.01},
                               {SharedParamKind::PhaseBias, "G32", "L2W", 0.5, 0.03},
                               {SharedParamKind::Clock, "G12", "", 7.0, 0.04},
                               {SharedParamKind::PhaseBias, "G12", "L1C", 4.0, 0.02}};
    carry.prev_carry.clear();
    carry.prev_carry_cov.P = Eigen::MatrixXd::Zero(static_cast<Eigen::Index>(ks.size()),
                                                   static_cast<Eigen::Index>(ks.size()));
    for (std::size_t i = 0; i < ks.size(); ++i) {
        CarryPrior cp{};
        cp.kind = ks[i].kind; cp.sat = ks[i].sat; cp.signal = ks[i].sig;
        cp.value = ks[i].v; cp.var = ks[i].var;
        carry.prev_carry.push_back(cp);
        carry.prev_carry_cov.row[sharedCarryKey(cp.kind, cp.sat, cp.signal)] = i;
        carry.prev_carry_cov.P(static_cast<Eigen::Index>(i), static_cast<Eigen::Index>(i)) = ks[i].var;
    }
    ctx->counters.n_loop_epochs = 2;
    carry.cxu_stash_valid = true;
    carry.cxu_stash_s     = 200.0;
    NetworkEpoch& e = fx->newEpoch(GnssTime{ 0, 200.0 });   // continuous GPS seconds 200
    e.solve.x_s = Eigen::VectorXd::Zero(1);
    networkPbHoldCloneHarvest(fx->run(), e);
    ASSERT_EQ((net::getSig(ctx->entities.sat_signals, "G32|L1W", &net::SatSignalState::smh_block) ? 1u : 0u), 1u);
    const PbHoldClone& c = (*net::getSig(ctx->entities.sat_signals, "G32|L1W", &net::SatSignalState::smh_block));
    EXPECT_TRUE(c.alive);
    EXPECT_TRUE(c.block);
    ASSERT_EQ(c.pv_pair.sig.size(), 2u);
    EXPECT_EQ(c.pv_pair.sig[0], "L1W");
    EXPECT_EQ(c.pv_pair.sig[1], "L2W");
    EXPECT_DOUBLE_EQ(c.value, 3.0) << "the clock row, the smoothed hold's birth";
    EXPECT_NEAR(c.pv_value, 3.0 - c.pv_pair.w[0] * 2.0 - c.pv_pair.w[1] * 0.5, 1e-12);
    ASSERT_EQ(c.pv_terms.size(), 3u);
    EXPECT_DOUBLE_EQ(c.pv_terms[0].coeff, -1.0);
    EXPECT_DOUBLE_EQ(c.pv_terms[1].coeff, c.pv_pair.w[0]);
    EXPECT_DOUBLE_EQ(c.pv_terms[2].p_prev, 0.03);
    EXPECT_EQ(ctx->counters.smh_pv_births, 1);
    EXPECT_EQ(ctx->counters.smh_pv_pairs.at("L1W+L2W"), 1);
    EXPECT_EQ(ctx->counters.smh_pv_defer_nopair, 1) << "G12 has one band";
    EXPECT_EQ((net::getSig(ctx->entities.sat_signals, "G12|L1W", &net::SatSignalState::smh_block_pending) ? 1u : 0u), 1u);
    EXPECT_TRUE((net::present(ctx->entities.sat_signals, &net::SatSignalState::smh_clone) == 0));
}

/// One satellite of a test epoch.
struct SmdSat {
    std::string sat;
    double      clk;
    double      pb;
};

NetworkSolutionEpoch smdEpoch(double tow, const std::vector<SmdSat>& sats) {
    NetworkSolutionEpoch sol;
    sol.epoch_time = GnssTime{ 2313, tow };
    sol.iod_ssr = 3; sol.provider_id = 42; sol.solution_id = 1;
    for (const auto& x : sats) {
        NetworkSatSolution s;
        s.sat = x.sat; s.has_clock = true; s.clock_c0_m = x.clk;
        s.code_bias_m["C1W"] = 0.2;
        s.phase_bias["L1W"] = NetworkPhaseBias{ x.pb, true, 1 };
        sol.sats.push_back(s);
    }
    return sol;
}

double pbOf(const NetworkSolutionEpoch& sol, const std::string& sat) {
    for (const auto& s : sol.sats)
        if (s.sat == sat) return s.phase_bias.at("L1W").bias_m;
    ADD_FAILURE() << "no phase bias for " << sat;
    return 0.0;
}

TEST(PbHoldBlockTest, AMarkRetiresTheBlockBesideTheWritingClockClone) {
    GnssConfig cfg;
    auto ctx = smdContext(cfg, "hold");
    const fs::path dir = fs::temp_directory_path() / "smd_traces_hold";
    fs::create_directories(dir);
    NetworkRunTraces traces(dir);
    NetworkProductEmitter emitter(0.05, /*emit_clock_disc=*/false);
    SsrCorrectionStore store;
    SyncedEpoch se;
    const auto step = [&](NetworkSolutionEpoch& sol, long long ep) {
        ctx->counters.n_loop_epochs = static_cast<decltype(ctx->counters.n_loop_epochs)>(ep);
        se.time = sol.epoch_time;
        networkPbHold(*ctx, cfg, smdLogger(), traces, emitter, se, sol);
        emitter.emitToStore(sol, store);
    };
    auto e1 = smdEpoch(100.0, {{"G32", 10.0, 1.0}, {"G12", 20.0, 2.0}, {"G05", 30.0, 3.0}});
    step(e1, 1);
    ASSERT_EQ((net::getSig(ctx->entities.sat_signals, "G32|L1W", &net::SatSignalState::smh_block_pending) ? 1u : 0u), 1u) << "the generation queues its block";
    net::dropAll(ctx->entities.sat_signals, &net::SatSignalState::smh_block_pending);
    net::dropAll(ctx->entities.sat_signals, &net::SatSignalState::smh_pending);
    // Epoch 2: the smoothed hold's clock clone passed 2 cm and writes the wire.  The key's block
    // clone beside it revised D(t0) by +3 cm (Phi_D = -3 cm) on the same clock motion, and
    // the split is recorded.
    PbHoldClone clk = liveBlock("G32", "L1W", 2);
    clk.block = false;
    clk.phi   = 0.02;
    net::putSig(ctx->entities, "G32|L1W", &net::SatSignalState::smh_clone) = clk;
    net::putSig(ctx->entities, "G32|L1W", &net::SatSignalState::smh_block) = liveBlock("G32", "L1W", 2);
    net::putSig(ctx->entities, "G32|L1W", &net::SatSignalState::smh_block).pv_phi = -0.03;
    net::putSig(ctx->entities, "G32|L1W", &net::SatSignalState::smh_block).phi    = 0.02;
    net::putSig(ctx->entities, "G32|L1W", &net::SatSignalState::smh_block).pv_step_d = 0.003;
    auto e2 = smdEpoch(130.0, {{"G32", 10.0, 1.5}, {"G12", 20.0, 2.5}, {"G05", 30.0, 3.5}});
    step(e2, 2);
    EXPECT_NEAR(pbOf(e2, "G32"), 0.98, 1e-12) << "the smoothed hold's Phi writes";
    ASSERT_EQ(ctx->counters.smh_pv_lvl_abs.size(), 1u);
    EXPECT_NEAR(ctx->counters.smh_pv_lvl_abs[0], 0.05, 1e-12) << "the level part Phi_c - Phi_D";
    EXPECT_EQ(ctx->counters.smh_pv_steps_unmarked, 0);
    // Epoch 3: an emitter mark on G32's phase channel retires the block; the clock clone
    // goes on writing, so the key does not bridge.
    net::putSig(ctx->entities, "G32|L1W", &net::SatSignalState::smh_clone).updated_ep = 3;
    net::putSig(ctx->entities, "G32|L1W", &net::SatSignalState::smh_block).pv_phi     = -0.04;
    net::putSig(ctx->entities, "G32|L1W", &net::SatSignalState::smh_block).updated_ep = 3;
    emitter.markSatDiscontinuity({"G32"}, NetworkProductEmitter::kDiscChanPhaseBias);
    auto e3 = smdEpoch(160.0, {{"G32", 10.0, 1.7}, {"G12", 20.0, 2.7}, {"G05", 30.0, 3.7}});
    step(e3, 3);
    EXPECT_NEAR(pbOf(e3, "G32"), 0.98, 1e-12);
    EXPECT_EQ((*net::getSig(ctx->entities.sat_signals, "G32|L1W", &net::SatSignalState::smh_block)).death, PbHoldCloneDeath::Followed);
    EXPECT_EQ((*net::getSig(ctx->entities.sat_signals, "G32|L1W", &net::SatSignalState::smh_block)).follow, PbHoldFollow::Mark);
    EXPECT_EQ(ctx->counters.smh_pv_follow[static_cast<int>(PbHoldFollow::Mark)], 1);
    EXPECT_EQ((net::getSig(ctx->entities.sat_signals, "G32|L1W", &net::SatSignalState::smh_block_pending) ? 1u : 0u), 1u) << "born again at this stash";
    EXPECT_EQ(ctx->counters.smh_bridges, 0) << "the mark retires the block, not the writing clone";
    EXPECT_EQ(ctx->counters.smh_reverts, 0);
    // Epoch 4: born again; a pair step above the emitter's 5 cm bar with nothing declared is
    // an unmarked step, passed as motion.
    net::putSig(ctx->entities, "G32|L1W", &net::SatSignalState::smh_clone).updated_ep = 4;
    net::putSig(ctx->entities, "G32|L1W", &net::SatSignalState::smh_block) = liveBlock("G32", "L1W", 4);
    net::putSig(ctx->entities, "G32|L1W", &net::SatSignalState::smh_block).pv_phi  = -0.01;
    net::putSig(ctx->entities, "G32|L1W", &net::SatSignalState::smh_block).pv_step_d = 0.08;
    auto e4 = smdEpoch(190.0, {{"G32", 10.0, 1.9}, {"G12", 20.0, 2.9}, {"G05", 30.0, 3.9}});
    step(e4, 4);
    EXPECT_NEAR(pbOf(e4, "G32"), 0.98, 1e-12);
    EXPECT_EQ(ctx->counters.smh_pv_steps_unmarked, 1);
    EXPECT_NEAR(ctx->counters.smh_pv_step_unmarked_max, 0.08, 1e-12);
    EXPECT_EQ(ctx->counters.smh_held_smoothed, 3);
}

TEST(PbHoldBlockTest, ASwitchToTheBridgeKeepsTheEpochsClockStep) {
    // At the epoch a clone dies the key takes the hold family's step of that epoch
    // on top of the transfer it had reached, so C - P does not step.  G32's clock moves 5 cm
    // at the death epoch while its mates hold (the constellation's median step is 0).  The
    // writing member is the smoothed hold's clock clone; the anchor's withdrawal is a bridge, not a
    // revert.
    GnssConfig cfg;
    auto ctx = smdContext(cfg, "bridgestep");
    const fs::path dir = fs::temp_directory_path() / "smd_traces_bridgestep";
    fs::create_directories(dir);
    NetworkRunTraces traces(dir);
    NetworkProductEmitter emitter(0.05, /*emit_clock_disc=*/false);
    SsrCorrectionStore store;
    SyncedEpoch se;
    const auto step = [&](NetworkSolutionEpoch& sol, long long ep) {
        ctx->counters.n_loop_epochs = static_cast<decltype(ctx->counters.n_loop_epochs)>(ep);
        se.time = sol.epoch_time;
        networkPbHold(*ctx, cfg, smdLogger(), traces, emitter, se, sol);
        emitter.emitToStore(sol, store);
    };
    auto e1 = smdEpoch(100.0, {{"G32", 10.0, 1.0}, {"G12", 20.0, 2.0}, {"G05", 30.0, 3.0}});
    step(e1, 1);
    net::dropAll(ctx->entities.sat_signals, &net::SatSignalState::smh_block_pending);
    net::dropAll(ctx->entities.sat_signals, &net::SatSignalState::smh_pending);
    PbHoldClone clk = liveBlock("G32", "L1W", 2);
    clk.block = false;
    clk.phi   = -0.03;
    net::putSig(ctx->entities, "G32|L1W", &net::SatSignalState::smh_clone) = clk;
    auto e2 = smdEpoch(130.0, {{"G32", 10.0, 1.5}, {"G12", 20.0, 2.5}, {"G05", 30.0, 3.5}});
    step(e2, 2);
    EXPECT_NEAR(pbOf(e2, "G32"), 1.03, 1e-12);
    // Epoch 3: the anchor withdraws the clock (the clone dies), and G32's clock moves 5 cm.
    net::putSig(ctx->entities, "G32|L1W", &net::SatSignalState::smh_clone).alive = false;
    net::putSig(ctx->entities, "G32|L1W", &net::SatSignalState::smh_clone).death = PbHoldCloneDeath::Withdrawn;
    auto e3 = smdEpoch(160.0, {{"G32", 10.05, 1.7}, {"G12", 20.0, 2.7}, {"G05", 30.0, 3.7}});
    step(e3, 3);
    EXPECT_NEAR(pbOf(e3, "G32"), 1.08, 1e-12) << "the bridge takes this epoch's clock step";
    // Epoch 4: the bridge goes on with LAH's steps, the -3 cm the clone reached kept.
    auto e4 = smdEpoch(190.0, {{"G32", 10.07, 1.9}, {"G12", 20.0, 2.9}, {"G05", 30.0, 3.9}});
    step(e4, 4);
    EXPECT_NEAR(pbOf(e4, "G32"), 1.10, 1e-12);
    EXPECT_EQ(ctx->counters.smh_bridges, 1);
}

TEST(PbHoldBlockTest, TheClockMemberIgnoresTheBlockOnTheWire) {
    GnssConfig cfg;
    auto ctx = smdContext(cfg, "clockmember");
    const fs::path dir = fs::temp_directory_path() / "smd_traces_clk";
    fs::create_directories(dir);
    NetworkRunTraces traces(dir);
    NetworkProductEmitter emitter(0.05, /*emit_clock_disc=*/false);
    SsrCorrectionStore store;
    SyncedEpoch se;
    const auto step = [&](NetworkSolutionEpoch& sol, long long ep) {
        ctx->counters.n_loop_epochs = static_cast<decltype(ctx->counters.n_loop_epochs)>(ep);
        se.time = sol.epoch_time;
        networkPbHold(*ctx, cfg, smdLogger(), traces, emitter, se, sol);
        emitter.emitToStore(sol, store);
    };
    auto e1 = smdEpoch(100.0, {{"G32", 10.0, 1.0}, {"G12", 20.0, 2.0}});
    step(e1, 1);
    net::dropAll(ctx->entities.sat_signals, &net::SatSignalState::smh_block_pending);
    net::dropAll(ctx->entities.sat_signals, &net::SatSignalState::smh_pending);
    PbHoldClone c = liveBlock("G32", "L1W", 2);
    c.block = false;
    c.phi   = -0.02;
    net::putSig(ctx->entities, "G32|L1W", &net::SatSignalState::smh_clone) = c;
    net::putSig(ctx->entities, "G32|L1W", &net::SatSignalState::smh_block) = liveBlock("G32", "L1W", 2);
    net::putSig(ctx->entities, "G32|L1W", &net::SatSignalState::smh_block).pv_phi = -0.07;
    net::putSig(ctx->entities, "G32|L1W", &net::SatSignalState::smh_block).phi    = -0.02;
    auto e2 = smdEpoch(130.0, {{"G32", 10.0, 1.5}, {"G12", 20.0, 2.5}});
    step(e2, 2);
    EXPECT_NEAR(pbOf(e2, "G32"), 1.02, 1e-12) << "the smoothed hold's Phi writes; the block is an instrument";
    ASSERT_EQ(ctx->counters.smh_pv_phi_abs.size(), 1u);
    EXPECT_NEAR(ctx->counters.smh_pv_phi_abs[0], 0.07, 1e-12);
}

// --- 5. The S line -----------------------------------------------------------------------------

std::string slurp(const fs::path& p) {
    std::ifstream f(p);
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

/// The writer declares the smoothed hold as pbhold=3.  A file written while the removed
/// block member existed carries pbhold=4, which still reads as a smoothed hold, so an older
/// product keeps its meaning.
TEST(PbHoldBlockTest, AnOlderPbhold4FileReadsAsASmoothedHold) {
    const fs::path p = fs::temp_directory_path() / "smd_contract.txt";
    SbasisContract c;
    c.present = true;
    c.apriori_product_id = "CODFIN2024129";
    c.emit_pb_hold = true;
    c.emit_pb_hold_smoothed = true;
    { NetworkProductWriter w(p.string(), c); }
    std::string text = slurp(p);
    const std::string written = ",pbhold=3";
    const std::size_t at = text.find(written);
    ASSERT_NE(at, std::string::npos) << "the smoothed hold is declared as pbhold=3";
    text.replace(at, written.size(), ",pbhold=4");
    { std::ofstream f(p); f << text; }
    bool ok = false;
    SbasisContract back;
    NetworkProductReader::read(p.string(), &back, &ok);
    EXPECT_TRUE(ok);
    EXPECT_TRUE(back.emit_pb_hold);
    EXPECT_TRUE(back.emit_pb_hold_smoothed) << "the removed block member was a smoothed hold";
    EXPECT_FALSE(back.emit_pb_hold_phase_clock);
    fs::remove(p);
}

}  // namespace
}  // namespace GnssProcessor
