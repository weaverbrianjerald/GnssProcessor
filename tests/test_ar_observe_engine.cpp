/**
 * @file test_ar_observe_engine.cpp
 * @brief The consumer engine's observe-mode integer search: which ambiguity states enter it,
 *        the chain gate (golden and untokenized wires give nothing), the hand-off of the
 *        epoch, and that it reads the filter without changing it.
 *
 * The state is a real StateManager with ambiguity states and a hand-built covariance, so the
 * seam reads exactly what the consumer's filter would hold; the seam is entered through
 * RealTimeEngine::resolveAmbiguities, the call site of the production epoch.  The
 * measurement update and the legacy resolver are not run: with the AR mode 0 the engine
 * builds no resolver, which is the configuration the search is built for.
 */

#include <gtest/gtest.h>

#include "realtime/RealTimeEngine.hpp"
#include "support/SameBits.hpp"
#include "support/TestTempDir.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace GnssProcessor {

// House friend-shim idiom: expose only the engine seams this suite needs.
struct ArObserveTestAccess {
    static void initialize(RealTimeEngine& e, const SsrCorrectionStore& store) {
        e.product_parser_ = std::make_unique<ProductParser>(e.config_, e.log_manager_);
        e.rinex_parser_ = std::make_unique<RinexParser>(
            e.config_, e.log_manager_, *e.product_parser_);
        e.state_manager_ = std::make_unique<StateManager>(5, e.log_manager_, e.config_);
        e.ppp_model_ = std::make_unique<PppModel>(
            e.config_, *e.product_parser_, e.log_manager_, *e.rinex_parser_);
        e.ssr_store_override_ = &store;
    }
    static StateManager& states(RealTimeEngine& e) { return *e.state_manager_; }

    /// One epoch of the estimation tail's AR step: the production call site, so the
    /// placement of the observe seam at the top of resolveAmbiguities is covered too.
    static void resolve(RealTimeEngine& e, const std::vector<PppObservation>& obs,
                        const GnssTime& t, bool update_applied = true) {
        const std::vector<RinexObservation> raw;
        RealTimeEngine::RtEpochContext ctx{ raw };
        ctx.modeled_obs = obs;
        ctx.current_time = t;
        ctx.update_applied = update_applied;
        e.resolveAmbiguities(ctx);
    }
    /// The first seam of the next epoch, which empties the previous epoch's search output.
    static void startNextEpoch(RealTimeEngine& e, const GnssTime& t) {
        const std::vector<RinexObservation> raw;
        RealTimeEngine::RtEpochContext ctx{ raw };
        ctx.current_time = t;
        e.admitBacklogAndSync(ctx);
    }
    static const ArObserveEpoch& buffer(const RealTimeEngine& e) { return e.ar_observe_epoch_; }
    static bool lastArRan(const RealTimeEngine& e) { return e.last_ar_ran_; }
    static bool hasResolver(const RealTimeEngine& e) { return static_cast<bool>(e.ambiguity_resolver_); }
};

namespace {

constexpr double kC  = 299792458.0;
constexpr double kL1 = kC / 1575.42e6;
constexpr double kL2 = kC / 1227.60e6;

const GnssTime kNow{ 2313, 261600.0 };

struct Sat {
    const char* name;
    double elevation;
};
// Five healthy GPS satellites; G01 is the highest, so it is the reference of every group.
const Sat kSats[] = { {"G01", 70.0}, {"G02", 55.0}, {"G03", 45.0}, {"G04", 35.0}, {"G05", 25.0} };

/// Build the JSON of a derived consumer.  @p extra_closed_loop is spliced into the closed_loop
/// object, @p gate sets network.require_signal_integer_for_ar.
std::string configJson(bool observe, const std::string& search, bool gate,
                       const std::string& extra_closed_loop = "") {
    std::string cl = std::string("\"ar_mode\": 0, \"ar_observe\": ") + (observe ? "true" : "false")
                   + ", \"ar_search\": \"" + search + "\"" + extra_closed_loop;
    return std::string(R"({
      "processing": { "mode": "ppp-rtk-network", "active_systems": ["GPS"],
                      "signal_priorities": { "GPS": "WC" },
                      "enable_ssr_bias_max_age": true, "ssr_bias_max_age_s": 60.0 },
      "estimation": { "ambiguity_resolution": { "elevation_mask": 20.0 } },
      "network": {
        "clock_datum_station": "AAAA",
        "require_signal_integer_for_ar": )") + (gate ? "true" : "false") + R"(,
        "closed_loop": { )" + cl + R"( },
        "stations": [
          { "name": "AAAA", "position_ecef": [ -2493304.99, -4655214.79, 3565497.67 ] },
          { "name": "BBBB", "position_ecef": [ -2281621.75, -1453585.08, 5756964.89 ] }
        ]
      }
    })";
}

