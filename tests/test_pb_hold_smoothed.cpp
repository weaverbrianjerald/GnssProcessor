/**
 * @file test_pb_hold_smoothed.cpp
 * @brief Unit gates of card SMH, the smoothed phase-bias hold on a carried clock clone
 *        (@c network.emit_pb_hold_smoothed).
 *
 * 1. The clone row, advanced by @ref pbHoldCloneAdvance and re-harvested by
 *    @ref pbHoldCloneRebase over card CTU's exact epoch inputs, equals the fixed-point
 *    smoother of a small dense random-walk model (an augmented Kalman filter carrying
 *    c(t0) as a zero-noise state) at every epoch, with a reinit of a column the clone
 *    has spread onto and of the clone's own clock.
 * 2. A frame step of the prior means never steps the clone; a pinned term adds no
 *    revision.
 * 3. A clone that would leave the exact route dies with its cause.
 * 4. The hold writes the clone's transfer and reverts continuously to the phase-clock
 *    transfer when the clone dies.
 * 5. The loader and the S line.
 */

#include <gtest/gtest.h>

#include "support/NetworkTestRun.hpp"   // the run and epoch the clone seams' epoch forms take
#include "network/state/NetworkRunContext.hpp"
#include "config/GnssConfig.hpp"
#include "config/GnssConfigTypes.hpp"
#include "network/carry/NetworkCouplingExact.hpp"
#include "network/emit/NetworkPbHold.hpp"
#include "network/emit/NetworkPbHoldClone.hpp"
#include "network/driver/NetworkRunTypes.hpp"
#include "logging/LogManager.hpp"
#include "network/diag/NetworkRunTraces.hpp"
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

// --- The dense walk model --------------------------------------------------------------

/// The five border keys of the model: two satellite clocks and three phase biases.
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

/// One epoch's observation rows: a code-like row per clock, phase-like rows c - p, and a
/// random row, so the posterior has the clock/phase-bias correlations of the producer.
void epochRows(std::mt19937& rng, Eigen::MatrixXd& H, Eigen::VectorXd& r) {
    std::normal_distribution<double> nd(0.0, 1.0);
    H.setZero(6, 5);
    r.resize(6);
    H(0, 0) = 1.0;                 r(0) = 0.25;     // code, G01
    H(1, 3) = 1.0;                 r(1) = 0.25;     // code, G02
    H(2, 0) = 1.0; H(2, 1) = -1.0; r(2) = 1e-4;     // phase, G01 L1C
    H(3, 0) = 1.0; H(3, 2) = -1.0; r(3) = 1e-4;     // phase, G01 L2W
    H(4, 3) = 1.0; H(4, 4) = -1.0; r(4) = 1e-4;     // phase, G02 L1C
    for (int j = 0; j < 5; ++j) H(5, j) = 0.3 * nd(rng);
    r(5) = 1e-2;
}

/// A Kalman measurement update in the Joseph form (stable with a reinit's large prior).
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

/// The reinit's prior variance in the reference filter (the q -> infinity limit of the
/// exact rule's Reinit route, large enough and still well conditioned).
constexpr double kReinitVar = 1e7;

/// |a - b| within @p tol relative to max(1, |b|).
::testing::AssertionResult nearRel(double a, double b, double tol) {
    if (std::abs(a - b) <= tol * std::max(1.0, std::abs(b))) return ::testing::AssertionSuccess();
    return ::testing::AssertionFailure() << a << " vs " << b << " (tol " << tol << " relative)";
}

/// Card CTU's epoch inputs over the stash (x, P) of the previous epoch, every key in S
/// with the walk as its inflation, except @p reinit (a D key on the Reinit route).
CouplingExactEpoch exactEpoch(const Eigen::VectorXd& x, const Eigen::MatrixXd& P,
                              const Eigen::VectorXd& q, double stash_s, int reinit) {
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
        if (p == reinit) {
            ep.route_p[static_cast<std::size_t>(p)] = static_cast<int>(CouplingExactDRoute::Reinit);
            continue;
        }
        ep.s_of_p[static_cast<std::size_t>(p)] = static_cast<int>(ep.p_of_s.size());
        ep.p_of_s.push_back(p);
        ep.col_of_s.push_back(p);
        qs.push_back(q(p));
        sh.push_back(x(p));
    }
    ep.q_s    = Eigen::Map<const Eigen::VectorXd>(qs.data(), static_cast<Eigen::Index>(qs.size()));
    ep.shat_s = Eigen::Map<const Eigen::VectorXd>(sh.data(), static_cast<Eigen::Index>(sh.size()));
    EXPECT_TRUE(couplingExactFactor(ep));
    return ep;
}

/// The clone's inputs for one epoch of the model: posterior @p x_post, prior means
/// @p x_prior (both indexed by column = P row).
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
        const auto keys = modelKeys();
        for (const auto& k : keys)
            if (keyOf(k) == keyOf(t)) return k.col;
        return -1;
    };
    in.post_of_col = [x_post](Eigen::Index c) { return x_post(c); };
    in.prior_of    = [x_prior](const PbHoldCloneTerm& t) {
        const auto keys = modelKeys();
        for (const auto& k : keys)
            if (keyOf(k) == keyOf(t)) return x_prior(k.col);
        return std::numeric_limits<double>::quiet_NaN();
    };
    return in;
}

/// A clone born on column 0 (G01's clock) of the posterior (x, P).
PbHoldClone birth(const Eigen::VectorXd& x, const Eigen::MatrixXd& P, double stamp) {
    PbHoldClone c;
    c.sat = "G01";
    c.signal = "L1C";
    c.generation = 1;
    c.alive = true;
    PbHoldCloneTerm t = modelKeys()[0];
    t.coeff  = -1.0;
    t.s_prev = x(0);
    t.p_prev = P(0, 0);
    c.terms  = {t};
    c.value  = x(0);
    c.c_ref  = x(0);
    c.var    = 0.0;
    c.stamp_s = stamp;
    c.t0_s   = stamp;
    return c;
}

std::map<std::string, std::pair<double, double>> stashOf(const Eigen::VectorXd& x,
                                                         const Eigen::MatrixXd& P) {
    std::map<std::string, std::pair<double, double>> m;
    const auto keys = modelKeys();
    for (const auto& k : keys) m[keyOf(k)] = {x(k.col), P(k.col, k.col)};
    return m;
}

