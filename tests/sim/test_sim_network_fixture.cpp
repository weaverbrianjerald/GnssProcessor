/**
 * @file test_sim_network_fixture.cpp
 * @brief Unit battery for the synthetic MINIMUM NETWORK fixture (docs/SPEC_synthetic_minimum_network.md).
 *
 * Product-free and processor-free, like the rest of `gnss_simulator_selftest`:
 * these tests exercise the pieces that make a multi-station fixture MEAN
 * something, and they exist because each of them has a silent failure mode that
 * produces a beautiful, passing, worthless run.
 *
 * Suite names here (`SimNetworkSeedSalt`, `SimNetworkStationConfig`,
 * `SimNetworkSatClockError`, `SimNetworkRoster`) are owned by THIS FILE ONLY —
 * the repo keeps one suite per file with zero cross-file overlap.
 */

#include <gtest/gtest.h>

#include <cmath>
#include <fstream>
#include <map>
#include <set>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "sim/SatClockError.hpp"
#include "sim/SimConfig.hpp"

using namespace GnssProcessor;
using namespace GnssProcessor::sim;

namespace {

std::filesystem::path tempDir(const std::string& leaf) {
    const auto d = std::filesystem::temp_directory_path() / ("gnss_simnet_test_" + leaf);
    std::error_code ec;
    std::filesystem::create_directories(d, ec);
    return d;
}

void writeText(const std::filesystem::path& p, const std::string& s) {
    std::ofstream out(p);
    out << s;
}

GnssTime t0At(double tow, int week = 2313) {
    GnssTime t;
    t.week = week;
    t.tow = tow;
    return t;
}

} // namespace

// ---------------------------------------------------------------------------
// Seed salt.  The salt is what keeps N station processes from sharing one noise,
// multipath, slip, ZWD and receiver-clock realization.  If it collides or returns
// the legacy sentinel, the network silently becomes perfectly conditioned and
// meaningless, and no run gate can see it.
// ---------------------------------------------------------------------------

TEST(SimNetworkSeedSalt, IsDeterministicForAName) {
    EXPECT_EQ(stationSeedSalt("SNA000SIM"), stationSeedSalt("SNA000SIM"));
    EXPECT_EQ(stationSeedSalt(""), stationSeedSalt(""));
}

TEST(SimNetworkSeedSalt, NeverReturnsTheLegacySentinel) {
    // 0 means "single station, leave every seed exactly as configured".  A station
    // landing on it would share the unsalted realization with any other that did.
    for (const char* n : { "", "A", "SNA000SIM", "SNB000SIM", "SNC000SIM", "SND000SIM",
                           "SNE000SIM", "ZZZZZZZZZ", "000000000" })
        EXPECT_NE(stationSeedSalt(n), 0ull) << "name=" << n;
}

TEST(SimNetworkSeedSalt, DistinctNamesGiveDistinctSalts) {
    // Includes the near-miss names a real roster uses -- one character apart is the
    // case a weak hash fails on, and it is exactly how stations get named.
    const std::vector<std::string> names = { "SNA000SIM", "SNB000SIM", "SNC000SIM",
                                             "SND000SIM", "SNE000SIM", "SNA000SIN",
                                             "SNA001SIM", "SNA000SI",  "NSA000SIM" };
    std::set<std::uint64_t> salts;
    for (const auto& n : names) salts.insert(stationSeedSalt(n));
    EXPECT_EQ(salts.size(), names.size()) << "a salt collision silently clones a station";
}