class ArObserveEngineTest : public ::testing::Test {
protected:
    LogManager log;
    GnssConfig consumer;                 // the derived config the engine runs on
    SsrCorrectionStore store;
    std::unique_ptr<RealTimeEngine> engine;

    void build(bool observe, const std::string& search = "decorrelated_partial", bool gate = false,
               const std::string& extra = "") {
        const auto path = gnss_test::tempDir() / "ar_observe_engine.json";
        {
            std::ofstream out(path);
            out << configJson(observe, search, gate, extra);
        }
        GnssConfig base;
        const bool loaded = base.loadFromJson(path, log);
        std::error_code ec;
        std::filesystem::remove(path, ec);
        ASSERT_TRUE(loaded);
        engine.reset();                  // the engine holds a reference to the config it replaces
        consumer = base.deriveClosedLoopConsumerConfig();
        ASSERT_EQ(consumer.arObserve(), observe);
        engine = std::make_unique<RealTimeEngine>(consumer, log);
        ArObserveTestAccess::initialize(*engine, store);
    }

    StateManager& states() { return ArObserveTestAccess::states(*engine); }

    /// Add one ambiguity at @p cycles + noise-free float value; returns its key.
    std::string addAmb(const std::string& sat, const std::string& signal, double cycles,
                       double sigma_cyc) {
        const double lam = (signal[1] == '1') ? kL1 : kL2;
        const std::string key = sat + "_" + signal;
        states().addAmbiguity(key, cycles * lam, sigma_cyc * lam);
        return key;
    }

    /// The five healthy satellites on L1C and L2W: floats a few hundredths of a cycle from
    /// integers (a different fraction per satellite, so no single difference is an exact
    /// integer), sigma 0.01 cycle.  Satellite k's integers are (10 k + 3, 7 k - 2).
    void healthyState() {
        for (int k = 0; k < 5; ++k) {
            addAmb(kSats[k].name, "L1C", 10.0 * k + 3.0 + 0.010 + 0.012 * k, 0.01);
            addAmb(kSats[k].name, "L2W", 7.0 * k - 2.0 - 0.020 - 0.007 * k, 0.01);
        }
        finishCovariance();
    }

    /// Position block diagonal with @p sigma_pos per axis (the default keeps the 3D sigma well
    /// under the search's 0.20 m bar), ambiguity block as added (diagonal), and a weak position
    /// to ambiguity correlation, different for each ambiguity and not linear in the state index
    /// (two ambiguities with the same correlation would difference it away, and a linear pattern
    /// against the linear float fractions of healthyState can cancel exactly), so that
    /// conditioning moves the position.
    void finishCovariance(double sigma_pos = 0.01) {
        const Eigen::Index n = static_cast<Eigen::Index>(states().getStateSize());
        Eigen::MatrixXd P = states().getCovariance();
        ASSERT_EQ(P.rows(), n);
        for (const auto& [key, idx] : states().getAmbiguities()) {
            (void)key;
            const double sigma = std::sqrt(P(static_cast<Eigen::Index>(idx), static_cast<Eigen::Index>(idx)));
            for (int d = 0; d < 3; ++d) {
                const double rho = 0.1 * (d + 1) / 3.0
                                 * (1.0 + 0.1 * static_cast<double>((idx * idx * 3 + 1) % 7));   // at most 0.16
                const double c = rho * sigma_pos * sigma;
                P(d, static_cast<Eigen::Index>(idx)) = P(static_cast<Eigen::Index>(idx), d) = c;
            }
        }
        for (int d = 0; d < 3; ++d) P(d, d) = sigma_pos * sigma_pos;
        states().setCovariance(P);
        Eigen::VectorXd x = states().getStateVector();
        x.segment<3>(0) = Eigen::Vector3d(-2493304.0, -4655214.0, 3565497.0);
        states().setStateVector(x);
    }

