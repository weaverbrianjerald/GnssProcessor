/**
 * @file test_handover_stransform_dense.cpp
 * @brief The dense clock-pivot S-transform of card SCL build B2
 *        (@c network.handover_stransform_dense,
 *        docs/refound/SCL_self_contained_clock_level_2026-10-03.md, sections 5 and 6, U2).
 *
 * At a clock-pivot handover the diagonal kernel moves the carried clock values and adds
 * var_q to each carried clock variance, and leaves the dense block in the outgoing frame.
 * The dense kernel re-expresses both by S P S^T with S = I - u e_q', u the constellation's
 * clock family.  The cases:
 *
 *  (a) the kernel equals the explicit S P+ S^T and S x+ of the block augmented with the
 *      outgoing pivot (value 0, zero row), entry by entry, to 1e-12;
 *  (b) the incoming pivot lands on exactly 0 with an exactly zero row and column;
 *  (c) every combination a with a'u = 0 keeps its variance, and the entries with u = 0 on
 *      both sides are bitwise unchanged;
 *  (d) a second call changes nothing (bitwise);
 *  (e) the result is positive semi-definite;
 *  (f) the diagonal kernel over-states a clock contrast by exactly 2 var_q (documented);
 *  (g) the not-applied paths change nothing;
 *  (h) the seam: with the dense knob off it runs the diagonal kernel and leaves the block
 *      bitwise unchanged; on, it records the edge for the clone fold and the anchor keep;
 *  (i) the loader: default off, the INERT rules and the refusals;
 *  (j) card SCL B1 across the dense handover: the clone fold's joint prior equals the
 *      explicit law of (S y + w, y_C), in information form.
 */

#include <gtest/gtest.h>

#include "app/NetworkRunContext.hpp"
#include "config/GnssConfig.hpp"
#include "filter/NetworkClockClone.hpp"
#include "filter/NetworkEstimator.hpp"
#include "filter/NetworkHandoverStransform.hpp"
#include "filter/NetworkRunTypes.hpp"
#include "filter/NetworkSbasis.hpp"
#include "logging/LogManager.hpp"
#include "logging/NetworkRunTraces.hpp"

#include <Eigen/Dense>

#include <spdlog/sinks/null_sink.h>
#include <spdlog/spdlog.h>

#include <cmath>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <random>
#include <set>
#include <string>
#include <vector>

using namespace GnssProcessor;
namespace fs = std::filesystem;