/// Run the model for @p T epochs with a clone born at @p t0 and a reinit of column
/// @p reinit_col at epoch @p t_reinit (-1 = none).  At every epoch after the birth the
/// clone's value, its variance and the increment's posterior variance are compared with
/// the augmented filter [x, c(t0)].
void runAgainstTheSmoother(int T, int t0, int reinit_col, int t_reinit, double tol) {
    std::mt19937 rng(20260929u);
    std::normal_distribution<double> nd(0.0, 1.0);
    const Eigen::VectorXd q = walkQ();
    Eigen::VectorXd truth = Eigen::VectorXd::Zero(5);
    Eigen::VectorXd x     = Eigen::VectorXd::Zero(5);
    Eigen::MatrixXd P     = Eigen::MatrixXd::Identity(5, 5);
    Eigen::VectorXd xa;   // augmented [x; c(t0)]
    Eigen::MatrixXd Pa;
    PbHoldClone clone;
    bool born = false;
    for (int t = 1; t <= T; ++t) {
        for (int j = 0; j < 5; ++j) truth(j) += std::sqrt(q(j)) * nd(rng);
        Eigen::MatrixXd H;
        Eigen::VectorXd r;
        epochRows(rng, H, r);
        Eigen::VectorXd y = H * truth;
        for (Eigen::Index i = 0; i < y.size(); ++i) y(i) += std::sqrt(r(i)) * nd(rng);
        const bool reinit_now = (t == t_reinit);
        // The exact inputs are built on the previous epoch's stash (x, P).
        const CouplingExactEpoch ep = exactEpoch(x, P, q, t - 1.0, reinit_now ? reinit_col : -1);
        const Eigen::VectorXd x_prior = x;
        // Time update (a reinit decorrelates its key: the q -> infinity limit).
        P += Eigen::MatrixXd(q.asDiagonal());
        if (reinit_now) {
            P.row(reinit_col).setZero();
            P.col(reinit_col).setZero();
            P(reinit_col, reinit_col) = kReinitVar;
        }
        kfUpdate(x, P, H, r, y);
        if (born) {
            Eigen::VectorXd qa = Eigen::VectorXd::Zero(6);
            qa.head(5) = q;
            Pa += Eigen::MatrixXd(qa.asDiagonal());
            if (reinit_now) {
                Pa.row(reinit_col).setZero();
                Pa.col(reinit_col).setZero();
                Pa(reinit_col, reinit_col) = kReinitVar;
            }
            Eigen::MatrixXd Ha = Eigen::MatrixXd::Zero(H.rows(), 6);
            Ha.leftCols(5) = H;
            kfUpdate(xa, Pa, Ha, r, y);

            const PbHoldCloneInputs in = cloneInputs(ep, x, x_prior);
            const PbHoldCloneStep st = pbHoldCloneAdvance(in, clone);
            ASSERT_TRUE(st.ok) << "epoch " << t;
            // The value is the smoother's c(t0|t).
            EXPECT_NEAR(st.value, xa(5), tol) << "epoch " << t;
            // x0 = v - m'(x - xhat) - e: Var(x0 | data) = sigma2 + m' P m.
            Eigen::VectorXd m = Eigen::VectorXd::Zero(5);
            for (const auto& tm : st.terms)
                if (tm.col >= 0 && tm.p_prev > 0.0) m(tm.col) += tm.coeff;
            EXPECT_TRUE(nearRel(st.var + m.dot(P * m), Pa(5, 5), tol)) << "epoch " << t;
            // The increment c(t) - c(t0): sigma2 + (e_c + m)' P (e_c + m).
            Eigen::VectorXd g = m;
            g(0) += 1.0;
            const double vinc = Pa(0, 0) + Pa(5, 5) - 2.0 * Pa(0, 5);
            EXPECT_TRUE(nearRel(st.var + g.dot(P * g), vinc, tol)) << "epoch " << t;
            if (reinit_now && reinit_col == 0) EXPECT_FALSE(st.parent_in_s);
            else                               EXPECT_TRUE(st.parent_in_s);
            clone.value = st.value;
            clone.var   = st.var;
            clone.terms = pbHoldCloneRebase(st.terms, stashOf(x, P), {}, nullptr);
            clone.stamp_s = t;
        }
        if (t == t0) {
            clone = birth(x, P, t);
            born  = true;
            xa.resize(6);
            xa.head(5) = x;
            xa(5) = x(0);
            Pa = Eigen::MatrixXd::Zero(6, 6);
            Pa.topLeftCorner(5, 5) = P;
            Pa.block(0, 5, 5, 1) = P.col(0);
            Pa.block(5, 0, 1, 5) = P.row(0);
            Pa(5, 5) = P(0, 0);
        }
    }
    ASSERT_TRUE(born);
}

// --- 1. The smoother -------------------------------------------------------------------

TEST(PbHoldSmoothedTest, TheCloneRowIsTheFixedPointSmoother) {
    runAgainstTheSmoother(/*T=*/40, /*t0=*/3, /*reinit_col=*/-1, /*t_reinit=*/-1, 1e-8);
}

TEST(PbHoldSmoothedTest, AReinitOfASpreadColumnIsReattributedThroughP) {
    // G01's L1C phase bias, which the clone spreads onto within one epoch.
    runAgainstTheSmoother(/*T=*/30, /*t0=*/3, /*reinit_col=*/1, /*t_reinit=*/12, 1e-6);
}

TEST(PbHoldSmoothedTest, AReinitOfTheClonesOwnClockKeepsItUnbiased) {
    runAgainstTheSmoother(/*T=*/30, /*t0=*/3, /*reinit_col=*/0, /*t_reinit=*/9, 1e-6);
}

