/**
 * @file test_producer_ar_trace.cpp
 * @brief PRODUCER-AR PHASE-2 TRACE (docs/network_ambiguity_resolution_design.md,
 *        the 2026-09-10 note §4.1) — the cap-permutation mapping, the header
 *        contract, and OFF-inertness.
 *
 * The Phase-2 diagnostic's search does NOT run on the DD basis it reports.  When
 * `network.ambiguity_resolution.ils_max_dim` is armed, the basis is sorted by
 * ascending formal variance, truncated, and only that CAPPED, PERMUTED copy is
 * handed to `ils::ilsFix` — so `IlsResult::kept` indexes the search vector, and
 * `IlsResult::afix` is in *kept order*, which is a third indexing.  A trace that
 * collapses those three without the double indirection still produces a perfectly
 * well-formed CSV: every row parses, the fixed-DD count is right, and the integers
 * are simply attached to the WRONG satellites.  Nothing downstream can detect that
 * — the grader would report a wrong-fix rate and the covariance would take the
 * blame.  Hence @ref GnssProcessor::netar::mapFixToBasis is a pure function and
 * hence this file: the mapping is proven on a fabricated case where the right
 * answer is known by construction, not inferred from a 24 h run.
 *
 *   Case 1  the card's worked example — 5 DDs, cap 3, a partial fix of 2.
 *   Case 2  the uncapped path is the IDENTITY (and must not sort).
 *   Case 3  a rejected fix labels nothing as kept, but subset membership stands.
 *   Case 4  σ-ordering picks the lowest-variance DDs, ties and all.
 *   Case 5  malformed input loses rows; it never mislabels them.
 *   Case 6  the header constant matches the documented column contract.
 *   Case 7  OFF path: the stream is not open, so no rows can be written.
 *
 * Suite name `ProducerArTrace` is owned by this file alone (suite-hygiene rule in
 * CLAUDE.md — one suite, one file).
 */

#include <gtest/gtest.h>

#include "filter/NetworkProducerAr.hpp"
#include "logging/NetworkRunTraces.hpp"

#include <Eigen/Dense>

#include <fstream>
#include <sstream>
#include <string>
#include <vector>

using GnssProcessor::netar::DdFixLabel;
using GnssProcessor::netar::lowestSigmaOrder;
using GnssProcessor::netar::mapFixToBasis;

namespace {

/// Diagonal covariance with the given per-DD variances (cycles²).
Eigen::MatrixXd diagQ(const std::vector<double>& v) {
    Eigen::MatrixXd Q = Eigen::MatrixXd::Zero(static_cast<int>(v.size()),
                                              static_cast<int>(v.size()));
    for (int i = 0; i < static_cast<int>(v.size()); ++i) Q(i, i) = v[static_cast<std::size_t>(i)];
    return Q;
}

Eigen::VectorXd vec(const std::vector<double>& v) {
    Eigen::VectorXd x(static_cast<int>(v.size()));
    for (int i = 0; i < static_cast<int>(v.size()); ++i) x(i) = v[static_cast<std::size_t>(i)];
    return x;
}

}  // namespace

// ===========================================================================
// Case 1 — the worked example: 5 DDs, cap 3, the ILS keeps 2 of the 3 searched.
// ===========================================================================
TEST(ProducerArTrace, CapPermutationMapsKeptBackToTheBasis) {
    // Formal variances chosen so the σ-ordering is unambiguous and is NOT the
    // basis order: ascending Q_aa gives basis rows 4, 1, 3, then 0, 2.
    const Eigen::MatrixXd Q = diagQ({0.90, 0.02, 4.00, 0.30, 0.01});
    const std::vector<int> search_of = lowestSigmaOrder(Q, 3);
    ASSERT_EQ(search_of.size(), 3u);
    EXPECT_EQ(search_of[0], 4);   // Q=0.01
    EXPECT_EQ(search_of[1], 1);   // Q=0.02
    EXPECT_EQ(search_of[2], 3);   // Q=0.30

    // The ILS searched rows {4,1,3} and accepted a PARTIAL subset: search rows 0
    // and 2 — i.e. basis DDs 4 and 3 — with integers 7 and -2, in kept order.
    const std::vector<int> kept = {0, 2};
    const Eigen::VectorXd afix = vec({7.0, -2.0});

    const std::vector<DdFixLabel> lab = mapFixToBasis(5, search_of, kept, afix);
    ASSERT_EQ(lab.size(), 5u);

    // Subset membership: exactly the three searched rows.
    EXPECT_FALSE(lab[0].in_subset);
    EXPECT_TRUE(lab[1].in_subset);
    EXPECT_FALSE(lab[2].in_subset);
    EXPECT_TRUE(lab[3].in_subset);
    EXPECT_TRUE(lab[4].in_subset);

    // Kept: basis 4 <- search 0 <- afix(0) = 7 ; basis 3 <- search 2 <- afix(1) = -2.
    EXPECT_TRUE(lab[4].kept);
    EXPECT_DOUBLE_EQ(lab[4].fixed_int, 7.0);
    EXPECT_TRUE(lab[3].kept);
    EXPECT_DOUBLE_EQ(lab[3].fixed_int, -2.0);

    // Searched but NOT returned by the ILS: in the subset, no integer.
    EXPECT_TRUE(lab[1].in_subset);
    EXPECT_FALSE(lab[1].kept);

    // Never searched: no integer, no membership.
    EXPECT_FALSE(lab[0].kept);
    EXPECT_FALSE(lab[2].kept);

    // THE NEGATIVE CONTROL — what the naive mapping would have produced.  Reading
    // `kept` as basis indices puts 7 on basis DD 0 and -2 on basis DD 2, i.e. both
    // integers on satellites that were never even searched.  Asserting the two
    // disagree is what makes case 1 a test of the indirection rather than of
    // arithmetic that happens to line up.
    EXPECT_FALSE(lab[0].kept) << "naive kept-as-basis mapping would fix DD 0";
    EXPECT_FALSE(lab[2].kept) << "naive kept-as-basis mapping would fix DD 2";
}

