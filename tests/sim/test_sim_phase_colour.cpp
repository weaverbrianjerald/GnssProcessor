/**
 * @file test_sim_phase_colour.cpp
 * @brief The simulator's coloured carrier-phase error (card SFR,
 *        docs/refound/SFR_sim_fixture_coloured_phase_2026-10-01.md section 2).
 *
 * Product-free and processor-free, like the rest of `gnss_simulator_selftest`:
 * the recursion lives in @c sim::PhaseColourGenerator (sim/PhaseColour.hpp), so
 * its statistics are tested here on long synthetic sequences rather than through
 * a full synthesis.  The full-synthesis guarantee (an off block regenerates an
 * existing fixture byte for byte) is the null regeneration gate of the card, not
 * a unit test.
 *
 * The suite names here (`SimPhaseColour`, `SimPhaseColourConfig`,
 * `SimPhaseColourRoster`) are owned by this file alone; the repository keeps one
 * suite per file with no cross-file overlap.
 *
 * The failure modes guarded, each of which yields a fixture that looks normal:
 *
 *  - an off block that still draws or still adds something => every existing
 *    recipe silently stops regenerating byte for byte;
 *  - a non-stationary start or a wrong phi => the injected process has the wrong
 *    variance or correlation time, and the per-class calibration chases it;
 *  - a gap that does not decorrelate => a re-rising satellite carries a stale
 *    error across a set, which the field does not;
 *  - a per-band draw => the error acquires a geometry-free part the free slant
 *    ionosphere absorbs, so the estimator never sees most of it;
 *  - a lost roster override => every station gets the network default and the
 *    per-class calibration is inert.
 */

#include <gtest/gtest.h>

#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <limits>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "common/Constants.hpp"
#include "sim/PhaseColour.hpp"
#include "sim/SimConfig.hpp"

using namespace GnssProcessor;
using namespace GnssProcessor::sim;

namespace {

std::filesystem::path tempDir(const std::string& leaf) {
    const auto d = std::filesystem::temp_directory_path() / ("gnss_simpc_test_" + leaf);
    std::error_code ec;
    std::filesystem::create_directories(d, ec);
    return d;
}

void writeText(const std::filesystem::path& p, const std::string& s) {
    std::ofstream out(p);
    out << s;
}

nlohmann::json readJson(const std::filesystem::path& p) {
    std::ifstream in(p);
    nlohmann::json j;
    in >> j;
    return j;
}

/// Sample mean, variance and lag-k autocorrelation of a series.
struct SeriesStats {
    double mean{ 0.0 };
    double var{ 0.0 };
    double acf(const std::vector<double>& x, std::size_t lag) const {
        double num = 0.0, den = 0.0;
        for (std::size_t i = 0; i < x.size(); ++i) {
            den += (x[i] - mean) * (x[i] - mean);
            if (i + lag < x.size()) num += (x[i] - mean) * (x[i + lag] - mean);
        }
        return num / den;
    }
};

SeriesStats statsOf(const std::vector<double>& x) {
    SeriesStats s;
    for (double v : x) s.mean += v;
    s.mean /= static_cast<double>(x.size());
    for (double v : x) s.var += (v - s.mean) * (v - s.mean);
    s.var /= static_cast<double>(x.size() - 1);
    return s;
}

/// Pearson correlation of two equal-length series.
double corr(const std::vector<double>& a, const std::vector<double>& b) {
    const SeriesStats sa = statsOf(a), sb = statsOf(b);
    double num = 0.0;
    for (std::size_t i = 0; i < a.size(); ++i) num += (a[i] - sa.mean) * (b[i] - sb.mean);
    num /= static_cast<double>(a.size() - 1);
    return num / std::sqrt(sa.var * sb.var);
}

constexpr std::uint64_t kSeed = 0x5F12C0DEull;

} // namespace

// ---------------------------------------------------------------------------
// The recursion.
// ---------------------------------------------------------------------------

TEST(SimPhaseColour, InactiveDrawsNothingAndReturnsExactlyZero) {
    // sigma 0 is the default.  It must add a literal +0.0 (x + 0.0 == x keeps the
    // phase byte-identical) and must not consume the stream, or keep any state.
    PhaseColourGenerator pc(0.0, 100.0, 5.0, kSeed);
    EXPECT_FALSE(pc.active());
    for (int ep = 0; ep < 1000; ++ep) {
        for (const char* sat : { "G01", "G17", "E05" }) {
            const double e = pc.sample(sat, 30.0 * ep, 5.0 + 0.08 * ep);
            EXPECT_EQ(e, 0.0);
            EXPECT_FALSE(std::signbit(e)) << "a -0.0 would flip the sign of a zero sum";
        }
    }
    EXPECT_EQ(pc.draws(), 0u);
    EXPECT_EQ(pc.starts(), 0u);
}

