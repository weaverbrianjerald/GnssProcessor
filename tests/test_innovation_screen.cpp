/**
 * @file test_innovation_screen.cpp
 * @brief The consumer's pre-update innovation screen: the conditional statistic, the
 *        sequential removal, the abstain rules and the exclusion episodes.
 *
 * The statistic is tested against a direct computation (the Gaussian conditional of one
 * row given the others), on a toy system that has the structure the screen exists for: a
 * loose receiver clock shared by every row, loose position, and phase rows with tight
 * ambiguities. The same system shows why the marginal form cannot do the job: the
 * 92 m phase step is a fraction of one marginal standard deviation, and many conditional
 * standard deviations.
 */

#include <gtest/gtest.h>

#include "realtime/InnovationScreen.hpp"

#include <cmath>
#include <cstdio>
#include <random>
#include <string>
#include <vector>

namespace GnssProcessor {
namespace {

using Row = InnovationScreen::Row;
using Kind = InnovationScreen::EventKind;

/// A static consumer epoch: states [clock, x, y, z, one ambiguity per satellite], one code
/// and one phase row per satellite.
struct Toy {
    int n_sat{ 0 };
    Eigen::MatrixXd H, P, R, S;
    std::vector<Row> rows;

    /// Index of the code row of satellite @p s and of its phase row.
    int codeRow(int s) const { return 2 * s; }
    int phaseRow(int s) const { return 2 * s + 1; }
};

Toy makeToy(int n_sat, double clock_var = 30000.0, double amb_var = 4.0e-4) {
    Toy t;
    t.n_sat = n_sat;
    const int n = 4 + n_sat;
    const int m = 2 * n_sat;
    t.H = Eigen::MatrixXd::Zero(m, n);
    t.R = Eigen::MatrixXd::Zero(m, m);
    t.rows.resize(static_cast<size_t>(m));
    for (int s = 0; s < n_sat; ++s) {
        const double az = 6.283185307 * s / n_sat;
        const double el = 0.35 + 1.0 * ((s * 7) % n_sat) / n_sat;
        const Eigen::Vector3d u(std::cos(el) * std::cos(az), std::cos(el) * std::sin(az),
                                std::sin(el));
        char sat[8];
        std::snprintf(sat, sizeof(sat), "G%02d", s + 1);
        for (int k = 0; k < 2; ++k) {
            const int r = 2 * s + k;
            t.H(r, 0) = 1.0;
            t.H.block(r, 1, 1, 3) = u.transpose();
            if (k == 1) t.H(r, 4 + s) = 1.0;
            t.R(r, r) = (k == 0) ? 0.25 : 9.0e-6;
            t.rows[static_cast<size_t>(r)].sat = sat;
            t.rows[static_cast<size_t>(r)].signal = (k == 0) ? "C1W" : "L1W";
        }
    }
    t.P = Eigen::MatrixXd::Zero(n, n);
    t.P(0, 0) = clock_var;
    for (int i = 1; i <= 3; ++i) t.P(i, i) = 100.0;
    for (int s = 0; s < n_sat; ++s) t.P(4 + s, 4 + s) = amb_var;
    t.S = t.H * t.P * t.H.transpose() + t.R;
    return t;
}

/// A clean innovation draw: nu ~ N(0, S).
Eigen::VectorXd draw(const Toy& t, std::mt19937& rng) {
    std::normal_distribution<double> nd(0.0, 1.0);
    Eigen::VectorXd z(t.S.rows());
    for (Eigen::Index i = 0; i < z.size(); ++i) z(i) = nd(rng);
    Eigen::LLT<Eigen::MatrixXd> llt(t.S);
    return llt.matrixL() * z;
}

// The statistic is the Gaussian conditional of one row given all the others.
TEST(InnovationScreenStatistic, LeaveOneOutEqualsTheDirectConditional) {
    const Toy t = makeToy(6);
    std::mt19937 rng(11);
    const Eigen::VectorXd nu = draw(t, rng);
    const auto st = InnovationScreen::leaveOneOut(t.S, nu);
    ASSERT_TRUE(st.ok);
    const Eigen::Index m = nu.size();
    for (Eigen::Index i = 0; i < m; ++i) {
        std::vector<Eigen::Index> o;
        for (Eigen::Index j = 0; j < m; ++j) if (j != i) o.push_back(j);
        Eigen::MatrixXd Soo(m - 1, m - 1);
        Eigen::VectorXd Sio(m - 1), noo(m - 1);
        for (Eigen::Index a = 0; a < m - 1; ++a) {
            noo(a) = nu(o[static_cast<size_t>(a)]);
            Sio(a) = t.S(i, o[static_cast<size_t>(a)]);
            for (Eigen::Index b = 0; b < m - 1; ++b)
                Soo(a, b) = t.S(o[static_cast<size_t>(a)], o[static_cast<size_t>(b)]);
        }
        const Eigen::VectorXd k = Soo.ldlt().solve(Sio);
        const double resid = nu(i) - k.dot(noo);
        const double var = t.S(i, i) - k.dot(Sio);
        // The toy covariance has a condition number near 1e10 (a 3e4 m2 clock beside a
        // 9e-6 m2 phase row), so two correct routes agree to about 1e-6, not to rounding.
        EXPECT_NEAR(st.w(i), resid / std::sqrt(var), 1e-5 * (1.0 + std::fabs(st.w(i))));
        EXPECT_NEAR(st.sigma(i), std::sqrt(var), 1e-5 * (1.0 + std::sqrt(var)));
    }
}

// Under the model the statistic is standard normal: unit mean square, no flag at k = 6.
TEST(InnovationScreenStatistic, CleanDrawsAreStandardNormalAndNeverFlag) {
    const Toy t = makeToy(10);
    std::mt19937 rng(7);
    InnovationScreen screen;
    double sumsq = 0.0;
    long n = 0;
    for (int e = 0; e < 400; ++e) {
        const Eigen::VectorXd nu = draw(t, rng);
        const auto st = InnovationScreen::leaveOneOut(t.S, nu);
        ASSERT_TRUE(st.ok);
        sumsq += st.w.squaredNorm();
        n += st.w.size();
        const auto out = screen.evaluate(t.S, nu, t.rows);
        EXPECT_TRUE(out.events.empty());
        EXPECT_FALSE(out.changesUpdate());
    }
    EXPECT_NEAR(sumsq / static_cast<double>(n), 1.0, 0.06);
    const auto& c = screen.counters();
    EXPECT_EQ(c.epochs, 400u);
    EXPECT_EQ(c.flagged, 0u);
    EXPECT_EQ(c.epochs_flagged, 0u);
    uint64_t in_hist = 0;
    for (auto h : c.hist) in_hist += h;
    EXPECT_EQ(in_hist, 400u);
}

// The case the screen exists for: one 92 m phase step in a system whose clock alone puts
// 173 m into every marginal standard deviation.
TEST(InnovationScreenStatistic, A92MetrePhaseStepIsInvisibleMarginallyAndGrossConditionally) {
    const Toy t = makeToy(10);
    std::mt19937 rng(3);
    Eigen::VectorXd nu = draw(t, rng);
    const int bad = t.phaseRow(4);
    nu(bad) += 92.409;

    const double marginal = nu(bad) / std::sqrt(t.S(bad, bad));
    EXPECT_LT(std::fabs(marginal), 1.0) << "the marginal form sees less than one sigma";

    InnovationScreen screen;
    const auto out = screen.evaluate(t.S, nu, t.rows);
    ASSERT_EQ(out.events.size(), 1u);
    EXPECT_EQ(out.events[0].kind, Kind::Exclude);
    EXPECT_EQ(out.events[0].sat, t.rows[static_cast<size_t>(bad)].sat);
    EXPECT_EQ(out.events[0].signal, "L1W");
    EXPECT_GT(std::fabs(out.events[0].w), 100.0);
    EXPECT_NEAR(out.events[0].innovation, nu(bad), 1e-12);
    ASSERT_EQ(out.exclude.size(), 1u);
    EXPECT_EQ(out.exclude[0], "G05_L1W");
    EXPECT_TRUE(out.refloat.empty());
    EXPECT_GE(out.max_abs_w, 100.0);
    EXPECT_EQ(screen.counters().hist[7], 1u) << "the epoch lands in the 100-and-above bucket";
}

// Two gross rows: both leave, in the order of their |w|, and the second one's statistic is
// the one computed with the first already removed (the downdate equals a refactor).
TEST(InnovationScreenSequential, TwoGrossRowsLeaveInOrderWithTheDowndatedStatistic) {
    const Toy t = makeToy(10);
    std::mt19937 rng(5);
    Eigen::VectorXd nu = draw(t, rng);
    const int a = t.phaseRow(2), b = t.phaseRow(7);
    nu(a) += 40.0;
    nu(b) += 25.0;

    InnovationScreen screen;
    const auto out = screen.evaluate(t.S, nu, t.rows);
    ASSERT_EQ(out.events.size(), 2u);
    EXPECT_EQ(out.events[0].sat, "G03");
    EXPECT_EQ(out.events[1].sat, "G08");

    // Direct statistic of row b with row a removed.
    std::vector<Eigen::Index> keep;
    for (Eigen::Index i = 0; i < nu.size(); ++i) if (i != a) keep.push_back(i);
    const Eigen::Index mk = static_cast<Eigen::Index>(keep.size());
    Eigen::MatrixXd Sk(mk, mk);
    Eigen::VectorXd nk(mk);
    Eigen::Index pos_b = -1;
    for (Eigen::Index i = 0; i < mk; ++i) {
        nk(i) = nu(keep[static_cast<size_t>(i)]);
        if (keep[static_cast<size_t>(i)] == b) pos_b = i;
        for (Eigen::Index j = 0; j < mk; ++j)
            Sk(i, j) = t.S(keep[static_cast<size_t>(i)], keep[static_cast<size_t>(j)]);
    }
    const auto direct = InnovationScreen::leaveOneOut(Sk, nk);
    ASSERT_TRUE(direct.ok);
    EXPECT_NEAR(out.events[1].w, direct.w(pos_b), 1e-5 * (1.0 + std::fabs(direct.w(pos_b))));
    EXPECT_NEAR(out.events[1].sigma, direct.sigma(pos_b), 1e-5 * (1.0 + direct.sigma(pos_b)));
    EXPECT_EQ(screen.counters().episodes, 2u);
}

// A row that is not eligible takes no part in the screen: it is never excluded, and a fault
// in it cannot make an eligible row look wrong (it is marginalized out of S, not left in as
// a contaminated predictor).
TEST(InnovationScreenSequential, AnIneligibleGrossRowNeitherLeavesNorBlamesTheOthers) {
    Toy t = makeToy(10);
    std::mt19937 rng(9);
    Eigen::VectorXd nu = draw(t, rng);
    const int bad = t.phaseRow(1);
    nu(bad) += 60.0;
    t.rows[static_cast<size_t>(bad)].eligible = false;
    InnovationScreen screen;
    const auto out = screen.evaluate(t.S, nu, t.rows);
    EXPECT_TRUE(out.events.empty());
    EXPECT_EQ(screen.counters().flagged, 0u);
    EXPECT_EQ(screen.counters().rows, static_cast<uint64_t>(t.rows.size() - 1));

    // The same fault in an eligible row is found, and only that row.
    t.rows[static_cast<size_t>(bad)].eligible = true;
    InnovationScreen screen2;
    const auto out2 = screen2.evaluate(t.S, nu, t.rows);
    ASSERT_EQ(out2.events.size(), 1u);
    EXPECT_EQ(out2.events[0].sat, "G02");
}

// More than the allowed fraction flagged: the prior is wrong, not the rows.
TEST(InnovationScreenAbstain, TooManyFlaggedRowsAbstains) {
    const Toy t = makeToy(10);
    std::mt19937 rng(13);
    Eigen::VectorXd nu = draw(t, rng);
    for (int s = 0; s < 10; ++s) if (s % 2 == 0) nu(t.phaseRow(s)) += 50.0 + 3.0 * s;
    InnovationScreen screen;
    InnovationScreen::Config c = screen.config();
    c.max_fraction = 0.1;   // 20 rows: at most 2
    screen.configure(c);
    const auto out = screen.evaluate(t.S, nu, t.rows);
    EXPECT_EQ(out.abstain, InnovationScreen::Abstain::TooMany);
    EXPECT_FALSE(out.changesUpdate());
    EXPECT_GE(out.flagged_before_abstain, 2);
    EXPECT_EQ(screen.counters().abstain_many, 1u);
    EXPECT_EQ(screen.counters().flagged, 0u) << "an abstained epoch flags nothing";
}

TEST(InnovationScreenAbstain, TooFewRowsAbstains) {
    const Toy t = makeToy(2);   // 4 rows, below the default minimum of 6
    std::mt19937 rng(2);
    Eigen::VectorXd nu = draw(t, rng);
    nu(t.phaseRow(0)) += 80.0;
    InnovationScreen screen;
    const auto out = screen.evaluate(t.S, nu, t.rows);
    EXPECT_EQ(out.abstain, InnovationScreen::Abstain::TooFew);
    EXPECT_FALSE(out.changesUpdate());
    EXPECT_EQ(screen.counters().abstain_few, 1u);
}

TEST(InnovationScreenAbstain, ANonPositiveDefiniteCovarianceAbstains) {
    Toy t = makeToy(6);
    std::mt19937 rng(4);
    const Eigen::VectorXd nu = draw(t, rng);
    t.S(0, 0) = -1.0;
    InnovationScreen screen;
    const auto out = screen.evaluate(t.S, nu, t.rows);
    EXPECT_EQ(out.abstain, InnovationScreen::Abstain::Numerical);
    EXPECT_EQ(screen.counters().abstain_numeric, 1u);
}

TEST(InnovationScreenAbstain, InconsistentSizesAreSkippedNotCrashed) {
    const Toy t = makeToy(6);
    std::mt19937 rng(4);
    const Eigen::VectorXd nu = draw(t, rng);
    InnovationScreen screen;
    std::vector<Row> short_rows(t.rows.begin(), t.rows.end() - 1);
    const auto out = screen.evaluate(t.S, nu, short_rows);
    EXPECT_TRUE(out.events.empty());
    EXPECT_EQ(screen.counters().skipped, 1u);
    EXPECT_EQ(screen.counters().epochs, 0u);
}

// The two edges of an exclusion. A phase row flagged on three consecutive epochs is
// re-floated and returns to the update that epoch; a transient is readmitted.
class InnovationScreenEpisodes : public ::testing::Test {
protected:
    Toy toy = makeToy(10);
    std::mt19937 rng{ 21 };
    InnovationScreen screen;

