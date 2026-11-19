/**
 * @file test_support_carried_family_states.cpp
 * @brief The shared builder of one station's carried families (tests/support/CarriedFamilyStates.hpp)
 *        and its step helpers, checked against the engine's own rules.
 *
 * The response tests of the carried families start from a station whose carried state is known
 * exactly.  These cases make sure the builder writes that state as the harvest does (the cells,
 * the keys, the birth and first-variance rules, one shared harvest block with the wet delay as
 * its last row) and that the state folds as one block through the block builder the station
 * build uses.  The step helpers are checked against the documented effect of the engine entry
 * point each one calls: the chain frame shifts, the wet delay's event response, the float
 * vehicle's drop and the receiver phase bias's step test.
 */

// The support header comes first so that this file proves it compiles on its own.
#include "../support/CarriedFamilyStates.hpp"

#include <gtest/gtest.h>

#include "config/GnssConfigTypes.hpp"
#include "network/carry/Coupling.hpp"
#include "network/solve/Estimator.hpp"
#include "network/state/CarrySites.hpp"

#include <Eigen/Dense>

#include <cmath>
#include <initializer_list>
#include <limits>
#include <memory>
#include <string>
#include <vector>

using namespace GnssProcessor;
using gnss_test::CarriedFamilyState;

namespace {

/// A regression term on the satellite clock G01, the border column the block fixture registers.
CoupledShared clockTerm() {
    return CoupledShared{SharedParamKind::Clock, "G01", "", 0, 0, 'C', /*sh_layer=*/0,
                         /*coeff=*/0.8, /*s_prev=*/1.5, /*p_prev=*/0.2};
}

/// A regression term on a satellite's phase bias.
CoupledShared phaseBiasTerm(const std::string& sat, const std::string& sig, double s_prev) {
    return CoupledShared{SharedParamKind::PhaseBias, sat, sig, 0, 0, 'C', 0, 1.0, s_prev, 1e-4};
}

/// Station @p station with every family: two GPS ambiguities, the clock with a converged drift,
/// the Galileo inter-system bias, the GPS L1C receiver phase bias and the wet delay, harvested at
/// second 1000 of loop epoch 7 into one block.
CarriedFamilyState wholeStation(int station = 0) {
    CarriedFamilyState s;
    s.station  = station;
    s.stamp_s  = 1000.0;
    s.epoch    = 7;
    s.cross_m2 = 1e-6;
    s.coupling = {clockTerm()};
    s.arcs  = {{"G05", "L1C", 2.0, 0.04}, {"G12", "L1C", -1.5, 0.03}};
    s.clock = gnss_test::CarriedClock{29988.0, 1e-4, 0.3, 1e-6, 3};
    s.isbs  = {{System::Galileo, 8.0, 4e-4}};
    s.rpbs  = {{System::GPS, "L1C", 0.6, 2e-4}};
    s.zwd   = gnss_test::CarriedZwd{0.16, 4e-5};
    return s;
}

/// A registered estimator and a station slot with the coupling-row builder over them, as the
/// block tests of the carried families build it.
struct BlockFixture {
    NetworkEstimator est;
    GlobalVtecShConfig shc{};
    net::SatSignalTable tu_reinit_hist;
    StationAssembly R;

    BlockFixture() {
        est.registerClock("G01");
        est.registerPhaseBias("G01", "L1C");
        est.beginEpoch();
        R.A_s = Eigen::MatrixXd::Zero(0, est.numShared());
        R.A_r = Eigen::MatrixXd::Zero(0, 10);
        R.w.resize(0);
        R.y.resize(0);
    }

