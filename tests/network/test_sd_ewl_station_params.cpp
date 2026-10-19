/**
 * @file test_sd_ewl_station_params.cpp
 * @brief The SD pre-reduce EWL row stages read a station's parameter set
 *        (network/state/StationParams.hpp), not its state manager.
 *
 * Two stages read the set: the ambiguity candidates (@ref sdEwlFillCandidates) and the
 * linearization points of an arc (@ref sdEwlBuildArc: the two ambiguities, the two
 * satellite phase biases and the two satellite code biases of the elected pair).  Contracts:
 *  - a candidate is an ambiguity of the set that has a phase row this epoch, listed under its
 *    satellite and band digit, each list ascending by signal name whatever the order in
 *    which the set holds them;
 *  - the arc's predicted frame term is the hand-written sum over the values of the set, and a
 *    shared bias the set does not hold reads as zero (a column another station registered).
 */

#include <gtest/gtest.h>

#include "common/Constants.hpp"
#include "common/GnssIdentity.hpp"
#include "common/ParamRef.hpp"
#include "common/PppObservation.hpp"
#include "config/GnssConfig.hpp"
#include "network/ar/SdEwlAr.hpp"
#include "network/ar/SdEwlRows.hpp"
#include "network/driver/RunTypes.hpp"
#include "network/solve/Estimator.hpp"
#include "network/state/StationParams.hpp"

#include <Eigen/Dense>

#include <map>
#include <string>
#include <vector>

using namespace GnssProcessor;

namespace {

using PD = ParamDisposition;

SatId satOf(const char* text) { return *SatId::parse(text); }

SignalId sigOf(const SatId& s, const char* code) { return *SignalId::fromObsCode(s.sys, code); }

/// Everything a row view borrows, owned in one place.
struct Bed {
    GnssConfig                 config;
    NetStation                 st;
    NetworkEstimator           est;
    std::vector<PppObservation> sta_obs;
    std::map<int, int>         h2local;
    GnssTime                   t{};
    Eigen::MatrixXd            A_s, A_r;
    Eigen::VectorXd            w, y;
    std::vector<RowTag>        tags;
    std::vector<std::string>   trace_rows;
    SdEwlConfig                cfg;
    NetStation::SdEwlCounters  ctr;
    SdEwlTraceFull             tr2 = [](const char*, const std::string&, const std::string&, const std::string&,
                                        const std::string&, const std::string&, const std::string&, std::size_t,
                                        std::size_t, bool, double, double, double, double, double, long long, bool,
                                        bool) {};
    SdEwlTraceShort            tr = [](const char*, const std::string&, const std::string&, const std::string&,
                                       const std::string&, const std::string&, const std::string&, std::size_t,
                                       std::size_t, bool, double, double, double, double, double) {};

    /// Append a parameter to the station's set with a linearization point; its slot index.
    int add(const ParamRef& ref, double x_lin) {
        addParam(st.params, LocalSlot{ref, x_lin, PD::EstimatedLocal, false});
        return static_cast<int>(st.params.slots.size()) - 1;
    }

    SdEwlRowView view() {
        return SdEwlRowView{config, st, est, sta_obs, h2local, t, A_s, A_r, w, y, tags, trace_rows,
                            nullptr, 0, cfg, ctr, false, tr2, tr};
    }
};

}  // namespace