namespace {

/// The block's keys in row order: three GPS clocks (G02 the incoming pivot), two Galileo
/// clocks and three biases.  The outgoing pivot G01 was pinned at the stash, so it has no row.
struct Key { SharedParamKind kind; const char* sat; const char* sig; double value; };
const std::vector<Key> kKeys{
    {SharedParamKind::Clock,     "G02", "",    -2.0742503730608925},
    {SharedParamKind::Clock,     "G05", "",    -0.75},
    {SharedParamKind::Clock,     "E02", "",    +0.1443824781885379},
    {SharedParamKind::Clock,     "G32", "",    +1.25},
    {SharedParamKind::CodeBias,  "G02", "C1C", +0.031},
    {SharedParamKind::PhaseBias, "G05", "L1C", -0.480},
    {SharedParamKind::Clock,     "E03", "",    -0.50},
    {SharedParamKind::CodeBias,  "E03", "C1C", +0.012},
};
constexpr int kK = 8;       // block size before the transform
constexpr int kQ = 0;       // G02's row
const std::vector<int> kGpsClockRows{0, 1, 3};

/// P = A A' + 1e-6 I from a fixed seed, with a common clock component so the clocks are
/// positively correlated (the dominant case on the network).
Eigen::MatrixXd fixtureP() {
    std::mt19937 rng(7);
    std::normal_distribution<double> nd(0.0, 0.01);
    Eigen::MatrixXd A(kK, kK);
    for (int i = 0; i < kK; ++i)
        for (int j = 0; j < kK; ++j) A(i, j) = nd(rng);
    for (int i = 0; i < kK; ++i)
        if (kKeys[static_cast<std::size_t>(i)].kind == SharedParamKind::Clock) A(i, 0) += 0.03;
    Eigen::MatrixXd P = A * A.transpose();
    P += 1e-6 * Eigen::MatrixXd::Identity(kK, kK);
    return 0.5 * (P + P.transpose());
}

/// The carried state: one entry per block key at var = P_ii (its decision made, unmoved), and
/// a parked GPS clock G07 with no block row.
NetworkCarryState fixtureCarry() {
    NetworkCarryState c; net::NetworkEntities c_ent;
    const Eigen::MatrixXd P = fixtureP();
    c.prev_carry_cov.P = P;
    for (int i = 0; i < kK; ++i) {
        const Key& k = kKeys[static_cast<std::size_t>(i)];
        c.prev_carry_cov.row[sharedCarryKey(k.kind, k.sat, k.sig)] = static_cast<std::size_t>(i);
        CarryPrior cp;
        cp.kind   = k.kind;
        cp.sat    = k.sat;
        cp.signal = k.sig;
        cp.value  = k.value;
        cp.var    = P(i, i);
        cp.fold.disposition    = (k.kind == SharedParamKind::Clock) ? CarryDisposition::Anchored
                                                                    : CarryDisposition::Folded;
        cp.fold.value_decided_m = cp.value;
        cp.fold.var_decided_m2  = cp.var;
        c.prev_carry.push_back(cp);
    }
    CarryPrior parked;
    parked.kind = SharedParamKind::Clock;
    parked.sat = "G07";
    parked.value = 0.5;
    parked.var = 0.0625;
    parked.t_park_s = 1000.0;
    c.prev_carry.push_back(parked);
    return c;
}

const CarryPrior* entry(const NetworkCarryState& c, const std::string& sat,
                        SharedParamKind kind = SharedParamKind::Clock, const std::string& sig = "") {
    for (const CarryPrior& cp : c.prev_carry)
        if (cp.kind == kind && cp.sat == sat && cp.signal == sig) return &cp;
    return nullptr;
}

/// The explicit reference: the block augmented with G01 (value 0, zero row) as row kK, and
/// S = I - u e_q' with u = 1 on the GPS clocks (G01 included).
struct Reference {
    Eigen::MatrixXd Pplus, S, P_exp;
    Eigen::VectorXd x_plus, x_exp, u;
};
Reference reference() {
    Reference r;
    const Eigen::MatrixXd P = fixtureP();
    r.Pplus = Eigen::MatrixXd::Zero(kK + 1, kK + 1);
    r.Pplus.topLeftCorner(kK, kK) = P;
    r.x_plus = Eigen::VectorXd::Zero(kK + 1);
    for (int i = 0; i < kK; ++i) r.x_plus(i) = kKeys[static_cast<std::size_t>(i)].value;
    r.u = Eigen::VectorXd::Zero(kK + 1);
    for (int i : kGpsClockRows) r.u(i) = 1.0;
    r.u(kK) = 1.0;
    r.S = Eigen::MatrixXd::Identity(kK + 1, kK + 1);
    r.S.col(kQ) -= r.u;
    r.P_exp = r.S * r.Pplus * r.S.transpose();
    r.x_exp = r.S * r.x_plus;
    return r;
}

/// A minimal network config with the given network block.
struct Loaded {
    LogManager log;
    GnssConfig cfg;
    fs::path   path;
    bool       ok{false};
    Loaded(const std::string& tag, const std::string& network_extra) {
        const std::string body = std::string("{\n") +
            "  \"processing\": {\n"
            "    \"mode\": \"ppp-rtk-network\",\n"
            "    \"active_systems\": [ \"GPS\" ],\n"
            "    \"signal_priorities\": { \"GPS\": \"WC\" }\n"
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
        path = fs::temp_directory_path() / ("sts_dense_cfg_" + tag + ".json");
        { std::ofstream f(path); f << body; }
        ok = cfg.loadFromJson(path, log);
    }
    ~Loaded() { std::error_code ec; fs::remove(path, ec); }
};

const std::string kArmed =
    "    \"scoped_handover_response\": true,\n"
    "    \"handover_stransform\": true,\n"
    "    \"shared_carry_full_cov\": true,\n";

/// A run context with traces, at the handover G01 -> G02 (G01 has left the layout).
struct SeamBed {
    Loaded L;
    fs::path trace_dir;
    std::unique_ptr<NetworkRunTraces> traces;
    std::unique_ptr<NetworkRunContext> ctx;
    std::shared_ptr<spdlog::logger> log;
    std::vector<std::string> registered{"G02", "G05", "G32", "E02", "E03"};
    SeamBed(const std::string& tag, const std::string& extra) : L(tag, extra) {
        EXPECT_TRUE(L.ok);
        static auto sink = std::make_shared<spdlog::sinks::null_sink_mt>();
        log = std::make_shared<spdlog::logger>("sts_dense_test", sink);
        trace_dir = fs::temp_directory_path() / ("sts_dense_traces_" + tag);
        fs::create_directories(trace_dir);
        traces = std::make_unique<NetworkRunTraces>(trace_dir);
        traces->openAll(L.cfg, L.cfg.getNetworkConfig(), L.cfg.getGlobalVtecShConfig(),
                        /*sh_model=*/nullptr, /*cl_trace_armed=*/false, log);
        ctx = std::make_unique<NetworkRunContext>(L.cfg, L.cfg.getNetworkConfig());
        NetworkCarryState fx = fixtureCarry(); net::NetworkEntities fx_ent;
        ctx->carry.prev_carry     = fx.prev_carry;
        ctx->carry.prev_carry_cov = fx.prev_carry_cov;
        ctx->carry.prev_pivot["G"] = "G01";
        ctx->carry.last_pivot["G"] = "G01";
        ctx->carry.cxu_pivot['G']  = {"G01", 0.0};
    }
    ~SeamBed() {
        traces.reset();
        std::error_code ec;
        fs::remove_all(trace_dir, ec);
    }
    void handover() {
        NetworkSbasis::Plan plan;
        plan.clock_pivot["G"] = "G02";
        plan.clock_pivot["E"] = "E02";
        networkHandoverStransform(*ctx, L.cfg, log, *traces, GnssTime{2313, 276150.0}, plan,
                                  /*datum_changed=*/true, std::set<char>{'G'}, registered);
    }
};

bool bitwiseEqual(const Eigen::MatrixXd& A, const Eigen::MatrixXd& B) {
    if (A.rows() != B.rows() || A.cols() != B.cols()) return false;
    for (Eigen::Index i = 0; i < A.rows(); ++i)
        for (Eigen::Index j = 0; j < A.cols(); ++j)
            if (!(A(i, j) == B(i, j))) return false;
    return true;
}

bool carryEqual(const std::vector<CarryPrior>& a, const std::vector<CarryPrior>& b) {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i)
        if (a[i].kind != b[i].kind || a[i].sat != b[i].sat || a[i].signal != b[i].signal
            || !(a[i].value == b[i].value) || !(a[i].var == b[i].var))
            return false;
    return true;
}

}  // namespace