    CouplingRowBuilder make() {
        return CouplingRowBuilder{est, shc, /*q_clock=*/1e-4, /*q_cb=*/1e-4, /*q_pb=*/1e-4,
                                  /*tu=*/false, /*gauge=*/false, /*reinit=*/false,
                                  tu_reinit_hist, R};
    }
};

/// The block columns of the whole station's carried entries, each at its own variance, in the
/// order of their block rows (local columns 0 to 5 in that order).
std::vector<LocalBlockCol> wholeStationColumns(const net::NetworkEntities& ent, int station) {
    Eigen::Index next = 0;
    const auto col = [&next](const LocalCarry* lc) {
        EXPECT_NE(lc, nullptr);
        return LocalBlockCol{next++, lc ? lc->value_abs - 0.5 : 0.0, lc, lc ? lc->var : 1.0,
                             30.0, 1.0};
    };
    std::vector<LocalBlockCol> bc;
    bc.push_back(col(gnss_test::carriedArc(ent, station, "G05", "L1C")));
    bc.push_back(col(gnss_test::carriedArc(ent, station, "G12", "L1C")));
    bc.push_back(col(gnss_test::carriedClockFamily(ent, station, "")));
    bc.back().is_rx_clk = true;
    bc.push_back(col(gnss_test::carriedClockFamily(ent, station, gnss_test::isbKey(System::Galileo))));
    bc.back().is_rx_clk = true;
    bc.back().is_rx_isb = true;
    bc.push_back(col(gnss_test::carriedRxPhaseBias(ent, station, System::GPS, "L1C")));
    bc.back().is_rx_pb = true;
    bc.push_back(col(gnss_test::carriedZwd(ent, station)));
    bc.back().is_zwd = true;
    return bc;
}

}  // namespace

// The leveling carry: each ambiguity in its arc's cell with the harvest's stamps, and every
// column of the station in one block holding its own row.  Fails if the builder stamped another
// epoch or second, skipped the birth prior, or gave the entries separate blocks.
TEST(SupportCarriedFamilyStates, LevelingArcsTakeTheHarvestStampsAndShareOneBlock) {
    net::NetworkEntities ent;
    const auto blk = gnss_test::stashCarriedFamilies(ent, wholeStation());
    ASSERT_NE(blk, nullptr);
    ASSERT_EQ(blk->C.rows(), 6);
    EXPECT_DOUBLE_EQ(blk->stamp_s, 1000.0);
    EXPECT_DOUBLE_EQ(blk->C(0, 0), 0.04);
    EXPECT_DOUBLE_EQ(blk->C(5, 5), 4e-5);
    EXPECT_DOUBLE_EQ(blk->C(0, 1), 1e-6);

    const LocalCarry* a = gnss_test::carriedArc(ent, 0, "G05", "L1C");
    ASSERT_NE(a, nullptr);
    EXPECT_DOUBLE_EQ(a->value_abs, 2.0);
    EXPECT_DOUBLE_EQ(a->var, 0.04);
    EXPECT_DOUBLE_EQ(a->last_seen_s, 1000.0);
    EXPECT_DOUBLE_EQ(a->var_first, 0.04);
    EXPECT_DOUBLE_EQ(a->var_birth, 0.04);
    EXPECT_EQ(a->birth_ep, 7);
    EXPECT_DOUBLE_EQ(a->birth_s, 1000.0);
    EXPECT_DOUBLE_EQ(a->relevel_s, 1000.0);   // created: the re-level clock starts
    EXPECT_EQ(a->n_folds, 0);
    ASSERT_EQ(a->coupling.size(), 1u);
    EXPECT_EQ(a->blk, blk);
    EXPECT_EQ(a->blk_idx, 0);
    const LocalCarry* b = gnss_test::carriedArc(ent, 0, "G12", "L1C");
    ASSERT_NE(b, nullptr);
    EXPECT_EQ(b->blk, blk);
    EXPECT_EQ(b->blk_idx, 1);
    EXPECT_EQ(gnss_test::carriedArc(ent, 1, "G05", "L1C"), nullptr);   // another station: none
}