    std::vector<PppObservation> observations() const {
        std::vector<PppObservation> out;
        for (const Sat& s : kSats) {
            PppObservation o{};
            o.satellite = s.name;
            o.system = System::GPS;
            o.time = kNow;
            o.elevation = s.elevation;
            out.push_back(o);
        }
        return out;
    }

    /// Push the satellite phase biases of every healthy satellite, integer-grade unless named.
    void pushBiases(const std::vector<std::string>& non_integer = {}, std::uint32_t gen_g02_l1c = 5) {
        for (const Sat& s : kSats) {
            SsrPhaseBias rec;
            rec.sat = s.name;
            rec.received_time = kNow;
            const bool integer =
                std::find(non_integer.begin(), non_integer.end(), s.name) == non_integer.end();
            for (const char* sig : { "L1C", "L2W" }) {
                SsrPhaseBiasEntry e;
                e.bias_m = 0.1;
                e.signal_integer = integer;
                if (std::string(s.name) == "G02" && std::string(sig) == "L1C")
                    e.hold_generation = gen_g02_l1c;
                rec.bias_per_signal[sig] = e;
            }
            SsrHeader h;
            h.epoch_time = kNow;
            h.system = System::GPS;
            store.pushPhaseBias(rec, h);
        }
    }

    WireChainContext wire(int amb_datum, const std::map<std::string, std::uint32_t>& cmp) {
        WireChainContext w;
        w.amb_datum = amb_datum;
        w.cmp = cmp;
        return w;
    }
    /// A component token for every key of the satellites in @p comp (component number per satellite).
    static std::map<std::string, std::uint32_t> components(
        const std::vector<std::pair<std::string, std::uint32_t>>& comp) {
        std::map<std::string, std::uint32_t> m;
        for (const auto& [sat, c] : comp)
            for (const char* sig : { "L1C", "L2W" }) m[sat + "|" + sig] = c;
        return m;
    }

    void resolve() { ArObserveTestAccess::resolve(*engine, observations(), kNow); }
    const ArObserveEpoch& buffer() { return ArObserveTestAccess::buffer(*engine); }
};

// ---------------------------------------------------------------------------------------
// Off, and what it leaves alone
// ---------------------------------------------------------------------------------------

TEST_F(ArObserveEngineTest, TheKnobOffReadsAndKeepsNothing) {
    build(/*observe=*/false);
    healthyState();
    EXPECT_FALSE(engine->arObserveArmed());
    resolve();
    ArObserveTestAccess::startNextEpoch(*engine, kNow);
    EXPECT_FALSE(engine->arObserveEpoch().valid);
    EXPECT_TRUE(engine->arObserveEpoch().rows.empty());
    const ArObserveCounters& c = engine->arObserveCounters();
    EXPECT_EQ(c.epochs, 0);
    EXPECT_EQ(c.candidates, 0);
    EXPECT_FALSE(ArObserveTestAccess::buffer(*engine).valid);
}