// (a) and (b): the kernel against the explicit S P+ S^T, and the incoming pivot exact.
TEST(HandoverStransformDense, EqualsTheExplicitSPSTransposeOfTheAugmentedBlock) {
    NetworkCarryState c = fixtureCarry(); net::NetworkEntities c_ent;
    const Reference ref = reference();
    const ClockStransformDenseResult r = applyClockStransformDense(c, c_ent, 'G', "G02", "G01");
    ASSERT_TRUE(r.applied) << r.reason;
    EXPECT_TRUE(r.selfcheck_ok);
    EXPECT_TRUE(r.p_added);
    EXPECT_EQ(r.rows, 3);     // G02, G05, G32
    EXPECT_EQ(r.parked, 1);   // G07
    EXPECT_EQ(r.k_old, static_cast<std::size_t>(kK));
    EXPECT_EQ(r.iq, kQ);
    EXPECT_EQ(r.c0, kKeys[kQ].value);
    EXPECT_EQ(r.pqq, fixtureP()(kQ, kQ));

    const SharedCarryCov& pcc = c.prev_carry_cov;
    ASSERT_EQ(pcc.P.rows(), kK + 1);
    ASSERT_EQ(pcc.row.at("G01|CLK"), static_cast<std::size_t>(kK));
    const double scale = ref.Pplus.cwiseAbs().maxCoeff();
    for (int i = 0; i <= kK; ++i)
        for (int j = 0; j <= kK; ++j)
            EXPECT_NEAR(pcc.P(i, j), ref.P_exp(i, j), 1e-12 * scale) << i << "," << j;
    // Values, matched by key, and every re-expressed variance is the diagonal of P'.
    for (int i = 0; i < kK; ++i) {
        const Key& k = kKeys[static_cast<std::size_t>(i)];
        const CarryPrior* cp = entry(c, k.sat, k.kind, k.sig);
        ASSERT_NE(cp, nullptr);
        EXPECT_NEAR(cp->value, ref.x_exp(i), 1e-12) << k.sat;
        EXPECT_EQ(cp->var, pcc.P(i, i)) << k.sat;
    }
    const CarryPrior* p = entry(c, "G01");
    ASSERT_NE(p, nullptr);
    EXPECT_EQ(p->value, -kKeys[kQ].value);
    EXPECT_EQ(p->var, fixtureP()(kQ, kQ));
    EXPECT_EQ(p->fold.disposition, CarryDisposition::Undecided) << "decided at the fold";
    // The parked G07: the diagonal rule.
    const CarryPrior* g07 = entry(c, "G07");
    ASSERT_NE(g07, nullptr);
    EXPECT_EQ(g07->value, 0.5 - kKeys[kQ].value);
    EXPECT_EQ(g07->var, 0.0625 + fixtureP()(kQ, kQ));

    // (b) exactly 0, not close to it.
    EXPECT_EQ(entry(c, "G02")->value, 0.0);
    EXPECT_EQ(entry(c, "G02")->var, 0.0);
    for (int j = 0; j <= kK; ++j) {
        EXPECT_EQ(pcc.P(kQ, j), 0.0) << j;
        EXPECT_EQ(pcc.P(j, kQ), 0.0) << j;
    }
}

