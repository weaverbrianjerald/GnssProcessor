/**
 * @file test_measurement_model_prepass.cpp
 * @brief MeasurementModel's single-receiver path on synthetic GPS observations:
 *        the state pre-pass's births and prunes, the Jacobian row, the first
 *        phase innovation and the row variance.
 *
 * MeasurementModel is split by concern (the pre-pass, the row, the variance,
 * the DD build, the network surface).  These cases pin what a reader of the split
 * relies on: the ambiguity seed (StateManager::computeAmbiguitySeed) and the
 * Jacobian row (buildJacobianRow) are two copies of the observation equation
 * that agree at birth, so a phase row's first innovation is zero; the prune
 * follows the observed signals and spares a hold-protected key; the NL/WL
 * basis births one NL and one WL state; and a code row without .BIA or SSR
 * coverage is downweighted by the configured factor.  Synthetic modeled
 * observations avoid product files.
 */

#include <gtest/gtest.h>

#include "measurement/MeasurementModel.hpp"
#include "products/ProductParser.hpp"
#include "products/SsrCorrectionStore.hpp"
#include "rinex/RinexParser.hpp"
#include "filter/StateKeySchema.hpp"
#include "common/Constants.hpp"

#include <cmath>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>
#include "support/TestTempDir.hpp"   // per-process, per-test scratch directory

using namespace GnssProcessor;

namespace {

constexpr const char* kSat = "G12";

class MeasurementModelPrepassTest : public ::testing::Test {
protected:
    LogManager log;
    GnssConfig config;
    SsrCorrectionStore store;  // empty, and isolated from the process-wide store
    std::unique_ptr<ProductParser> products;
    std::unique_ptr<RinexParser> rinex;
    std::unique_ptr<StateManager> states;
    std::unique_ptr<PppModel> ppp;
    std::unique_ptr<MeasurementModel> model;
    int epoch = 0;

    /// Load an uncombined GPS PPP-RTK configuration with the given ambiguity
    /// basis and build the model stack around it.
    void load(const std::string& basis) {
        const auto path = gnss_test::tempDir()
            / (std::string("mm_prepass_")
               + ::testing::UnitTest::GetInstance()->current_test_info()->name() + ".json");
        {
            std::ofstream out(path);
            out << R"({
                "processing": {"mode": "ppp-rtk", "active_systems": ["GPS"]},
                "estimation": {
                    "estimate_velocity": false,
                    "estimate_phase_clock": false,
                    "estimate_ifb": false,
                    "estimate_sat_biases": false,
                    "no_bia_code_downweight": 100.0,
                    "state_ambiguity_basis": ")" << basis << R"(",
                    "ambiguity_resolution": {"mode": 0},
                    "between_satellite_sd": {"enabled": false}
                },
                "filter": {"initial_std": {
                    "position": 1.0, "clock": 1.0, "troposphere": 0.1,
                    "ionosphere": 1.0, "ambiguity": 100.0
                }}
            })";
        }
        const bool loaded = config.loadFromJson(path, log);
        std::error_code ec;
        std::filesystem::remove(path, ec);
        ASSERT_TRUE(loaded);
        ASSERT_TRUE(config.isUncombinedEnabled());
        products = std::make_unique<ProductParser>(config, log);
        rinex = std::make_unique<RinexParser>(config, log, *products);
        states = std::make_unique<StateManager>(5, log, config);
        ppp = std::make_unique<PppModel>(config, *products, log, *rinex);
        model = std::make_unique<MeasurementModel>(*states, *ppp, config, log);
        model->setSsrStore(&store);
        auto x = states->getStateVector();
        x.segment<3>(states->getPositionIndex()) = Eigen::Vector3d(6378137.0, 0.0, 0.0);
        states->setStateVector(x);
    }

    /// One satellite with code and phase on L1 and, optionally, L2.  Each phase
    /// is observed at the range plus a constant offset (the ambiguity, in
    /// metres); each code at the range.
    PppObservation observation(bool with_l2 = true) {
        PppObservation o{};
        o.satellite = kSat;
        o.system = System::GPS;
        o.time = GnssTime{2418, 1000.0 + 30.0 * epoch++};
        o.rx_pos_linearization = Eigen::Vector3d(6378137.0, 0.0, 0.0);
        o.los = Eigen::Vector3d(1.0, 1.0, 1.0).normalized();
        o.range = 2.2e7;
        o.sat_pos = o.rx_pos_linearization + o.range * o.los;
        o.elevation = 45.0;
        o.azimuth = 45.0;
        o.mf_w = std::sqrt(2.0);
        o.mf_d = std::sqrt(2.0);
        const auto add = [&](const std::string& signal, double offset) {
            o.modeled[signal] = o.range;
            o.observed[signal] = o.range + offset;
            o.variances[signal] = signal[0] == 'L' ? 0.000025 : 0.25;
        };
        add("C1W", 0.0);
        add("L1W", 12.0);
        if (with_l2) {
            add("C2W", 0.0);
            add("L2W", 7.0);
        }
        return o;
    }

    EpochMeasurement update(const PppObservation& o) {
        return model->computeMeasurementUpdate({o}, states->getStateVector());
    }
};

