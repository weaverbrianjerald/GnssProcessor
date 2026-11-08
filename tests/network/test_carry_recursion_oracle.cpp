/**
 * @file test_carry_recursion_oracle.cpp
 * @brief A multi-epoch oracle of the shipped carry recursion across arc events
 *        (docs/refound/CRO_carry_recursion_oracle_2026-11-07.md).
 *
 * One synthetic network (five satellites, three stations, two bands of phase and code, a free
 * receiver clock per station and epoch and a free slant ionosphere per line of sight and epoch,
 * station 0 holding the phase datum, the clock pivot pinned at 0, the other clocks walking at the
 * Q3 anchor's 2.9e-8 m^2/s over 30 s epochs, sixty epochs) carries four events, one at a time and
 * then together:
 *
 *   A  an ambiguity arc born at epoch 20 (station 2 first sees satellite 3);
 *   B  a slip of both bands of (station 1, satellite 3) at epoch 30, the truth jumping by 5 L1
 *      and 3 L2 cycles;
 *   C  satellite 2 absent over epochs 40 to 49 and back at 50;
 *   D  the pivot satellite 0 setting at epoch 45, the pivot moving to satellite 1.
 *
 * The reference is a joint information-form Kalman filter over the whole state (every non-pivot
 * clock, an absent one included; every phase-bias segment; every ambiguity arc), with each event
 * handled exactly: a birth and a slip's new arc enter at zero information, a departed clock keeps
 * walking unobserved, a handover is the S-transform S P S^T of the whole state.  It is checked
 * against the batch over epochs 1 to k.
 *
 * The candidate is the golden posture's per-station recursion, generalized from the exact-update
 * fixture's mode 3 (tests/network/test_coupling_exact_update.cpp) to a changing set of arcs and
 * columns, on the shipped functions where one exists: couplingExactRow and couplingExactCross for
 * the rows, ambFloatDropFloored for the slip, applyClockStransform (the diagonal kernel, after the
 * rows) for the handover; the READMIT seed is carryFoldDecide's Seeded rule (var + q * gap) and
 * the dense border fold is BorderPrior.cpp's (the stash block with its diagonal replaced).  The
 * same code with the joint block and a slip answered by erase and rebirth is the factorized form
 * of the reference and must equal it.
 *
 * The fixture builders of test_coupling_exact_update.cpp are not shared: this one needs an arc
 * lifecycle, a time-varying pivot and a clock continuous across a gap, which change every builder.
 */

#include <gtest/gtest.h>

#include "network/state/RunContext.hpp"
#include "network/ar/AmbFloat.hpp"
#include "network/carry/CouplingExact.hpp"
#include "network/driver/RunTypes.hpp"
#include "network/solve/HandoverStransform.hpp"
#include "network/solve/Sbasis.hpp"

#include <Eigen/Dense>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <map>
#include <random>
#include <string>
#include <tuple>
#include <vector>

using namespace GnssProcessor;

namespace {

using Mat = Eigen::MatrixXd;
using Vec = Eigen::VectorXd;

// ---------------------------------------------------------------------------
// The network and its events.
// ---------------------------------------------------------------------------

constexpr int    kS      = 5;          // satellites G01..G05 (index 0..4)
constexpr int    kR      = 3;          // stations; station 0 holds the phase datum
constexpr int    kK      = 60;         // epochs 1..kK
constexpr double kDt     = 30.0;       // s
constexpr double kQClk   = 2.9e-8;     // the Q3 anchor's clock walk (m^2/s)
constexpr double kSigPh  = 0.003;      // m
constexpr double kSigCd  = 0.30;       // m
const double     kAlpha[2] = {1.0, 1.6469444444444444};   // (f1/f2)^2, GPS L1/L2
const double     kLambda[2] = {0.19029367279836487, 0.24421021342456825};   // m

// Event A: the line of sight (station 2, satellite 3) is first observed at epoch 20.
constexpr int kBirthK = 20, kBirthR = 2, kBirthS = 3;
// Event B: both bands of (station 1, satellite 3) slip at epoch 30.
constexpr int kSlipK = 30, kSlipR = 1, kSlipS = 3;
const double  kSlipM[2] = {5.0 * kLambda[0], 3.0 * kLambda[1]};
// Event C: satellite 2 is absent over epochs 40..49 and returns at 50.
constexpr int kDepS = 2, kDepK0 = 40, kDepK1 = 49;
// Event D: the pivot satellite 0 sets at epoch 45 and the pivot moves to satellite 1.
constexpr int kHandK = 45, kOldPiv = 0, kNewPiv = 1;

struct Events {
    bool birth{false}, slip{false}, readmit{false}, handover{false};
};

/// Identity of a column: a satellite clock (s), a phase-bias segment (s, band j, seg) or an
/// ambiguity arc (station r, s, j, generation gen in @c seg).
enum class Kind : int { Clk = 0, Pb = 1, Amb = 2 };
struct Key {
    Kind kind{Kind::Clk};
    int  s{0}, j{0}, seg{0}, r{0};
    bool operator<(const Key& o) const {
        return std::tie(kind, s, j, seg, r) < std::tie(o.kind, o.s, o.j, o.seg, o.r);
    }
    bool operator==(const Key& o) const {
        return kind == o.kind && s == o.s && j == o.j && seg == o.seg && r == o.r;
    }
};
Key clkKey(int s) { return Key{Kind::Clk, s, 0, 0, 0}; }

std::string satName(int s) { return "G0" + std::to_string(s + 1); }
const char* sigName(int j) { return j == 0 ? "L1C" : "L2W"; }

struct Net {
    Events ev;
    std::array<std::array<double, kS>, kK + 1> c{};            // the clocks in the epoch's frame
    double p[kS][2]{};
    double N[kR][kS][2][2]{};                                   // [r][s][j][generation]
    std::array<std::array<double, kR>, kK + 1> rx{};
    std::array<std::array<std::array<double, kS>, kR>, kK + 1> ion{};
    struct Obs { double ph[2]{}, cd[2]{}; };
    std::array<std::array<std::array<Obs, kS>, kR>, kK + 1> obs{};