// (c): the S-transform property on the block.
TEST(HandoverStransformDense, ContrastsAndTheUnmovedBlockAreInvariant) {
    NetworkCarryState c = fixtureCarry(); net::NetworkEntities c_ent;
    const Reference ref = reference();
    ASSERT_TRUE(applyClockStransformDense(c, c_ent, 'G', "G02", "G01").applied);
    const Eigen::MatrixXd& Pn = c.prev_carry_cov.P;
    const Eigen::MatrixXd& Po = ref.Pplus;
    const double tol = 1e-12 * Po.cwiseAbs().maxCoeff();
    const std::vector<int> gps{0, 1, 3, kK};
    // Every GPS clock contrast, the outgoing pivot's included.
    for (int i : gps)
        for (int j : gps) {
            if (j <= i) continue;
            EXPECT_NEAR(Pn(i, i) + Pn(j, j) - 2.0 * Pn(i, j), Po(i, i) + Po(j, j) - 2.0 * Po(i, j), tol)
                << i << "-" << j;
        }
    // A bias against a GPS clock contrast, and two contrasts against each other.
    for (int b : {4, 5, 7})
        for (int i : gps)
            for (int j : gps) {
                if (j <= i) continue;
                EXPECT_NEAR(Pn(b, i) - Pn(b, j), Po(b, i) - Po(b, j), tol) << b << ":" << i << "-" << j;
            }
    // Cov(x_G05 - x_G02, x_G32 - x_G01): two contrasts against each other.
    EXPECT_NEAR(Pn(1, 3) - Pn(1, kK) - Pn(0, 3) + Pn(0, kK),
                Po(1, 3) - Po(1, kK) - Po(0, 3) + Po(0, kK), tol);
    // Entries with u = 0 on both sides: bitwise unchanged (Galileo, the biases).
    const std::vector<int> other{2, 4, 5, 6, 7};
    for (int i : other)
        for (int j : other) EXPECT_EQ(Pn(i, j), Po(i, j)) << i << "," << j;
    for (int i : {2, 6}) {
        const Key& k = kKeys[static_cast<std::size_t>(i)];
        EXPECT_EQ(entry(c, k.sat)->value, k.value) << k.sat;
    }
}

// (d) and (e): idempotent, and positive semi-definite.
TEST(HandoverStransformDense, SecondCallIsTheIdentityAndTheResultIsPsd) {
    NetworkCarryState c = fixtureCarry(); net::NetworkEntities c_ent;
    ASSERT_TRUE(applyClockStransformDense(c, c_ent, 'G', "G02", "G01").applied);
    const Eigen::MatrixXd P1 = c.prev_carry_cov.P;
    const std::vector<CarryPrior> carry1 = c.prev_carry;
    const auto row1 = c.prev_carry_cov.row;
    const ClockStransformDenseResult r2 = applyClockStransformDense(c, c_ent, 'G', "G02", "G01");
    ASSERT_TRUE(r2.applied);
    EXPECT_EQ(r2.c0, 0.0);
    EXPECT_EQ(r2.pqq, 0.0);
    EXPECT_FALSE(r2.p_added);
    EXPECT_TRUE(r2.selfcheck_ok);
    EXPECT_TRUE(bitwiseEqual(c.prev_carry_cov.P, P1));
    EXPECT_TRUE(carryEqual(c.prev_carry, carry1));
    EXPECT_EQ(c.prev_carry_cov.row, row1);

    Eigen::SelfAdjointEigenSolver<Eigen::MatrixXd> es(P1, Eigen::EigenvaluesOnly);
    EXPECT_GE(es.eigenvalues().minCoeff(), -1e-12 * fixtureP().norm());
}

// (f): the diagonal kernel, folded with the off-diagonals it leaves in the outgoing frame,
// over-states every clock contrast by exactly 2 var_q; the dense kernel keeps it.
TEST(HandoverStransformDense, DiagonalKernelOverstatesAClockContrastByTwiceVarQ) {
    NetworkCarryState diag = fixtureCarry(); net::NetworkEntities diag_ent;
    NetworkCarryState dense = fixtureCarry(); net::NetworkEntities dense_ent;
    const Eigen::MatrixXd P0 = fixtureP();
    const double var_q = P0(kQ, kQ);
    const std::vector<std::string> registered{"G02", "G05", "G32", "E02", "E03"};
    const auto v_c = NetworkSbasis::clockNullDirection(registered, "G");
    ASSERT_TRUE(applyClockStransform(diag, diag_ent, v_c, "G02").applied);
    ASSERT_TRUE(applyClockStransformDense(dense, dense_ent, 'G', "G02", "G01").applied);
    EXPECT_TRUE(bitwiseEqual(diag.prev_carry_cov.P, P0)) << "the diagonal kernel leaves the block";
    const double c_true = P0(1, 1) + P0(3, 3) - 2.0 * P0(1, 3);
    const double c_diag = entry(diag, "G05")->var + entry(diag, "G32")->var - 2.0 * diag.prev_carry_cov.P(1, 3);
    const double c_dense = dense.prev_carry_cov.P(1, 1) + dense.prev_carry_cov.P(3, 3)
                         - 2.0 * dense.prev_carry_cov.P(1, 3);
    EXPECT_NEAR(c_dense, c_true, 1e-15);
    EXPECT_NEAR(c_diag - c_dense, 2.0 * var_q, 1e-15);
    // The same values either way.
    for (const std::string s : {"G02", "G05", "G32"})
        EXPECT_NEAR(entry(diag, s)->value, entry(dense, s)->value, 1e-15) << s;
}