TEST_F(ArObserveEngineTest, ArmedSearchLeavesTheFilterBitIdentical) {
    // The same state run through the tail's AR step with the search off and on, AR mode 0:
    // the state vector, covariance and ambiguity map are the same in every bit, before and
    // after, and neither engine built a resolver or ran one.
    struct Snapshot {
        Eigen::VectorXd x0, x1;
        Eigen::MatrixXd P0, P1;
        std::map<std::string, std::size_t> map0, map1;
        bool resolver{ false }, ar_ran{ false };
        long long epochs{ 0 };
        std::size_t rows{ 0 };
    };
    auto run = [&](bool observe) {
        build(observe);
        healthyState();
        Snapshot s;
        s.x0 = states().getStateVector();
        s.P0 = states().getCovariance();
        s.map0 = states().getAmbiguities();
        resolve();
        s.x1 = states().getStateVector();
        s.P1 = states().getCovariance();
        s.map1 = states().getAmbiguities();
        s.resolver = ArObserveTestAccess::hasResolver(*engine);
        s.ar_ran = ArObserveTestAccess::lastArRan(*engine) || engine->lastArRan()
                || engine->lastArResult().success;
        s.epochs = engine->arObserveCounters().epochs;
        s.rows = buffer().rows.size();
        return s;
    };
    auto sameMatrix = [](const Eigen::MatrixXd& a, const Eigen::MatrixXd& b) {
        return a.rows() == b.rows() && a.cols() == b.cols()
            && std::memcmp(a.data(), b.data(), sizeof(double) * static_cast<std::size_t>(a.size())) == 0;
    };
    const Snapshot off = run(false);
    const Snapshot on = run(true);

    EXPECT_TRUE(gnss_test::sameBits(off.x1, off.x0));
    EXPECT_TRUE(gnss_test::sameBits(on.x1, on.x0));
    EXPECT_TRUE(sameMatrix(off.P1, off.P0));
    EXPECT_TRUE(sameMatrix(on.P1, on.P0));
    EXPECT_EQ(on.map1, on.map0);
    EXPECT_TRUE(gnss_test::sameBits(on.x1, off.x1));
    EXPECT_TRUE(sameMatrix(on.P1, off.P1));
    EXPECT_EQ(on.map1, off.map1);
    EXPECT_FALSE(off.resolver);
    EXPECT_FALSE(on.resolver);
    EXPECT_FALSE(off.ar_ran);
    EXPECT_FALSE(on.ar_ran);

    // The armed engine did search: the instrument is not silently inert.
    EXPECT_EQ(off.epochs, 0);
    EXPECT_EQ(on.epochs, 1);
    EXPECT_EQ(off.rows, 0u);
    EXPECT_GT(on.rows, 0u);
}

// ---------------------------------------------------------------------------------------
// Which ambiguity states enter
// ---------------------------------------------------------------------------------------

TEST_F(ArObserveEngineTest, HealthyStatesAllEnterAndTheReferenceIsTheHighestElevation) {
    build(true);
    healthyState();
    resolve();
    const ArObserveCounters& c = engine->arObserveCounters();
    EXPECT_EQ(c.candidates, 10);
    EXPECT_EQ(c.eligible, 10);
    EXPECT_EQ(c.gated_out, 0);
    ASSERT_TRUE(buffer().valid);
    // 4 SDs per band, 2 bands: 8 band rows; 4 satellite pairs, each with a wide lane.
    EXPECT_EQ(buffer().summary.n_entries, 8);
    EXPECT_EQ(buffer().summary.n_pairs, 4);
    EXPECT_EQ(buffer().rows.size(), 12u);
    for (const ArFixRow& r : buffer().rows) {
        EXPECT_EQ(r.ref_sat, "G01");
        EXPECT_EQ(r.system, 'G');
    }
}

TEST_F(ArObserveEngineTest, ResolverExclusionsAreTheResolversOwn) {
    // The states the resolver would not pair stay out: a zero value (an uninitialized seed), a
    // zero variance (a frozen pivot), a satellite under the AR mask, an unresolvable
    // ambiguity, an iono-free key and GLONASS.
    build(true);
    healthyState();
    addAmb("G06", "L1C", 0.0, 0.01);            // value 0
    addAmb("G07", "L1C", 12.03, 0.01);          // variance 0 (set below)
    addAmb("G08", "L1C", 14.03, 0.01);          // 10 degrees: under the 20 degree mask
    addAmb("G09", "L1C", 16.03, 0.01);          // unresolvable
    addAmb("R05", "L1C", 18.03, 0.01);          // GLONASS
    states().addAmbiguity("G01_L1C_L5Q", 30.0, 0.1);   // iono-free combination
    states().markAmbiguityUnresolvable("G09_L1C");
    {
        Eigen::MatrixXd P = states().getCovariance();
        const auto idx7 = static_cast<Eigen::Index>(states().getAmbiguityIndex("G07_L1C"));
        P.row(idx7).setZero();
        P.col(idx7).setZero();
        states().setCovariance(P);
    }
    std::vector<PppObservation> obs = observations();
    for (const char* extra : { "G06", "G07", "G08", "G09", "R05" }) {
        PppObservation o{};
        o.satellite = extra;
        o.system = (extra[0] == 'R') ? System::GLONASS : System::GPS;
        o.time = kNow;
        o.elevation = std::string(extra) == "G08" ? 10.0 : 40.0;
        obs.push_back(o);
    }
    ArObserveTestAccess::resolve(*engine, obs, kNow);
    EXPECT_EQ(engine->arObserveCounters().candidates, 10);     // only the five healthy satellites
    ASSERT_TRUE(buffer().valid);
    for (const ArFixRow& r : buffer().rows) {
        EXPECT_TRUE(r.sat != "G06" && r.sat != "G07" && r.sat != "G08" && r.sat != "G09"
                    && r.sat != "R05");
    }
}