TEST(SimNetworkSeedSalt, SurvivesAJsonDoubleRoundTrip) {
    // The salt is written into a JSON child config, and a JSON number is conventionally
    // a double (53-bit mantissa).  A full 64-bit salt comes back ROUNDED, so two
    // stations differing only in the low bits would collide and share one whole noise
    // realization -- silently, and only in the multi-station path.  Regression test for
    // exactly that: the value must be exact through a double, both ways.
    for (const char* n : { "SNA000SIM", "SNB000SIM", "SNC000SIM", "SND000SIM",
                           "SNE000SIM", "SNZ000SIM" }) {
        const std::uint64_t salt = stationSeedSalt(n);
        EXPECT_LT(salt, 1ull << 53) << "name=" << n << " salt is not JSON-double-safe";
        EXPECT_EQ(static_cast<std::uint64_t>(static_cast<double>(salt)), salt) << "name=" << n;
    }
}

TEST(SimNetworkSeedSalt, OneCharacterChangeMovesManyBits) {
    // Adjacent salts would seed adjacent, correlated mt19937_64 states -- correlated
    // enough to bias variance-component estimation, different enough that no equality
    // check catches it.  Require a broad avalanche, not merely inequality.
    const std::uint64_t a = stationSeedSalt("SNA000SIM");
    const std::uint64_t b = stationSeedSalt("SNB000SIM");
    int bits = 0;
    for (std::uint64_t x = a ^ b; x; x >>= 1) bits += static_cast<int>(x & 1ull);
    EXPECT_GE(bits, 16) << "only " << bits << " bits differ between adjacent station names";
}

// ---------------------------------------------------------------------------
// Child config derivation.
// ---------------------------------------------------------------------------

TEST(SimNetworkStationConfig, OverridesIdentityOriginAndAnchorAndStripsRoster) {
    const auto dir = tempDir("childcfg");
    const auto base = dir / "base.json";
    writeText(base, R"({
        "trajectory": { "n_epochs": 40, "lat0_deg": 1.0, "lon0_deg": 2.0, "h0_m": 3.0 },
        "iono": { "quiet_vtec_tecu": 17.5 },
        "network_stations": [ {"name":"SNA000SIM","lat_deg":10,"lon_deg":20},
                              {"name":"SNB000SIM","lat_deg":11,"lon_deg":21},
                              {"name":"SNC000SIM","lat_deg":12,"lon_deg":22} ]
    })");

    NetworkStation st;
    st.name = "SNB000SIM";
    st.lat_deg = 11.0;
    st.lon_deg = 21.0;
    st.h_m = 123.0;

    const auto child = dir / "child.json";
    std::string err;
    ASSERT_TRUE(writeStationConfig(base, st, 44.0, -55.0, child, &err)) << err;

    std::ifstream in(child);
    nlohmann::json j;
    in >> j;

    EXPECT_EQ(j["station"]["name"], "SNB000SIM");
    EXPECT_EQ(j["station"]["seed_salt"].get<std::uint64_t>(), stationSeedSalt("SNB000SIM"));
    EXPECT_DOUBLE_EQ(j["trajectory"]["lat0_deg"].get<double>(), 11.0);
    EXPECT_DOUBLE_EQ(j["trajectory"]["lon0_deg"].get<double>(), 21.0);
    EXPECT_DOUBLE_EQ(j["trajectory"]["h0_m"].get<double>(), 123.0);
    // The ABSOLUTE anchor must be written, or every child re-derives the storm centre
    // from its own origin and sits exactly at it.
    EXPECT_DOUBLE_EQ(j["iono"]["center_lat_deg"].get<double>(), 44.0);
    EXPECT_DOUBLE_EQ(j["iono"]["center_lon_deg"].get<double>(), -55.0);
    // Unrelated knobs survive verbatim.
    EXPECT_EQ(j["trajectory"]["n_epochs"].get<int>(), 40);
    EXPECT_DOUBLE_EQ(j["iono"]["quiet_vtec_tecu"].get<double>(), 17.5);
    // And the child must not think it owns a roster (it would recurse).
    EXPECT_FALSE(j.contains("network_stations"));

    // The derived config must round-trip through the real loader.
    SimConfig cfg;
    ASSERT_TRUE(loadSimConfig(child, cfg, &err)) << err;
    EXPECT_EQ(cfg.station.name, "SNB000SIM");
    EXPECT_EQ(cfg.station.seed_salt, stationSeedSalt("SNB000SIM"));
    EXPECT_TRUE(cfg.network_stations.empty());
    EXPECT_TRUE(std::isfinite(cfg.iono.center_lat_deg));
}