// The stash copies the solve's covariance, which is symmetric only to rounding: an asymmetry
// in the entries the kernel does not write is left as it is and does not fail the self-check
// (the 6 h golden arms' first build flagged every edge this way).
TEST(HandoverStransformDense, SelfCheckIgnoresTheStashsRoundingAsymmetry) {
    NetworkCarryState c = fixtureCarry(); net::NetworkEntities c_ent;
    c.prev_carry_cov.P(2, 4) += 1e-19;   // E02 against the G02 code bias: u = 0 on both sides
    const double kept = c.prev_carry_cov.P(2, 4);
    const ClockStransformDenseResult r = applyClockStransformDense(c, c_ent, 'G', "G02", "G01");
    ASSERT_TRUE(r.applied);
    EXPECT_TRUE(r.selfcheck_ok);
    EXPECT_EQ(c.prev_carry_cov.P(2, 4), kept);
    // Rows the kernel wrote are exactly symmetric, whatever the input's rounding.
    for (int i : {0, 1, 3, kK})
        for (int j = 0; j <= kK; ++j) EXPECT_EQ(c.prev_carry_cov.P(i, j), c.prev_carry_cov.P(j, i));
}

// (g): the paths that cannot apply change nothing.
TEST(HandoverStransformDense, NotAppliedPathsChangeNothing) {
    const NetworkCarryState base = fixtureCarry(); net::NetworkEntities base_ent;   // the reference (the state is not copyable)
    {   // no block row for the incoming pivot (a carried entry only)
        NetworkCarryState c = fixtureCarry(); net::NetworkEntities c_ent;
        CarryPrior g09;
        g09.kind = SharedParamKind::Clock; g09.sat = "G09"; g09.value = 0.25; g09.var = 0.5;
        c.prev_carry.push_back(g09);
        const std::vector<CarryPrior> before = c.prev_carry;
        const ClockStransformDenseResult r = applyClockStransformDense(c, c_ent, 'G', "G09", "G01");
        EXPECT_FALSE(r.applied);
        EXPECT_STREQ(r.reason, "no_q_row");
        EXPECT_TRUE(bitwiseEqual(c.prev_carry_cov.P, base.prev_carry_cov.P));
        EXPECT_TRUE(carryEqual(c.prev_carry, before));
    }
    {   // no carried entry for the incoming pivot
        NetworkCarryState c = fixtureCarry(); net::NetworkEntities c_ent;
        const ClockStransformDenseResult r = applyClockStransformDense(c, c_ent, 'G', "G10", "G01");
        EXPECT_FALSE(r.applied);
        EXPECT_STREQ(r.reason, "no_q_entry");
        EXPECT_TRUE(bitwiseEqual(c.prev_carry_cov.P, base.prev_carry_cov.P));
        EXPECT_TRUE(carryEqual(c.prev_carry, base.prev_carry));
    }
    {   // an empty block
        NetworkCarryState c = fixtureCarry(); net::NetworkEntities c_ent;
        c.prev_carry_cov.clear();
        const ClockStransformDenseResult r = applyClockStransformDense(c, c_ent, 'G', "G02", "G01");
        EXPECT_FALSE(r.applied);
        EXPECT_STREQ(r.reason, "empty_block");
        EXPECT_TRUE(carryEqual(c.prev_carry, base.prev_carry));
    }
}

