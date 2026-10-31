/**
 * @file test_pivot_column_resolver.cpp
 * @brief The BSSD pivot's ambiguity columns under both ambiguity bases: the
 *        handover re-reference, the pivot freeze, the disturbance check and the
 *        S-transform's zeroed reference columns.
 *
 * Under @c estimation.state_ambiguity_basis = nl_wl a satellite's ambiguities
 * are the columns "<sat>_NL_<b>" and "<sat>_WL_<a>_<b>" (cycles), not one
 * "<sat>_<phase signal>" column per signal.  BssdTransform's step 7 zeroes the
 * pivot's NL and WL columns, so the single-difference rows treat the pivot's
 * NL/WL state as the frame's datum; PivotManager has to agree: it re-references
 * every NL/WL column of the constellation onto the new pivot at a handover,
 * freezes the pivot's NL/WL columns, and detects a pivot whose NL/WL state has
 * left (0, 0).  These cases drive PivotManager and BssdTransform directly on a
 * StateManager holding hand-placed ambiguity states, with synthetic
 * observations that carry only what pivot selection reads (satellite,
 * elevation, observed phase signals).  The per-band cases pin that the per-band
 * path is unchanged.
 */

#include <gtest/gtest.h>

#include "filter/PivotManager.hpp"
#include "filter/StateKeySchema.hpp"
#include "filter/StateManager.hpp"
#include "measurement/BssdTransform.hpp"
#include "measurement/MeasurementModel.hpp"
#include "measurement/ObservationEquation.hpp"
#include "products/ProductParser.hpp"
#include "products/SsrCorrectionStore.hpp"
#include "rinex/RinexParser.hpp"

#include <Eigen/Dense>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

using namespace GnssProcessor;

namespace {

/// The configured initial ambiguity standard deviation, which the handover
/// writes on a reset or re-seeded column.
constexpr double kAmbStd = 100.0;

class PivotColumnResolverTest : public ::testing::Test {
protected:
    LogManager log;
    GnssConfig config;
    SsrCorrectionStore store;  // empty, and isolated from the process-wide store
    std::unique_ptr<ProductParser> products;
    std::unique_ptr<RinexParser> rinex;
    std::unique_ptr<StateManager> states;
    std::unique_ptr<PppModel> ppp;
    std::unique_ptr<MeasurementModel> model;  // supplies the handover's seed input
    std::unique_ptr<PivotManager> pivots;