// A second harvest keeps what the harvest keeps (birth stamps, first variance, birth prior,
// re-level clock) and replaces the rest, with every row in the new block.  The wet delay keeps no
// birth: its harvest writes a fresh entry.  Fails if a re-stash restarted an arc's birth or let
// an entry keep the previous block.
TEST(SupportCarriedFamilyStates, ReStashKeepsBirthAndFirstVarianceAndMovesToTheNewBlock) {
    net::NetworkEntities ent;
    CarriedFamilyState s = wholeStation();
    const auto blk1 = gnss_test::stashCarriedFamilies(ent, s);
    s.stamp_s = 1030.0;
    s.epoch   = 8;
    s.arcs[0].value_abs = 2.01;
    s.arcs[0].var       = 0.02;
    s.clock->var        = 2e-4;
    const auto blk2 = gnss_test::stashCarriedFamilies(ent, s);
    ASSERT_NE(blk2, nullptr);
    EXPECT_NE(blk2, blk1);

    const LocalCarry* a = gnss_test::carriedArc(ent, 0, "G05", "L1C");
    ASSERT_NE(a, nullptr);
    EXPECT_DOUBLE_EQ(a->value_abs, 2.01);
    EXPECT_DOUBLE_EQ(a->var, 0.02);
    EXPECT_DOUBLE_EQ(a->last_seen_s, 1030.0);
    EXPECT_DOUBLE_EQ(a->var_first, 0.04);
    EXPECT_DOUBLE_EQ(a->var_birth, 0.04);
    EXPECT_EQ(a->birth_ep, 7);
    EXPECT_DOUBLE_EQ(a->birth_s, 1000.0);
    EXPECT_DOUBLE_EQ(a->relevel_s, 1000.0);   // an ordinary re-stash is no re-level
    EXPECT_EQ(a->blk, blk2);

    const LocalCarry* c = gnss_test::carriedClockFamily(ent, 0, "");
    ASSERT_NE(c, nullptr);
    EXPECT_DOUBLE_EQ(c->var, 2e-4);
    EXPECT_DOUBLE_EQ(c->var_first, 1e-4);
    EXPECT_EQ(c->birth_ep, 7);
    EXPECT_EQ(c->blk, blk2);

    const LocalCarry* z = gnss_test::carriedZwd(ent, 0);
    ASSERT_NE(z, nullptr);
    EXPECT_EQ(z->birth_ep, 0);
    EXPECT_DOUBLE_EQ(z->last_seen_s, 1030.0);
    EXPECT_EQ(z->blk, blk2);
}

// The receiver clock is the (bias, drift) pair the injector predicts from: the clock's entry and
// a drift state whose left end is the harvested clock.  Fails if the builder left the drift out
// or stamped its left end at another value or second.
TEST(SupportCarriedFamilyStates, ClockAndDriftAreThePairTheInjectorPredictsFrom) {
    net::NetworkEntities ent;
    gnss_test::stashCarriedFamilies(ent, wholeStation());
    const LocalCarry* c = gnss_test::carriedClockFamily(ent, 0, "");
    ASSERT_NE(c, nullptr);
    EXPECT_DOUBLE_EQ(c->value_abs, 29988.0);
    EXPECT_DOUBLE_EQ(c->var, 1e-4);
    EXPECT_EQ(c->blk_idx, 2);
    const net::RxClkRate* rt = gnss_test::carriedClockRate(ent, 0);
    ASSERT_NE(rt, nullptr);
    EXPECT_DOUBLE_EQ(rt->rate, 0.3);
    EXPECT_DOUBLE_EQ(rt->rate_var, 1e-6);
    EXPECT_EQ(rt->n_incr, 3);
    EXPECT_DOUBLE_EQ(rt->prev_abs, 29988.0);
    EXPECT_DOUBLE_EQ(rt->prev_s, 1000.0);
    EXPECT_DOUBLE_EQ(rt->prev_var, 1e-4);
    // The prediction of the next 30 s epoch at q = 0.09 m^2 per epoch.
    const RxClkPred p = rxClockPredict(rt->prev_abs, rt->prev_var, rt->rate, rt->rate_var, 30.0, 1.0, 0.09);
    EXPECT_NEAR(p.mean, 29988.0 + 9.0, 1e-9);
    EXPECT_NEAR(p.var, 1e-4 + 0.09 + 1e-6 * 900.0, 1e-15);
}