TEST(SimNetworkStationConfig, HonoursTheSimulatorWrapperObject) {
    // The loader reads either the root or a "simulator" wrapper; the overrides must
    // land in whichever one the knobs live in, not beside it.
    const auto dir = tempDir("childwrap");
    const auto base = dir / "base.json";
    writeText(base, R"({"simulator": {"trajectory": {"n_epochs": 25}}})");

    NetworkStation st;
    st.name = "SNC000SIM";
    st.lat_deg = 5.0;
    st.lon_deg = 6.0;

    const auto child = dir / "child.json";
    std::string err;
    ASSERT_TRUE(writeStationConfig(base, st, 1.0, 2.0, child, &err)) << err;

    SimConfig cfg;
    ASSERT_TRUE(loadSimConfig(child, cfg, &err)) << err;
    EXPECT_EQ(cfg.station.name, "SNC000SIM");
    EXPECT_DOUBLE_EQ(cfg.trajectory.lat0_deg, 5.0);
    EXPECT_EQ(cfg.trajectory.n_epochs, 25);
}

// ---------------------------------------------------------------------------
// Roster validation.
// ---------------------------------------------------------------------------

TEST(SimNetworkRoster, RejectsDuplicateNames) {
    const auto dir = tempDir("roster");
    const auto p = dir / "dup.json";
    writeText(p, R"({"network_stations":[
        {"name":"SNA000SIM","lat_deg":1,"lon_deg":2},
        {"name":"SNA000SIM","lat_deg":3,"lon_deg":4},
        {"name":"SNC000SIM","lat_deg":5,"lon_deg":6}]})");
    SimConfig cfg;
    std::string err;
    EXPECT_FALSE(loadSimConfig(p, cfg, &err));
    EXPECT_NE(err.find("duplicate"), std::string::npos) << err;
}

TEST(SimNetworkRoster, RejectsFewerThanThreeSolveStations) {
    const auto dir = tempDir("roster");
    const auto p = dir / "small.json";
    writeText(p, R"({"network_stations":[
        {"name":"SNA000SIM","lat_deg":1,"lon_deg":2},
        {"name":"SNB000SIM","lat_deg":3,"lon_deg":4}]})");
    SimConfig cfg;
    std::string err;
    EXPECT_FALSE(loadSimConfig(p, cfg, &err));
}

TEST(SimNetworkRoster, CountsTheConsumerOutsideTheSolve) {
    // 3 peers + 1 held-out consumer is legal; 3 total WITH a consumer is not, because
    // it leaves only 2 in the solve.
    const auto dir = tempDir("roster");
    SimConfig cfg;
    std::string err;

    const auto ok = dir / "ok.json";
    writeText(ok, R"({"network_stations":[
        {"name":"SNA000SIM","lat_deg":1,"lon_deg":2},
        {"name":"SNB000SIM","lat_deg":3,"lon_deg":4},
        {"name":"SNC000SIM","lat_deg":5,"lon_deg":6},
        {"name":"SNZ000SIM","lat_deg":7,"lon_deg":8,"consumer":true}]})");
    ASSERT_TRUE(loadSimConfig(ok, cfg, &err)) << err;
    EXPECT_EQ(cfg.network_stations.size(), 4u);

    SimConfig cfg2;
    const auto bad = dir / "bad.json";
    writeText(bad, R"({"network_stations":[
        {"name":"SNA000SIM","lat_deg":1,"lon_deg":2},
        {"name":"SNB000SIM","lat_deg":3,"lon_deg":4},
        {"name":"SNZ000SIM","lat_deg":7,"lon_deg":8,"consumer":true}]})");
    EXPECT_FALSE(loadSimConfig(bad, cfg2, &err));
}