    bool handoverAt(int k) const { return ev.handover && k == kHandK; }
    int  pivot(int k) const { return (ev.handover && k >= kHandK) ? kNewPiv : kOldPiv; }
    bool satPresent(int k, int s) const {
        if (ev.readmit && s == kDepS && k >= kDepK0 && k <= kDepK1) return false;
        if (ev.handover && s == kOldPiv && k >= kHandK) return false;
        return true;
    }
    bool losPresent(int k, int r, int s) const {
        if (!satPresent(k, s)) return false;
        return !(ev.birth && r == kBirthR && s == kBirthS && k < kBirthK);
    }
    int pbSeg(int k, int s) const { return (ev.readmit && s == kDepS && k > kDepK1) ? 1 : 0; }
    int arcGen(int k, int r, int s) const {
        return (ev.slip && r == kSlipR && s == kSlipS && k >= kSlipK) ? 1 : 0;
    }
    /// Whether the reference carries satellite s's clock at epoch k: every non-pivot clock,
    /// an absent satellite's included, except the outgoing pivot after it has set.
    bool refClock(int k, int s) const {
        if (s == pivot(k)) return false;
        return !(ev.handover && s == kOldPiv && k >= kHandK);
    }
    static double time(int k) { return 1000.0 + k * kDt; }
};

/// The truth in the estimator's frame and the observations.  Every random number is drawn in a
/// fixed order whatever the events, so all scenarios share one realization.
Net makeNet(const Events& ev, unsigned seed = 2026u) {
    Net n;
    n.ev = ev;
    std::mt19937 rng(seed);
    std::normal_distribution<double> nd(0.0, 1.0);
    double c0[kS], w[kK + 1][kS];
    for (int s = 0; s < kS; ++s) {
        c0[s] = 0.8 * nd(rng);
        for (int j = 0; j < 2; ++j) n.p[s][j] = nd(rng);
        for (int r = 0; r < kR; ++r)
            for (int j = 0; j < 2; ++j) {
                n.N[r][s][j][0] = 3.0 * nd(rng);
                n.N[r][s][j][1] = n.N[r][s][j][0]
                                + ((r == kSlipR && s == kSlipS) ? kSlipM[j] : 0.0);
            }
    }
    for (int k = 2; k <= kK; ++k)
        for (int s = 0; s < kS; ++s) w[k][s] = nd(rng);
    // The clocks: the pivot at 0, the others walking independently at kQClk; at the handover
    // epoch the previous clocks are re-expressed in the incoming pivot's frame first.
    for (int s = 0; s < kS; ++s) n.c[1][s] = (s == n.pivot(1)) ? 0.0 : c0[s];
    for (int k = 2; k <= kK; ++k) {
        const double shift = n.handoverAt(k) ? n.c[k - 1][kNewPiv] : 0.0;
        for (int s = 0; s < kS; ++s)
            n.c[k][s] = (s == n.pivot(k)) ? 0.0
                      : n.c[k - 1][s] - shift + std::sqrt(kQClk * kDt) * w[k][s];
    }
    for (int k = 1; k <= kK; ++k)
        for (int r = 0; r < kR; ++r) {
            n.rx[k][r] = 10.0 * nd(rng);
            for (int s = 0; s < kS; ++s) {
                n.ion[k][r][s] = 5.0 + nd(rng);
                auto& o = n.obs[k][r][s];
                const int g = n.arcGen(k, r, s);
                for (int j = 0; j < 2; ++j) {
                    o.ph[j] = n.rx[k][r] - n.c[k][s] + n.p[s][j] + n.N[r][s][j][g]
                            - kAlpha[j] * n.ion[k][r][s] + kSigPh * nd(rng);
                    o.cd[j] = n.rx[k][r] - n.c[k][s] + kAlpha[j] * n.ion[k][r][s]
                            + kSigCd * nd(rng);
                }
            }
        }
    return n;
}

// ---------------------------------------------------------------------------
// One epoch's information, the epoch's locals eliminated.
// ---------------------------------------------------------------------------

/// Inverse of a symmetric positive-definite matrix through LDLT.
Mat spdInverse(const Mat& A) {
    const Eigen::LDLT<Mat> f(0.5 * (A + A.transpose()));
    return f.solve(Mat::Identity(A.rows(), A.cols()));
}

/// The information one epoch's rows carry on its global columns, after the receiver clocks and
/// the slant ionosphere (nothing else involves them) are eliminated by a Schur complement.
/// Known terms move to the right-hand side at their true values: the pinned pivot's clock (0 in
/// the frame) and station 0's ambiguities (the held datum).
struct EpochInfo {
    std::vector<Key> keys;
    Mat L;
    Vec e;
};

EpochInfo epochInfo(const Net& n, int k) {
    std::map<Key, int> gi;
    std::vector<Key> gk;
    const auto gcol = [&](const Key& key) {
        const auto it = gi.find(key);
        if (it != gi.end()) return it->second;
        const int c = static_cast<int>(gk.size());
        gi[key] = c;
        gk.push_back(key);
        return c;
    };
    std::map<std::pair<int, int>, int> ion_l;   // (r, s) -> local index after the receiver clocks
    for (int r = 0; r < kR; ++r)
        for (int s = 0; s < kS; ++s)
            if (n.losPresent(k, r, s)) {
                const int li = kR + static_cast<int>(ion_l.size());
                ion_l[{r, s}] = li;
            }
    const int nl = kR + static_cast<int>(ion_l.size());
    struct Row {
        std::vector<std::pair<int, double>> g, l;
        double y{0.0}, w{0.0};
    };
    std::vector<Row> rows;
    for (int r = 0; r < kR; ++r)
        for (int s = 0; s < kS; ++s) {
            if (!n.losPresent(k, r, s)) continue;
            const auto& o = n.obs[k][r][s];
            const int g = n.arcGen(k, r, s);
            for (int type = 0; type < 2; ++type)   // 0 phase, 1 code
                for (int j = 0; j < 2; ++j) {
                    Row row;
                    row.y = (type == 0) ? o.ph[j] : o.cd[j];
                    row.w = 1.0 / std::pow(type == 0 ? kSigPh : kSigCd, 2);
                    row.l.emplace_back(r, 1.0);                                   // receiver clock
                    row.l.emplace_back(ion_l.at({r, s}), (type == 0 ? -1.0 : 1.0) * kAlpha[j]);
                    if (s == n.pivot(k)) row.y += n.c[k][s];                     // the pinned pivot
                    else row.g.emplace_back(gcol(clkKey(s)), -1.0);
                    if (type == 0) {
                        row.g.emplace_back(gcol(Key{Kind::Pb, s, j, n.pbSeg(k, s), 0}), 1.0);
                        if (r == 0) row.y -= n.N[r][s][j][g];                    // the held datum
                        else row.g.emplace_back(gcol(Key{Kind::Amb, s, j, g, r}), 1.0);
                    }
                    rows.push_back(row);
                }
        }
    const int ng = static_cast<int>(gk.size());
    Mat A = Mat::Zero(ng + nl, ng + nl);
    Vec b = Vec::Zero(ng + nl);
    for (const Row& row : rows) {
        std::vector<std::pair<int, double>> a = row.g;
        for (const auto& [li, v] : row.l) a.emplace_back(ng + li, v);
        for (const auto& [ci, ai] : a) {
            for (const auto& [cj, aj] : a) A(ci, cj) += row.w * ai * aj;
            b(ci) += row.w * ai * row.y;
        }
    }
    const Mat Lll_inv = spdInverse(A.bottomRightCorner(nl, nl));
    const Mat Lgl = A.topRightCorner(ng, nl);
    EpochInfo out;
    out.keys = gk;
    out.L = A.topLeftCorner(ng, ng) - Lgl * Lll_inv * Lgl.transpose();
    out.e = b.head(ng) - Lgl * (Lll_inv * b.tail(nl));
    return out;
}

/// The border at epoch k: the clocks and phase biases of the epoch, in key order.
std::vector<Key> borderKeys(const EpochInfo& ei) {
    std::vector<Key> b;
    for (const Key& key : ei.keys)
        if (key.kind != Kind::Amb) b.push_back(key);
    std::sort(b.begin(), b.end());
    return b;
}

/// The shared border after one epoch: its keys, mean and covariance.
struct EpochBorder {
    std::vector<Key> keys;
    Vec x;
    Mat P;
};

/// Adds @p w a a' to L and @p w a y to e for a sparse row a.
void addRow(Mat& L, Vec& e, const std::vector<std::pair<int, double>>& a, double y, double w) {
    for (const auto& [ci, ai] : a) {
        for (const auto& [cj, aj] : a) L(ci, cj) += w * ai * aj;
        e(ci) += w * ai * y;
    }
}

// ---------------------------------------------------------------------------
// The reference: the joint information-form filter, and the batch that checks it.
// ---------------------------------------------------------------------------

struct RefOut {
    std::vector<EpochBorder> ep;          // ep[k], k = 1..kK
    // The predicted state at the handover epoch (after the transform and the walk), for the
    // contrast instrument.
    std::vector<Key> hand_keys;
    Mat hand_P;
};

RefOut runReference(const Net& n) {
    RefOut out;
    out.ep.resize(kK + 1);
    std::vector<Key> sk;   // state keys
    Vec x;
    Mat P;
    for (int k = 1; k <= kK; ++k) {
        const EpochInfo ei = epochInfo(n, k);
        if (k > 1) {
            if (n.handoverAt(k)) {
                // The exact S-transform: every clock re-expressed against the incoming pivot,
                // whose row leaves (its new-frame value is exactly the pin).
                int iq = -1;
                for (int i = 0; i < static_cast<int>(sk.size()); ++i)
                    if (sk[i] == clkKey(kNewPiv)) iq = i;
                EXPECT_GE(iq, 0);
                std::vector<Key> nk;
                Mat T = Mat::Zero(static_cast<Eigen::Index>(sk.size()) - 1,
                                  static_cast<Eigen::Index>(sk.size()));
                for (int i = 0; i < static_cast<int>(sk.size()); ++i) {
                    if (i == iq) continue;
                    const int row = static_cast<int>(nk.size());
                    nk.push_back(sk[i]);
                    T(row, i) = 1.0;
                    if (sk[i].kind == Kind::Clk) T(row, iq) = -1.0;
                }
                x = (T * x).eval();
                P = (T * P * T.transpose()).eval();
                sk = nk;
            }
            for (int i = 0; i < static_cast<int>(sk.size()); ++i)
                if (sk[i].kind == Kind::Clk) P(i, i) += kQClk * kDt;
            if (n.handoverAt(k)) {
                out.hand_keys = sk;
                out.hand_P = P;
            }
        }
        // New columns enter at zero information.
        std::vector<Key> all = sk;
        std::map<Key, int> idx;
        for (int i = 0; i < static_cast<int>(all.size()); ++i) idx[all[i]] = i;
        for (const Key& key : ei.keys)
            if (!idx.count(key)) {
                idx[key] = static_cast<int>(all.size());
                all.push_back(key);
            }
        const int nx = static_cast<int>(all.size()), ns = static_cast<int>(sk.size());
        Mat L = Mat::Zero(nx, nx);
        Vec e = Vec::Zero(nx);
        if (ns > 0) {
            const Mat Lam = spdInverse(P);
            L.topLeftCorner(ns, ns) = Lam;
            e.head(ns) = Lam * x;
        }
        for (int a = 0; a < static_cast<int>(ei.keys.size()); ++a) {
            const int ia = idx.at(ei.keys[a]);
            e(ia) += ei.e(a);
            for (int c = 0; c < static_cast<int>(ei.keys.size()); ++c) L(ia, idx.at(ei.keys[c])) += ei.L(a, c);
        }
        P = spdInverse(L);
        x = P * e;
        sk = all;
        EpochBorder& eb = out.ep[k];
        eb.keys = borderKeys(ei);
        const int nb = static_cast<int>(eb.keys.size());
        eb.x.resize(nb);
        eb.P.resize(nb, nb);
        for (int a = 0; a < nb; ++a) {
            eb.x(a) = x(idx.at(eb.keys[a]));
            for (int c = 0; c < nb; ++c) eb.P(a, c) = P(idx.at(eb.keys[a]), idx.at(eb.keys[c]));
        }
    }
    return out;
}

/// The batch over epochs 1..kend: a clock column per (epoch, satellite) the reference carries,
/// one column per phase-bias segment and per arc, every epoch's information and the walk ties
/// (written in the new frame across the handover), all at once.  Returns the border at kend.
EpochBorder runBatch(const Net& n, int kend) {
    std::map<std::pair<int, int>, int> ccol;
    std::map<Key, int> gcol;
    int nc = 0;
    for (int k = 1; k <= kend; ++k)
        for (int s = 0; s < kS; ++s)
            if (n.refClock(k, s)) ccol[{k, s}] = nc++;
    std::vector<EpochInfo> eis(static_cast<std::size_t>(kend + 1));
    for (int k = 1; k <= kend; ++k) {
        eis[k] = epochInfo(n, k);
        for (const Key& key : eis[k].keys)
            if (key.kind != Kind::Clk && !gcol.count(key)) gcol[key] = nc++;
    }
    Mat L = Mat::Zero(nc, nc);
    Vec e = Vec::Zero(nc);
    for (int k = 1; k <= kend; ++k) {
        const EpochInfo& ei = eis[k];
        std::vector<int> ci(ei.keys.size());
        for (std::size_t a = 0; a < ei.keys.size(); ++a)
            ci[a] = ei.keys[a].kind == Kind::Clk ? ccol.at({k, ei.keys[a].s}) : gcol.at(ei.keys[a]);
        for (std::size_t a = 0; a < ei.keys.size(); ++a) {
            e(ci[a]) += ei.e(static_cast<Eigen::Index>(a));
            for (std::size_t c = 0; c < ei.keys.size(); ++c)
                L(ci[a], ci[c]) += ei.L(static_cast<Eigen::Index>(a), static_cast<Eigen::Index>(c));
        }
    }
    const double w = 1.0 / (kQClk * kDt);
    for (int k = 2; k <= kend; ++k)
        for (int s = 0; s < kS; ++s) {
            if (!n.refClock(k, s)) continue;
            std::vector<std::pair<int, double>> a{{ccol.at({k, s}), 1.0}, {ccol.at({k - 1, s}), -1.0}};
            if (n.handoverAt(k)) a.emplace_back(ccol.at({k - 1, kNewPiv}), 1.0);   // c' = c - c_q
            addRow(L, e, a, 0.0, w);
        }
    const Eigen::LDLT<Mat> f(L);
    const Vec x = f.solve(e);
    EpochBorder eb;
    eb.keys = borderKeys(eis[kend]);
    const int nb = static_cast<int>(eb.keys.size());
    std::vector<int> bc(nb);
    for (int a = 0; a < nb; ++a)
        bc[a] = eb.keys[a].kind == Kind::Clk ? ccol.at({kend, eb.keys[a].s}) : gcol.at(eb.keys[a]);
    Mat Ib = Mat::Zero(nc, nb);
    for (int a = 0; a < nb; ++a) Ib(bc[a], a) = 1.0;
    const Mat cols = f.solve(Ib);
    eb.x.resize(nb);
    eb.P.resize(nb, nb);
    for (int a = 0; a < nb; ++a) {
        eb.x(a) = x(bc[a]);
        for (int c = 0; c < nb; ++c) eb.P(a, c) = cols(bc[a], c);
    }
    return eb;
}

// ---------------------------------------------------------------------------
// The candidate: the shipped per-station recursion with the vehicle's event responses.
// ---------------------------------------------------------------------------

enum class SlipResp { Drop, Erase };

/// How the candidate meets the clock-pivot handover.
enum class HandoverForm {
    /// The golden posture: the rows built before the transform against the stash in the old
    /// frame (the departed pivot's term spread over the S clocks), then the diagonal kernel
    /// applyClockStransform on the carried values, the fold on the stash's off-diagonals.
    DiagonalAfterRows,
    /// network.handover_stransform_dense: the same rows, the fold on the dense kernel's exact
    /// S P S^T (applyClockStransformDense).
    DenseAfterRows,
    /// A test-side exact form: the dense kernel's block with the outgoing pivot appended, and
    /// the rows re-expressed in the new frame before they are built, the departed pivot's term
    /// on its own column (which the one rule then re-attributes as departed).
    ExactRows,
};

struct CandOpts {
    /// false: the shipped per-station form (the exact-update fixture's mode 3: per-row inflation
    /// on the diagonal only, the cross-station harvest covariance dropped).  true: the joint
    /// block (mode 0: every pair's time-update covariance and the whole harvest covariance).
    bool joint{false};
    /// Drop: the float vehicle's bounded drop (shipped).  Erase: the slipped arc's row deleted
    /// and the post-slip arc born fresh (the exact response).
    SlipResp slip{SlipResp::Drop};
    /// The handover kernel's mechanism: Handover (the golden posture: after the rows, membership
    /// from the registered clocks) or HandoverBeforeRows (its wide traversal also reaches a parked
    /// clock of the constellation).  Only the value moves of the kernel are used either way.
    CarryMechanism sts_mech{CarryMechanism::Handover};
    HandoverForm handover{HandoverForm::DiagonalAfterRows};
    /// false: no READMIT park (network.readmit_seed_horizon_s 0): the returning clock is fresh.
    bool readmit_seed{true};
    /// true (the producer's dense fold): the pinned incoming pivot's carried prior is folded with
    /// the others and its pin conditions them; false: its prior is marginalized out of the fold.
    bool fold_pinned_pivot{true};
};

/// The dense kernel applyClockStransformDense on the carried stash (and any parked clock):
/// the re-expressed values and block in the stash's row order, the incoming pivot's row zero,
/// and the outgoing pivot appended (value -c0) when the kernel adds it.
struct DenseStash {
    std::vector<double> val;              // bprev order, then the appended pivot if any
    Mat P;                                // the same order
    bool p_added{false};
    std::vector<std::pair<double, double>> parks;   // parked (value, var) after the kernel
    ClockStransformDenseResult res;
};

template <class ParkT>
DenseStash denseTransform(const std::vector<Key>& bprev, const Vec& sprev, const Mat& Pprev,
                          const std::vector<ParkT>& parks, int q, int p_out) {
    NetworkCarryState carry;
    net::NetworkEntities ent;
    const int np = static_cast<int>(bprev.size());
    carry.prev_carry_cov.P = Pprev;
    for (int p = 0; p < np; ++p) {
        CarryPrior cp{};
        cp.kind   = bprev[p].kind == Kind::Clk ? SharedParamKind::Clock : SharedParamKind::PhaseBias;
        cp.sat    = satName(bprev[p].s);
        cp.signal = bprev[p].kind == Kind::Clk ? "" : sigName(bprev[p].j);
        cp.value  = sprev(p);
        cp.var    = Pprev(p, p);
        carry.prev_carry_cov.row[sharedCarryKey(cp.kind, cp.sat, cp.signal)] = static_cast<std::size_t>(p);
        carry.prev_carry.push_back(cp);
    }
    for (const ParkT& pk : parks) {
        CarryPrior cp{};
        cp.kind     = SharedParamKind::Clock;
        cp.sat      = satName(pk.s);
        cp.value    = pk.value;
        cp.var      = pk.var;
        cp.t_park_s = pk.t;
        carry.prev_carry.push_back(cp);
    }
    DenseStash out;
    out.res = applyClockStransformDense(carry, ent, 'G', satName(q), satName(p_out));
    EXPECT_TRUE(out.res.applied) << out.res.reason;
    EXPECT_TRUE(out.res.selfcheck_ok);
    out.p_added = out.res.p_added;
    out.P = carry.prev_carry_cov.P;
    for (int p = 0; p < np; ++p) out.val.push_back(carry.prev_carry[static_cast<std::size_t>(p)].value);
    for (std::size_t a = 0; a < parks.size(); ++a) {
        const CarryPrior& cp = carry.prev_carry[static_cast<std::size_t>(np) + a];
        out.parks.emplace_back(cp.value, cp.var);
    }
    if (out.p_added) out.val.push_back(carry.prev_carry.back().value);
    return out;
}

struct CandOut {
    std::vector<EpochBorder> ep;                 // ep[k]
    // The slip: the shipped drop's outcome.
    int    drops_acted{0};
    double drop_var_restored{0.0};               // the restored variance of the L1 arc (m^2)
    double drop_var_before{0.0};                 // its carried conditional variance before (m^2)
    double drop_value_kept_err{0.0};             // kept value minus the post-slip truth, L1 (m)
    // The READMIT seed.
    double seed_value{0.0}, seed_var{0.0}, seed_gap_s{0.0};
    // The handover fold.
    bool   sts_applied{false};
    double sts_c0{0.0}, sts_var_q{0.0};
    double park_moved{0.0};                      // how far the kernel moved a parked clock (m)
    std::vector<Key> hand_keys;                  // the non-pivot clocks of the dense prior
    Mat    hand_prior_prepin, hand_prior_postpin, hand_prior_exact_own;
};

CandOut runCandidate(const Net& n, const CandOpts& o) {
    CandOut out;
    out.ep.resize(kK + 1);
    // The carried state after the previous epoch.
    std::vector<Key> bprev;
    Vec sprev;
    Mat Pprev;
    std::vector<Key> ak;     // carried arcs (rows), in order
    Vec nhat;                // the rows' values
    Mat G;                   // the regression on bprev: N = nhat + G (s - sprev) + e
    Mat C;                   // the conditional covariance (cross-station zero unless joint)
    std::map<Key, double> var_first;
    struct Park { int s; double value, var, t; };
    std::vector<Park> parks;
    // Under the drop the slipped arc keeps its identity (generation 0) while the truth slips.
    const auto candKey = [&](Key key) {
        if (key.kind == Kind::Amb && o.slip == SlipResp::Drop) key.seg = 0;
        return key;
    };
    for (int k = 1; k <= kK; ++k) {
        EpochInfo ei = epochInfo(n, k);
        for (Key& key : ei.keys) key = candKey(key);
        const std::vector<Key> b = borderKeys(ei);
        const bool slip_now = n.ev.slip && k == kSlipK;
        if (slip_now && o.slip == SlipResp::Erase) {
            // The slipped arc's row is deleted: the marginal of the other rows is the sub-block.
            std::vector<int> keep;
            for (int i = 0; i < static_cast<int>(ak.size()); ++i)
                if (!(ak[i].r == kSlipR && ak[i].s == kSlipS)) keep.push_back(i);
            std::vector<Key> nk;
            Vec nn(static_cast<Eigen::Index>(keep.size()));
            Mat nG(static_cast<Eigen::Index>(keep.size()), G.cols());
            Mat nC(static_cast<Eigen::Index>(keep.size()), static_cast<Eigen::Index>(keep.size()));
            for (int a = 0; a < static_cast<int>(keep.size()); ++a) {
                nk.push_back(ak[keep[a]]);
                nn(a) = nhat(keep[a]);
                nG.row(a) = G.row(keep[a]);
                for (int c = 0; c < static_cast<int>(keep.size()); ++c) nC(a, c) = C(keep[a], keep[c]);
            }
            ak = nk; nhat = nn; G = nG; C = nC;
        }
        // Unknowns: the border, the carried arcs, then the arcs new this epoch.
        std::vector<Key> arcs = ak;
        std::map<Key, int> col;
        for (int i = 0; i < static_cast<int>(b.size()); ++i) col[b[i]] = i;
        const int nb = static_cast<int>(b.size()), nca = static_cast<int>(ak.size());
        for (const Key& key : ei.keys)
            if (key.kind == Kind::Amb && std::find(arcs.begin(), arcs.end(), key) == arcs.end())
                arcs.push_back(key);
        const int na = static_cast<int>(arcs.size()), nx = nb + na;
        for (int i = 0; i < na; ++i) col[arcs[i]] = nb + i;
        Mat L = Mat::Zero(nx, nx);
        Vec e = Vec::Zero(nx);
        if (k > 1) {
            int np = static_cast<int>(bprev.size());
            // (1) A clock whose satellite left the layout is parked at the last stash.
            for (int p = 0; p < np; ++p)
                if (bprev[p].kind == Kind::Clk && !n.satPresent(k, bprev[p].s) && o.readmit_seed)
                    parks.push_back(Park{bprev[p].s, sprev(p), Pprev(p, p), Net::time(k - 1)});
            // (1b) The dense kernel, for the two forms that use it.
            const bool handover_epoch = n.handoverAt(k);
            DenseStash dz;
            if (handover_epoch && o.handover != HandoverForm::DiagonalAfterRows) {
                dz = denseTransform(bprev, sprev, Pprev, parks, kNewPiv, kOldPiv);
                out.sts_applied = dz.res.applied;
                out.sts_c0 = dz.res.c0;
                out.sts_var_q = dz.res.pqq;
                for (std::size_t a = 0; a < parks.size(); ++a) {
                    out.park_moved = dz.parks[a].first - parks[a].value;
                    parks[a].value = dz.parks[a].first;
                    parks[a].var   = dz.parks[a].second;
                }
            }
            if (handover_epoch && o.handover == HandoverForm::ExactRows) {
                // The stash re-expressed in the incoming frame: the incoming pivot's row leaves,
                // the outgoing pivot's (departed) joins; each row's clock terms follow
                // x_j = x'_j - x'_p, so the outgoing pivot takes minus their sum.
                EXPECT_TRUE(dz.p_added);
                std::vector<int> src;
                for (int p = 0; p < np; ++p)
                    if (!(bprev[p] == clkKey(kNewPiv))) src.push_back(p);
                src.push_back(np);   // the appended outgoing pivot
                const int n2 = static_cast<int>(src.size());
                std::vector<Key> b2;
                Vec s2(n2);
                Mat P2(n2, n2), G2 = Mat::Zero(G.rows(), n2);
                for (int a = 0; a < n2; ++a) {
                    b2.push_back(a + 1 < n2 ? bprev[src[a]] : clkKey(kOldPiv));
                    s2(a) = dz.val[static_cast<std::size_t>(src[a])];
                    for (int c = 0; c < n2; ++c) P2(a, c) = dz.P(src[a], src[c]);
                    if (a + 1 < n2) G2.col(a) = G.col(src[a]);
                }
                for (int p = 0; p < np; ++p)
                    if (bprev[p].kind == Kind::Clk) G2.col(n2 - 1) -= G.col(p);
                bprev = b2; sprev = s2; Pprev = P2; G = G2;
                np = n2;
            }
            // (2) The rows' epoch over the stash as it was harvested (built before any handover
            // transform): S = the stashed columns present now, a pinned incoming pivot included
            // (its column known), at the fold's inflation; D = the departed ones.
            CouplingExactEpoch xe;
            xe.P = Pprev;
            xe.s_of_p.assign(np, -1);
            xe.route_p.assign(np, static_cast<int>(CouplingExactDRoute::Departed));
            xe.clk_p.assign(np, 0);
            xe.cc_p.assign(np, 'G');
            std::vector<double> qs, sh;
            for (int p = 0; p < np; ++p) {
                const Key& key = bprev[p];
                xe.clk_p[p] = key.kind == Kind::Clk ? 1 : 0;
                Eigen::Index c = -2;   // -2: departed; -1: present and known (the pinned pivot)
                if (key.kind == Kind::Clk) {
                    if (n.satPresent(k, key.s)) c = (key.s == n.pivot(k)) ? -1 : col.at(key);
                } else if (col.count(key)) {
                    c = col.at(key);
                }
                if (c == -2) continue;
                xe.route_p[p] = static_cast<int>(CouplingExactDRoute::Present);
                xe.s_of_p[p] = static_cast<int>(xe.p_of_s.size());
                xe.p_of_s.push_back(p);
                xe.col_of_s.push_back(c);
                qs.push_back(key.kind == Kind::Clk ? kQClk * kDt : 0.0);
                sh.push_back(sprev(p));
            }
            const Eigen::Index nS = static_cast<Eigen::Index>(qs.size());
            xe.q_s = Eigen::Map<Vec>(qs.data(), nS);
            xe.shat_s = Eigen::Map<Vec>(sh.data(), nS);
            EXPECT_TRUE(couplingExactFactor(xe)) << "epoch " << k;
            // (3) The carried rows.
            // The rows meet the old frame's departed pivot unless they were re-expressed first.
            const bool handover = handover_epoch && o.handover != HandoverForm::ExactRows;
            if (handover_epoch) EXPECT_FALSE(n.satPresent(k, n.pivot(k - 1)))
                << "the fixture's handover has the outgoing pivot leave the layout";
            Mat A = Mat::Zero(nca, nx);
            Vec y = Vec::Zero(nca);
            Mat Sig = C;
            std::vector<CouplingExactRow> xr(static_cast<std::size_t>(nca));
            std::vector<char> dropped(static_cast<std::size_t>(nca), 0);
            for (int i = 0; i < nca; ++i) {
                A(i, col.at(arcs[i])) = 1.0;
                if (slip_now && o.slip == SlipResp::Drop && ak[i].r == kSlipR && ak[i].s == kSlipS) {
                    // The float vehicle's drop, on the arc's carry as the producer holds it.
                    LocalCarry lc{};
                    lc.value_abs = nhat(i);
                    lc.var       = C(i, i);
                    lc.var_first = var_first.at(ak[i]);
                    lc.var_birth = lc.var_first;          // the birth floor is 0 on the goldens
                    lc.coupling.push_back(CoupledShared{});
                    const AmbDropOutcome d = ambFloatDropFloored(lc, 0.0, 0.0, Net::time(k), k,
                                                                 /*decouple_always=*/true);
                    EXPECT_TRUE(lc.coupling.empty()) << "the drop clears the border regression";
                    EXPECT_EQ(lc.value_abs, nhat(i)) << "the drop keeps the carried value";
                    if (d.acted) ++out.drops_acted;
                    if (ak[i].j == 0) {
                        out.drop_var_before     = C(i, i);
                        out.drop_var_restored   = lc.var;
                        out.drop_value_kept_err = lc.value_abs - n.N[kSlipR][kSlipS][0][1];
                    }
                    dropped[i] = 1;
                    y(i) = lc.value_abs;
                    for (int j = 0; j < nca; ++j) Sig(i, j) = Sig(j, i) = 0.0;
                    Sig(i, i) = lc.var;
                    continue;
                }
                std::vector<std::pair<int, double>> terms;
                double m_piv = 0.0;   // the pinned pivot's term: the row blind to the clock gauge
                for (int p = 0; p < np; ++p)
                    if (G(i, p) != 0.0) {
                        terms.emplace_back(p, -G(i, p));
                        if (bprev[p].kind == Kind::Clk) m_piv += G(i, p);
                    }
                xr[i] = couplingExactRow(xe, terms);
                EXPECT_TRUE(xr[i].ok);
                y(i) = nhat(i);
                for (Eigen::Index s = 0; s < nS; ++s) {
                    const double m = xr[i].m_new(s);
                    if (xe.col_of_s[s] >= 0) A(i, xe.col_of_s[s]) += m;
                    y(i) += m * xe.shat_s(s);
                }
                if (handover) {
                    // The stash's pivot left the layout: its term and the update's clock-family
                    // deficit are spread evenly over the S clocks (exactRowIngredients).
                    const auto it = xr[i].clk_deficit.find('G');
                    const double d = m_piv + (it != xr[i].clk_deficit.end() ? it->second : 0.0);
                    std::vector<Eigen::Index> cs;
                    for (Eigen::Index s = 0; s < nS; ++s)
                        if (xe.clk_p[xe.p_of_s[s]]) cs.push_back(s);
                    EXPECT_FALSE(cs.empty());
                    const double share = d / static_cast<double>(cs.size());
                    for (const Eigen::Index s : cs) {
                        if (xe.col_of_s[s] >= 0) A(i, xe.col_of_s[s]) += share;
                        y(i) += share * xe.shat_s(s);
                    }
                }
                // With the pivot unchanged the term multiplies its pinned 0 and adds nothing.
            }
            for (int i = 0; i < nca; ++i)
                for (int j = 0; j < nca; ++j) {
                    if (dropped[i] || dropped[j]) continue;
                    if (o.joint) Sig(i, j) += couplingExactCross(xe, xr[i], xr[j]);
                    else if (i == j) Sig(i, i) += xr[i].var_add;
                }
            if (nca > 0) {
                const Mat W = spdInverse(Sig);
                L += A.transpose() * W * A;
                e += A.transpose() * W * y;
            }
            // (4) The border fold over the carried keys present now (the dense branch): the
            // carried values and variances, re-expressed by the shipped kernel at a handover.
            std::vector<double> val(np), var(np);
            for (int p = 0; p < np; ++p) { val[p] = sprev(p); var[p] = Pprev(p, p); }
            const Mat* Pfold = &Pprev;
            if (handover && o.handover == HandoverForm::DenseAfterRows) {
                for (int p = 0; p < np; ++p) { val[p] = dz.val[p]; var[p] = dz.P(p, p); }
                Pfold = &dz.P;
            }
            if (handover && o.handover == HandoverForm::DiagonalAfterRows) {
                NetworkCarryState carry;
                net::NetworkEntities ent;
                for (int p = 0; p < np; ++p) {
                    CarryPrior cp{};
                    cp.kind   = bprev[p].kind == Kind::Clk ? SharedParamKind::Clock : SharedParamKind::PhaseBias;
                    cp.sat    = satName(bprev[p].s);
                    cp.signal = bprev[p].kind == Kind::Clk ? "" : sigName(bprev[p].j);
                    cp.value  = val[p];
                    cp.var    = var[p];
                    carry.prev_carry.push_back(cp);
                }
                for (const Park& pk : parks) {
                    CarryPrior cp{};
                    cp.kind     = SharedParamKind::Clock;
                    cp.sat      = satName(pk.s);
                    cp.value    = pk.value;
                    cp.var      = pk.var;
                    cp.t_park_s = pk.t;
                    carry.prev_carry.push_back(cp);
                }
                std::vector<std::string> registered;
                for (int s = 0; s < kS; ++s)
                    if (n.satPresent(k, s)) registered.push_back(satName(s));
                const auto v_c = NetworkSbasis::clockNullDirection(registered, "G");
                const ClockStransformResult r =
                    applyClockStransform(carry, ent, v_c, satName(kNewPiv), o.sts_mech);
                out.sts_applied = r.applied;
                out.sts_c0 = r.c0;
                out.sts_var_q = r.var_q;
                EXPECT_TRUE(r.selfcheck_ok);
                for (int p = 0; p < np; ++p) {
                    val[p] = carry.prev_carry[static_cast<std::size_t>(p)].value;
                    var[p] = carry.prev_carry[static_cast<std::size_t>(p)].var;
                }
                for (std::size_t a = 0; a < parks.size(); ++a) {
                    const CarryPrior& cp = carry.prev_carry[static_cast<std::size_t>(np) + a];
                    out.park_moved = cp.value - parks[a].value;
                    parks[a].value = cp.value;
                    parks[a].var   = cp.var;
                }
            }
            std::vector<int> dk;           // the folded P rows (S), the pinned pivot included
            for (Eigen::Index s = 0; s < nS; ++s) dk.push_back(xe.p_of_s[s]);
            const int kd = static_cast<int>(dk.size());
            if (kd > 0) {
                Mat cov(kd, kd);
                Vec v(kd);
                for (int a = 0; a < kd; ++a) {
                    v(a) = val[dk[a]];
                    for (int c = 0; c < kd; ++c) cov(a, c) = (*Pfold)(dk[a], dk[c]);
                    cov(a, a) = var[dk[a]] + xe.q_s(a);
                }
                Mat Lam = spdInverse(cov);
                if (!o.fold_pinned_pivot) {
                    // The pinned column's prior marginalized instead of conditioned on its pin.
                    std::vector<int> fr;
                    for (int a = 0; a < kd; ++a)
                        if (xe.col_of_s[a] >= 0) fr.push_back(a);
                    Mat cf(static_cast<Eigen::Index>(fr.size()), static_cast<Eigen::Index>(fr.size()));
                    for (std::size_t a = 0; a < fr.size(); ++a)
                        for (std::size_t c = 0; c < fr.size(); ++c) cf(a, c) = cov(fr[a], fr[c]);
                    const Mat lf = spdInverse(cf);
                    Lam.setZero();
                    for (std::size_t a = 0; a < fr.size(); ++a)
                        for (std::size_t c = 0; c < fr.size(); ++c) Lam(fr[a], fr[c]) = lf(a, c);
                }
                const Vec eta = Lam * v;
                // The pinned incoming pivot is known at 0: the prior conditioned on its pin keeps
                // the other columns' block of the information and its right-hand side.
                for (int a = 0; a < kd; ++a) {
                    const Eigen::Index ca = xe.col_of_s[a];
                    if (ca < 0) continue;
                    e(ca) += eta(a);
                    for (int c = 0; c < kd; ++c)
                        if (xe.col_of_s[c] >= 0) L(ca, xe.col_of_s[c]) += Lam(a, c);
                }
                if (handover) {
                    // The handover instrument: the folded clock block before and after the pin,
                    // and the exact re-expression of the candidate's own carried block.
                    std::vector<int> ci, oi;      // positions in dk of the non-pivot clocks; of all known-free
                    int iq = -1;
                    for (int a = 0; a < kd; ++a) {
                        const Key& key = bprev[dk[a]];
                        if (xe.col_of_s[a] < 0) { iq = a; continue; }
                        oi.push_back(a);
                        if (key.kind == Kind::Clk) { ci.push_back(a); out.hand_keys.push_back(key); }
                    }
                    EXPECT_GE(iq, 0);
                    Mat Loo(static_cast<Eigen::Index>(oi.size()), static_cast<Eigen::Index>(oi.size()));
                    for (std::size_t a = 0; a < oi.size(); ++a)
                        for (std::size_t c = 0; c < oi.size(); ++c) Loo(a, c) = Lam(oi[a], oi[c]);
                    const Mat post = spdInverse(Loo);
                    const int nh = static_cast<int>(ci.size());
                    out.hand_prior_prepin.resize(nh, nh);
                    out.hand_prior_postpin.resize(nh, nh);
                    out.hand_prior_exact_own.resize(nh, nh);
                    for (int a = 0; a < nh; ++a)
                        for (int c = 0; c < nh; ++c) {
                            out.hand_prior_prepin(a, c) = cov(ci[a], ci[c]);
                            const auto pa = std::find(oi.begin(), oi.end(), ci[a]) - oi.begin();
                            const auto pc = std::find(oi.begin(), oi.end(), ci[c]) - oi.begin();
                            out.hand_prior_postpin(a, c) = post(pa, pc);
                            const int ia = dk[ci[a]], ic = dk[ci[c]], qq = dk[iq];
                            out.hand_prior_exact_own(a, c) = Pprev(ia, ic) - Pprev(ia, qq)
                                                           - Pprev(qq, ic) + Pprev(qq, qq)
                                                           + (a == c ? kQClk * kDt : 0.0);
                        }
                }
            }
            // (5) The READMIT seed: a parked clock whose column is back folds on the diagonal at
            // its variance plus q over the gap since the park.
            for (std::size_t a = 0; a < parks.size();) {
                if (!n.satPresent(k, parks[a].s)) { ++a; continue; }
                const double gap = Net::time(k) - parks[a].t;
                const double v_eff = parks[a].var + kQClk * gap;
                const int c = col.at(clkKey(parks[a].s));
                L(c, c) += 1.0 / v_eff;
                e(c) += parks[a].value / v_eff;
                out.seed_value = parks[a].value;
                out.seed_var = v_eff;
                out.seed_gap_s = gap;
                parks.erase(parks.begin() + static_cast<std::ptrdiff_t>(a));
            }
        }
        // (6) The epoch's rows.
        for (int a = 0; a < static_cast<int>(ei.keys.size()); ++a) {
            const int ia = col.at(ei.keys[a]);
            e(ia) += ei.e(a);
            for (int c = 0; c < static_cast<int>(ei.keys.size()); ++c) L(ia, col.at(ei.keys[c])) += ei.L(a, c);
        }
        const Mat Sx = spdInverse(L);
        const Vec x = Sx * e;
        // (7) The harvest: the border marginal and the arcs' conditional on it.
        bprev = b;
        sprev = x.head(nb);
        Pprev = Sx.topLeftCorner(nb, nb);
        const Mat Pinv = spdInverse(Pprev);
        const Mat Sab = Sx.block(nb, 0, na, nb);
        G = Sab * Pinv;
        C = Sx.bottomRightCorner(na, na) - Sab * Pinv * Sab.transpose();
        if (!o.joint)
            for (int i = 0; i < na; ++i)
                for (int j = 0; j < na; ++j)
                    if (arcs[i].r != arcs[j].r) C(i, j) = 0.0;
        nhat = x.tail(na);
        ak = arcs;
        for (int i = nca; i < na; ++i) var_first[arcs[i]] = C(i, i);   // the first harvest
        out.ep[k] = EpochBorder{b, sprev, Pprev};
    }
    return out;
}

// ---------------------------------------------------------------------------
// The instruments.
// ---------------------------------------------------------------------------

struct EpochCmp {
    double maxabs{0.0};       // max |candidate - reference| over the border (m)
    double z{0.0};            // max |d_i| / sigma_ref,i
    double nees{0.0};         // d' P_ref^-1 d
    int    nb{0};
    double sig_min{1e300}, sig_max{0.0};   // sigma_cand / sigma_ref over the columns
    double cov_rel{0.0};      // max |P_cand - P_ref| / sqrt(P_ref,ii P_ref,jj)
    Key    key_z{}, key_sig_min{}, key_sig_max{};   // the columns that set z and the ratios
};

/// A column's name for the printed tables: "clk s2", "pb s3 L1" (segment 1 marked "'").
std::string keyName(const Key& k) {
    if (k.kind == Kind::Clk) return "clk s" + std::to_string(k.s);
    return std::string(k.kind == Kind::Pb ? "pb s" : "amb s") + std::to_string(k.s)
         + (k.j == 0 ? " L1" : " L2") + (k.seg > 0 ? "'" : "");
}

EpochCmp compareEpoch(const EpochBorder& c, const EpochBorder& r) {
    EpochCmp out;
    EXPECT_TRUE(c.keys == r.keys) << "border keys differ";
    if (!(c.keys == r.keys)) return out;
    const Vec d = c.x - r.x;
    out.nb = static_cast<int>(d.size());
    out.maxabs = d.cwiseAbs().maxCoeff();
    out.nees = d.dot(spdInverse(r.P) * d);
    for (Eigen::Index a = 0; a < d.size(); ++a) {
        const double z = std::abs(d(a)) / std::sqrt(r.P(a, a));
        if (z > out.z) { out.z = z; out.key_z = c.keys[static_cast<std::size_t>(a)]; }
        const double ratio = std::sqrt(c.P(a, a) / r.P(a, a));
        if (ratio < out.sig_min) { out.sig_min = ratio; out.key_sig_min = c.keys[static_cast<std::size_t>(a)]; }
        if (ratio > out.sig_max) { out.sig_max = ratio; out.key_sig_max = c.keys[static_cast<std::size_t>(a)]; }
        for (Eigen::Index b = 0; b < d.size(); ++b)
            out.cov_rel = std::max(out.cov_rel, std::abs(c.P(a, b) - r.P(a, b))
                                                    / std::sqrt(r.P(a, a) * r.P(b, b)));
    }
    return out;
}

/// The worst of the instruments over epochs [k0, k1].
struct WindowCmp {
    double maxabs{0.0}, z{0.0}, nees{0.0}, sig_min{1e300}, sig_max{0.0}, cov_rel{0.0};
    int    k_maxabs{0};
};

WindowCmp compareWindow(const CandOut& c, const RefOut& r, int k0, int k1) {
    WindowCmp w;
    for (int k = k0; k <= k1; ++k) {
        const EpochCmp e = compareEpoch(c.ep[k], r.ep[k]);
        if (e.maxabs > w.maxabs) { w.maxabs = e.maxabs; w.k_maxabs = k; }
        w.z = std::max(w.z, e.z);
        w.nees = std::max(w.nees, e.nees);
        w.sig_min = std::min(w.sig_min, e.sig_min);
        w.sig_max = std::max(w.sig_max, e.sig_max);
        w.cov_rel = std::max(w.cov_rel, e.cov_rel);
    }
    return w;
}

/// The largest departure of a column's sigma ratio (candidate / reference) from 1.
double sigDev(const WindowCmp& w) { return std::max(1.0 - w.sig_min, w.sig_max - 1.0); }

void printWindow(const char* tag, const char* what, const WindowCmp& w) {
    std::printf("[CRO] %-8s %-24s max|d| %.3e m (epoch %d)  z %.4f  NEES %.4e  sigma ratio "
                "%.4f..%.4f (|dev| %.3e)\n", tag, what, w.maxabs, w.k_maxabs, w.z, w.nees,
                w.sig_min, w.sig_max, sigDev(w));
}

/// Measured bounds of one window, each stated as a number at the call, no looser than twice
/// the value measured on 2026-11-07 (card CRO, section 8).
struct Bounds {
    double maxabs;    ///< max |candidate - reference| over the border and the window (m)
    double nees;      ///< the largest NEES of an epoch
    double z;         ///< the largest |d_i| / sigma_ref,i
    double sig_dev;   ///< the largest |sigma_cand / sigma_ref - 1|
};

void expectWindow(const char* tag, const char* what, const WindowCmp& w, const Bounds& b) {
    printWindow(tag, what, w);
    EXPECT_LT(w.maxabs, b.maxabs) << tag << ", " << what << ": max|d|";
    EXPECT_LT(w.nees, b.nees) << tag << ", " << what << ": NEES";
    EXPECT_LT(w.z, b.z) << tag << ", " << what << ": z";
    EXPECT_LT(sigDev(w), b.sig_dev) << tag << ", " << what << ": sigma ratio";
}

/// The per-epoch table over [k0, k1] (the card's by-epoch rows).
void printEpochs(const char* tag, const CandOut& c, const RefOut& r, int k0, int k1) {
    for (int k = k0; k <= k1; ++k) {
        const EpochCmp e = compareEpoch(c.ep[k], r.ep[k]);
        std::printf("[CRO] %-8s epoch %2d  n_b %2d  max|d| %.3e m  z %.4f (%s)  NEES %.4e  sigma "
                    "ratio %.4f (%s) .. %.4f (%s)\n", tag, k, e.nb, e.maxabs, e.z,
                    keyName(e.key_z).c_str(), e.nees, e.sig_min, keyName(e.key_sig_min).c_str(),
                    e.sig_max, keyName(e.key_sig_max).c_str());
    }
}

/// The reference and the candidate on one scenario, with the baseline's candidate for the
/// increments.
struct Scenario {
    Net net;
    RefOut ref;
    CandOut cand;
};

Scenario runScenario(const Events& ev, const CandOpts& o = {}) {
    Scenario s{makeNet(ev), {}, {}};
    s.ref = runReference(s.net);
    s.cand = runCandidate(s.net, o);
    return s;
}

}  // namespace