TEST(PbHoldSmoothedTest, RestrictingTheEpochToTheFoldedKeysIsTheReinitRoute) {
    // The epoch's inputs built with every key in S, then restricted to the keys the solve
    // folded, equal the inputs built with the unfolded key on the Reinit route; the clone
    // advanced on them equals the one advanced on the reference.
    Eigen::VectorXd x(5);
    x << 0.3, 0.1, -0.2, 0.5, 0.05;
    Eigen::MatrixXd A = Eigen::MatrixXd::Random(5, 5);
    const Eigen::MatrixXd P = A * A.transpose() + Eigen::MatrixXd::Identity(5, 5);
    for (const int k : {0, 1, 3}) {
        const CouplingExactEpoch full = exactEpoch(x, P, walkQ(), 2.0, -1);
        const CouplingExactEpoch ref  = exactEpoch(x, P, walkQ(), 2.0, k);
        const CouplingExactEpoch r    = pbHoldCloneRestrictEpoch(full, {k});
        ASSERT_TRUE(r.ok);
        EXPECT_EQ(r.p_of_s, ref.p_of_s);
        EXPECT_EQ(r.col_of_s, ref.col_of_s);
        EXPECT_EQ(r.s_of_p, ref.s_of_p);
        EXPECT_EQ(r.route_p, ref.route_p);
        EXPECT_TRUE(r.q_s.isApprox(ref.q_s));
        EXPECT_TRUE(r.shat_s.isApprox(ref.shat_s));
        EXPECT_TRUE(r.F.isApprox(ref.F, 1e-12));
        PbHoldClone c = birth(x, P, 2.0);
        Eigen::VectorXd post = x;
        post(0) += 0.01;
        post(2) -= 0.02;
        const PbHoldCloneStep a = pbHoldCloneAdvance(cloneInputs(r, post, x), c);
        const PbHoldCloneStep b = pbHoldCloneAdvance(cloneInputs(ref, post, x), c);
        ASSERT_TRUE(a.ok);
        ASSERT_TRUE(b.ok);
        EXPECT_NEAR(a.value, b.value, 1e-12);
        EXPECT_NEAR(a.var, b.var, 1e-12);
    }
    const CouplingExactEpoch full = exactEpoch(x, P, walkQ(), 2.0, -1);
    const CouplingExactEpoch same = pbHoldCloneRestrictEpoch(full, {});
    EXPECT_TRUE(same.F.isApprox(full.F));
}

// --- 2. The value side ---------------------------------------------------------------

TEST(PbHoldSmoothedTest, AFrameStepOfThePriorMeansDoesNotStepTheClone) {
    Eigen::VectorXd x(5);
    x << 0.3, 0.1, -0.2, 0.5, 0.05;
    Eigen::MatrixXd A = Eigen::MatrixXd::Random(5, 5);
    const Eigen::MatrixXd P = A * A.transpose() + Eigen::MatrixXd::Identity(5, 5);
    const CouplingExactEpoch ep = exactEpoch(x, P, walkQ(), 7.0, -1);
    PbHoldClone c = birth(x, P, 7.0);
    Eigen::VectorXd post = x;
    post(0) += 0.02;
    post(1) -= 0.01;
    const PbHoldCloneStep a = pbHoldCloneAdvance(cloneInputs(ep, post, x), c);
    // A declared step of both clocks (a pivot handover's re-realization) in the prior
    // means and, with it, in the posterior.
    Eigen::VectorXd shift = Eigen::VectorXd::Zero(5);
    shift(0) = 1.7;
    shift(3) = 1.7;
    const PbHoldCloneStep b = pbHoldCloneAdvance(cloneInputs(ep, post + shift, x + shift), c);
    ASSERT_TRUE(a.ok);
    ASSERT_TRUE(b.ok);
    EXPECT_NEAR(a.value, b.value, 1e-12) << "a declared frame step must land on C alone";
    // A clone on the constellation's pinned clock: exactly known, so no revision whatever
    // its column reads (a transfer of zero), and the term is kept as harvested.
    PbHoldCloneTerm pin = modelKeys()[3];
    pin.coeff  = -1.0;
    pin.s_prev = 2.0;
    pin.p_prev = 0.0;
    PbHoldClone cp = c;
    cp.sat   = "G02";
    cp.terms = {pin};
    cp.value = 2.0;
    cp.c_ref = 2.0;
    CouplingExactEpoch ep2 = ep;
    ep2.p_of_key.erase(keyOf(pin));   // the pinned clock is not in the stash
    const PbHoldCloneStep d = pbHoldCloneAdvance(cloneInputs(ep2, post + shift, x), cp);
    ASSERT_TRUE(d.ok);
    EXPECT_DOUBLE_EQ(d.value, 2.0);
    bool kept = false;
    for (const auto& t : d.terms)
        if (keyOf(t) == keyOf(pin) && !(t.p_prev > 0.0) && t.coeff == -1.0) kept = true;
    EXPECT_TRUE(kept) << "a pinned term is kept as harvested";
}

// --- 3. No fallback ----------------------------------------------------------------------

TEST(PbHoldSmoothedTest, ACloneDiesRatherThanTakeTheShrink) {
    Eigen::VectorXd x = Eigen::VectorXd::Zero(5);
    const Eigen::MatrixXd P = Eigen::MatrixXd::Identity(5, 5);
    CouplingExactEpoch ep = exactEpoch(x, P, walkQ(), 4.0, -1);
    PbHoldClone c = birth(x, P, 4.0);

    CouplingExactEpoch bad = ep;
    bad.ok = false;
    PbHoldCloneStep st = pbHoldCloneAdvance(cloneInputs(bad, x, x), c);
    EXPECT_FALSE(st.ok);
    EXPECT_EQ(st.death, PbHoldCloneDeath::Unfactored);

    PbHoldClone stale = c;
    stale.stamp_s = 3.0;
    st = pbHoldCloneAdvance(cloneInputs(ep, x, x), stale);
    EXPECT_FALSE(st.ok);
    EXPECT_EQ(st.death, PbHoldCloneDeath::Stale);

    CouplingExactEpoch outside = ep;
    outside.p_of_key.erase("G01|CLK");
    st = pbHoldCloneAdvance(cloneInputs(outside, x, x), c);
    EXPECT_FALSE(st.ok);
    EXPECT_EQ(st.death, PbHoldCloneDeath::ParentOutside)
        << "the clone's own clock outside the block is card 1d's route in all but name";
}