TEST(SimNetworkRoster, RejectsMoreThanOneConsumer) {
    const auto dir = tempDir("roster");
    const auto p = dir / "twocons.json";
    writeText(p, R"({"network_stations":[
        {"name":"SNA000SIM","lat_deg":1,"lon_deg":2},
        {"name":"SNB000SIM","lat_deg":3,"lon_deg":4},
        {"name":"SNC000SIM","lat_deg":5,"lon_deg":6},
        {"name":"SNY000SIM","lat_deg":7,"lon_deg":8,"consumer":true},
        {"name":"SNZ000SIM","lat_deg":9,"lon_deg":10,"consumer":true}]})");
    SimConfig cfg;
    std::string err;
    EXPECT_FALSE(loadSimConfig(p, cfg, &err));
}

TEST(SimNetworkRoster, AbsoluteStormAnchorNeedsBothComponents) {
    const auto dir = tempDir("roster");
    const auto p = dir / "halfanchor.json";
    writeText(p, R"({"iono": {"center_lat_deg": 40.0}})");
    SimConfig cfg;
    std::string err;
    EXPECT_FALSE(loadSimConfig(p, cfg, &err));
    EXPECT_NE(err.find("together"), std::string::npos) << err;
}

TEST(SimNetworkRoster, ReceiverBiasListsLoadAndRejectDuplicates) {
    const auto dir = tempDir("rxbias");
    SimConfig cfg;
    std::string err;

    const auto ok = dir / "ok.json";
    writeText(ok, R"({"biases": {"rx_code_bias":[{"code":"C1C","value_m":0.35},
                                                 {"code":"C2W","value_m":-0.12}],
                                 "rx_phase_bias":[{"code":"L1C","value_m":0.01}]}})");
    ASSERT_TRUE(loadSimConfig(ok, cfg, &err)) << err;
    ASSERT_EQ(cfg.biases.rx_code_bias.size(), 2u);
    EXPECT_EQ(cfg.biases.rx_code_bias[0].code, "C1C");
    EXPECT_DOUBLE_EQ(cfg.biases.rx_code_bias[0].value_m, 0.35);
    EXPECT_EQ(cfg.biases.rx_phase_bias.size(), 1u);

    SimConfig cfg2;
    const auto dup = dir / "dup.json";
    writeText(dup, R"({"biases": {"rx_code_bias":[{"code":"C1C","value_m":0.1},
                                                  {"code":"C1C","value_m":0.2}]}})");
    EXPECT_FALSE(loadSimConfig(dup, cfg2, &err));
}

TEST(SimNetworkRoster, ReceiverBiasDefaultsEmptySoLegacyIsByteIdentical) {
    const auto dir = tempDir("rxbias");
    const auto p = dir / "none.json";
    writeText(p, R"({"trajectory": {"n_epochs": 10}})");
    SimConfig cfg;
    std::string err;
    ASSERT_TRUE(loadSimConfig(p, cfg, &err)) << err;
    EXPECT_TRUE(cfg.biases.rx_code_bias.empty());
    EXPECT_TRUE(cfg.biases.rx_phase_bias.empty());
    EXPECT_TRUE(cfg.station.name.empty());
    EXPECT_EQ(cfg.station.seed_salt, 0ull);
}

// ---------------------------------------------------------------------------
// Satellite clock error.
// ---------------------------------------------------------------------------

TEST(SimNetworkSatClockError, InactiveByDefaultAndReturnsExactlyZero) {
    SimConfig::SatClock cfg;      // all-zero amplitudes, no steps
    SatClockError e;
    std::string err;
    ASSERT_TRUE(e.build(cfg, { "G01", "G02" }, t0At(302400.0), 30.0, 20, &err)) << err;
    EXPECT_FALSE(e.active());
    EXPECT_EQ(e.metres("G01", t0At(302400.0)), 0.0);
    EXPECT_EQ(e.peakAbsMetres(), 0.0);
}