// The inter-system bias is a clock-family entry keyed by the system name beside the primary
// clock, and carries no drift.  Fails if the key changed shape or a drift state was written for
// the bias.
TEST(SupportCarriedFamilyStates, IsbIsKeyedBySystemNameBesideTheClockAndCarriesNoDrift) {
    net::NetworkEntities ent;
    gnss_test::stashCarriedFamilies(ent, wholeStation());
    EXPECT_EQ(gnss_test::isbKey(System::Galileo), "Galileo");
    const LocalCarry* b = gnss_test::carriedClockFamily(ent, 0, "Galileo");
    ASSERT_NE(b, nullptr);
    EXPECT_DOUBLE_EQ(b->value_abs, 8.0);
    EXPECT_DOUBLE_EQ(b->var, 4e-4);
    EXPECT_EQ(b->blk_idx, 3);
    EXPECT_EQ(net::get(ent.stations, StationId{0}, &net::StationState::signals, std::string("Galileo"),
                       &net::StationSignalState::rx_clk_rate),
              nullptr);
    EXPECT_EQ(net::present(ent.stations, &net::StationState::signals, &net::StationSignalState::rx_clk), 2u);
}

// The receiver phase bias sits under the chain's key, and the harvest's step test reads it over
// its own gap: just inside the threshold it holds, just outside it trips, past the horizon it is
// no test.  Fails if the key or the stamp differ from what the harvest reads.
TEST(SupportCarriedFamilyStates, ReceiverPhaseBiasIsKeyedAsTheChainKeysItAndItsStepTestReadsIt) {
    net::NetworkEntities ent;
    gnss_test::stashCarriedFamilies(ent, wholeStation());
    EXPECT_EQ(gnss_test::rxPhaseBiasKey(System::GPS, "L1C"), "GPS_RPB_L1C");
    const LocalCarry* p = net::get(ent.stations, StationId{0}, &net::StationState::signals,
                                   std::string("GPS_RPB_L1C"), &net::StationSignalState::rpb);
    ASSERT_NE(p, nullptr);
    EXPECT_DOUBLE_EQ(p->value_abs, 0.6);
    EXPECT_EQ(p->blk_idx, 4);
    EXPECT_EQ(p->birth_ep, 7);

    const double q = 3e-6, horizon = 900.0, var_now = 2e-4, now = 1030.0;
    const double thr = kRxPbStepSigma * std::sqrt(var_now + 2e-4 + q * 30.0 + kRxPbStepGuardVarM2);
    EXPECT_FALSE(gnss_test::rxPhaseBiasStepTrips(ent, 0, System::GPS, "L1C", 0.6 + thr - 1e-6, var_now,
                                                 now, q, horizon));
    EXPECT_TRUE(gnss_test::rxPhaseBiasStepTrips(ent, 0, System::GPS, "L1C", 0.6 + thr + 1e-6, var_now,
                                                now, q, horizon));
    EXPECT_FALSE(gnss_test::rxPhaseBiasStepTrips(ent, 0, System::GPS, "L1C", 99.0, var_now,
                                                 1000.0 + horizon + 1.0, q, horizon));
    EXPECT_FALSE(gnss_test::rxPhaseBiasStepTrips(ent, 1, System::GPS, "L1C", 99.0, var_now, now, q,
                                                 horizon));   // no entry: no test
}

