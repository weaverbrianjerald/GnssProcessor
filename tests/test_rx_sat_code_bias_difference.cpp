/**
 * @file test_rx_sat_code_bias_difference.cpp
 * @brief The between-satellite difference keeps the receiver-satellite code-bias columns: +1 on the
 *        satellite's column and -1 on the pivot's, nothing zeroed.
 *
 * Two views of the same fact.  Through the model: the differenced code row of a satellite against
 * the pivot, as the filter builds it, loads both columns.  Through the transform alone: a hand-made
 * undifferenced row set, in which the satellite code-bias column of the pivot is zeroed (step
 * 6c) and the receiver-satellite column of the pivot is not.
 */

#include <gtest/gtest.h>

#include "filter/StateKeySchema.hpp"
#include "measurement/BssdTransform.hpp"
#include "measurement/MeasurementModel.hpp"
#include "products/SsrCorrectionStore.hpp"
#include "support/ModelStack.hpp"

#include <Eigen/Dense>

#include <string>
#include <vector>

using namespace GnssProcessor;

namespace {

class RxSatCodeBiasDifferenceTest : public ::testing::Test {
protected:
    LogManager log;
    gnss_test::ModelStack stack;
    int epoch = 0;

    StateManager& states() { return *stack.states; }

    void load(const std::string& estimation) {
        ASSERT_TRUE(stack.load(log, ::testing::UnitTest::GetInstance()->current_test_info()->name(),
                               "ppp-rtk", "", estimation, /*bssd=*/true));
    }

    void cover(const std::string& sat, const std::vector<std::string>& signals) {
        SsrHeader header;
        SsrCodeBias cb;
        cb.sat = sat;
        for (const std::string& s : signals) cb.bias_per_signal[s] = 0.1;
        cb.received_time = GnssTime{ 2418, 1000.0 };
        stack.store.pushCodeBias(cb, header);
    }

    std::vector<PppObservation> sky() {
        return {
            gnss_test::syntheticObservation("G15", System::GPS, { "C1W", "L1W", "C2W", "L2W" }, epoch, 200.0, 70.0),
            gnss_test::syntheticObservation("G20", System::GPS, { "C1W", "L1W", "C2W", "L2W" }, epoch, 300.0, 50.0),
            gnss_test::syntheticObservation("G12", System::GPS, { "C1W", "L1W", "C2W", "L2W" }, epoch, 100.0, 40.0),
        };
    }

    EpochMeasurement update() {
        ++epoch;
        return stack.model->computeMeasurementUpdate(sky(), states().getStateVector());
    }

    /// The row of the differenced observation "<sat>-<pivot>" on @p signal.
    static Eigen::Index rowOf(const EpochMeasurement& m, const std::string& pair, const std::string& signal) {
        for (Eigen::Index r = 0; r < static_cast<Eigen::Index>(m.satellite_ids.size()); ++r)
            if (m.satellite_ids[static_cast<std::size_t>(r)] == pair
                && m.signal_types[static_cast<std::size_t>(r)] == signal)
                return r;
        return -1;
    }
};

}  // namespace