TEST(SimNetworkSatClockError, IsIndependentOfTheSatelliteSetAndItsOrder) {
    // THE determinism property that lets N station processes agree on the satellite
    // side.  If a satellite's series depended on the set or the iteration order, two
    // stations with different skies would inject DIFFERENT satellite clocks -- a
    // satellite-side term made station-dependent, which grades as an estimator defect.
    SimConfig::SatClock cfg;
    cfg.bias0_sigma_m = 0.5;
    cfg.rate_sigma_mps = 1e-4;
    cfg.rw_q_m2ps = 3e-8;

    SatClockError a, b, c;
    std::string err;
    ASSERT_TRUE(a.build(cfg, { "G01", "G02", "G03" }, t0At(302400.0), 30.0, 30, &err)) << err;
    ASSERT_TRUE(b.build(cfg, { "G03", "G01" }, t0At(302400.0), 30.0, 30, &err)) << err;   // subset, reordered
    ASSERT_TRUE(c.build(cfg, { "G05", "G01", "G02", "G03", "G09" }, t0At(302400.0), 30.0, 30, &err)) << err;

    for (int k = 0; k < 30; ++k) {
        GnssTime t = t0At(302400.0 + 30.0 * k);
        EXPECT_DOUBLE_EQ(a.metres("G01", t), b.metres("G01", t)) << "k=" << k;
        EXPECT_DOUBLE_EQ(a.metres("G01", t), c.metres("G01", t)) << "k=" << k;
        EXPECT_DOUBLE_EQ(a.metres("G03", t), b.metres("G03", t)) << "k=" << k;
    }
}

TEST(SimNetworkSatClockError, DistinctSatellitesGetDistinctRealizations) {
    SimConfig::SatClock cfg;
    cfg.bias0_sigma_m = 1.0;
    SatClockError e;
    std::string err;
    ASSERT_TRUE(e.build(cfg, { "G01", "G02", "G03", "G04" }, t0At(302400.0), 30.0, 5, &err)) << err;
    std::set<double> vals;
    for (const char* s : { "G01", "G02", "G03", "G04" })
        vals.insert(e.metres(s, t0At(302400.0)));
    EXPECT_EQ(vals.size(), 4u);
}

TEST(SimNetworkSatClockError, RandomWalkFirstEpochIsUndisturbedAndScalesAsSqrtQdt) {
    // sigma per step = sqrt(q*dt) -- the SAME convention the producer's
    // filter.process_noise.sat_clock_q uses, so injected truth and the estimator's
    // stochastic model are directly comparable.  q_phys ~ 3e-8 is ~0.9 mm / 30 s.
    SimConfig::SatClock cfg;
    cfg.rw_q_m2ps = 3e-8;
    const double dt = 30.0;
    const int n = 4000;

    std::vector<std::string> sats;
    for (int i = 1; i <= 32; ++i) {
        char id[4];
        std::snprintf(id, sizeof(id), "G%02d", i);
        sats.emplace_back(id);
    }
    SatClockError e;
    std::string err;
    ASSERT_TRUE(e.build(cfg, sats, t0At(302400.0), dt, n, &err)) << err;
    ASSERT_TRUE(e.active());

    // First epoch: pure w_0 = 0, no bias/rate armed.
    for (const auto& s : sats) EXPECT_DOUBLE_EQ(e.metres(s, t0At(302400.0)), 0.0);

    // Pooled one-step increment scatter across all satellites and epochs.
    double ss = 0.0;
    long cnt = 0;
    for (const auto& s : sats)
        for (int k = 1; k < n; ++k) {
            const double d = e.metres(s, t0At(302400.0 + dt * k)) -
                             e.metres(s, t0At(302400.0 + dt * (k - 1)));
            ss += d * d;
            ++cnt;
        }
    const double rms = std::sqrt(ss / static_cast<double>(cnt));
    const double expect = std::sqrt(cfg.rw_q_m2ps * dt);       // ~9.3e-4 m
    EXPECT_NEAR(rms, expect, 0.05 * expect) << "rms=" << rms << " expect=" << expect;
}