// The wet delay is the block's last row; with the event response's record requested, a new
// entry opens the first-sighting record at the harvest second, and a re-stash leaves it.  Fails
// if the wet delay moved off the last row or the record was opened without being asked.
TEST(SupportCarriedFamilyStates, WetDelayIsTheLastBlockRowAndOpensItsFirstSightingRecordWhenAsked) {
    net::NetworkEntities ent;
    const auto blk = gnss_test::stashCarriedFamilies(ent, wholeStation());
    const LocalCarry* z = gnss_test::carriedZwd(ent, 0);
    ASSERT_NE(z, nullptr);
    EXPECT_DOUBLE_EQ(z->value_abs, 0.16);
    EXPECT_EQ(z->blk, blk);
    EXPECT_EQ(z->blk_idx, static_cast<int>(blk->C.rows()) - 1);
    EXPECT_FALSE(ent.stations.at(StationId{0}).zwd_edge.has_value());

    net::NetworkEntities ent2;
    CarriedFamilyState s = wholeStation();
    s.zwd_edge = true;
    gnss_test::stashCarriedFamilies(ent2, s);
    const auto& edge = ent2.stations.at(StationId{0}).zwd_edge;
    ASSERT_TRUE(edge.has_value());
    EXPECT_EQ(edge->cause, ZwdEdgeCause::FirstSighting);
    EXPECT_DOUBLE_EQ(edge->decoupled_s, 1000.0);
    EXPECT_DOUBLE_EQ(edge->created_s, 1000.0);
    s.stamp_s = 1030.0;
    gnss_test::stashCarriedFamilies(ent2, s);
    EXPECT_DOUBLE_EQ(ent2.stations.at(StationId{0}).zwd_edge->created_s, 1000.0);
}

// The whole station folds through the block builder as one block: no pair is missing, the
// block's covariance is the harvested one, and each family is counted under its own flag.
// Fails if any entry's block or row differs from the one the builder shares.
TEST(SupportCarriedFamilyStates, WholeStationFoldsAsOneBlockWithNoMissingPair) {
    net::NetworkEntities ent;
    const auto blk = gnss_test::stashCarriedFamilies(ent, wholeStation());
    BlockFixture f;
    auto cb = f.make();
    ASSERT_TRUE(cb.buildLocalBlock(wholeStationColumns(ent, 0)));
    EXPECT_EQ(f.R.pl_blk_pair_missing, 0);
    EXPECT_EQ(f.R.pl_blk_rows, 6);
    EXPECT_EQ(cb.crows.size(), 6u);
    EXPECT_EQ(f.R.pl_blk_zwd_cols, 1);
    EXPECT_EQ(f.R.pl_blk_rxclk_cols, 2);
    EXPECT_EQ(f.R.pl_blk_rxclk_isb_cols, 1);
    EXPECT_EQ(f.R.pl_blk_rxpb_cols, 1);
    const Eigen::MatrixXd& S = f.R.pl_blk.Sigma;
    ASSERT_EQ(S.rows(), 6);
    for (int i = 0; i < 6; ++i)
        for (int j = 0; j < 6; ++j) EXPECT_DOUBLE_EQ(S(i, j), blk->C(i, j)) << i << "," << j;
}

// Without the in-block route no entry holds a block row, and the same fold zeroes every pair
// (15 of 6 columns; the 5 with the wet delay counted in its own share).  Fails if the diagonal
// state left a block pointer or row behind.
TEST(SupportCarriedFamilyStates, DiagonalStateHoldsNoBlockRowAndFoldsWithEveryPairZeroed) {
    net::NetworkEntities ent;
    CarriedFamilyState s = wholeStation();
    s.in_block = false;
    EXPECT_EQ(gnss_test::stashCarriedFamilies(ent, s), nullptr);
    for (const LocalCarry* lc : {gnss_test::carriedArc(ent, 0, "G05", "L1C"),
                                 gnss_test::carriedClockFamily(ent, 0, ""),
                                 gnss_test::carriedRxPhaseBias(ent, 0, System::GPS, "L1C"),
                                 gnss_test::carriedZwd(ent, 0)}) {
        ASSERT_NE(lc, nullptr);
        EXPECT_EQ(lc->blk, nullptr);
        EXPECT_EQ(lc->blk_idx, -1);
    }
    BlockFixture f;
    auto cb = f.make();
    ASSERT_TRUE(cb.buildLocalBlock(wholeStationColumns(ent, 0)));
    EXPECT_EQ(f.R.pl_blk_pair_missing, 15);
    EXPECT_EQ(f.R.pl_blk_zwd_missing_cross, 5);
}