TEST_F(ArObserveEngineTest, TheArConstellationListFiltersTheCandidates) {
    // GPS only; a Galileo pair is left out of the search.
    build(true, "full", false, ", \"ar_active_systems\": [\"GPS\"]");
    healthyState();
    addAmb("E01", "L1C", 20.03, 0.01);
    addAmb("E02", "L1C", 30.03, 0.01);
    finishCovariance();
    std::vector<PppObservation> obs = observations();
    for (const char* e : { "E01", "E02" }) {
        PppObservation o{};
        o.satellite = e;
        o.system = System::Galileo;
        o.time = kNow;
        o.elevation = 50.0;
        obs.push_back(o);
    }
    ArObserveTestAccess::resolve(*engine, obs, kNow);
    EXPECT_EQ(engine->arObserveCounters().candidates, 10);
}

TEST_F(ArObserveEngineTest, RowsAreTheStateManagersFloatAndCovarianceDifferenced) {
    // Loose floats (0.5 cycle sigma), so the full search fixes nothing and each row reports the
    // float itself and its formal sigma: D x / lambda and sqrt(D P D^T) / lambda, with D written
    // out from the manager's own indices.
    build(true, "full");
    for (int k = 0; k < 3; ++k) addAmb(kSats[k].name, "L1C", 100.0 + 13.37 * k + 0.2, 0.5);
    finishCovariance();
    resolve();
    ASSERT_TRUE(buffer().valid);
    ASSERT_EQ(buffer().rows.size(), 2u);                // G02-G01 and G03-G01; one band, no wide lane
    EXPECT_FALSE(buffer().summary.accepted);
    EXPECT_EQ(buffer().summary.fix_ecef, buffer().summary.float_ecef);

    const Eigen::VectorXd x = states().getStateVector();
    const Eigen::MatrixXd P = states().getCovariance();
    const int i0 = states().getAmbiguityIndex("G01_L1C");
    const int i1 = states().getAmbiguityIndex("G02_L1C");
    const int i2 = states().getAmbiguityIndex("G03_L1C");
    ASSERT_GE(i0, 0);
    ASSERT_GE(i1, 0);
    ASSERT_GE(i2, 0);
    Eigen::MatrixXd D = Eigen::MatrixXd::Zero(2, x.size());
    D(0, i1) = 1.0;  D(0, i0) = -1.0;
    D(1, i2) = 1.0;  D(1, i0) = -1.0;
    const Eigen::VectorXd a = D * x / kL1;
    const Eigen::MatrixXd Q = D * P * D.transpose() / (kL1 * kL1);
    ASSERT_EQ(buffer().rows[0].sat, "G02");
    ASSERT_EQ(buffer().rows[1].sat, "G03");
    for (int i = 0; i < 2; ++i) {
        const ArFixRow& r = buffer().rows[static_cast<std::size_t>(i)];
        EXPECT_EQ(r.ref_sat, "G01");
        EXPECT_NEAR(r.float_cyc, a(i), 1e-9);
        EXPECT_NEAR(r.cond_sigma_cyc, std::sqrt(Q(i, i)), 1e-9);
        EXPECT_NEAR(r.cond_sigma_cyc, std::sqrt(0.5), 1e-6);    // two independent 0.5 cycle sigmas
        EXPECT_FALSE(r.determined);
        EXPECT_FALSE(r.kept);
    }
}