TEST(SimPhaseColour, StepFormulaIsExactAtTheLimits) {
    // dt = 0: phi = 1, the process holds.  dt >> tau: phi underflows to 0 and the
    // step IS the fresh draw (a stationary restart).
    EXPECT_EQ(gaussMarkovUnitStep(1.7, 0.0, 100.0, -0.4), 1.7);
    EXPECT_EQ(gaussMarkovUnitStep(5.0, 1.0e6, 100.0, 0.3), 0.3);
    const double phi = std::exp(-30.0 / 100.0);
    EXPECT_NEAR(gaussMarkovUnitStep(1.0, 30.0, 100.0, 0.0), phi, 1e-15);
    EXPECT_NEAR(gaussMarkovUnitStep(0.0, 30.0, 100.0, 1.0), std::sqrt(1.0 - phi * phi), 1e-15);
}

TEST(SimPhaseColour, ElevationFactorIsTheWhiteNoiseShape) {
    EXPECT_DOUBLE_EQ(phaseColourElevFactor(90.0, 5.0), 1.0);
    EXPECT_NEAR(phaseColourElevFactor(30.0, 5.0), 2.0, 1e-12);
    // Below the floor the factor is clamped at the floor's value.
    const double at_floor = 1.0 / std::sin(5.0 * Constants::PI / 180.0);
    EXPECT_DOUBLE_EQ(phaseColourElevFactor(5.0, 5.0), at_floor);
    EXPECT_DOUBLE_EQ(phaseColourElevFactor(1.0, 5.0), at_floor);
    // The injected error is sigma * factor * u.
    PhaseColourGenerator pc(0.004, 100.0, 5.0, kSeed);
    const double e = pc.sample("G05", 0.0, 30.0);
    EXPECT_NEAR(e, 0.004 * 2.0 * pc.lastUnit(), 1e-15);
}

TEST(SimPhaseColour, LongSequenceHasUnitVarianceAndLagCorrelationExpMinusDtOverTau) {
    // One line of sight at zenith (factor 1, sigma 1 => e == u) over 200 000 epochs
    // of 30 s.  For an AR(1) with phi = exp(-0.3) = 0.741, the standard error of
    // the sample variance is ~0.006 and of the lag-1 autocorrelation ~0.0015, so the
    // tolerances below are about 5 standard errors.
    const double tau = 100.0, dt = 30.0;
    PhaseColourGenerator pc(1.0, tau, 5.0, kSeed);
    const std::size_t n = 200000;
    std::vector<double> u(n);
    for (std::size_t i = 0; i < n; ++i) {
        const double e = pc.sample("G07", dt * static_cast<double>(i), 90.0);
        u[i] = pc.lastUnit();
        ASSERT_NEAR(e, u[i], 1e-12);
    }
    EXPECT_EQ(pc.draws(), n);
    EXPECT_EQ(pc.starts(), 1u);
    const SeriesStats s = statsOf(u);
    const double phi = std::exp(-dt / tau);
    EXPECT_NEAR(s.mean, 0.0, 0.03);
    EXPECT_NEAR(s.var, 1.0, 0.03);
    EXPECT_NEAR(s.acf(u, 1), phi, 0.01);
    EXPECT_NEAR(s.acf(u, 2), phi * phi, 0.015);
    EXPECT_NEAR(s.acf(u, 10), std::pow(phi, 10), 0.02);
}

TEST(SimPhaseColour, StartIsStationary) {
    // Across many independent lines of sight, the first sample and the sample one
    // step later both have unit variance: no ignition transient.  20 000 lines give
    // a variance standard error of ~0.01.
    PhaseColourGenerator pc(1.0, 100.0, 5.0, kSeed);
    const int n = 20000;
    std::vector<double> u0(n), u1(n);
    for (int k = 0; k < n; ++k) {
        pc.sample("L" + std::to_string(k), 0.0, 90.0);
        u0[k] = pc.lastUnit();
    }
    for (int k = 0; k < n; ++k) {
        pc.sample("L" + std::to_string(k), 30.0, 90.0);
        u1[k] = pc.lastUnit();
    }
    EXPECT_EQ(pc.starts(), static_cast<std::uint64_t>(n));
    EXPECT_NEAR(statsOf(u0).var, 1.0, 0.05);
    EXPECT_NEAR(statsOf(u1).var, 1.0, 0.05);
    EXPECT_NEAR(corr(u0, u1), std::exp(-0.3), 0.03);
}