// (h): the seam.  Off, the diagonal kernel and the block untouched; on, the dense kernel and
// its record for the clone fold and the anchor keep.
TEST(HandoverStransformDense, SeamOffRunsTheDiagonalKernelAndOnRecordsTheDenseEdge) {
    {
        SeamBed B("seam_off", kArmed);
        ASSERT_FALSE(B.ctx->knobs.handover_stransform_dense);
        NetworkCarryState expect = fixtureCarry(); net::NetworkEntities expect_ent;
        const auto v_c = NetworkSbasis::clockNullDirection(B.registered, "G");
        ASSERT_TRUE(applyClockStransform(expect, expect_ent, v_c, "G02").applied);
        B.handover();
        EXPECT_TRUE(bitwiseEqual(B.ctx->carry.prev_carry_cov.P, fixtureP()));
        EXPECT_EQ(B.ctx->carry.prev_carry_cov.row.size(), static_cast<std::size_t>(kK));
        EXPECT_TRUE(carryEqual(B.ctx->carry.prev_carry, expect.prev_carry));
        EXPECT_TRUE(B.ctx->carry.sts_dense.empty());
        EXPECT_EQ(B.ctx->carry.sts_consts.count('G'), 1u);
        EXPECT_EQ(B.ctx->counters.sts_dense_edges, 0);
    }
    {
        SeamBed B("seam_on", kArmed + "    \"handover_stransform_dense\": true,\n");
        ASSERT_TRUE(B.ctx->knobs.handover_stransform_dense);
        NetworkCarryState expect = fixtureCarry(); net::NetworkEntities expect_ent;
        ASSERT_TRUE(applyClockStransformDense(expect, expect_ent, 'G', "G02", "G01").applied);
        B.handover();
        EXPECT_TRUE(bitwiseEqual(B.ctx->carry.prev_carry_cov.P, expect.prev_carry_cov.P));
        EXPECT_TRUE(carryEqual(B.ctx->carry.prev_carry, expect.prev_carry));
        ASSERT_EQ(B.ctx->carry.sts_dense.count('G'), 1u);
        const auto& rec = B.ctx->carry.sts_dense.at('G');
        EXPECT_EQ(rec.q, "G02");
        EXPECT_EQ(rec.p, "G01");
        EXPECT_TRUE(rec.p_added);
        EXPECT_TRUE(rec.selfcheck_ok);
        EXPECT_EQ(rec.k_old, static_cast<std::size_t>(kK));
        EXPECT_EQ(rec.iq, kQ);
        EXPECT_EQ(rec.row_sats, (std::set<std::string>{"G01", "G02", "G05", "G32"}));
        // The parked G07 has no decision this epoch, so it is not in the clean set.
        EXPECT_EQ(rec.clean, (std::set<std::string>{"G02", "G05", "G32"}));
        EXPECT_EQ(B.ctx->carry.sts_consts.count('G'), 1u);
        EXPECT_EQ(B.ctx->counters.sts_edges, 1);
        EXPECT_EQ(B.ctx->counters.sts_dense_edges, 1);
        EXPECT_EQ(B.ctx->counters.sts_dense_fallback, 0);
        EXPECT_EQ(B.ctx->counters.sts_dense_p_added, 1);
        EXPECT_EQ(B.ctx->counters.sts_dense_rows, 3);
        EXPECT_EQ(B.ctx->counters.sts_dense_parked, 1);
        EXPECT_EQ(B.ctx->counters.prm_ref_calls[static_cast<int>(CarryMechanism::Handover)], 1);
        EXPECT_GT(B.ctx->counters.sts_dense_var_saved, 0.0) << "positively correlated clocks";
        // The next epoch clears the record before anything reads it.
        B.ctx->carry.sts_dense['E'];
        NetworkSbasis::Plan plan;
        networkHandoverStransform(*B.ctx, B.L.cfg, B.log, *B.traces, GnssTime{2313, 276180.0},
                                  plan, /*datum_changed=*/false, {}, B.registered);
        EXPECT_TRUE(B.ctx->carry.sts_dense.empty());
    }
    {   // the incoming pivot without a block row: the diagonal kernel, counted
        SeamBed B("seam_fallback", kArmed + "    \"handover_stransform_dense\": true,\n");
        B.ctx->carry.prev_carry_cov.row.erase("G02|CLK");
        const Eigen::MatrixXd P0 = B.ctx->carry.prev_carry_cov.P;
        B.handover();
        EXPECT_EQ(B.ctx->counters.sts_dense_fallback, 1);
        EXPECT_EQ(B.ctx->counters.sts_dense_edges, 0);
        EXPECT_EQ(B.ctx->counters.sts_edges, 1);
        EXPECT_TRUE(B.ctx->carry.sts_dense.empty());
        EXPECT_TRUE(bitwiseEqual(B.ctx->carry.prev_carry_cov.P, P0));
        EXPECT_EQ(entry(B.ctx->carry, "G02")->value, 0.0);
    }
}

// (i): the loader.
TEST(HandoverStransformDense, LoaderDefaultOffInertRulesAndRefusals) {
    {
        Loaded off("off", kArmed);
        ASSERT_TRUE(off.ok);
        EXPECT_FALSE(off.cfg.networkHandoverStransformDense());
        EXPECT_FALSE(off.cfg.networkClockAnchorDatumKeep());
    }
    {
        Loaded on("on", kArmed + "    \"handover_stransform_dense\": true,\n");
        ASSERT_TRUE(on.ok);
        EXPECT_TRUE(on.cfg.networkHandoverStransformDense());
    }
    {   // without the transform it replaces
        Loaded inert("no_sts",
                     "    \"scoped_handover_response\": true,\n"
                     "    \"shared_carry_full_cov\": true,\n"
                     "    \"handover_stransform_dense\": true,\n");
        ASSERT_TRUE(inert.ok);
        EXPECT_FALSE(inert.cfg.networkHandoverStransformDense());
    }
    {   // without the dense block
        Loaded inert("no_fullcov",
                     "    \"scoped_handover_response\": true,\n"
                     "    \"handover_stransform\": true,\n"
                     "    \"handover_stransform_dense\": true,\n");
        ASSERT_TRUE(inert.ok);
        EXPECT_FALSE(inert.cfg.networkHandoverStransformDense());
    }
    {   // refused with the transform before the rows
        Loaded refused("rows", kArmed + "    \"handover_stransform_before_rows\": true,\n"
                                        "    \"handover_stransform_dense\": true,\n");
        EXPECT_FALSE(refused.ok);
        Loaded rows_alone("rows_alone", kArmed + "    \"handover_stransform_before_rows\": true,\n");
        EXPECT_TRUE(rows_alone.ok);
    }
    {   // refused with the receiver clock in the block
        Loaded refused("rxclk", kArmed + "    \"carry_rx_clock_in_block\": true,\n"
                                         "    \"handover_stransform_dense\": true,\n");
        EXPECT_FALSE(refused.ok);
        Loaded rx_alone("rxclk_alone", kArmed + "    \"carry_rx_clock_in_block\": true,\n");
        EXPECT_TRUE(rx_alone.ok);
    }
    {   // the anchor keep: inert without the dense knob or without the anchor
        Loaded no_dense("keep_no_dense", kArmed + "    \"clock_temporal_anchor\": true,\n"
                                                  "    \"clock_anchor_datum_keep\": true,\n");
        ASSERT_TRUE(no_dense.ok);
        EXPECT_FALSE(no_dense.cfg.networkClockAnchorDatumKeep());
        Loaded no_anchor("keep_no_anchor", kArmed + "    \"handover_stransform_dense\": true,\n"
                                                    "    \"clock_anchor_datum_keep\": true,\n");
        ASSERT_TRUE(no_anchor.ok);
        EXPECT_FALSE(no_anchor.cfg.networkClockAnchorDatumKeep());
        Loaded keep("keep", kArmed + "    \"handover_stransform_dense\": true,\n"
                                     "    \"clock_temporal_anchor\": true,\n"
                                     "    \"clock_anchor_datum_keep\": true,\n");
        ASSERT_TRUE(keep.ok);
        EXPECT_TRUE(keep.cfg.networkClockAnchorDatumKeep());
        const NetworkKnobs k = NetworkKnobs::snapshot(keep.cfg, keep.cfg.getNetworkConfig());
        EXPECT_TRUE(k.handover_stransform_dense);
        EXPECT_TRUE(k.cta_datum_keep);
    }
}