    InnovationScreen::Outcome step(int bad_phase_sat, double step_m) {
        Eigen::VectorXd nu = draw(toy, rng);
        if (bad_phase_sat >= 0) nu(toy.phaseRow(bad_phase_sat)) += step_m;
        return screen.evaluate(toy.S, nu, toy.rows);
    }
};

TEST_F(InnovationScreenEpisodes, ATransientIsExcludedOnceThenReadmitted) {
    const auto o1 = step(3, 70.0);
    ASSERT_EQ(o1.events.size(), 1u);
    EXPECT_EQ(o1.events[0].kind, Kind::Exclude);
    EXPECT_EQ(o1.events[0].streak, 1);
    EXPECT_EQ(o1.exclude, std::vector<std::string>{ "G04_L1W" });
    EXPECT_EQ(screen.openEpisodes(), 1u);

    const auto o2 = step(-1, 0.0);
    ASSERT_EQ(o2.events.size(), 1u);
    EXPECT_EQ(o2.events[0].kind, Kind::Readmit);
    EXPECT_EQ(o2.events[0].sat, "G04");
    EXPECT_EQ(o2.events[0].streak, 1);
    EXPECT_FALSE(o2.changesUpdate());
    EXPECT_EQ(screen.openEpisodes(), 0u);

    const auto& c = screen.counters();
    EXPECT_EQ(c.episodes, 1u);
    EXPECT_EQ(c.readmitted, 1u);
    EXPECT_EQ(c.open(), 0u);
    EXPECT_TRUE(c.closureHolds());
}

TEST_F(InnovationScreenEpisodes, APersistentPhaseStepIsRefloatedOnTheThirdEpochAndNotExcludedThen) {
    const auto o1 = step(3, 70.0);
    const auto o2 = step(3, 70.0);
    EXPECT_EQ(o1.events[0].streak, 1);
    EXPECT_EQ(o2.events[0].streak, 2);
    EXPECT_EQ(o2.events[0].kind, Kind::Exclude);
    const auto o3 = step(3, 70.0);
    ASSERT_EQ(o3.events.size(), 1u);
    EXPECT_EQ(o3.events[0].kind, Kind::Refloat);
    EXPECT_EQ(o3.events[0].streak, 3);
    ASSERT_EQ(o3.refloat.size(), 1u);
    EXPECT_EQ(o3.refloat[0].first, "G04");
    EXPECT_EQ(o3.refloat[0].second, "L1W");
    EXPECT_TRUE(o3.exclude.empty()) << "the re-floated row returns to the update";
    EXPECT_EQ(screen.openEpisodes(), 0u);

    const auto& c = screen.counters();
    EXPECT_EQ(c.episodes, 1u);
    EXPECT_EQ(c.refloated, 1u);
    EXPECT_EQ(c.flagged, 3u);
    EXPECT_EQ(c.max_streak, 3);
    EXPECT_TRUE(c.closureHolds());
}

TEST_F(InnovationScreenEpisodes, ACodeRowIsNeverRefloatedAndStaysOutWhileItFails) {
    for (int e = 0; e < 6; ++e) {
        Eigen::VectorXd nu = draw(toy, rng);
        nu(toy.codeRow(5)) += 80.0;
        const auto out = screen.evaluate(toy.S, nu, toy.rows);
        ASSERT_EQ(out.events.size(), 1u);
        EXPECT_EQ(out.events[0].kind, Kind::Exclude);
        EXPECT_EQ(out.events[0].signal, "C1W");
        EXPECT_EQ(out.events[0].streak, e + 1);
        EXPECT_TRUE(out.refloat.empty());
    }
    EXPECT_EQ(screen.counters().max_streak, 6);
    EXPECT_EQ(screen.counters().refloated, 0u);
    EXPECT_EQ(screen.counters().open(), 1u);
}

TEST_F(InnovationScreenEpisodes, AnExcludedKeyThatLeavesTheRowSetIsLost) {
    step(3, 70.0);
    // Epoch two: satellite G04 is no longer tracked.
    std::vector<Row> rows;
    std::vector<Eigen::Index> keep;
    for (Eigen::Index i = 0; i < static_cast<Eigen::Index>(toy.rows.size()); ++i)
        if (toy.rows[static_cast<size_t>(i)].sat != "G04") keep.push_back(i);
    const Eigen::Index mk = static_cast<Eigen::Index>(keep.size());
    Eigen::MatrixXd S(mk, mk);
    for (Eigen::Index i = 0; i < mk; ++i) {
        rows.push_back(toy.rows[static_cast<size_t>(keep[static_cast<size_t>(i)])]);
        for (Eigen::Index j = 0; j < mk; ++j)
            S(i, j) = toy.S(keep[static_cast<size_t>(i)], keep[static_cast<size_t>(j)]);
    }
    Eigen::LLT<Eigen::MatrixXd> llt(S);
    std::normal_distribution<double> nd(0.0, 1.0);
    Eigen::VectorXd z(mk);
    for (Eigen::Index i = 0; i < mk; ++i) z(i) = nd(rng);
    const Eigen::VectorXd nu = llt.matrixL() * z;
    const auto out = screen.evaluate(S, nu, rows);
    ASSERT_EQ(out.events.size(), 1u);
    EXPECT_EQ(out.events[0].kind, Kind::Lost);
    EXPECT_EQ(out.events[0].sat, "G04");
    EXPECT_EQ(screen.counters().lost, 1u);
    EXPECT_EQ(screen.openEpisodes(), 0u);
    EXPECT_TRUE(screen.counters().closureHolds());
}

TEST_F(InnovationScreenEpisodes, AnAbstainingEpochReleasesTheOpenExclusions) {
    step(3, 70.0);
    ASSERT_EQ(screen.openEpisodes(), 1u);
    // A mass event: the screen gives up and the rows go to the update.
    Eigen::VectorXd nu = draw(toy, rng);
    for (int s = 0; s < 10; ++s) nu(toy.phaseRow(s)) += 60.0 + 2.0 * s;
    const auto out = screen.evaluate(toy.S, nu, toy.rows);
    EXPECT_EQ(out.abstain, InnovationScreen::Abstain::TooMany);
    bool released = false;
    for (const auto& e : out.events) if (e.kind == Kind::Released) released = true;
    EXPECT_TRUE(released);
    EXPECT_EQ(screen.openEpisodes(), 0u);
    EXPECT_EQ(screen.counters().released, 1u);
    EXPECT_TRUE(screen.counters().closureHolds());
}

TEST_F(InnovationScreenEpisodes, ObserveModeCountsTheSameAndWritesNothing) {
    InnovationScreen act, obs;
    InnovationScreen::Config c = obs.config();
    c.mode = InnovationScreen::Mode::Observe;
    obs.configure(c);
    std::mt19937 r1(40), r2(40);
    for (int e = 0; e < 5; ++e) {
        Eigen::VectorXd n1 = draw(toy, r1), n2 = draw(toy, r2);
        n1(toy.phaseRow(6)) += 55.0;
        n2(toy.phaseRow(6)) += 55.0;
        const auto a = act.evaluate(toy.S, n1, toy.rows);
        const auto b = obs.evaluate(toy.S, n2, toy.rows);
        EXPECT_TRUE(a.acted);
        EXPECT_FALSE(b.acted);
        EXPECT_FALSE(b.changesUpdate());
        EXPECT_EQ(a.events.size(), b.events.size());
    }
    EXPECT_EQ(act.counters().flagged, obs.counters().flagged);
    EXPECT_EQ(act.counters().episodes, obs.counters().episodes);
    EXPECT_EQ(act.counters().refloated, obs.counters().refloated);
}

// Whatever the sequence, the closure identities hold.
TEST_F(InnovationScreenEpisodes, ClosureHoldsOverARandomSequence) {
    std::mt19937 pick(99);
    for (int e = 0; e < 300; ++e) {
        Eigen::VectorXd nu = draw(toy, rng);
        const int r = static_cast<int>(pick() % 10);
        if (r < 3) nu(toy.phaseRow(static_cast<int>(pick() % 10))) += 40.0 + (pick() % 50);
        if (r == 9) for (int s = 0; s < 10; ++s) nu(toy.phaseRow(s)) += 45.0 + 3.0 * s;
        screen.evaluate(toy.S, nu, toy.rows);
    }
    const auto& c = screen.counters();
    EXPECT_TRUE(c.closureHolds());
    EXPECT_EQ(c.episodes, c.readmitted + c.refloated + c.lost + c.released + c.open());
    EXPECT_EQ(c.open(), screen.openEpisodes());
    EXPECT_EQ(c.flagged, c.flagged_phase + c.flagged_code);
}

} // namespace
} // namespace GnssProcessor