TEST(SimPhaseColour, PhiFollowsTheTimeSinceTheLastVisibleEpoch) {
    // A satellite missing one 30 s epoch is advanced across 60 s, not 30 s; a
    // set-and-rise gap of 50 tau decorrelates it completely.  20 000 lines of
    // sight: the correlation standard error is ~0.007.
    const int n = 20000;
    auto corrAcross = [n](double dt) {
        PhaseColourGenerator pc(1.0, 100.0, 5.0, kSeed);
        std::vector<double> a(n), b(n);
        for (int k = 0; k < n; ++k) { pc.sample("L" + std::to_string(k), 0.0, 90.0); a[k] = pc.lastUnit(); }
        for (int k = 0; k < n; ++k) { pc.sample("L" + std::to_string(k), dt, 90.0); b[k] = pc.lastUnit(); }
        return corr(a, b);
    };
    EXPECT_NEAR(corrAcross(60.0), std::exp(-0.6), 0.03);
    EXPECT_NEAR(corrAcross(5000.0), 0.0, 0.03);  // 50 tau
}

TEST(SimPhaseColour, LinesOfSightAreIndependent) {
    // Two satellites sampled at the same epochs share the stream but not the
    // process: their cross-correlation is ~0 (standard error ~0.008 at 50 000).
    PhaseColourGenerator pc(1.0, 100.0, 5.0, kSeed);
    const std::size_t n = 50000;
    std::vector<double> a(n), b(n);
    for (std::size_t i = 0; i < n; ++i) {
        pc.sample("G01", 30.0 * i, 90.0); a[i] = pc.lastUnit();
        pc.sample("E11", 30.0 * i, 90.0); b[i] = pc.lastUnit();
    }
    EXPECT_NEAR(corr(a, b), 0.0, 0.04);
}

TEST(SimPhaseColour, SameSeedReproducesTheRealization) {
    // A network is N independent processes; each station's realization must be a
    // pure function of its (salted) seed and its visibility sequence.
    PhaseColourGenerator a(0.005, 110.0, 5.0, kSeed), b(0.005, 110.0, 5.0, kSeed),
        c(0.005, 110.0, 5.0, kSeed + 1);
    bool any_diff = false;
    for (int ep = 0; ep < 500; ++ep) {
        for (const char* sat : { "G02", "G09", "E24" }) {
            const double ea = a.sample(sat, 30.0 * ep, 40.0);
            EXPECT_EQ(ea, b.sample(sat, 30.0 * ep, 40.0));
            any_diff = any_diff || (ea != c.sample(sat, 30.0 * ep, 40.0));
        }
    }
    EXPECT_TRUE(any_diff) << "a different seed must give a different realization";
}

TEST(SimPhaseColour, SameInstantRequeryReturnsTheSameValueWithoutDrawing) {
    // The simulator samples once per satellite outside its band loop; this pins
    // that even a per-band call site would inject one realization per epoch.
    PhaseColourGenerator pc(0.006, 100.0, 5.0, kSeed);
    const double e1 = pc.sample("G12", 300.0, 25.0);
    const double e2 = pc.sample("G12", 300.0, 25.0);
    const double e3 = pc.sample("G12", 300.0, 25.0);
    EXPECT_EQ(e1, e2);
    EXPECT_EQ(e1, e3);
    EXPECT_EQ(pc.draws(), 1u);
}