// ===========================================================================
// Case 2 — uncapped: identity, and the basis order is LEFT ALONE.
// ===========================================================================
TEST(ProducerArTrace, UncappedOrderIsTheUnsortedIdentity) {
    const Eigen::MatrixXd Q = diagQ({0.90, 0.02, 4.00, 0.30, 0.01});
    // cap 0 (the loader default) and cap >= n both mean "search everything".  The
    // legacy pre-trace code did not sort in that case, and the search order is an
    // input to MLAMBDA's decorrelation, so sorting here would silently change what
    // an uncapped armed run does.
    for (int cap : {0, -1, 5, 9}) {
        const std::vector<int> ord = lowestSigmaOrder(Q, cap);
        ASSERT_EQ(ord.size(), 5u) << "cap=" << cap;
        for (int i = 0; i < 5; ++i) EXPECT_EQ(ord[static_cast<std::size_t>(i)], i) << "cap=" << cap;
    }

    // Full-set fix: kept = 0..n-1 maps straight through, and every DD is in-subset.
    const std::vector<int> ord = lowestSigmaOrder(Q, 0);
    const std::vector<int> kept = {0, 1, 2, 3, 4};
    const Eigen::VectorXd afix = vec({1.0, 2.0, 3.0, 4.0, 5.0});
    const std::vector<DdFixLabel> lab = mapFixToBasis(5, ord, kept, afix);
    for (int i = 0; i < 5; ++i) {
        EXPECT_TRUE(lab[static_cast<std::size_t>(i)].in_subset);
        EXPECT_TRUE(lab[static_cast<std::size_t>(i)].kept);
        EXPECT_DOUBLE_EQ(lab[static_cast<std::size_t>(i)].fixed_int, i + 1.0);
    }
}

// ===========================================================================
// Case 3 — a rejected fix: `kept`/`afix` are empty, subset membership survives.
// ===========================================================================
TEST(ProducerArTrace, RejectedFixKeepsSubsetButLabelsNothing) {
    const Eigen::MatrixXd Q = diagQ({0.90, 0.02, 4.00, 0.30, 0.01});
    const std::vector<int> search_of = lowestSigmaOrder(Q, 2);   // basis 4, 1
    const std::vector<DdFixLabel> lab =
        mapFixToBasis(5, search_of, {}, Eigen::VectorXd());
    ASSERT_EQ(lab.size(), 5u);
    EXPECT_TRUE(lab[4].in_subset);
    EXPECT_TRUE(lab[1].in_subset);
    for (const auto& l : lab) {
        EXPECT_FALSE(l.kept);
        // `fixed_int` must stay at its neutral value so a reader who forgets to
        // gate on `kept` sees 0, not a stale integer from another DD.
        EXPECT_DOUBLE_EQ(l.fixed_int, 0.0);
    }
}