// ---------------------------------------------------------------------------
// The oracle itself.
// ---------------------------------------------------------------------------

// E1: the reference (the joint filter with each event handled exactly) equals the batch over
// epochs 1..k at the event epochs and the window's end, in every scenario.
TEST(CarryRecursionOracle, ReferenceEqualsTheBatchAcrossEveryEvent) {
    const std::vector<std::pair<const char*, Events>> scen{
        {"baseline", Events{}},
        {"A", Events{true, false, false, false}},
        {"B", Events{false, true, false, false}},
        {"C", Events{false, false, true, false}},
        {"D", Events{false, false, false, true}},
        {"ALL", Events{true, true, true, true}},
    };
    for (const auto& [tag, ev] : scen) {
        SCOPED_TRACE(tag);
        const Net n = makeNet(ev);
        const RefOut r = runReference(n);
        for (const int k : {kBirthK, kSlipK, kDepK0, kHandK, kDepK1 + 1, kK}) {
            const EpochBorder bt = runBatch(n, k);
            const EpochCmp e = compareEpoch(r.ep[k], bt);
            std::printf("[CRO] E1 %-8s epoch %2d  max|ref - batch| %.2e m, covariance %.2e\n",
                        tag, k, e.maxabs, e.cov_rel);
            EXPECT_LT(e.maxabs, 1e-8) << "epoch " << k;
            EXPECT_LT(e.cov_rel, 1e-8) << "epoch " << k;
        }
    }
}