TEST(SimPhaseColour, TheSameMetresReachEveryBand) {
    // Mirror of the simulator's phase assembly: per band, cycles = (base_b + e) / lambda_b
    // with base_b the band's own range terms (here a common range minus a
    // band-dependent ionospheric advance).  The injected metres recovered from every
    // band are the same e; so the ionosphere-free combination keeps all of it and the
    // geometry-free combination none of it.
    PhaseColourGenerator pc(0.007, 100.0, 5.0, kSeed);
    const double e = pc.sample("G21", 0.0, 12.0);
    ASSERT_NE(e, 0.0);
    const double range = 2.2e7, stec_m_l1 = 3.1;
    struct BandSpec { double lambda, f; };
    const std::vector<BandSpec> bands = {
        { Constants::GPS_L1_WAVELENGTH, Constants::GPS_L1_FREQ },
        { Constants::GPS_L2_WAVELENGTH, Constants::GPS_L2_FREQ },
        { Constants::GPS_L5_WAVELENGTH, Constants::GPS_L5_FREQ } };
    std::vector<double> injected_m;
    for (const auto& b : bands) {
        const double iono = stec_m_l1 * (Constants::GPS_L1_FREQ * Constants::GPS_L1_FREQ) / (b.f * b.f);
        const double base = range - iono;
        const double cycles = (base + e) / b.lambda;
        const double cycles_off = base / b.lambda;
        injected_m.push_back((cycles - cycles_off) * b.lambda);
    }
    // Rounding of a 2.2e7 m range in cycles limits this to ~1e-8 m (0.01 micrometre).
    for (double m : injected_m) EXPECT_NEAR(m, e, 1e-7);
    const double f1 = bands[0].f, f2 = bands[1].f;
    const double c1 = f1 * f1 / (f1 * f1 - f2 * f2), c2 = -f2 * f2 / (f1 * f1 - f2 * f2);
    EXPECT_NEAR(c1 * injected_m[0] + c2 * injected_m[1], e, 1e-6);   // IF keeps it
    EXPECT_NEAR(injected_m[0] - injected_m[1], 0.0, 2e-7);            // GF drops it
}

// ---------------------------------------------------------------------------
// The config block.
// ---------------------------------------------------------------------------

TEST(SimPhaseColourConfig, DefaultsAreOffWithTheNoiseFloorAndADistinctSeed) {
    const SimConfig cfg;
    EXPECT_EQ(cfg.phase_colour.sigma_zen_m, 0.0);
    EXPECT_EQ(cfg.phase_colour.tau_s, 100.0);
    EXPECT_TRUE(std::isnan(cfg.phase_colour.elev_factor_floor_deg));
    EXPECT_EQ(cfg.phaseColourElevFloorDeg(), cfg.noise.elev_factor_floor_deg);
    EXPECT_EQ(cfg.phaseColourElevFloorDeg(), 5.0);
    // A seed shared with another stream would correlate the new error with it.
    for (std::uint64_t other : { cfg.noise.seed, cfg.multipath.seed, cfg.atmosphere.seed,
                                 cfg.sat_clock.seed, cfg.trajectory.clock_seed })
        EXPECT_NE(cfg.phase_colour.seed, other);
}

TEST(SimPhaseColourConfig, AbsentBlockLeavesTheDefaults) {
    const auto dir = tempDir("cfg");
    const auto p = dir / "none.json";
    writeText(p, R"({"trajectory": {"n_epochs": 10}, "noise": {"phase_sigma_zen_m": 0.002}})");
    SimConfig cfg;
    std::string err;
    ASSERT_TRUE(loadSimConfig(p, cfg, &err)) << err;
    EXPECT_EQ(cfg.phase_colour.sigma_zen_m, 0.0);
    EXPECT_EQ(cfg.phase_colour.tau_s, 100.0);
    EXPECT_EQ(cfg.phase_colour.seed, SimConfig{}.phase_colour.seed);
    EXPECT_TRUE(std::isnan(cfg.phase_colour.elev_factor_floor_deg));
}

TEST(SimPhaseColourConfig, ParsesTheBlock) {
    const auto dir = tempDir("cfg");
    const auto p = dir / "on.json";
    writeText(p, R"({"phase_colour": {"sigma_zen_m": 0.006, "tau_s": 110.0,
                                     "elev_factor_floor_deg": 7.5, "seed": 12345}})");
    SimConfig cfg;
    std::string err;
    ASSERT_TRUE(loadSimConfig(p, cfg, &err)) << err;
    EXPECT_DOUBLE_EQ(cfg.phase_colour.sigma_zen_m, 0.006);
    EXPECT_DOUBLE_EQ(cfg.phase_colour.tau_s, 110.0);
    EXPECT_DOUBLE_EQ(cfg.phaseColourElevFloorDeg(), 7.5);
    EXPECT_EQ(cfg.phase_colour.seed, 12345u);
}

TEST(SimPhaseColourConfig, AnUnsetFloorFollowsTheWhiteNoiseFloor) {
    const auto dir = tempDir("cfg");
    const auto p = dir / "floor.json";
    writeText(p, R"({"noise": {"elev_factor_floor_deg": 8.0}, "phase_colour": {"sigma_zen_m": 0.005}})");
    SimConfig cfg;
    std::string err;
    ASSERT_TRUE(loadSimConfig(p, cfg, &err)) << err;
    EXPECT_DOUBLE_EQ(cfg.phaseColourElevFloorDeg(), 8.0);
}

