/**
 * @file test_dd_innovation_state.cpp
 * @brief The baseline DD build's phase innovations read the state after the
 *        build's own state writes.
 *
 * The DD overload of MeasurementModel::computeMeasurementUpdate writes the
 * filter state three times before it forms the innovations: the pivot handover
 * S-transforms and renames the DD ambiguity states, the prune removes the states
 * no row references (compacting the indices), and the births append states for
 * new phase rows.  The caller passes a copy of the state taken before the call.
 * The innovations used to read that copy whenever its size equalled the
 * state size after the writes, so a handover that changes no size, or a prune
 * balanced by births, subtracted ambiguities from the wrong basis or the wrong
 * index.  The KalmanFilter applies the update to the current state, so the
 * innovation must be formed against it.
 *
 * The oracle is physical.  The observations carry a constant phase offset per
 * satellite and band, so a state that is consistent with them gives a zero
 * phase innovation.  A newborn ambiguity is seeded at its row's pre-ambiguity
 * innovation (zero innovation), and the handover S-transform maps a consistent
 * old-pivot basis onto a consistent new-pivot basis, so after either event every
 * phase innovation of an unchanged observation set stays zero.  Read from the
 * stale copy, the handover epoch's innovations carry the old pivot's offset
 * against the new pivot, and the newborn rows carry the set satellite's
 * ambiguity.  Synthetic modeled observations avoid product files.
 */

#include <gtest/gtest.h>

#include "measurement/MeasurementModel.hpp"
#include "products/ProductParser.hpp"
#include "products/SsrCorrectionStore.hpp"
#include "rinex/RinexParser.hpp"
#include "filter/StateKeySchema.hpp"

#include <cmath>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <string>
#include <vector>

using namespace GnssProcessor;

namespace {

/// One satellite of the synthetic sky: its rover SNR (the pivot ranking) and
/// its index, which sets its phase offsets.
struct SkySat {
    std::string sat;
    double snr;
    int k;
};

class DdInnovationStateTest : public ::testing::Test {
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