    /// Load an uncombined GPS configuration with BSSD on (pivot elevation
    /// threshold 35 degrees) and the given ambiguity basis.
    void load(const std::string& basis) {
        const auto path = std::filesystem::temp_directory_path()
            / (std::string("pivot_cols_")
               + ::testing::UnitTest::GetInstance()->current_test_info()->name() + ".json");
        {
            std::ofstream out(path);
            out << R"({
                "processing": {"mode": "PPP", "uncombined_enabled": true,
                               "active_systems": ["GPS"]},
                "estimation": {
                    "estimate_velocity": false,
                    "estimate_ifb": false,
                    "estimate_sat_biases": false,
                    "state_ambiguity_basis": ")" << basis << R"(",
                    "ambiguity_resolution": {"mode": 0},
                    "between_satellite_sd": {"enabled": true, "pivot_min_elevation": 35.0}
                },
                "filter": {"initial_std": {"ambiguity": 100.0}}
            })";
        }
        const bool loaded = config.loadFromJson(path, log);
        std::error_code ec;
        std::filesystem::remove(path, ec);
        ASSERT_TRUE(loaded);
        ASSERT_TRUE(config.enableBetweenSatelliteSd());
        products = std::make_unique<ProductParser>(config, log);
        rinex = std::make_unique<RinexParser>(config, log, *products);
        states = std::make_unique<StateManager>(5, log, config);
        ppp = std::make_unique<PppModel>(config, *products, log, *rinex);
        model = std::make_unique<MeasurementModel>(*states, *ppp, config, log);
        model->setSsrStore(&store);
        pivots = std::make_unique<PivotManager>(config, log);
    }

    /// A GPS satellite observed on L1W and L2W (and L5Q when @p with_l5) at
    /// @p elevation degrees.  Pivot selection reads only these fields.
    static PppObservation sat(const std::string& id, double elevation, bool with_l5 = false) {
        PppObservation o{};
        o.satellite = id;
        o.system = System::GPS;
        o.elevation = elevation;
        for (const char* s : {"C1W", "L1W", "C2W", "L2W"}) o.observed[s] = 2.0e7;
        if (with_l5) { o.observed["C5Q"] = 2.0e7; o.observed["L5Q"] = 2.0e7; }
        return o;
    }

    /// Add an ambiguity state with a value.
    int add(const std::string& key, double value) {
        return static_cast<int>(states->addAmbiguity(key, value));
    }

    int idx(const std::string& key) const { return states->getAmbiguityIndex(key); }
    double x(const std::string& key) const { return states->getStateVector()(idx(key)); }
    double P(const std::string& a, const std::string& b) const {
        return states->getCovariance()(idx(a), idx(b));
    }

    /// Replace the ambiguity block of P by a dense, correlated, positive
    /// definite matrix (every other entry left), with the rows and columns of
    /// @p frozen set to zero (the frozen pivot's signature).
    void correlate(const std::vector<std::string>& keys,
                   const std::vector<std::string>& frozen) {
        Eigen::MatrixXd Pm = states->getCovariance();
        for (size_t i = 0; i < keys.size(); ++i)
            for (size_t j = 0; j < keys.size(); ++j)
                Pm(idx(keys[i]), idx(keys[j])) =
                    (i == j) ? 4.0 + static_cast<double>(i) : 0.3 + 0.05 * static_cast<double>(i + j);
        for (const auto& k : frozen) {
            Pm.row(idx(k)).setZero();
            Pm.col(idx(k)).setZero();
        }
        states->setCovariance(Pm);
    }

    /// The S-transform T P T' of one handover written densely: every column of a
    /// group loses the new pivot's column of the same group, the pivot's own
    /// columns go to zero.
    static Eigen::MatrixXd oracle(const Eigen::MatrixXd& P0,
                                  const std::vector<std::pair<std::vector<int>, int>>& groups) {
        Eigen::MatrixXd T = Eigen::MatrixXd::Identity(P0.rows(), P0.cols());
        for (const auto& [members, piv] : groups) {
            for (const int m : members) {
                if (m == piv) T(m, m) = 0.0;
                else          T(m, piv) = -1.0;
            }
        }
        return T * P0 * T.transpose();
    }
};

// ---------------------------------------------------------------------------
// NL/WL basis
// ---------------------------------------------------------------------------

TEST_F(PivotColumnResolverTest, NlWlFreezeZeroesThePivotsNlAndWlColumns) {
    load("nl_wl");
    pivots->handlePivotHandover({sat("G01", 60.0), sat("G02", 50.0), sat("G03", 40.0)},
                                *states, model->ambiguitySeedInput(), "");
    ASSERT_EQ(pivots->getPivotSatellites().at("GPS"), "G01");

    for (const auto& s : {"G01", "G02", "G03"}) {
        add(StateKeys::makeNlAmbiguityKey(s, '1'), 0.0);
        add(StateKeys::makeWlAmbiguityKey(s, '1', '2'), 0.0);
    }
    pivots->freezePivotAmbiguityCovariance(*states);

    for (const auto& k : {"G01_NL_1", "G01_WL_1_2"}) {
        EXPECT_EQ(states->getCovariance().row(idx(k)).norm(), 0.0) << k;
        EXPECT_EQ(states->getCovariance().col(idx(k)).norm(), 0.0) << k;
    }
    EXPECT_EQ(P("G02_NL_1", "G02_NL_1"), states->getCovariance()(idx("G03_NL_1"), idx("G03_NL_1")));
    EXPECT_GT(P("G02_WL_1_2", "G02_WL_1_2"), 0.0);
}

