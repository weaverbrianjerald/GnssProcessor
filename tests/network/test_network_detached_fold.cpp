/**
 * @file test_network_detached_fold.cpp
 * @brief Unit gates of the detached fold of fixed integer cycles and the held solve
 *        (step FCF-1): NetworkNormalEquations::foldCycleRowsDetached and
 *        NetworkEstimator::solveHeldDetached, with no production caller.
 *
 * Suite @c NetworkDetachedFold:
 *  - the oracle: on 500 random arrowheads (2 to 12 stations, 20 to 60 shared columns,
 *    random datum pins, rows through anchor-pinned local columns in a third of them, 1 to
 *    3 blocks over disjoint station sets) the held estimate and the posterior cycles
 *    zhat + Pz kappa equal the Card-4 dense path (setDdPriors with a diagonal Pz, solve,
 *    ddPriorPosteriorMean) to 1e-9 relative; a held estimate beyond the bar is arbitrated
 *    by a refined solve of the whole stacked network (it must be within the bar of that
 *    solve; the dense fold's distance to it is printed), at most 5 such systems;
 *  - the estimator untouched: the shared covariance, the reduced normals and right-hand
 *    side and every reconstructStation are bitwise unchanged by the call, and two calls
 *    are bitwise identical;
 *  - the identity kappa = S^-1 v against a dense S = Pz + B + G_f' Sigma_ff G_f built from
 *    jointConditionalCovariance over every folded row (the blocks' float cycles are
 *    correlated through the shared border), with v checked against reconstructStation, the
 *    joint chi2 against v' S^-1 v, and each block's own chi2 against v_b' S_bb^-1 v_b; a
 *    comparison beyond the bar is arbitrated by the stacked saddle-point solve, in which
 *    kappa is a primary unknown, and at least 90 of 100 systems meet the bar outright; the
 *    statistics' means under right targets (chi2_expect, chi2_joint_expect) equal
 *    K - tr(S^-1 Pz) from the dense S to 1e-9 of K;
 *  - diag(A^-1) by selected inversion against the factor's own solves, and X = A^-1 G^T;
 *  - the planted wrong integer, on 12 L1/L2 pairs whose wide-lane and ionosphere-free
 *    directions have sigma 0.06 and 0.04 cycle: with the right targets the statistic
 *    averages its expected K - tr(S^-1 Pz) (the kernel's chi2_expect) within 3 % over
 *    1 000 noise draws at hold sigmas 0.01, 0.003 and 0.0001 cycle, and chi2/K
 *    averages 1 within 3 % at 0.0001 cycle (at 0.01 the hold variance exceeds the float's
 *    along the ionosphere-free direction and the mean is well below 1); a +1 L1 cycle
 *    exceeds chi2_K(1 - 1e-4) in at least 99 % of 1 000 trials at each hold;
 *  - one (1, 1) pair, which keeps the wide lane, at network size (K 330 and 2840): the
 *    unit's own two-degree-of-freedom statistic catches it, the overall test over the
 *    whole component does not at the design's hold (its non-centrality stays the unit's
 *    while the bar grows with K);
 *  - the guards: two blocks sharing a station throw; dependent rows (a repeat, a
 *    contradictory triple c3 = c1 + c2 held at N1 + N2 + 1) make their block degenerate at
 *    a zero hold and at the seam's holds, with no contribution to dN; a float from a
 *    perturbed system gives float_repro_rel above 1e-8 and x_s is that float, bitwise; a
 *    non-finite float is refused; the normwise float reproduction stays at round-off from
 *    a condition number of 1e2 to 1e12; empty input returns x_float exactly; the
 *    preconditions (read from the kernel's folded state) and malformed rows throw;
 *  - the kernel's section timers: armed, a held solve with two folded blocks leaves every
 *    accumulator bitwise as it was (the held solves are the fixed-cycle seam's cost; see the
 *    fixed-cycle fold design, section 5).
 *
 * Suite name owned by this file alone (suite-hygiene rule in CLAUDE.md).
 */

#include <gtest/gtest.h>

#include "network/solve/Estimator.hpp"
#include "network/solve/NormalEquations.hpp"

#include <Eigen/Dense>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <iterator>
#include <limits>
#include <map>
#include <numeric>
#include <random>
#include <string>
#include <utility>
#include <vector>