TEST_F(MeasurementModelPrepassTest, BirthsSeedEachPhaseRowToAZeroFirstInnovation) {
    load("per_band");
    const PppObservation o = observation();
    const EpochMeasurement m = update(o);

    const int iono = states->getIonosphereIndex(kSat);
    const int a1 = states->getAmbiguityIndex(StateKeys::makeAmbiguityKey(kSat, "L1W"));
    const int a2 = states->getAmbiguityIndex(StateKeys::makeAmbiguityKey(kSat, "L2W"));
    ASSERT_GE(iono, 0);
    ASSERT_GE(a1, 0);
    ASSERT_GE(a2, 0);

    // One row per usable signal, in the observation map's order.
    ASSERT_EQ(m.innovations.size(), 4);
    ASSERT_EQ(m.signal_types, (std::vector<std::string>{"C1W", "C2W", "L1W", "L2W"}));

    const double alpha2 = std::pow(Constants::GPS_L1_FREQ / Constants::GPS_L2_FREQ, 2);
    for (Eigen::Index r = 0; r < m.innovations.size(); ++r) {
        const std::string& sig = m.signal_types[static_cast<size_t>(r)];
        const bool phase = sig[0] == 'L';
        const double alpha = sig[1] == '1' ? 1.0 : alpha2;
        // Position partial -LOS, the wet mapping function on the zenith delay,
        // and the ionosphere coefficient +alpha on code, -alpha on phase.
        EXPECT_TRUE(m.H.row(r).segment<3>(states->getPositionIndex()).transpose()
                        .isApprox(-o.los, 1e-12)) << sig;
        EXPECT_DOUBLE_EQ(m.H(r, states->getTropoIndex()), o.mf_w) << sig;
        EXPECT_NEAR(m.H(r, iono), (phase ? -1.0 : 1.0) * alpha, 1e-12) << sig;
        if (phase) {
            // The seed and the Jacobian agree at birth: zero first innovation.
            EXPECT_NEAR(m.innovations(r), 0.0, 1e-6) << sig;
            EXPECT_DOUBLE_EQ(m.H(r, sig == "L1W" ? a1 : a2), 1.0) << sig;
            // The stochastic variance, untouched (no network term, no factor).
            EXPECT_DOUBLE_EQ(m.R_v(r), 0.000025) << sig;
        } else {
            // A code row without .BIA or SSR coverage: variance x the factor.
            EXPECT_DOUBLE_EQ(m.R_v(r), 0.25 * 100.0) << sig;
        }
    }
}

TEST_F(MeasurementModelPrepassTest, PruneFollowsTheObservedSignalsAndSparesAHeldKey) {
    load("per_band");
    update(observation());
    const std::string l1 = StateKeys::makeAmbiguityKey(kSat, "L1W");
    const std::string l2 = StateKeys::makeAmbiguityKey(kSat, "L2W");
    ASSERT_GE(states->getAmbiguityIndex(l2), 0);

    // L2 held across a tracking gap (soft-LLI bridge): its state survives the
    // epoch without an L2 observation.
    model->setProtectedAmbiguityKeys({l2});
    update(observation(false));
    EXPECT_GE(states->getAmbiguityIndex(l2), 0);

    // The hold released: the next epoch without L2 prunes it, and L1 stays.
    model->setProtectedAmbiguityKeys({});
    const EpochMeasurement m = update(observation(false));
    EXPECT_LT(states->getAmbiguityIndex(l2), 0);
    EXPECT_GE(states->getAmbiguityIndex(l1), 0);
    EXPECT_EQ(m.signal_types, (std::vector<std::string>{"C1W", "L1W"}));
}

TEST_F(MeasurementModelPrepassTest, NlWlBasisBirthsOneNarrowLaneAndOneWideLane) {
    load("nl_wl");
    const EpochMeasurement m = update(observation());

    EXPECT_GE(states->getAmbiguityIndex(StateKeys::makeNlAmbiguityKey(kSat, '1')), 0);
    EXPECT_GE(states->getAmbiguityIndex(StateKeys::makeWlAmbiguityKey(kSat, '1', '2')), 0);
    EXPECT_LT(states->getAmbiguityIndex(StateKeys::makeAmbiguityKey(kSat, "L1W")), 0);
    EXPECT_LT(states->getAmbiguityIndex(StateKeys::makeAmbiguityKey(kSat, "L2W")), 0);

    // Both phase rows project onto NL and WL: lambda_b * (NL - s * WL)
    // reproduces each band's seed, so the first innovations are zero.
    ASSERT_EQ(m.innovations.size(), 4);
    for (Eigen::Index r = 0; r < m.innovations.size(); ++r) {
        const std::string& sig = m.signal_types[static_cast<size_t>(r)];
        if (sig[0] == 'L') EXPECT_NEAR(m.innovations(r), 0.0, 1e-6) << sig;
    }
}

}  // namespace
