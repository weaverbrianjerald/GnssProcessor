/**
 * @file test_network_solve_skip.cpp
 * @brief The network solve's exit table (card NOM3 step SG): one row per SolveSkip enumerator in
 *        its order, the frozen disposition literals, the trace flags, the heartbeat states and
 *        the tail, and the facts the rows read.  The three silent exits (DatumFailed,
 *        SolveFailed, WeRestoreFailed: no H line, no trace row) are pinned here until a card of
 *        their own changes them (finding 3 of the SG design).
 */
#include <gtest/gtest.h>

#include "network/qc/NetworkRaimWithinEpoch.hpp"
#include "network/solve/NetworkSolveGlue.hpp"

#include <cstring>
#include <vector>

using namespace GnssProcessor;

namespace {
const SolveSkipRow& rowOf(SolveSkip s) { return kSolveSkips[static_cast<std::size_t>(s)]; }
}  // namespace

TEST(NetworkSolveSkipTable, OneRowPerEnumeratorInOrder) {
    ASSERT_EQ(kSolveSkips.size(), 5u);
    for (std::size_t i = 0; i < kSolveSkips.size(); ++i)
        EXPECT_EQ(static_cast<std::size_t>(kSolveSkips[i].skip), i) << "row " << i;
}

TEST(NetworkSolveSkipTable, DispositionLiteralsAreTheFrozenOnes) {
    EXPECT_STREQ(rowOf(SolveSkip::DatumFailed).disposition, "datum_failed");
    EXPECT_STREQ(rowOf(SolveSkip::GateWithheld).disposition, "gate_withheld");
    EXPECT_STREQ(rowOf(SolveSkip::SolveFailed).disposition, "solve_failed");
    EXPECT_STREQ(rowOf(SolveSkip::WeRestoreFailed).disposition, "we_restore_failed");
    EXPECT_STREQ(rowOf(SolveSkip::Sigma0Withheld).disposition, "sigma0_withheld");
}

TEST(NetworkSolveSkipTable, GateWithheldWritesFlagZeroAndTheGateHeartbeat) {
    const SolveSkipRow& r = rowOf(SolveSkip::GateWithheld);
    EXPECT_EQ(r.trace_flag, 0);
    EXPECT_TRUE(r.h_line);
    EXPECT_EQ(r.h_state, NetworkProducerStatus::State::WithheldGate);
    EXPECT_TRUE(r.col_datum_changed);
    EXPECT_FALSE(r.col_t2_sigma0);
    EXPECT_EQ(r.tail, nullptr);
}

TEST(NetworkSolveSkipTable, Sigma0WithheldWritesFlagOneTheMetersAndTheTail) {
    const SolveSkipRow& r = rowOf(SolveSkip::Sigma0Withheld);
    EXPECT_EQ(r.trace_flag, 1);
    EXPECT_TRUE(r.h_line);
    EXPECT_EQ(r.h_state, NetworkProducerStatus::State::WithheldSigma0);
    EXPECT_TRUE(r.col_t2_sigma0);
    EXPECT_TRUE(r.col_t2_meters);
    EXPECT_EQ(r.tail, &networkSigma0WithholdTail);
}

TEST(NetworkSolveSkipTable, SilentExitsWriteNoHeartbeatAndNoTraceRow) {
    for (const SolveSkip s : {SolveSkip::DatumFailed, SolveSkip::SolveFailed, SolveSkip::WeRestoreFailed}) {
        const SolveSkipRow& r = rowOf(s);
        EXPECT_EQ(r.trace_flag, -1) << r.disposition;
        EXPECT_FALSE(r.h_line) << r.disposition;
        EXPECT_EQ(r.tail, nullptr) << r.disposition;
        EXPECT_FALSE(r.col_t2_meters) << r.disposition;
    }
    EXPECT_FALSE(rowOf(SolveSkip::DatumFailed).col_datum_changed);
    EXPECT_TRUE(rowOf(SolveSkip::SolveFailed).col_datum_changed);
    EXPECT_TRUE(rowOf(SolveSkip::WeRestoreFailed).col_t2_sigma0);
}

TEST(NetworkSolveSkipTable, FactsPointAtTheOrchestratorsObjects) {
    SolveDatum d;
    d.datum_changed = true;
    const SolveSkipFacts f = SolveSkipFacts::of(d);
    EXPECT_EQ(f.plan, &d.plan);
    EXPECT_TRUE(f.datum_changed);
    EXPECT_EQ(f.sigma0_adapt, 0.0);   // the gate's H sigma0 default (VALUE_EQUAL)
    EXPECT_EQ(f.est_index_of, nullptr);
    WithinEpochOutcome we;
    we.sigma0_adapt = 2.5;
    we.n_adapt = 3;
    const std::vector<int> est_index_of{0, -1, 1};
    const SolveSkipFacts g = SolveSkipFacts::of(d, we, est_index_of);
    EXPECT_EQ(g.plan, &d.plan);
    EXPECT_EQ(g.est_index_of, &est_index_of);
    EXPECT_EQ(g.sigma0_adapt, 2.5);
    EXPECT_EQ(g.n_adapt, 3);
    const SolveSkipFacts empty{};
    EXPECT_EQ(empty.plan, nullptr);
    EXPECT_FALSE(empty.datum_changed);
}