    /// Load a GPS Baseline-Single configuration with the pivot handover on and
    /// build the model stack around it.
    void SetUp() override {
        const auto path = std::filesystem::temp_directory_path()
            / (std::string("dd_innov_")
               + ::testing::UnitTest::GetInstance()->current_test_info()->name() + ".json");
        {
            std::ofstream out(path);
            out << R"({
                "processing": {"mode": "Baseline-Single", "active_systems": ["GPS"],
                               "frequency_mode": "dual"},
                "baseline_fixture": "Custom",
                "baseline": {
                    "reference": {"name": "REF0", "fixed": true,
                                  "position_ecef": [6378137.0, 0.0, 0.0]},
                    "rover": {"name": "ROV0"},
                    "code_only": false,
                    "signal_aliasing": true,
                    "use_iono_free": false,
                    "estimate_differential_tropo": false,
                    "enable_pivot_handover": true
                },
                "estimation": {"ambiguity_resolution": {"mode": 0}},
                "filter": {"initial_std": {"position": 1.0, "ambiguity": 100.0}}
            })";
        }
        const bool loaded = config.loadFromJson(path, log);
        std::error_code ec;
        std::filesystem::remove(path, ec);
        ASSERT_TRUE(loaded);
        ASSERT_TRUE(config.getBaselineConfig().enable_pivot_handover);
        products = std::make_unique<ProductParser>(config, log);
        rinex = std::make_unique<RinexParser>(config, log, *products);
        states = std::make_unique<StateManager>(5, log, config);
        ppp = std::make_unique<PppModel>(config, *products, log, *rinex);
        model = std::make_unique<MeasurementModel>(*states, *ppp, config, log);
        model->setSsrStore(&store);
    }

    /// One receiver's observation of one satellite: code and phase on L1C and
    /// L2W, each code at the range and each phase at the range plus an offset
    /// that differs by satellite, band and receiver; the rover adds S1C.
    PppObservation observation(const SkySat& s, bool rover, int at_epoch) const {
        PppObservation o{};
        o.satellite = s.sat;
        o.system = System::GPS;
        o.time = GnssTime{2418, 1000.0 + 30.0 * at_epoch};
        o.rx_pos_linearization = Eigen::Vector3d(6378137.0, 0.0, 0.0);
        o.los = Eigen::Vector3d(1.0, 0.1 * s.k, 1.0 - 0.05 * s.k).normalized();
        o.range = 2.2e7 + 1.0e5 * s.k;
        o.sat_pos = o.rx_pos_linearization + o.range * o.los;
        o.elevation = 40.0 + s.k;
        o.azimuth = 30.0 * s.k;
        o.mf_w = 1.5;
        o.mf_d = 1.5;
        const double l1 = rover ? 10.0 + 3.0 * s.k : 1.0 + 0.5 * s.k;
        const double l2 = rover ? 7.0 + 5.0 * s.k : 2.0 + 0.25 * s.k;
        const auto add = [&](const std::string& signal, double offset) {
            o.modeled[signal] = o.range;
            o.observed[signal] = o.range + offset;
            o.variances[signal] = signal[0] == 'L' ? 0.000025 : 0.25;
        };
        add("C1C", 0.0);
        add("L1C", l1);
        add("C2W", 0.0);
        add("L2W", l2);
        if (rover) o.observed["S1C"] = s.snr;
        return o;
    }

    /// One DD build over @p sky with the state as the caller passes it (a copy
    /// taken before the call, as ApplicationBaseline does).
    EpochMeasurement build(const std::vector<SkySat>& sky) {
        std::vector<PppObservation> rov, ref;
        for (const auto& s : sky) {
            rov.push_back(observation(s, true, epoch));
            ref.push_back(observation(s, false, epoch));
        }
        ++epoch;
        return model->computeMeasurementUpdate(rov, ref, states->getStateVector());
    }

    /// Every phase row's innovation, keyed by "sat_i-pivot code".
    static std::map<std::string, double> phaseInnovations(const EpochMeasurement& m) {
        std::map<std::string, double> out;
        for (Eigen::Index a = 0; a < m.innovations.size(); ++a) {
            const auto& sig = m.signal_types[static_cast<size_t>(a)];
            if (sig.empty() || sig[0] != 'L') continue;
            out[m.satellite_ids[static_cast<size_t>(a)] + " " + sig] = m.innovations(a);
        }
        return out;
    }

    /// The state column of each phase row's ambiguity (the H entry 1.0 past the
    /// position block) matches the state's key for that row.
    void expectAmbiguityColumnsMatchKeys(const EpochMeasurement& m) const {
        for (Eigen::Index a = 0; a < m.innovations.size(); ++a) {
            const auto& sig = m.signal_types[static_cast<size_t>(a)];
            if (sig.empty() || sig[0] != 'L') continue;
            const auto& id = m.satellite_ids[static_cast<size_t>(a)];
            const auto dash = id.find('-');
            ASSERT_NE(dash, std::string::npos);
            const std::string key = "DD|GPS|" + id.substr(0, dash) + "|" + sig + "|"
                                  + id.substr(dash + 1);
            const int idx = states->getAmbiguityIndex(key);
            ASSERT_GE(idx, 0) << key;
            EXPECT_EQ(m.H(a, idx), 1.0) << key;
        }
    }
};

const SkySat kG01{"G01", 50.0, 1};
const SkySat kG02{"G02", 45.0, 2};
const SkySat kG03{"G03", 40.0, 3};
const SkySat kG04{"G04", 35.0, 4};