// A known frame step of a receiver on GPS L1C: its phase bias moves by the step and its GPS L1C
// ambiguities by minus the step; another band, another constellation, another station and the
// other families stay.  Fails if the helper keyed the step otherwise than the chain election.
TEST(SupportCarriedFamilyStates, ReceiverChainStepMovesTheBiasByTheStepAndItsBandsArcsByMinusIt) {
    net::NetworkEntities ent;
    NetworkCarryState carry;
    CarriedFamilyState s = wholeStation();
    s.arcs.push_back({"E24", "L1C", 5.0, 0.05});
    s.arcs.push_back({"G05", "L2W", 3.0, 0.05});
    gnss_test::stashCarriedFamilies(ent, s);
    gnss_test::stashCarriedFamilies(ent, wholeStation(1));

    const gnss_test::CarryShiftResult r =
        gnss_test::applyReceiverChainStep(carry, ent, 0, System::GPS, "L1C", 0.1);
    EXPECT_EQ(r.n_values, 3);   // two ambiguities and the receiver phase bias
    EXPECT_EQ(r.n_terms, 0);
    EXPECT_DOUBLE_EQ(gnss_test::carriedArc(ent, 0, "G05", "L1C")->value_abs, 2.0 - 0.1);
    EXPECT_DOUBLE_EQ(gnss_test::carriedArc(ent, 0, "G12", "L1C")->value_abs, -1.5 - 0.1);
    EXPECT_DOUBLE_EQ(gnss_test::carriedRxPhaseBias(ent, 0, System::GPS, "L1C")->value_abs, 0.6 + 0.1);
    EXPECT_DOUBLE_EQ(gnss_test::carriedArc(ent, 0, "E24", "L1C")->value_abs, 5.0);
    EXPECT_DOUBLE_EQ(gnss_test::carriedArc(ent, 0, "G05", "L2W")->value_abs, 3.0);
    EXPECT_DOUBLE_EQ(gnss_test::carriedArc(ent, 1, "G05", "L1C")->value_abs, 2.0);
    EXPECT_DOUBLE_EQ(gnss_test::carriedRxPhaseBias(ent, 1, System::GPS, "L1C")->value_abs, 0.6);
    EXPECT_DOUBLE_EQ(gnss_test::carriedZwd(ent, 0)->value_abs, 0.16);
    EXPECT_DOUBLE_EQ(gnss_test::carriedClockFamily(ent, 0, "")->value_abs, 29988.0);
}