TEST(SdEwlStationParamsTest, TheCandidatesAreTheAmbiguitiesOfTheSetThatHaveAPhaseRowAscendingBySignal) {
    Bed bed;
    const SatId g05 = satOf("G05"), e24 = satOf("E24"), r07 = satOf("R07");
    // Birth order unlike the signal order, so a list kept in the set's insertion order would fail.
    const int g_l2x = bed.add(ambiguityRef(g05, sigOf(g05, "L2X")), 1.0);
    const int g_l2w = bed.add(ambiguityRef(g05, sigOf(g05, "L2W")), 2.0);
    const int g_l5q = bed.add(ambiguityRef(g05, sigOf(g05, "L5Q")), 3.0);
    const int g_l1c = bed.add(ambiguityRef(g05, sigOf(g05, "L1C")), 4.0);
    const int e_l5q = bed.add(ambiguityRef(e24, sigOf(e24, "L5Q")), 5.0);
    const int e_l1c = bed.add(ambiguityRef(e24, sigOf(e24, "L1C")), 6.0);
    const int r_l2c = bed.add(ambiguityRef(r07, sigOf(r07, "L2C")), 7.0);
    bed.add(slantIonoRef(g05), 8.0);   // another family: never a candidate
    // The local columns of this epoch's phase rows: every ambiguity but G05 L1C.
    int col = 0;
    for (const int idx : {g_l2x, g_l2w, g_l5q, e_l5q, e_l1c, r_l2c}) bed.h2local[idx] = col++;
    (void)g_l1c;

    SdEwlCandidates cand;
    sdEwlFillCandidates(bed.view(), cand);

    // (satellite, band) keys: G05 bands 2 and 5 (band 1 has no phase row), E24 bands 5 and 1, R07 band 2.
    ASSERT_EQ(cand.size(), 5u);
    ASSERT_EQ(cand.count({"G05", '2'}), 1u);
    const std::vector<SdEwlCand>& g2 = cand.at({"G05", '2'});
    ASSERT_EQ(g2.size(), 2u);
    EXPECT_EQ(g2[0], std::make_tuple(std::string("L2W"), bed.h2local.at(g_l2w), g_l2w));
    EXPECT_EQ(g2[1], std::make_tuple(std::string("L2X"), bed.h2local.at(g_l2x), g_l2x));
    ASSERT_EQ(cand.count({"G05", '5'}), 1u);
    EXPECT_EQ(cand.at({"G05", '5'}).front(), std::make_tuple(std::string("L5Q"), bed.h2local.at(g_l5q), g_l5q));
    EXPECT_EQ(cand.count({"G05", '1'}), 0u);
    ASSERT_EQ(cand.count({"E24", '5'}), 1u);
    EXPECT_EQ(cand.at({"E24", '5'}).front(), std::make_tuple(std::string("L5Q"), bed.h2local.at(e_l5q), e_l5q));
    ASSERT_EQ(cand.count({"E24", '1'}), 1u);
    EXPECT_EQ(cand.at({"E24", '1'}).front(), std::make_tuple(std::string("L1C"), bed.h2local.at(e_l1c), e_l1c));
    ASSERT_EQ(cand.count({"R07", '2'}), 1u);   // GLONASS is listed; the pair search never asks for it
    EXPECT_EQ(cand.at({"R07", '2'}).front(), std::make_tuple(std::string("L2C"), bed.h2local.at(r_l2c), r_l2c));
}

namespace {

/// The elected pair of G05 on L2W and L5Q, with its two ambiguity slots, as the pair search
/// leaves it for @ref sdEwlBuildArc.
struct ArcBed : Bed {
    SatId             g05 = satOf("G05");
    PppObservation    obs;
    SdEwlPairSearch   ps;
    char              cons{'G'};
    double            f1{0.0}, f2{0.0};

    ArcBed() {
        obs.satellite = "G05";
        char b1 = 0, b2 = 0;
        EXPECT_TRUE(sdEwlBands(cons, b1, b2, f1, f2));
        ps.sig1 = "L2W";
        ps.sig2 = "L5Q";
        ps.sidx1 = add(ambiguityRef(g05, sigOf(g05, "L2W")), 100.25);
        ps.sidx2 = add(ambiguityRef(g05, sigOf(g05, "L5Q")), 200.5);
        ps.col1 = 0;
        ps.col2 = 1;
        ps.n_cand = 1;
        ps.zd.mean_m = 3.0;
        ps.zd.var_of_mean_m2 = 0.01;
        ps.zd.count = 20;
        ps.zd.lambda_wl = 5.861;
        ps.zd.valid = true;
        ps.zd.exists = true;
        ps.zd.code1 = "C2W";
        ps.zd.code2 = "C5Q";
        est.registerPhaseBias("G05", "L2W");
        est.registerPhaseBias("G05", "L5Q");
        est.registerCodeBias("G05", "C2W");
        est.registerCodeBias("G05", "C5Q");
    }