TEST(PbHoldSmoothedTest, TheRebaseTakesTheNewStashAndKeepsThePinnedPivot) {
    std::vector<PbHoldCloneTerm> terms = modelKeys();
    terms[0].coeff = -0.9;
    terms[1].coeff = 0.2;
    terms[2].coeff = 1e-12;    // below the relative sparsification bar
    terms[3].coeff = 0.3;      // the pinned pivot
    terms[4].coeff = 0.1;      // not stashed: falls back to the solve
    std::map<std::string, std::pair<double, double>> stash = {
        {"G01|CLK", {1.0, 0.04}}, {"G01|PB|L1C", {2.0, 0.01}}, {"G01|PB|L2W", {3.0, 0.01}}};
    std::map<std::string, double> pinned = {{"G02|CLK", 5.0}};
    const auto fb = [](Eigen::Index c) { return std::pair<double, double>{10.0 + c, 0.5}; };
    const auto out = pbHoldCloneRebase(terms, stash, pinned, fb);
    ASSERT_EQ(out.size(), 4u);
    EXPECT_DOUBLE_EQ(out[0].s_prev, 1.0);
    EXPECT_DOUBLE_EQ(out[0].p_prev, 0.04);
    EXPECT_DOUBLE_EQ(out[1].s_prev, 2.0);
    EXPECT_EQ(keyOf(out[2]), "G02|CLK");
    EXPECT_DOUBLE_EQ(out[2].s_prev, 5.0);
    EXPECT_DOUBLE_EQ(out[2].p_prev, 0.0);
    EXPECT_EQ(keyOf(out[3]), "G02|PB|L1C");
    EXPECT_DOUBLE_EQ(out[3].s_prev, 14.0);
    EXPECT_DOUBLE_EQ(out[3].p_prev, 0.5);
}

// --- 4. The hold -----------------------------------------------------------------------

std::shared_ptr<spdlog::logger> smhLogger() {
    static auto sink = std::make_shared<spdlog::sinks::null_sink_mt>();
    return std::make_shared<spdlog::logger>("smh_test", sink);
}

std::string smhConfig(bool hold, bool smoothed, bool phase_clock, bool exact, bool within = true,
                      bool anchor = true, const std::string& extra = std::string{}) {
    const auto b = [](bool v) { return std::string(v ? "true" : "false"); };
    return std::string("{\n") +
        "  \"processing\": {\n"
        "    \"mode\": \"ppp-rtk-network\",\n"
        "    \"active_systems\": [ \"GPS\" ],\n"
        "    \"signal_priorities\": { \"GPS\": \"WC\" }\n"
        "  },\n"
        "  \"network\": {\n"
        "    \"emit_pb_hold\": " + b(hold) + ",\n"
        "    \"emit_pb_hold_smoothed\": " + b(smoothed) + ",\n"
        "    \"emit_pb_hold_phase_clock\": " + b(phase_clock) + ",\n"
        "    \"carry_conditional_coupling\": " + b(exact) + ",\n"
        "    \"carry_coupling_time_update\": " + b(exact) + ",\n"
        "    \"shared_carry_full_cov\": " + b(exact) + ",\n"
        "    \"carry_coupling_exact_update\": " + b(exact) + ",\n"
        "    \"raim\": " + b(within) + ",\n"
        "    \"raim_within_epoch\": " + b(within) + ",\n"
        "    \"clock_temporal_anchor\": " + b(anchor) + ",\n" +
        extra +
        "    \"stations\": [\n"
        "      { \"name\": \"AAAA\", \"position_ecef\": [ -2493304.99, -4655214.79, 3565497.67 ] },\n"
        "      { \"name\": \"BBBB\", \"position_ecef\": [ -2281621.75, -1453585.08, 5756964.89 ] }\n"
        "    ]\n"
        "  }\n"
        "}\n";
}

bool smhLoads(bool hold, bool smoothed, bool phase_clock, bool exact, const char* tag,
              bool within = true, bool anchor = true, const std::string& extra = std::string{}) {
    LogManager log_mgr;
    GnssConfig cfg;
    const fs::path p = fs::temp_directory_path() / (std::string("smh_load_") + tag + ".json");
    { std::ofstream f(p); f << smhConfig(hold, smoothed, phase_clock, exact, within, anchor, extra); }
    const bool ok = cfg.loadFromJson(p, log_mgr);
    std::error_code ec; fs::remove(p, ec);
    return ok;
}

TEST(PbHoldSmoothedTest, LoaderRequiresTheHoldAndTheExactUpdateAndExcludesThePhaseClockMode) {
    EXPECT_TRUE(smhLoads(true, true, false, true, "ok"));
    EXPECT_FALSE(smhLoads(false, true, false, true, "nohold")) << "a mode of the hold";
    EXPECT_FALSE(smhLoads(true, true, false, false, "noexact"))
        << "card 1d's shrink would destroy the clone in one epoch";
    EXPECT_FALSE(smhLoads(true, true, true, true, "both"))
        << "the phase-clock transfer is the smoothed hold's fallback, not a second mode";
    EXPECT_TRUE(smhLoads(true, false, false, false, "off"));
    EXPECT_FALSE(smhLoads(true, true, false, true, "nowithin", /*within=*/false))
        << "the within-epoch fold capture is the record of what the final solve folded";
    EXPECT_TRUE(smhLoads(true, true, false, true, "within_implied", /*within=*/false,
                         /*anchor=*/true, "    \"raim_withhold_sigma0\": 6.0,\n"))
        << "the refusal reads the resolved tier, which the withhold ceiling arms";
    EXPECT_FALSE(smhLoads(true, true, false, true, "noanchor", /*within=*/true, /*anchor=*/false))
        << "without card Q3's anchor the clock's q is a datum device (section 9)";
}

/// One satellite of a test epoch.
struct SmhSat {
    std::string sat;
    double      clk;
    double      pb;
};