// A known frame step of a satellite's phase bias: its ambiguities at every station move by minus
// the step, and the regression terms on it move by the step at every site the shift's table
// declares.  Fails if the helper missed a station or the walk moved a site the table exempts.
TEST(SupportCarriedFamilyStates, SatelliteChainStepMovesItsArcsAndTheTermsOnItsPhaseBias) {
    net::NetworkEntities ent;
    NetworkCarryState carry;
    for (int sta : {0, 1}) {
        CarriedFamilyState s = wholeStation(sta);
        s.coupling = {phaseBiasTerm("G05", "L1C", 0.4)};
        gnss_test::stashCarriedFamilies(ent, s);
    }
    const gnss_test::CarryShiftResult r = gnss_test::applySatelliteChainStep(carry, ent, "G05", "L1C", 0.3);
    const auto moves = [](CarrySite site) {
        return carryMechanismMoves(CarryMechanism::Eq34Shift, site, /*complete=*/false) ? 1 : 0;
    };
    ASSERT_EQ(moves(CarrySite::AmbiguityCoupling), 1);
    ASSERT_EQ(moves(CarrySite::ZwdCoupling), 1);
    // Per station: the two ambiguities' terms, the wet delay's, and where declared the receiver
    // phase bias's and the two clock-family entries'.
    const long long per_station =
        2 + 1 + moves(CarrySite::RxPhaseBiasCoupling) + 2 * moves(CarrySite::RxClockCoupling);
    EXPECT_EQ(r.n_values, 2);   // G05 L1C at both stations
    EXPECT_EQ(r.n_terms, 2 * per_station);
    for (int sta : {0, 1}) {
        SCOPED_TRACE(sta);
        EXPECT_DOUBLE_EQ(gnss_test::carriedArc(ent, sta, "G05", "L1C")->value_abs, 2.0 - 0.3);
        EXPECT_DOUBLE_EQ(gnss_test::carriedArc(ent, sta, "G12", "L1C")->value_abs, -1.5);
        EXPECT_DOUBLE_EQ(gnss_test::carriedArc(ent, sta, "G12", "L1C")->coupling[0].s_prev, 0.4 + 0.3);
        EXPECT_DOUBLE_EQ(gnss_test::carriedZwd(ent, sta)->coupling[0].s_prev, 0.4 + 0.3);
        EXPECT_DOUBLE_EQ(gnss_test::carriedRxPhaseBias(ent, sta, System::GPS, "L1C")->value_abs, 0.6);
        EXPECT_DOUBLE_EQ(gnss_test::carriedClockFamily(ent, sta, "")->coupling[0].s_prev,
                         moves(CarrySite::RxClockCoupling) ? 0.4 + 0.3 : 0.4);
    }
}

// A zero or non-finite step moves nothing, on either helper.
TEST(SupportCarriedFamilyStates, ZeroOrNonFiniteChainStepMovesNothing) {
    net::NetworkEntities ent;
    NetworkCarryState carry;
    CarriedFamilyState s = wholeStation();
    s.coupling = {phaseBiasTerm("G05", "L1C", 0.4)};
    gnss_test::stashCarriedFamilies(ent, s);
    const auto r0 = gnss_test::applyReceiverChainStep(carry, ent, 0, System::GPS, "L1C", 0.0);
    EXPECT_EQ(r0.n_values + r0.n_terms, 0);
    const auto rn = gnss_test::applySatelliteChainStep(carry, ent, "G05", "L1C",
                                                       std::numeric_limits<double>::quiet_NaN());
    EXPECT_EQ(rn.n_values + rn.n_terms, 0);
    EXPECT_DOUBLE_EQ(gnss_test::carriedArc(ent, 0, "G05", "L1C")->value_abs, 2.0);
    EXPECT_DOUBLE_EQ(gnss_test::carriedArc(ent, 0, "G05", "L1C")->coupling[0].s_prev, 0.4);
    EXPECT_DOUBLE_EQ(gnss_test::carriedRxPhaseBias(ent, 0, System::GPS, "L1C")->value_abs, 0.6);
}

// The wet delay's event response without inflation erases the station's entry once and opens
// its decoupling record; nothing else of the station moves.  Fails if the helper reached another
// cell or the engine's erase stopped opening the record.
TEST(SupportCarriedFamilyStates, WetDelayEventErasesTheEntryAndOpensTheDecouplingRecord) {
    net::NetworkEntities ent;
    gnss_test::stashCarriedFamilies(ent, wholeStation());
    ZwdCarryCounters counters;
    EXPECT_TRUE(gnss_test::applyWetDelayEvent(ent, 0, ZwdEdgeCause::SlipLatch, 1030.0, 0.0, counters));
    EXPECT_EQ(gnss_test::carriedZwd(ent, 0), nullptr);
    const auto& edge = ent.stations.at(StationId{0}).zwd_edge;
    ASSERT_TRUE(edge.has_value());
    EXPECT_EQ(edge->cause, ZwdEdgeCause::SlipLatch);
    EXPECT_DOUBLE_EQ(edge->decoupled_s, 1030.0);
    EXPECT_DOUBLE_EQ(edge->created_s, 0.0);
    EXPECT_NE(gnss_test::carriedArc(ent, 0, "G05", "L1C"), nullptr);
    EXPECT_NE(gnss_test::carriedClockFamily(ent, 0, ""), nullptr);
    EXPECT_FALSE(gnss_test::applyWetDelayEvent(ent, 0, ZwdEdgeCause::SlipLatch, 1060.0, 0.0, counters));
    EXPECT_EQ(counters.n_zwd_event_inflated, 0);
}

