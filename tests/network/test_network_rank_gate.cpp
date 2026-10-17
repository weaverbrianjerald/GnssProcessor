/**
 * @file test_network_rank_gate.cpp
 * @brief Unit tests for the shared-border null-space rank-gate tolerance as a
 *        per-config calibration parameter
 *        (@c network.rank_gate_tol → NetworkEstimator::setRankGateTol).
 *
 * The gate decides whether a whole epoch is emitted or withheld.  This file pins
 * the three properties the design relies on (docs/SPEC_beidou_w6_datum.md §2):
 *
 *  - The default is NetworkEstimator::kNullSpaceTol and the gate semantics are
 *    unchanged: exact equality against the declared S-basis dimension, throwing
 *    in both directions, over a relative cutoff @c tol·max(λmax,1).  The
 *    two-sided throw is why the tolerance is per-config: a value pushed below a
 *    config's own gauge depth stops counting the declared gauges and withholds a
 *    config whose datum is correct (DG1, DG4).
 *  - An armed (deeper) tolerance reclassifies a weak but real layer from "null" to
 *    "observable" while the true gauges stay counted.  This is the mechanism by
 *    which BeiDou's ~1e-3 families stop withholding (DG3).
 *  - One resolved value feeds three consumers: the gate, the estimability dump and
 *    the prioritized drop-and-heal agree.  In particular the heal does not
 *    soft-anchor a direction the gate no longer counts as null, which would put
 *    prior information on real parameters (DG2).
 *
 * The spectrum-shape tests plant eigenvalues directly on a diagonal matrix (the
 * gate's own input), so the decade placement is exact rather than emergent.  The
 * consumer-consistency tests use a registered system (the same orphaned
 * phase-bias construction as test_network_phase_datum.cpp) because the dump and
 * the heal both read the estimator's reduced normals.
 */

#include <gtest/gtest.h>
#include <Eigen/Dense>

#include <filesystem>
#include <fstream>
#include <memory>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#include <spdlog/spdlog.h>
#include <spdlog/sinks/ostream_sink.h>

#include "config/GnssConfig.hpp"
#include "network/solve/Estimator.hpp"
#include "network/solve/PhaseDatum.hpp"
#include "network/solve/Sbasis.hpp"
#include "filter/StateKeySchema.hpp"
#include "logging/LogManager.hpp"
#include "../support/TestTempDir.hpp"   // per-process, per-test scratch directory

namespace GnssProcessor {
namespace {

/// A planted spectrum as a diagonal matrix, which is what the gate eigen-solves.
Eigen::MatrixXd planted(const std::vector<double>& ev) {
    Eigen::MatrixXd D = Eigen::MatrixXd::Zero(static_cast<Eigen::Index>(ev.size()),
                                              static_cast<Eigen::Index>(ev.size()));
    for (std::size_t i = 0; i < ev.size(); ++i)
        D(static_cast<Eigen::Index>(i), static_cast<Eigen::Index>(i)) = ev[i];
    return D;
}

/// The shape of the measured 305-station spectrum: λmax 1e6, a weak but real
/// layer at 1e-4 (1e-10 relative; BeiDou's families), and two declared gauges at
/// 1e-10 (1e-16 relative).  With max(λmax,1) = 1e6 the cutoffs are:
///   tol 1e-9  (default) -> 1e-3  : counts the gauges and the weak real layer (4)
///   tol 1e-12 (armed)   -> 1e-6  : counts only the gauges                    (2)
///   tol 1e-18 (too deep)-> 1e-12 : counts nothing                            (0)
const std::vector<double> kStormShape = {1e-10, 1e-10, 1e-4, 1e-4, 1e6};
constexpr int kStormDeclaredDim = 2;

/// A registered single-station system carrying a second phase-bias column that is
/// either genuinely null or weakly observable: the two below-cutoff classes of
/// the design's §0.1 decomposition, which have different owners.
///
///  - @p weak_w == 0 : PB(G02,L1) gets no rows at all (its only anchor station
///    has dropped).  It is structurally null, so its eigenvalue is exactly 0 and
///    below any positive cutoff.  No tolerance can reclassify it; only the
///    drop-and-heal resolves it.  Mirrors test_network_phase_datum.cpp's
///    HealFixture.
///  - @p weak_w > 0  : PB(G02,L1) gets one phase row at that weight plus its own
///    pinned ambiguity, so it is separable but weakly: the BeiDou-family shape
///    the gate miscounts at 1e-9 and the calibration reclassifies.
///
/// The clock gauge is folded as a tight constraint before the gate, so the
/// declared datum dimension is 0 and the only candidate null direction is the
/// second phase-bias column.
struct SecondColumnFixture {
    NetworkEstimator est;
    std::set<std::string> keys;