// ===========================================================================
// Case 4 — the ordering is by ascending formal variance, ties included.
// ===========================================================================
TEST(ProducerArTrace, LowestSigmaOrderPicksTheTightestDds) {
    const Eigen::MatrixXd Q = diagQ({5.0, 1.0, 3.0, 1.0, 2.0});
    const std::vector<int> ord = lowestSigmaOrder(Q, 3);
    ASSERT_EQ(ord.size(), 3u);
    // The two 1.0 entries (rows 1 and 3) must both be selected ahead of 2.0.
    EXPECT_TRUE((ord[0] == 1 && ord[1] == 3) || (ord[0] == 3 && ord[1] == 1));
    EXPECT_EQ(ord[2], 4);
    // And the loosest DD (row 0, Q = 5) must never be in a cap-3 subset.
    for (int i : ord) EXPECT_NE(i, 0);
}

// ===========================================================================
// Case 5 — malformed input DROPS rows; it must never mislabel one.
// ===========================================================================
TEST(ProducerArTrace, OutOfRangeIndicesAreDroppedNotMislabelled) {
    // search_of carries an out-of-basis index (9) and a negative one; kept carries
    // an index past the end of search_of; afix is SHORTER than kept.  Every one of
    // these is a "cannot happen" that would, if trusted, put an integer on the
    // wrong satellite.  The contract is: lose the row.
    const std::vector<int> search_of = {2, 9, -1, 0};
    const std::vector<int> kept = {0, 3, 7};
    const Eigen::VectorXd afix = vec({4.0, 6.0});   // only 2 entries for 3 kept

    const std::vector<DdFixLabel> lab = mapFixToBasis(3, search_of, kept, afix);
    ASSERT_EQ(lab.size(), 3u);
    EXPECT_TRUE(lab[2].in_subset);    // search_of[0] = 2, valid
    EXPECT_TRUE(lab[0].in_subset);    // search_of[3] = 0, valid
    EXPECT_FALSE(lab[1].in_subset);   // 9 and -1 dropped

    EXPECT_TRUE(lab[2].kept);                       // kept[0]=0 -> search_of[0]=2 -> afix(0)
    EXPECT_DOUBLE_EQ(lab[2].fixed_int, 4.0);
    EXPECT_TRUE(lab[0].kept);                       // kept[1]=3 -> search_of[3]=0 -> afix(1)
    EXPECT_DOUBLE_EQ(lab[0].fixed_int, 6.0);
    EXPECT_FALSE(lab[1].kept);                      // kept[2]=7 is past search_of AND past afix

    // Zero-sized basis must not be a crash or an allocation.
    EXPECT_TRUE(mapFixToBasis(0, search_of, kept, afix).empty());
    EXPECT_TRUE(mapFixToBasis(-3, search_of, kept, afix).empty());
}

// ===========================================================================
// Case 6 — the header IS the file contract; the grader parses by name.
// ===========================================================================
TEST(ProducerArTrace, HeaderMatchesTheDocumentedColumnContract) {
    const std::string hdr = GnssProcessor::kProducerArHeader;
    EXPECT_EQ(hdr,
              "week,tow,cons,signal,sat_j,ref_sat,sta_r,ref_sta,lambda_m,float_cyc,"
              "sigma_cyc,in_subset,fixed_int,kept,ratio,p_succ,n_dd,n_search,n_fixed\n");
    // The identity columns are the reason the file exists (without them a row
    // cannot be joined to truth_ambiguities.csv), and the star reference must be
    // on every row so a grader can difference it away — SPEC_amb_iono §6.12.
    for (const char* col : {"sat_j", "ref_sat", "sta_r", "ref_sta",
                            "float_cyc", "sigma_cyc", "in_subset", "fixed_int", "kept"})
        EXPECT_NE(hdr.find(col), std::string::npos) << "missing column " << col;
    EXPECT_EQ(hdr.back(), '\n');
}

// ===========================================================================
// Case 7 — OFF path: a default-constructed trace owner has no stream and no file.
// ===========================================================================
TEST(ProducerArTrace, OffPathOpensNoStreamAndWritesNoRows) {
    const std::filesystem::path dir =
        std::filesystem::temp_directory_path() / "gnss_producer_ar_off_test";
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);

    GnssProcessor::NetworkRunTraces traces(dir);
    // openAll is not called (that is the ARMED path); the OFF path is exactly this
    // state, and the writer in NetworkProducerAr.cpp is gated on is_open().
    EXPECT_FALSE(traces.producer_ar_trace.is_open());
    EXPECT_EQ(traces.producer_ar_trace_path.filename().string(), "network_producer_ar.csv");
    EXPECT_FALSE(std::filesystem::exists(traces.producer_ar_trace_path));

    // closeAll must tolerate the never-opened stream and must not create the file.
    traces.closeAll();
    EXPECT_FALSE(std::filesystem::exists(traces.producer_ar_trace_path));
    std::filesystem::remove_all(dir, ec);
}