// With inflation the response keeps the value and adds the inflation to the sigma in
// quadrature, and counts it.  Fails if the helper did not pass the inflation through.
TEST(SupportCarriedFamilyStates, WetDelayEventWithInflationKeepsTheValueAndWidensTheVariance) {
    net::NetworkEntities ent;
    gnss_test::stashCarriedFamilies(ent, wholeStation());
    ZwdCarryCounters counters;
    EXPECT_TRUE(gnss_test::applyWetDelayEvent(ent, 0, ZwdEdgeCause::RaimArc, 1030.0, 0.02, counters));
    const LocalCarry* z = gnss_test::carriedZwd(ent, 0);
    ASSERT_NE(z, nullptr);
    EXPECT_DOUBLE_EQ(z->value_abs, 0.16);
    EXPECT_DOUBLE_EQ(z->var, 4e-5 + 0.02 * 0.02);
    EXPECT_EQ(counters.n_zwd_event_inflated, 1);
    EXPECT_EQ(ent.stations.at(StationId{0}).zwd_edge->cause, ZwdEdgeCause::RaimArc);
}

// The float vehicle's drop of one converged ambiguity restores its birth variance, clears its
// regression and block row and restarts its re-level clock; its sibling keeps its row, and the
// next harvest carries the drop memory forward.  Fails if the helper dropped another arc or the
// builder lost the drop memory at the re-stash.
TEST(SupportCarriedFamilyStates, ArcSlipDropRestoresTheBirthVarianceAndDecouplesOnlyThatArc) {
    net::NetworkEntities ent;
    CarriedFamilyState s = wholeStation();
    gnss_test::stashCarriedFamilies(ent, s);
    s.stamp_s = 1030.0;
    s.epoch   = 8;
    s.arcs[0].var = 1e-4;   // converged since its birth at 0.04
    const auto blk = gnss_test::stashCarriedFamilies(ent, s);

    const auto out = gnss_test::applyArcSlipDrop(ent, 0, "G05", "L1C", 0.0, 0.0, 1060.0, 9);
    ASSERT_TRUE(out.has_value());
    EXPECT_TRUE(out->acted);
    const LocalCarry* a = gnss_test::carriedArc(ent, 0, "G05", "L1C");
    EXPECT_DOUBLE_EQ(a->var, 0.04);
    EXPECT_DOUBLE_EQ(a->value_abs, 2.0);
    EXPECT_TRUE(a->coupling.empty());
    EXPECT_EQ(a->blk, nullptr);
    EXPECT_EQ(a->blk_idx, -1);
    EXPECT_DOUBLE_EQ(a->relevel_s, 1060.0);
    EXPECT_EQ(a->dropped_ep, 9);
    const LocalCarry* b = gnss_test::carriedArc(ent, 0, "G12", "L1C");
    EXPECT_EQ(b->blk, blk);
    EXPECT_EQ(b->blk_idx, 1);
    EXPECT_FALSE(gnss_test::applyArcSlipDrop(ent, 0, "G30", "L1C", 0.0, 0.0, 1060.0, 9).has_value());

    s.stamp_s = 1090.0;
    s.epoch   = 10;
    gnss_test::stashCarriedFamilies(ent, s);
    a = gnss_test::carriedArc(ent, 0, "G05", "L1C");
    EXPECT_EQ(a->dropped_ep, 9);
    EXPECT_EQ(a->drop_walk_ph, 2);
    EXPECT_DOUBLE_EQ(a->relevel_s, 1060.0);
    EXPECT_EQ(a->birth_ep, 7);
}