TEST_F(PivotColumnResolverTest, NlWlHandoverReReferencesEveryNlAndWlColumn) {
    load("nl_wl");
    pivots->handlePivotHandover({sat("G01", 60.0), sat("G02", 50.0), sat("G03", 40.0)},
                                *states, model->ambiguitySeedInput(), "");
    ASSERT_EQ(pivots->getPivotSatellites().at("GPS"), "G01");

    add("G01_NL_1", 0.0);  add("G01_WL_1_2", 0.0);
    add("G02_NL_1", 5.0);  add("G02_WL_1_2", 2.0);
    add("G03_NL_1", 7.0);  add("G03_WL_1_2", -1.0);
    const std::vector<std::string> keys = {"G01_NL_1", "G01_WL_1_2", "G02_NL_1",
                                           "G02_WL_1_2", "G03_NL_1", "G03_WL_1_2"};
    correlate(keys, {"G01_NL_1", "G01_WL_1_2"});
    const Eigen::MatrixXd P0 = states->getCovariance();

    // G01 sets below the 35 degree threshold: a clean handover to G02.
    pivots->handlePivotHandover({sat("G01", 20.0), sat("G02", 50.0), sat("G03", 40.0)},
                                *states, model->ambiguitySeedInput(), "");
    ASSERT_EQ(pivots->getPivotSatellites().at("GPS"), "G02");
    ASSERT_EQ(pivots->getCleanHandovers().count("GPS"), 1u);

    EXPECT_DOUBLE_EQ(x("G02_NL_1"), 0.0);
    EXPECT_DOUBLE_EQ(x("G02_WL_1_2"), 0.0);
    EXPECT_DOUBLE_EQ(x("G03_NL_1"), 2.0);
    EXPECT_DOUBLE_EQ(x("G03_WL_1_2"), -3.0);
    EXPECT_DOUBLE_EQ(x("G01_NL_1"), -5.0);
    EXPECT_DOUBLE_EQ(x("G01_WL_1_2"), -2.0);

    const Eigen::MatrixXd expect = oracle(P0, {
        {{idx("G01_NL_1"), idx("G02_NL_1"), idx("G03_NL_1")}, idx("G02_NL_1")},
        {{idx("G01_WL_1_2"), idx("G02_WL_1_2"), idx("G03_WL_1_2")}, idx("G02_WL_1_2")},
    });
    EXPECT_LT((states->getCovariance() - expect).cwiseAbs().maxCoeff(), 1e-12);
    for (const auto& k : {"G01_NL_1", "G01_WL_1_2", "G03_NL_1", "G03_WL_1_2"})
        EXPECT_FALSE(states->isAmbiguityPendingReseed(k)) << k;
}

