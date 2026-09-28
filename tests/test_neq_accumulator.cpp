/**
 * @file test_neq_accumulator.cpp
 * @brief Unit tests for the cross-session normal-equation accumulator (item #13B).
 *
 * The main test checks that the per-epoch Schur reduction is exact: a multi-epoch
 * batch fed through the accumulator (each epoch carrying a global position block and
 * an epoch-local receiver-clock parameter) and solved from the reduced normal system
 * reproduces, to machine precision, a single dense stacked least-squares solve over
 * [position + one clock per epoch] with the clocks marginalized out.  The dense
 * reference is assembled independently from the raw design rows, so agreement
 * cross-checks the marginalization identities for the normal matrix, the RHS and the
 * post-fit weighted SSR that drives σ̂₀².  A second test covers the arc stamping, and
 * a third checks that an epoch touching no retained parameter adds nothing to
 * n_obs or dof.
 */

#include <gtest/gtest.h>
#include <Eigen/Dense>

#include <cmath>
#include <random>
#include <set>
#include <string>
#include <vector>

#include "common/EpochMeasurement.hpp"
#include "filter/NeqAccumulator.hpp"

namespace GnssProcessor {
namespace {

// Build one epoch's measurement with a 2-D position block + a scalar clock.
// labels = {"POS_X", "POS_Y", "CLK"}; H = [pos_partials | 1].
EpochMeasurement makeEpoch(const Eigen::MatrixXd& pos_part,  // m×2
                           const Eigen::VectorXd& y,         // m
                           const Eigen::VectorXd& var) {     // m (R diagonal)
    const Eigen::Index m = y.size();
    EpochMeasurement e;
    e.innovations = y;
    e.H.resize(m, 3);
    e.H.leftCols<2>() = pos_part;
    e.H.col(2).setOnes();          // clock partial
    e.R_v = var;
    e.satellite_ids.assign(static_cast<std::size_t>(m), "G01");
    e.signal_types.assign(static_cast<std::size_t>(m), "C1C");
    return e;
}

// ---------------------------------------------------------------------------
// Exactness: reduced (per-epoch-clock-eliminated) solve == dense stacked solve.
// ---------------------------------------------------------------------------
TEST(NeqAccumulator, SchurReducedSolveMatchesDenseStacked) {
    std::mt19937 gen(12345);
    std::uniform_real_distribution<double> dpart(-1.0, 1.0);
    std::uniform_real_distribution<double> dvar(0.5, 3.0);
    std::uniform_real_distribution<double> dy(-0.5, 0.5);

    const std::vector<Eigen::Index> ms = { 4, 3, 5, 4 };  // obs per epoch
    const Eigen::Index E = static_cast<Eigen::Index>(ms.size());
    Eigen::Index total = 0;
    for (auto m : ms) total += m;

    // Dense reference system over [POS_X, POS_Y, CLK_0, ... CLK_{E-1}].
    const Eigen::Index ncol = 2 + E;
    Eigen::MatrixXd A = Eigen::MatrixXd::Zero(total, ncol);
    Eigen::VectorXd y(total);
    Eigen::VectorXd w(total);

    NeqAccumulator acc;
    const std::vector<std::string> labels = { "POS_X", "POS_Y", "CLK" };

    Eigen::Index row = 0;
    for (Eigen::Index e = 0; e < E; ++e) {
        const Eigen::Index m = ms[static_cast<std::size_t>(e)];
        Eigen::MatrixXd pos_part(m, 2);
        Eigen::VectorXd ye(m), var(m);
        for (Eigen::Index i = 0; i < m; ++i) {
            pos_part(i, 0) = dpart(gen);
            pos_part(i, 1) = dpart(gen);
            ye(i)  = dy(gen);
            var(i) = dvar(gen);
            // mirror into the dense reference
            A(row, 0) = pos_part(i, 0);
            A(row, 1) = pos_part(i, 1);
            A(row, 2 + e) = 1.0;               // this epoch's clock
            y(row) = ye(i);
            w(row) = 1.0 / var(i);
            ++row;
        }
        acc.accumulate(makeEpoch(pos_part, ye, var), labels, {});
    }

    // Dense stacked weighted least squares.
    const Eigen::MatrixXd AtW  = A.transpose() * w.asDiagonal();
    const Eigen::MatrixXd N    = AtW * A;
    const Eigen::VectorXd u    = AtW * y;
    const Eigen::VectorXd xall = N.ldlt().solve(u);
    const Eigen::Vector2d pos_dense = xall.head<2>();
    const double wssr_dense = y.dot(w.asDiagonal() * y) - u.dot(xall);

    // Reduced solve from the accumulator.
    const NeqAccumulator::Solution sol = acc.solve();
    ASSERT_TRUE(sol.ok);
    EXPECT_EQ(sol.n_params, 2u);                 // POS_X, POS_Y retained
    EXPECT_EQ(sol.n_obs, static_cast<std::size_t>(total));
    EXPECT_EQ(sol.n_local, static_cast<std::size_t>(E));   // one clock per epoch eliminated
    EXPECT_NEAR(sol.dof, static_cast<double>(total) - 2.0 - static_cast<double>(E), 1e-9);

    // Pull POS_X / POS_Y out of the (unordered) label set.
    double rx = 0.0, ry = 0.0;
    for (std::size_t i = 0; i < sol.labels.size(); ++i) {
        if (sol.labels[i] == "POS_X") rx = sol.x(static_cast<Eigen::Index>(i));
        else if (sol.labels[i] == "POS_Y") ry = sol.x(static_cast<Eigen::Index>(i));
    }
    EXPECT_NEAR(rx, pos_dense(0), 1e-8);
    EXPECT_NEAR(ry, pos_dense(1), 1e-8);

    // Post-fit weighted SSR (drives σ̂₀²) must match the dense marginalized value.
    EXPECT_NEAR(sol.wssr, wssr_dense, 1e-7);
    const double dof = static_cast<double>(total) - 2.0 - static_cast<double>(E);
    EXPECT_NEAR(sol.sigma0_sq, wssr_dense / dof, 1e-8);
}

// ---------------------------------------------------------------------------
// Arc-stamping: a flagged slip-reset on a satellite starts a fresh ambiguity
// parameter rather than co-mingling the two arcs into one.
// ---------------------------------------------------------------------------
TEST(NeqAccumulator, ResetStartsNewAmbiguityArc) {
    NeqAccumulator acc;
    // labels: POS_X (global), CLK (epoch-local), AMB:G05_L1C (arc-stamped global).
    const std::vector<std::string> labels = { "POS_X", "CLK", "AMB:G05_L1C" };

    Eigen::MatrixXd posOnly(3, 2);   // unused col shape helper not needed here
    auto makeAmbEpoch = [&](double bias) {
        EpochMeasurement e;
        const Eigen::Index m = 3;
        e.innovations = Eigen::VectorXd::Constant(m, bias);
        e.H.resize(m, 3);
        e.H.col(0).setLinSpaced(m, 0.2, 0.6);   // POS_X partials
        e.H.col(1).setOnes();                   // CLK
        e.H.col(2).setConstant(1.0);            // AMB (phase) partial
        e.R_v = Eigen::VectorXd::Constant(m, 1.0);
        e.satellite_ids.assign(static_cast<std::size_t>(m), "G05");
        e.signal_types.assign(static_cast<std::size_t>(m), "L1C");
        return e;
    };

    acc.accumulate(makeAmbEpoch(0.1), labels, {});               // arc #0
    EXPECT_EQ(acc.numParameters(), 2u);   // POS_X + AMB:G05_L1C#0

    acc.accumulate(makeAmbEpoch(0.2), labels, {});               // same arc #0
    EXPECT_EQ(acc.numParameters(), 2u);   // unchanged: still POS_X + #0

    acc.accumulate(makeAmbEpoch(0.3), labels, { "G05" });        // reset → arc #1
    EXPECT_EQ(acc.numParameters(), 3u);   // POS_X + #0 + #1

    // Confirm both arc segments are present as distinct labels.
    const NeqAccumulator::Solution sol = acc.solve();
    int n_arc0 = 0, n_arc1 = 0;
    for (const auto& l : sol.labels) {
        if (l == "AMB:G05_L1C#0") ++n_arc0;
        if (l == "AMB:G05_L1C#1") ++n_arc1;
    }
    EXPECT_EQ(n_arc0, 1);
    EXPECT_EQ(n_arc1, 1);
}

// ---------------------------------------------------------------------------
// Accounting: an epoch that touches no retained (global) parameter does not add to
// n_obs or dof; counting it would bias σ̂₀² (= WSSR/dof) downward.
// ---------------------------------------------------------------------------
TEST(NeqAccumulator, AllLocalEpochDoesNotInflateObsCount) {
    std::mt19937 gen(777);
    std::uniform_real_distribution<double> dpart(-1.0, 1.0), dy(-0.5, 0.5);
    NeqAccumulator acc;
    const std::vector<std::string> labels = { "POS_X", "POS_Y", "CLK" };

    // Two normal epochs (3 obs each) that constrain position.
    Eigen::Index good_rows = 0;
    for (int e = 0; e < 2; ++e) {
        const Eigen::Index m = 3;
        Eigen::MatrixXd pp(m, 2);
        Eigen::VectorXd y(m), var(m);
        for (Eigen::Index i = 0; i < m; ++i) {
            pp(i, 0) = dpart(gen); pp(i, 1) = dpart(gen);
            y(i) = dy(gen); var(i) = 1.0;
        }
        acc.accumulate(makeEpoch(pp, y, var), labels, {});
        good_rows += m;
    }

    // One all-local epoch: only the CLK column is active (position partials 0),
    // so ng==0 and the epoch is dropped from the retained system entirely.
    {
        EpochMeasurement e;
        const Eigen::Index m = 4;
        e.innovations = Eigen::VectorXd::Constant(m, 0.1);
        e.H = Eigen::MatrixXd::Zero(m, 3);
        e.H.col(2).setOnes();                 // CLK only
        e.R_v = Eigen::VectorXd::Constant(m, 1.0);
        e.satellite_ids.assign(static_cast<std::size_t>(m), "G01");
        e.signal_types.assign(static_cast<std::size_t>(m), "C1C");
        acc.accumulate(e, labels, {});
    }

    const NeqAccumulator::Solution sol = acc.solve();
    ASSERT_TRUE(sol.ok);
    EXPECT_EQ(sol.n_obs, static_cast<std::size_t>(good_rows));  // 6, not 10
    EXPECT_EQ(sol.n_local, 2u);                                 // one CLK per good epoch
    EXPECT_NEAR(sol.dof, static_cast<double>(good_rows) - 2.0 - 2.0, 1e-9);
}

}  // namespace
}  // namespace GnssProcessor