    /// The arc the stage builds, or null when it skipped the satellite.
    const SdEwlArc* build(std::map<char, std::vector<SdEwlArc>>& by_const) {
        sdEwlBuildArc(view(), obs, cons, f1, f2, ps, by_const);
        return by_const.count(cons) != 0 && !by_const.at(cons).empty() ? &by_const.at(cons).front() : nullptr;
    }
};

}  // namespace

TEST(SdEwlStationParamsTest, TheArcsFrameTermIsTheSumOverTheValuesOfTheSet) {
    ArcBed bed;
    bed.add(satPhaseBiasRef(bed.g05, sigOf(bed.g05, "L2W")), 0.5);
    bed.add(satPhaseBiasRef(bed.g05, sigOf(bed.g05, "L5Q")), -0.75);
    bed.add(satCodeBiasRef(bed.g05, sigOf(bed.g05, "C2W")), 1.25);
    bed.add(satCodeBiasRef(bed.g05, sigOf(bed.g05, "C5Q")), -2.0);

    std::map<char, std::vector<SdEwlArc>> by_const;
    const SdEwlArc* a = bed.build(by_const);
    ASSERT_NE(a, nullptr);

    const double f1 = bed.f1, f2 = bed.f2;
    const double lam_ewl = bed.ps.zd.lambda_wl;
    const double expect_amb = (lam_ewl / (Constants::SPEED_OF_LIGHT / f1)) * 100.25 - (lam_ewl / (Constants::SPEED_OF_LIGHT / f2)) * 200.5;
    const double expect_bias = (f1 / (f1 - f2)) * 0.5 - (f2 / (f1 - f2)) * -0.75
                             - (f1 / (f1 + f2)) * 1.25 - (f2 / (f1 + f2)) * -2.0;
    EXPECT_DOUBLE_EQ(a->pred_amb_m, expect_amb);
    EXPECT_DOUBLE_EQ(a->pred_bias_m, expect_bias);
    EXPECT_EQ(a->pred_m, a->pred_amb_m + a->pred_bias_m);
    EXPECT_EQ(a->sig1, "L2W");
    EXPECT_EQ(a->code2, "C5Q");
}

TEST(SdEwlStationParamsTest, ASharedBiasTheSetDoesNotHoldReadsAsZero) {
    // The estimator registered the column (another station holds it); this station has no
    // state for the L5Q phase bias and the C2W code bias, which matches its real rows: the
    // innovations omit a column the station has no state for.
    ArcBed bed;
    bed.add(satPhaseBiasRef(bed.g05, sigOf(bed.g05, "L2W")), 0.5);
    bed.add(satCodeBiasRef(bed.g05, sigOf(bed.g05, "C5Q")), -2.0);

    std::map<char, std::vector<SdEwlArc>> by_const;
    const SdEwlArc* a = bed.build(by_const);
    ASSERT_NE(a, nullptr);

    const double f1 = bed.f1, f2 = bed.f2;
    const double expect_bias = (f1 / (f1 - f2)) * 0.5 - (f2 / (f1 - f2)) * 0.0
                             - (f1 / (f1 + f2)) * 0.0 - (f2 / (f1 + f2)) * -2.0;
    EXPECT_DOUBLE_EQ(a->pred_bias_m, expect_bias);
}

TEST(SdEwlStationParamsTest, ABiasOfAnotherSatelliteOrSignalIsNotTheArcs) {
    ArcBed bed;
    const SatId g06 = satOf("G06");
    bed.add(satPhaseBiasRef(g06, sigOf(g06, "L2W")), 9.0);                       // another satellite
    bed.add(satPhaseBiasRef(bed.g05, sigOf(bed.g05, "L1C")), 9.0);               // another signal
    bed.add(satCodeBiasRef(bed.g05, sigOf(bed.g05, "C5Q")), -2.0);

    std::map<char, std::vector<SdEwlArc>> by_const;
    const SdEwlArc* a = bed.build(by_const);
    ASSERT_NE(a, nullptr);

    const double f1 = bed.f1, f2 = bed.f2;
    const double expect_bias = (f1 / (f1 - f2)) * 0.0 - (f2 / (f1 - f2)) * 0.0
                             - (f1 / (f1 + f2)) * 0.0 - (f2 / (f1 + f2)) * -2.0;
    EXPECT_DOUBLE_EQ(a->pred_bias_m, expect_bias);
}