// E2: the factorized recursion in its joint form (every row pair's time-update covariance and
// the cross-station harvest covariance kept), with a slip answered by erase and rebirth, is the
// reference: the exact coupling update is exact through arc births and deaths, at every epoch.
TEST(CarryRecursionOracle, FactorizedJointRecursionIsExactThroughBirthsAndErasures) {
    CandOpts joint;
    joint.joint = true;
    joint.slip = SlipResp::Erase;
    const std::vector<std::pair<const char*, Events>> scen{
        {"baseline", Events{}},
        {"A", Events{true, false, false, false}},
        {"B", Events{false, true, false, false}},
        {"A+B", Events{true, true, false, false}},
    };
    for (const auto& [tag, ev] : scen) {
        SCOPED_TRACE(tag);
        const Scenario s = runScenario(ev, joint);
        const WindowCmp w = compareWindow(s.cand, s.ref, 1, kK);
        std::printf("[CRO] E2 %-8s max|joint factorized - reference| %.2e m, covariance %.2e\n",
                    tag, w.maxabs, w.cov_rel);
        EXPECT_LT(w.maxabs, 1e-8);
        EXPECT_LT(w.cov_rel, 1e-8);
    }
}


// The steady residue of the shipped per-station form on the event-free scenario: the
// cross-station time-update and harvest covariances it drops (cards CTU section 4, WSX).
// Measured: 2.70 mm at epoch 60 and growing slowly with the epochs, NEES 1.01 against
// n_b = 14, every sigma within 6.9e-5 of the reference's.
TEST(CarryRecursionOracle, ShippedFormSteadyResidue) {
    const Scenario s = runScenario(Events{});
    printEpochs("baseline", s.cand, s.ref, 1, 6);
    printEpochs("baseline", s.cand, s.ref, 58, 60);
    expectWindow("baseline", "epochs 1..60", compareWindow(s.cand, s.ref, 1, kK),
                 Bounds{4.0e-3, 1.5, 0.043, 1.0e-4});
}