TEST_F(ArObserveEngineTest, ThePositionIsTheStateManagersPositionBlock) {
    build(true);
    healthyState();
    resolve();
    ASSERT_TRUE(buffer().valid);
    ASSERT_EQ(states().getPositionIndex(), 0u);
    const Eigen::VectorXd x = states().getStateVector();
    EXPECT_DOUBLE_EQ(buffer().summary.float_ecef.x(), x(0));
    EXPECT_DOUBLE_EQ(buffer().summary.float_ecef.y(), x(1));
    EXPECT_DOUBLE_EQ(buffer().summary.float_ecef.z(), x(2));
}

// ---------------------------------------------------------------------------------------
// The search through the engine
// ---------------------------------------------------------------------------------------

TEST_F(ArObserveEngineTest, TightFloatsAreFixedAndConditionThePosition) {
    for (const std::string search : { "full", "decorrelated_partial" }) {
        SCOPED_TRACE(search);
        build(true, search);
        healthyState();
        resolve();
        ASSERT_TRUE(buffer().valid);
        const ArEpochSummary& s = buffer().summary;
        EXPECT_TRUE(s.accepted);
        EXPECT_GT(s.z_fixed, 0);
        EXPECT_GT(s.n_band_det, 0);
        EXPECT_TRUE(s.conditioned);
        EXPECT_GT((s.fix_ecef - s.float_ecef).norm(), 0.0);
        for (const ArFixRow& r : buffer().rows) {
            if (r.signal == "WL") continue;
            const int k = (r.sat[2] - '0') - 1;                       // G02 -> 1
            const long long expected = (r.signal == "L1C") ? 10 * k + 3 : 7 * k - 2;
            const long long ref_expected = (r.signal == "L1C") ? 3 : -2;
            EXPECT_EQ(r.fixed_int, expected - ref_expected) << r.sat << " " << r.signal;
        }
    }
}

TEST_F(ArObserveEngineTest, AnUnconvergedPositionIsGatedLikeTheResolver) {
    // The search is held back until the float has converged, to the larger of the resolver's bar
    // (estimation.ambiguity_resolution.max_position_sigma_m, 0.05 m here) and 0.20 m: a 3D sigma
    // of 0.5 m records the epoch's float position and nothing else.
    build(true);
    for (int k = 0; k < 5; ++k) {
        addAmb(kSats[k].name, "L1C", 10.0 * k + 3.0 + 0.010 + 0.012 * k, 0.01);
        addAmb(kSats[k].name, "L2W", 7.0 * k - 2.0 - 0.020 - 0.007 * k, 0.01);
    }
    finishCovariance(/*sigma_pos=*/0.5 / std::sqrt(3.0));
    resolve();
    ASSERT_TRUE(buffer().valid);
    EXPECT_TRUE(buffer().summary.gated);
    EXPECT_TRUE(buffer().rows.empty());
    EXPECT_EQ(buffer().summary.fix_ecef, buffer().summary.float_ecef);
    const ArObserveCounters& c = engine->arObserveCounters();
    EXPECT_EQ(c.epochs, 1);
    EXPECT_EQ(c.epochs_gated, 1);
    EXPECT_EQ(c.epochs_searched, 0);
    EXPECT_EQ(c.candidates, 10);
    EXPECT_TRUE(c.closureHolds());
    // The same state with a converged position is searched.
    finishCovariance(0.01);
    resolve();
    EXPECT_FALSE(buffer().summary.gated);
    EXPECT_EQ(engine->arObserveCounters().epochs_searched, 1);
    // A 3D sigma of 0.1 m is above the resolver's own 0.05 m bar and under the 0.20 m the
    // observer waits for, so the search runs.
    finishCovariance(0.1 / std::sqrt(3.0));
    resolve();
    EXPECT_FALSE(buffer().summary.gated);
    EXPECT_EQ(engine->arObserveCounters().epochs_searched, 2);
    EXPECT_EQ(engine->arObserveCounters().epochs_gated, 1);
    EXPECT_TRUE(engine->arObserveCounters().closureHolds());
}