TEST_F(PivotColumnResolverTest, NlWlDisturbedPivotIsHandedOverAndReseeded) {
    load("nl_wl");
    pivots->handlePivotHandover({sat("G01", 60.0), sat("G02", 50.0), sat("G03", 40.0)},
                                *states, model->ambiguitySeedInput(), "");
    add("G01_NL_1", 3.0);  add("G01_WL_1_2", 0.0);   // the pivot's NL left 0: disturbed
    add("G02_NL_1", 5.0);  add("G02_WL_1_2", 2.0);
    add("G03_NL_1", 7.0);  add("G03_WL_1_2", -1.0);
    correlate({"G01_NL_1", "G01_WL_1_2", "G02_NL_1", "G02_WL_1_2", "G03_NL_1", "G03_WL_1_2"},
              {"G01_NL_1", "G01_WL_1_2"});

    // G01 stays high: only the disturbance check can move the pivot.
    pivots->handlePivotHandover({sat("G01", 60.0), sat("G02", 50.0), sat("G03", 40.0)},
                                *states, model->ambiguitySeedInput(), "");
    ASSERT_EQ(pivots->getPivotSatellites().at("GPS"), "G02");
    EXPECT_EQ(pivots->getCleanHandovers().count("GPS"), 0u);

    // The non-pivot columns move to the new frame; the disturbed old pivot's
    // columns are re-seeded by the NL/WL seeding owner on the next pre-pass.
    EXPECT_DOUBLE_EQ(x("G02_NL_1"), 0.0);
    EXPECT_DOUBLE_EQ(x("G03_NL_1"), 2.0);
    EXPECT_DOUBLE_EQ(x("G03_WL_1_2"), -3.0);
    for (const auto& k : {"G01_NL_1", "G01_WL_1_2"}) {
        EXPECT_TRUE(states->isAmbiguityPendingReseed(k)) << k;
        EXPECT_DOUBLE_EQ(P(k, k), kAmbStd * kAmbStd) << k;
        EXPECT_EQ(states->getCovariance().row(idx(k)).norm(), kAmbStd * kAmbStd) << k;
    }
}

TEST_F(PivotColumnResolverTest, NlWlColumnTheNewPivotLacksIsReset) {
    load("nl_wl");
    pivots->handlePivotHandover({sat("G01", 60.0, true), sat("G02", 50.0, true),
                                 sat("G03", 40.0, true)}, *states, model->ambiguitySeedInput(), "");
    add("G01_NL_1", 0.0);  add("G01_WL_1_2", 0.0);  add("G01_WL_1_5", 0.0);
    add("G02_NL_1", 5.0);  add("G02_WL_1_2", 2.0);
    add("G03_NL_1", 7.0);  add("G03_WL_1_2", -1.0); add("G03_WL_1_5", 4.0);
    correlate({"G01_NL_1", "G01_WL_1_2", "G01_WL_1_5", "G02_NL_1", "G02_WL_1_2",
               "G03_NL_1", "G03_WL_1_2", "G03_WL_1_5"},
              {"G01_NL_1", "G01_WL_1_2", "G01_WL_1_5"});

    // G02 tracks L5 but holds no WL_1_5 column yet (its L5 arc has not been born).
    pivots->handlePivotHandover({sat("G01", 20.0, true), sat("G02", 50.0, true),
                                 sat("G03", 40.0, true)}, *states, model->ambiguitySeedInput(), "");
    ASSERT_EQ(pivots->getPivotSatellites().at("GPS"), "G02");

    // G02 has no WL_1_5 column: the WL_1_5 columns still refer to G01.
    EXPECT_DOUBLE_EQ(x("G03_NL_1"), 2.0);
    for (const auto& k : {"G01_WL_1_5", "G03_WL_1_5"}) {
        EXPECT_TRUE(states->isAmbiguityPendingReseed(k)) << k;
        EXPECT_DOUBLE_EQ(P(k, k), kAmbStd * kAmbStd) << k;
    }
    EXPECT_FALSE(states->isAmbiguityPendingReseed("G03_NL_1"));
}