TEST_F(DdInnovationStateTest, ForcedHandoverWithoutSizeChangeKeepsPhaseInnovationsAtZero) {
    const std::vector<SkySat> sky{kG01, kG02, kG03, kG04};

    // Epoch 1: pivot G01 (highest SNR); six ambiguities born at zero innovation.
    const EpochMeasurement m1 = build(sky);
    const auto i1 = phaseInnovations(m1);
    ASSERT_EQ(i1.size(), 6u);
    for (const auto& [row, v] : i1) EXPECT_NEAR(v, 0.0, 1e-9) << row;
    // The births grew the state, so the caller's copy was never the state's size.
    EXPECT_EQ(model->ddStaleSnapshotBuilds(), 0u);
    const Eigen::Index size1 = states->getStateSize();

    // Epoch 2: the KalmanFilter's coherent pivot slip forces a re-rank without
    // G01.  G02 becomes the pivot; G01 stays in view, so the handover renames all
    // six states onto G02 and the prune and births change nothing.
    model->requestBaselinePivotRerank("GPS", {"G01"});
    const EpochMeasurement m2 = build(sky);
    ASSERT_EQ(states->getStateSize(), size1);
    ASSERT_GE(states->getAmbiguityIndex("DD|GPS|G01|L1C|G02"), 0);
    const auto i2 = phaseInnovations(m2);
    ASSERT_EQ(i2.size(), 6u);
    ASSERT_EQ(i2.count("G01-G02 L1C"), 1u);
    for (const auto& [row, v] : i2) EXPECT_NEAR(v, 0.0, 1e-9) << row;
    expectAmbiguityColumnsMatchKeys(m2);
    EXPECT_EQ(model->ddStaleSnapshotBuilds(), 1u);

    // Epoch 3: nothing changes, so the copy equals the state and is not counted.
    const EpochMeasurement m3 = build(sky);
    for (const auto& [row, v] : phaseInnovations(m3)) EXPECT_NEAR(v, 0.0, 1e-9) << row;
    EXPECT_EQ(model->ddStaleSnapshotBuilds(), 1u);
}

TEST_F(DdInnovationStateTest, BalancedPruneAndBirthSeedsTheRisingSatelliteAtZeroInnovation) {
    // Epoch 1: pivot G01; G02 and G03 born on L1C and L2W.
    const EpochMeasurement m1 = build({kG01, kG02, kG03});
    ASSERT_EQ(phaseInnovations(m1).size(), 4u);
    const Eigen::Index size1 = states->getStateSize();

    // Epoch 2: G03 sets and G04 rises.  The prune removes G03's two states and the
    // births append G04's two, so the state keeps its size with another layout.
    const EpochMeasurement m2 = build({kG01, kG02, kG04});
    ASSERT_EQ(states->getStateSize(), size1);
    EXPECT_LT(states->getAmbiguityIndex("DD|GPS|G03|L1C|G01"), 0);
    const auto i2 = phaseInnovations(m2);
    ASSERT_EQ(i2.size(), 4u);
    ASSERT_EQ(i2.count("G04-G01 L2W"), 1u);
    for (const auto& [row, v] : i2) EXPECT_NEAR(v, 0.0, 1e-9) << row;
    expectAmbiguityColumnsMatchKeys(m2);
    EXPECT_EQ(model->ddStaleSnapshotBuilds(), 1u);
}

TEST_F(DdInnovationStateTest, CodeRowInnovationIsThePreAmbiguityDoubleDifference) {
    // Code rows carry no ambiguity: with every code at the range their DD is zero
    // whatever the state, and a handover does not move them.
    build({kG01, kG02, kG03});
    model->requestBaselinePivotRerank("GPS", {"G01"});
    const EpochMeasurement m = build({kG01, kG02, kG03});
    size_t code_rows = 0;
    for (Eigen::Index a = 0; a < m.innovations.size(); ++a) {
        const auto& sig = m.signal_types[static_cast<size_t>(a)];
        if (sig.empty() || sig[0] != 'C') continue;
        ++code_rows;
        EXPECT_NEAR(m.innovations(a), 0.0, 1e-9) << m.satellite_ids[static_cast<size_t>(a)];
    }
    EXPECT_EQ(code_rows, 4u);
}

} // namespace