// Event A: an arc born at epoch 20.  A birth carries no prior, so the shipped form differs
// from the reference by its steady residue only.  Measured over epochs 20..25: 0.674 mm
// against the baseline's 0.655 mm, NEES 0.057.
TEST(CarryRecursionOracle, ArcBirth) {
    const Scenario base = runScenario(Events{});
    const Scenario s = runScenario(Events{true, false, false, false});
    printEpochs("A", s.cand, s.ref, kBirthK - 1, kBirthK + 5);
    printWindow("baseline", "epochs 20..25", compareWindow(base.cand, base.ref, kBirthK, kBirthK + 5));
    expectWindow("A", "event epochs 20..25", compareWindow(s.cand, s.ref, kBirthK, kBirthK + 5),
                 Bounds{1.0e-3, 0.085, 0.0069, 2.8e-5});
    expectWindow("A", "epochs 1..60", compareWindow(s.cand, s.ref, 1, kK),
                 Bounds{4.0e-3, 1.6, 0.043, 1.05e-4});
}

// Event B: a slip at epoch 30 answered by the vehicle's drop (the pre-slip mean kept, the
// variance restored to the first harvest's).  The shipped operator acts on both bands and keeps
// the value; the restored L1 variance is 0.493 m^2 on a kept value 1.165 m from the post-slip
// truth.  Measured: the border moves 5.6 cm (z 0.24, the slipped satellite's L2 phase bias) in
// the six epochs from the slip and still 4.0 cm (z 0.22) over epochs 46..60, while the same
// per-station form with erase and rebirth stays at the steady residue (1.0 mm): the kept mean,
// not the per-station form, is the departure.
TEST(CarryRecursionOracle, SlipDropKeepsThePreSlipMean) {
    const Scenario base = runScenario(Events{});
    const Scenario s = runScenario(Events{false, true, false, false});
    std::printf("[CRO] B drop: acted %d, L1 arc var %.4e -> %.4e m^2, kept value minus the "
                "post-slip truth %+.4f m\n", s.cand.drops_acted, s.cand.drop_var_before,
                s.cand.drop_var_restored, s.cand.drop_value_kept_err);
    EXPECT_EQ(s.cand.drops_acted, 2) << "both bands' arcs dropped";
    EXPECT_GT(s.cand.drop_var_restored, 10.0 * s.cand.drop_var_before)
        << "the drop restores the birth variance";
    printEpochs("B", s.cand, s.ref, kSlipK - 1, kSlipK + 5);
    printWindow("baseline", "epochs 30..35", compareWindow(base.cand, base.ref, kSlipK, kSlipK + 5));
    expectWindow("B", "event epochs 30..35", compareWindow(s.cand, s.ref, kSlipK, kSlipK + 5),
                 Bounds{8.4e-2, 0.45, 0.36, 1.8e-2});
    printWindow("B", "epochs 36..45", compareWindow(s.cand, s.ref, kSlipK + 6, kSlipK + 15));
    expectWindow("B", "epochs 46..60", compareWindow(s.cand, s.ref, kSlipK + 16, kK),
                 Bounds{6.0e-2, 1.7, 0.32, 1.35e-2});
    // The control: the same per-station form with the slip answered by erase and rebirth.
    CandOpts erase;
    erase.slip = SlipResp::Erase;
    const CandOut ce = runCandidate(s.net, erase);
    expectWindow("Berase", "event epochs 30..35", compareWindow(ce, s.ref, kSlipK, kSlipK + 5),
                 Bounds{1.5e-3, 0.30, 0.012, 5.3e-5});
}