    void build(double weak_w) {
        const Eigen::Index c   = est.registerClock("G01");
        const Eigen::Index pb1 = est.registerPhaseBias("G01", "L1");
        const Eigen::Index pb2 = est.registerPhaseBias("G02", "L1");
        keys = { StateKeys::makeSatPhaseBiasKey("G01", "L1"),
                 StateKeys::makeSatPhaseBiasKey("G02", "L1") };

        est.beginEpoch();
        const bool weak = weak_w > 0.0;
        const Eigen::Index n_local = weak ? 3 : 2;   // [a_r, M1(, M2)]
        const Eigen::Index m       = weak ? 5 : 3;
        Eigen::MatrixXd A_s = Eigen::MatrixXd::Zero(m, est.numShared());
        Eigen::MatrixXd A_r = Eigen::MatrixXd::Zero(m, n_local);
        Eigen::VectorXd w   = Eigen::VectorXd::Ones(m);
        Eigen::VectorXd y   = Eigen::VectorXd::Zero(m);
        A_s(0, c) = -1.0; A_r(0, 0) = 1.0;                                  // code
        A_s(1, c) = -1.0; A_s(1, pb1) = 1.0; A_r(1, 0) = 1.0; A_r(1, 1) = 1.0;  // phase
        A_r(2, 1) = 1.0;  w(2) = 1e10;                                      // pivot amb pin
        if (weak) {
            A_s(3, pb2) = 1.0; A_r(3, 0) = 1.0; A_r(3, 2) = 1.0; w(3) = weak_w;
            A_r(4, 2) = 1.0;   w(4) = 1e10;                                 // its own amb pin
        }
        est.addStation(A_s, A_r, w, y);

        Eigen::MatrixXd H = Eigen::MatrixXd::Zero(1, est.numShared());
        H(0, c) = 1.0;
        est.addSharedConstraintRows(H, Eigen::VectorXd::Zero(1),
                                    NetworkSbasis::kClockGaugeWeight);
    }
};

std::shared_ptr<spdlog::logger> sinkLogger(std::ostringstream& oss, const char* name) {
    return std::make_shared<spdlog::logger>(
        name, std::make_shared<spdlog::sinks::ostream_sink_mt>(oss));
}

/// The gate's observed near-zero count, whatever it is.  Passing declared_dim = -1
/// makes the equality test fail, and the out-param is written before that test,
/// so this reads the count without the caller having to predict it.
int observedNullCount(const NetworkSbasis& sb, const Eigen::MatrixXd& N, double tol) {
    int observed = -1;
    try {
        sb.assertNullSpaceRank(N, /*declared_dim=*/-1, tol, &observed);
    } catch (const std::runtime_error&) {
    }
    return observed;
}

// ---------------------------------------------------------------------------
// The default is the compile constant, and there is no other default.  A fresh
// estimator behaves as it did before the knob existed: the off-path byte-identity
// rests on the identical double reaching the identical arithmetic (DG1).
// ---------------------------------------------------------------------------
TEST(NetworkRankGateTest, DefaultToleranceIsTheCompileConstant) {
    NetworkEstimator est;
    EXPECT_EQ(est.rankGateTol(), NetworkEstimator::kNullSpaceTol);
    EXPECT_EQ(NetworkEstimator::kNullSpaceTol, 1e-9);

    est.setRankGateTol(1e-12);
    EXPECT_EQ(est.rankGateTol(), 1e-12);

    // Configuration-level, not per-epoch: the resolved value survives the layout
    // lifecycle; otherwise the gate would revert mid-run without notice.
    est.registerClock("G01");
    est.beginEpoch();
    EXPECT_EQ(est.rankGateTol(), 1e-12);
    est.reset();
    EXPECT_EQ(est.rankGateTol(), 1e-12);
}

// ---------------------------------------------------------------------------
// Exact equality, throwing in both directions.  A misplaced tolerance can
// provoke the over-specified branch, and its message says "over-specified" so
// the reader looks for a too-deep cutoff rather than a missing datum constraint,
// the confusion DG3's placement rule guards against (DG4).
// ---------------------------------------------------------------------------
TEST(NetworkRankGateTest, GateThrowsInBothDirections) {
    const NetworkSbasis sb;
    const Eigen::MatrixXd N = planted(kStormShape);

    // Default tol: cutoff 1e-3 counts 4 (2 gauges + the 2 weak real) vs declared 2.
    try {
        sb.assertNullSpaceRank(N, kStormDeclaredDim, NetworkEstimator::kNullSpaceTol);
        FAIL() << "observed 4 vs declared 2 must throw";
    } catch (const std::runtime_error& e) {
        EXPECT_NE(std::string(e.what()).find("under-specified"), std::string::npos)
            << e.what();
    }

    // A tolerance below the gauge depth: nothing is counted, so the declared
    // gauges go unseen and a correct datum is reported as over-specified.
    try {
        sb.assertNullSpaceRank(N, kStormDeclaredDim, 1e-18);
        FAIL() << "observed 0 vs declared 2 must throw";
    } catch (const std::runtime_error& e) {
        EXPECT_NE(std::string(e.what()).find("over-specified"), std::string::npos)
            << e.what();
    }
}

// ---------------------------------------------------------------------------
// The calibration itself: an armed tolerance placed in the measured gap
// reclassifies the weak real layer as observable while still counting every
// declared gauge.  This is the mechanism by which the BeiDou arm can emit (DG3).
// ---------------------------------------------------------------------------
TEST(NetworkRankGateTest, ArmedToleranceReclassifiesTheWeakRealLayer) {
    const NetworkSbasis sb;
    const Eigen::MatrixXd N = planted(kStormShape);

    int observed = -1;
    EXPECT_THROW(sb.assertNullSpaceRank(N, kStormDeclaredDim,
                                        NetworkEstimator::kNullSpaceTol, &observed),
                 std::runtime_error);
    EXPECT_EQ(observed, 4) << "default cutoff 1e-3 miscounts the 1e-4 layer as null";

    // Armed at 1e-12 -> cutoff 1e-6, mid-gap: the weak real layer passes, the
    // gauges are still counted, and the gate agrees with the declared dimension.
    observed = -1;
    EXPECT_NO_THROW(sb.assertNullSpaceRank(N, kStormDeclaredDim, 1e-12, &observed));
    EXPECT_EQ(observed, kStormDeclaredDim)
        << "armed cutoff must count the gauges and nothing else";

    // The gauges are still counted at the armed value.  This guards against
    // calibrating until the epoch emits, which would switch the gate off.
    EXPECT_THROW(sb.assertNullSpaceRank(N, /*declared_dim=*/0, 1e-12),
                 std::runtime_error);
}

// ---------------------------------------------------------------------------
// The observed count reaches the caller on both paths.  The throwing path needs
// it most: the exception carries the number only as prose, and under- and
// over-specified epochs are counted separately, not parsed from a log string (DG6).
// ---------------------------------------------------------------------------
TEST(NetworkRankGateTest, ObservedCountIsReportedOnThePassingAndThrowingPaths) {
    const NetworkSbasis sb;
    const Eigen::MatrixXd N = planted(kStormShape);

    int observed = -1;
    EXPECT_NO_THROW(sb.assertNullSpaceRank(N, /*declared_dim=*/4,
                                           NetworkEstimator::kNullSpaceTol, &observed));
    EXPECT_EQ(observed, 4);

    observed = -1;
    EXPECT_THROW(sb.assertNullSpaceRank(N, /*declared_dim=*/2, 1e-18, &observed),
                 std::runtime_error);
    EXPECT_EQ(observed, 0) << "over-specified: the count must still be reported";

    // The degenerate guards report 0 rather than leaving the caller's variable
    // stale from a previous epoch.
    observed = 7;
    EXPECT_NO_THROW(sb.assertNullSpaceRank(Eigen::MatrixXd(0, 0), 0, 1e-9, &observed));
    EXPECT_EQ(observed, 0);
}

// ---------------------------------------------------------------------------
// The estimability dump describes the decision the gate is about to make: the
// same resolved tolerance gives the same cutoff and the same below-cutoff count,
// at the default and at armed values.  A dump that disagreed with the gate could
// not support any calibration argument (DG2, consumer 3).
// ---------------------------------------------------------------------------
TEST(NetworkRankGateTest, DumpReportsTheGatesOwnCutoffAndCount) {
    for (const double tol : {NetworkEstimator::kNullSpaceTol, 1e-12, 1e-18}) {
        SecondColumnFixture f;
        f.est.setRankGateTol(tol);
        f.build(/*weak_w=*/1e-6);

        std::ostringstream oss;
        const auto dump = f.est.dumpEstimabilitySpectrum(
            3600.0, /*declared_dim=*/0, std::vector<std::string>{}, sinkLogger(oss, "dg_dump"));
        ASSERT_TRUE(dump.valid) << "tol=" << tol;

        // The dump's own cutoff is the resolved tolerance times max(λmax, 1), the
        // same arithmetic NetworkSbasis::assertNullSpaceRank performs.
        EXPECT_DOUBLE_EQ(dump.cutoff, tol * std::max(dump.ev_max, 1.0)) << "tol=" << tol;

        // The gate, handed the same tolerance, counts what the dump reported
        // (declared_dim=n_below makes it a pass, which isolates the count).
        const NetworkSbasis sb;
        int observed = -1;
        EXPECT_NO_THROW(sb.assertNullSpaceRank(f.est.reducedSharedNormals(),
                                               dump.n_below, tol, &observed))
            << "tol=" << tol;
        EXPECT_EQ(observed, dump.n_below) << "tol=" << tol;

        // The dumped tail is the below-cutoff set plus two (the emitter's
        // deliberate +2 context), which every consumer of the CSV must filter by
        // ev < cutoff rather than trusting the row count.
        EXPECT_GE(static_cast<int>(dump.dirs.size()), dump.n_below) << "tol=" << tol;
        int below_in_dirs = 0;
        for (const auto& d : dump.dirs) if (d.ev < dump.cutoff) ++below_in_dirs;
        EXPECT_EQ(below_in_dirs, dump.n_below) << "tol=" << tol;
    }
}

// ---------------------------------------------------------------------------
// The class boundary the design rests on (§0.1): a genuinely null direction is
// null at every tolerance, so no gate calibration can clear it.  A structurally
// absent column (no rows) has eigenvalue exactly 0, which is below any positive
// cutoff by construction.  The four-class decomposition therefore assigns the
// deep orphans to the heal and only the weak but real families to the
// calibration, and calibrating until the epoch emits cannot clear an
// orphan.
// ---------------------------------------------------------------------------
TEST(NetworkRankGateTest, GenuinelyNullDirectionSurvivesEveryTolerance) {
    const NetworkSbasis sb;
    for (const double tol : {NetworkEstimator::kNullSpaceTol, 1e-12, 1e-18, 1e-30}) {
        SecondColumnFixture f;
        f.est.setRankGateTol(tol);
        f.build(/*weak_w=*/0.0);          // structurally orphaned column
        int observed = -1;
        EXPECT_THROW(sb.assertNullSpaceRank(f.est.reducedSharedNormals(), 0,
                                            f.est.rankGateTol(), &observed),
                     std::runtime_error) << "tol=" << tol;
        EXPECT_GE(observed, 1) << "tol=" << tol;

        // The heal resolves it at every tolerance: the soft-anchor half of the
        // division of labour.
        std::vector<NetworkEstimator::SharedPrior> cap;
        std::vector<PhaseHealEvent> events;
        EXPECT_EQ(healNearNullPhaseBias(f.est, f.keys, /*prioritized=*/true,
                                        /*armed=*/true, cap, &events), 1)
            << "tol=" << tol;
        ASSERT_EQ(events.size(), 1u) << "tol=" << tol;
        EXPECT_EQ(events[0].sat, "G02");
        EXPECT_EQ(events[0].signal, "L1");
        EXPECT_GT(events[0].span, 0.3) << "PA2 attribution: the span that selected it";
        EXPECT_NO_THROW(sb.assertNullSpaceRank(f.est.reducedSharedNormals(), 0,
                                               f.est.rankGateTol())) << "tol=" << tol;
    }
}

// ---------------------------------------------------------------------------
// The drop-and-heal follows the same resolved cutoff.  The default cutoff counts
// a weak but real column as null and the heal soft-anchors it; once the
// calibration moves the cutoff below it, the gate calls it observable and the
// heal leaves it alone.  Otherwise a σ = 1 m prior would land on a parameter the
// gate has just declared real, the near-cutoff fragility that ordered the gate
// calibration ahead of the Kaula prior (DG2 consumer 2, PA1; §0.3).
// ---------------------------------------------------------------------------
TEST(NetworkRankGateTest, HealFollowsTheResolvedToleranceNotTheDefault) {
    constexpr double kWeakW = 1e-6;
    const NetworkSbasis sb;

    // (a) default tolerance: the weak column sits below the cutoff -> gate
    //     throws, heal anchors it.
    double weak_ev = 0.0, ev_max = 0.0;
    {
        SecondColumnFixture f;
        f.build(kWeakW);
        int observed = -1;
        EXPECT_THROW(sb.assertNullSpaceRank(f.est.reducedSharedNormals(), 0,
                                            f.est.rankGateTol(), &observed),
                     std::runtime_error);
        ASSERT_EQ(observed, 1) << "precondition: exactly the weak column is counted null";

        std::ostringstream oss;
        const auto d = f.est.dumpEstimabilitySpectrum(
            0.0, 0, std::vector<std::string>{}, sinkLogger(oss, "dg_weak"));
        ASSERT_TRUE(d.valid);
        ev_max = d.ev_max;
        for (const auto& dir : d.dirs)
            if (dir.ev < d.cutoff) weak_ev = dir.ev;   // the single below-cutoff one
        ASSERT_GT(weak_ev, 0.0) << "the weak column must be observable, just barely";

        std::vector<NetworkEstimator::SharedPrior> cap;
        std::vector<PhaseHealEvent> events;
        EXPECT_EQ(healNearNullPhaseBias(f.est, f.keys, /*prioritized=*/true,
                                        /*armed=*/true, cap, &events), 1);
        EXPECT_EQ(events.size(), 1u);
    }

    // (b) armed one decade below the weak column's own relative depth: the
    //     measured-gap placement DG3 requires, derived here from the spectrum
    //     rather than guessed (the spec forbids a guessed placement).
    {
        const double armed = 0.1 * weak_ev / std::max(ev_max, 1.0);
        ASSERT_GT(armed, 0.0);
        SecondColumnFixture f;
        f.est.setRankGateTol(armed);
        f.build(kWeakW);
        int observed = -1;
        ASSERT_NO_THROW(sb.assertNullSpaceRank(f.est.reducedSharedNormals(), 0,
                                               f.est.rankGateTol(), &observed))
            << "armed=" << armed;
        ASSERT_EQ(observed, 0) << "the calibrated gate must call the weak column real";

        std::vector<NetworkEstimator::SharedPrior> cap;
        std::vector<PhaseHealEvent> events;
        EXPECT_EQ(healNearNullPhaseBias(f.est, f.keys, /*prioritized=*/true,
                                        /*armed=*/true, cap, &events), 0)
            << "the heal must not soft-anchor a column the gate calls observable";
        EXPECT_TRUE(cap.empty());
        EXPECT_TRUE(events.empty());
    }
}

// ---------------------------------------------------------------------------
// The attribution channel is optional and observational: passing nullptr (the
// older call shape, still used by other tests) heals the same columns with the
// same priors (PA2).
// ---------------------------------------------------------------------------
TEST(NetworkRankGateTest, HealAttributionChannelIsObservational) {
    SecondColumnFixture with_out, without_out;
    with_out.build(/*weak_w=*/0.0);
    without_out.build(/*weak_w=*/0.0);

    std::vector<NetworkEstimator::SharedPrior> cap_a, cap_b;
    std::vector<PhaseHealEvent> events;
    const int healed_a = healNearNullPhaseBias(with_out.est, with_out.keys, true, true,
                                               cap_a, &events);
    const int healed_b = healNearNullPhaseBias(without_out.est, without_out.keys, true, true,
                                               cap_b);
    EXPECT_EQ(healed_a, healed_b);
    EXPECT_EQ(healed_a, static_cast<int>(events.size()));
    ASSERT_EQ(cap_a.size(), cap_b.size());
    for (std::size_t i = 0; i < cap_a.size(); ++i) {
        EXPECT_EQ(cap_a[i].sat, cap_b[i].sat);
        EXPECT_EQ(cap_a[i].signal, cap_b[i].signal);
        EXPECT_EQ(cap_a[i].info, cap_b[i].info);
        EXPECT_EQ(cap_a[i].value, cap_b[i].value);
    }
}

// ---------------------------------------------------------------------------
// The phase-to-code companion key transform behind the orphan-window counter
//.  A field run cannot check it: the counter reads 0 at both 305- and
// 50-station density, and 0 is also what a miswired counter reports.  Comparing
// the phase key's signal ("L2X") against the census's code keys ("C2X") without
// the transform leaves the counter at zero forever, which reads as "no orphans"
// (invariant #4).  The transform is L<band><attr> -> C<band><attr>, and it
// matches the one the state-allocation co-gate applies in
// MeasurementModel::ensureNetworkSharedStates; otherwise the two would disagree
// about which phase column belongs to which code column.
// ---------------------------------------------------------------------------
TEST(NetworkRankGateTest, PhaseToCodeCompanionKeyTransformMatchesTheCoGate) {
    struct Case { const char* sat; const char* phase_sig; const char* code_sig; };
    const Case cases[] = {
        {"C21", "L2X", "C2X"},   // BDS-2 the co-gate quarantined in the field
        {"C13", "L7X", "C7X"},
        {"C06", "L1I", "C1I"},
        {"C26", "L7D", "C7D"},
        {"G01", "L1C", "C1C"},
        {"E10", "L5Q", "C5Q"},
    };
    for (const auto& c : cases) {
        const std::string phase_key = StateKeys::makeSatPhaseBiasKey(c.sat, c.phase_sig);
        const auto parts = StateKeys::parseSatPhaseBiasKey(phase_key);
        ASSERT_TRUE(parts.has_value()) << phase_key;
        std::string csig = parts->signal;
        ASSERT_FALSE(csig.empty());
        csig[0] = 'C';
        EXPECT_EQ(csig, c.code_sig);
        EXPECT_EQ(StateKeys::makeSatBiasKey(parts->satellite, csig),
                  StateKeys::makeSatBiasKey(c.sat, c.code_sig));
        // The untransformed phase signal does not collide with the code key, so
        // skipping the transform is detectable.
        EXPECT_NE(StateKeys::makeSatBiasKey(parts->satellite, parts->signal),
                  StateKeys::makeSatBiasKey(c.sat, c.code_sig))
            << "reusing the phase signal verbatim must never match a code key";
    }
}

// ---------------------------------------------------------------------------
// The loose physical-bound prior on admitted OSB columns.
// It targets the residual below-cutoff population seen on the armed 305-station
// arm: 3 declared clock gauges plus one CB+CLK direction with zero phase weight,
// which the drop-and-heal structurally cannot reach.
// ---------------------------------------------------------------------------
TEST(NetworkRankGateTest, KaulaPriorsAreOnePerAdmittedColumnAtOneOverSigmaSquared) {
    const std::vector<std::pair<std::string, std::string>> keys = {
        {"C21", "C5P"}, {"C21", "C5X"}, {"C22", "C2I"}};

    const auto p = NetworkSbasis::buildCodeBiasKaulaPriors(keys, /*sigma_m=*/10.0);
    ASSERT_EQ(p.size(), keys.size());
    for (std::size_t i = 0; i < p.size(); ++i) {
        EXPECT_EQ(p[i].kind, SharedParamKind::CodeBias);
        EXPECT_EQ(p[i].sat, keys[i].first)     << "registration order must be preserved";
        EXPECT_EQ(p[i].signal, keys[i].second);
        EXPECT_EQ(p[i].value, 0.0) << "the physical bound is centred on zero";
        EXPECT_DOUBLE_EQ(p[i].info, 1.0 / (10.0 * 10.0));
    }

    // A non-positive sigma means info = 1/0, a hard pin on every admitted OSB,
    // i.e. an unannounced datum takeover labelled "loose prior".  The seam refuses
    // it, not only the loader.
    EXPECT_TRUE(NetworkSbasis::buildCodeBiasKaulaPriors(keys, 0.0).empty());
    EXPECT_TRUE(NetworkSbasis::buildCodeBiasKaulaPriors(keys, -1.0).empty());
    // No admitted columns -> nothing folded (the off/inert path).
    EXPECT_TRUE(NetworkSbasis::buildCodeBiasKaulaPriors({}, 10.0).empty());
}

// K1/K3 on a registered system: the prior lifts a code-dominated near-null
// direction above the gate's cutoff without being a datum row, so the declared
// dimension the gate is checked against does not move.  This is the mechanism
// the residual population needs, isolated from the phase side (there is no PB
// column here).
TEST(NetworkRankGateTest, KaulaPriorLiftsACodeDominatedNullDirection) {
    // Shared columns [CLK(C21), CB(C21,C2I), CB(C21,C7Z)], locals [a_r].
    //   code C2I : P = a_r - c + b1
    //   code C7Z : P = a_r - c + b2
    // Two code rows, three shared unknowns and one local: the satellite clock is
    // inseparable from a common shift of its two code biases (δc = δb1 = δb2):
    // a code-dominated gauge with zero phase weight, the class the drop-and-heal
    // cannot reach.
    NetworkEstimator est;
    const Eigen::Index c  = est.registerClock("C21");
    const Eigen::Index b1 = est.registerCodeBias("C21", "C2I");
    const Eigen::Index b2 = est.registerCodeBias("C21", "C7Z");
    est.beginEpoch();
    Eigen::MatrixXd A_s = Eigen::MatrixXd::Zero(2, est.numShared());
    Eigen::MatrixXd A_r = Eigen::MatrixXd::Zero(2, 1);
    Eigen::VectorXd w   = Eigen::VectorXd::Ones(2);
    Eigen::VectorXd y   = Eigen::VectorXd::Zero(2);
    A_s(0, c) = -1.0; A_s(0, b1) = 1.0; A_r(0, 0) = 1.0;
    A_s(1, c) = -1.0; A_s(1, b2) = 1.0; A_r(1, 0) = 1.0;
    est.addStation(A_s, A_r, w, y);

    const NetworkSbasis sb;
    int before = -1;
    EXPECT_THROW(sb.assertNullSpaceRank(est.reducedSharedNormals(), /*declared_dim=*/0,
                                        est.rankGateTol(), &before),
                 std::runtime_error);
    ASSERT_GE(before, 1) << "precondition: a code-dominated null direction exists";

    // The direction is code-dominated: no component of it is a phase-bias column,
    // so the drop-and-heal (which only anchors PB columns spanning a near-null
    // direction) has nothing to act on.  That is why KP is a separate lever rather
    // than a heal tuning knob.  Asserted on the dumped spectrum rather than by
    // calling the heal with a fabricated key.
    {
        std::vector<std::string> col_desc(static_cast<std::size_t>(est.numShared()));
        col_desc[static_cast<std::size_t>(c)]  = "CLK|C21";
        col_desc[static_cast<std::size_t>(b1)] = "CB|C21|C2I";
        col_desc[static_cast<std::size_t>(b2)] = "CB|C21|C7Z";
        std::ostringstream oss;
        const auto d = est.dumpEstimabilitySpectrum(0.0, 0, col_desc,
                                                    sinkLogger(oss, "kp_dump"));
        ASSERT_TRUE(d.valid);
        int checked = 0;
        for (const auto& dir : d.dirs) {
            if (dir.ev >= d.cutoff) continue;
            ++checked;
            for (const auto& [label, weight] : dir.top)
                EXPECT_NE(label.rfind("PB|", 0), 0u)
                    << "a below-cutoff direction here must carry NO phase weight; got "
                    << label << '=' << weight;
        }
        EXPECT_GE(checked, 1);
    }
    std::vector<NetworkEstimator::SharedPrior> cap;
    EXPECT_EQ(healNearNullPhaseBias(est, /*shared_phase_keys=*/{},
                                    /*prioritized=*/true, /*armed=*/true, cap), 0)
        << "no phase-bias column exists in a code-only system — the heal is inert here";

    // KP folds the physical bound and the code-dominated direction becomes
    // observable.  The claim is a strict decrease, not zero: this toy system also
    // carries a legitimate satellite-clock ↔ receiver-clock gauge with no
    // code-bias weight, which the prior leaves alone; resolving it would put
    // information on a real datum freedom.
    const auto kp = NetworkSbasis::buildCodeBiasKaulaPriors(
        {{"C21", "C2I"}, {"C21", "C7Z"}}, /*sigma_m=*/10.0);
    ASSERT_EQ(kp.size(), 2u);
    est.addSharedPrior(kp);
    const int after = observedNullCount(sb, est.reducedSharedNormals(), est.rankGateTol());
    EXPECT_LT(after, before)
        << "K1: the bound must strictly reduce the null count (" << before
        << " -> " << after << ')';
}

// The margin inequality, stated as arithmetic so a later sigma or tolerance
// change cannot recreate unnoticed the configuration the CB-ADM study rejected (prior
// information sitting at the rank cutoff) (K2).  With sigma 10 m, info = 0.01,
// which clears the 6.0218e-06 cutoff below by about 3.2 decades.
TEST(NetworkRankGateTest, KaulaPriorClearsTheCutoffByTwoDecades) {
    // From the measured 305-station storm arm: lambda_max 6.0218e+06 with the armed
    // rank_gate_tol 1e-12 gives cutoff 6.0218e-06.
    constexpr double kLambdaMax = 6.021829601e+06;
    constexpr double kArmedTol  = 1e-12;
    const double cutoff = kArmedTol * std::max(kLambdaMax, 1.0);

    const auto p = NetworkSbasis::buildCodeBiasKaulaPriors({{"C21", "C5P"}}, /*sigma_m=*/10.0);
    ASSERT_EQ(p.size(), 1u);
    const double decades = std::log10(p.front().info / cutoff);
    EXPECT_GE(decades, 2.0)
        << "K2: info " << p.front().info << " vs cutoff " << cutoff
        << " is only " << decades << " decades — the rejected near-cutoff configuration";

    // The inverse guard: a prior loose enough to sit at the cutoff fails the same
    // test, so the check is not vacuous.
    const double sigma_at_cutoff = 1.0 / std::sqrt(cutoff);
    const auto weak = NetworkSbasis::buildCodeBiasKaulaPriors({{"C21", "C5P"}}, sigma_at_cutoff);
    ASSERT_EQ(weak.size(), 1u);
    EXPECT_LT(std::log10(weak.front().info / cutoff), 2.0);
}

// ---------------------------------------------------------------------------
// Loader contract: the key parses, defaults to the compile constant, and its
// guards hold (DG1).  The floor matters: a tolerance below double's relative
// resolution against lambda_max counts no direction as null, which turns the
// gate into "declared_dim must be 0", in effect an off switch presented as a
// tuning value (invariant #4).
// ---------------------------------------------------------------------------
namespace {

std::filesystem::path writeCfg(const std::string& tag, const std::string& network_extra) {
    const std::filesystem::path p =
        gnss_test::tempDir() / ("rank_gate_cfg_" + tag + ".json");
    std::ofstream f(p);
    f << "{\n"
         "  \"processing\": {\n"
         "    \"mode\": \"ppp-rtk-network\",\n"
         "    \"active_systems\": [ \"GPS\", \"Galileo\" ],\n"
         "    \"signal_priorities\": { \"GPS\": \"WC\", \"Galileo\": \"CB\" }\n"
         "  },\n"
         "  \"network\": {\n"
         "    \"clock_datum_station\": \"AAAA\",\n"
      << network_extra <<
         "    \"stations\": [\n"
         "      { \"name\": \"AAAA\", \"position_ecef\": [ -2493304.99, -4655214.79, 3565497.67 ] },\n"
         "      { \"name\": \"BBBB\", \"position_ecef\": [ -2281621.75, -1453585.08, 5756964.89 ] }\n"
         "    ]\n"
         "  }\n"
         "}\n";
    f.close();
    return p;
}

}  // namespace

TEST(NetworkRankGateTest, ConfigDefaultsToTheCompileConstant) {
    LogManager log;
    const auto p = writeCfg("default", "");
    GnssConfig cfg;
    ASSERT_TRUE(cfg.loadFromJson(p, log));
    EXPECT_EQ(cfg.networkRankGateTol(), NetworkEstimator::kNullSpaceTol)
        << "an unset key must reproduce the pre-card constant EXACTLY — the "
           "OFF-path byte-identity claim is that the same double travels through "
           "the same arithmetic";
    std::error_code ec;
    std::filesystem::remove(p, ec);
}

TEST(NetworkRankGateTest, ConfigParsesArmsValidatesAndClamps) {
    LogManager log;
    struct Case { const char* tag; const char* json; bool ok; double want; };
    const Case cases[] = {
        {"armed",  "    \"rank_gate_tol\": 1e-12,\n", true,  1e-12},
        {"zero",   "    \"rank_gate_tol\": 0.0,\n",   false, 0.0},
        {"neg",    "    \"rank_gate_tol\": -1e-9,\n", false, 0.0},
        // Below the 1e-16 floor: accepted but clamped, never honoured as given.
        {"floor",  "    \"rank_gate_tol\": 1e-30,\n", true,  1e-16},
    };
    // The loader contract of the physical-bound prior is checked here too: off by default at the
    // documented sigma, and a non-positive sigma is refused rather than folded as
    // info = inf.
    {
        const auto p = writeCfg("kp_default", "");
        GnssConfig cfg;
        ASSERT_TRUE(cfg.loadFromJson(p, log));
        EXPECT_FALSE(cfg.networkCbKaulaPrior());
        EXPECT_EQ(cfg.networkCbKaulaSigmaM(), 10.0);
        std::error_code ec; std::filesystem::remove(p, ec);
    }
    {
        const auto p = writeCfg("kp_bad_sigma",
                                "    \"cb_kaula_prior\": true,\n"
                                "    \"cb_kaula_sigma_m\": 0.0,\n");
        GnssConfig cfg;
        EXPECT_FALSE(cfg.loadFromJson(p, log))
            << "sigma <= 0 means info = inf: a hard pin on every admitted OSB";
        std::error_code ec; std::filesystem::remove(p, ec);
    }
    for (const auto& c : cases) {
        const auto p = writeCfg(c.tag, c.json);
        GnssConfig cfg;
        const bool loaded = cfg.loadFromJson(p, log);
        EXPECT_EQ(loaded, c.ok) << c.tag;
        if (c.ok) EXPECT_EQ(cfg.networkRankGateTol(), c.want) << c.tag;
        std::error_code ec;
        std::filesystem::remove(p, ec);
    }
}

} // namespace
} // namespace GnssProcessor