TEST(SimNetworkSatClockError, ScriptedStepIsHeldFromItsEpochOnward) {
    SimConfig::SatClock cfg;
    SimConfig::ScriptedSatClockStep ev;
    ev.sat = "G07";
    ev.epoch = 10;          // 1-based
    ev.step_m = 2.5;
    cfg.steps.push_back(ev);

    SatClockError e;
    std::string err;
    ASSERT_TRUE(e.build(cfg, { "G07", "G08" }, t0At(302400.0), 30.0, 20, &err)) << err;
    ASSERT_TRUE(e.active());

    // Before the step: exactly zero (no stochastic channel armed).
    EXPECT_DOUBLE_EQ(e.metres("G07", t0At(302400.0 + 30.0 * 8)), 0.0);   // epoch 9
    // From the step epoch onward: held, not an impulse.
    EXPECT_DOUBLE_EQ(e.metres("G07", t0At(302400.0 + 30.0 * 9)), 2.5);   // epoch 10
    EXPECT_DOUBLE_EQ(e.metres("G07", t0At(302400.0 + 30.0 * 19)), 2.5);  // last epoch
    // And it touches nobody else -- an event that leaked would grade as a false alarm.
    EXPECT_DOUBLE_EQ(e.metres("G08", t0At(302400.0 + 30.0 * 15)), 0.0);
}

TEST(SimNetworkSatClockError, RampAccumulatesFromTheStepEpoch) {
    SimConfig::SatClock cfg;
    SimConfig::ScriptedSatClockStep ev;
    ev.sat = "G07";
    ev.epoch = 5;
    ev.ramp_mps = 0.01;                 // 0.3 m per 30 s epoch
    cfg.steps.push_back(ev);

    SatClockError e;
    std::string err;
    ASSERT_TRUE(e.build(cfg, { "G07" }, t0At(302400.0), 30.0, 12, &err)) << err;
    EXPECT_DOUBLE_EQ(e.metres("G07", t0At(302400.0 + 30.0 * 4)), 0.0);   // epoch 5: t=0
    EXPECT_NEAR(e.metres("G07", t0At(302400.0 + 30.0 * 5)), 0.30, 1e-12);
    EXPECT_NEAR(e.metres("G07", t0At(302400.0 + 30.0 * 9)), 1.50, 1e-12);
}

TEST(SimNetworkSatClockError, InterpolatesOffGridAndClampsAtBothEnds) {
    // The emission time is ~70 ms before the epoch and differs per station because the
    // geometry does, so off-grid evaluation is the normal case, not an edge case.
    SimConfig::SatClock cfg;
    SimConfig::ScriptedSatClockStep ev;
    ev.sat = "G01";
    ev.epoch = 1;
    ev.ramp_mps = 1.0;                  // 30 m per epoch: a clean linear ramp
    cfg.steps.push_back(ev);

    SatClockError e;
    std::string err;
    ASSERT_TRUE(e.build(cfg, { "G01" }, t0At(302400.0), 30.0, 5, &err)) << err;

    EXPECT_NEAR(e.metres("G01", t0At(302400.0 + 15.0)), 15.0, 1e-9);   // mid-interval
    EXPECT_NEAR(e.metres("G01", t0At(302400.0 + 44.0)), 44.0, 1e-9);
    // Clamped before the first epoch (the emission time of epoch 1) and after the last.
    EXPECT_NEAR(e.metres("G01", t0At(302400.0 - 0.07)), 0.0, 1e-12);
    EXPECT_NEAR(e.metres("G01", t0At(302400.0 + 30.0 * 50)), 120.0, 1e-9);
}