TEST_F(RxSatCodeBiasDifferenceTest, TheDifferencedCodeRowLoadsThePlusOneOnTheSatelliteAndMinusOneOnThePivot) {
    load(R"(, "estimate_rx_sat_code_bias": true)");
    for (const char* sat : { "G15", "G20", "G12" }) cover(sat, { "C1W", "C2W" });
    const EpochMeasurement m = update();

    const std::string pivot = stack.model->getBssdPivotSatellites().at("GPS");
    ASSERT_EQ(pivot, "G15");   // the highest satellite
    for (const char* sat : { "G20", "G12" })
        for (const char* signal : { "C1W", "C2W" }) {
            const Eigen::Index row = rowOf(m, std::string(sat) + "-" + pivot, signal);
            ASSERT_GE(row, 0) << sat << " " << signal;
            const int own = states().getRxSatCodeBiasIndex(std::string(sat) + "|" + signal);
            const int ref = states().getRxSatCodeBiasIndex(pivot + "|" + signal);
            ASSERT_GE(own, 0);
            ASSERT_GE(ref, 0);
            EXPECT_EQ(m.H(row, own), +1.0) << sat << " " << signal;
            EXPECT_EQ(m.H(row, ref), -1.0) << sat << " " << signal;
            // No other column of the family is in the row.
            for (const auto& [key, idx] : states().getRxSatCodeBiasStates())
                if (static_cast<int>(idx) != own && static_cast<int>(idx) != ref)
                    EXPECT_EQ(m.H(row, static_cast<Eigen::Index>(idx)), 0.0) << key;
        }
    // The two signals have columns of their own.
    EXPECT_NE(states().getRxSatCodeBiasIndex("G20|C1W"), states().getRxSatCodeBiasIndex("G20|C2W"));
    // A differenced phase row carries none of them.
    const Eigen::Index phase = rowOf(m, "G20-" + pivot, "L1W");
    ASSERT_GE(phase, 0);
    for (const auto& [key, idx] : states().getRxSatCodeBiasStates())
        EXPECT_EQ(m.H(phase, static_cast<Eigen::Index>(idx)), 0.0) << key;
}

TEST_F(RxSatCodeBiasDifferenceTest, TheTransformZeroesThePivotsSatelliteBiasColumnButNotItsReceiverSatelliteColumn) {
    load(R"(, "estimate_rx_sat_code_bias": true, "estimate_sat_biases": true)");
    // C1W is covered (the family's state), C2W is not (the satellite code-bias state).
    for (const char* sat : { "G15", "G20" }) cover(sat, { "C1W" });
    update();
    const PivotManager& pivots = stack.model->getPivotManager();
    ASSERT_EQ(pivots.getPivotSatellites().at("GPS"), "G15");
    ASSERT_GE(states().getRxSatCodeBiasIndex("G15|C1W"), 0);
    ASSERT_GE(states().getSatBiasIndex("G15|C2W"), 0);

    // Four code rows by hand: each satellite on each signal, a 1 on the column of its own bias.
    const Eigen::Index n = static_cast<Eigen::Index>(states().getStateSize());
    EpochMeasurement m;
    m.observations = { sky()[0], sky()[1] };
    m.satellite_ids = { "G15", "G15", "G20", "G20" };
    m.signal_types  = { "C1W", "C2W", "C1W", "C2W" };
    m.innovations = Eigen::VectorXd::Constant(4, 0.01);
    m.R_v = Eigen::VectorXd::Constant(4, 0.25);
    m.H = Eigen::MatrixXd::Zero(4, n);
    for (int r = 0; r < 4; ++r) {
        const std::string& sat = m.satellite_ids[static_cast<std::size_t>(r)];
        const bool c1 = m.signal_types[static_cast<std::size_t>(r)] == "C1W";
        m.H(r, c1 ? states().getRxSatCodeBiasIndex(sat + "|C1W") : states().getSatBiasIndex(sat + "|C2W")) = 1.0;
    }
    BssdTransform(stack.config, states(), pivots, log).apply(m, "");

    ASSERT_EQ(m.innovations.size(), 2);
    ASSERT_EQ(m.signal_types[0], "C1W");
    ASSERT_EQ(m.signal_types[1], "C2W");
    // C1W row: +1 on G20, -1 on the pivot's receiver-satellite column.
    EXPECT_EQ(m.H(0, states().getRxSatCodeBiasIndex("G20|C1W")), +1.0);
    EXPECT_EQ(m.H(0, states().getRxSatCodeBiasIndex("G15|C1W")), -1.0);
    // C2W row: the pivot's satellite code-bias column is zeroed (step 6c), G20's stays.
    EXPECT_EQ(m.H(1, states().getSatBiasIndex("G20|C2W")), +1.0);
    EXPECT_EQ(m.H(1, states().getSatBiasIndex("G15|C2W")), 0.0);
}