// Event C: satellite 2 absent over epochs 40..49, its clock READMIT-seeded at the return at
// its parked variance plus q over the 330 s gap.  Measured: the departure itself (the rows' D
// route) stays at the steady residue (1.96 mm); from the return the border differs by up to
// 6.6 cm over epochs 50..55 and 10.9 cm at epoch 57 (z 0.57, the returning satellite's new
// phase bias), with that column's sigma up to 1.41 times the reference's: the seed drops the
// parked clock's covariance with the rest and the gap's smoothing of its value, an information
// loss on the conservative side.  Without the seed (the clock fresh) the return misses the
// reference by 2.07 m (z 8.8): the seed removes about 97 % of it.
TEST(CarryRecursionOracle, DepartureAndReadmitSeed) {
    const Scenario base = runScenario(Events{});
    const Scenario s = runScenario(Events{false, false, true, false});
    std::printf("[CRO] C seed: value %+.6f m, variance %.4e m^2 over a %.0f s gap\n",
                s.cand.seed_value, s.cand.seed_var, s.cand.seed_gap_s);
    EXPECT_DOUBLE_EQ(s.cand.seed_gap_s, (kDepK1 + 1 - (kDepK0 - 1)) * kDt);
    printEpochs("C", s.cand, s.ref, kDepK0 - 1, kDepK0 + 2);
    printEpochs("C", s.cand, s.ref, kDepK1, kK);
    printWindow("baseline", "epochs 50..55", compareWindow(base.cand, base.ref, kDepK1 + 1, kDepK1 + 6));
    expectWindow("C", "departure 40..45", compareWindow(s.cand, s.ref, kDepK0, kDepK0 + 5),
                 Bounds{2.9e-3, 1.6, 0.027, 1.2e-4});
    const WindowCmp ret = compareWindow(s.cand, s.ref, kDepK1 + 1, kDepK1 + 6);
    expectWindow("C", "return 50..55", ret, Bounds{9.9e-2, 4.5, 0.49, 0.61});
    expectWindow("C", "epochs 56..60", compareWindow(s.cand, s.ref, kDepK1 + 7, kK),
                 Bounds{0.16, 3.8, 0.85, 0.51});
    // The control: no READMIT park, the returning clock fresh.
    CandOpts fresh;
    fresh.readmit_seed = false;
    const CandOut cf = runCandidate(s.net, fresh);
    printEpochs("Cfresh", cf, s.ref, kDepK1 + 1, kDepK1 + 3);
    const WindowCmp retf = compareWindow(cf, s.ref, kDepK1 + 1, kDepK1 + 6);
    printWindow("Cfresh", "return 50..55", retf);
    // Measured ratio 0.0318: the seeded return's largest difference over the fresh one's.
    EXPECT_LT(ret.maxabs, 0.05 * retf.maxabs) << "the seed should remove most of the fresh return's miss";
}