TEST(SimPhaseColourConfig, RejectsInvalidValues) {
    const auto dir = tempDir("cfg");
    for (const char* body : {
             R"({"phase_colour": {"sigma_zen_m": -0.001}})",
             R"({"phase_colour": {"sigma_zen_m": "0.005"}})",
             R"({"phase_colour": {"tau_s": 0.0}})",
             R"({"phase_colour": {"tau_s": -30.0}})",
             R"({"phase_colour": {"elev_factor_floor_deg": 95.0}})",
             R"({"phase_colour": {"elev_factor_floor_deg": 0.0}})",
             R"({"phase_colour": {"seed": -1}})" }) {
        const auto p = dir / "bad.json";
        writeText(p, body);
        SimConfig cfg;
        std::string err;
        EXPECT_FALSE(loadSimConfig(p, cfg, &err)) << body;
        EXPECT_NE(err.find("phase_colour"), std::string::npos) << body << " -> " << err;
    }
}

// ---------------------------------------------------------------------------
// The per-station override through the network roster.
// ---------------------------------------------------------------------------

TEST(SimPhaseColourRoster, PerStationOverrideParsesAndValidates) {
    const auto dir = tempDir("roster");
    const auto ok = dir / "ok.json";
    writeText(ok, R"({"network_stations":[
        {"name":"SNA000SIM","lat_deg":1,"lon_deg":2,"phase_colour_sigma_zen_m":0.007,"phase_colour_tau_s":120},
        {"name":"SNB000SIM","lat_deg":3,"lon_deg":4,"phase_colour_tau_s":90.0},
        {"name":"SNC000SIM","lat_deg":5,"lon_deg":6}]})");
    SimConfig cfg;
    std::string err;
    ASSERT_TRUE(loadSimConfig(ok, cfg, &err)) << err;
    ASSERT_EQ(cfg.network_stations.size(), 3u);
    EXPECT_DOUBLE_EQ(cfg.network_stations[0].phase_colour_sigma_zen_m, 0.007);
    EXPECT_DOUBLE_EQ(cfg.network_stations[0].phase_colour_tau_s, 120.0);
    EXPECT_TRUE(std::isnan(cfg.network_stations[1].phase_colour_sigma_zen_m));
    EXPECT_DOUBLE_EQ(cfg.network_stations[1].phase_colour_tau_s, 90.0);
    EXPECT_TRUE(std::isnan(cfg.network_stations[2].phase_colour_sigma_zen_m));
    EXPECT_TRUE(std::isnan(cfg.network_stations[2].phase_colour_tau_s));

    for (const char* bad : {
             R"({"network_stations":[{"name":"SNA000SIM","lat_deg":1,"lon_deg":2,"phase_colour_tau_s":0},
                                     {"name":"SNB000SIM","lat_deg":3,"lon_deg":4},
                                     {"name":"SNC000SIM","lat_deg":5,"lon_deg":6}]})",
             R"({"network_stations":[{"name":"SNA000SIM","lat_deg":1,"lon_deg":2,"phase_colour_sigma_zen_m":-1},
                                     {"name":"SNB000SIM","lat_deg":3,"lon_deg":4},
                                     {"name":"SNC000SIM","lat_deg":5,"lon_deg":6}]})" }) {
        const auto p = dir / "bad.json";
        writeText(p, bad);
        SimConfig c2;
        EXPECT_FALSE(loadSimConfig(p, c2, &err)) << bad;
        EXPECT_NE(err.find("phase_colour"), std::string::npos) << err;
    }
}