TEST(SimNetworkSatClockError, RefusesAStepOnASatelliteItDoesNotGenerate) {
    // An event that injects nothing is graded downstream as an UNDETECTED event, i.e.
    // it reads as a guard defect rather than a fixture typo.  Fail loudly instead.
    SimConfig::SatClock cfg;
    SimConfig::ScriptedSatClockStep ev;
    ev.sat = "G31";
    ev.epoch = 3;
    ev.step_m = 1.0;
    cfg.steps.push_back(ev);

    SatClockError e;
    std::string err;
    EXPECT_FALSE(e.build(cfg, { "G01", "G02" }, t0At(302400.0), 30.0, 10, &err));
    EXPECT_NE(err.find("G31"), std::string::npos) << err;
}

TEST(SimNetworkSatClockError, TruthFileIsWrittenOnlyWhenActive) {
    const auto dir = tempDir("satclk");
    const auto p = dir / "truth_satclock.csv";
    std::error_code ec;
    std::filesystem::remove(p, ec);

    SimConfig::SatClock off;
    SatClockError inactive;
    std::string err;
    ASSERT_TRUE(inactive.build(off, { "G01" }, t0At(302400.0), 30.0, 5, &err)) << err;
    EXPECT_TRUE(inactive.writeTruth(p));
    // An ABSENT file says "this channel was not simulated"; an empty one cannot.
    EXPECT_FALSE(std::filesystem::exists(p));

    SimConfig::SatClock on;
    on.bias0_sigma_m = 1.0;
    SatClockError active;
    ASSERT_TRUE(active.build(on, { "G01", "G02" }, t0At(302400.0), 30.0, 5, &err)) << err;
    ASSERT_TRUE(active.writeTruth(p));
    ASSERT_TRUE(std::filesystem::exists(p));

    std::ifstream in(p);
    std::string header;
    std::getline(in, header);
    EXPECT_EQ(header, "week,tow_s,satellite,clk_err_m,clk_err_s");
    int rows = 0;
    for (std::string line; std::getline(in, line);) if (!line.empty()) ++rows;
    EXPECT_EQ(rows, 2 * 5) << "one row per (epoch, satellite)";
}

TEST(SimNetworkSatClockError, ConfigLoaderValidatesTheStepList) {
    const auto dir = tempDir("satclkcfg");
    SimConfig cfg;
    std::string err;

    const auto ok = dir / "ok.json";
    writeText(ok, R"({"trajectory":{"n_epochs":100},
                      "sat_clock":{"rw_q_m2ps":3e-8,
                                   "steps":[{"sat":"G05","epoch":50,"step_m":1.5}]}})");
    ASSERT_TRUE(loadSimConfig(ok, cfg, &err)) << err;
    EXPECT_DOUBLE_EQ(cfg.sat_clock.rw_q_m2ps, 3e-8);
    ASSERT_EQ(cfg.sat_clock.steps.size(), 1u);
    EXPECT_EQ(cfg.sat_clock.steps[0].sat, "G05");
    EXPECT_EQ(cfg.sat_clock.steps[0].epoch, 50);

    // An epoch outside the arc would inject nothing.
    SimConfig c2;
    const auto oob = dir / "oob.json";
    writeText(oob, R"({"trajectory":{"n_epochs":20},
                       "sat_clock":{"steps":[{"sat":"G05","epoch":500,"step_m":1.5}]}})");
    EXPECT_FALSE(loadSimConfig(oob, c2, &err));

    // A step with neither a step nor a ramp injects nothing and would be graded as an
    // undetected event.
    SimConfig c3;
    const auto empty = dir / "empty.json";
    writeText(empty, R"({"trajectory":{"n_epochs":20},
                         "sat_clock":{"steps":[{"sat":"G05","epoch":5}]}})");
    EXPECT_FALSE(loadSimConfig(empty, c3, &err));

    // A negative 1-sigma is a sign error, not a knob.
    SimConfig c4;
    const auto neg = dir / "neg.json";
    writeText(neg, R"({"sat_clock":{"rw_q_m2ps":-1e-8}})");
    EXPECT_FALSE(loadSimConfig(neg, c4, &err));
}