TEST_F(ArObserveEngineTest, AnEpochWithoutAnAppliedUpdateSearchesNothing) {
    build(true);
    healthyState();
    ArObserveTestAccess::resolve(*engine, observations(), kNow, /*update_applied=*/false);
    EXPECT_FALSE(buffer().valid);
    EXPECT_FALSE(engine->arObserveEpoch().valid);
    EXPECT_EQ(engine->arObserveCounters().epochs, 0);
}

TEST_F(ArObserveEngineTest, TheNextEpochStartsWithAnEmptyBuffer) {
    // An epoch that one of the gates ends never reaches the search stage, so the harness must
    // not read the previous epoch's rows again: the first seam of every epoch empties the buffer.
    build(true);
    healthyState();
    resolve();
    ASSERT_TRUE(engine->arObserveEpoch().valid);
    EXPECT_FALSE(engine->arObserveEpoch().rows.empty());
    EXPECT_EQ(engine->arObserveEpoch().time, kNow);
    ArObserveTestAccess::startNextEpoch(*engine, GnssTime{ 2313, 261630.0 });
    EXPECT_FALSE(engine->arObserveEpoch().valid);
    EXPECT_TRUE(engine->arObserveEpoch().rows.empty());
    EXPECT_EQ(engine->arObserveCounters().epochs, 1) << "the totals keep what was searched";
}

// ---------------------------------------------------------------------------------------
// The chain gate
// ---------------------------------------------------------------------------------------

TEST_F(ArObserveEngineTest, AGoldenWireAdmitsNothing) {
    // amb_datum 0, no components, biases integer-grade: every state reaches the gate, none passes.
    build(true, "decorrelated_partial", /*gate=*/true);
    healthyState();
    pushBiases();
    engine->setWireChainContext(wire(0, {}));
    resolve();
    const ArObserveCounters& c = engine->arObserveCounters();
    EXPECT_EQ(c.candidates, 10);
    EXPECT_EQ(c.eligible, 0);
    EXPECT_EQ(c.gated_out, 10);
    ASSERT_TRUE(buffer().valid);
    EXPECT_TRUE(buffer().rows.empty());
    EXPECT_EQ(buffer().summary.n_pairs, 0);
    EXPECT_EQ(buffer().summary.fix_ecef, buffer().summary.float_ecef);
    EXPECT_TRUE(c.closureHolds());
}

TEST_F(ArObserveEngineTest, AChainWireWithoutComponentsAdmitsNothing) {
    build(true, "decorrelated_partial", true);
    healthyState();
    pushBiases();
    engine->setWireChainContext(wire(kWireAmbDatumChain, {}));
    resolve();
    EXPECT_EQ(engine->arObserveCounters().eligible, 0);
    EXPECT_TRUE(buffer().rows.empty());
}

TEST_F(ArObserveEngineTest, AWireThatNeverWasDeclaredAdmitsNothing) {
    build(true, "decorrelated_partial", true);
    healthyState();
    pushBiases();
    // No setWireChainContext call: amb_datum -1.
    resolve();
    EXPECT_EQ(engine->wireChainContext().amb_datum, -1);
    EXPECT_EQ(engine->arObserveCounters().eligible, 0);
}

TEST_F(ArObserveEngineTest, AChainWireWithComponentsPairsWithinEachComponent) {
    build(true, "decorrelated_partial", true);
    healthyState();
    pushBiases();
    // Component 1: G01 G02 G03 (reference G01); component 2: G04 G05 (reference G04).
    engine->setWireChainContext(wire(kWireAmbDatumChain,
        components({ {"G01", 1}, {"G02", 1}, {"G03", 1}, {"G04", 2}, {"G05", 2} })));
    resolve();
    const ArObserveCounters& c = engine->arObserveCounters();
    EXPECT_EQ(c.eligible, 10);
    ASSERT_TRUE(buffer().valid);
    // Per band: G02-G01, G03-G01 and G05-G04: 3 SDs; 2 bands; 3 pairs.
    EXPECT_EQ(buffer().summary.n_entries, 6);
    EXPECT_EQ(buffer().summary.n_pairs, 3);
    for (const ArFixRow& r : buffer().rows) {
        const bool first = (r.sat == "G02" || r.sat == "G03");
        EXPECT_EQ(r.ref_sat, first ? "G01" : "G04") << r.sat;
        EXPECT_EQ(r.cmp, first ? 1u : 2u) << r.sat;
    }
}