// (j): card SCL B1 across the dense handover.  The rows reference the clones c_s(t-1) in the
// outgoing frame; the fold's joint prior over (the epoch's columns, the clones) must be the
// law of (S y + w, y_C): [[S P+ S' + W, S P+ E_C], [E_C' P+ S', P_CC]] with mean (S x+, x_C),
// in information form, with the outgoing pivot (absent from the layout) marginalized.
TEST(HandoverStransformDense, CloneFoldIsTheJointLawAcrossTheDenseHandover) {
    SeamBed B("clone_fold", kArmed +
              "    \"carry_conditional_coupling\": true,\n"
              "    \"carry_coupling_time_update\": true,\n"
              "    \"carry_coupling_exact_update\": true,\n"
              "    \"carry_coupling_clock_clone\": true,\n"
              "    \"handover_stransform_dense\": true,\n");
    ASSERT_TRUE(B.ctx->knobs.carry_cpl_clone);
    ASSERT_TRUE(B.ctx->knobs.handover_stransform_dense);
    const Eigen::MatrixXd P0 = fixtureP();
    const Reference ref = reference();
    // Card CTU's copy of the block, taken before PASS B (before the transform).
    B.ctx->carry.cxu_epoch.ok = true;
    B.ctx->carry.cxu_epoch.P  = P0;
    B.handover();
    ASSERT_EQ(B.ctx->carry.sts_dense.count('G'), 1u);

    // The layout: the epoch's columns (G01 has left), then the GPS clocks' clones.
    NetworkEstimator est;
    const std::map<std::string, Eigen::Index> col{
        {"G02|CLK", est.registerClock("G02")},
        {"G05|CLK", est.registerClock("G05")},
        {"G32|CLK", est.registerClock("G32")},
        {"E02|CLK", est.registerClock("E02")},
        {"E03|CLK", est.registerClock("E03")},
        {"G02|CB|C1C", est.registerCodeBias("G02", "C1C")},
        {"G05|PB|L1C", est.registerPhaseBias("G05", "L1C")},
        {"E03|CB|C1C", est.registerCodeBias("E03", "C1C")}};
    ClockCloneEpoch& E = B.ctx->carry.ccl;
    E.clear();
    const std::map<std::string, int> old_row{{"G02", 0}, {"G05", 1}, {"G32", 3}};
    for (const auto& [sat, r] : old_row) {
        E.col[sat] = est.registerClockClone(sat);
        ClockCloneActive a;
        a.p    = r;
        a.s    = r;
        a.col  = E.col[sat];
        a.q    = 1e-4;
        a.shat = kKeys[static_cast<std::size_t>(r)].value;
        E.active[sat] = a;
    }
    E.rows_on = true;
    est.beginEpoch();

    // The fold's inputs, as networkInjectD2CarryPrior forms them: every carried entry with a
    // block row (G01 included), var_eff = P'_ii + q.
    const SharedCarryCov& pcc = B.ctx->carry.prev_carry_cov;
    std::vector<NetworkEstimator::SharedPrior> dkeys;
    std::vector<std::size_t> drow;
    std::vector<double> dvar_eff;
    std::vector<char> dinflate, dreexp;
    std::vector<double> w_of;   // the increment of each dense key
    for (const CarryPrior& cp : B.ctx->carry.prev_carry) {
        const auto rit = pcc.row.find(sharedCarryKey(cp.kind, cp.sat, cp.signal));
        if (rit == pcc.row.end()) continue;
        NetworkEstimator::SharedPrior p;
        p.kind = cp.kind; p.sat = cp.sat; p.signal = cp.signal; p.value = cp.value;
        const double q = (cp.kind == SharedParamKind::Clock) ? 1e-4 : 1e-6;
        const double ve = pcc.P(static_cast<Eigen::Index>(rit->second), static_cast<Eigen::Index>(rit->second)) + q;
        p.info = 1.0 / ve;
        dkeys.push_back(p);
        drow.push_back(rit->second);
        dvar_eff.push_back(ve);
        w_of.push_back(q);
        dinflate.push_back(0);
        dreexp.push_back((cp.value != cp.fold.value_decided_m || cp.var != cp.fold.var_decided_m2) ? 1 : 0);
    }
    const std::size_t kd = dkeys.size();
    Eigen::MatrixXd cov_eff(static_cast<Eigen::Index>(kd), static_cast<Eigen::Index>(kd));
    for (std::size_t a = 0; a < kd; ++a)
        for (std::size_t b = 0; b < kd; ++b)
            cov_eff(static_cast<Eigen::Index>(a), static_cast<Eigen::Index>(b)) =
                pcc.P(static_cast<Eigen::Index>(drow[a]), static_cast<Eigen::Index>(drow[b]));
    for (std::size_t a = 0; a < kd; ++a)
        cov_eff(static_cast<Eigen::Index>(a), static_cast<Eigen::Index>(a)) = dvar_eff[a];

    EpochWorkspace ws;
    ASSERT_TRUE(networkClockCloneDenseFold(*B.ctx, ws, est, dkeys, drow, dvar_eff, dinflate,
                                           dreexp, cov_eff, /*within_epoch_armed=*/true, nullptr));
    const ClockCloneCounters& C = B.ctx->counters.ccl;
    EXPECT_EQ(C.fold_dense_epochs, 1);
    EXPECT_EQ(C.fold_dense_standin, 3);
    EXPECT_EQ(C.fold_dense_noref, 0);
    EXPECT_EQ(C.fold_reexpressed, 0);
    EXPECT_EQ(E.standin, (std::set<std::string>{"G02", "G05", "G32"}));
    ASSERT_EQ(ws.cap_ccl_walk.size(), 2u);
    for (const auto& w : ws.cap_ccl_walk) EXPECT_EQ(w.ref, "G02") << w.sat;

    // The explicit joint over the layout's 11 columns, in the augmented old-frame variables
    // (row kK is G01, which the layout does not hold, so it is marginalized by omission).
    const std::map<std::string, int> aug{{"G02|CLK", 0}, {"G05|CLK", 1}, {"E02|CLK", 2},
                                         {"G32|CLK", 3}, {"G02|CB|C1C", 4}, {"G05|PB|L1C", 5},
                                         {"E03|CLK", 6}, {"E03|CB|C1C", 7}};
    const Eigen::Index n = est.numShared();
    ASSERT_EQ(n, 11);
    std::vector<int> cur_idx(static_cast<std::size_t>(n), -1), clone_idx(static_cast<std::size_t>(n), -1);
    std::vector<double> w_cur(static_cast<std::size_t>(n), 0.0);
    for (const auto& [key, c] : col) {
        cur_idx[static_cast<std::size_t>(c)] = aug.at(key);
        for (std::size_t a = 0; a < kd; ++a)
            if (sharedCarryKey(dkeys[a].kind, dkeys[a].sat, dkeys[a].signal) == key)
                w_cur[static_cast<std::size_t>(c)] =
                    dvar_eff[a] - pcc.P(static_cast<Eigen::Index>(drow[a]), static_cast<Eigen::Index>(drow[a]));
    }
    for (const auto& [sat, c] : E.col) clone_idx[static_cast<std::size_t>(c)] = old_row.at(sat);
    const Eigen::MatrixXd SP  = ref.S * ref.Pplus;
    const Eigen::MatrixXd SPS = SP * ref.S.transpose();
    Eigen::MatrixXd J(n, n);
    Eigen::VectorXd mu(n);
    for (Eigen::Index i = 0; i < n; ++i) {
        const int ci = cur_idx[static_cast<std::size_t>(i)], ki = clone_idx[static_cast<std::size_t>(i)];
        mu(i) = (ci >= 0) ? ref.x_exp(ci) : ref.x_plus(ki);
        for (Eigen::Index j = 0; j < n; ++j) {
            const int cj = cur_idx[static_cast<std::size_t>(j)], kj = clone_idx[static_cast<std::size_t>(j)];
            if (ci >= 0 && cj >= 0)      J(i, j) = SPS(ci, cj) + ((i == j) ? w_cur[static_cast<std::size_t>(i)] : 0.0);
            else if (ci >= 0)            J(i, j) = SP(ci, kj);
            else if (cj >= 0)            J(i, j) = SP(cj, ki);
            else                         J(i, j) = ref.Pplus(ki, kj);
        }
    }
    const Eigen::MatrixXd L_exp = J.inverse();
    const Eigen::VectorXd eta_exp = L_exp * mu;
    const Eigen::MatrixXd& N = est.normals().sharedNormals();
    const Eigen::VectorXd& u = est.normals().sharedRhs();
    const double ls = L_exp.cwiseAbs().maxCoeff();
    for (Eigen::Index i = 0; i < n; ++i) {
        for (Eigen::Index j = 0; j < n; ++j)
            EXPECT_NEAR(N(i, j), L_exp(i, j), 1e-7 * ls) << i << "," << j;
        EXPECT_NEAR(u(i), eta_exp(i), 1e-7 * eta_exp.cwiseAbs().maxCoeff()) << i;
    }
}