// Event D: the pivot sets at epoch 45 and the pivot moves to satellite 1, through the rows built
// before the transform and the shipped diagonal kernel.  E3 (exact): inside the candidate the
// folded clock contrast exceeds the exact re-expression of its own block by 2 var_q (the
// header's dropped cross term).  Measured: every contrast of the dense prior is 3.00 times the
// reference's predicted variance (var_q is about one contrast here), the folded block states
// the incoming pivot's phase bias 15 % tight in sigma (ratio 0.852) with NEES 22.1 at the
// handover epoch, decaying to 3.1 by epoch 51 and to 0.37 by epoch 60, the column still 12 %
// tight; the border moves up to 3.5 cm (z 0.18).  The decomposition: the dense kernel under the
// same rows (network.handover_stransform_dense) brings the window to 3.5 mm, NEES 0.56, every
// sigma within 2.5e-4; the exact form (the rows re-expressed first) to 2.4 mm, NEES 1.13.  So
// the departure is the stale off-diagonals of the diagonal kernel's fold, not the rows' spread
// of the departed pivot's term; marginalizing the pinned pivot's prior instead of conditioning
// on its pin does not remove it (printed as Dnopin).
TEST(CarryRecursionOracle, ClockPivotHandoverDiagonalKernel) {
    const Scenario base = runScenario(Events{});
    const Scenario s = runScenario(Events{false, false, false, true});
    ASSERT_TRUE(s.cand.sts_applied);
    std::printf("[CRO] D kernel: c0 %+.6f m, var_q %.4e m^2\n", s.cand.sts_c0, s.cand.sts_var_q);
    const CandOut& c = s.cand;
    const int nh = static_cast<int>(c.hand_keys.size());
    EXPECT_EQ(nh, 3) << "the non-pivot clocks of the dense prior at the handover";
    std::map<Key, int> ri;
    for (int i = 0; i < static_cast<int>(s.ref.hand_keys.size()); ++i) ri[s.ref.hand_keys[i]] = i;
    for (int a = 0; a < nh; ++a)
        for (int b = a + 1; b < nh; ++b) {
            const auto con = [&](const Mat& P, int i, int j) { return P(i, i) + P(j, j) - 2.0 * P(i, j); };
            const int ra = ri.at(c.hand_keys[a]), rb = ri.at(c.hand_keys[b]);
            const double ref = con(s.ref.hand_P, ra, rb);
            const double pre = con(c.hand_prior_prepin, a, b), post = con(c.hand_prior_postpin, a, b);
            const double own = con(c.hand_prior_exact_own, a, b);
            std::printf("[CRO] D contrast s%d - s%d: reference %.4e m^2; candidate prior %.4e "
                        "(ratio %.4f) before the pin, %.4e (ratio %.4f) after; excess over its own "
                        "exact block %.4e = %.6f var_q\n", c.hand_keys[a].s, c.hand_keys[b].s, ref,
                        pre, pre / ref, post, post / ref, pre - own, (pre - own) / c.sts_var_q);
            // E3: the documented over-statement, exact.
            EXPECT_NEAR(pre - own, 2.0 * c.sts_var_q, 1e-10 * c.sts_var_q);
            // Measured 2.9996 before and after the pin: over-stated (conservative), bounded.
            EXPECT_GT(post / ref, 1.0);
            EXPECT_LT(post / ref, 3.5);
        }
    printEpochs("D", s.cand, s.ref, kHandK - 1, kK);
    printWindow("baseline", "epochs 45..50", compareWindow(base.cand, base.ref, kHandK, kHandK + 5));
    expectWindow("D", "event epochs 45..50", compareWindow(s.cand, s.ref, kHandK, kHandK + 5),
                 Bounds{5.2e-2, 33.0, 0.27, 0.22});
    expectWindow("D", "epochs 51..60", compareWindow(s.cand, s.ref, kHandK + 6, kK),
                 Bounds{4.4e-2, 4.6, 0.24, 0.20});
    // The decomposition.
    struct Variant { const char* tag; HandoverForm form; bool fold_pin; Bounds ev, after; bool assert_it; };
    const std::vector<Variant> vars{
        {"Ddense", HandoverForm::DenseAfterRows, true,
         Bounds{5.2e-3, 0.83, 0.027, 3.8e-4}, Bounds{7.7e-3, 1.0, 0.042, 7.4e-4}, true},
        {"Dexact", HandoverForm::ExactRows, true,
         Bounds{3.6e-3, 1.7, 0.035, 2.1e-4}, Bounds{4.7e-3, 2.4, 0.047, 2.2e-4}, true},
        {"Dnopin", HandoverForm::DiagonalAfterRows, false, Bounds{}, Bounds{}, false},
    };
    for (const Variant& v : vars) {
        CandOpts f;
        f.handover = v.form;
        f.fold_pinned_pivot = v.fold_pin;
        const CandOut cv = runCandidate(s.net, f);
        printEpochs(v.tag, cv, s.ref, kHandK, kHandK + 2);
        const WindowCmp we = compareWindow(cv, s.ref, kHandK, kHandK + 5);
        const WindowCmp wa = compareWindow(cv, s.ref, kHandK + 6, kK);
        if (v.assert_it) {
            expectWindow(v.tag, "event epochs 45..50", we, v.ev);
            expectWindow(v.tag, "epochs 51..60", wa, v.after);
        } else {
            printWindow(v.tag, "event epochs 45..50", we);
            printWindow(v.tag, "epochs 51..60", wa);
        }
    }
}