TEST_F(PivotColumnResolverTest, NlWlTransformZeroesThePivotsNlAndWlColumns) {
    load("nl_wl");
    pivots->handlePivotHandover({sat("G01", 60.0), sat("G02", 50.0)}, *states, model->ambiguitySeedInput(), "");
    for (const auto& s : {"G01", "G02"}) {
        add(StateKeys::makeNlAmbiguityKey(s, '1'), 0.0);
        add(StateKeys::makeWlAmbiguityKey(s, '1', '2'), 0.0);
    }
    const Eigen::Index n = states->getStateVector().size();

    // Four phase rows, each loading its satellite's NL column and, on L2, its WL.
    EpochMeasurement m;
    m.observations = {sat("G01", 60.0), sat("G02", 50.0)};
    m.satellite_ids = {"G01", "G01", "G02", "G02"};
    m.signal_types  = {"L1W", "L2W", "L1W", "L2W"};
    m.innovations = Eigen::VectorXd::Constant(4, 0.01);
    m.R_v = Eigen::VectorXd::Constant(4, 1e-6);
    m.H = Eigen::MatrixXd::Zero(4, n);
    for (int r = 0; r < 4; ++r) {
        const std::string& s = m.satellite_ids[static_cast<size_t>(r)];
        m.H(r, idx(s + "_NL_1")) = 0.19;
        if (r % 2 == 1) m.H(r, idx(s + "_WL_1_2")) = 0.24;
    }
    BssdTransform(config, *states, *pivots, log).apply(m, "");

    ASSERT_EQ(m.innovations.size(), 2);
    EXPECT_EQ(m.H.col(idx("G01_NL_1")).norm(), 0.0);
    EXPECT_EQ(m.H.col(idx("G01_WL_1_2")).norm(), 0.0);
    EXPECT_DOUBLE_EQ(m.H(0, idx("G02_NL_1")), 0.19);
    EXPECT_DOUBLE_EQ(m.H(1, idx("G02_WL_1_2")), 0.24);
}

// ---------------------------------------------------------------------------
// Per-band basis (unchanged by the resolver)
// ---------------------------------------------------------------------------

TEST_F(PivotColumnResolverTest, PerBandHandoverReReferencesEachSignal) {
    load("per_band");
    pivots->handlePivotHandover({sat("G01", 60.0), sat("G02", 50.0), sat("G03", 40.0)},
                                *states, model->ambiguitySeedInput(), "");
    add("G01_L1W", 0.0);  add("G01_L2W", 0.0);
    add("G02_L1W", 5.0);  add("G02_L2W", 2.0);
    add("G03_L1W", 7.0);  add("G03_L2W", -1.0);
    const std::vector<std::string> keys = {"G01_L1W", "G01_L2W", "G02_L1W",
                                           "G02_L2W", "G03_L1W", "G03_L2W"};
    correlate(keys, {"G01_L1W", "G01_L2W"});
    const Eigen::MatrixXd P0 = states->getCovariance();

    pivots->handlePivotHandover({sat("G01", 20.0), sat("G02", 50.0), sat("G03", 40.0)},
                                *states, model->ambiguitySeedInput(), "");
    ASSERT_EQ(pivots->getPivotSatellites().at("GPS"), "G02");
    EXPECT_DOUBLE_EQ(x("G03_L1W"), 2.0);
    EXPECT_DOUBLE_EQ(x("G03_L2W"), -3.0);
    EXPECT_DOUBLE_EQ(x("G01_L1W"), -5.0);
    const Eigen::MatrixXd expect = oracle(P0, {
        {{idx("G01_L1W"), idx("G02_L1W"), idx("G03_L1W")}, idx("G02_L1W")},
        {{idx("G01_L2W"), idx("G02_L2W"), idx("G03_L2W")}, idx("G02_L2W")},
    });
    EXPECT_LT((states->getCovariance() - expect).cwiseAbs().maxCoeff(), 1e-12);
}

TEST_F(PivotColumnResolverTest, PerBandFreezeAndDisturbanceUseTheSignalColumns) {
    load("per_band");
    pivots->handlePivotHandover({sat("G01", 60.0), sat("G02", 50.0)}, *states, model->ambiguitySeedInput(), "");
    add("G01_L1W", 0.0);  add("G01_L2W", 0.0);
    add("G02_L1W", 5.0);  add("G02_L2W", 2.0);
    pivots->freezePivotAmbiguityCovariance(*states);
    EXPECT_EQ(P("G01_L1W", "G01_L1W"), 0.0);
    EXPECT_EQ(P("G01_L2W", "G01_L2W"), 0.0);

    // An undisturbed pivot stays; a disturbed one is handed over.
    pivots->handlePivotHandover({sat("G01", 60.0), sat("G02", 50.0)}, *states, model->ambiguitySeedInput(), "");
    EXPECT_EQ(pivots->getPivotSatellites().at("GPS"), "G01");
    auto xv = states->getStateVector();
    xv(idx("G01_L2W")) = 0.5;
    states->setStateVector(xv);
    pivots->handlePivotHandover({sat("G01", 60.0), sat("G02", 50.0)}, *states, model->ambiguitySeedInput(), "");
    EXPECT_EQ(pivots->getPivotSatellites().at("GPS"), "G02");
}