namespace GnssProcessor {
namespace {

using Row    = NetworkNormalEquations::CycleRow;
using Term   = NetworkNormalEquations::DdPriorBlock::Term;
using Blocks = std::vector<std::vector<Row>>;

constexpr double kF1 = 1575.42e6;                  // GPS L1 (Hz)
constexpr double kF2 = 1227.60e6;                  // GPS L2 (Hz)
constexpr double kC  = 299792458.0;
constexpr double kL1 = kC / kF1;                   // L1 wavelength (m)
constexpr double kL2 = kC / kF2;                   // L2 wavelength (m)

// ============================== the random arrowhead ==============================

/// One station's observation rows.
struct StationData {
    Eigen::MatrixXd A_s, A_r;
    Eigen::VectorXd w, y;
};

/// A local column held by a tight station prior (an anchor pin of the phase datum).
struct LocalPin {
    std::size_t  sta;
    Eigen::Index col;
    double       info;
    double       value;
};

/// A network epoch: the real stations, a shared-block prior entered as a station with no
/// locals (so the reduced system is positive definite), anchor pins and the datum.
struct System {
    Eigen::Index n_shared{0};
    std::vector<StationData> sta;
    StationData prior;
    std::vector<LocalPin> local_pins;
    NetworkEstimator::Datum datum;
    Eigen::VectorXd x_s_true;                  // truth of the shared block
    std::vector<Eigen::VectorXd> x_r_true;     // truth of each station's locals
};

double gauss(std::mt19937_64& rng) {
    std::normal_distribution<double> n01(0.0, 1.0);
    return n01(rng);
}

double uniform(std::mt19937_64& rng, double lo, double hi) {
    std::uniform_real_distribution<double> u(lo, hi);
    return u(rng);
}

int uniformInt(std::mt19937_64& rng, int lo, int hi) {   // inclusive
    std::uniform_int_distribution<int> u(lo, hi);
    return u(rng);
}

Eigen::MatrixXd randn(std::mt19937_64& rng, Eigen::Index r, Eigen::Index c) {
    Eigen::MatrixXd m(r, c);
    for (Eigen::Index j = 0; j < c; ++j)
        for (Eigen::Index i = 0; i < r; ++i) m(i, j) = gauss(rng);
    return m;
}

/// Fresh observations y = A x_true + e, e ~ N(0, W^-1), for every station and the prior;
/// with @p noisy false, y = A x_true exactly (no draw), so the float is the truth.
void drawObservations(std::mt19937_64& rng, System& s, bool noisy = true) {
    for (std::size_t r = 0; r < s.sta.size(); ++r) {
        StationData& st = s.sta[r];
        st.y = st.A_s * s.x_s_true + st.A_r * s.x_r_true[r];
        if (noisy)
            for (Eigen::Index i = 0; i < st.y.size(); ++i) st.y(i) += gauss(rng) / std::sqrt(st.w(i));
    }
    s.prior.y = s.x_s_true;
    if (noisy)
        for (Eigen::Index i = 0; i < s.prior.y.size(); ++i)
            s.prior.y(i) += gauss(rng) / std::sqrt(s.prior.w(i));
}

/// A random arrowhead: each station has 4 to 9 locals (or @p n_local when positive) and
/// three more rows than locals.
System randomSystem(std::mt19937_64& rng, int n_sta, Eigen::Index n_shared, int n_pins,
                    Eigen::Index n_local = 0) {
    System s;
    s.n_shared = n_shared;
    s.x_s_true = randn(rng, n_shared, 1);
    for (int r = 0; r < n_sta; ++r) {
        const Eigen::Index nl = (n_local > 0) ? n_local : uniformInt(rng, 4, 9);
        const Eigen::Index m  = nl + 3;
        StationData st;
        st.A_s = randn(rng, m, n_shared);
        st.A_r = randn(rng, m, nl);
        st.w   = Eigen::VectorXd(m);
        for (Eigen::Index i = 0; i < m; ++i) st.w(i) = uniform(rng, 0.5, 2.0);
        s.sta.push_back(std::move(st));
        s.x_r_true.push_back(randn(rng, nl, 1));
    }
    s.prior.A_s = Eigen::MatrixXd::Identity(n_shared, n_shared);
    s.prior.A_r = Eigen::MatrixXd(n_shared, 0);
    s.prior.w   = Eigen::VectorXd::Constant(n_shared, 0.5);
    std::vector<Eigen::Index> cols(static_cast<std::size_t>(n_shared));
    std::iota(cols.begin(), cols.end(), Eigen::Index{0});
    std::shuffle(cols.begin(), cols.end(), rng);
    for (int p = 0; p < n_pins; ++p) {
        const Eigen::Index c = cols[static_cast<std::size_t>(p)];
        s.datum.push_back(NetworkEstimator::Pin{c, s.x_s_true(c)});   // pinned at the truth
    }
    drawObservations(rng, s);
    return s;
}

/// The estimator of a system, accumulated and not yet solved (station r has index r).
NetworkEstimator buildEstimator(const System& s) {
    NetworkEstimator est;
    for (Eigen::Index j = 0; j < s.n_shared; ++j) est.registerClock("S" + std::to_string(j));
    est.beginEpoch();
    for (std::size_t r = 0; r < s.sta.size(); ++r) {
        const std::size_t id = est.addStation(s.sta[r].A_s, s.sta[r].A_r, s.sta[r].w, s.sta[r].y);
        EXPECT_EQ(id, r);
    }
    est.addStation(s.prior.A_s, s.prior.A_r, s.prior.w, s.prior.y);
    for (const LocalPin& p : s.local_pins) {
        est.addStationPrior(p.sta, {p.col}, Eigen::VectorXd::Constant(1, p.info),
                            Eigen::VectorXd::Constant(1, p.info * p.value));
    }
    return est;
}

/// Drawn rows left out of a block by @ref randomBlocks as dependent (printed by the oracle).
long long g_dependent_rows_left_out = 0;

/// Whether @p row is linearly independent of @p rows (the dense coefficient matrix over
/// their (station, column) pairs gains rank).
bool independentOf(const std::vector<Row>& rows, const Row& row) {
    std::map<std::pair<std::size_t, Eigen::Index>, Eigen::Index> idx;
    auto add = [&idx](const Row& r) {
        for (const Term& t : r.terms) idx.emplace(std::make_pair(t.station, t.local_col), 0);
    };
    for (const Row& r : rows) add(r);
    add(row);
    Eigen::Index n = 0;
    for (auto& [key, i] : idx) i = n++;
    Eigen::MatrixXd C = Eigen::MatrixXd::Zero(static_cast<Eigen::Index>(rows.size()) + 1, n);
    for (std::size_t k = 0; k < rows.size(); ++k)
        for (const Term& t : rows[k].terms)
            C(static_cast<Eigen::Index>(k), idx.at({t.station, t.local_col})) += t.coeff;
    for (const Term& t : row.terms)
        C(static_cast<Eigen::Index>(rows.size()), idx.at({t.station, t.local_col})) += t.coeff;
    return Eigen::FullPivLU<Eigen::MatrixXd>(C).rank() == C.rows();
}

/// Random blocks of cycle rows over disjoint station sets: 1 to 3 blocks of at least two
/// stations, 1 to 10 rows drawn each (DD rows over two stations, two-column rows at one
/// station, single-column rows and six-term cycles over three stations), hold sigmas of
/// 0.005 to 0.05 cycle of L1 or L2.  A drawn row that is linearly dependent on its block's
/// earlier rows is left out (the kernel refuses dependent rows as a placement error; the
/// draws are those of a block that kept it, so every other row is unchanged).
Blocks randomBlocks(std::mt19937_64& rng, const System& s) {
    const int n_sta = static_cast<int>(s.sta.size());
    const int nb = uniformInt(rng, 1, std::min(3, n_sta / 2));
    std::vector<int> perm(static_cast<std::size_t>(n_sta));
    std::iota(perm.begin(), perm.end(), 0);
    std::shuffle(perm.begin(), perm.end(), rng);
    std::vector<std::vector<int>> members(static_cast<std::size_t>(nb));
    for (int b = 0; b < nb; ++b) {
        members[static_cast<std::size_t>(b)].push_back(perm[static_cast<std::size_t>(2 * b)]);
        members[static_cast<std::size_t>(b)].push_back(perm[static_cast<std::size_t>(2 * b + 1)]);
    }
    for (int i = 2 * nb; i < n_sta; ++i) {
        const int b = uniformInt(rng, 0, nb);   // nb: the station is in no block
        if (b < nb) members[static_cast<std::size_t>(b)].push_back(perm[static_cast<std::size_t>(i)]);
    }
    auto nl = [&s](int r) { return static_cast<int>(s.sta[static_cast<std::size_t>(r)].A_r.cols()); };
    Blocks blocks(static_cast<std::size_t>(nb));
    for (int b = 0; b < nb; ++b) {
        const std::vector<int>& mem = members[static_cast<std::size_t>(b)];
        const int K = uniformInt(rng, 1, 10);
        for (int k = 0; k < K; ++k) {
            Row row;
            const int type = uniformInt(rng, 0, 19);
            std::vector<int> pick = mem;
            std::shuffle(pick.begin(), pick.end(), rng);
            const auto st = [](int r) { return static_cast<std::size_t>(r); };
            if (type < 12) {                         // DD over two stations
                const int r1 = pick[0], r2 = pick[1];
                const int nc = std::min(nl(r1), nl(r2));
                const int c1 = uniformInt(rng, 0, nc - 1);
                int c2 = uniformInt(rng, 0, nc - 2);
                if (c2 >= c1) ++c2;
                row.terms = {Term{st(r1), c1, +1.0}, Term{st(r1), c2, -1.0},
                             Term{st(r2), c1, -1.0}, Term{st(r2), c2, +1.0}};
            } else if (type < 17) {                  // two columns at one station
                const int r1 = pick[0];
                const int c1 = uniformInt(rng, 0, nl(r1) - 1);
                int c2 = uniformInt(rng, 0, nl(r1) - 2);
                if (c2 >= c1) ++c2;
                row.terms = {Term{st(r1), c1, +1.0}, Term{st(r1), c2, -1.0}};
            } else if (type < 19 || mem.size() < 3) {   // one column
                const int r1 = pick[0];
                row.terms = {Term{st(r1), uniformInt(rng, 0, nl(r1) - 1), +1.0}};
            } else {                                 // a six-term cycle over three stations
                const int r1 = pick[0], r2 = pick[1], r3 = pick[2];
                const int nc = std::min({nl(r1), nl(r2), nl(r3)});
                row.terms = {Term{st(r1), 0, +1.0}, Term{st(r1), 1, -1.0},
                             Term{st(r2), 1, +1.0}, Term{st(r2), 2, -1.0},
                             Term{st(r3), 2, +1.0}, Term{st(r3), nc - 1, -1.0}};
            }
            const double lam = (uniformInt(rng, 0, 1) == 0) ? kL1 : kL2;
            const double sig = uniform(rng, 0.005, 0.05);
            row.pz_m2  = (sig * lam) * (sig * lam);
            row.zhat_m = 0.3 * gauss(rng);
            std::vector<Row>& kept = blocks[static_cast<std::size_t>(b)];
            if (independentOf(kept, row)) kept.push_back(std::move(row));
            else ++g_dependent_rows_left_out;
        }
    }
    return blocks;
}

/// The same rows as Card-4 dense DD-prior blocks (diagonal Pz).
std::vector<NetworkNormalEquations::DdPriorBlock> asDdPriors(const Blocks& blocks) {
    std::vector<NetworkNormalEquations::DdPriorBlock> out;
    for (const auto& rows : blocks) {
        NetworkNormalEquations::DdPriorBlock d;
        const Eigen::Index K = static_cast<Eigen::Index>(rows.size());
        d.Pz   = Eigen::MatrixXd::Zero(K, K);
        d.zhat = Eigen::VectorXd(K);
        for (Eigen::Index k = 0; k < K; ++k) {
            const Row& r = rows[static_cast<std::size_t>(k)];
            d.rows.push_back(r.terms);
            d.Pz(k, k) = r.pz_m2;
            d.zhat(k)  = r.zhat_m;
        }
        out.push_back(std::move(d));
    }
    return out;
}

double relErr(const Eigen::VectorXd& a, const Eigen::VectorXd& b) {
    const double den = std::max(b.cwiseAbs().maxCoeff(), 1e-300);
    return (a - b).cwiseAbs().maxCoeff() / den;
}

template <typename M>
bool bitwiseEqual(const M& a, const M& b) {
    if (a.rows() != b.rows() || a.cols() != b.cols()) return false;
    if (a.size() == 0) return true;
    return std::memcmp(a.data(), b.data(), sizeof(double) * static_cast<std::size_t>(a.size())) == 0;
}

/// The dense cycle matrix C (K x requests) of one block and the request list.
void cycleMatrix(const std::vector<Row>& rows,
                 std::vector<std::pair<std::size_t, Eigen::Index>>& req, Eigen::MatrixXd& C) {
    std::map<std::pair<std::size_t, Eigen::Index>, Eigen::Index> idx;
    for (const Row& r : rows)
        for (const Term& t : r.terms) idx.emplace(std::make_pair(t.station, t.local_col), 0);
    req.clear();
    for (auto& [key, i] : idx) {
        i = static_cast<Eigen::Index>(req.size());
        req.push_back(key);
    }
    C = Eigen::MatrixXd::Zero(static_cast<Eigen::Index>(rows.size()),
                              static_cast<Eigen::Index>(req.size()));
    for (std::size_t k = 0; k < rows.size(); ++k)
        for (const Term& t : rows[k].terms)
            C(static_cast<Eigen::Index>(k), idx.at({t.station, t.local_col})) += t.coeff;
}

// ============================== chi-square ==============================

/// Regularized lower incomplete gamma P(a, x) (series below a + 1, Lentz continued fraction
/// above).
double gammaP(double a, double x) {
    if (x <= 0.0) return 0.0;
    const double lpre = -x + a * std::log(x) - std::lgamma(a);
    if (x < a + 1.0) {
        double ap = a, del = 1.0 / a, sum = del;
        for (int n = 0; n < 10000; ++n) {
            ap += 1.0;
            del *= x / ap;
            sum += del;
            if (std::abs(del) < std::abs(sum) * 1e-16) break;
        }
        return sum * std::exp(lpre);
    }
    const double tiny = 1e-300;
    double b = x + 1.0 - a, c = 1.0 / tiny, d = 1.0 / b, h = d;
    for (int i = 1; i < 10000; ++i) {
        const double an = -i * (i - a);
        b += 2.0;
        d = an * d + b;
        if (std::abs(d) < tiny) d = tiny;
        c = b + an / c;
        if (std::abs(c) < tiny) c = tiny;
        d = 1.0 / d;
        const double del = d * c;
        h *= del;
        if (std::abs(del - 1.0) < 1e-16) break;
    }
    return 1.0 - std::exp(lpre) * h;
}

/// The chi-square quantile with @p dof degrees of freedom at probability @p p (bisection).
double chi2Quantile(int dof, double p) {
    double lo = 0.0, hi = dof + 50.0 * std::sqrt(2.0 * dof) + 100.0;
    for (int it = 0; it < 200; ++it) {
        const double mid = 0.5 * (lo + hi);
        if (gammaP(0.5 * dof, 0.5 * mid) < p) lo = mid; else hi = mid;
    }
    return 0.5 * (lo + hi);
}

// ============================== the oracle ==============================

/// Every block's rows as one list (the joint rows of the held solve).
std::vector<Row> allRows(const Blocks& blocks) {
    std::vector<Row> rows;
    for (const auto& b : blocks) rows.insert(rows.end(), b.begin(), b.end());
    return rows;
}

/// The held solve of the whole stacked network, the arbiter when the detached solve and a
/// dense reference disagree beyond a bar.  The unknowns are every column (the shared
/// columns, then each station's locals at its offset) and every row's kappa, from the
/// symmetric saddle-point system
///     [ N  C' ] [ x     ]   [ u    ]
///     [ C -Pz ] [ kappa ] = [ zhat ]
/// (x is the held estimate and C x = zhat + Pz kappa; N the float normals), the datum pinned with
/// applyDatumPins, solved by a pivoted LU with five rounds of iterative refinement.  kappa
/// is a primary unknown here, so it carries no cancellation from C x - zhat.
struct Stacked {
    Eigen::VectorXd x;        // every column
    Eigen::VectorXd kappa;    // every row, in the blocks' order
};

Stacked stackedHeld(const System& s, const Blocks& blocks) {
    const Eigen::Index ns = s.n_shared;
    std::vector<Eigen::Index> off(s.sta.size());
    Eigen::Index n = ns;
    for (std::size_t r = 0; r < s.sta.size(); ++r) {
        off[r] = n;
        n += s.sta[r].A_r.cols();
    }
    Eigen::MatrixXd N = Eigen::MatrixXd::Zero(n, n);
    Eigen::VectorXd u = Eigen::VectorXd::Zero(n);
    for (std::size_t r = 0; r < s.sta.size(); ++r) {
        const StationData& st = s.sta[r];
        Eigen::MatrixXd A = Eigen::MatrixXd::Zero(st.A_s.rows(), n);
        A.leftCols(ns) = st.A_s;
        A.middleCols(off[r], st.A_r.cols()) = st.A_r;
        N += A.transpose() * st.w.asDiagonal() * A;
        u += A.transpose() * st.w.asDiagonal() * st.y;
    }
    N.topLeftCorner(ns, ns) += s.prior.A_s.transpose() * s.prior.w.asDiagonal() * s.prior.A_s;
    u.head(ns) += s.prior.A_s.transpose() * s.prior.w.asDiagonal() * s.prior.y;
    for (const LocalPin& p : s.local_pins) {
        N(off[p.sta] + p.col, off[p.sta] + p.col) += p.info;
        u(off[p.sta] + p.col) += p.info * p.value;
    }
    NetworkEstimator::applyDatumPins(N, u, s.datum);   // the rows touch no shared column
    const std::vector<Row> rows = allRows(blocks);
    const Eigen::Index K = static_cast<Eigen::Index>(rows.size());
    Eigen::MatrixXd M = Eigen::MatrixXd::Zero(n + K, n + K);
    Eigen::VectorXd rhs(n + K);
    M.topLeftCorner(n, n) = N;
    rhs.head(n) = u;
    for (Eigen::Index k = 0; k < K; ++k) {
        const Row& r = rows[static_cast<std::size_t>(k)];
        for (const Term& t : r.terms) {
            M(n + k, off[t.station] + t.local_col) += t.coeff;
            M(off[t.station] + t.local_col, n + k) += t.coeff;
        }
        M(n + k, n + k) = -r.pz_m2;
        rhs(n + k) = r.zhat_m;
    }
    const Eigen::PartialPivLU<Eigen::MatrixXd> lu(M);
    Eigen::VectorXd sol = lu.solve(rhs);
    for (int it = 0; it < 5; ++it) sol += lu.solve(rhs - M * sol);
    return Stacked{sol.head(n), sol.tail(K)};
}

TEST(NetworkDetachedFold, OracleEqualsDenseDdPriorFoldOn500Arrowheads) {
    double worst_x = 0.0, worst_post = 0.0, worst_kappa = 0.0;
    int with_anchor = 0, multi_block = 0, arbitrated = 0;
    long long rows_total = 0;
    const long long left_out_before = g_dependent_rows_left_out;
    for (int seed = 0; seed < 500; ++seed) {
        std::mt19937_64 rng(0xFCF1000ULL + static_cast<unsigned long long>(seed));
        const int n_sta = uniformInt(rng, 2, 12);
        const Eigen::Index n_shared = uniformInt(rng, 20, 60);
        System s = randomSystem(rng, n_sta, n_shared, uniformInt(rng, 1, 4));
        const Blocks blocks = randomBlocks(rng, s);
        if (uniformInt(rng, 0, 2) == 0) {   // a row through an anchor-pinned local column
            const Term& t = blocks[0][0].terms[0];
            s.local_pins.push_back(LocalPin{t.station, t.local_col, 1e8, 0.0});
            ++with_anchor;
        }
        if (blocks.size() > 1) ++multi_block;
        for (const auto& b : blocks) rows_total += static_cast<long long>(b.size());

        NetworkEstimator est = buildEstimator(s);
        NetworkEstimator dense = est;   // the same accumulated epoch, solved with the dense fold
        const Eigen::VectorXd x_float = est.solve(s.datum);
        dense.setDdPriors(asDdPriors(blocks));
        const Eigen::VectorXd x_dense = dense.solve(s.datum);

        const NetworkEstimator::HeldSolve h = est.solveHeldDetached(s.datum, x_float, blocks);
        ASSERT_TRUE(h.ok) << "seed " << seed << " float_repro_rel " << h.float_repro_rel;
        ASSERT_EQ(h.kappa.size(), blocks.size());
        const double ex = relErr(h.x_s, x_dense);
        worst_x = std::max(worst_x, ex);
        if (ex > 1e-9) {
            // Beyond the bar: arbitrate with the stacked third path.  The detached solve
            // passes only when it is within the bar of the stacked solve (the printed dense
            // distance then names the oracle as the path that carries the rounding).
            const Eigen::VectorXd x_ref = stackedHeld(s, blocks).x.head(n_shared);
            const double e_det = relErr(h.x_s, x_ref), e_dense = relErr(x_dense, x_ref);
            ++arbitrated;
            std::cout << "[NetworkDetachedFold] oracle seed " << seed << ": held x_s against the"
                      << " dense fold " << ex << "; against a refined stacked solve: detached "
                      << e_det << ", dense " << e_dense << "\n";
            EXPECT_LE(e_det, 1e-9) << "seed " << seed << ": held x_s against the stacked solve";
        }
        for (std::size_t b = 0; b < blocks.size(); ++b) {
            ASSERT_EQ(h.degenerate[b], 0) << "seed " << seed << " block " << b;
            ASSERT_EQ(h.dependent[b], 0) << "seed " << seed << " block " << b;
            const Eigen::Index K = static_cast<Eigen::Index>(blocks[b].size());
            Eigen::VectorXd zhat(K), pz(K);
            for (Eigen::Index k = 0; k < K; ++k) {
                zhat(k) = blocks[b][static_cast<std::size_t>(k)].zhat_m;
                pz(k)   = blocks[b][static_cast<std::size_t>(k)].pz_m2;
            }
            const Eigen::VectorXd post_dense = dense.ddPriorPosteriorMean(b, x_dense);
            const Eigen::VectorXd post_det   = zhat + pz.cwiseProduct(h.kappa[b]);
            const Eigen::VectorXd kappa_dense = (post_dense - zhat).cwiseQuotient(pz);
            const double ep = relErr(post_det, post_dense);
            const double ek = relErr(h.kappa[b], kappa_dense);
            worst_post  = std::max(worst_post, ep);
            worst_kappa = std::max(worst_kappa, ek);
            EXPECT_LE(ep, 1e-9) << "seed " << seed << " block " << b << ": posterior cycles";
            EXPECT_LE(ek, 1e-9) << "seed " << seed << " block " << b << ": kappa";
        }
        for (const NetworkEstimator::Pin& p : s.datum) {   // the pins hold on the held solve
            EXPECT_NEAR(h.x_s(p.column), p.value, 1e-12 * std::max(1.0, std::abs(p.value)));
        }
    }
    std::cout << "[NetworkDetachedFold] oracle: 500 systems, " << rows_total << " rows ("
              << (g_dependent_rows_left_out - left_out_before) << " drawn rows left out as"
              << " dependent), " << multi_block << " with several blocks, " << with_anchor
              << " with a row through an anchor-pinned column; worst relative error x_s "
              << worst_x << " (" << arbitrated << " beyond 1e-9, arbitrated), posterior cycles "
              << worst_post << ", kappa " << worst_kappa << "\n";
    EXPECT_LE(arbitrated, 5);
    EXPECT_GT(multi_block, 50);
    EXPECT_GT(with_anchor, 50);
}

// ============================== the estimator untouched ==============================

TEST(NetworkDetachedFold, EstimatorUntouchedAndCallsRepeatBitwise) {
    for (int seed = 0; seed < 20; ++seed) {
        std::mt19937_64 rng(0xFCF1200ULL + static_cast<unsigned long long>(seed));
        System s = randomSystem(rng, uniformInt(rng, 2, 12), uniformInt(rng, 20, 60), 2);
        const Blocks blocks = randomBlocks(rng, s);
        NetworkEstimator est = buildEstimator(s);
        const Eigen::VectorXd x_float = est.solve(s.datum);

        const Eigen::MatrixXd cov0 = est.sharedCovariance();
        const Eigen::MatrixXd N0   = est.normals().reducedSharedNormals();
        const Eigen::VectorXd u0   = est.normals().reducedSharedRhs();
        std::vector<Eigen::VectorXd> xr0;
        for (std::size_t r = 0; r < s.sta.size(); ++r) xr0.push_back(est.reconstructStation(r, x_float));

        const NetworkEstimator::HeldSolve h1 = est.solveHeldDetached(s.datum, x_float, blocks);
        ASSERT_TRUE(h1.ok);
        EXPECT_GT(h1.max_shift_m, 0.0);

        EXPECT_TRUE(bitwiseEqual(est.sharedCovariance(), cov0)) << "seed " << seed;
        EXPECT_TRUE(bitwiseEqual(est.normals().reducedSharedNormals(), N0)) << "seed " << seed;
        EXPECT_TRUE(bitwiseEqual(est.normals().reducedSharedRhs(), u0)) << "seed " << seed;
        EXPECT_TRUE(bitwiseEqual(est.reducedSharedNormals(), N0)) << "seed " << seed;
        for (std::size_t r = 0; r < s.sta.size(); ++r)
            EXPECT_TRUE(bitwiseEqual(est.reconstructStation(r, x_float), xr0[r]))
                << "seed " << seed << " station " << r;

        const NetworkEstimator::HeldSolve h2 = est.solveHeldDetached(s.datum, x_float, blocks);
        EXPECT_TRUE(bitwiseEqual(h1.x_s, h2.x_s));
        EXPECT_EQ(std::memcmp(&h1.float_repro_rel, &h2.float_repro_rel, sizeof(double)), 0);
        EXPECT_EQ(std::memcmp(&h1.max_shift_m, &h2.max_shift_m, sizeof(double)), 0);
        EXPECT_EQ(h1.factor_nnz, h2.factor_nnz);
        for (std::size_t b = 0; b < blocks.size(); ++b) {
            EXPECT_TRUE(bitwiseEqual(h1.v[b], h2.v[b]));
            EXPECT_TRUE(bitwiseEqual(h1.kappa[b], h2.kappa[b]));
            EXPECT_EQ(std::memcmp(&h1.chi2[b], &h2.chi2[b], sizeof(double)), 0);
        }
    }
}

// ============================== kappa = S^-1 v ==============================

/// The dense float covariance of a row set plus its hold variances,
/// S = C J C' + Pz with J from jointConditionalCovariance (datum columns exact), and the
/// float cycle values minus their targets from reconstructStation (an independent path).
void denseRowStatistics(const NetworkEstimator& est, const NetworkEstimator::Datum& datum,
                        const std::vector<Eigen::VectorXd>& xr, const std::vector<Row>& rows,
                        Eigen::MatrixXd& S, Eigen::VectorXd& v) {
    std::vector<std::pair<std::size_t, Eigen::Index>> req;
    Eigen::MatrixXd C;
    cycleMatrix(rows, req, C);
    Eigen::VectorXd xl(static_cast<Eigen::Index>(req.size()));
    for (std::size_t i = 0; i < req.size(); ++i)
        xl(static_cast<Eigen::Index>(i)) = xr[req[i].first](req[i].second);
    const Eigen::Index K = C.rows();
    Eigen::VectorXd zhat(K), pz(K);
    for (Eigen::Index k = 0; k < K; ++k) {
        zhat(k) = rows[static_cast<std::size_t>(k)].zhat_m;
        pz(k)   = rows[static_cast<std::size_t>(k)].pz_m2;
    }
    v = C * xl - zhat;
    S = C * est.jointConditionalCovariance(req, datum) * C.transpose();
    S.diagonal() += pz;
}

/// The mean of v' S^-1 v under right targets, K - tr(S^-1 Pz), from a dense S.
double denseNullMean(const Eigen::MatrixXd& S, const std::vector<Row>& rows) {
    Eigen::VectorXd pz(S.rows());
    for (Eigen::Index k = 0; k < pz.size(); ++k) pz(k) = rows[static_cast<std::size_t>(k)].pz_m2;
    const Eigen::MatrixXd SinvPz = S.ldlt().solve(Eigen::MatrixXd(pz.asDiagonal()));
    return static_cast<double>(S.rows()) - SinvPz.trace();
}

TEST(NetworkDetachedFold, KappaIsSInverseVAgainstDenseCovariance) {
    double worst_v = 0.0, worst_k = 0.0, worst_j = 0.0, worst_c = 0.0, worst_arb = 0.0;
    double worst_e0 = 0.0;
    int multi = 0, at_bar = 0, arbitrated = 0;
    for (int seed = 0; seed < 100; ++seed) {
        std::mt19937_64 rng(0xFCF1300ULL + static_cast<unsigned long long>(seed));
        System s = randomSystem(rng, uniformInt(rng, 2, 12), uniformInt(rng, 20, 60),
                                uniformInt(rng, 1, 4));
        const Blocks blocks = randomBlocks(rng, s);
        if (blocks.size() > 1) ++multi;
        NetworkEstimator est = buildEstimator(s);
        const Eigen::VectorXd x_float = est.solve(s.datum);
        const NetworkEstimator::HeldSolve h = est.solveHeldDetached(s.datum, x_float, blocks);
        ASSERT_TRUE(h.ok);
        std::vector<Eigen::VectorXd> xr;
        for (std::size_t r = 0; r < s.sta.size(); ++r) xr.push_back(est.reconstructStation(r, x_float));

        // Joint: kappa over every row equals S^-1 v with the joint S (the blocks' float
        // cycles are correlated through the shared border), and v' kappa is chi2_joint.
        Eigen::MatrixXd S;
        Eigen::VectorXd v_ref;
        denseRowStatistics(est, s.datum, xr, allRows(blocks), S, v_ref);
        Eigen::VectorXd v_all(v_ref.size()), k_all(v_ref.size());
        Eigen::Index at = 0;
        for (std::size_t b = 0; b < blocks.size(); ++b) {
            v_all.segment(at, h.v[b].size()) = h.v[b];
            k_all.segment(at, h.kappa[b].size()) = h.kappa[b];
            at += h.v[b].size();
        }
        const Eigen::VectorXd k_ref = S.ldlt().solve(v_ref);
        const double chi_ref = v_ref.dot(k_ref);
        // The joint statistic's mean under right targets, K - tr(S^-1 Pz), against the
        // dense S (absolute error relative to K).
        const double e0j = std::abs(h.chi2_joint_expect - denseNullMean(S, allRows(blocks)))
                         / static_cast<double>(v_ref.size());
        worst_e0 = std::max(worst_e0, e0j);
        EXPECT_LE(e0j, 1e-9) << "seed " << seed << ": chi2_joint_expect";
        const double ev = relErr(v_all, v_ref);
        const double ek = relErr(k_all, k_ref);
        const double ej = std::abs(h.chi2_joint - chi_ref) / chi_ref;
        worst_v = std::max(worst_v, ev);
        worst_k = std::max(worst_k, ek);
        worst_j = std::max(worst_j, ej);
        EXPECT_LE(ev, 1e-9) << "seed " << seed << ": v";
        EXPECT_EQ(h.dof_joint, v_ref.size());
        if (ek <= 1e-9 && ej <= 1e-9) {
            ++at_bar;
        } else {
            // Beyond the bar against the dense S: arbitrate with the stacked saddle-point
            // solve, an independent path in which kappa is a primary unknown.
            const Stacked st = stackedHeld(s, blocks);
            const double chi_st = v_all.dot(st.kappa);
            const double ek_st = relErr(k_all, st.kappa), ek_ref = relErr(k_ref, st.kappa);
            const double ej_st = std::abs(h.chi2_joint - chi_st) / chi_st;
            const double ej_ref = std::abs(chi_ref - chi_st) / chi_st;
            ++arbitrated;
            worst_arb = std::max({worst_arb, ek_st, ej_st});
            std::cout << "[NetworkDetachedFold] identity seed " << seed << ": against the dense S"
                      << " kappa " << ek << ", chi2_joint " << ej << "; against the stacked solve:"
                      << " detached kappa " << ek_st << ", chi2_joint " << ej_st << "; dense S kappa "
                      << ek_ref << ", chi2_joint " << ej_ref << "\n";
            EXPECT_LE(ek_st, 1e-9) << "seed " << seed << ": kappa against the stacked solve";
            EXPECT_LE(ej_st, 1e-9) << "seed " << seed << ": chi2_joint against the stacked solve";
        }

        // Each block's own statistic: v_b' S_bb^-1 v_b with the block's own S.
        for (std::size_t b = 0; b < blocks.size(); ++b) {
            Eigen::MatrixXd Sb;
            Eigen::VectorXd vb;
            denseRowStatistics(est, s.datum, xr, blocks[b], Sb, vb);
            const double e0b = std::abs(h.chi2_expect[b] - denseNullMean(Sb, blocks[b]))
                             / static_cast<double>(vb.size());
            worst_e0 = std::max(worst_e0, e0b);
            EXPECT_LE(e0b, 1e-9) << "seed " << seed << " block " << b << ": chi2_expect";
            const double cb = vb.dot(Sb.ldlt().solve(vb));
            const double ec = std::abs(h.chi2[b] - cb) / cb;
            worst_c = std::max(worst_c, ec);
            if (ec <= 1e-9) continue;
            const Stacked st = stackedHeld(s, {blocks[b]});   // the block folded alone
            const double c_st = h.v[b].dot(st.kappa);
            const double ec_st = std::abs(h.chi2[b] - c_st) / c_st;
            ++arbitrated;
            worst_arb = std::max(worst_arb, ec_st);
            std::cout << "[NetworkDetachedFold] identity seed " << seed << " block " << b
                      << ": own chi2 against the dense S_bb " << ec << ", against the stacked"
                      << " solve: detached " << ec_st << ", dense S_bb "
                      << std::abs(cb - c_st) / c_st << "\n";
            EXPECT_LE(ec_st, 1e-9) << "seed " << seed << " block " << b << ": own chi2";
        }
    }
    std::cout << "[NetworkDetachedFold] identity: 100 systems (" << multi
              << " with several blocks), " << at_bar << " at the bar against the dense S, "
              << arbitrated << " comparisons arbitrated (worst against the stacked solve "
              << worst_arb << "); worst relative error against the dense S: v " << worst_v
              << ", kappa " << worst_k << ", chi2_joint " << worst_j << ", per-block chi2 "
              << worst_c << "; the statistics' means under right targets (joint and per"
              << " block) " << worst_e0 << " of K\n";
    EXPECT_GT(multi, 20);
    EXPECT_GE(at_bar, 90);
}

// ============================== diag(A^-1) by selected inversion ==============================

TEST(NetworkDetachedFold, SelectedInverseDiagonalMatchesSparseSolves) {
    // diag(A^-1) from Takahashi's recurrence on the factor's pattern against e_k' A^-1 e_k
    // from the factor's own solves, on every folded block of 50 random systems.
    double worst = 0.0;
    long long rows = 0;
    for (int seed = 0; seed < 50; ++seed) {
        std::mt19937_64 rng(0xFCF1600ULL + static_cast<unsigned long long>(seed));
        System s = randomSystem(rng, uniformInt(rng, 2, 12), uniformInt(rng, 20, 60), 2);
        const Blocks blocks = randomBlocks(rng, s);
        NetworkEstimator est = buildEstimator(s);
        est.solve(s.datum);
        const NetworkNormalEquations::DetachedFold fold = est.normals().foldCycleRowsDetached(blocks);
        for (const NetworkNormalEquations::DetachedFoldBlock& fb : fold.blocks) {
            ASSERT_TRUE(fb.factor != nullptr) << "seed " << seed;
            const Eigen::Index K = fb.zhat.size();
            ASSERT_EQ(fb.ainv_diag.size(), K);
            ASSERT_EQ(fb.X.rows(), K);
            ASSERT_EQ(fb.X.cols(), s.n_shared);
            for (Eigen::Index k = 0; k < K; ++k) {
                const Eigen::VectorXd col =
                    NetworkNormalEquations::solveFoldBlock(fb, Eigen::VectorXd::Unit(K, k));
                const double e = std::abs(fb.ainv_diag(k) - col(k)) / std::abs(col(k));
                worst = std::max(worst, e);
                EXPECT_GT(fb.ainv_diag(k), 0.0);
                EXPECT_LE(e, 1e-10) << "seed " << seed << " row " << k;
            }
            rows += K;
            // X is A^-1 G^T, column by column.
            for (Eigen::Index j = 0; j < s.n_shared; j += 7) {
                const Eigen::VectorXd xj =
                    NetworkNormalEquations::solveFoldBlock(fb, fb.G.row(j).transpose());
                if (xj.cwiseAbs().maxCoeff() > 0.0)
                    EXPECT_LE(relErr(fb.X.col(j), xj), 1e-10) << "seed " << seed << " column " << j;
            }
        }
    }
    std::cout << "[NetworkDetachedFold] selected inversion: " << rows
              << " diagonal entries of A^-1 on 50 systems, worst relative error against the"
              << " factor's solves " << worst << "\n";
}

// ============================== the planted wrong integer ==============================

/// A network whose fold units are L1/L2 pairs with a prescribed float covariance: each
/// pair is two single-column rows on two local columns of one station, re-parameterized
/// so that the float covariance of the pair (shared propagation included) is the target.
struct PairBed {
    System s;
    std::vector<std::pair<std::size_t, Eigen::Index>> pair_cols;   // (station, L1 column); L2 = +1
    std::vector<std::pair<long long, long long>> ints;              // (N1, N2) per pair
};

/// The pair covariance in cycles with sigma_wl on N1 - N2 and sigma_if on the
/// ionosphere-free combination in narrow-lane cycles (f1 N1 - f2 N2) / (f1 - f2),
/// uncorrelated.
Eigen::Matrix2d pairCovarianceCycles(double sigma_wl, double sigma_if) {
    Eigen::Matrix2d M;
    M << 1.0, -1.0, kF1 / (kF1 - kF2), -kF2 / (kF1 - kF2);
    const Eigen::Matrix2d Minv = M.inverse();
    const Eigen::Matrix2d D = Eigen::Vector2d(sigma_wl * sigma_wl, sigma_if * sigma_if).asDiagonal();
    return Minv * D * Minv.transpose();
}

PairBed makePairBed(std::mt19937_64& rng, int n_sta, int pairs_per_sta, Eigen::Index n_shared,
                    const Eigen::Matrix2d& target_cyc, Eigen::Index n_local = 0) {
    PairBed bed;
    bed.s = randomSystem(rng, n_sta, n_shared, 2, n_local);
    // The pairs: columns (2p, 2p+1) of each station.
    for (int r = 0; r < n_sta; ++r)
        for (int p = 0; p < pairs_per_sta; ++p)
            bed.pair_cols.emplace_back(static_cast<std::size_t>(r), static_cast<Eigen::Index>(2 * p));
    // Measure each pair's float covariance and map it onto the target with a 2x2 change
    // of the station's local parameters (the column space, so the shared block, is kept).
    const Eigen::Matrix2d lam = Eigen::Vector2d(kL1, kL2).asDiagonal();
    const Eigen::Matrix2d target_m = lam * target_cyc * lam;
    const Eigen::Matrix2d Q = target_m.llt().matrixL();
    {
        NetworkEstimator est = buildEstimator(bed.s);
        est.solve(bed.s.datum);
        for (const auto& [r, c] : bed.pair_cols) {
            const Eigen::MatrixXd J = est.jointConditionalCovariance({{r, c}, {r, c + 1}}, bed.s.datum);
            const Eigen::Matrix2d R = Eigen::Matrix2d(J).llt().matrixL();
            const Eigen::Matrix2d T = R * Q.inverse();              // a = T a'
            StationData& st = bed.s.sta[r];
            const Eigen::MatrixXd cols = st.A_r.middleCols(c, 2) * T;
            st.A_r.middleCols(c, 2) = cols;
        }
    }
    // The truth: integer pairs on the re-parameterized columns, in metres.
    for (const auto& [r, c] : bed.pair_cols) {
        const long long n1 = uniformInt(rng, -20, 20), n2 = uniformInt(rng, -20, 20);
        bed.ints.emplace_back(n1, n2);
        bed.s.x_r_true[r](c)     = kL1 * static_cast<double>(n1);
        bed.s.x_r_true[r](c + 1) = kL2 * static_cast<double>(n2);
    }
    return bed;
}

/// The bed's pairs as one block (the stations hold no other block), targets at the bed's
/// integers (the first pair's L1 shifted by @p planted_l1_delta), hold sigma
/// @p sigma_cyc cycle of each band.
std::vector<Row> pairRows(const PairBed& bed, long long planted_l1_delta, double sigma_cyc) {
    std::vector<Row> rows;
    for (std::size_t p = 0; p < bed.pair_cols.size(); ++p) {
        const auto [r, c] = bed.pair_cols[p];
        const long long n1 = bed.ints[p].first + (p == 0 ? planted_l1_delta : 0);
        const long long n2 = bed.ints[p].second;
        Row l1, l2;
        l1.terms  = {Term{r, c, +1.0}};
        l1.zhat_m = kL1 * static_cast<double>(n1);
        l1.pz_m2  = (sigma_cyc * kL1) * (sigma_cyc * kL1);
        l2.terms  = {Term{r, c + 1, +1.0}};
        l2.zhat_m = kL2 * static_cast<double>(n2);
        l2.pz_m2  = (sigma_cyc * kL2) * (sigma_cyc * kL2);
        rows.push_back(l1);
        rows.push_back(l2);
    }
    return rows;
}

TEST(NetworkDetachedFold, PlantedWrongIntegerCalibrationAndPower) {
    std::mt19937_64 rng(0xFCF1400ULL);
    const Eigen::Matrix2d target = pairCovarianceCycles(0.06, 0.04);
    PairBed bed = makePairBed(rng, 6, 2, 20, target);   // 12 pairs, K = 24

    // The hold sigmas: the design's 0.01 cycle, the 0.003 cycle of the FCF-2 arms (fixed-cycle fold design,
    // section 3), and 0.0001 cycle, a hold small against the float in every
    // direction (0.001 cycle, measured in section 2 of that design, lies between).
    constexpr int kHolds = 3;
    const double sigmas[kHolds] = {0.01, 0.003, 0.0001};

    // The re-parameterization gives the prescribed float pair covariance, and the dense S
    // gives each hold's expected statistic under right targets, K - tr(S^-1 Pz).
    double expect_per_dof[kHolds] = {0.0, 0.0, 0.0};
    {
        NetworkEstimator est = buildEstimator(bed.s);
        const Eigen::VectorXd x_float = est.solve(bed.s.datum);
        const Eigen::Matrix2d lam = Eigen::Vector2d(kL1, kL2).asDiagonal();
        for (const auto& [r, c] : bed.pair_cols) {
            const Eigen::Matrix2d J =
                est.jointConditionalCovariance({{r, c}, {r, c + 1}}, bed.s.datum);
            const Eigen::Matrix2d Jc = lam.inverse() * J * lam.inverse();   // cycles^2
            EXPECT_NEAR(std::sqrt(Jc(0, 0) + Jc(1, 1) - 2.0 * Jc(0, 1)), 0.06, 1e-9);
            const Eigen::Vector2d q(kF1 / (kF1 - kF2), -kF2 / (kF1 - kF2));
            EXPECT_NEAR(std::sqrt(q.dot(Jc * q)), 0.04, 1e-9);
        }
        std::vector<Eigen::VectorXd> xr;
        for (std::size_t r = 0; r < bed.s.sta.size(); ++r) xr.push_back(est.reconstructStation(r, x_float));
        for (int i = 0; i < kHolds; ++i) {
            const std::vector<Row> rows = pairRows(bed, 0, sigmas[i]);
            Eigen::MatrixXd S;
            Eigen::VectorXd v;
            denseRowStatistics(est, bed.s.datum, xr, rows, S, v);
            Eigen::VectorXd pz(S.rows());
            for (Eigen::Index k = 0; k < pz.size(); ++k) pz(k) = rows[static_cast<std::size_t>(k)].pz_m2;
            const Eigen::MatrixXd SinvPz = S.ldlt().solve(Eigen::MatrixXd(pz.asDiagonal()));
            expect_per_dof[i] = 1.0 - SinvPz.trace() / static_cast<double>(S.rows());
        }
    }

    const int K = static_cast<int>(pairRows(bed, 0, sigmas[0]).size());
    const double bar = chi2Quantile(K, 1.0 - 1e-4);
    const int trials = 1000;
    double sum_h0[kHolds] = {0.0, 0.0, 0.0};
    double worst_expect[kHolds] = {0.0, 0.0, 0.0};   // the kernel's mean against the dense one
    int exceed_h0[kHolds] = {0, 0, 0}, exceed_h1[kHolds] = {0, 0, 0};
    double min_h1[kHolds];
    std::fill(std::begin(min_h1), std::end(min_h1), std::numeric_limits<double>::infinity());
    for (int t = 0; t < trials; ++t) {
        drawObservations(rng, bed.s);
        NetworkEstimator est = buildEstimator(bed.s);
        const Eigen::VectorXd x_float = est.solve(bed.s.datum);
        for (int i = 0; i < kHolds; ++i) {
            const NetworkEstimator::HeldSolve h0 =
                est.solveHeldDetached(bed.s.datum, x_float, {pairRows(bed, 0, sigmas[i])});
            const NetworkEstimator::HeldSolve h1 =
                est.solveHeldDetached(bed.s.datum, x_float, {pairRows(bed, +1, sigmas[i])});
            ASSERT_TRUE(h0.ok);
            ASSERT_TRUE(h1.ok);
            // The kernel states the statistic's mean under right targets every epoch.
            worst_expect[i] = std::max(worst_expect[i],
                                       std::abs(h0.chi2_expect[0] / K - expect_per_dof[i]));
            EXPECT_EQ(h0.chi2_expect[0], h1.chi2_expect[0]);   // it does not read the targets
            sum_h0[i] += h0.chi2[0] / K;
            if (h0.chi2[0] > bar) ++exceed_h0[i];
            if (h1.chi2[0] > bar) ++exceed_h1[i];
            min_h1[i] = std::min(min_h1[i], h1.chi2[0]);
        }
    }
    for (int i = 0; i < kHolds; ++i) {
        const double mean_h0 = sum_h0[i] / trials;
        const double power = static_cast<double>(exceed_h1[i]) / trials;
        std::cout << "[NetworkDetachedFold] planted, hold sigma " << sigmas[i] << " cycle: K " << K
                  << ", bar chi2_K(1-1e-4) " << bar << "; right targets: chi2/K mean " << mean_h0
                  << " over " << trials << " draws (expected " << expect_per_dof[i]
                  << ", the kernel's chi2_expect/K within " << worst_expect[i] << " of it), "
                  << exceed_h0[i] << " above the bar; +1 L1 cycle: " << exceed_h1[i] << " of "
                  << trials << " above the bar (smallest chi2 " << min_h1[i] << ")\n";
        // The statistic's mean under right targets is K - tr(S^-1 Pz), which the kernel
        // states (chi2_expect) to the dense value; the Monte Carlo mean matches it at
        // every hold.
        EXPECT_LE(worst_expect[i], 1e-9) << "sigma " << sigmas[i];
        EXPECT_NEAR(mean_h0, expect_per_dof[i], 0.03 * expect_per_dof[i]) << "sigma " << sigmas[i];
        EXPECT_GE(power, 0.99) << "sigma " << sigmas[i];
    }
    // chi2/K within 3 % of 1 holds for a hold small against the float in every direction
    // (0.0001 cycle).  At the design's 0.01 cycle it does not: on this pair the hold
    // variance exceeds the float's along the ionosphere-free direction, and the mean drops
    // to the expected value printed above; a calibration is graded
    // against chi2_expect, not against K.
    EXPECT_NEAR(sum_h0[kHolds - 1] / trials, 1.0, 0.03);
}

// ============================== one (1, 1) pair at network size ==============================

/// A pair bed whose pairs share nothing with any other row: each pair's two local columns
/// are observed only by two rows of their own (no shared column, no other local), weighted
/// so that the pair's float covariance is the target exactly.  G is zero on the pairs'
/// rows and B is block diagonal, so the pairs' float errors are independent: the
/// independent-pairs model of the FCF-1 review, which a DD whose satellite terms cancel
/// approaches.  The stations keep two more locals and their rows on the border.
PairBed makeIndependentPairBed(std::mt19937_64& rng, int n_sta, int pairs_per_sta,
                               Eigen::Index n_shared, const Eigen::Matrix2d& target_cyc) {
    PairBed bed;
    const Eigen::Index n_pc = 2 * pairs_per_sta;
    bed.s = randomSystem(rng, n_sta, n_shared, 2, n_pc + 2);
    const Eigen::Matrix2d lam = Eigen::Vector2d(kL1, kL2).asDiagonal();
    const Eigen::Matrix2d L = (lam * target_cyc * lam).llt().matrixL();
    const Eigen::Matrix2d Ap = L.inverse();   // (Ap' Ap)^-1 = L L' = the target (m^2)
    for (int r = 0; r < n_sta; ++r) {
        StationData& st = bed.s.sta[static_cast<std::size_t>(r)];
        st.A_r.leftCols(n_pc).setZero();      // the station's rows see the last two locals only
        const Eigen::Index m0 = st.A_s.rows(), m = m0 + n_pc;
        StationData out;
        out.A_s = Eigen::MatrixXd::Zero(m, n_shared);
        out.A_r = Eigen::MatrixXd::Zero(m, st.A_r.cols());
        out.w   = Eigen::VectorXd::Ones(m);
        out.A_s.topRows(m0) = st.A_s;
        out.A_r.topRows(m0) = st.A_r;
        out.w.head(m0)      = st.w;
        for (int p = 0; p < pairs_per_sta; ++p) {
            out.A_r.block(m0 + 2 * p, 2 * p, 2, 2) = Ap;
            bed.pair_cols.emplace_back(static_cast<std::size_t>(r), static_cast<Eigen::Index>(2 * p));
        }
        st = std::move(out);
    }
    for (const auto& [r, c] : bed.pair_cols) {
        const long long n1 = uniformInt(rng, -20, 20), n2 = uniformInt(rng, -20, 20);
        bed.ints.emplace_back(n1, n2);
        bed.s.x_r_true[r](c)     = kL1 * static_cast<double>(n1);
        bed.s.x_r_true[r](c + 1) = kL2 * static_cast<double>(n2);
    }
    return bed;
}

/// The pair bed's rows with a (1, 1) error on the first pair.
std::vector<Row> oneOneRows(const PairBed& bed, double sigma_cyc) {
    std::vector<Row> rows = pairRows(bed, +1, sigma_cyc);
    rows[1].zhat_m += kL2;   // the first pair's L2: N2 + 1
    return rows;
}

/// The noise-free statistics of one (1, 1) error on the first pair of a bed, for the whole
/// bed folded as one block (the overall test over one station component) and for the pair
/// folded alone (the unit's own two-degree-of-freedom statistic).  With noise-free data the
/// float is the truth, so each statistic is the error's non-centrality; the kernel states
/// each statistic's mean under right targets beside it.
struct NoiseFree {
    int    K{0};
    double nc_block{0.0}, expect_block{0.0}, bar_block{0.0};   // overall: nc, H0 mean, chi2_K(1-1e-4)
    double nc_unit{0.0}, expect_unit{0.0};                     // the pair alone
};

NoiseFree noiseFreeOneOne(PairBed bed, double sigma_cyc) {
    std::mt19937_64 unused(0);
    drawObservations(unused, bed.s, /*noisy=*/false);
    NetworkEstimator est = buildEstimator(bed.s);
    const Eigen::VectorXd x_float = est.solve(bed.s.datum);
    const std::vector<Row> rows = oneOneRows(bed, sigma_cyc);
    const NetworkEstimator::HeldSolve hb = est.solveHeldDetached(bed.s.datum, x_float, {rows});
    const NetworkEstimator::HeldSolve hu =
        est.solveHeldDetached(bed.s.datum, x_float, {{rows[0], rows[1]}});
    EXPECT_TRUE(hb.ok);
    EXPECT_TRUE(hu.ok);
    NoiseFree nf;
    nf.K            = static_cast<int>(rows.size());
    nf.nc_block     = hb.chi2[0];
    nf.expect_block = hb.chi2_expect[0];
    nf.bar_block    = chi2Quantile(nf.K, 1.0 - 1e-4);
    nf.nc_unit      = hu.chi2[0];
    nf.expect_unit  = hu.chi2_expect[0];
    return nf;
}

/// Bounds on the power of a test whose statistic, under an error of non-centrality nc,
/// has the mean E0 + nc and a variance at most 2 E0 + 4 nc (the eigenvalues of
/// S^-1 Var(v) are at most 1, so tr((S^-1 Var(v))^2) <= E0, and the cross term's
/// variance is at most 4 nc): Cantelli's inequality gives an upper bound on the power
/// while the mean is below the bar, and a lower bound while it is above; the normal
/// approximation is printed beside them.
struct PowerBounds { double upper{1.0}, lower{0.0}, normal{0.0}; };

PowerBounds powerBounds(double expect, double nc, double bar) {
    const double mu = expect + nc, var = 2.0 * expect + 4.0 * nc, gap = bar - mu;
    PowerBounds b;
    if (gap > 0.0) b.upper = var / (var + gap * gap);
    else           b.lower = 1.0 - var / (var + gap * gap);
    b.normal = 0.5 * std::erfc(gap / std::sqrt(2.0 * var));
    return b;
}

TEST(NetworkDetachedFold, OneOnePairAgainstTheOverallTestAtNetworkSize) {
    // A (1, 1) error keeps the wide lane: on a settled pair (wide lane 0.06, ionosphere-free
    // 0.04 narrow-lane cycle) its non-centrality is a few hundred, against several thousand
    // for a +1 L1 cycle.  Folded among K rows it adds its non-centrality to a statistic
    // whose bar grows with K.  Whether the overall test still sees it depends on what the
    // other rows share with the pair: nothing on the independent bed (the error's
    // non-centrality is the pair's own at every K), the shared border on the coupled bed
    // (the held rows tighten the border the pair's float error runs through, and the
    // non-centrality grows).  The unit's own statistic sees it at every K on both.  Beds of
    // K = 330 (one component at 60 stations' scale) and K = 2840 (300 stations').
    const Eigen::Matrix2d target = pairCovarianceCycles(0.06, 0.04);
    const double unit_bar = chi2Quantile(2, 1.0 - 1e-6);
    struct Bed { const char* name; PairBed bed; };
    std::vector<Bed> beds;
    {
        std::mt19937_64 rng(0xFCF1700ULL);
        beds.push_back({"independent, K 330", makeIndependentPairBed(rng, 15, 11, 20, target)});
        beds.push_back({"independent, K 2840", makeIndependentPairBed(rng, 142, 10, 20, target)});
        beds.push_back({"coupled, K 330", makePairBed(rng, 15, 11, 20, target, 24)});
        beds.push_back({"coupled, K 2840", makePairBed(rng, 142, 10, 20, target, 22)});
    }
    for (const double sig : {0.01, 0.003}) {
        std::vector<NoiseFree> nfs;
        for (const Bed& b : beds) {
            const NoiseFree nf = noiseFreeOneOne(b.bed, sig);
            nfs.push_back(nf);
            const PowerBounds overall = powerBounds(nf.expect_block, nf.nc_block, nf.bar_block);
            const PowerBounds unit    = powerBounds(nf.expect_unit, nf.nc_unit, unit_bar);
            std::cout << "[NetworkDetachedFold] (1,1) pair, hold " << sig << " cycle, " << b.name
                      << ": unit alone nc " << nf.nc_unit << " (H0 mean " << nf.expect_unit
                      << " of 2), power at least " << unit.lower << "; overall nc "
                      << nf.nc_block << ", H0 mean " << nf.expect_block << ", bar "
                      << nf.bar_block << ", power at most " << overall.upper << ", at least "
                      << overall.lower << ", about " << overall.normal << " (normal)\n";
            // The unit's own statistic catches the error at every K, on both beds.
            EXPECT_GE(unit.lower, 0.97) << b.name << ", hold " << sig;
            // Held rows can only add information about the pair (a Schur complement).
            EXPECT_GE(nf.nc_block, (1.0 - 1e-9) * nf.nc_unit) << b.name << ", hold " << sig;
        }
        // On the independent bed the error's non-centrality is the pair's own at every K,
        // and the statistic's mean is the sum of the pairs' (the kernel holds the pairs
        // apart exactly), so the overall test's power falls as its bar grows.
        for (int k = 0; k < 2; ++k) {
            const NoiseFree& nf = nfs[static_cast<std::size_t>(k)];
            EXPECT_NEAR(nf.nc_block, nf.nc_unit, 1e-8 * nf.nc_unit) << "hold " << sig;
            EXPECT_NEAR(nf.expect_block, 0.5 * nf.K * nf.expect_unit, 1e-8 * nf.K) << "hold " << sig;
        }
        if (sig == 0.01) {
            // At the design's hold, 300 stations' scale: the overall test cannot see it.
            EXPECT_LT(powerBounds(nfs[1].expect_block, nfs[1].nc_block, nfs[1].bar_block).upper, 0.01);
        }
    }
}

// ============================== the guards ==============================

/// A small fixed bed for the guards: four stations, 24 shared columns, two pins.
System guardSystem(std::uint64_t seed) {
    std::mt19937_64 rng(seed);
    return randomSystem(rng, 4, 24, 2);
}

Row oneRow(std::size_t sta, Eigen::Index col, double zhat, double pz) {
    Row r;
    r.terms  = {Term{sta, col, +1.0}};
    r.zhat_m = zhat;
    r.pz_m2  = pz;
    return r;
}

/// A DD row between stations a and b on columns c1 and c2.
Row ddRow(std::size_t a, std::size_t b, Eigen::Index c1, Eigen::Index c2, double zhat, double pz) {
    Row r;
    r.terms  = {Term{a, c1, +1.0}, Term{a, c2, -1.0}, Term{b, c1, -1.0}, Term{b, c2, +1.0}};
    r.zhat_m = zhat;
    r.pz_m2  = pz;
    return r;
}

TEST(NetworkDetachedFold, TwoBlocksSharingAStationThrow) {
    const System s = guardSystem(0xFCF1501ULL);
    NetworkEstimator est = buildEstimator(s);
    const Eigen::VectorXd x = est.solve(s.datum);
    const double pz = 1e-5;
    const Blocks shared = {{ddRow(0, 1, 0, 1, 0.1, pz)}, {ddRow(1, 2, 2, 3, 0.2, pz)}};
    EXPECT_THROW(est.normals().foldCycleRowsDetached(shared), std::invalid_argument);
    EXPECT_THROW(est.solveHeldDetached(s.datum, x, shared), std::invalid_argument);
    const Blocks disjoint = {{ddRow(0, 1, 0, 1, 0.1, pz)}, {ddRow(2, 3, 2, 3, 0.2, pz)}};
    EXPECT_NO_THROW(est.solveHeldDetached(s.datum, x, disjoint));
}

// The held solves are not timed or counted in the kernel's section timers (fixed-cycle fold design,
// section 5): with the timers armed, the float solve is counted, and a held solve with two
// folded blocks (three shared solves inside it) leaves every accumulator bitwise as it was.
TEST(NetworkDetachedFold, HeldSolveLeavesTheKernelsSectionTimersAlone) {
    const System s = guardSystem(0xFCF2501ULL);
    NetworkEstimator est = buildEstimator(s);
    est.setPerfTiming(true);
    const Eigen::VectorXd x_float = est.solve(s.datum);
    const NetworkNormalEquations::SectionTimers before = est.normals().sectionTimers();
    ASSERT_GE(before.solve_calls, 1u);
    const double pz = 1e-5;
    const Blocks two = {{ddRow(0, 1, 0, 1, 0.1, pz)}, {ddRow(2, 3, 2, 3, 0.2, pz)}};
    const NetworkEstimator::HeldSolve h = est.solveHeldDetached(s.datum, x_float, two);
    ASSERT_TRUE(h.ok);
    const NetworkNormalEquations::SectionTimers after = est.normals().sectionTimers();
    EXPECT_EQ(after.solve_calls, before.solve_calls);
    EXPECT_EQ(after.add_station_calls, before.add_station_calls);
    EXPECT_EQ(after.reduce_recomputes, before.reduce_recomputes);
    EXPECT_EQ(after.cov_consumer_calls, before.cov_consumer_calls);
    EXPECT_EQ(std::memcmp(&after.solve_ms, &before.solve_ms, sizeof(double)), 0);
    EXPECT_EQ(std::memcmp(&after.cov_inverse_ms, &before.cov_inverse_ms, sizeof(double)), 0);
    EXPECT_EQ(std::memcmp(&after.reduce_ms, &before.reduce_ms, sizeof(double)), 0);
}

TEST(NetworkDetachedFold, DependentRowsMakeTheirBlockDegenerateAtAnyHold) {
    const System s = guardSystem(0xFCF1502ULL);
    NetworkEstimator est = buildEstimator(s);
    const Eigen::VectorXd x = est.solve(s.datum);
    const NetworkNormalEquations& neq = est.normals();
    const double pz1 = (0.01 * kL1) * (0.01 * kL1);    // the design's hold, L1
    const double pz3 = (0.003 * kL1) * (0.003 * kL1);  // the FCF-2 arms' hold, L1
    const std::vector<Row> good = {ddRow(0, 1, 0, 1, 0.1, pz1), oneRow(0, 2, -0.2, pz1)};

    // The dependent sets, each beside a good block on other stations:
    //  - a repeated row, under a zero hold and under the two seam holds (a repeat would
    //    double its hold);
    //  - a contradictory dependent triple c3 = c1 + c2 held at N3 = N1 + N2 + 1 (it would be
    //    averaged into the held solve); the cycles are DDs between stations 2 and 3 on
    //    local columns (0, 1), (1, 2) and (0, 2), so c1 + c2 = c3 term by term.
    struct Case { const char* name; std::vector<Row> rows; };
    std::vector<Case> cases;
    for (double pz : {0.0, pz1, pz3}) {
        const Row r = ddRow(2, 3, 1, 2, 0.05, pz);
        cases.push_back({pz == 0.0 ? "repeat, hard" : (pz == pz1 ? "repeat, 0.01 cycle"
                                                                 : "repeat, 0.003 cycle"),
                         {r, oneRow(3, 0, 0.3, pz), r}});
    }
    for (double pz : {pz1, pz3}) {
        const double n1 = 3.0, n2 = -5.0;
        cases.push_back({pz == pz1 ? "contradictory triple, 0.01 cycle"
                                   : "contradictory triple, 0.003 cycle",
                         {ddRow(2, 3, 0, 1, kL1 * n1, pz), ddRow(2, 3, 1, 2, kL1 * n2, pz),
                          ddRow(2, 3, 0, 2, kL1 * (n1 + n2 + 1.0), pz)}});
    }
    const NetworkNormalEquations::DetachedFold alone = neq.foldCycleRowsDetached({good});
    const NetworkEstimator::HeldSolve h_alone = est.solveHeldDetached(s.datum, x, {good});
    ASSERT_TRUE(h_alone.ok);
    for (const Case& c : cases) {
        const NetworkNormalEquations::DetachedFold both = neq.foldCycleRowsDetached({good, c.rows});
        std::cout << "[NetworkDetachedFold] dependent rows (" << c.name << "): smallest pivot of"
                  << " the equilibrated B relative to the largest " << both.blocks[1].b_min_pivot_rel
                  << "\n";
        EXPECT_FALSE(both.blocks[0].degenerate) << c.name;
        EXPECT_TRUE(both.blocks[1].dependent) << c.name;
        EXPECT_TRUE(both.blocks[1].degenerate) << c.name;
        EXPECT_TRUE(both.blocks[1].factor == nullptr) << c.name;
        EXPECT_EQ(both.blocks[1].X.size(), 0) << c.name;
        EXPECT_TRUE(bitwiseEqual(both.dN, alone.dN)) << c.name << ": a degenerate block adds nothing to dN";
        EXPECT_TRUE(bitwiseEqual(both.du, alone.du)) << c.name;
        EXPECT_THROW(NetworkNormalEquations::solveFoldBlock(both.blocks[1], Eigen::VectorXd::Zero(3)),
                     std::logic_error);

        const NetworkEstimator::HeldSolve h = est.solveHeldDetached(s.datum, x, {good, c.rows});
        EXPECT_TRUE(h.ok) << c.name;
        EXPECT_EQ(h.degenerate[1], 1) << c.name;
        EXPECT_EQ(h.dependent[1], 1) << c.name;
        EXPECT_TRUE(h.kappa[1].size() == 0) << c.name;
        EXPECT_TRUE(std::isnan(h.chi2[1])) << c.name;
        EXPECT_TRUE(std::isnan(h.chi2_expect[1])) << c.name;
        EXPECT_EQ(h.v[1].size(), 3) << c.name;
        EXPECT_EQ(h.dof_joint, 2) << c.name;
        EXPECT_TRUE(bitwiseEqual(h.x_s, h_alone.x_s)) << c.name;
    }

    // The independent rows of the same sets fold, under a zero hold too: a zero hold
    // variance is legal where B alone is positive definite.
    for (double pz : {0.0, pz1, pz3}) {
        const NetworkNormalEquations::DetachedFold ok = neq.foldCycleRowsDetached(
            {{ddRow(2, 3, 1, 2, 0.05, pz), oneRow(3, 0, 0.3, pz)},
             {ddRow(0, 1, 0, 1, kL1 * 3.0, pz), ddRow(0, 1, 1, 2, kL1 * -5.0, pz)}});
        for (const NetworkNormalEquations::DetachedFoldBlock& fb : ok.blocks) {
            EXPECT_FALSE(fb.dependent) << "pz " << pz;
            EXPECT_FALSE(fb.degenerate) << "pz " << pz;
            EXPECT_GT(fb.b_min_pivot_rel, 1e-12) << "pz " << pz;
        }
    }
}

TEST(NetworkDetachedFold, FloatFromAPerturbedSystemIsNotReproduced) {
    const System s = guardSystem(0xFCF1503ULL);
    System t = s;
    t.sta[1].y(2) += 0.5;   // one observation moved: a different reduced right-hand side
    NetworkEstimator est_a = buildEstimator(s);
    NetworkEstimator est_b = buildEstimator(t);
    const Eigen::VectorXd x_a = est_a.solve(s.datum);
    const Eigen::VectorXd x_b = est_b.solve(t.datum);
    const Blocks blocks = {{ddRow(0, 1, 0, 1, 0.1, 1e-5)}, {ddRow(2, 3, 2, 3, 0.2, 1e-5)}};
    const NetworkEstimator::HeldSolve own   = est_a.solveHeldDetached(s.datum, x_a, blocks);
    const NetworkEstimator::HeldSolve other = est_a.solveHeldDetached(s.datum, x_b, blocks);
    std::cout << "[NetworkDetachedFold] float_repro_rel (normwise backward error): own float "
              << own.float_repro_rel << ", a perturbed system's float " << other.float_repro_rel
              << "\n";
    EXPECT_TRUE(own.ok);
    EXPECT_LT(own.float_repro_rel, 1e-12);
    EXPECT_GT(own.max_shift_m, 0.0);
    EXPECT_GT(other.float_repro_rel, 1e-8);
    EXPECT_FALSE(other.ok);
    // A refused held solve hands back the float it was given, bitwise: no reader of x_s
    // receives a held vector built on a cache that did not produce that float.
    EXPECT_TRUE(bitwiseEqual(other.x_s, x_b));
    EXPECT_GT(other.max_shift_m, 0.0);   // the diagnostic shift of the refused estimate

    // A float that is not finite: refused (infinite backward error), x_s still the float
    // passed in, bitwise, and the shift infinite.
    Eigen::VectorXd x_nan = x_a;
    x_nan(3) = std::numeric_limits<double>::quiet_NaN();
    const NetworkEstimator::HeldSolve nan = est_a.solveHeldDetached(s.datum, x_nan, blocks);
    EXPECT_FALSE(nan.ok);
    EXPECT_TRUE(std::isinf(nan.float_repro_rel));
    EXPECT_TRUE(std::isinf(nan.max_shift_m));
    EXPECT_TRUE(bitwiseEqual(nan.x_s, x_nan));
}

TEST(NetworkDetachedFold, FloatReproductionIsRoundOffAtEveryConditioning) {
    // The normwise backward error of the float on its own cache stays at round-off where a
    // residual relative to |u| alone grows with the condition number.  One shared column
    // is observed weakly, so that the pinned reduced normals' condition number spans 1e3
    // to 1e11, and the truth is a large multiple of their weakest eigenvector with
    // noise-free data: the float then lies in the weakly determined direction, u = N x is
    // small, and the residual of a backward-stable solve, of order the round-off times
    // |N| |x|, is large against |u| (the case of the FCF-1 review).
    for (double weak : {1e-1, 1e-3, 1e-5}) {
        System s = guardSystem(0xFCF1506ULL);
        Eigen::Index c = 0;   // the first shared column the datum does not pin
        while (std::any_of(s.datum.begin(), s.datum.end(),
                           [c](const NetworkEstimator::Pin& p) { return p.column == c; }))
            ++c;
        for (StationData& st : s.sta) st.A_s.col(c) *= weak;   // one weakly observed column
        s.prior.w(c) *= weak * weak;
        Eigen::MatrixXd Np, Np2;
        Eigen::VectorXd up, up2;
        {
            NetworkEstimator est = buildEstimator(s);
            est.solve(s.datum);
            Np = est.normals().reducedSharedNormals();
            up = est.normals().reducedSharedRhs();
            NetworkEstimator::applyDatumPins(Np, up, s.datum);
        }
        // The weakest eigenvector of the pinned normals has no pinned component (a pinned
        // row and column are zero apart from the diagonal), so the pins hold it at zero.
        const Eigen::SelfAdjointEigenSolver<Eigen::MatrixXd> eig(Np);
        const double cond = eig.eigenvalues().maxCoeff() / eig.eigenvalues().minCoeff();
        s.x_s_true = 1e3 * eig.eigenvectors().col(0);
        for (NetworkEstimator::Pin& p : s.datum) p.value = s.x_s_true(p.column);
        std::mt19937_64 unused(0);
        drawObservations(unused, s, /*noisy=*/false);
        NetworkEstimator est = buildEstimator(s);
        const Eigen::VectorXd x = est.solve(s.datum);
        const NetworkEstimator::HeldSolve h =
            est.solveHeldDetached(s.datum, x, {{ddRow(0, 1, 0, 1, 0.1, 1e-5)}});
        Np2 = est.normals().reducedSharedNormals();
        up2 = est.normals().reducedSharedRhs();
        NetworkEstimator::applyDatumPins(Np2, up2, s.datum);
        const double rel_u = (Np2 * x - up2).cwiseAbs().maxCoeff() / up2.cwiseAbs().maxCoeff();
        std::cout << "[NetworkDetachedFold] float reproduction: cond " << cond
                  << ", normwise backward error " << h.float_repro_rel
                  << ", residual relative to |u| " << rel_u << "\n";
        EXPECT_TRUE(h.ok) << "weak " << weak;
        EXPECT_LT(h.float_repro_rel, 1e-13) << "weak " << weak;
    }
}

TEST(NetworkDetachedFold, EmptyInputReturnsTheFloatExactly) {
    const System s = guardSystem(0xFCF1504ULL);
    NetworkEstimator est = buildEstimator(s);
    const Eigen::VectorXd x = est.solve(s.datum);
    const NetworkEstimator::HeldSolve h = est.solveHeldDetached(s.datum, x, {});
    EXPECT_FALSE(h.ok);
    EXPECT_TRUE(bitwiseEqual(h.x_s, x));
    EXPECT_TRUE(h.v.empty());
    EXPECT_TRUE(h.kappa.empty());
    EXPECT_EQ(h.max_shift_m, 0.0);
    // Only empty blocks (no rows): nothing folds either.
    const NetworkEstimator::HeldSolve he = est.solveHeldDetached(s.datum, x, {{}, {}});
    EXPECT_FALSE(he.ok);
    EXPECT_TRUE(bitwiseEqual(he.x_s, x));
    EXPECT_EQ(he.chi2[0], 0.0);
    const NetworkNormalEquations::DetachedFold f = est.normals().foldCycleRowsDetached({});
    EXPECT_TRUE(f.blocks.empty());
    EXPECT_EQ(f.dN.cwiseAbs().maxCoeff(), 0.0);
    EXPECT_EQ(f.du.cwiseAbs().maxCoeff(), 0.0);
}

TEST(NetworkDetachedFold, PreconditionsAndMalformedRowsThrow) {
    const System s = guardSystem(0xFCF1505ULL);
    const Blocks blocks = {{ddRow(0, 1, 0, 1, 0.1, 1e-5)}};
    {
        NetworkEstimator est = buildEstimator(s);   // not solved
        EXPECT_THROW(est.solveHeldDetached(s.datum, Eigen::VectorXd::Zero(24), blocks),
                     std::logic_error);
    }
    {
        NetworkEstimator est = buildEstimator(s);   // the float solve folded Card-4 DD priors
        est.setDdPriors(asDdPriors(blocks));
        const Eigen::VectorXd x = est.solve(s.datum);
        EXPECT_THROW(est.solveHeldDetached(s.datum, x, blocks), std::logic_error);
        // The guard reads the kernel's folded state, not the request list: clearing the
        // list after the solve leaves the folded system, and the call still throws.
        est.setDdPriors({});
        EXPECT_THROW(est.solveHeldDetached(s.datum, x, blocks), std::logic_error);
    }
    {
        NetworkEstimator est = buildEstimator(s);   // the float solve folded a sum-to-zero gauge
        NetworkNormalEquations::SumZeroGauge g;
        g.terms = {{0, 3}, {1, 3}, {2, 3}};
        est.setPhaseSumZeroGauges({g});
        const Eigen::VectorXd x = est.solve(s.datum);
        ASSERT_EQ(est.normals().numSumZeroGauges(), 1u);
        EXPECT_THROW(est.solveHeldDetached(s.datum, x, blocks), std::logic_error);
        est.setPhaseSumZeroGauges({});
        EXPECT_THROW(est.solveHeldDetached(s.datum, x, blocks), std::logic_error);
    }
    {
        // A request set after the solve folded nothing: the cached system is the float's,
        // and the held solve runs.
        NetworkEstimator est = buildEstimator(s);
        const Eigen::VectorXd x = est.solve(s.datum);
        est.setDdPriors(asDdPriors(blocks));
        EXPECT_NO_THROW(est.solveHeldDetached(s.datum, x, blocks));
    }
    NetworkEstimator est = buildEstimator(s);
    const Eigen::VectorXd x = est.solve(s.datum);
    EXPECT_THROW(est.solveHeldDetached(s.datum, Eigen::VectorXd::Zero(5), blocks),
                 std::invalid_argument);
    const NetworkNormalEquations& neq = est.normals();
    Row empty_terms;
    empty_terms.pz_m2 = 1e-5;
    EXPECT_THROW(neq.foldCycleRowsDetached({{empty_terms}}), std::invalid_argument);
    EXPECT_THROW(neq.foldCycleRowsDetached({{oneRow(0, 0, 0.0, -1e-5)}}), std::invalid_argument);
    EXPECT_THROW(neq.foldCycleRowsDetached(
                     {{oneRow(0, 0, std::numeric_limits<double>::quiet_NaN(), 1e-5)}}),
                 std::invalid_argument);
    EXPECT_THROW(neq.foldCycleRowsDetached({{oneRow(0, 99, 0.0, 1e-5)}}), std::invalid_argument);
    EXPECT_THROW(neq.foldCycleRowsDetached({{oneRow(99, 0, 0.0, 1e-5)}}), std::out_of_range);
    // The pseudo-station of the shared prior has no locals: any column is out of range.
    EXPECT_THROW(neq.foldCycleRowsDetached({{oneRow(4, 0, 0.0, 1e-5)}}), std::invalid_argument);
}

}  // namespace
}  // namespace GnssProcessor