// All four events together; satellite 2 is parked across the handover.  The golden posture's
// kernel (mechanism Handover) takes its membership from the registered clocks, so the parked
// clock stays in the outgoing frame and the READMIT seed at epoch 50 lands c0 = 0.371 m off at
// a sigma of 0.117 m: the border misses the reference by 0.68 m (z 3.05) at the return.  The
// kernel's wide traversal (mechanism HandoverBeforeRows, which reaches a parked clock of the
// constellation) moves it by -c0 and the return falls to 0.10 m (z 0.55), the size of event C
// alone; the exact handover form gives 0.093 m (NEES 0.93).  The two lines on the parked clock
// characterize the golden kernel's membership rule: a fix that walks the parked clock changes
// them and lowers the golden bounds below.
TEST(CarryRecursionOracle, AllEventsTogether) {
    const Scenario s = runScenario(Events{true, true, true, true});
    CandOpts wide;
    wide.sts_mech = CarryMechanism::HandoverBeforeRows;
    const CandOut cw = runCandidate(s.net, wide);
    CandOpts exact;
    exact.handover = HandoverForm::ExactRows;
    const CandOut cx = runCandidate(s.net, exact);
    std::printf("[CRO] ALL kernel: c0 %+.6f m, var_q %.4e m^2; parked clock moved %+.6f m "
                "(golden mechanism), %+.6f m (wide traversal), %+.6f m (dense kernel)\n",
                s.cand.sts_c0, s.cand.sts_var_q, s.cand.park_moved, cw.park_moved, cx.park_moved);
    std::printf("[CRO] ALL seed: value %+.6f m (wide %+.6f m), variance %.4e m^2\n",
                s.cand.seed_value, cw.seed_value, s.cand.seed_var);
    EXPECT_EQ(s.cand.park_moved, 0.0) << "the golden kernel leaves a parked clock in the old frame";
    EXPECT_NEAR(cw.park_moved, -cw.sts_c0, 1e-12) << "the wide traversal walks it by -c0";
    printEpochs("ALL", s.cand, s.ref, 29, 31);
    printEpochs("ALL", s.cand, s.ref, 44, 52);
    printEpochs("ALLwide", cw, s.ref, 49, 52);
    expectWindow("ALL", "before the return 1..49", compareWindow(s.cand, s.ref, 1, kDepK1),
                 Bounds{0.11, 25.0, 0.56, 0.22});
    expectWindow("ALL", "return 50..55", compareWindow(s.cand, s.ref, kDepK1 + 1, kDepK1 + 6),
                 Bounds{1.0, 22.0, 4.6, 0.53});
    expectWindow("ALLwide", "return 50..55", compareWindow(cw, s.ref, kDepK1 + 1, kDepK1 + 6),
                 Bounds{0.15, 4.7, 0.83, 0.95});
    expectWindow("ALLwide", "epochs 1..60", compareWindow(cw, s.ref, 1, kK),
                 Bounds{0.30, 25.0, 1.56, 0.95});
    expectWindow("ALLexact", "return 50..55", compareWindow(cx, s.ref, kDepK1 + 1, kDepK1 + 6),
                 Bounds{0.14, 1.4, 0.74, 1.0});
    expectWindow("ALLexact", "epochs 1..60", compareWindow(cx, s.ref, 1, kK),
                 Bounds{0.31, 2.8, 1.6, 1.0});
}