TEST(SimPhaseColourRoster, OverrideRoundTripsThroughWriteStationConfig) {
    // The base config states the network-level block; the roster overrides one
    // station.  The overridden station's child carries the override, the other
    // child inherits the base block verbatim, and both load through the real loader.
    const auto dir = tempDir("child");
    const auto base = dir / "base.json";
    writeText(base, R"({
        "phase_colour": { "sigma_zen_m": 0.005, "tau_s": 100.0, "seed": 777 },
        "network_stations": [
            {"name":"SNA000SIM","lat_deg":10,"lon_deg":20,
             "phase_colour_sigma_zen_m":0.0068,"phase_colour_tau_s":121.0},
            {"name":"SNB000SIM","lat_deg":11,"lon_deg":21},
            {"name":"SNC000SIM","lat_deg":12,"lon_deg":22} ]
    })");
    SimConfig net;
    std::string err;
    ASSERT_TRUE(loadSimConfig(base, net, &err)) << err;
    ASSERT_EQ(net.network_stations.size(), 3u);

    const auto childA = dir / "childA.json";
    const auto childB = dir / "childB.json";
    ASSERT_TRUE(writeStationConfig(base, net.network_stations[0], 0.0, 0.0, childA, &err)) << err;
    ASSERT_TRUE(writeStationConfig(base, net.network_stations[1], 0.0, 0.0, childB, &err)) << err;

    const nlohmann::json ja = readJson(childA), jb = readJson(childB);
    EXPECT_DOUBLE_EQ(ja["phase_colour"]["sigma_zen_m"].get<double>(), 0.0068);
    EXPECT_DOUBLE_EQ(ja["phase_colour"]["tau_s"].get<double>(), 121.0);
    EXPECT_EQ(ja["phase_colour"]["seed"].get<std::uint64_t>(), 777u);   // untouched knob survives
    EXPECT_EQ(jb["phase_colour"], nlohmann::json::parse(
                                      R"({ "sigma_zen_m": 0.005, "tau_s": 100.0, "seed": 777 })"));
    EXPECT_FALSE(ja.contains("network_stations"));

    SimConfig ca, cb;
    ASSERT_TRUE(loadSimConfig(childA, ca, &err)) << err;
    ASSERT_TRUE(loadSimConfig(childB, cb, &err)) << err;
    EXPECT_DOUBLE_EQ(ca.phase_colour.sigma_zen_m, 0.0068);
    EXPECT_DOUBLE_EQ(ca.phase_colour.tau_s, 121.0);
    EXPECT_EQ(ca.phase_colour.seed, 777u);
    EXPECT_DOUBLE_EQ(cb.phase_colour.sigma_zen_m, 0.005);
    EXPECT_DOUBLE_EQ(cb.phase_colour.tau_s, 100.0);
}

TEST(SimPhaseColourRoster, NoOverrideAndNoBlockWritesNoBlock) {
    // An existing recipe (no phase_colour anywhere) must produce child configs with
    // no phase_colour key at all, or the regenerated children would differ from the
    // archived ones.  A tau-only override on such a base stays OFF (sigma 0).
    const auto dir = tempDir("childlegacy");
    const auto base = dir / "base.json";
    writeText(base, R"({
        "trajectory": { "n_epochs": 20 },
        "network_stations": [
            {"name":"SNA000SIM","lat_deg":10,"lon_deg":20},
            {"name":"SNB000SIM","lat_deg":11,"lon_deg":21,"phase_colour_tau_s":80.0},
            {"name":"SNC000SIM","lat_deg":12,"lon_deg":22} ]
    })");
    SimConfig net;
    std::string err;
    ASSERT_TRUE(loadSimConfig(base, net, &err)) << err;

    const auto childA = dir / "childA.json";
    ASSERT_TRUE(writeStationConfig(base, net.network_stations[0], 0.0, 0.0, childA, &err)) << err;
    EXPECT_FALSE(readJson(childA).contains("phase_colour"));

    const auto childB = dir / "childB.json";
    ASSERT_TRUE(writeStationConfig(base, net.network_stations[1], 0.0, 0.0, childB, &err)) << err;
    SimConfig cb;
    ASSERT_TRUE(loadSimConfig(childB, cb, &err)) << err;
    EXPECT_DOUBLE_EQ(cb.phase_colour.tau_s, 80.0);
    EXPECT_EQ(cb.phase_colour.sigma_zen_m, 0.0);
}

TEST(SimPhaseColourRoster, OverrideLandsInsideTheSimulatorWrapper) {
    const auto dir = tempDir("childwrap");
    const auto base = dir / "base.json";
    writeText(base, R"({"simulator": {"trajectory": {"n_epochs": 25}}})");
    NetworkStation st;
    st.name = "SNC000SIM";
    st.phase_colour_sigma_zen_m = 0.0045;
    const auto child = dir / "child.json";
    std::string err;
    ASSERT_TRUE(writeStationConfig(base, st, 1.0, 2.0, child, &err)) << err;
    SimConfig cfg;
    ASSERT_TRUE(loadSimConfig(child, cfg, &err)) << err;
    EXPECT_DOUBLE_EQ(cfg.phase_colour.sigma_zen_m, 0.0045);
    EXPECT_DOUBLE_EQ(cfg.phase_colour.tau_s, 100.0);
}