// ---------------------------------------------------------------------------
// The resolver itself
// ---------------------------------------------------------------------------

TEST_F(PivotColumnResolverTest, ResolverNamesTheNlAndWlColumnsOfOneSatellite) {
    load("nl_wl");
    add("G01_NL_1", 0.0);  add("G01_WL_1_2", 0.0);  add("G01_WL_1_5", 0.0);
    add("G02_NL_1", 1.0);  add("G02_WL_1_2", 1.0);

    const auto cols = pivots->satelliteAmbiguityColumns(*states, "G01");
    ASSERT_EQ(cols.size(), 3u);
    EXPECT_EQ(cols[0].kind, "NL_1");
    EXPECT_EQ(cols[1].kind, "WL_1_2");
    EXPECT_EQ(cols[2].kind, "WL_1_5");
    EXPECT_EQ(cols[1].key, "G01_WL_1_2");
    EXPECT_EQ(cols[1].index, idx("G01_WL_1_2"));

    // The epoch's phase signals do not filter NL/WL columns.
    const std::set<std::string> sigs = {"L1W"};
    EXPECT_EQ(pivots->satelliteAmbiguityColumns(*states, "G01", &sigs).size(), 3u);

    // A phase row loads the NL column, and on a non-reference band the WL column.
    EXPECT_EQ(pivots->phaseRowAmbiguityColumns(*states, "G01", "L1W"),
              (std::vector<int>{idx("G01_NL_1")}));
    EXPECT_EQ(pivots->phaseRowAmbiguityColumns(*states, "G01", "L2W"),
              (std::vector<int>{idx("G01_NL_1"), idx("G01_WL_1_2")}));
    EXPECT_EQ(pivots->phaseRowAmbiguityColumns(*states, "G01", "L5Q"),
              (std::vector<int>{idx("G01_NL_1"), idx("G01_WL_1_5")}));
    EXPECT_TRUE(pivots->phaseRowAmbiguityColumns(*states, "G03", "L1W").empty());
}

TEST_F(PivotColumnResolverTest, ResolverPerBandReadsTheEpochSignalsOrEveryPhaseColumn) {
    load("per_band");
    add("G01_L1W", 0.0);  add("G01_L2W", 0.0);  add("G01_L5Q", 0.0);
    add("G02_L1W", 1.0);

    const std::set<std::string> sigs = {"L2W", "L1W", "L1C"};
    const auto on_sigs = pivots->satelliteAmbiguityColumns(*states, "G01", &sigs);
    ASSERT_EQ(on_sigs.size(), 2u);
    EXPECT_EQ(on_sigs[0].kind, "L1W");
    EXPECT_EQ(on_sigs[1].kind, "L2W");

    const auto all = pivots->satelliteAmbiguityColumns(*states, "G01");
    ASSERT_EQ(all.size(), 3u);
    EXPECT_EQ(all[2].key, "G01_L5Q");

    EXPECT_EQ(pivots->phaseRowAmbiguityColumns(*states, "G01", "L2W"),
              (std::vector<int>{idx("G01_L2W")}));
    EXPECT_TRUE(pivots->phaseRowAmbiguityColumns(*states, "G02", "L2W").empty());
}

}  // namespace