TEST_F(ArObserveEngineTest, SatellitesOfDifferentComponentsNeverPair) {
    build(true, "full", true);
    // Two satellites on one signal, in different components: nothing to difference.
    addAmb("G01", "L1C", 3.03, 0.01);
    addAmb("G02", "L1C", 13.03, 0.01);
    finishCovariance();
    pushBiases();
    engine->setWireChainContext(wire(kWireAmbDatumChain, { {"G01|L1C", 1}, {"G02|L1C", 2} }));
    resolve();
    EXPECT_EQ(engine->arObserveCounters().eligible, 2);
    ASSERT_TRUE(buffer().valid);
    EXPECT_TRUE(buffer().rows.empty());
}

TEST_F(ArObserveEngineTest, ABiasThatIsNotIntegerGradeIsRefused) {
    build(true, "decorrelated_partial", true);
    healthyState();
    pushBiases({ "G03" });
    engine->setWireChainContext(wire(kWireAmbDatumChain,
        components({ {"G01", 1}, {"G02", 1}, {"G03", 1}, {"G04", 1}, {"G05", 1} })));
    resolve();
    const ArObserveCounters& c = engine->arObserveCounters();
    EXPECT_EQ(c.candidates, 10);
    EXPECT_EQ(c.eligible, 8);                       // G03's two signals refused
    EXPECT_EQ(c.gated_out, 2);
    for (const ArFixRow& r : buffer().rows) {
        EXPECT_NE(r.sat, "G03");
        EXPECT_NE(r.ref_sat, "G03");
    }
}

TEST_F(ArObserveEngineTest, AMissingBiasIsRefused) {
    build(true, "decorrelated_partial", true);
    healthyState();
    // Nothing pushed: no satellite has a store phase bias.
    engine->setWireChainContext(wire(kWireAmbDatumChain,
        components({ {"G01", 1}, {"G02", 1}, {"G03", 1}, {"G04", 1}, {"G05", 1} })));
    resolve();
    EXPECT_EQ(engine->arObserveCounters().eligible, 0);
}

TEST_F(ArObserveEngineTest, WithTheGateOffTheWireDoesNotMatter) {
    build(true, "decorrelated_partial", /*gate=*/false);
    healthyState();
    engine->setWireChainContext(wire(0, {}));      // a golden wire, no store entries
    resolve();
    EXPECT_EQ(engine->arObserveCounters().eligible, 10);
    EXPECT_EQ(buffer().summary.n_pairs, 4);
}

TEST_F(ArObserveEngineTest, HoldGenerationsComeFromTheStoreEntries) {
    build(true, "decorrelated_partial");
    healthyState();
    pushBiases({}, /*gen_g02_l1c=*/5);
    resolve();
    bool seen = false;
    for (const ArFixRow& r : buffer().rows) {
        if (r.sat == "G02" && r.signal == "L1C") {
            seen = true;
            EXPECT_EQ(r.gen_sat, 5u);
            EXPECT_EQ(r.gen_ref, 0u);                 // G01 is unheld
        } else if (r.sat == "G03" && r.signal == "L1C") {
            EXPECT_EQ(r.gen_sat, 0u);
        }
    }
    EXPECT_TRUE(seen);
}

TEST_F(ArObserveEngineTest, TheRunEndLineReportsTheTotals) {
    build(true);
    healthyState();
    resolve();
    EXPECT_TRUE(engine->arObserveCounters().closureHolds());
    EXPECT_EQ(engine->arObserveCounters().epochs, 1);
    EXPECT_EQ(engine->arObserveCounters().epochs_searched, 1);
    engine->logArObserveSummary("closed loop");    // prints; must not throw
}

} // namespace
} // namespace GnssProcessor