NetworkSolutionEpoch smhEpoch(double tow, const std::vector<SmhSat>& sats) {
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

TEST(PbHoldSmoothedTest, TheHoldWritesTheClonesTransferAndRevertsContinuously) {
    LogManager log_mgr;
    GnssConfig cfg;
    const fs::path cfg_path = fs::temp_directory_path() / "smh_cfg_hold.json";
    { std::ofstream f(cfg_path); f << smhConfig(true, true, false, true); }
    ASSERT_TRUE(cfg.loadFromJson(cfg_path, log_mgr));
    auto ctx = std::make_unique<NetworkRunContext>(cfg, cfg.getNetworkConfig());
    ASSERT_TRUE(ctx->knobs.emit_pb_hold_smoothed) << "the knob must reach the run snapshot";
    const fs::path dir = fs::temp_directory_path() / "smh_traces";
    fs::create_directories(dir);
    NetworkRunTraces traces(dir);
    NetworkProductEmitter emitter(0.05, /*emit_clock_disc=*/false);
    SsrCorrectionStore store;
    SyncedEpoch se;
    const auto step = [&](NetworkSolutionEpoch& sol, long long ep) {
        ctx->counters.n_loop_epochs = static_cast<decltype(ctx->counters.n_loop_epochs)>(ep);
        se.time = sol.epoch_time;
        networkPbHold(*ctx, cfg, smhLogger(), traces, emitter, se, sol);
        emitter.emitToStore(sol, store);
    };
    // Epoch 1: the generation starts, the clone's birth is queued.
    auto e1 = smhEpoch(100.0, {{"G32", 10.0, 1.0}, {"G12", 20.0, 2.0}, {"G05", 30.0, 3.0}});
    step(e1, 1);
    EXPECT_DOUBLE_EQ(pbOf(e1, "G32"), 1.0);
    ASSERT_EQ((net::getSig(ctx->entities.sat_signals, "G32|L1W", &net::SatSignalState::smh_pending) ? 1u : 0u), 1u);
    // The stash of epoch 1 gives G32 a clone (the harvest's birth, by hand here).
    PbHoldClone c = (*net::getSig(ctx->entities.sat_signals, "G32|L1W", &net::SatSignalState::smh_pending));
    c.alive = true;
    c.value = 5.0;
    c.c_ref = 5.0;
    net::putSig(ctx->entities, "G32|L1W", &net::SatSignalState::smh_clone) = c;
    net::dropAll(ctx->entities.sat_signals, &net::SatSignalState::smh_pending);
    // Epoch 2: the clone revised c(t0) by +3 cm with no step of the clock: the passed motion
    // is -3 cm, and the held bias follows the revision.
    net::putSig(ctx->entities, "G32|L1W", &net::SatSignalState::smh_clone).phi        = -0.03;
    net::putSig(ctx->entities, "G32|L1W", &net::SatSignalState::smh_clone).updated_ep = 2;
    auto e2 = smhEpoch(130.0, {{"G32", 10.0, 1.5}, {"G12", 20.0, 2.5}, {"G05", 30.0, 3.5}});
    step(e2, 2);
    EXPECT_NEAR(pbOf(e2, "G32"), 1.03, 1e-12);
    EXPECT_EQ(ctx->counters.smh_held_smoothed, 1);
    // G12 and G05 had no clone: they carry the phase-clock transfer from the start.
    EXPECT_NEAR(pbOf(e2, "G12"), 2.0, 1e-12);
    // Epoch 3: the clone dies; the key reverts with its last transfer.
    net::putSig(ctx->entities, "G32|L1W", &net::SatSignalState::smh_clone).alive = false;
    auto e3 = smhEpoch(160.0, {{"G32", 10.0, 1.7}, {"G12", 20.0, 2.7}, {"G05", 30.0, 3.7}});
    step(e3, 3);
    EXPECT_NEAR(pbOf(e3, "G32"), 1.03, 1e-12) << "continuous at the switch";
    // Epoch 4: G32's own clock moves 5 cm (the others hold): the phase-clock transfer.
    auto e4 = smhEpoch(190.0, {{"G32", 10.05, 1.9}, {"G12", 20.0, 2.9}, {"G05", 30.0, 3.9}});
    step(e4, 4);
    EXPECT_NEAR(pbOf(e4, "G32"), 1.08, 1e-12);
    EXPECT_EQ(ctx->counters.smh_reverts, 1) << "G32 at its death; G12 and G05 bridge, waiting for a birth";
    EXPECT_EQ(ctx->counters.smh_held_lah, 2);
    EXPECT_EQ(ctx->counters.smh_held_bridged, 6);
    std::error_code ec; fs::remove(cfg_path, ec);
}

/// A run context from the smoothed-hold config, for the seam tests below.
std::unique_ptr<NetworkRunContext> smhContext(GnssConfig& cfg, const char* tag) {
    LogManager log_mgr;
    const fs::path p = fs::temp_directory_path() / (std::string("smh_ctx_") + tag + ".json");
    { std::ofstream f(p); f << smhConfig(true, true, false, true); }
    EXPECT_TRUE(cfg.loadFromJson(p, log_mgr));
    std::error_code ec; fs::remove(p, ec);
    return std::make_unique<NetworkRunContext>(cfg, cfg.getNetworkConfig());
}

/// The run the clone seams' epoch forms take (tests/support/NetworkTestRun.hpp), over the
/// configuration smhContext loads.
std::unique_ptr<NetworkTestRun> smhRun(GnssConfig& cfg, const char* tag) {
    (void)smhContext(cfg, tag);   // loads cfg
    return std::make_unique<NetworkTestRun>(cfg, smhLogger());
}

PbHoldClone liveClone(const std::string& sat, double v, long long ep) {
    PbHoldClone c;
    c.sat        = sat;
    c.signal     = "L1W";
    c.generation = 1;
    c.alive      = true;
    c.value      = v;
    c.c_ref      = v;
    c.t0_s       = static_cast<double>(ep);
    c.updated_ep = ep;
    return c;
}

TEST(PbHoldSmoothedTest, TheBridgeKeepsTheMedianAndTheRebornCloneContinuesIt) {
    GnssConfig cfg;
    auto ctx = smhContext(cfg, "bridge");
    const fs::path dir = fs::temp_directory_path() / "smh_traces_bridge";
    fs::create_directories(dir);
    NetworkRunTraces traces(dir);
    NetworkProductEmitter emitter(0.05, /*emit_clock_disc=*/false);
    SsrCorrectionStore store;
    SyncedEpoch se;
    const auto step = [&](NetworkSolutionEpoch& sol, long long ep) {
        ctx->counters.n_loop_epochs = static_cast<decltype(ctx->counters.n_loop_epochs)>(ep);
        se.time = sol.epoch_time;
        networkPbHold(*ctx, cfg, smhLogger(), traces, emitter, se, sol);
        emitter.emitToStore(sol, store);
    };
    auto e1 = smhEpoch(100.0, {{"G32", 10.0, 1.0}, {"G12", 20.0, 2.0}, {"G05", 30.0, 3.0}});
    step(e1, 1);
    EXPECT_EQ((*net::getSig(ctx->entities.sat_signals, "G32|L1W", &net::SatSignalState::smh_pending)).queued_ep, 1);
    net::putSig(ctx->entities, "G32|L1W", &net::SatSignalState::smh_clone) = liveClone("G32", 5.0, 1);
    net::dropAll(ctx->entities.sat_signals, &net::SatSignalState::smh_pending);
    // Epoch 2: the clone's revision +3 cm (passed motion -3 cm).
    net::putSig(ctx->entities, "G32|L1W", &net::SatSignalState::smh_clone).phi        = -0.03;
    net::putSig(ctx->entities, "G32|L1W", &net::SatSignalState::smh_clone).updated_ep = 2;
    auto e2 = smhEpoch(130.0, {{"G32", 10.0, 1.5}, {"G12", 20.0, 2.5}, {"G05", 30.0, 3.5}});
    step(e2, 2);
    EXPECT_NEAR(pbOf(e2, "G32"), 1.03, 1e-12);
    // Epoch 3: the anchor withdraws G32's clock: the key bridges, continuous, not reverted.
    net::putSig(ctx->entities, "G32|L1W", &net::SatSignalState::smh_clone).alive = false;
    net::putSig(ctx->entities, "G32|L1W", &net::SatSignalState::smh_clone).death = PbHoldCloneDeath::Withdrawn;
    auto e3 = smhEpoch(160.0, {{"G32", 10.0, 1.7}, {"G12", 20.0, 2.7}, {"G05", 30.0, 3.7}});
    step(e3, 3);
    EXPECT_NEAR(pbOf(e3, "G32"), 1.03, 1e-12) << "continuous at the switch to the bridge";
    EXPECT_EQ(ctx->counters.smh_bridges, 1);
    EXPECT_EQ(ctx->counters.smh_reverts, 0);
    // Epoch 4: every clock moves, G32 by 5 cm and its mates by 2 cm: the bridge is the
    // phase-clock transfer, so the constellation's median step stays on the clock.
    auto e4 = smhEpoch(190.0, {{"G32", 10.05, 1.9}, {"G12", 20.02, 2.9}, {"G05", 30.02, 3.9}});
    step(e4, 4);
    EXPECT_NEAR(pbOf(e4, "G32"), 1.06, 1e-12);
    // Epoch 5: the clone is born again (the anchor holds) and revises its c(t_b) by +2 cm:
    // its transfer continues the bridge's.
    net::putSig(ctx->entities, "G32|L1W", &net::SatSignalState::smh_clone) = liveClone("G32", 7.0, 4);
    net::putSig(ctx->entities, "G32|L1W", &net::SatSignalState::smh_clone).phi        = -0.02;
    net::putSig(ctx->entities, "G32|L1W", &net::SatSignalState::smh_clone).updated_ep = 5;
    auto e5 = smhEpoch(220.0, {{"G32", 10.05, 2.1}, {"G12", 20.02, 3.1}, {"G05", 30.02, 4.1}});
    step(e5, 5);
    EXPECT_NEAR(pbOf(e5, "G32"), 1.08, 1e-12) << "the reborn clone's transfer is on top of the bridge's";
    EXPECT_EQ(ctx->counters.smh_held_smoothed, 2);
    EXPECT_EQ(ctx->counters.smh_reverts, 0);
}

TEST(PbHoldSmoothedTest, ASwitchToTheBridgeKeepsTheEpochsClockStep) {
    // Section 12: at the epoch a clone dies the key takes LAH's step of that epoch on top of
    // the transfer it had reached, so C - P does not step.  G32's clock moves 5 cm at the
    // death epoch while its mates hold (the constellation's median step is 0).
    GnssConfig cfg;
    auto ctx = smhContext(cfg, "bridgestep");
    const fs::path dir = fs::temp_directory_path() / "smh_traces_bridgestep";
    fs::create_directories(dir);
    NetworkRunTraces traces(dir);
    NetworkProductEmitter emitter(0.05, /*emit_clock_disc=*/false);
    SsrCorrectionStore store;
    SyncedEpoch se;
    const auto step = [&](NetworkSolutionEpoch& sol, long long ep) {
        ctx->counters.n_loop_epochs = static_cast<decltype(ctx->counters.n_loop_epochs)>(ep);
        se.time = sol.epoch_time;
        networkPbHold(*ctx, cfg, smhLogger(), traces, emitter, se, sol);
        emitter.emitToStore(sol, store);
    };
    auto e1 = smhEpoch(100.0, {{"G32", 10.0, 1.0}, {"G12", 20.0, 2.0}, {"G05", 30.0, 3.0}});
    step(e1, 1);
    net::putSig(ctx->entities, "G32|L1W", &net::SatSignalState::smh_clone) = liveClone("G32", 5.0, 1);
    net::dropAll(ctx->entities.sat_signals, &net::SatSignalState::smh_pending);
    net::putSig(ctx->entities, "G32|L1W", &net::SatSignalState::smh_clone).phi        = -0.03;
    net::putSig(ctx->entities, "G32|L1W", &net::SatSignalState::smh_clone).updated_ep = 2;
    auto e2 = smhEpoch(130.0, {{"G32", 10.0, 1.5}, {"G12", 20.0, 2.5}, {"G05", 30.0, 3.5}});
    step(e2, 2);
    EXPECT_NEAR(pbOf(e2, "G32"), 1.03, 1e-12);
    // Epoch 3: the anchor withdraws G32's clock, which moves 5 cm this epoch.
    net::putSig(ctx->entities, "G32|L1W", &net::SatSignalState::smh_clone).alive = false;
    net::putSig(ctx->entities, "G32|L1W", &net::SatSignalState::smh_clone).death = PbHoldCloneDeath::Withdrawn;
    auto e3 = smhEpoch(160.0, {{"G32", 10.05, 1.7}, {"G12", 20.0, 2.7}, {"G05", 30.0, 3.7}});
    step(e3, 3);
    EXPECT_NEAR(pbOf(e3, "G32"), 1.08, 1e-12) << "the bridge takes this epoch's clock step";
    // Epoch 4: the bridge goes on with LAH's steps, the -3 cm the clone reached kept.
    auto e4 = smhEpoch(190.0, {{"G32", 10.07, 1.9}, {"G12", 20.0, 2.9}, {"G05", 30.0, 3.9}});
    step(e4, 4);
    EXPECT_NEAR(pbOf(e4, "G32"), 1.10, 1e-12);
    EXPECT_EQ(ctx->counters.smh_bridges, 1);
}

TEST(PbHoldSmoothedTest, AZeroPassedMotionWritesThePhaseClockTransfer) {
    // Section 10: the members share LAH's frame.  G32 is smoothed with no passed motion, G12
    // and G05 bridge; the clocks move by different amounts, so the constellation's median
    // step (2 cm) is not a common mode of the three keys.  The smoothed key must write what
    // LAH writes, or it and its bridged neighbours drift apart by the median.
    GnssConfig cfg;
    auto ctx = smhContext(cfg, "frame");
    const fs::path dir = fs::temp_directory_path() / "smh_traces_frame";
    fs::create_directories(dir);
    NetworkRunTraces traces(dir);
    NetworkProductEmitter emitter(0.05, /*emit_clock_disc=*/false);
    SsrCorrectionStore store;
    SyncedEpoch se;
    const auto step = [&](NetworkSolutionEpoch& sol, long long ep) {
        ctx->counters.n_loop_epochs = static_cast<decltype(ctx->counters.n_loop_epochs)>(ep);
        se.time = sol.epoch_time;
        networkPbHold(*ctx, cfg, smhLogger(), traces, emitter, se, sol);
        emitter.emitToStore(sol, store);
    };
    auto e1 = smhEpoch(100.0, {{"G32", 10.0, 1.0}, {"G12", 20.0, 2.0}, {"G05", 30.0, 3.0}});
    step(e1, 1);
    net::putSig(ctx->entities, "G32|L1W", &net::SatSignalState::smh_clone) = liveClone("G32", 5.0, 1);
    net::dropAll(ctx->entities.sat_signals, &net::SatSignalState::smh_pending);
    for (int ep = 2; ep <= 4; ++ep) {
        net::putSig(ctx->entities, "G32|L1W", &net::SatSignalState::smh_clone).updated_ep = ep;
        const double k = static_cast<double>(ep - 1);
        auto e = smhEpoch(100.0 + 30.0 * k, {{"G32", 10.0 + 0.05 * k, 1.0},
                                              {"G12", 20.0 + 0.02 * k, 2.0},
                                              {"G05", 30.0 + 0.02 * k, 3.0}});
        step(e, ep);
        EXPECT_NEAR(pbOf(e, "G32"), 1.0 + 0.03 * k, 1e-12) << "epoch " << ep;
        EXPECT_NEAR(pbOf(e, "G12"), 2.0, 1e-12);
    }
    EXPECT_EQ(ctx->counters.smh_held_smoothed, 3);
}

TEST(PbHoldSmoothedTest, ADisengagedParentKillsTheCloneAndQueuesTheBirthAgain) {
    GnssConfig cfg;
    auto fx = smhRun(cfg, "disengage");
    NetworkRunContext* ctx = &fx->ctx();
    ASSERT_TRUE(ctx->knobs.cta_on) << "the loader requires the anchor";
    ctx->counters.n_loop_epochs = 7;
    for (const char* s : {"G32", "G12", "G05", "G07"})
        net::putSig(ctx->entities, std::string(s) + "|L1W", &net::SatSignalState::smh_clone) = liveClone(s, 1.0, 6);
    auto& an = ctx->carry.clk_anchor;
    an.sat["G32"].anchored = true;  an.sat["G32"].folded_anchored = true;
    an.tripped.insert("G32");                                  // withdrawn this epoch
    an.sat["G12"].anchored = false; an.sat["G12"].folded_anchored = false;   // LOOSE fold
    an.sat["G05"].anchored = true;  an.sat["G05"].folded_anchored = true;    // engaged
    net::put(ctx->entities.constellations, System::GPS, &net::ConstellationState::cxu_pivot) = {"G07", 0.0};                  // G07 pinned: no anchor fold
    NetworkEpoch& e = fx->newEpoch(GnssTime{});   // an empty workspace and datum, the run's estimator
    e.solve.x_s = Eigen::VectorXd::Zero(1);
    networkPbHoldCloneUpdate(fx->run(), e);
    const auto cl = net::sigMap(ctx->entities.sat_signals, &net::SatSignalState::smh_clone, '|');
    EXPECT_EQ(cl.at("G32|L1W").death, PbHoldCloneDeath::Withdrawn);
    EXPECT_EQ(cl.at("G12|L1W").death, PbHoldCloneDeath::Disengaged);
    // The engaged and the pinned clones go on to the exact route, which this empty epoch
    // cannot give them: a structural death, no re-birth.
    EXPECT_EQ(cl.at("G05|L1W").death, PbHoldCloneDeath::Unfactored);
    EXPECT_EQ(cl.at("G07|L1W").death, PbHoldCloneDeath::Unfactored);
    const auto pend = net::sigMap(ctx->entities.sat_signals, &net::SatSignalState::smh_pending, '|');
    ASSERT_EQ(pend.size(), 2u);
    EXPECT_TRUE(pend.at("G32|L1W").rebirth);
    EXPECT_EQ(pend.at("G32|L1W").queued_ep, 7);
    EXPECT_EQ(pend.at("G12|L1W").generation, 1u);
}

TEST(PbHoldSmoothedTest, ABirthWaitsForTheAnchorAndForAStashedClock) {
    GnssConfig cfg;
    auto fx = smhRun(cfg, "defer");
    NetworkRunContext* ctx = &fx->ctx();
    auto& carry = ctx->carry;
    for (const char* s : {"G32", "G12", "G05"}) {
        net::PbHoldKey k;
        k.generation = 1;
        net::putSig(ctx->entities, std::string(s) + "|L1W", &net::SatSignalState::pb_hold) = k;
    }
    net::putSig(ctx->entities, "G05|L1W", &net::SatSignalState::pb_hold).generation = 2;   // G05's pending birth is of an ended generation
    for (const char* s : {"G32", "G12", "G05"}) {
        PbHoldClone pc;
        pc.sat = s; pc.signal = "L1W"; pc.generation = 1; pc.queued_ep = 1;
        net::putSig(ctx->entities, std::string(s) + "|L1W", &net::SatSignalState::smh_pending) = pc;
    }
    net::putSig(ctx->entities, "G12|L1W", &net::SatSignalState::smh_pending).rebirth = true;
    // The stash holds G32's clock only; G12's clock is anchored but not stashed.
    CarryPrior cp{};
    cp.kind = SharedParamKind::Clock; cp.sat = "G32"; cp.value = 3.0; cp.var = 0.01;
    carry.prev_carry = {cp};
    carry.prev_carry_cov.row["G32|CLK"] = 0;
    carry.prev_carry_cov.P = Eigen::MatrixXd::Constant(1, 1, 0.01);
    carry.clk_anchor.sat["G32"].anchored = false;               // LOOSE
    carry.clk_anchor.sat["G12"].anchored = true;
    const auto harvest = [&](long long ep) {
        ctx->counters.n_loop_epochs = static_cast<decltype(ctx->counters.n_loop_epochs)>(ep);
        carry.cxu_stash_valid = true;
        carry.cxu_stash_s     = 100.0 * static_cast<double>(ep);
        NetworkEpoch& e = fx->newEpoch(GnssTime{ 0, carry.cxu_stash_s });   // continuous GPS seconds = the stash's
        e.solve.x_s = Eigen::VectorXd::Zero(1);
        networkPbHoldCloneHarvest(fx->run(), e);
    };
    harvest(1);
    EXPECT_EQ(ctx->counters.smh_pending_dropped, 1) << "G05's generation ended";
    EXPECT_EQ(ctx->counters.smh_defer_loose, 1);
    EXPECT_EQ(ctx->counters.smh_defer_nostash, 1);
    EXPECT_EQ((net::getSig(ctx->entities.sat_signals, "G32|L1W", &net::SatSignalState::smh_clone) ? 1u : 0u), 0u);
    harvest(2);
    carry.clk_anchor.sat["G32"].anchored = true;                // the anchor engages
    harvest(3);
    ASSERT_EQ((net::getSig(ctx->entities.sat_signals, "G32|L1W", &net::SatSignalState::smh_clone) ? 1u : 0u), 1u);
    const PbHoldClone& c = (*net::getSig(ctx->entities.sat_signals, "G32|L1W", &net::SatSignalState::smh_clone));
    EXPECT_TRUE(c.alive);
    EXPECT_DOUBLE_EQ(c.value, 3.0);
    EXPECT_DOUBLE_EQ(c.c_ref, 3.0);
    EXPECT_DOUBLE_EQ(c.t0_s, 300.0);
    EXPECT_EQ(ctx->counters.smh_births, 1);
    EXPECT_EQ(ctx->counters.smh_births_deferred, 1);
    ASSERT_EQ(ctx->counters.smh_defer_ep.size(), 1u);
    EXPECT_DOUBLE_EQ(ctx->counters.smh_defer_ep[0], 2.0);
    EXPECT_EQ(ctx->counters.smh_defer_loose, 2);
    EXPECT_EQ(ctx->counters.smh_rebirths, 0);
    EXPECT_EQ((net::getSig(ctx->entities.sat_signals, "G12|L1W", &net::SatSignalState::smh_pending) ? 1u : 0u), 1u) << "still waiting for a stashed clock";
    EXPECT_EQ((net::getSig(ctx->entities.sat_signals, "G32|L1W", &net::SatSignalState::smh_pending) ? 1u : 0u), 0u);
    // A withdrawal in the stash's own epoch defers the birth even with the anchor set.
    net::dropAll(ctx->entities.sat_signals, &net::SatSignalState::smh_pending);
    net::dropAll(ctx->entities.sat_signals, &net::SatSignalState::smh_clone);
    PbHoldClone pc;
    pc.sat = "G32"; pc.signal = "L1W"; pc.generation = 1; pc.queued_ep = 3; pc.rebirth = true;
    net::putSig(ctx->entities, "G32|L1W", &net::SatSignalState::smh_pending) = pc;
    carry.clk_anchor.tripped.insert("G32");
    harvest(4);
    EXPECT_EQ((net::getSig(ctx->entities.sat_signals, "G32|L1W", &net::SatSignalState::smh_clone) ? 1u : 0u), 0u);
    carry.clk_anchor.tripped.clear();
    harvest(5);
    EXPECT_EQ((net::getSig(ctx->entities.sat_signals, "G32|L1W", &net::SatSignalState::smh_clone) ? 1u : 0u), 1u);
    EXPECT_EQ(ctx->counters.smh_rebirths, 1);
}

// --- 5. The contract ---------------------------------------------------------------------

std::string slurp(const fs::path& p) {
    std::ifstream f(p);
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

TEST(PbHoldSmoothedTest, SLineWritesPbhold3AndCheckContractNamesADisagreement) {
    const fs::path p = fs::temp_directory_path() / "smh_contract.txt";
    SbasisContract c;
    c.present = true;
    c.apriori_product_id = "CODFIN2024129";
    c.emit_pb_hold = true;
    c.emit_pb_hold_smoothed = true;
    { NetworkProductWriter w(p.string(), c); }
    const std::string body = slurp(p);
    EXPECT_NE(body.find(",pbhold=3"), std::string::npos);
    bool ok = false;
    SbasisContract back;
    NetworkProductReader::read(p.string(), &back, &ok);
    EXPECT_TRUE(ok);
    EXPECT_TRUE(back.emit_pb_hold);
    EXPECT_TRUE(back.emit_pb_hold_smoothed);
    EXPECT_FALSE(back.emit_pb_hold_phase_clock);
    SbasisContract expected = back;
    expected.emit_pb_hold_smoothed = false;
    std::vector<std::string> notes;
    EXPECT_FALSE(NetworkProductReader::checkContract(back, expected, &notes));
    bool named = false;
    for (const auto& n : notes)
        if (n.find("emit_pb_hold_smoothed") != std::string::npos) named = true;
    EXPECT_TRUE(named);
    fs::remove(p);
}

}  // namespace
}  // namespace GnssProcessor
