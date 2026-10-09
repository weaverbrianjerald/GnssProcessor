/**
 * @file main.cpp
 * @brief Entry point for the GNSS exam dataset simulator (gnss_simulator).
 *
 * Subcommands: selftest, probe-orbit, synth-static, synth-kinematic, synth-network,
 * spp-reference, verify and all.  The usage text printed by main() gives the
 * arguments of each.
 *
 * The @c selftest subcommand (milestone M1) synthesizes a small RINEX-3.04 mixed
 * observation file with @c RinexObsWriter, parses it back with the production
 * @c RinexParser, and checks that every written code and phase value round-trips.
 * It tests the byte-level writer/parser contract without any physical observation
 * model (orbits, clocks, atmosphere).
 *
 * Usage:
 *   gnss_simulator selftest [output_dir]
 */

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <random>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include <Eigen/Dense>
#include <nlohmann/json.hpp>

#include "common/Constants.hpp"
#include "config/GnssConfig.hpp"
#include "logging/LogManager.hpp"
#include "math/Coordinates.hpp"
#include "math/TimeUtils.hpp"
#include "model/ModelGeometry.hpp"
#include "products/LiveEphemerisStore.hpp"
#include "products/ProductCache.hpp"
#include "products/ProductParser.hpp"
#include "products/ProductTypes.hpp"
#include "products/RinexNavParser.hpp"
#include "rinex/RinexParser.hpp"
#include "sim/AmbiguityDraw.hpp"
#include "sim/EphemerisProvider.hpp"
#include "sim/IonoDisturbance.hpp"
#include "sim/PhaseColour.hpp"
#include "sim/RinexObsWriter.hpp"
#include "sim/SatClockError.hpp"
#include "sim/ScriptedSlips.hpp"
#include "sim/SimConfig.hpp"
#include "sim/TrajectoryGenerator.hpp"
#include "sim/Verifier.hpp"

using namespace GnssProcessor;

namespace {

/// Carrier wavelength (m) for the (system, RINEX code) pairs used by the selftest.
/// Any other pair returns the GPS L1 wavelength.
double wavelengthFor(char sys, const std::string& code) {
    const char band = code.size() >= 2 ? code[1] : '?';
    if (sys == 'G') {
        if (band == '1') return Constants::GPS_L1_WAVELENGTH;
        if (band == '2') return Constants::GPS_L2_WAVELENGTH;
        if (band == '5') return Constants::GPS_L5_WAVELENGTH;
    } else if (sys == 'E') {
        if (band == '1') return Constants::GAL_E1_WAVELENGTH;
        if (band == '5') return Constants::GAL_E5A_WAVELENGTH;
        if (band == '7') return Constants::GAL_E5B_WAVELENGTH;
    }
    return Constants::GPS_L1_WAVELENGTH;
}

int runSelfTest(const std::filesystem::path& outdir) {
    std::error_code ec;
    std::filesystem::create_directories(outdir, ec);

    // ---- Build the header: GPS L1/L2 + Galileo E1/E5a, dual frequency. ------
    sim::ObsHeaderSpec hdr;
    hdr.version = 3.04;
    hdr.run_date = "20260624 000000 UTC";
    hdr.approx_xyz = { -5063042.0, 4524238.0, -1761984.0 }; // near Fiji
    hdr.interval_s = 30.0;
    hdr.time_of_first_obs = TimeUtils::gregorianGpsToGpsTime(2024, 3, 15, 2, 0, 0.0);
    // Snap the start to the integer-second grid: the JD-based time conversion
    // carries an error of tens of microseconds, and a 30 s RINEX must land on
    // HH:MM:00 / HH:MM:30.  (The scenario generator builds integer TOW grids
    // directly; the selftest rounds instead.)
    hdr.time_of_first_obs.tow = std::round(hdr.time_of_first_obs.tow);
    hdr.time_system = "GPS";
    hdr.obs_types = {
        { 'G', { "C1C", "L1C", "C2W", "L2W" } },
        { 'E', { "C1C", "L1C", "C5Q", "L5Q" } },
    };

    // ---- Truth: a value per (satellite, code), reused across all epochs. -----
    // Code values [m] are arbitrary but Earth-surface plausible; phase values
    // [cycles] are the code range divided by the carrier wavelength.
    struct SatPlan {
        std::string id;
        double base_range_m;
    };
    const std::vector<SatPlan> plan = {
        { "G01", 21567812.340 },
        { "G07", 20011233.450 },
        { "E11", 23890145.220 },
    };

    std::map<std::string, std::map<std::string, double>> truth; // sat -> code -> value
    auto codesFor = [&](char sys) -> const std::vector<std::string>& {
        for (const auto& sc : hdr.obs_types)
            if (sc.first == sys) return sc.second;
        static const std::vector<std::string> empty;
        return empty;
    };

    std::vector<sim::SatObs> epoch_template;
    for (const auto& p : plan) {
        sim::SatObs so;
        so.sat_id = p.id;
        const char sys = p.id[0];
        int band_idx = 0;
        for (const auto& code : codesFor(sys)) {
            // Give each band a slightly different range so columns are distinct.
            const double range_m = p.base_range_m + 3.33 * band_idx;
            double value;
            if (code[0] == 'C') {
                value = std::round(range_m * 1000.0) / 1000.0; // 3-decimal metres
            } else { // 'L' carrier phase in cycles
                value = std::round((range_m / wavelengthFor(sys, code)) * 1000.0) / 1000.0;
            }
            so.values[code] = value;
            truth[p.id][code] = value;
            ++band_idx;
        }
        epoch_template.push_back(so);
    }

    // ---- Write 3 epochs at 30 s spacing. ------------------------------------
    const std::filesystem::path obs_path = outdir / "SARV_R_20240750200_01H_30S_MO.rnx";
    sim::RinexObsWriter writer;
    std::string err;
    if (!writer.open(obs_path, hdr, &err)) {
        std::cerr << "[selftest] FAIL: writer.open: " << err << "\n";
        return 1;
    }
    GnssTime t = hdr.time_of_first_obs;
    const int n_epochs = 3;
    for (int e = 0; e < n_epochs; ++e) {
        writer.writeEpoch(t, epoch_template, /*epoch_flag=*/0);
        t.tow += hdr.interval_s;
    }
    writer.close();
    std::cout << "[selftest] wrote " << obs_path.string() << "\n";

    // ---- Round-trip through the production RinexParser. ---------------------
    GnssConfig config; // defaults: rinex_types_ = "CL", systems = all
    config.setObservationPath(obs_path);

    LogManager log;
    log.initialize(outdir / "logs", LogLevel::Warn);
    ProductParser product_parser(config, log);
    RinexParser parser(config, log, product_parser);

    if (!parser.parse()) {
        std::cerr << "[selftest] FAIL: RinexParser::parse() returned false\n";
        return 1;
    }

    const auto& obs = parser.getObservations();
    const std::size_t expected_records =
        static_cast<std::size_t>(n_epochs) * plan.size(); // one record per (epoch, sat)
    std::cout << "[selftest] parsed " << obs.size() << " observation records ("
              << "expected " << expected_records << ")\n";

    int failures = 0;
    if (obs.size() != expected_records) {
        std::cerr << "[selftest] FAIL: record count mismatch\n";
        ++failures;
    }

    // Header round-trip checks.
    const auto& parsed_hdr = parser.getHeader();
    if (parsed_hdr.version < 3.039 || parsed_hdr.version > 3.041) {
        std::cerr << "[selftest] FAIL: version parsed as " << parsed_hdr.version << "\n";
        ++failures;
    }
    if (!parsed_hdr.interval || std::abs(*parsed_hdr.interval - 30.0) > 1e-6) {
        std::cerr << "[selftest] FAIL: interval not recovered as 30.0\n";
        ++failures;
    }

    // Value round-trip checks: every parsed measurement must match truth.
    const double tol = 5e-3;
    std::size_t checked = 0;
    std::map<std::string, std::map<std::string, bool>> seen;
    for (const auto& o : obs) {
        for (const auto& kv : o.measurements) {
            const std::string& code = kv.first;
            const double got = kv.second;
            auto sit = truth.find(o.satellite);
            if (sit == truth.end() || sit->second.find(code) == sit->second.end()) {
                std::cerr << "[selftest] FAIL: unexpected obs " << o.satellite << " " << code << "\n";
                ++failures;
                continue;
            }
            const double want = sit->second[code];
            if (std::abs(got - want) > tol) {
                std::cerr << "[selftest] FAIL: " << o.satellite << " " << code
                          << " got " << got << " want " << want << "\n";
                ++failures;
            }
            seen[o.satellite][code] = true;
            ++checked;
        }
    }

    // Every truth (sat, code) must have been observed at least once.
    for (const auto& sc : truth) {
        for (const auto& cv : sc.second) {
            if (!seen[sc.first][cv.first]) {
                std::cerr << "[selftest] FAIL: missing obs " << sc.first << " " << cv.first << "\n";
                ++failures;
            }
        }
    }

    std::cout << "[selftest] value comparisons: " << checked << ", failures: " << failures << "\n";
    if (failures == 0) {
        std::cout << "[selftest] PASS\n";
        return 0;
    }
    return 1;
}

// probe-orbit (milestone M2): validates the precise SP3/CLK chain to satellite
// position and clock.
//
// Builds the Application product chain (ProductParser -> ProductCache ->
// Model::Geometry) on the bundled offline fixture data/2024-129/, then compares
// Geometry::getSatellitePV with a tabulated SP3 node (1 cm bound) and
// getSatelliteClockCorrection with a CLK sample (1 us bound).  It also fails when
// the clock is reported as broadcast-grade: the processor then drops carrier
// phase, and the PPP part of the exam would run without phase and without an
// error (DESIGN.md §5.4).
int runProbeOrbit(const std::filesystem::path& outdir) {
    std::error_code ec;
    std::filesystem::create_directories(outdir, ec);

    const std::filesystem::path root = GNSS_PROJECT_ROOT;
    const std::filesystem::path base = root / "data" / "2024-129";
    const std::filesystem::path obs = base / "obs" / "gcgo00usa" / "GCGO00USA_R_20241290000_01D_30S_MO.rnx";
    const std::filesystem::path sp3 = base / "products" / "COD0MGXFIN_20241290000_01D_05M_ORB.SP3";
    const std::filesystem::path clk = base / "products" / "COD0MGXFIN_20241290000_01D_30S_CLK.CLK";
    for (const std::filesystem::path* p : { &obs, &sp3, &clk }) {
        if (!std::filesystem::exists(*p)) {
            std::cerr << "[probe-orbit] FAIL: missing fixture " << p->string() << "\n";
            return 1;
        }
    }

    // ---- Replicate Application::loadInputData() (the real chain). -----------
    GnssConfig config;
    config.setObservationPath(obs);
    config.setProductPaths({ sp3, clk });

    LogManager log;
    log.initialize(outdir / "logs", LogLevel::Warn);
    ProductParser product_parser(config, log);
    RinexParser rinex(config, log, product_parser);
    if (!rinex.parse()) {
        std::cerr << "[probe-orbit] FAIL: RinexParser::parse() on the fixture obs file\n";
        return 1;
    }
    product_parser.loadProducts(); // parses SP3 + CLK from config product paths

    ProductCache& cache = ProductCache::getInstance();
    cache.clearCache(); // process-wide singleton: start from an empty cache
    if (!cache.loadProducts(product_parser, log)) {
        std::cerr << "[probe-orbit] FAIL: ProductCache::loadProducts()\n";
        return 1;
    }
    std::cout << "[probe-orbit] cache loaded: " << cache.getOrbitCount() << " orbit nodes, "
              << cache.getClockCount() << " clock samples\n";
    if (cache.getOrbitCount() == 0 || cache.getClockCount() == 0) {
        std::cerr << "[probe-orbit] FAIL: empty orbit/clock cache\n";
        return 1;
    }

    Model::Geometry geom(config, cache, rinex, log); // ctor calls loadData() -> indexes orbits

    // ---- Pick the GPS satellite with the most tabulated nodes. -------------
    std::map<std::string, std::vector<const OrbitData*>> by_sat;
    for (const auto& o : cache.getOrbits()) {
        if (!o.satellite.empty() && o.satellite[0] == 'G') by_sat[o.satellite].push_back(&o);
    }
    if (by_sat.empty()) {
        std::cerr << "[probe-orbit] FAIL: no GPS orbit nodes in cache\n";
        return 1;
    }
    std::string sat;
    std::size_t best = 0;
    for (auto& kv : by_sat) {
        if (kv.second.size() > best) { best = kv.second.size(); sat = kv.first; }
    }
    auto nodes = by_sat[sat];
    std::sort(nodes.begin(), nodes.end(),
              [](const OrbitData* a, const OrbitData* b) { return a->time < b->time; });
    const OrbitData* node = nodes[nodes.size() / 2]; // mid-day node has interpolation neighbors

    int failures = 0;

    // ---- Orbit cross-check: interpolated position vs the SP3 node. ---------
    auto pv = geom.getSatellitePV(sat, node->time);
    const Eigen::Vector3d& pos = pv.first;
    const double pos_err_m = (pos - node->position).norm();
    std::cout << "[probe-orbit] sat " << sat << " @ week " << node->time.week
              << " tow " << node->time.tow << " (" << best << " nodes)\n";
    std::cout << "[probe-orbit]   getSatellitePV : " << pos.x() << ", " << pos.y() << ", " << pos.z() << " m\n";
    std::cout << "[probe-orbit]   SP3 node       : " << node->position.x() << ", " << node->position.y()
              << ", " << node->position.z() << " m\n";
    std::cout << "[probe-orbit]   |pos - SP3 node| = " << pos_err_m * 1000.0 << " mm; |pos| = "
              << pos.norm() / 1000.0 << " km\n";
    if (pos.norm() < 1.0e6) {
        std::cerr << "[probe-orbit] FAIL: degenerate satellite position\n";
        ++failures;
    }
    if (pos_err_m > 1.0e-2) {
        std::cerr << "[probe-orbit] FAIL: position error " << pos_err_m << " m exceeds 1 cm at an SP3 node\n";
        ++failures;
    }

    // ---- Clock cross-check: getSatelliteClockCorrection vs CLK sample. -----
    const ClockData* cnode = nullptr;
    double best_dt = 1.0e18;
    for (const auto& c : cache.getClocks()) {
        if (c.satellite != sat) continue;
        const double dt = std::abs(TimeUtils::continuousGpsSeconds(c.time) -
                                   TimeUtils::continuousGpsSeconds(node->time));
        if (dt < best_dt) { best_dt = dt; cnode = &c; }
    }
    if (!cnode) {
        std::cerr << "[probe-orbit] FAIL: no CLK sample for " << sat << "\n";
        ++failures;
    } else {
        bool broadcast_grade = false;
        const double clk_s = geom.getCorrections().getSatelliteClockCorrection(sat, cnode->time, &broadcast_grade);
        const double clk_err_s = std::abs(clk_s - cnode->clock_bias);
        std::cout << "[probe-orbit]   clock @ tow " << cnode->time.tow << ": getCorr = " << clk_s
                  << " s, CLK node = " << cnode->clock_bias << " s, err = " << clk_err_s * 1.0e9
                  << " ns, broadcast_grade = " << (broadcast_grade ? "true" : "false") << "\n";
        if (clk_err_s > 1.0e-6) {
            std::cerr << "[probe-orbit] FAIL: clock error " << clk_err_s << " s exceeds 1 us at a CLK sample\n";
            ++failures;
        }
        if (broadcast_grade) {
            std::cerr << "[probe-orbit] FAIL: clock returned broadcast-grade despite a precise CLK product "
                         "(PPP would drop carrier phase)\n";
            ++failures;
        }
    }

    if (failures == 0) {
        std::cout << "[probe-orbit] PASS - precise SP3/CLK -> getSatellitePV + precise clock chain validated\n";
        return 0;
    }
    return 1;
}

// ===========================================================================
// Forward pseudorange model (milestone M2).
//
// Uses the processor's geometry conventions (ModelGeometry.cpp) without deviation,
// so that synthesized observations invert against the verifier:
//   - emission time : obs.time - P/c - sat_clk   (pseudorange-based; rx clock cancels)
//   - Sagnac        : theta = omega_E * (||s_em - rx|| / c); sat_pos = R_z(theta) * s_em
//                     R_z = [[ cos, sin, 0],[-sin, cos, 0],[0,0,1]]
//   - model         : P = range + c*dt_r - c*sat_clk  (+noise)
// The P returned by forwardPseudorange carries no ionosphere, troposphere or
// relativity; the M7 correction terms are returned separately (m7_corr_out) for
// the caller to add.  The internal SPP check uses the same bare model, so it tests
// the geometry, clock, Sagnac and emission-time computation in isolation.
// ===========================================================================

GnssTime addSeconds(GnssTime t, double dt_s) {
    t.tow += dt_s;
    while (t.tow < 0.0) { t.tow += static_cast<double>(TimeUtils::SECONDS_IN_WEEK); t.week -= 1; }
    while (t.tow >= static_cast<double>(TimeUtils::SECONDS_IN_WEEK)) {
        t.tow -= static_cast<double>(TimeUtils::SECONDS_IN_WEEK);
        t.week += 1;
    }
    return t;
}

// Rotate an emission-time satellite ECEF position into the reception-time ECEF
// frame (Sagnac), using the geometric time of flight for the rotation angle, as
// Geometry::computeSatelliteGeometry does.
Eigen::Vector3d sagnacRotate(const Eigen::Vector3d& s_em, const Eigen::Vector3d& rx) {
    const double theta =
        Constants::EARTH_ANGULAR_VELOCITY * ((s_em - rx).norm() / Constants::SPEED_OF_LIGHT);
    Eigen::Matrix3d R;
    R << std::cos(theta), std::sin(theta), 0.0,
        -std::sin(theta), std::cos(theta), 0.0,
         0.0,             0.0,             1.0;
    return R * s_em;
}

// Forward pseudorange for one satellite at one epoch (no noise).  Iterates the
// pseudorange-based emission time to self-consistency.  Returns false if no
// orbit/clock covers the emission epoch.
bool forwardPseudorange(const sim::EphemerisProvider& eph, const std::string& sat,
                        const GnssTime& obs_time, const Eigen::Vector3d& rx, double cdt_r_m,
                        double& P_out, Eigen::Vector3d& sat_pos_out, Eigen::Vector3d& sat_vel_out,
                        double& sat_clk_out, double& elev_deg_out, double& az_deg_out,
                        const sim::SimConfig::Corrections& corr, double& m7_corr_out,
                        const sim::SatClockError* sat_clk_err = nullptr,
                        double* clk_err_m_out = nullptr) {
    const double c = Constants::SPEED_OF_LIGHT;

    // Seed P from a rough range at obs.time; also serves as a coverage pre-check.
    Eigen::Vector3d s0, v0;
    if (!eph.positionVelocity(sat, obs_time, s0, v0)) return false;
    double P = (sagnacRotate(s0, rx) - rx).norm();

    Eigen::Vector3d sat_pos = Eigen::Vector3d::Zero();
    Eigen::Vector3d s_em = Eigen::Vector3d::Zero(), v_em = Eigen::Vector3d::Zero(); // emission state
    double sat_clk = 0.0;
    double clk_err_m = 0.0;   // synthetic error at the converged emission time (metres)
    for (int it = 0; it < 8; ++it) {
        const GnssTime approx_em = addSeconds(obs_time, -P / c);
        const double sc_approx = eph.clock(sat, approx_em);
        const GnssTime em = addSeconds(obs_time, -(P / c + sc_approx));
        if (!eph.positionVelocity(sat, em, s_em, v_em)) return false;
        bool bcast = false;
        sat_clk = eph.clock(sat, em, &bcast);
        // Coverage test before the synthetic error.  |sat_clk| < 1e-12 also means "no
        // clock product covers this emission epoch; drop the satellite".  The synthetic
        // error is applied after this test: applied before it, the error could give an
        // uncovered satellite a clock, or cancel a real clock and drop a covered one.
        if (std::abs(sat_clk) < 1e-12) return false;
        // Synthetic satellite-clock error (SPEC_synthetic_minimum_network.md §3.1).
        // dt_true = dt_product + err/c, so the modeled pseudorange P is err metres
        // shorter.  The error is evaluated at the emission time, which differs per
        // station with the geometry; it remains a pure function of (satellite, time),
        // so N independent station processes see the same satellite-side error.
        clk_err_m = sat_clk_err ? sat_clk_err->metres(sat, em) : 0.0;
        if (clk_err_m != 0.0) sat_clk += clk_err_m / c;
        sat_pos = sagnacRotate(s_em, rx);
        const double range = (sat_pos - rx).norm();
        const double P_new = range + cdt_r_m - c * sat_clk;
        const bool converged = std::abs(P_new - P) < 1e-6;
        P = P_new;
        if (converged) break;
    }
    if (clk_err_m_out) *clk_err_m_out = clk_err_m;
    const auto topo = Model::Geometry::computeTopocentric(rx, sat_pos);
    P_out = P;
    sat_pos_out = sat_pos;      // reception-frame (Sagnac-rotated)
    sat_vel_out = v_em;         // emission-frame velocity (unrotated), as the processor's
                                // geom.sat_vel for the antenna attitude frame
    sat_clk_out = sat_clk;
    elev_deg_out = topo.elevation_deg;
    az_deg_out = topo.azimuth_deg;

    // M7 correction terms: non-dispersive, added to code and phase like the
    // troposphere, and left out of the bare internal SPP.  Relativity uses the
    // emission-time (pre-Sagnac) state; the range and site terms use the
    // reception-frame (rotated) sat_pos and rx.
    double m7 = 0.0;
    if (corr.relativity)       m7 += eph.relativityMetres(sat, s_em, v_em);
    if (corr.shapiro)          m7 += eph.shapiroMetres(sat, rx, sat_pos);
    if (corr.solid_earth_tide) m7 += eph.solidEarthTideMetres(rx, sat_pos, obs_time);
    if (corr.pole_tide)        m7 += eph.poleTideMetres(rx, sat_pos, obs_time);
    if (corr.ocean_tide)       m7 += eph.oceanTideMetres(rx, sat_pos, obs_time);
    m7_corr_out = m7;
    return true;
}

// One pseudorange for the internal SPP check.  @c is_gal carries the GPS->Galileo
// inter-system-bias unknown: GPS rows reference the receiver clock alone, Galileo
// rows reference receiver clock + ISB, as the processor adds a per-system
// time-offset state (MeasurementModel.cpp, getSystemTimeOffsetIndex).
struct SppObs {
    std::string sat;
    double P;
    bool is_gal;
};

// Single-point code solution equivalent to the processor's: recovers rx ECEF,
// c*dt_r and, when any Galileo row is present, the GPS->Galileo ISB from {sat,
// pseudorange} by Gauss-Newton, with the same emission/Sagnac/clock model as the
// synthesizer.  With Galileo present the normal system is 5x5 (x,y,z,cdt,isb);
// GPS-only it is 4x4.
bool solveSpp(const sim::EphemerisProvider& eph, const GnssTime& obs_time,
              const std::vector<SppObs>& obs,
              Eigen::Vector3d& rx_est, double& cdt_est, double& isb_est) {
    const double c = Constants::SPEED_OF_LIGHT;
    bool has_gal = false;
    for (const auto& o : obs) {
        if (o.is_gal) { has_gal = true; break; }
    }
    const int n = has_gal ? 5 : 4; // x, y, z, cdt [, isb]
    for (int iter = 0; iter < 15; ++iter) {
        Eigen::MatrixXd N = Eigen::MatrixXd::Zero(n, n);
        Eigen::VectorXd b = Eigen::VectorXd::Zero(n);
        int used = 0;
        for (const auto& o : obs) {
            const GnssTime approx_em = addSeconds(obs_time, -o.P / c);
            const double sc_approx = eph.clock(o.sat, approx_em);
            const GnssTime em = addSeconds(obs_time, -(o.P / c + sc_approx));
            Eigen::Vector3d s_em, v_em;
            if (!eph.positionVelocity(o.sat, em, s_em, v_em)) continue;
            const double sat_clk = eph.clock(o.sat, em);
            const Eigen::Vector3d sat_pos = sagnacRotate(s_em, rx_est);
            const double range = (sat_pos - rx_est).norm();
            const Eigen::Vector3d los = (sat_pos - rx_est) / range;
            const double isb_term = o.is_gal ? isb_est : 0.0;
            const double pred = range + cdt_est + isb_term - c * sat_clk;
            const double resid = o.P - pred;
            Eigen::VectorXd g(n); // d(pred)/d[rx, cdt, isb] = [-los, +1, is_gal]
            g(0) = -los.x();
            g(1) = -los.y();
            g(2) = -los.z();
            g(3) = 1.0;
            if (has_gal) g(4) = o.is_gal ? 1.0 : 0.0;
            N += g * g.transpose();
            b += g * resid;
            ++used;
        }
        if (used < n) return false;
        const Eigen::VectorXd dx = N.ldlt().solve(b);
        rx_est += dx.head<3>();
        cdt_est += dx(3);
        if (has_gal) isb_est += dx(4);
        if (dx.head<3>().norm() < 1e-4) break;
    }
    return true;
}

// Dilution of precision for an epoch's visible-satellite geometry, computed in
// the local ENU frame so HDOP/VDOP are meaningful: build the 4x4 normal matrix
// from unit line-of-sight + clock columns and invert.
struct Dop {
    double gdop{ 0 }, pdop{ 0 }, hdop{ 0 }, vdop{ 0 }, tdop{ 0 };
    bool ok{ false };
};
Dop computeDop(const Eigen::Vector3d& rx, const std::vector<Eigen::Vector3d>& sat_pos,
               const Coordinates::Llh& llh) {
    Dop d;
    if (sat_pos.size() < 4) return d;
    Eigen::Matrix4d HtH = Eigen::Matrix4d::Zero();
    for (const auto& sp : sat_pos) {
        const Eigen::Vector3d los_enu = Coordinates::ecefToEnu((sp - rx).normalized(), llh);
        Eigen::Vector4d h;
        h << los_enu.x(), los_enu.y(), los_enu.z(), 1.0;
        HtH += h * h.transpose();
    }
    const Eigen::Matrix4d Q = HtH.inverse();
    if (!Q.allFinite()) return d;
    const double qe = Q(0, 0), qn = Q(1, 1), qu = Q(2, 2), qt = Q(3, 3);
    d.hdop = std::sqrt(std::max(0.0, qe + qn));
    d.vdop = std::sqrt(std::max(0.0, qu));
    d.pdop = std::sqrt(std::max(0.0, qe + qn + qu));
    d.tdop = std::sqrt(std::max(0.0, qt));
    d.gdop = std::sqrt(std::max(0.0, qe + qn + qu + qt));
    d.ok = true;
    return d;
}

// ---------------------------------------------------------------------------
//  Fixture date and the quantities derived from it.
//
//  One derivation feeds the fixture directory, every product filename, the emitted
//  RINEX filename, the product-day edge guard and the scenario_summary date.  They
//  are coupled and change together: if an arc's start_tow_s moves to another day
//  while the emitted filename still names the old one, the processor resolves the
//  old day's orbits against the new epochs.  That is a multi-day extrapolation that
//  raises no error and appears only as a degraded solution.
// ---------------------------------------------------------------------------
struct FixtureDate {
    int year{ 2024 }, doy{ 129 };
    int week{ 0 };                 ///< GPS week of 00:00 that day.
    double day_start_tow{ 0.0 };   ///< tow of 00:00 that day (= dow * 86400).
    double day_end_tow{ 0.0 };     ///< tow of 24:00 that day.
    std::string yyyydoy;           ///< "2024131", as it appears in IGS long filenames.
    std::string dir;               ///< "2024-131", the data/ subdirectory.
    std::string iso;               ///< "2024-05-10", for human-facing output.
};

static FixtureDate makeFixtureDate(int year, int doy) {
    FixtureDate fd;
    fd.year = year;
    fd.doy = doy;
    // DOY -> calendar month/day (proleptic Gregorian leap rule).
    const bool leap = (year % 4 == 0 && year % 100 != 0) || (year % 400 == 0);
    const int mlen[12] = { 31, leap ? 29 : 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
    int month = 1, day = doy;
    for (int m = 0; m < 12 && day > mlen[m]; ++m) { day -= mlen[m]; ++month; }
    // gregorianGpsToGpsTime takes a GPS-scale civil date, so this is the GPS-time day
    // boundary (no leap-second shift), the boundary the products are cut on.
    const GnssTime t0 = TimeUtils::gregorianGpsToGpsTime(year, month, day, 0, 0, 0.0);
    fd.week = t0.week;
    fd.day_start_tow = t0.tow;
    fd.day_end_tow = t0.tow + 86400.0;
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%04d%03d", year, doy);
    fd.yyyydoy = buf;
    std::snprintf(buf, sizeof(buf), "%04d-%03d", year, doy);
    fd.dir = buf;
    std::snprintf(buf, sizeof(buf), "%04d-%02d-%02d", year, month, day);
    fd.iso = buf;
    return fd;
}

// ===========================================================================
//  spp-reference: author-side reference runs for exam Part B (SPEC/ANSWER_KEY).
//
//  Reads the emitted hero RINEX back through the production RinexParser (the data
//  as a student sees it, not as the synthesizer holds it) and computes the two
//  graded Part-B solutions at every epoch by epoch-wise weighted Gauss-Newton.  The
//  model is the one the brief asks of a student and no more: emission time +
//  Sagnac, SV clock including the periodic relativistic term, and hydrostatic
//  troposphere.  Tides, antenna, Shapiro and ZWD are not modeled; they stay in the
//  data as unmodeled error, which the answer key has to quantify.
//
//    leg 1  "the real-time view":  GPS L1 C1C, orbits and clocks from the real
//           broadcast navigation file through the production RinexNavParser ->
//           LiveEphemerisStore chain (fed epoch-synchronously, so each epoch
//           sees only the ephemerides a live receiver would hold), +TGD,
//           +broadcast Klobuchar.
//    leg 2  "the analyst's check": GPS C1C/C2W ionosphere-free code with the
//           precise COD MGX SP3/CLK, observables OSB-corrected from the .BIA.
//
//  A/B variants (leg 1 without Klobuchar, leg 1 without TGD, leg 1 on precise
//  SP3/CLK, leg 2 without OSB) quantify the individual terms for the rubric.  Per-epoch errors against
//  truth_position.csv go to <truth_dir>/truth_spp.csv, summary statistics to
//  <truth_dir>/truth_spp_summary.json.  Both are key-side artifacts: the handout
//  ship-list does not include truth_*.csv, so nothing reaches the students.
// ===========================================================================

struct RefObs {
    std::string sat;
    double P{ 0.0 };  ///< the (possibly bias-corrected / IF-combined) code observable [m]
};

struct RefSol {
    bool ok{ false };
    Eigen::Vector3d rx{ Eigen::Vector3d::Zero() };
    double cdt{ 0.0 };   ///< receiver clock c*dt [m]
    int n_used{ 0 };
    double pdop{ 0.0 };
    /// Formal 1-sigma of the epoch's position in the LOCAL ENU frame [m].
    /// Part B asks for a documented stochastic model at three epochs, so the
    /// reference publishes the sigmas that model yields, not only DOP.  Scaled by
    /// the a-posteriori variance factor (below) rather than an assumed a-priori
    /// code sigma: the weights w = sin^2(el) are relative, and with the residuals
    /// setting the scale the sigmas grow when the storm increases leg 1's misfit
    /// (the Part-B/D teaching point).
    double sig_e{ 0.0 }, sig_n{ 0.0 }, sig_u{ 0.0 };
    /// sqrt of the a-posteriori variance factor, sigma0 = sqrt(sum(w*v^2)/(n-4)) [m].
    double sig0{ 0.0 };
};

// One epoch-wise elevation-weighted (w = sin^2 el) Gauss-Newton code solution
// (x, y, z, c*dt).  @p brdc selects the orbit/clock source: non-null = the
// LiveEphemerisStore broadcast chain (leg 1; gated healthy-only at insert and
// |t - toe| <= 7200 s here, the LNAV fit interval), null = the provider's
// precise SP3/CLK (leg 2).  Emission time iterates from the observed
// pseudorange, as in the synthesizer's forward model, so the convention
// cancels.  The relativistic periodic term is computed from the emission-state
// PV of whichever orbit source is active (equivalent to the broadcast
// F*e*sqrtA*sinE form at the sub-mm level).
RefSol solveReferenceEpoch(const sim::EphemerisProvider& eph, LiveEphemerisStore* brdc,
                           const std::map<std::string, double>* tgd_s, bool apply_tgd,
                           bool apply_klob, const GnssTime& t, const std::vector<RefObs>& obs,
                           const Eigen::Vector3d& rx0, double cdt0, double mask_deg) {
    const double c = Constants::SPEED_OF_LIGHT;
    RefSol sol;
    Eigen::Vector3d rx = rx0;
    double cdt = cdt0;

    // Shared per-observation model evaluation (GN accumulation + final DOP pass).
    auto evalObs = [&](const RefObs& o, double& resid, Eigen::Vector3d& los, double& w,
                       Eigen::Vector3d& sat_pos_out) -> bool {
        const GnssTime em0 = addSeconds(t, -o.P / c);
        double clk0 = 0.0;
        if (brdc) {
            const auto age = brdc->toeAgeSeconds(o.sat, t);
            if (!age || std::abs(*age) > 7200.0) return false; // outside the fit interval
            const auto ck = brdc->computeSatelliteClock(o.sat, em0);
            if (!ck) return false;
            clk0 = ck->bias;
        } else {
            clk0 = eph.clock(o.sat, em0);
            if (std::abs(clk0) < 1e-12) return false; // no precise clock at this epoch
        }
        const GnssTime em = addSeconds(t, -(o.P / c + clk0));
        Eigen::Vector3d s_em, v_em;
        double clk = 0.0;
        if (brdc) {
            const auto st = brdc->computeSatellitePV(o.sat, em);
            const auto ck = brdc->computeSatelliteClock(o.sat, em);
            if (!st || !ck) return false;
            s_em = st->position;
            v_em = st->velocity;
            clk = ck->bias;
        } else {
            if (!eph.positionVelocity(o.sat, em, s_em, v_em)) return false;
            clk = eph.clock(o.sat, em);
            if (std::abs(clk) < 1e-12) return false;
        }
        const Eigen::Vector3d sat_pos = sagnacRotate(s_em, rx);
        const double range = (sat_pos - rx).norm();
        const auto topo = Model::Geometry::computeTopocentric(rx, sat_pos);
        if (topo.elevation_deg < mask_deg) return false;
        double mf_d = 1.0, mf_w = 1.0;
        eph.tropoMapping(topo.elevation_deg, rx, t, mf_d, mf_w);
        double pred = range + cdt - c * clk + eph.relativityMetres(o.sat, s_em, v_em) +
                      eph.tropoZhd(rx, t) * mf_d;
        if (apply_tgd && tgd_s) {
            const auto it = tgd_s->find(o.sat);
            if (it != tgd_s->end()) pred += c * it->second; // dt_L1 = af - TGD => pred += c*TGD
        }
        if (apply_klob)
            pred += eph.klobucharDelayMetres(rx, sat_pos, t, Constants::GPS_L1_FREQ);
        resid = o.P - pred;
        los = (sat_pos - rx) / range;
        const double sin_el = std::sin(topo.elevation_deg * Constants::PI / 180.0);
        w = sin_el * sin_el;
        sat_pos_out = sat_pos;
        return true;
    };

    for (int iter = 0; iter < 12; ++iter) {
        Eigen::Matrix4d N = Eigen::Matrix4d::Zero();
        Eigen::Vector4d b = Eigen::Vector4d::Zero();
        int used = 0;
        for (const auto& o : obs) {
            double resid = 0.0, w = 0.0;
            Eigen::Vector3d los, sp;
            if (!evalObs(o, resid, los, w, sp)) continue;
            Eigen::Vector4d g;
            g << -los.x(), -los.y(), -los.z(), 1.0;
            N += w * g * g.transpose();
            b += w * g * resid;
            ++used;
        }
        if (used < 4) return sol;
        const Eigen::Vector4d dx = N.ldlt().solve(b);
        if (!dx.allFinite()) return sol;
        rx += dx.head<3>();
        cdt += dx(3);
        if (dx.head<3>().norm() < 1e-4) break;
    }
    if (!rx.allFinite() || (rx - rx0).norm() > 5.0e6) return sol; // diverged

    // Final pass at the solution: the used-satellite set, its DOP, and the formal
    // covariance.  N is rebuilt here (not reused from the last iteration) so it is
    // evaluated at the converged point; the pass also accumulates the weighted residual
    // sum of squares for the a-posteriori variance factor.
    std::vector<Eigen::Vector3d> used_pos;
    Eigen::Matrix4d N = Eigen::Matrix4d::Zero();
    double wvv = 0.0;
    for (const auto& o : obs) {
        double resid = 0.0, w = 0.0;
        Eigen::Vector3d los, sp;
        if (!evalObs(o, resid, los, w, sp)) continue;
        used_pos.push_back(sp);
        Eigen::Vector4d g;
        g << -los.x(), -los.y(), -los.z(), 1.0;
        N += w * g * g.transpose();
        wvv += w * resid * resid;
    }
    if (used_pos.size() < 4) return sol;
    const Dop dop = computeDop(rx, used_pos, Coordinates::ecefToLlh(rx));
    sol.ok = true;
    sol.rx = rx;
    sol.cdt = cdt;
    sol.n_used = static_cast<int>(used_pos.size());
    sol.pdop = dop.ok ? dop.pdop : 0.0;

    // Cov(x) = sigma0^2 * N^-1 with sigma0^2 = sum(w*v^2)/(n-4); rotate the 3x3
    // position block into ENU so sigma_e/n/u are directly comparable with what a
    // student reports.  Redundancy n-4 <= 0 (a 4-satellite fix) leaves sigma at 0.
    const int dof = static_cast<int>(used_pos.size()) - 4;
    if (dof > 0) {
        const double var0 = wvv / static_cast<double>(dof);
        Eigen::Matrix4d Ninv = Eigen::Matrix4d::Zero();
        const Eigen::FullPivLU<Eigen::Matrix4d> lu(N);
        if (lu.isInvertible()) {
            Ninv = lu.inverse();
            const Eigen::Matrix3d cov_ecef = var0 * Ninv.topLeftCorner<3, 3>();
            const auto llh = Coordinates::ecefToLlh(rx);
            const double sla = std::sin(llh.lat_rad), cla = std::cos(llh.lat_rad);
            const double slo = std::sin(llh.lon_rad), clo = std::cos(llh.lon_rad);
            Eigen::Matrix3d R;
            R << -slo, clo, 0.0,
                 -sla * clo, -sla * slo, cla,
                  cla * clo, cla * slo, sla;
            const Eigen::Matrix3d cov_enu = R * cov_ecef * R.transpose();
            sol.sig_e = std::sqrt(std::max(0.0, cov_enu(0, 0)));
            sol.sig_n = std::sqrt(std::max(0.0, cov_enu(1, 1)));
            sol.sig_u = std::sqrt(std::max(0.0, cov_enu(2, 2)));
            sol.sig0 = std::sqrt(std::max(0.0, var0));
        }
    }
    return sol;
}

int runSppReference(const std::filesystem::path& truth_dir, const sim::SimConfig& scfg) {
    const std::string tag = "spp-ref";

    const FixtureDate fd = makeFixtureDate(scfg.date.year, scfg.date.doy);
    const std::filesystem::path root = GNSS_PROJECT_ROOT;
    const std::filesystem::path base = root / "data" / fd.dir;
    const std::filesystem::path setup_obs = base / "obs" / "gcgo00usa" /
        ("GCGO00USA_R_" + fd.yyyydoy + "0000_01D_30S_MO.rnx");
    const std::filesystem::path sp3 = base / "products" / ("COD0MGXFIN_" + fd.yyyydoy + "0000_01D_05M_ORB.SP3");
    const std::filesystem::path clk = base / "products" / ("COD0MGXFIN_" + fd.yyyydoy + "0000_01D_30S_CLK.CLK");
    const std::filesystem::path nav = base / "products" / ("BRDC00IGS_R_" + fd.yyyydoy + "0000_01D_MN.rnx");
    const std::filesystem::path bia = base / "products" / ("COD0MGXFIN_" + fd.yyyydoy + "0000_01D_01D_OSB.BIA");
    for (const std::filesystem::path* p : { &setup_obs, &sp3, &clk, &nav }) {
        if (!std::filesystem::exists(*p)) {
            std::cerr << "[" << tag << "] FAIL: missing fixture " << p->string() << "\n";
            return 1;
        }
    }
    // Leg 2 corrects the observables with the same OSBs the synthesizer injected.
    // Without the .BIA the two leg-2 variants coincide with no other sign, so a
    // warning is printed.
    // A phase-only scope (card ARL) injected no code OSB, so leg 2 removes none.
    const bool want_osb = scfg.corrections.signal_bias && std::filesystem::exists(bia) &&
                          scfg.corrections.signal_bias_scope != "phase";
    if (scfg.corrections.signal_bias && !want_osb)
        std::cerr << "[" << tag << "] WARN: corrections.signal_bias is set but the .BIA is "
                     "missing; leg 2 runs bias-blind\n";

    // --- The emitted hero RINEX: exactly one *_MO.rnx in the truth dir. --------
    std::filesystem::path obs_rnx;
    int n_rnx = 0;
    for (const auto& de : std::filesystem::directory_iterator(truth_dir)) {
        if (!de.is_regular_file()) continue;
        const std::string name = de.path().filename().string();
        if (name.size() > 7 && name.compare(name.size() - 7, 7, "_MO.rnx") == 0) {
            obs_rnx = de.path();
            ++n_rnx;
        }
    }
    if (n_rnx != 1) {
        std::cerr << "[" << tag << "] FAIL: expected exactly one *_MO.rnx in "
                  << truth_dir.string() << ", found " << n_rnx << "\n";
        return 1;
    }

    // --- Truth trajectory, keyed on the integer-ms tow grid. -------------------
    struct TruthRow {
        Eigen::Vector3d xyz;
        double lat_deg{ 0 }, lon_deg{ 0 }, h_m{ 0 };
    };
    std::map<long long, TruthRow> truth;
    {
        std::ifstream tf(truth_dir / "truth_position.csv");
        if (!tf) {
            std::cerr << "[" << tag << "] FAIL: cannot open " << (truth_dir / "truth_position.csv").string() << "\n";
            return 1;
        }
        auto split = [](const std::string& s) {
            std::vector<std::string> out;
            std::size_t p = 0;
            while (true) {
                const std::size_t q = s.find(',', p);
                out.push_back(s.substr(p, q == std::string::npos ? q : q - p));
                if (q == std::string::npos) break;
                p = q + 1;
            }
            return out;
        };
        std::string line;
        if (!std::getline(tf, line)) {
            std::cerr << "[" << tag << "] FAIL: empty truth_position.csv\n";
            return 1;
        }
        const auto hdr = split(line);
        auto col = [&](const char* name) {
            for (std::size_t i = 0; i < hdr.size(); ++i)
                if (hdr[i] == name) return static_cast<int>(i);
            return -1;
        };
        const int c_tow = col("tow_s"), c_x = col("x_m"), c_y = col("y_m"), c_z = col("z_m");
        const int c_lat = col("lat_deg"), c_lon = col("lon_deg"), c_h = col("h_m");
        if (c_tow < 0 || c_x < 0 || c_y < 0 || c_z < 0 || c_lat < 0 || c_lon < 0 || c_h < 0) {
            std::cerr << "[" << tag << "] FAIL: truth_position.csv missing a required column\n";
            return 1;
        }
        while (std::getline(tf, line)) {
            if (line.empty()) continue;
            const auto v = split(line);
            const int need = std::max({ c_tow, c_x, c_y, c_z, c_lat, c_lon, c_h });
            if (static_cast<int>(v.size()) <= need) continue;
            TruthRow r;
            const double tow = std::atof(v[c_tow].c_str());
            r.xyz = Eigen::Vector3d(std::atof(v[c_x].c_str()), std::atof(v[c_y].c_str()),
                                    std::atof(v[c_z].c_str()));
            r.lat_deg = std::atof(v[c_lat].c_str());
            r.lon_deg = std::atof(v[c_lon].c_str());
            r.h_m = std::atof(v[c_h].c_str());
            truth[static_cast<long long>(std::llround(tow * 1000.0))] = r;
        }
    }
    if (truth.empty()) {
        std::cerr << "[" << tag << "] FAIL: no rows parsed from truth_position.csv\n";
        return 1;
    }

    // --- Precise chain + Klobuchar + OSB via the standard provider. ------------
    std::vector<std::filesystem::path> products = { sp3, clk };
    if (want_osb) products.push_back(bia);
    sim::EphemerisProvider eph(setup_obs, products, truth_dir / "logs_spp" / "provider", nav);
    if (!eph.ok()) {
        std::cerr << "[" << tag << "] FAIL: EphemerisProvider setup\n";
        return 1;
    }
    if (!eph.hasKlobuchar()) {
        std::cerr << "[" << tag << "] FAIL: no broadcast Klobuchar field (GPSA/GPSB) in "
                  << nav.string() << " — leg 1 cannot run the bridge stack\n";
        return 1;
    }
    if (want_osb && !eph.hasSignalBias()) {
        std::cerr << "[" << tag << "] FAIL: .BIA present but no OSB entries loaded\n";
        return 1;
    }

    // --- Broadcast ephemerides: healthy GPS records, sorted by toe. ------------
    auto toeAbs = [](const BroadcastEphemeris& r) {
        return r.kepler.toe.week * 604800.0 + r.kepler.toe.tow;
    };
    std::vector<BroadcastEphemeris> brdc;
    int n_unhealthy = 0;
    for (auto& r : RinexNavParser::parse(nav.string())) {
        if (r.satellite.empty() || r.satellite[0] != 'G') continue;
        if (r.health != 0) {
            ++n_unhealthy;
            continue;
        }
        brdc.push_back(r);
    }
    std::sort(brdc.begin(), brdc.end(),
              [&](const BroadcastEphemeris& a, const BroadcastEphemeris& b) {
                  return toeAbs(a) < toeAbs(b);
              });
    if (brdc.empty()) {
        std::cerr << "[" << tag << "] FAIL: no healthy GPS broadcast ephemerides in " << nav.string() << "\n";
        return 1;
    }
    std::cout << "[" << tag << "] " << brdc.size() << " healthy GPS broadcast ephemerides ("
              << n_unhealthy << " unhealthy skipped); OSB " << (want_osb ? "ON" : "OFF") << "\n";

    // --- The student's view: parse the emitted RINEX back.  Reuses the
    // provider's LogManager: spdlog logger names are process-global, so a second
    // LogManager here would fail to initialize.
    GnssConfig obs_config;
    obs_config.setObservationPath(obs_rnx);
    ProductParser obs_pp(obs_config, eph.logManager());
    RinexParser obs_parser(obs_config, eph.logManager(), obs_pp);
    if (!obs_parser.parse()) {
        std::cerr << "[" << tag << "] FAIL: RinexParser::parse() on " << obs_rnx.string() << "\n";
        return 1;
    }
    const auto& all_obs = obs_parser.getObservations();
    struct EpochObs {
        GnssTime t;
        std::vector<const RinexObservation*> recs;
    };
    std::vector<EpochObs> epochs;
    for (const auto& o : all_obs) {
        if (epochs.empty() || epochs.back().t.week != o.time.week ||
            std::abs(epochs.back().t.tow - o.time.tow) > 1e-6)
            epochs.push_back({ o.time, {} });
        epochs.back().recs.push_back(&o);
    }
    std::cout << "[" << tag << "] " << obs_rnx.filename().string() << ": " << all_obs.size()
              << " records over " << epochs.size() << " epochs\n";

    // --- Epoch loop: 6 solutions per epoch. ------------------------------------
    const double c = Constants::SPEED_OF_LIGHT;
    const double f1 = Constants::GPS_L1_FREQ, f2 = Constants::GPS_L2_FREQ;
    const double a1 = f1 * f1 / (f1 * f1 - f2 * f2);   // IF combination coefficients
    const double a2 = f2 * f2 / (f1 * f1 - f2 * f2);
    const double mask_deg = scfg.geometry.elevation_mask_deg;
    const double d2r = Constants::PI / 180.0;

    auto& store = LiveEphemerisStore::getInstance();
    store.clear();
    std::map<std::string, double> tgd_s;   // per-sat TGD of the latest inserted record
    std::size_t next_eph = 0;

    Eigen::Vector3d seed_l1 = obs_parser.getApproxPositionXyz();
    Eigen::Vector3d seed_if = seed_l1;
    double seed_l1_cdt = 0.0, seed_if_cdt = 0.0;

    std::ofstream csv(truth_dir / "truth_spp.csv");
    // The *_sig_[enu]_m / *_sig0_m columns give each leg's formal per-epoch
    // uncertainty (a-posteriori-scaled, ENU) next to its error, so Part B's
    // "document your stochastic model" has a reference at whichever three epochs
    // the student picked.  DOP alone describes geometry only, not the measurement
    // noise the student assumed.
    csv << "week,tow_s,l1_n,l1_pdop,l1_cdt_m,l1_e_m,l1_n_m,l1_u_m,l1_3d_m,"
           "l1_sig_e_m,l1_sig_n_m,l1_sig_u_m,l1_sig0_m,"
           "l1_noklob_3d_m,l1_notgd_3d_m,l1_prec_3d_m,if_n,if_pdop,if_cdt_m,if_e_m,if_n_m,if_u_m,"
           "if_3d_m,if_sig_e_m,if_sig_n_m,if_sig_u_m,if_sig0_m,if_noosb_3d_m\n";
    csv << std::fixed;

    struct Acc {
        std::vector<double> d3, dh, du;
        int fail{ 0 };
        long long n_used_sum{ 0 };
        std::array<double, 24> hr_sum{};
        std::array<int, 24> hr_n{};
    };
    Acc a_l1, a_nk, a_nt, a_pr, a_if, a_no;
    std::vector<double> delta_klob, delta_tgd, delta_osb, delta_brdc; // paired per-epoch 3D deltas
    int n_truth_miss = 0;

    for (const auto& ep : epochs) {
        const double t_abs = ep.t.week * 604800.0 + ep.t.tow;
        // Feed the store epoch-synchronously: a live receiver holds the currently
        // broadcast data set, whose toe can sit up to ~2 h ahead of clock time.
        while (next_eph < brdc.size() && toeAbs(brdc[next_eph]) <= t_abs + 7200.0) {
            store.insert(brdc[next_eph]);
            tgd_s[brdc[next_eph].satellite] = brdc[next_eph].kepler.tgd0;
            ++next_eph;
        }

        std::vector<RefObs> l1, if_osb, if_raw;
        for (const auto* r : ep.recs) {
            if (r->satellite.empty() || r->satellite[0] != 'G') continue;
            const auto c1 = r->measurements.find("C1C");
            if (c1 == r->measurements.end() || !((*c1).second > 1.0e6)) continue;
            l1.push_back({ r->satellite, (*c1).second });
            const auto c2 = r->measurements.find("C2W");
            if (c2 == r->measurements.end() || !((*c2).second > 1.0e6)) continue;
            const double b1 = want_osb ? eph.signalBiasMetres(r->satellite, "C1C", ep.t) : 0.0;
            const double b2 = want_osb ? eph.signalBiasMetres(r->satellite, "C2W", ep.t) : 0.0;
            if_osb.push_back({ r->satellite, a1 * ((*c1).second - b1) - a2 * ((*c2).second - b2) });
            if_raw.push_back({ r->satellite, a1 * (*c1).second - a2 * (*c2).second });
        }

        const RefSol s_l1 = solveReferenceEpoch(eph, &store, &tgd_s, true, true, ep.t, l1,
                                                seed_l1, seed_l1_cdt, mask_deg);
        const RefSol s_nk = solveReferenceEpoch(eph, &store, &tgd_s, true, false, ep.t, l1,
                                                seed_l1, seed_l1_cdt, mask_deg);
        const RefSol s_nt = solveReferenceEpoch(eph, &store, &tgd_s, false, true, ep.t, l1,
                                                seed_l1, seed_l1_cdt, mask_deg);
        // Precise-orbit/clock twin of the no-TGD broadcast leg (same raw C1C, same
        // Klobuchar, identical bias content on both sides): the paired delta
        // against s_nt isolates the broadcast orbit/clock error alone.
        const RefSol s_pr = solveReferenceEpoch(eph, nullptr, nullptr, false, true, ep.t, l1,
                                                seed_l1, seed_l1_cdt, mask_deg);
        const RefSol s_if = solveReferenceEpoch(eph, nullptr, nullptr, false, false, ep.t, if_osb,
                                                seed_if, seed_if_cdt, mask_deg);
        const RefSol s_no = solveReferenceEpoch(eph, nullptr, nullptr, false, false, ep.t, if_raw,
                                                seed_if, seed_if_cdt, mask_deg);
        if (s_l1.ok) { seed_l1 = s_l1.rx; seed_l1_cdt = s_l1.cdt; }
        if (s_if.ok) { seed_if = s_if.rx; seed_if_cdt = s_if.cdt; }

        const auto tr = truth.find(static_cast<long long>(std::llround(ep.t.tow * 1000.0)));
        if (tr == truth.end()) ++n_truth_miss;
        // Bucket on integer milliseconds: the parser's calendar->tow conversion can
        // sit an ulp below an exact hour boundary, which would drop boundary epochs
        // into the previous UT-hour bucket and shift every label by one.
        const double rel_ms = static_cast<double>(std::llround((ep.t.tow - fd.day_start_tow) * 1000.0));
        const int hr = std::max(0, std::min(23, static_cast<int>(rel_ms / 3600000.0)));

        // ENU error of one solution against this epoch's truth; nan when unsolved.
        auto errOf = [&](const RefSol& s, Eigen::Vector3d& enu, double& d3) -> bool {
            if (!s.ok || tr == truth.end()) return false;
            const Eigen::Vector3d dx = s.rx - tr->second.xyz;
            const Coordinates::Llh llh{ tr->second.lat_deg * d2r, tr->second.lon_deg * d2r,
                                        tr->second.h_m };
            enu = Coordinates::ecefToEnu(dx, llh);
            d3 = dx.norm();
            return true;
        };
        auto accum = [&](const RefSol& s, Acc& a, Eigen::Vector3d& enu, double& d3) {
            if (!errOf(s, enu, d3)) {
                ++a.fail;
                return false;
            }
            a.d3.push_back(d3);
            a.dh.push_back(std::hypot(enu.x(), enu.y()));
            a.du.push_back(std::abs(enu.z()));
            a.n_used_sum += s.n_used;
            a.hr_sum[hr] += d3;
            a.hr_n[hr] += 1;
            return true;
        };
        Eigen::Vector3d e_l1, e_nk, e_nt, e_pr, e_if, e_no;
        double d_l1 = 0, d_nk = 0, d_nt = 0, d_pr = 0, d_if = 0, d_no = 0;
        const bool ok_l1 = accum(s_l1, a_l1, e_l1, d_l1);
        const bool ok_nk = accum(s_nk, a_nk, e_nk, d_nk);
        const bool ok_nt = accum(s_nt, a_nt, e_nt, d_nt);
        const bool ok_pr = accum(s_pr, a_pr, e_pr, d_pr);
        const bool ok_if = accum(s_if, a_if, e_if, d_if);
        const bool ok_no = accum(s_no, a_no, e_no, d_no);
        if (ok_l1 && ok_nk) delta_klob.push_back(d_nk - d_l1);
        if (ok_l1 && ok_nt) delta_tgd.push_back(d_nt - d_l1);
        if (ok_if && ok_no) delta_osb.push_back(d_no - d_if);
        if (ok_nt && ok_pr) delta_brdc.push_back(d_nt - d_pr);

        auto num = [&](double v, int prec) {
            if (std::isfinite(v))
                csv << std::setprecision(prec) << v;
            else
                csv << "nan";
        };
        csv << ep.t.week << ",";
        num(ep.t.tow, 3);
        csv << "," << (s_l1.ok ? s_l1.n_used : 0) << ",";
        num(s_l1.ok ? s_l1.pdop : std::nan(""), 2);
        csv << ",";
        num(s_l1.ok ? s_l1.cdt : std::nan(""), 3);
        csv << ",";
        num(ok_l1 ? e_l1.x() : std::nan(""), 4);
        csv << ",";
        num(ok_l1 ? e_l1.y() : std::nan(""), 4);
        csv << ",";
        num(ok_l1 ? e_l1.z() : std::nan(""), 4);
        csv << ",";
        num(ok_l1 ? d_l1 : std::nan(""), 4);
        csv << ",";
        num(s_l1.ok ? s_l1.sig_e : std::nan(""), 4);
        csv << ",";
        num(s_l1.ok ? s_l1.sig_n : std::nan(""), 4);
        csv << ",";
        num(s_l1.ok ? s_l1.sig_u : std::nan(""), 4);
        csv << ",";
        num(s_l1.ok ? s_l1.sig0 : std::nan(""), 4);
        csv << ",";
        num(ok_nk ? d_nk : std::nan(""), 4);
        csv << ",";
        num(ok_nt ? d_nt : std::nan(""), 4);
        csv << ",";
        num(ok_pr ? d_pr : std::nan(""), 4);
        csv << "," << (s_if.ok ? s_if.n_used : 0) << ",";
        num(s_if.ok ? s_if.pdop : std::nan(""), 2);
        csv << ",";
        num(s_if.ok ? s_if.cdt : std::nan(""), 3);
        csv << ",";
        num(ok_if ? e_if.x() : std::nan(""), 4);
        csv << ",";
        num(ok_if ? e_if.y() : std::nan(""), 4);
        csv << ",";
        num(ok_if ? e_if.z() : std::nan(""), 4);
        csv << ",";
        num(ok_if ? d_if : std::nan(""), 4);
        csv << ",";
        num(s_if.ok ? s_if.sig_e : std::nan(""), 4);
        csv << ",";
        num(s_if.ok ? s_if.sig_n : std::nan(""), 4);
        csv << ",";
        num(s_if.ok ? s_if.sig_u : std::nan(""), 4);
        csv << ",";
        num(s_if.ok ? s_if.sig0 : std::nan(""), 4);
        csv << ",";
        num(ok_no ? d_no : std::nan(""), 4);
        csv << "\n";
    }
    csv.close();

    // --- Summary statistics + JSON. --------------------------------------------
    struct VarStats {
        int n{ 0 };
        double median{ 0 }, mean{ 0 }, p90{ 0 }, max{ 0 };
    };
    auto statsOf = [](const std::vector<double>& in) {
        VarStats s;
        if (in.empty()) return s;
        std::vector<double> v = in;
        std::sort(v.begin(), v.end());
        s.n = static_cast<int>(v.size());
        s.median = v[v.size() / 2];
        double sum = 0;
        for (double x : v) sum += x;
        s.mean = sum / static_cast<double>(v.size());
        s.p90 = v[static_cast<std::size_t>(std::floor(0.9 * static_cast<double>(v.size() - 1)))];
        s.max = v.back();
        return s;
    };
    auto meanOf = [](const std::vector<double>& v) {
        if (v.empty()) return 0.0;
        double s = 0;
        for (double x : v) s += x;
        return s / static_cast<double>(v.size());
    };

    nlohmann::json j;
    j["generated_by"] = "gnss_simulator spp-reference";
    j["date"] = { { "year", scfg.date.year }, { "doy", scfg.date.doy }, { "iso", fd.iso } };
    j["obs_rinex"] = obs_rnx.filename().string();
    j["n_epochs"] = epochs.size();
    j["elevation_mask_deg"] = mask_deg;
    j["estimator"] = "epoch-wise Gauss-Newton WLS (x,y,z,c*dt), w = sin^2(el)";
    j["modeled"] = "emission time + Sagnac; SV clock (+ periodic relativistic); "
                   "Saastamoinen ZHD * GMF dry; leg1 +TGD +broadcast Klobuchar; "
                   "leg2 ionosphere-free C1C/C2W with OSB-corrected observables";
    j["unmodeled_by_design"] = "wet troposphere (ZWD), Shapiro, solid-earth/pole tide, "
                               "ocean loading, satellite+receiver antenna PCO/PCV, multipath";
    auto pack = [&](const Acc& a, const char* note) {
        const VarStats s3 = statsOf(a.d3), sh = statsOf(a.dh), su = statsOf(a.du);
        nlohmann::json v;
        v["note"] = note;
        v["n_solved"] = s3.n;
        v["n_failed"] = a.fail;
        v["mean_sats_used"] = s3.n ? static_cast<double>(a.n_used_sum) / s3.n : 0.0;
        v["err3d_m"] = { { "median", s3.median }, { "mean", s3.mean }, { "p90", s3.p90 }, { "max", s3.max } };
        v["horizontal_median_m"] = sh.median;
        v["up_abs_median_m"] = su.median;
        nlohmann::json hours = nlohmann::json::array();
        for (int h = 0; h < 24; ++h)
            if (a.hr_n[h])
                hours.push_back({ { "ut_hour", h },
                                  { "n", a.hr_n[h] },
                                  { "mean_3d_m", a.hr_sum[h] / a.hr_n[h] } });
        v["hourly"] = hours;
        return v;
    };
    j["variants"]["l1_broadcast"] = pack(a_l1, "leg 1: C1C + brdc eph/clk + TGD + Klobuchar (the bridge stack)");
    j["variants"]["l1_no_klobuchar"] = pack(a_nk, "leg 1 A/B: Klobuchar correction omitted");
    j["variants"]["l1_no_tgd"] = pack(a_nt, "leg 1 A/B: TGD not applied");
    j["variants"]["l1_precise_orbits"] = pack(a_pr, "leg 1 A/B: precise SP3/CLK instead of broadcast "
                                                    "(pair vs l1_no_tgd = pure broadcast orbit/clock error)");
    j["variants"]["if_precise"] = pack(a_if, "leg 2: IF(C1C,C2W) code + precise SP3/CLK, OSB-corrected");
    j["variants"]["if_no_osb"] = pack(a_no, "leg 2 A/B: raw observables, biases ignored");
    j["paired_mean_delta_3d_m"] = { { "klobuchar_off_minus_on", meanOf(delta_klob) },
                                    { "tgd_off_minus_on", meanOf(delta_tgd) },
                                    { "osb_off_minus_on", meanOf(delta_osb) },
                                    { "broadcast_minus_precise_orbits", meanOf(delta_brdc) } };
    {
        std::ofstream js(truth_dir / "truth_spp_summary.json");
        js << j.dump(2) << "\n";
    }

    auto report = [&](const char* label, const Acc& a) {
        const VarStats s3 = statsOf(a.d3), sh = statsOf(a.dh), su = statsOf(a.du);
        std::cout << "[" << tag << "] " << label << ": " << s3.n << "/" << epochs.size()
                  << " solved";
        if (a.fail) std::cout << " (" << a.fail << " failed)";
        if (s3.n)
            std::cout << "; 3D m median " << std::fixed << std::setprecision(3) << s3.median
                      << " mean " << s3.mean << " p90 " << s3.p90 << " max " << s3.max
                      << "; H med " << sh.median << " |U| med " << su.median;
        std::cout << "\n";
    };
    auto hourly = [&](const char* label, const Acc& a) {
        std::cout << "[" << tag << "]   " << label << " hourly mean 3D (UT h):";
        for (int h = 0; h < 24; ++h)
            if (a.hr_n[h])
                std::cout << " " << h << "=" << std::fixed << std::setprecision(2)
                          << a.hr_sum[h] / a.hr_n[h];
        std::cout << "\n";
    };
    if (n_truth_miss)
        std::cout << "[" << tag << "] WARN: " << n_truth_miss << " epochs had no truth row\n";
    report("leg1 brdc+TGD+Klob ", a_l1);
    hourly("leg1", a_l1);
    report("leg1 no-Klobuchar  ", a_nk);
    report("leg1 no-TGD        ", a_nt);
    report("leg1 precise-orbit ", a_pr);
    report("leg2 IF+OSB precise", a_if);
    hourly("leg2", a_if);
    report("leg2 IF no-OSB     ", a_no);
    std::cout << "[" << tag << "] paired mean d3D: Klobuchar saves "
              << std::setprecision(3) << meanOf(delta_klob) << " m; TGD saves "
              << meanOf(delta_tgd) << " m; OSB saves " << meanOf(delta_osb)
              << " m; broadcast orbit/clock costs " << meanOf(delta_brdc) << " m\n";
    std::cout << "[" << tag << "] wrote " << (truth_dir / "truth_spp.csv").string() << " and "
              << (truth_dir / "truth_spp_summary.json").string() << "\n";
    const bool complete = a_l1.d3.size() == epochs.size() && a_if.d3.size() == epochs.size();
    if (!complete)
        std::cerr << "[" << tag << "] WARN: not every epoch produced both canonical solutions\n";
    return 0;
}

// Synthesis (milestones M2/M3; subcommands synth-static and synth-kinematic, also run
// once per station by synth-network).  Synthesizes GPS and Galileo observations from
// real precise orbits for a truth trajectory (static point or kinematic vessel) and
// checks that each epoch inverts to its truth position, both noise-free (forward-model
// consistency within 1 mm) and with elevation-weighted noise.  The bare model (no
// ionosphere, troposphere or relativity) is recovered end-to-end by the processor with
// its corrections off.  A per-epoch visibility/DOP pre-flight catches unsolvable
// geometry.
int runSynth(const std::filesystem::path& outdir, bool kinematic, const sim::SimConfig& scfg) {
    const std::string tag = kinematic ? "synth-kinematic" : "synth-static";
    std::error_code ec;
    std::filesystem::create_directories(outdir, ec);

    const FixtureDate fd = makeFixtureDate(scfg.date.year, scfg.date.doy);
    const std::filesystem::path root = GNSS_PROJECT_ROOT;
    const std::filesystem::path base = root / "data" / fd.dir;
    const std::filesystem::path setup_obs = base / "obs" / "gcgo00usa" /
        ("GCGO00USA_R_" + fd.yyyydoy + "0000_01D_30S_MO.rnx");
    const std::filesystem::path sp3 = base / "products" / ("COD0MGXFIN_" + fd.yyyydoy + "0000_01D_05M_ORB.SP3");
    const std::filesystem::path clk = base / "products" / ("COD0MGXFIN_" + fd.yyyydoy + "0000_01D_30S_CLK.CLK");
    const std::filesystem::path nav = base / "products" / ("BRDC00IGS_R_" + fd.yyyydoy + "0000_01D_MN.rnx");
    // IGS20 ANTEX for the antenna PCO/PCV terms, loaded into the provider only when
    // the sim config requests an antenna term (corrections.sat_antenna for the
    // satellite part, corrections.rx_antenna_type for the receiver part).  It is the
    // file the processor auto-loads in internal mode (files.internal_data_root/static/
    // atx/igs20.atx), so the injected antenna terms cancel against the processor's own
    // model when the matching enable_satellite_pco/pcv + enable_receiver_pco/pcv are on.
    const std::filesystem::path atx = root / "data" / "static" / "atx" / "igs20.atx";
    // The day's Earth Rotation Parameters (polar motion xp/yp + UT1-UTC), loaded
    // into the provider whenever a correction that consumes ERP geometry is
    // requested.  Two consumers: (1) the pole tide reads xp/yp directly.  With no ERP,
    // GnssCorrections::computePoleTideCorrection returns 0, so the injected pole tide
    // would be 0 while the processor (which auto-globs the obs-day .erp in internal
    // mode) still subtracts a real mm-to-cm term, and the two would not cancel.
    // (2) The Sun/Moon frame that drives the solid-earth tide and the
    // satellite-antenna yaw attitude is evaluated from the same ERP.  It is the file
    // the processor loads (PathResolver globs the obs-day .erp), so with it loaded
    // here the pole tide cancels and the solid-tide and antenna Sun frame are
    // bit-for-bit what the processor computes.
    const std::filesystem::path erp = base / "products" / ("COD0MGXFIN_" + fd.yyyydoy + "0000_01D_12H_ERP.ERP");
    // The day's Bias-SINEX (.BIA) OSB product, loaded into the provider only when
    // corrections.signal_bias is requested.  It is the COD MGX file the processor
    // auto-globs in internal mode, so the observable-specific biases the sim adds to each
    // signal cancel against the processor's own getBiasCorrection once the run config
    // sets model_corrections.enable_code_phase_bias.
    const std::filesystem::path bia = base / "products" / ("COD0MGXFIN_" + fd.yyyydoy + "0000_01D_01D_OSB.BIA");
    for (const std::filesystem::path* p : { &setup_obs, &sp3, &clk }) {
        if (!std::filesystem::exists(*p)) {
            std::cerr << "[" << tag << "] FAIL: missing fixture " << p->string() << "\n";
            return 1;
        }
    }

    // Either antenna term needs the ATX.  The receiver term is requested by naming a
    // receiver antenna type, the string that is also declared in the RINEX
    // "ANT # / TYPE" header, so the file cannot declare one antenna while carrying
    // another's pattern.
    const bool want_rx_antenna = !scfg.corrections.rx_antenna_type.empty();
    const bool want_antenna = scfg.corrections.sat_antenna || want_rx_antenna;
    std::vector<std::filesystem::path> products = { sp3, clk };
    if (want_antenna) {
        if (!std::filesystem::exists(atx)) {
            std::cerr << "[" << tag << "] FAIL: an antenna correction (corrections.sat_antenna "
                         "or corrections.rx_antenna_type) is set but the ATX is missing: "
                      << atx.string() << "\n";
            return 1;
        }
        products.push_back(atx);
    }
    // Pole tide, solid-earth tide, and the satellite-antenna attitude frame all
    // consume ERP-derived geometry (see the erp declaration above), so the ERP is
    // loaded whenever any of them is requested.  The processor does the same: it
    // globs the obs-day .erp and applies it under the corresponding enable_* flags.
    const bool need_erp = scfg.corrections.pole_tide || scfg.corrections.solid_earth_tide ||
                          scfg.corrections.sat_antenna;
    if (need_erp) {
        if (!std::filesystem::exists(erp)) {
            std::cerr << "[" << tag << "] FAIL: an ERP-consuming correction (pole_tide/"
                         "solid_earth_tide/sat_antenna) is set but ERP missing: "
                      << erp.string() << "\n";
            return 1;
        }
        products.push_back(erp);
    }

    // Observable-specific signal biases need the day's .BIA.  It joins the provider's
    // product set so ProductParser::parseBia loads the OSBs into the shared cache (from
    // which geometry_'s GnssCorrections reads them).  A missing fixture is a hard
    // failure: injecting nothing while the run config still applies the processor's
    // own copy would leave a term that does not cancel.
    const bool want_signal_bias = scfg.corrections.signal_bias;
    if (want_signal_bias) {
        if (!std::filesystem::exists(bia)) {
            std::cerr << "[" << tag << "] FAIL: corrections.signal_bias is set but the .BIA "
                         "product is missing: " << bia.string() << "\n";
            return 1;
        }
        products.push_back(bia);
    }
    // Carrier-phase wind-up (M10): reuses the geometry pipeline's own
    // GnssCorrections::computePhaseWindUp (accumulated mode).  No extra product is
    // needed; the attitude comes from the Sun position the geometry already computes.
    const bool want_windup = scfg.corrections.phase_windup;

    sim::EphemerisProvider eph(setup_obs, products, outdir / "logs", nav,
                               scfg.corrections.rx_antenna_type);
    if (!eph.ok()) {
        std::cerr << "[" << tag << "] FAIL: EphemerisProvider setup\n";
        return 1;
    }
    // An ATX that yields no antennas is a hard failure rather than a run that
    // injects nothing.
    if (want_antenna && !eph.hasAntenna()) {
        std::cerr << "[" << tag << "] FAIL: an antenna correction is set but no antenna model "
                     "loaded from " << atx.string() << "\n";
        return 1;
    }
    // Likewise a hard failure when an ERP-consuming correction is on but no ERP entry
    // parsed.  Otherwise the pole tide would inject 0 (asymmetric with the processor)
    // and the solid-tide and antenna Sun frame would differ by the polar motion the
    // processor still applies.
    if (need_erp && eph.erpNodeCount() == 0) {
        std::cerr << "[" << tag << "] FAIL: ERP-consuming correction set but no ERP entries loaded from "
                  << erp.string() << "\n";
        return 1;
    }
    // A hard failure when signal biases were requested but the .BIA yielded no
    // entries.  Otherwise the sim would inject 0 while the processor's
    // enable_code_phase_bias still applies the real OSBs, which does not cancel.
    if (want_signal_bias && !eph.hasSignalBias()) {
        std::cerr << "[" << tag << "] FAIL: corrections.signal_bias set but no OSB entries loaded from "
                  << bia.string() << "\n";
        return 1;
    }
    std::cout << "[" << tag << "] provider ready: " << eph.orbitNodeCount() << " orbit nodes, "
              << eph.clockSampleCount() << " clock samples";
    if (need_erp) std::cout << " (+" << eph.erpNodeCount() << " ERP entries)";
    if (eph.hasAntenna()) {
        std::cout << " (+satellite antenna PCO/PCV";
        if (eph.hasReceiverAntenna())
            std::cout << ", +receiver PCO/PCV '" << scfg.corrections.rx_antenna_type << "'";
        std::cout << ")";
    }
    if (eph.hasSignalBias()) std::cout << " (+OSB signal biases)";
    std::cout << "\n";

    // ---- Per-station RNG salt (SPEC_synthetic_minimum_network.md §1.1 F1) --------
    // Zero (the single-station default) leaves every seed as configured, so every
    // existing scenario is byte-identical.  A network fans out N independent processes;
    // with shared seeds they would share one noise, multipath, slip, ZWD and
    // receiver-clock realization.  Between-station differences would then cancel the
    // noise exactly, the common receiver clock would be absorbed into the
    // satellite-clock datum, and the network would appear perfectly conditioned while
    // carrying no information, which no run gate can detect.  The salt is mixed
    // (splitmix64 finalizer) rather than only XORed, so two stations whose salts
    // differ in one low bit do not seed adjacent, correlated mt19937_64 states.
    const std::uint64_t seed_salt = scfg.station.seed_salt;
    auto saltSeed = [seed_salt](std::uint64_t base) -> std::uint64_t {
        if (seed_salt == 0) return base;                 // legacy: untouched
        std::uint64_t x = base ^ (seed_salt * 0x9E3779B97F4A7C15ull);
        x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ull;
        x = (x ^ (x >> 27)) * 0x94D049BB133111EBull;
        return x ^ (x >> 31);
    };

    // ---- Per-observable receiver biases (SPEC_synthetic_minimum_network.md §4.5) ----
    // Quasi-static hardware: satellite-independent, epoch-constant, per RINEX code.
    // Both maps are empty by default, so every existing scenario is byte-identical and
    // the lookup below returns a literal 0.0.
    std::map<std::string, double> rx_code_bias_map, rx_phase_bias_map;
    for (const auto& b : scfg.biases.rx_code_bias)  rx_code_bias_map[b.code]  = b.value_m;
    for (const auto& b : scfg.biases.rx_phase_bias) rx_phase_bias_map[b.code] = b.value_m;
    const bool want_rx_bias = !rx_code_bias_map.empty() || !rx_phase_bias_map.empty();
    auto rxBias = [](const std::map<std::string, double>& m, const std::string& code) -> double {
        const auto it = m.find(code);
        return it == m.end() ? 0.0 : it->second;
    };

    // ---- Truth trajectory (knobs from SimConfig; kinematic from the subcommand) ----
    sim::TrajectoryConfig tcfg;
    tcfg.kinematic = kinematic;
    tcfg.interval_s = scfg.trajectory.interval_s;
    tcfg.n_epochs = scfg.trajectory.n_epochs;
    tcfg.t0.tow = scfg.trajectory.start_tow_s;   // time-of-day only; the week/date stays fixture-bound
    tcfg.cruise_speed_mps = scfg.trajectory.cruise_speed_mps;
    tcfg.lat0_deg = scfg.trajectory.lat0_deg;
    tcfg.lon0_deg = scfg.trajectory.lon0_deg;
    tcfg.h0_m = scfg.trajectory.h0_m;
    tcfg.heading0_deg = scfg.trajectory.heading0_deg;
    tcfg.heading_amp_deg = scfg.trajectory.heading_amp_deg;
    tcfg.heading_period_s = scfg.trajectory.heading_period_s;
    tcfg.static_prefix_s = scfg.trajectory.static_prefix_s;
    tcfg.heave_amp_m = scfg.trajectory.heave_amp_m;
    tcfg.heave_period_s = scfg.trajectory.heave_period_s;
    tcfg.clock_bias0_s = scfg.trajectory.clock_bias0_s;
    tcfg.clock_drift_sps = scfg.trajectory.clock_drift_sps;
    tcfg.clock_rw_q_m2ps = scfg.trajectory.clock_rw_q_m2ps;
    tcfg.clock_seed = saltSeed(scfg.trajectory.clock_seed);
    tcfg.dest_lat_deg = scfg.trajectory.dest_lat_deg;
    tcfg.dest_lon_deg = scfg.trajectory.dest_lon_deg;
    tcfg.landfall_decel_frac = scfg.trajectory.landfall_decel_frac;
    tcfg.landfall_terminal_frac = scfg.trajectory.landfall_terminal_frac;
    tcfg.approach_weave_amp_m = scfg.trajectory.approach_weave_amp_m;
    tcfg.approach_weave_period_s = scfg.trajectory.approach_weave_period_s;
    tcfg.waypoints = scfg.trajectory.waypoints;
    sim::TrajectoryGenerator traj(tcfg);
    const auto& truth = traj.states();
    if (truth.empty()) {
        std::cerr << "[" << tag << "] FAIL: empty trajectory\n";
        return 1;
    }

    // ---- Ocean tidal loading (opt-in, EOT20 reuse) -------------------------
    // Frozen (default): coefficients are synthesized once at approx_pos, the point
    // the sim writes as the RINEX APPROX POSITION XYZ (the first truth epoch's ECEF,
    // or its quantized form, below), which is where Model::Geometry::loadData
    // synthesizes the processor's own set.  Both sides therefore evaluate the same
    // frozen 11-constituent model and the term cancels even though the vessel moves
    // ~100 km.
    //
    // Along-track (corrections.ocean_tide_along_track): the EOT20 field is sampled at
    // anchors along the trajectory, so the sim injects the loading at the vessel's
    // current position while the processor keeps the coefficients it froze at the
    // APPROX POSITION.  The asymmetry is intended: it is the one M7 term that does
    // not cancel, and truth_otl.csv records the part left over.
    //
    // In both modes a load failure is a hard failure rather than a run that injects
    // nothing: the processor's enable_ocean_tide_loading defaults to true, so a miss
    // here would leave a real cm-level term uncancelled.
    // A single expression gives the point at which the processor freezes its site
    // models: it feeds both the OTL reference below and hdr.approx_xyz further down,
    // so the sim's OTL reference cannot diverge from the APPROX POSITION the
    // processor reads out of the RINEX header.
    Eigen::Vector3d approx_pos = truth.front().ecef;
    // APPROX POSITION policy (SPEC R2).  "truth" (default) leaves it at the exact
    // trajectory start, which equals row 1 of truth_position.csv and so discloses
    // the answer in a student handout.  "quantized" rounds the disclosed last-known
    // position to a coarse lat/lon grid and the height to the nearest metre,
    // deterministically (no RNG draw, so no shared stream is perturbed).  This runs
    // before approx_pos feeds the frozen OTL reference and hdr.approx_xyz, so the sim
    // freezes ocean loading at the point the processor reads out of the RINEX header
    // (R2.2).  A ~110 m shift (the 0.001 deg default grid) moves the EOT20 field
    // sample negligibly (its grid is 0.125 deg).
    if (scfg.rinex.approx_position_mode == "quantized") {
        const double d2r = Constants::PI / 180.0;
        const Coordinates::Llh llh = Coordinates::ecefToLlh(approx_pos);
        const double q = scfg.rinex.approx_quantize_deg;
        const double lat_q = std::round((llh.lat_rad / d2r) / q) * q;   // lat_rad/d2r = degrees
        const double lon_q = std::round((llh.lon_rad / d2r) / q) * q;
        // Height rounds to the nearest metre rather than being set to 0: a floating
        // vessel sits at the sea surface, whose ellipsoidal height is the geoid
        // undulation (~+48 m near the hero LKP, EGM2008).  A zeroed header would
        // disclose a fix ~50 m below the water and defeat the Part-E height-systems
        // question.
        approx_pos = Coordinates::llhToEcef(lat_q * d2r, lon_q * d2r, std::round(llh.h_m));
    }

    if (scfg.corrections.ocean_tide) {
        const std::filesystem::path nc(scfg.corrections.eot20_nc_path);
        if (!std::filesystem::exists(nc)) {
            std::cerr << "[" << tag << "] FAIL: corrections.ocean_tide set but EOT20 grid missing: "
                      << nc.string() << "\n";
            return 1;
        }
        if (scfg.corrections.ocean_tide_along_track) {
            std::vector<sim::OtlField::TrackSample> otl_track;
            otl_track.reserve(truth.size());
            for (const auto& tp : truth) otl_track.push_back({ tp.time, tp.ecef });
            if (!eph.loadOceanTideAlongTrack(nc, otl_track,
                                             scfg.corrections.otl_anchor_spacing_km,
                                             approx_pos)) {
                std::cerr << "[" << tag << "] FAIL: could not build the along-track ocean-tide "
                             "field from " << nc.string() << "\n";
                return 1;
            }
            std::cout << "[" << tag << "] ocean tidal loading: ALONG-TRACK EOT20 field, "
                      << eph.oceanTideAnchorCount() << " anchors @ "
                      << scfg.corrections.otl_anchor_spacing_km
                      << " km (does NOT cancel; see truth_otl.csv)\n";
        } else if (!eph.loadOceanTide(nc, approx_pos)) {
            std::cerr << "[" << tag << "] FAIL: could not synthesize ocean-tide coefficients from "
                      << nc.string() << "\n";
            return 1;
        } else {
            std::cout << "[" << tag << "] ocean tidal loading: EOT20 coefficients at the RINEX "
                         "APPROX POSITION (radial component)\n";
        }
    }

    // ---- Product-day edge guard -------------------------------------------
    // The bundled precise products are single-day files (e.g. DOY 129 = 2024-05-08 =
    // GPS week 2313, tow [259200, 345600]): a 05M SP3 whose last node is the day end
    // and a 30S CLK whose last record is 30 s before it.  An arc that reaches an edge
    // cannot be centred in the Lagrange orbit window and (at the end) has no trailing
    // clock node, so orbits and clocks degrade into extrapolation there.
    //
    // An arc that leaves the product day is a hard failure, not a warning: the arc and
    // the fixture date disagree, and every epoch would be computed from the wrong day's
    // orbits without any error.  An arc within kMargin of an edge only warns, because
    // the cost there is accuracy.
    {
        const double kDayStart = fd.day_start_tow, kDayEnd = fd.day_end_tow, kMargin = 1800.0;
        const double t_first = truth.front().time.tow, t_last = truth.back().time.tow;
        if (t_first < kDayStart || t_last > kDayEnd) {
            std::cerr << "[" << tag << "] FAIL: arc tow [" << t_first << ", " << t_last
                      << "] runs OUTSIDE the " << fd.dir << " product day [" << kDayStart << ", "
                      << kDayEnd << "] — satellite orbits/clocks would be EXTRAPOLATED.\n"
                      << "        Either set trajectory.start_tow_s inside that span, or point"
                         " date.{year,doy} at the day the arc actually belongs to.\n";
            return 1;
        } else if (t_first < kDayStart + kMargin || t_last > kDayEnd - kMargin) {
            std::cerr << "[" << tag << "] WARN: arc tow [" << t_first << ", " << t_last
                      << "] sits within " << kMargin << " s of a product-day edge [" << kDayStart
                      << ", " << kDayEnd << "] — the Lagrange orbit window cannot be centred and"
                         " trailing clock nodes are missing (accuracy trap).\n";
        }
    }

    const double deg = Constants::PI / 180.0;
    const double c = Constants::SPEED_OF_LIGHT;
    const double mask_deg = scfg.geometry.elevation_mask_deg;

    std::mt19937_64 rng(saltSeed(scfg.noise.seed));
    std::normal_distribution<double> gauss(0.0, 1.0);
    const double sigma_zen = scfg.noise.code_sigma_zen_m, sigma_phase = scfg.noise.phase_sigma_zen_m;
    // --- Truth wet zenith delay (humid tropical environment) -------------------
    // Optionally time-varying: a diurnal sinusoid plus a random walk.  The walk is
    // drawn once per epoch from a dedicated stream (never `rng`/`aux_rng`), so tuning
    // the troposphere cannot shift the measurement-noise or multipath/slip draws, and
    // the storm-on/off control and byte-identity are preserved.  With the variation
    // knobs at their 0 defaults, zwd_ep equals scfg.atmosphere.zwd_m at every epoch.
    const auto& atm = scfg.atmosphere;
    const bool zwd_varies = (atm.zwd_diurnal_amp_m > 0.0) || (atm.zwd_rw_sigma_m > 0.0);
    // Salted per station.  Of all the streams, a shared copy of this one does the most
    // damage: it is drawn at most once per epoch, outside the satellite loop, so its
    // consumption does not depend on the sky and N stations would receive a
    // bit-identical ZWD series.  The ZWD, which the network has to separate from the
    // clock, would then be exactly degenerate with it, and no equality check would
    // detect that.
    std::mt19937_64 tropo_rng(saltSeed(atm.seed));
    // Its own distribution as well as its own engine (card SFR section 2.1).  A
    // normal_distribution caches the second value of each pair it draws, so the
    // `gauss` object shared with the white-noise stream handed each wet-delay draw a
    // value left over from that stream and the reverse: the wet-delay series moved
    // whenever the number of white-noise draws did (a satellite more in the sky), and
    // the white noise moved with the wet-delay knobs.  Recipes with a constant wet
    // delay draw nothing here and are unchanged.
    std::normal_distribution<double> tropo_gauss(0.0, 1.0);
    double zwd_rw_state = 0.0;          // accumulated random walk (m)
    double zwd_ep = atm.zwd_m;          // this epoch's truth ZWD (m)
    double zwd_sum = 0.0, zwd_min_seen = 1e9, zwd_max_seen = -1e9;
    const double zwd_true_m = atm.zwd_m;                   // arc mean when varying; the value when constant
    const double isb_true_m = scfg.biases.isb_gps_gal_m;   // GPS->Galileo inter-system bias (truth state, DESIGN §5.6)
    const double storm_scale = scfg.iono.storm_scale;      // multiplies storm amplitudes (0 = storm off, same RNG)

    // Multi-GNSS signal plan: GPS L1/L2 (C1C/L1C, C2W/L2W) + Galileo E1/E5a
    // (C1C/L1C, C5Q/L5Q), dual frequency.  Galileo E1 frequency == GPS L1 and
    // E5a == GPS L5 exactly (Constants.hpp:104,107), so the GPS<->Galileo difference
    // is injected as the receiver ISB on every Galileo code and phase row, not as a
    // frequency offset (GPS is the reference system, ISB = 0).
    struct Band {
        std::string code, phase;
        double wavelength;
        double mp_code_amp; ///< Marine multipath zenith amplitude on code (m), band-specific.
    };
    struct SysPlan {
        char sys;
        int max_prn;
        std::vector<Band> bands;
        double isb_m;
    };
    // Multipath amplitudes are milder than DESIGN §5.7 (semicodeless L2 worst);
    // per-band zenith amplitudes come from SimConfig (smartphone cases raise them).
    // The third band of each system is appended (never inserted) when armed, so the
    // first two bands keep their draw order within a satellite and a run with it off
    // is byte-identical.  Odijk 2016 §5.1: with fewer than three frequencies the
    // slant-ionosphere posture has no estimable satellite code bias, so the third band
    // is what makes the slant experiment possible (see SimConfig::Signals).
    std::vector<SysPlan> plans = {
        { 'G', 32,
          { { "C1C", "L1C", Constants::GPS_L1_WAVELENGTH, scfg.multipath.code_amp_gps_l1_m },
            { "C2W", "L2W", Constants::GPS_L2_WAVELENGTH, scfg.multipath.code_amp_gps_l2_m } },
          0.0 },
        { 'E', 36,
          { { "C1C", "L1C", Constants::GAL_E1_WAVELENGTH, scfg.multipath.code_amp_gal_e1_m },
            { "C5Q", "L5Q", Constants::GAL_E5A_WAVELENGTH, scfg.multipath.code_amp_gal_e5a_m } },
          isb_true_m },
    };
    if (scfg.signals.gps_l5)
        plans[0].bands.push_back({ "C5Q", "L5Q", Constants::GPS_L5_WAVELENGTH,
                                   scfg.multipath.code_amp_gps_l5_m });
    if (scfg.signals.galileo_e5b)
        plans[1].bands.push_back({ "C7Q", "L7Q", Constants::GAL_E5B_WAVELENGTH,
                                   scfg.multipath.code_amp_gal_e5b_m });
    if (scfg.signals.gps_l5 || scfg.signals.galileo_e5b)
        std::cout << "[" << tag << "] THIRD BAND armed: GPS "
                  << plans[0].bands.size() << " band(s), Galileo " << plans[1].bands.size()
                  << " band(s).  NOTE this fixture emits the third band on EVERY satellite of "
                     "the constellation, unlike the real GPS constellation where L5 is carried "
                     "by a subset — deliberate, so an estimability result cannot be confounded "
                     "with third-band sparsity.\n";

    // Synthetic satellite OSB (SimConfig::Signals::synth_osb_amp_m), added on top of
    // whatever the product .BIA supplies.  Deterministic in the PRN with no RNG, so the
    // four simulator streams are untouched.  It is needed because the shipped
    // COD0MGXFIN OSB product has no GPS band-5 rows, which would leave a third-band
    // code bias identically zero and the channel ungradeable.
    auto synthOsb = [&scfg](const std::string& sat, const std::string& code) -> double {
        if (scfg.signals.synth_osb_amp_m.empty()) return 0.0;
        const auto it = scfg.signals.synth_osb_amp_m.find(code);
        if (it == scfg.signals.synth_osb_amp_m.end()) return 0.0;
        const int prn = (sat.size() == 3) ? std::atoi(sat.substr(1).c_str()) : 0;
        return it->second * std::sin(2.0 * Constants::PI * ((prn * 7) % 23) / 23.0);
    };

    // ---- Synthetic satellite-clock error (SPEC_synthetic_minimum_network.md §3.1) ----
    // Pre-generated on the epoch grid for every PRN in the plan, before the epoch loop,
    // so no draw happens inside the satellite loop.  That loop runs before the
    // elevation mask, so a lazy draw would depend on the sky and break storm-on/off
    // byte-identity.  Inactive by default: metres() is then a literal 0.0 and the
    // emitted dataset is byte-identical.
    sim::SatClockError sat_clk_err;
    {
        std::vector<std::string> all_sats;
        for (const auto& plan : plans)
            for (int prn = 1; prn <= plan.max_prn; ++prn) {
                char id[4];
                std::snprintf(id, sizeof(id), "%c%02d", plan.sys, prn);
                all_sats.emplace_back(id);
            }
        std::string sce_err;
        if (!sat_clk_err.build(scfg.sat_clock, all_sats, tcfg.t0, tcfg.interval_s,
                               tcfg.n_epochs, &sce_err)) {
            std::cerr << "[" << tag << "] FAIL: " << sce_err << "\n";
            return 1;
        }
        if (sat_clk_err.active())
            std::cout << "[" << tag << "] satellite clock error ARMED: peak |err| = "
                      << std::fixed << std::setprecision(4) << sat_clk_err.peakAbsMetres()
                      << " m over " << sat_clk_err.satelliteCount() << " satellites"
                      << (scfg.sat_clock.steps.empty()
                              ? std::string()
                              : (" (+" + std::to_string(scfg.sat_clock.steps.size()) +
                                 " scripted step(s))"))
                      << "\n";
    }

    // Ionospheric disturbance: a storm-enhanced-density blob + travelling
    // disturbance over the middle half of the arc (bracketed by clean epochs),
    // injected as +I on code / -I on phase per band.  Dual-frequency IF cancels
    // it (1/f^2, same STEC) so PPP position stays flat; scintillation inflates
    // phase noise in the storm core.  Centred near the vessel's IPP cloud.
    sim::IonoConfig icfg;
    icfg.quiet_vtec_tecu = scfg.iono.quiet_vtec_tecu;
    icfg.sed_amp_tecu = scfg.iono.sed_amp_tecu;
    icfg.tid_amp_tecu = scfg.iono.tid_amp_tecu;
    icfg.scint_sigma_phi_max_rad = scfg.iono.scint_sigma_phi_max_rad;
    icfg.sed_sigma_lat_deg = scfg.iono.sed_sigma_lat_deg;
    icfg.sed_sigma_lon_deg = scfg.iono.sed_sigma_lon_deg;
    icfg.drift_lat_deg_per_min = scfg.iono.drift_lat_deg_per_min;
    icfg.drift_lon_deg_per_min = scfg.iono.drift_lon_deg_per_min;
    icfg.tid_wavelength_km = scfg.iono.tid_wavelength_km;
    icfg.tid_period_s = scfg.iono.tid_period_s;
    icfg.tid_period_start_s = scfg.iono.tid_period_start_s;
    icfg.tid_period_end_s = scfg.iono.tid_period_end_s;
    icfg.tid_pulse_count = scfg.iono.tid_pulse_count;
    icfg.tid_pulse_duration_s = scfg.iono.tid_pulse_duration_s;
    // Stagger the pulses across the explicitly-named TID satellites (one satellite
    // disturbed at a time).  0 when tid_sats is unused, so nothing staggers.
    icfg.tid_n_sats = static_cast<int>(scfg.iono.tid_sats.size());
    icfg.sed_elev_exp = scfg.iono.sed_elev_exp;
    icfg.tid_positive = scfg.iono.tid_positive;
    icfg.tid_azimuth_deg = scfg.iono.tid_azimuth_deg;
    icfg.shell_height_km = scfg.iono.shell_height_km;
    icfg.scint_elev_clamp_deg = scfg.iono.scint_elev_clamp_deg;
    const double arc_s = tcfg.interval_s * static_cast<double>(truth.size() - 1);
    icfg.window_start_tow_s = truth.front().time.tow + scfg.iono.window_start_frac * arc_s;
    icfg.window_duration_s = scfg.iono.window_duration_frac * arc_s;
    // Turbulence has its own window (see IonoConfig): real roughness is episodic, so
    // it can sit in a different part of the arc from the SED/TID storm.
    icfg.sed_bulk = scfg.iono.sed_bulk;
    icfg.turb_sats = scfg.iono.turb_sats;
    icfg.turb_sigma_tecu = scfg.iono.turb_sigma_tecu;
    icfg.turb_step_s = (scfg.iono.turb_step_s > 0.0) ? scfg.iono.turb_step_s
                                                    : tcfg.interval_s;
    icfg.turb_window_start_tow_s =
        truth.front().time.tow + scfg.iono.turb_window_start_frac * arc_s;
    icfg.turb_window_duration_s = (scfg.iono.turb_window_duration_frac > 0.0)
                                      ? scfg.iono.turb_window_duration_frac * arc_s
                                      : 0.0;
    // Storm-centre anchor.  An absolute anchor (iono.center_lat_deg / _lon_deg) takes
    // precedence when set; otherwise the legacy offset-from-this-run's-origin form is
    // used.  A network requires the absolute form: with the legacy form each of the N
    // station processes derives the blob centre from its own origin, so every station
    // sits at the centre and a "travelling" disturbance arrives at the same time and
    // with the same shape everywhere.  That contradicts the modeled physics and grades
    // as a satellite-side effect.  (Only the SED blob and the legacy continuous TID are
    // IPP-driven; explicit tid_pulses and turbulence bypass the spatial weight, so those
    // two overlays are spatially flat in either form.)
    const bool absolute_storm_anchor = std::isfinite(scfg.iono.center_lat_deg) &&
                                       std::isfinite(scfg.iono.center_lon_deg);
    icfg.center_lat0_deg = absolute_storm_anchor
                               ? scfg.iono.center_lat_deg
                               : tcfg.lat0_deg + scfg.iono.center_lat_offset_deg;
    icfg.center_lon0_deg = absolute_storm_anchor
                               ? scfg.iono.center_lon_deg
                               : tcfg.lon0_deg + scfg.iono.center_lon_offset_deg; // default +0.20 = a touch east
    // storm_scale multiplies the storm amplitudes only (1.0 = nominal storm;
    // 0.0 = no storm, with the same RNG draws and the same quiet Klobuchar
    // background, for a controlled storm-on/off comparison that isolates the
    // storm effect).  The quiet background is the reused broadcast Klobuchar
    // (per sat, below) and is not scaled here.
    icfg.sed_amp_tecu *= storm_scale;
    icfg.tid_amp_tecu *= storm_scale;
    icfg.scint_sigma_phi_max_rad *= storm_scale;
    const sim::IonoDisturbance iono(icfg);
    // Per-satellite storm participation.  Precedence: (1) explicit iono.tid_pulses ->
    // any satellite named by a pulse is disturbed; (2) iono.tid_sats -> those PRNs and
    // no others; (3) a deterministic (RNG-free) per-sat hash over storm_active_fraction.
    const std::vector<std::string>& tid_sats = scfg.iono.tid_sats;
    const std::vector<sim::SimConfig::Iono::TidPulse>& tid_pulses = scfg.iono.tid_pulses;
    const bool explicit_pulses = !tid_pulses.empty();
    auto satHasPulse = [&](const std::string& sat) -> bool {
        for (const auto& p : tid_pulses) if (p.sat == sat) return true;
        return false;
    };
    auto stormGain = [&](const std::string& sat) -> double {
        if (explicit_pulses) return satHasPulse(sat) ? 1.0 : 0.0;
        if (!tid_sats.empty())
            return (std::find(tid_sats.begin(), tid_sats.end(), sat) != tid_sats.end()) ? 1.0 : 0.0;
        const double frac = scfg.iono.storm_active_fraction;
        if (frac >= 1.0) return 1.0;
        if (frac <= 0.0) return 0.0;
        unsigned long long h = 1469598103934665603ull;        // FNV-1a over the sat id
        for (char c : sat) { h ^= static_cast<unsigned char>(c); h *= 1099511628211ull; }
        return (static_cast<double>(h % 1000000ull) / 1000000.0 < frac) ? 1.0 : 0.0;
    };
    // The one satellite whose TID bumps are filled with a rapid flicker (legacy mode only).
    auto tidPeriodOverride = [&](const std::string& sat) -> double {
        return (sat == scfg.iono.tid_fast_sat) ? scfg.iono.tid_fast_period_s : 0.0;
    };
    // This satellite's slot among the TID satellites (-1 if it is not one), which
    // pulse(s) it owns when the round-robin train is staggered (legacy mode only).
    auto tidSlot = [&](const std::string& sat) -> int {
        auto it = std::find(tid_sats.begin(), tid_sats.end(), sat);
        return (it == tid_sats.end()) ? -1 : static_cast<int>(it - tid_sats.begin());
    };
    // Sum of this satellite's explicit TID pulses (vertical TECU) at epoch tow.  Returns
    // -1 when no explicit pulses are configured, so evaluate() uses its internal model.
    // Each pulse is a positive-definite raised-cosine bump (width duration_s) optionally
    // filled with a positive-definite carrier of period_s; overlapping pulses on one
    // satellite sum.  storm_scale still scales the amplitude for the storm-on/off control.
    auto tidVtecExplicit = [&](const std::string& sat, double tow) -> double {
        if (!explicit_pulses) return -1.0;
        double v = 0.0;
        for (const auto& p : tid_pulses) {
            if (p.sat != sat) continue;
            const double start = p.center_tow_s - 0.5 * p.duration_s;
            const double phase = (tow - start) / p.duration_s;      // 0..1 across the bump
            if (phase <= 0.0 || phase >= 1.0) continue;
            const double bump = 0.5 * (1.0 - std::cos(2.0 * Constants::PI * phase));
            double carrier = 1.0;
            if (p.period_s > 0.0)
                carrier = 0.5 * (1.0 - std::cos(2.0 * Constants::PI * (tow - start) / p.period_s));
            v += p.amp_tecu * bump * carrier;
        }
        return v * storm_scale;
    };
    // --- Quiet-background source ---------------------------------------------
    // "klobuchar" (default) reuses the broadcast field, so a single-frequency student's
    // own Klobuchar cancels it exactly and only the storm remains.  "gim" samples the
    // real CODE IONEX for the fixture day: Klobuchar then removes only part of it, and
    // the (GIM - Klobuchar) remainder stays in the single-frequency data as a real,
    // non-removable model residual.  "analytic" uses quiet_vtec_tecu/sin(el).  In every
    // case the injected first-order term is one slant STEC scaled by 1/f^2 per band,
    // so the iono-free combination still cancels it.
    enum class Background { Klobuchar, Gim, Analytic };
    const Background background =
        scfg.iono.background_source == "gim"      ? Background::Gim :
        scfg.iono.background_source == "analytic" ? Background::Analytic : Background::Klobuchar;

    // Basename of the IONEX actually loaded, echoed into scenario_summary.json so
    // the handout packager does not have to guess it by glob (make_brief.py emits a
    // C4 warning and falls back to a *.INX glob when storm.gim_file is absent).
    std::string gim_file_name;

    if (background == Background::Gim) {
        std::filesystem::path gim_path = scfg.iono.gim_path;
        if (gim_path.empty())
            gim_path = base / "products" / ("COD0OPSFIN_" + fd.yyyydoy + "0000_01D_01H_GIM.INX");
        if (!eph.loadGim(gim_path)) {
            std::cerr << "[" << tag << "] FAIL: iono.background_source=\"gim\" but the IONEX "
                      << gim_path.string() << " could not be loaded.\n";
            return 1;
        }
        gim_file_name = gim_path.filename().string();
        // The GIM background does not cancel against Klobuchar; that residual is the
        // single-frequency content of the dataset, so the broadcast field must still be
        // present to subtract.  Without it the Part-B exercise has no content and no
        // error says so.
        if (!eph.hasKlobuchar()) {
            std::cerr << "[" << tag << "] FAIL: iono.background_source=\"gim\" requires a broadcast "
                         "Klobuchar field (GPSA/GPSB) to form the single-frequency residual, but "
                      << nav.string() << " has none.\n";
            return 1;
        }
        // Epochs outside the map span are clamped to the endpoint map and still
        // longitude-rotated, which gives wrong truth with no error.  The run is refused
        // rather than extrapolated.
        GnssTime g0{}, g1{};
        if (!eph.gimEpochSpan(g0, g1)) {
            std::cerr << "[" << tag << "] FAIL: loaded IONEX carries no maps.\n";
            return 1;
        }
        // Compare in continuous GPS seconds, not (week == week && tow in range): a day
        // that ends the GPS week (DOY 132 = week 2313 day 6) puts its final map at
        // midnight of the next day = week+1, tow 18 (IONEX epochs are UTC-labelled;
        // +18 s leap on conversion), and a week-equality check would wrongly reject an
        // arc that lies strictly inside the span.
        const auto cont = [](const GnssTime& t) { return t.week * 604800.0 + t.tow; };
        const double A0 = cont(truth.front().time), A1 = cont(truth.back().time);
        if (A0 < cont(g0) || A1 > cont(g1)) {
            std::cerr << "[" << tag << "] FAIL: arc [w" << truth.front().time.week << "/"
                      << truth.front().time.tow << ", w" << truth.back().time.week << "/"
                      << truth.back().time.tow << "] is not strictly inside the IONEX map span [w"
                      << g0.week << "/" << g0.tow << ", w" << g1.week << "/" << g1.tow
                      << "] — the GIM would be clamped and longitude-rotated.\n";
            return 1;
        }
        std::cout << "[" << tag << "] quiet background: CODE GIM " << gim_path.filename().string()
                  << " (maps tow " << g0.tow << " .. " << g1.tow << "); Klobuchar retained for the "
                     "single-frequency residual\n";
    } else {
        std::cout << "[" << tag << "] quiet background: "
                  << (background == Background::Analytic
                          ? "analytic quiet_vtec_tecu/sin(el)"
                          : (eph.hasKlobuchar() ? "broadcast Klobuchar GPSA/GPSB (reused)"
                                                : "analytic fallback (no broadcast Klobuchar)"))
                  << "\n";
    }
    // Counts rays where the GIM query failed and Klobuchar stood in.  A run that mixes
    // two backgrounds is not a GIM dataset, so the run fails at the end if this is > 0.
    long long gim_fallback_count = 0;

    // --- Marine multipath (Gauss-Markov) + cycle slips (mixed flagging). -------
    // Drawn from a separate RNG stream so tuning these does not move the white
    // measurement noise (and vice versa), keeping the storm-on/off control exact.
    std::mt19937_64 aux_rng(saltSeed(scfg.multipath.seed));
    std::normal_distribution<double> ngm(0.0, 1.0);
    std::uniform_real_distribution<double> uni(0.0, 1.0);
    const double mp_tau_s = scfg.multipath.tau_s;                 // multipath correlation time
    const double mp_phi = std::exp(-tcfg.interval_s / mp_tau_s);  // ~0.905 at 30 s
    const double mp_phase_amp = scfg.multipath.phase_amp_zen_m;   // zenith phase multipath (m)

    // --- Coloured phase error (card SFR): per-line-of-sight Gauss-Markov. -------
    // Band-common in metres, phase only.  Its own station-salted stream (the tropo_rng
    // precedent), so it cannot move any other draw; with sigma_zen_m == 0 (the default)
    // the generator is inactive, draws nothing, and every sample below is a literal 0.0.
    sim::PhaseColourGenerator phase_colour(scfg.phase_colour.sigma_zen_m, scfg.phase_colour.tau_s,
                                           scfg.phaseColourElevFloorDeg(),
                                           saltSeed(scfg.phase_colour.seed));
    const bool want_phase_colour = phase_colour.active();
    double max_abs_phase_colour_m = 0.0;   // peak |e| over the arc (m)
    double sumsq_phase_colour_m2 = 0.0;    // for the arc rms of e (m^2)
    long long n_phase_colour = 0;          // (epoch, satellite) samples injected
    const double slip_base = scfg.slips.rate_base;     // per-epoch slip-rate base; scaled by 1/sin^2(el)
    const int slip_min_gap = scfg.slips.min_gap_epochs;// min epochs between slips on one (sat,band)
    auto slipProb = [&](double el_deg) -> double {
        if (el_deg >= scfg.slips.max_elev_deg) return 0.0; // slips concentrate at low elevation
        const double s = std::sin(el_deg * deg);
        return slip_base / (s * s);
    };
    // Optional slip-rate taper toward the arc end: full rate up to taper_start_frac
    // of the arc, then a cosine ramp to 0 by the last epoch (so the vessel's final
    // approach carries only a few slips).  1.0 (default) => no taper.
    const double slip_taper_start = scfg.slips.taper_start_frac;
    const long n_ep_total = static_cast<long>(truth.size());
    auto slipTaper = [&](int ep) -> double {
        if (slip_taper_start >= 1.0 || n_ep_total < 2) return 1.0;
        const double frac = static_cast<double>(ep) / static_cast<double>(n_ep_total - 1);
        if (frac <= slip_taper_start) return 1.0;
        const double u = (frac - slip_taper_start) / (1.0 - slip_taper_start); // 0 -> 1
        return 0.5 * (1.0 + std::cos(Constants::PI * u));                       // 1 -> 0
    };
    auto drawSlip = [&](double el_deg) -> int {
        // Slip magnitude scales with the local noise floor (~1/sin el): the code/
        // phase noise and multipath grow at low elevation, so a slip there must be large
        // to stay clear of that noise (otherwise it is unrecoverable and biases the
        // float ambiguity), and may be smaller (~10 cyc) at high elevation where the
        // noise is low.  This keeps the slip detectable at every elevation.
        const double s = std::sin(std::max(el_deg, scfg.noise.elev_factor_floor_deg) * deg);
        const double span = scfg.slips.max_cycles_zenith - scfg.slips.min_cycles_zenith;
        const double base = scfg.slips.min_cycles_zenith + uni(aux_rng) * span; // nominal cycles at zenith
        int mag = static_cast<int>(std::lround(base / s));
        if (mag < scfg.slips.min_magnitude) mag = scfg.slips.min_magnitude;
        return (uni(aux_rng) < 0.5) ? -mag : mag;
    };

    // Header APPROX POSITION = the (possibly quantized) reference, plus the deliberate
    // ENU error from rinex.approx_offset_enu_m.  The offset is applied here and nowhere
    // else: the frozen ocean-loading reference above still uses the un-offset
    // `approx_pos`, so the injected physics is bit-for-bit unchanged and only the
    // disclosed a priori moves.  A header a priori is not a fix; a large offset makes a
    // solver iterate its least squares from the seed instead of starting at the answer.
    Eigen::Vector3d hdr_approx = approx_pos;
    {
        const auto& off = scfg.rinex.approx_offset_enu_m;
        if (off[0] != 0.0 || off[1] != 0.0 || off[2] != 0.0) {
            const Coordinates::Llh a = Coordinates::ecefToLlh(approx_pos);
            hdr_approx += Coordinates::enuToEcefMatrix(a.lat_rad, a.lon_rad)
                          * Eigen::Vector3d(off[0], off[1], off[2]);
            const double dh = std::hypot(off[0], off[1]);
            std::cout << "[" << tag << "] APPROX POSITION carries a deliberate a-priori error: "
                      << "E " << off[0] << " N " << off[1] << " U " << off[2] << " m ("
                      << dh / 1000.0 << " km horizontal) - the header is a coarse seed, not a fix\n";
        }
    }

    sim::ObsHeaderSpec hdr;
    hdr.run_date = "20260624 000000 UTC";
    hdr.marker_type = kinematic ? "KINEMATIC" : "GEODETIC";
    hdr.approx_xyz = { hdr_approx.x(), hdr_approx.y(), hdr_approx.z() };
    hdr.interval_s = tcfg.interval_s;
    hdr.time_of_first_obs = tcfg.t0;
    // Declare the antenna whose pattern was injected.  This is the string handed to
    // Antenna's receiver lookup, so the header and the injection cannot diverge.  Padded
    // to the RINEX field width (16-char model + 4-char radome) so the radome lands in
    // cols 37-40 as the IGS convention requires.  With no receiver antenna modeled, the
    // writer's uncalibrated placeholder stays on purpose: an external service that finds
    // no calibration applies no antenna correction (CSRS-PPP "Case 4", NULL reference
    // PCO), which matches a dataset carrying no receiver antenna term.
    if (want_rx_antenna) {
        hdr.ant_type = scfg.corrections.rx_antenna_type;
        hdr.ant_type.resize(20, ' ');
    }
    // Station identity.  An empty name keeps the writer's "SARV" / "SARV00FJI" defaults,
    // so every existing scenario emits a byte-identical header.  A network has to set
    // it: otherwise every station's RINEX identifies itself as SARV, and anything that
    // takes station identity from the header merges the whole network into one station.
    if (!scfg.station.name.empty()) {
        hdr.marker_name = scfg.station.name;
        hdr.marker_number = scfg.station.name;
        hdr.marker_type = kinematic ? "KINEMATIC" : "GEODETIC";
    }
    // Derived from the band plan rather than restated: with a hand-written literal here,
    // a third band could be synthesized into rows the RINEX header never declares, and
    // since the writer emits per declared type, the band would be dropped without error.
    hdr.obs_types.clear();
    for (const auto& plan : plans) {
        std::vector<std::string> codes;
        codes.reserve(plan.bands.size() * 2);
        for (const auto& b : plan.bands) { codes.push_back(b.code); codes.push_back(b.phase); }
        hdr.obs_types.emplace_back(plan.sys, std::move(codes));
    }
    // RINEX header realism / leak knobs (SPEC R2).  Each override is applied only when the
    // config supplies it, so an absent/default rinex section keeps every writer default
    // and the file is byte-identical (R2.1).
    if (!scfg.rinex.pgm.empty())      hdr.pgm      = scfg.rinex.pgm;
    if (!scfg.rinex.run_by.empty())   hdr.run_by   = scfg.rinex.run_by;
    if (!scfg.rinex.observer.empty()) hdr.observer = scfg.rinex.observer;
    if (!scfg.rinex.agency.empty())   hdr.agency   = scfg.rinex.agency;
    if (scfg.rinex.file_date_mode == "collection_end") {
        // DATE field = UTC datetime of the last epoch.  GPS -> UTC by subtracting the
        // leap offset; the calendar conversion reuses the TimeUtils path the writer
        // uses for TIME OF FIRST OBS, then snaps seconds-of-day to the ms grid so the
        // lossy JD conversion cannot print a spurious sub-second tag.
        const GnssTime last_utc = addSeconds(truth.back().time,
                                             -static_cast<double>(scfg.rinex.gps_utc_leap_s));
        int Y, Mo, D, H, Mi; double Sc;
        TimeUtils::gpsTimeToGpsScaleCalendar(last_utc, Y, Mo, D, H, Mi, Sc);
        long ms = std::lround((H * 3600.0 + Mi * 60.0 + Sc) * 1000.0);
        if (ms < 0) ms = 0;
        if (ms >= 86400000L) ms = 86400000L - 1;   // defensive: inputs never reach a day rollover here
        const int Hh = static_cast<int>(ms / 3600000L); ms %= 3600000L;
        const int Mm = static_cast<int>(ms / 60000L);   ms %= 60000L;
        const int Ss = static_cast<int>(ms / 1000L);
        char buf[40];
        std::snprintf(buf, sizeof(buf), "%04d%02d%02d %02d%02d%02d UTC", Y, Mo, D, Hh, Mm, Ss);
        hdr.run_date = buf;
    }

    // The emitted filename carries the date of the fixture products it was built
    // against; the processor resolves its product day from this name.  The prefix is
    // the station marker when one is configured.  A network needs distinct filenames:
    // every truth file written below has a fixed name in `outdir`, so two stations
    // sharing an output directory would leave one RINEX and one truth set behind while
    // the run passed and printed a normal summary.
    const std::string file_prefix = scfg.station.name.empty() ? std::string("SARV")
                                                              : scfg.station.name;
    const std::filesystem::path obs_path =
        outdir / (file_prefix + "_R_" + fd.yyyydoy + "0000_01D_30S_MO.rnx");
    sim::RinexObsWriter writer;
    std::string werr;
    if (!writer.open(obs_path, hdr, &werr)) {
        std::cerr << "[" << tag << "] FAIL: writer.open: " << werr << "\n";
        return 1;
    }

    std::ofstream truth_csv(outdir / "truth_position.csv");
    truth_csv << "week,tow_s,x_m,y_m,z_m,lat_deg,lon_deg,h_m,clock_bias_s,heading_deg,speed_mps\n" << std::fixed;

    std::ofstream clk_csv(outdir / "truth_clock.csv");
    clk_csv << "week,tow_s,clock_bias_s,clock_bias_m,isb_gps_gal_m\n" << std::fixed;

    // Per-epoch truth troposphere.  Always emitted (even for a constant ZWD) so the
    // Verifier has one contract to read; truth_scalars.csv is a key,value file and
    // cannot carry a series.  Mirrors truth_clock.csv, which already publishes a
    // per-epoch truth series for a filter state.
    std::ofstream tro_csv(outdir / "truth_tropo.csv");
    tro_csv << "week,tow_s,zwd_m,zhd_m\n" << std::fixed;

    // Per-epoch ocean-loading truth.  Emitted only when OTL is injected: a run without
    // it has no ocean-loading truth to publish, and an all-zero file would read as
    // "modeled perfectly" rather than "not simulated".  Like truth_tropo.csv (and unlike
    // truth_position.csv, which is gated on a solvable epoch) it has one row per
    // trajectory epoch, so join it on tow rather than by row index.  inj_up is the
    // loading the vessel experienced; mod_up is what the processor models from its
    // APPROX-POSITION coefficients; d_up = inj - mod is the unmodeled remainder,
    // identically zero unless the field varies along track.  Displacements are purely
    // radial (EOT20 carries the height load only), so the local up component is the
    // whole signal.  A satellite at elevation el sees -d_up*sin(el) of range error,
    // which is the range partial with respect to the up coordinate, so d_up lands on the
    // height estimate, not on the clock or ZWD, and leaves no post-fit residual.
    // dist_from_approx_km is the straight-line distance from the APPROX POSITION, not
    // the path length; on a track that doubles back it is much smaller than the
    // distance sailed, and it is the quantity that predicts d_up.
    //
    // Caveat on mod_up: it is what the processor is expected to model (the sim's own
    // frozen coefficients), not an observation of what it loaded.  The two sides fail
    // differently: the sim fails hard on a missing or unreadable EOT20 grid, while
    // Model::Geometry::loadData only logs and installs nothing (and both the geometry
    // and ocean_tide loggers default to off).  A bad data_download.nc_path, a zero
    // APPROX POSITION, or a station-info/BLQ source taking precedence would then leave
    // the processor modeling zero with no error, and d_up would understate the real
    // unmodeled term by the whole ~50 mm frozen part.  Confirm with
    // logging.enable_ocean_tide before trusting a residual attributed to along-track
    // loading.
    const bool write_otl_truth = scfg.corrections.ocean_tide;
    std::ofstream otl_csv;
    if (write_otl_truth) {
        otl_csv.open(outdir / "truth_otl.csv");
        otl_csv << "week,tow_s,inj_up_m,mod_up_m,d_up_m,dist_from_approx_km\n" << std::fixed;
    }
    double otl_peak_abs_dup = 0.0, otl_peak_abs_inj = 0.0;

    std::ofstream vis_csv(outdir / "truth_visibility.csv");
    vis_csv << "week,tow_s,n_sats,n_gps,n_gal,gdop,pdop,hdop,vdop,sat_list\n" << std::fixed << std::setprecision(3);

    // Integer carrier ambiguities are arc-constant per (sat, band) and recorded
    // once, the first epoch each (sat, signal) is seen.
    std::ofstream amb_csv(outdir / "truth_ambiguities.csv");
    // setprecision(12), not 6.  The IF ambiguity is g*lam1*N1 + h*lam2*N2 with N ~ 1e6
    // under the Legacy regime, so a wavelength rounded to 6 decimals (0.190294 vs
    // 0.190293673) reconstructs B_IF 0.85 m wrong.  The integers are randomized, so this
    // file is the only source for them and has to carry full precision.  The Field
    // regime's N ~ 1e2 reduces that sensitivity by ~4 decades, but the precision stays:
    // graders read the file under both regimes, and 12 decimals costs nothing.
    // Consumers should still prefer lambda = c/f.
    amb_csv << "satellite,signal,N_int,wavelength_m\n" << std::fixed << std::setprecision(12);
    std::set<std::string> amb_seen;
    // The draw is in include/sim/AmbiguityDraw.hpp.  It is a fixture property that
    // decides what an estimator experiment can conclude (legacy N in [1e5,1e6) is ~4
    // decades above the measured IGS spread and defeats a zero-target cold start), so
    // it is a unit-testable free function rather than a local lambda.  That header also
    // documents the two structural properties the draw preserves in either regime: the
    // station salt (without it every between-station DD is exactly zero and AR reports
    // a 100 % fix rate while measuring nothing) and per-band independence (so the
    // wide-lane is not one constant across the network).
    const sim::AmbiguityDrawConfig amb_cfg = scfg.ambiguity;
    auto ambN = [amb_cfg, seed_salt](const std::string& sat, int prn, int band_idx) -> int {
        return sim::drawAmbiguity(amb_cfg, sat, prn, band_idx, seed_salt);
    };

    // Observable-specific signal biases (OSB), injected only when corrections.signal_bias
    // is set.  Each per-(satellite, observable) value is arc-constant (the product covers
    // one UTC day), so like the ambiguities it is recorded once, the first epoch each
    // (sat, signal) is seen.  osb_by_key retains the values so the ionosphere-free code
    // bias an OSB-ignorant PPP would suffer can be reported per GPS satellite in the
    // scenario summary (the pedagogical payload); max_abs_osb_m tracks the largest single
    // injected observable bias.
    const bool write_bias_truth = want_signal_bias;
    std::ofstream bias_csv;
    if (write_bias_truth) {
        bias_csv.open(outdir / "truth_bias.csv");
        // osb_m is the total injected bias (what the estimator must reproduce);
        // product_m / synth_m / source split it for provenance.  Column names are the
        // join key downstream (scripts/simnet_grade.py reads by DictReader), so
        // appending columns is safe; the two leading columns are unchanged.
        bias_csv << "satellite,signal,osb_ns,osb_m,product_m,synth_m,source\n"
                 << std::fixed << std::setprecision(6);
    }
    std::set<std::string> bias_seen;
    std::map<std::string, double> osb_by_key; // "G01|C1C" -> OSB (m)
    double max_abs_osb_m = 0.0;

    // Carrier-phase wind-up truth (M10), injected only when corrections.phase_windup is
    // set.  The angle is band-common in cycles, so one row per (epoch, satellite) with
    // the accumulated cycle count fully determines every band's injected metres
    // (multiply by the band wavelength).  windup_cycles is signed and continuous within
    // a tracked arc; a satellite that sets and re-rises (>300 s gap) restarts its
    // accumulator, as the processor's unwrap does.
    std::ofstream windup_csv;
    if (want_windup) {
        windup_csv.open(outdir / "truth_windup.csv");
        windup_csv << "week,tow_s,satellite,windup_cycles\n" << std::fixed << std::setprecision(6);
    }
    double max_abs_windup_cycles = 0.0;

    // Coloured phase error truth (card SFR), written only when the block is active
    // (an absent file states that the channel was not simulated).  One row per (epoch,
    // satellite): e_m is the band-common metres added to every band's carrier phase,
    // u the unit-variance Gauss-Markov value it was scaled from (e_m = sigma_zen_m *
    // 1/sin(max(el, floor)) * u), so the process can be graded without the elevation.
    std::ofstream phase_colour_csv;
    if (want_phase_colour) {
        phase_colour_csv.open(outdir / "truth_phase_colour.csv");
        phase_colour_csv << "week,tow_s,satellite,e_m,u\n" << std::fixed;
    }

    // Per-observable receiver bias truth.  Written only when at least one bias is
    // configured, like truth_bias.csv / truth_otl.csv: an empty file cannot be told
    // apart from "the channel was not simulated", while an absent file states it.
    // These values are constants, so the file is a small key table rather than a
    // per-epoch series.
    if (want_rx_bias) {
        std::ofstream rxb_csv(outdir / "truth_rx_bias.csv");
        rxb_csv << "station,kind,code,value_m\n" << std::fixed << std::setprecision(6);
        const std::string sta = scfg.station.name.empty() ? std::string("SARV")
                                                          : scfg.station.name;
        for (const auto& [code, v] : rx_code_bias_map)
            rxb_csv << sta << ",code," << code << "," << v << "\n";
        for (const auto& [code, v] : rx_phase_bias_map)
            rxb_csv << sta << ",phase," << code << "," << v << "\n";
    }

    // Per-(epoch,sat) ionosphere truth: STEC decomposition + per-L1 delay.
    std::ofstream atmo_csv(outdir / "truth_atmo.csv");
    // The first 15 columns (through in_storm_window) are fixed; existing tooling
    // (pyviz/conftest.py, exam_iono_truth.py) reads that header.  New columns are
    // appended:
    //   obliquity          the storm field's vertical->slant factor, published so a
    //                      double-mapped GIM background can be audited from the truth
    //                      file (no other gate sees that error; it cancels in iono-free).
    //   I_klob_L1_m        what a single-frequency student's broadcast Klobuchar removes.
    //   sf_iono_resid_m    what they cannot remove: injected quiet L1 delay - Klobuchar.
    //                      Exactly 0 for background_source="klobuchar"/"analytic".
    //   gim_*              the GIM's own geocentric IPP on ITS shell (450 km for CODE)
    //                      and the IONEX RMS as a slant-TEC sigma; 0 when no GIM.
    atmo_csv << "week,tow_s,satellite,elev_deg,az_deg,ipp_lat_deg,ipp_lon_deg,"
                "stec_quiet_tecu,dstec_sed_tecu,dstec_tid_tecu,dstec_turb_tecu,stec_total_tecu,"
                "I_L1_m,dI_L1_m,in_storm_window,"
                "obliquity,I_klob_L1_m,sf_iono_resid_m,"
                "gim_ipp_lat_deg,gim_ipp_lon_deg,gim_stec_std_tecu\n"
             << std::fixed << std::setprecision(4);

    double worst_clean = 0.0, worst_noisy = 0.0, worst_isb_clean = 0.0, worst_if_resid = 0.0;
    int total_obs = 0, epochs_written = 0;
    int min_sats = 1000, min_gps = 1000, min_gal = 1000;
    double worst_pdop = 0.0;
    // Single-frequency (GPS L1 + Klobuchar) SPP error, storm window vs quiet.
    double sf_quiet_max = 0.0, sf_storm_max = 0.0, sf_quiet_sum = 0.0, sf_storm_sum = 0.0;
    int sf_quiet_n = 0, sf_storm_n = 0;

    // Cycle-slip + multipath state, persisted per (sat, band) across epochs.
    std::map<std::string, double> mp_gm;        // unit-variance Gauss-Markov state per "sat/code"
    std::map<std::string, int> slip_accum;      // cumulative integer cycle slip per "sat/code"
    std::map<std::string, int> slip_last_epoch; // last epoch a slip hit a "sat/code"

    // Scripted slips (SPEC_scripted_slips.md).  When the list is empty (absent/`[]`),
    // the schedule never fires and every scripted-conditional output below keeps the
    // format of a run without scripted slips, so the output is byte-identical (R2).
    const bool has_scripted = !scfg.slips.scripted.empty();
    sim::ScriptedSlipSchedule sched(scfg.slips.scripted);
    int n_slips_scripted = 0, n_slips_scripted_flagged = 0;
    struct ScriptedApplied {
        std::string sat, signal;
        int epoch; double tow; int delta_cycles; bool lli; double elev_deg;
    };
    std::vector<ScriptedApplied> scripted_applied; // one entry per applied repetition

    std::ofstream slip_csv(outdir / "truth_slips.csv");
    // The trailing ",source" column (random|scripted) is added only when scripted
    // events exist; otherwise the file has the 7-column layout.
    slip_csv << "week,tow_s,satellite,signal,delta_cycles,flagged,elev_deg"
             << (has_scripted ? ",source" : "") << "\n" << std::fixed;
    // One row writer serves both the random and scripted paths, so their formatting
    // cannot diverge (the shared stream interleaves setprecision(3)/(2)).
    auto writeSlipRow = [&](const GnssTime& tt, const std::string& s, const std::string& sig,
                            int delta, bool flagged, double el, bool scripted) {
        slip_csv << tt.week << "," << std::setprecision(3) << tt.tow << "," << s << ","
                 << sig << "," << delta << "," << (flagged ? 1 : 0) << ","
                 << std::setprecision(2) << el;
        if (has_scripted) slip_csv << "," << (scripted ? "scripted" : "random");
        slip_csv << "\n";
    };
    int n_slips = 0, n_slips_flagged = 0, epoch_counter = 0;
    double max_mp_code = 0.0;
    // Storm headline: peak disturbance-only L1 slant delay + where it occurs.
    double peak_dI_L1 = 0.0, peak_dI_tow = 0.0;
    std::string peak_dI_sat;
    const double f_L1 = Constants::GPS_L1_FREQ;

    for (const auto& ts : truth) {
        const GnssTime t = ts.time;
        const Eigen::Vector3d r_rx = ts.ecef;
        const double cdt_r_m = c * ts.clock_bias_s;
        ++epoch_counter;

        // ---- This epoch's truth wet delay ------------------------------------
        // Computed once per epoch, outside the satellite loop: the random term must
        // advance per epoch, not per visible satellite (otherwise it would depend on
        // the sky and the storm-on/off control would diverge).
        if (zwd_varies) {
            // Mean-reverting Gauss-Markov term (same form as the marine multipath).  A
            // pure random walk is unbounded over a 12 h arc and moves the arc mean off
            // the configured value (3 mm/epoch moved the mean 0.15 -> 0.25 m).  With
            // phi = exp(-dt/tau) the process is stationary with 1-sigma =
            // zwd_rw_sigma_m, so the mean stays at the configured value.
            if (atm.zwd_rw_sigma_m > 0.0) {
                const double phi = std::exp(-scfg.trajectory.interval_s / atm.zwd_rw_tau_s);
                zwd_rw_state = phi * zwd_rw_state
                             + atm.zwd_rw_sigma_m * std::sqrt(std::max(0.0, 1.0 - phi * phi))
                                                  * tropo_gauss(tropo_rng);
            }
            const double dt = (t.week - truth.front().time.week) * 604800.0
                            + (t.tow - truth.front().time.tow) + atm.zwd_diurnal_phase_s;
            const double diurnal = atm.zwd_diurnal_amp_m
                                 * std::sin(2.0 * Constants::PI * dt / atm.zwd_diurnal_period_s);
            zwd_ep = std::max(atm.zwd_min_m, atm.zwd_m + diurnal + zwd_rw_state);
        }
        zwd_sum += zwd_ep;
        zwd_min_seen = std::min(zwd_min_seen, zwd_ep);
        zwd_max_seen = std::max(zwd_max_seen, zwd_ep);
        // ZHD depends only on (position, time), not on the satellite, so it is computed
        // once here, outside the satellite loop; this is the value truth_tropo.csv
        // records.
        const double zhd_ep = eph.tropoZhd(r_rx, t);
        tro_csv << t.week << "," << std::setprecision(3) << t.tow << ","
                << std::setprecision(6) << zwd_ep << "," << zhd_ep << "\n";

        // ---- This epoch's ocean-loading truth --------------------------------
        // Computed outside the satellite loop like the ZHD: the site displacement depends
        // on (position, time) only.  Projected onto the local up so the file records
        // the physical radial loading rather than an ECEF triple.
        if (write_otl_truth) {
            const Eigen::Vector3d inj = eph.oceanTideDisplacementEcef(r_rx, t);
            const Eigen::Vector3d mod = eph.oceanTideModeledEcef(r_rx, t);
            const double clat = std::cos(ts.lat_deg * Constants::PI / 180.0);
            const double slat = std::sin(ts.lat_deg * Constants::PI / 180.0);
            const double clon = std::cos(ts.lon_deg * Constants::PI / 180.0);
            const double slon = std::sin(ts.lon_deg * Constants::PI / 180.0);
            const Eigen::Vector3d up(clat * clon, clat * slon, slat);
            const double inj_up = inj.dot(up);
            const double mod_up = mod.dot(up);
            otl_peak_abs_dup = std::max(otl_peak_abs_dup, std::abs(inj_up - mod_up));
            otl_peak_abs_inj = std::max(otl_peak_abs_inj, std::abs(inj_up));
            otl_csv << t.week << "," << std::setprecision(3) << t.tow << ","
                    << std::setprecision(6) << inj_up << "," << mod_up << ","
                    << (inj_up - mod_up) << "," << std::setprecision(4)
                    << (r_rx - approx_pos).norm() / 1000.0 << "\n";
        }

        std::vector<SppObs> clean;       // bare P (+ISB for Galileo), noise-free internal SPP
        std::vector<SppObs> noisy_bare;  // bare P (+ISB) + code noise, noisy internal SPP
        std::vector<SppObs> single_freq; // GPS L1 code, Klobuchar+tropo removed (Part B student)
        std::vector<sim::SatObs> sat_rows;
        std::vector<Eigen::Vector3d> vis_satpos; // visible-sat ECEF for DOP
        std::string sat_list;
        int n_gps = 0, n_gal = 0;

        for (const auto& plan : plans) {
            for (int prn = 1; prn <= plan.max_prn; ++prn) {
                char id[4];
                std::snprintf(id, sizeof(id), "%c%02d", plan.sys, prn);
                const std::string sat(id);
                double P, sat_clk, elev, az, m7 = 0.0;
                double clk_err_m = 0.0;
                Eigen::Vector3d sat_pos, sat_vel;
                if (!forwardPseudorange(eph, sat, t, r_rx, cdt_r_m, P, sat_pos, sat_vel, sat_clk, elev, az,
                                        scfg.corrections, m7,
                                        sat_clk_err.active() ? &sat_clk_err : nullptr,
                                        &clk_err_m)) continue;
                if (elev < mask_deg) continue;

                const bool is_gal = (plan.sys == 'E');
                // Internal SPP sees bare geometry+clocks (+ISB on Galileo); it
                // recovers position, the receiver clock, and the ISB.
                //
                // The synthetic satellite-clock error is added back here, so this row is
                // the unperturbed pseudorange.  The noise-free SPP verifies the forward
                // model to 1 mm; it does not observe the injected fault.  With the fault
                // in this row, the gate would fail at the injection magnitude and point
                // at the injection.  Injecting inside EphemerisProvider::clock() instead,
                // so both sides move together, would let the gate pass however wrong the
                // arc became.  Only the emitted RINEX carries the error.
                clean.push_back({ sat, P + clk_err_m + plan.isb_m, is_gal });
                vis_satpos.push_back(sat_pos);
                if (!sat_list.empty()) sat_list += " ";
                sat_list += sat;
                if (is_gal) ++n_gal; else ++n_gps;

                // Troposphere via reuse: hydrostatic ZHD·mf_d (the model the processor
                // subtracts, so it cancels) + truth wet ZWD·mf_w (the state the
                // processor estimates).  Non-dispersive: same +delay on code and phase.
                const double zhd = zhd_ep;   // computed per epoch: same (position,time), satellite-independent
                double mf_d = 0.0, mf_w = 0.0;
                eph.tropoMapping(elev, r_rx, t, mf_d, mf_w);
                const double tropo = zhd * mf_d + zwd_ep * mf_w;

                // Multi-band code + phase with elevation-weighted noise.  All bands share
                // the geometric range (the ionosphere is added per band below); each
                // carries an integer ambiguity.  The GPS->Galileo ISB is a common +offset
                // on every Galileo code and phase row (GPS is the reference, ISB = 0).
                // Quiet background, per iono.background_source.  The storm (SED+TID) is
                // layered on top in evaluate().
                //
                // I_klob_L1 is what a single-frequency student's own broadcast Klobuchar
                // removes.  Under "klobuchar" the injected background is this number, so
                // it cancels exactly and sf_iono_residual is the literal 0.0 below rather
                // than a computed difference: the round trip *f^2/(40.3e16) then
                // *40.3e16/f^2 is not bit-exact and would break byte-identity against
                // every existing dataset.
                double I_klob_L1 = 0.0;
                if (eph.hasKlobuchar())
                    I_klob_L1 = eph.klobucharDelayMetres(r_rx, sat_pos, t, f_L1);

                double quiet_slant_stec = 0.0;
                double sf_iono_residual = 0.0;   // metres of L1 iono the student cannot remove
                double gim_std_tecu = 0.0, gim_ipp_lat = 0.0, gim_ipp_lon = 0.0;
                switch (background) {
                case Background::Gim: {
                    double stec = 0.0;
                    if (eph.gimSlantStec(r_rx, sat_pos, t, stec, gim_std_tecu, gim_ipp_lat, gim_ipp_lon)) {
                        quiet_slant_stec = stec;   // already slant; must not be mapped again.
                    } else {
                        // Fall back in place, never with `continue`: skipping the satellite
                        // would shift every downstream RNG draw for the rest of the arc.
                        ++gim_fallback_count;
                        quiet_slant_stec = I_klob_L1 * f_L1 * f_L1 / (40.3 * 1e16);
                    }
                    // The real, non-removable part: what the GIM put in minus what
                    // Klobuchar takes out.  Can be either sign.
                    sf_iono_residual =
                        sim::IonoDisturbance::slantDelayMetres(quiet_slant_stec, f_L1) - I_klob_L1;
                    break;
                }
                case Background::Klobuchar:
                    // Keeps existing Klobuchar-background datasets byte-identical,
                    // including the analytic fallback when the nav file carries no GPSA/GPSB.
                    quiet_slant_stec =
                        eph.hasKlobuchar()
                            ? I_klob_L1 * f_L1 * f_L1 / (40.3 * 1e16)
                            : icfg.quiet_vtec_tecu / std::sin(std::max(elev, scfg.noise.elev_factor_floor_deg) * deg);
                    sf_iono_residual = 0.0;        // cancels by construction
                    break;
                case Background::Analytic:
                    quiet_slant_stec = icfg.quiet_vtec_tecu / std::sin(std::max(elev, scfg.noise.elev_factor_floor_deg) * deg);
                    sf_iono_residual = 0.0;
                    break;
                }
                // Ionosphere at the moving IPP for this ray (Klobuchar quiet + storm).
                // A "stable" satellite (gain 0) sees only the quiet background + no scintillation.
                const double sgain = stormGain(sat);
                const sim::IonoSample samp = iono.evaluate(ts.lat_deg, ts.lon_deg, az, elev, t,
                                                           quiet_slant_stec, sgain,
                                                           tidPeriodOverride(sat), tidSlot(sat),
                                                           tidVtecExplicit(sat, t.tow),
                                                           iono.turbVtec(sat, t));
                const double sigphi = iono.scintSigmaPhiRad(samp, elev) * sgain; // no scintillation on stable sats
                const double dI_L1 = sim::IonoDisturbance::slantDelayMetres(
                    samp.dstec_sed_tecu + samp.dstec_tid_tecu + samp.dstec_turb_tecu,
                    f_L1); // disturbance-only L1 delay (turbulence counts as disturbance)
                if (std::abs(dI_L1) > peak_dI_L1) {
                    peak_dI_L1 = std::abs(dI_L1);
                    peak_dI_tow = t.tow;
                    peak_dI_sat = sat;
                }
                atmo_csv << t.week << "," << std::setprecision(3) << t.tow << "," << sat << ","
                         << std::setprecision(4) << elev << "," << az << "," << samp.ipp_lat_deg << ","
                         << samp.ipp_lon_deg << "," << samp.stec_quiet_tecu << "," << samp.dstec_sed_tecu
                         << "," << samp.dstec_tid_tecu << "," << samp.dstec_turb_tecu
                         << "," << samp.stec_total_tecu << ","
                         << sim::IonoDisturbance::slantDelayMetres(samp.stec_total_tecu, f_L1) << ","
                         << dI_L1 << "," << (samp.in_storm_window ? 1 : 0) << ","
                         << samp.obliquity << "," << I_klob_L1 << "," << sf_iono_residual << ","
                         << gim_ipp_lat << "," << gim_ipp_lon << "," << gim_std_tecu << "\n";

                const double el_factor = 1.0 / std::sin(std::max(elev, scfg.noise.elev_factor_floor_deg) * deg);
                const double sc = sigma_zen * el_factor;   // code sigma (m)
                const double sp = sigma_phase * el_factor; // phase sigma (m)

                // Coloured phase error (card SFR): one draw per (station, satellite,
                // epoch), outside the band loop, so the same metres reach every band's
                // phase.  The time argument is seconds since the arc's first epoch; the
                // generator advances each line of sight across the time since its own
                // previous visible epoch, so a set-and-rise gap decorrelates it.
                // Inactive (the default): a literal 0.0 and no draw.
                double pc_m = 0.0;
                if (want_phase_colour) {
                    const double t_arc_s = (t.week - truth.front().time.week) * 604800.0
                                         + (t.tow - truth.front().time.tow);
                    pc_m = phase_colour.sample(sat, t_arc_s, elev);
                    max_abs_phase_colour_m = std::max(max_abs_phase_colour_m, std::abs(pc_m));
                    sumsq_phase_colour_m2 += pc_m * pc_m;
                    ++n_phase_colour;
                    phase_colour_csv << t.week << "," << std::setprecision(3) << t.tow << ","
                                     << sat << "," << std::setprecision(6) << pc_m << ","
                                     << phase_colour.lastUnit() << "\n";
                }

                sim::SatObs row;
                row.sat_id = sat;
                double first_code_noise = 0.0;
                double I_band[2] = { 0.0, 0.0 };
                // Multipath elevation factor capped (default 20 deg): real low-elevation
                // multipath is large, but uncapped it biases the short post-slip
                // ambiguity segments enough to defeat truth recovery.
                const double mp_inv_sin = 1.0 / std::sin(std::max(elev, scfg.multipath.elev_factor_cap_deg) * deg);
                for (std::size_t bi = 0; bi < plan.bands.size(); ++bi) {
                    const Band& band = plan.bands[bi];
                    const double f_b = Constants::SPEED_OF_LIGHT / band.wavelength;
                    const double I_b = sim::IonoDisturbance::slantDelayMetres(samp.stec_total_tecu, f_b);
                    if (bi < 2) I_band[bi] = I_b;
                    const double nc = sc * gauss(rng);
                    const double np = sp * gauss(rng);
                    const double sig_scint_m = sigphi * band.wavelength / (2.0 * Constants::PI);
                    const double np_scint = sig_scint_m * gauss(rng); // band-decorrelated scintillation

                    // Marine multipath: per-band Gauss-Markov, elevation-amplified.  Band-
                    // specific (does not cancel in IF), so it sets the colored residual floor.
                    const std::string mkey = sat + "/" + band.code;
                    auto mit = mp_gm.find(mkey);
                    const double g = (mit == mp_gm.end())
                        ? ngm(aux_rng) // stationary initialization on first sight
                        : mp_phi * mit->second + std::sqrt(1.0 - mp_phi * mp_phi) * ngm(aux_rng);
                    mp_gm[mkey] = g;
                    const double mp_code = band.mp_code_amp * mp_inv_sin * g;
                    const double mp_phase = mp_phase_amp * mp_inv_sin * g;
                    max_mp_code = std::max(max_mp_code, std::abs(mp_code));

                    // Cycle slip: integer jump in this band's ambiguity, rate ~1/sin^2(el),
                    // with a per-band cooldown.  Mixed flagging: ~half carry LLI=1 (the rest
                    // must be detected from MW/geometry-free combinations).
                    const std::string skey = sat + "/" + band.phase;
                    int slip_lli = 0;
                    if (epoch_counter - slip_last_epoch[skey] >= slip_min_gap &&
                        uni(aux_rng) < slipProb(elev) * slipTaper(epoch_counter)) {
                        const int d = drawSlip(elev);
                        slip_accum[skey] += d;
                        slip_last_epoch[skey] = epoch_counter;
                        // Mixed flagging, alternating so a flagged/unflagged mix is
                        // guaranteed even when only a few slips occur: ~half carry LLI=1
                        // (cooperative receiver), the rest must be detected.  Magnitudes
                        // are scaled to the elevation noise floor, so even the unflagged
                        // slips stay clear of the noise and are recoverable via MW / GF.
                        const bool flagged = (n_slips % 2 == 0);
                        if (flagged) { slip_lli = 1; ++n_slips_flagged; }
                        ++n_slips;
                        writeSlipRow(t, sat, band.phase, d, flagged, elev, /*scripted=*/false);
                    }
                    // Scripted slips: deterministic, hand-authored events applied after
                    // the random block.  This block does not touch aux_rng,
                    // slip_last_epoch or n_slips (that would shift every downstream
                    // random draw and break byte-identity/additivity); it writes only
                    // slip_accum, the LLI OR (slip_lli), its own counters and the CSV.
                    for (const auto& f : sched.onEpoch(sat, band.phase, epoch_counter, elev)) {
                        slip_accum[skey] += f.delta_cycles; // physical: shifts N from here on
                        if (f.lli) slip_lli = 1;            // ORs with a same-epoch random flag
                        ++n_slips_scripted;
                        if (f.lli) ++n_slips_scripted_flagged;
                        writeSlipRow(t, sat, band.phase, f.delta_cycles, f.lli, elev, /*scripted=*/true);
                        scripted_applied.push_back(ScriptedApplied{
                            sat, band.phase, epoch_counter, t.tow, f.delta_cycles, f.lli, elev });
                    }
                    const int base_N = ambN(sat, prn, static_cast<int>(bi));
                    const int N = base_N + slip_accum[skey]; // piecewise-constant across slips
                    if (bi == 0) first_code_noise = nc;
                    // Antenna PCO/PCV (M7 antenna; satellite, plus receiver when
                    // corrections.rx_antenna_type is set): band-dependent (Galileo
                    // E01 != E05 PCO; PCV varies with nadir), so injected per signal
                    // here rather than folded into the non-dispersive m7.  It is the
                    // signed getAntennaCorrection() the processor adds to its modeled
                    // value (PppModel::computeBaseModeledValue, "+ antenna_corr");
                    // enabling model_corrections.enable_satellite_pco/pcv (and
                    // enable_receiver_pco/pcv) cancels it back to truth.  PCO/PCV are
                    // identical for code and phase (wind-up is a separate term, below),
                    // so one call per band serves both.  0 unless enabled in the sim config.
                    const double ant = want_antenna
                        ? eph.antennaMetres(sat, band.code, r_rx, sat_pos, sat_vel, elev, az, t)
                        : 0.0;
                    // Observable-specific signal biases (OSB): band- and observable-
                    // dependent (the code and phase OSBs of one band differ, e.g.
                    // C1C != L1C), so queried once per RINEX code and added to the
                    // matching row.  It is the signed getBiasCorrection() the processor
                    // adds to its modeled value ("+ bias_corr"); enabling
                    // model_corrections.enable_code_phase_bias cancels it back to truth.
                    // 0 unless corrections.signal_bias is set (then row.values is
                    // byte-identical: x + 0.0 == x).  Recorded once per (sat, signal).
                    double bias_code = 0.0, bias_phase = 0.0;
                    if (want_signal_bias) {
                        // product OSB + the deterministic synthetic delta (0 unless the
                        // code appears in signals.synth_osb_amp_m).  truth_bias.csv
                        // records the total, which is what the estimator must reproduce,
                        // with the split alongside so a grader can tell a product bias
                        // from an injected one without re-deriving either.
                        // corrections.signal_bias_scope (card ARL) drops one side: the
                        // excluded observable gets 0 and truth_bias.csv records 0 for it.
                        const bool osb_code  = scfg.corrections.signal_bias_scope != "phase";
                        const bool osb_phase = scfg.corrections.signal_bias_scope != "code";
                        const double prod_code  = osb_code  ? eph.signalBiasMetres(sat, band.code,  t) : 0.0;
                        const double prod_phase = osb_phase ? eph.signalBiasMetres(sat, band.phase, t) : 0.0;
                        const double syn_code   = osb_code  ? synthOsb(sat, band.code)  : 0.0;
                        const double syn_phase  = osb_phase ? synthOsb(sat, band.phase) : 0.0;
                        bias_code  = prod_code  + syn_code;
                        bias_phase = prod_phase + syn_phase;
                        auto record = [&](const std::string& sig, double osb_m,
                                          double prod_m, double syn_m) {
                            const std::string key = sat + "|" + sig;
                            if (bias_seen.insert(key).second) {
                                osb_by_key[key] = osb_m;
                                max_abs_osb_m = std::max(max_abs_osb_m, std::abs(osb_m));
                                bias_csv << sat << "," << sig << ","
                                         << osb_m / (Constants::SPEED_OF_LIGHT * 1e-9) << ","
                                         << osb_m << "," << prod_m << "," << syn_m << ","
                                         << (syn_m != 0.0 ? (prod_m != 0.0 ? "product+synth" : "synth")
                                                          : "product") << "\n";
                            }
                        };
                        record(band.code,  bias_code,  prod_code,  syn_code);
                        record(band.phase, bias_phase, prod_phase, syn_phase);
                    }
                    // Carrier-phase wind-up (M10): phase only, per band (the metres
                    // scale with each wavelength; the underlying accumulated angle is
                    // band-common, enforced by computePhaseWindUp's same-epoch guard,
                    // so MW/WL cycles are untouched).  It is the signed value the
                    // processor adds to its modeled phase when enable_phase_windup (+ the
                    // accumulated estimation.accumulate_phase_windup mode) is set, so it
                    // cancels in the graded loop; an external full-stack service models
                    // it from its own attitude chain.  0 unless corrections.phase_windup
                    // is set (then x + 0.0 == x, byte-identical).
                    const double windup_m = want_windup
                        ? eph.phaseWindupMetres(sat, r_rx, sat_pos, sat_vel, band.wavelength, t)
                        : 0.0;
                    if (want_windup && bi == 0 && windup_csv.is_open()) {
                        const double wc = windup_m / band.wavelength;  // band-common cycles
                        max_abs_windup_cycles = std::max(max_abs_windup_cycles, std::abs(wc));
                        windup_csv << t.week << "," << std::setprecision(3) << t.tow << ","
                                   << sat << "," << std::setprecision(6) << wc << "\n";
                    }
                    // +I on code (group delay), -I on phase (advance); same slant STEC
                    // scaled by 1/f^2 so the dual-frequency IF combination cancels it.
                    // m7 = the M7 correction terms (relativity, Shapiro, solid-earth,
                    // pole and ocean tide), non-dispersive like tropo; 0 unless enabled
                    // in the sim config.  The processor with the matching corrections on
                    // cancels it.  rxb_* = this station's per-observable receiver bias
                    // (0 unless biases.rx_code_bias / rx_phase_bias name the code).
                    // Satellite-independent and epoch-constant by construction
                    // (quasi-static hardware, the class the producer models).  It makes
                    // the S-basis contract's Table-8 fleet-mean identity a real test
                    // rather than 0 == 0 (SPEC §4.5).
                    const double rxb_code  = rxBias(rx_code_bias_map,  band.code);
                    const double rxb_phase = rxBias(rx_phase_bias_map, band.phase);
                    row.values[band.code] = P + tropo + m7 + ant + bias_code + rxb_code + plan.isb_m + I_b + nc + mp_code;
                    // pc_m (card SFR) is the band-common coloured phase error in metres,
                    // phase only; the literal 0.0 when the block is off, and x + 0.0 == x,
                    // so an off run is byte-identical.
                    row.values[band.phase] =
                        (P + tropo + m7 + ant + bias_phase + rxb_phase + plan.isb_m - I_b + windup_m + band.wavelength * N + np + np_scint + mp_phase + pc_m) /
                        band.wavelength;
                    if (slip_lli) row.lli[band.phase] = 1; // loss-of-lock flag for flagged slips
                    const std::string akey = sat + "/" + band.phase;
                    if (amb_seen.insert(akey).second) // record the initial integer (pre-slip)
                        amb_csv << sat << "," << band.phase << "," << base_N << "," << band.wavelength << "\n";
                }
                // Internal iono-free cancellation check on the noise-free codes:
                // cc1*(P+..+I1) + cc2*(P+..+I2) must drop the iono to ~0.
                if (plan.bands.size() == 2) {
                    const double f1 = Constants::SPEED_OF_LIGHT / plan.bands[0].wavelength;
                    const double f2 = Constants::SPEED_OF_LIGHT / plan.bands[1].wavelength;
                    const double cc1 = f1 * f1 / (f1 * f1 - f2 * f2);
                    const double cc2 = -f2 * f2 / (f1 * f1 - f2 * f2);
                    worst_if_resid = std::max(worst_if_resid, std::abs(cc1 * I_band[0] + cc2 * I_band[1]));
                }
                sat_rows.push_back(std::move(row));
                noisy_bare.push_back({ sat, P + plan.isb_m + first_code_noise, is_gal }); // bare(+ISB)+code noise
                // Single-frequency Part-B observable: GPS L1 code with the student's own
                // broadcast Klobuchar and the true troposphere removed (as a competent
                // single-frequency student would), leaving range + code noise + everything
                // Klobuchar cannot take out:
                //   dI_L1            the storm overlay (SED/TID/turbulence), always on top
                //                    of the background;
                //   sf_iono_residual the injected quiet background minus Klobuchar: zero
                //                    by construction for the Klobuchar background, and the
                //                    real model error when the background is a CODE GIM.
                if (!is_gal)
                    single_freq.push_back({ sat, P + dI_L1 + sf_iono_residual + first_code_noise, false });
            }
        }
        if (clean.size() < 5) continue; // need >=5 obs for x,y,z,clk,isb
        total_obs += static_cast<int>(clean.size());

        // Visibility / DOP pre-flight for this epoch.
        const Coordinates::Llh llh{ ts.lat_deg * deg, ts.lon_deg * deg, ts.h_m };
        const Dop dop = computeDop(r_rx, vis_satpos, llh);
        const int n_sats = static_cast<int>(vis_satpos.size());
        min_sats = std::min(min_sats, n_sats);
        min_gps = std::min(min_gps, n_gps);
        min_gal = std::min(min_gal, n_gal);
        if (dop.ok) worst_pdop = std::max(worst_pdop, dop.pdop);
        vis_csv << t.week << "," << t.tow << "," << n_sats << "," << n_gps << "," << n_gal << ","
                << dop.gdop << "," << dop.pdop << "," << dop.hdop << "," << dop.vdop << "," << sat_list << "\n";

        writer.writeEpoch(t, sat_rows);
        truth_csv << t.week << "," << std::setprecision(3) << t.tow << "," << std::setprecision(4)
                  << r_rx.x() << "," << r_rx.y() << "," << r_rx.z() << "," << std::setprecision(7)
                  << ts.lat_deg << "," << ts.lon_deg << "," << std::setprecision(4) << ts.h_m << ","
                  << std::setprecision(10) << ts.clock_bias_s << "," << std::setprecision(2)
                  << ts.heading_deg << "," << ts.speed_mps << "\n";
        clk_csv << t.week << "," << std::setprecision(3) << t.tow << "," << std::setprecision(12)
                << ts.clock_bias_s << "," << std::setprecision(4) << cdt_r_m << ","
                << std::setprecision(4) << isb_true_m << "\n";

        // Per-epoch noise-free inversion: must recover this epoch's true position and
        // the injected ISB (forward-model consistency including the ISB).
        Eigen::Vector3d rx_clean = r_rx + Eigen::Vector3d(1500.0, -1200.0, 900.0); // perturbed start
        double cdt_clean = 0.0, isb_clean = 0.0;
        if (solveSpp(eph, t, clean, rx_clean, cdt_clean, isb_clean)) {
            worst_clean = std::max(worst_clean, (rx_clean - r_rx).norm());
            worst_isb_clean = std::max(worst_isb_clean, std::abs(isb_clean - isb_true_m));
        }

        Eigen::Vector3d rx_noisy = r_rx + Eigen::Vector3d(1500.0, -1200.0, 900.0);
        double cdt_noisy = 0.0, isb_noisy = 0.0;
        if (solveSpp(eph, t, noisy_bare, rx_noisy, cdt_noisy, isb_noisy))
            worst_noisy = std::max(worst_noisy, (rx_noisy - r_rx).norm());

        // Single-frequency GPS L1 + Klobuchar SPP (Part B): the storm, sitting above
        // the removed Klobuchar quiet field, biases the position inside the window.
        if (single_freq.size() >= 4) {
            const bool epoch_storm = (t.tow >= icfg.window_start_tow_s &&
                                      t.tow <= icfg.window_start_tow_s + icfg.window_duration_s);
            Eigen::Vector3d rx_sf = r_rx + Eigen::Vector3d(1500.0, -1200.0, 900.0);
            double cdt_sf = 0.0, isb_sf = 0.0;
            if (solveSpp(eph, t, single_freq, rx_sf, cdt_sf, isb_sf)) {
                const double err = (rx_sf - r_rx).norm();
                if (epoch_storm) { ++sf_storm_n; sf_storm_sum += err; sf_storm_max = std::max(sf_storm_max, err); }
                else { ++sf_quiet_n; sf_quiet_sum += err; sf_quiet_max = std::max(sf_quiet_max, err); }
            }
        }
        ++epochs_written;
    }
    writer.close();

    // ---- Answer key: scalar truths (read by the Verifier) + human summary. ----
    const double win_start = icfg.window_start_tow_s;
    const double win_end = icfg.window_start_tow_s + icfg.window_duration_s;

    // Signal-bias summary: the ionosphere-free code bias an OSB-ignorant PPP would carry
    // per GPS satellite = c1*OSB(C1C) + c2*OSB(C2W), the IF combination of the injected
    // observable biases relative to the clock's C1W/C2W datum (whose own IF OSB is ~0 by
    // the absolute-bias convention).  Galileo emits its clock-reference pair, so its IF
    // OSB is ~0 and is not reported.  0/0 range when signal_bias is off (no rows written).
    double gps_if_osb_min = 0.0, gps_if_osb_max = 0.0;
    int gps_if_osb_n = 0;
    if (want_signal_bias && plans.size() >= 1 && plans[0].bands.size() == 2) {
        const double f1 = Constants::SPEED_OF_LIGHT / plans[0].bands[0].wavelength;
        const double f2 = Constants::SPEED_OF_LIGHT / plans[0].bands[1].wavelength;
        const double denom = f1 * f1 - f2 * f2;
        const double c1 = f1 * f1 / denom, c2 = -f2 * f2 / denom;
        const std::string s1 = plans[0].bands[0].code, s2 = plans[0].bands[1].code; // C1C, C2W
        std::set<std::string> gps_sats;
        for (const auto& kv : osb_by_key)
            if (!kv.first.empty() && kv.first[0] == 'G')
                gps_sats.insert(kv.first.substr(0, kv.first.find('|')));
        for (const auto& g : gps_sats) {
            auto i1 = osb_by_key.find(g + "|" + s1);
            auto i2 = osb_by_key.find(g + "|" + s2);
            if (i1 == osb_by_key.end() || i2 == osb_by_key.end()) continue;
            const double ifb = c1 * i1->second + c2 * i2->second;
            gps_if_osb_min = gps_if_osb_n ? std::min(gps_if_osb_min, ifb) : ifb;
            gps_if_osb_max = gps_if_osb_n ? std::max(gps_if_osb_max, ifb) : ifb;
            ++gps_if_osb_n;
        }
    }
    {
        std::ofstream sc(outdir / "truth_scalars.csv");
        sc << "key,value\n" << std::fixed;
        sc << "kinematic," << (kinematic ? 1 : 0) << "\n";
        sc << "isb_gps_gal_m," << std::setprecision(6) << isb_true_m << "\n";
        // zwd_m is the arc mean when the wet delay varies (the per-epoch series is in
        // truth_tropo.csv, which is what the Verifier grades); it equals the injected
        // constant when it does not.  Kept so the single-value student grader still works.
        sc << "zwd_m," << (epoch_counter ? zwd_sum / epoch_counter : zwd_true_m) << "\n";
        sc << "zwd_time_varying," << (zwd_varies ? 1 : 0) << "\n";
        if (zwd_varies) {
            sc << "zwd_min_m," << zwd_min_seen << "\n";
            sc << "zwd_max_m," << zwd_max_seen << "\n";
        }
        // Ocean loading.  Emitted only when injected, so OTL-off datasets keep their
        // byte-identical truth_scalars.csv.  The student grader reads this file and
        // scenario_summary.json's "truth" object; without these rows the term would be
        // visible only in truth_otl.csv.
        if (scfg.corrections.ocean_tide) {
            sc << "otl_along_track," << (eph.hasOceanTideAlongTrack() ? 1 : 0) << "\n";
            sc << "otl_peak_up_m," << std::setprecision(6) << otl_peak_abs_inj << "\n";
            // 0 in frozen mode (cancels by construction); nonzero is the purpose of the
            // along-track field.
            sc << "otl_unmodelled_peak_up_m," << otl_peak_abs_dup << "\n";
        }
        // Signal biases (OSB).  Emitted only when injected, so bias-off datasets keep a
        // byte-identical truth_scalars.csv.  The IF-code range is the quantity of
        // interest: the per-GPS-satellite ionosphere-free code bias an OSB-ignorant PPP
        // leaves in.
        if (scfg.corrections.signal_bias) {
            sc << "signal_bias_max_abs_osb_m," << std::setprecision(6) << max_abs_osb_m << "\n";
            sc << "signal_bias_gps_if_code_min_m," << gps_if_osb_min << "\n";
            sc << "signal_bias_gps_if_code_max_m," << gps_if_osb_max << "\n";
        }
        if (scfg.corrections.phase_windup)
            sc << "windup_max_abs_cycles," << std::setprecision(6) << max_abs_windup_cycles << "\n";
        // Coloured phase error (card SFR).  Emitted only when active, so an off run keeps
        // a byte-identical truth_scalars.csv; the next row sets its own precision.
        if (want_phase_colour) {
            sc << "phase_colour_sigma_zen_m," << std::setprecision(6) << phase_colour.sigmaZenM() << "\n";
            sc << "phase_colour_tau_s," << std::setprecision(3) << phase_colour.tauS() << "\n";
            sc << "phase_colour_elev_factor_floor_deg," << phase_colour.elevFloorDeg() << "\n";
            sc << "phase_colour_max_abs_m," << std::setprecision(6) << max_abs_phase_colour_m << "\n";
            sc << "phase_colour_rms_m,"
               << (n_phase_colour ? std::sqrt(sumsq_phase_colour_m2 / static_cast<double>(n_phase_colour)) : 0.0)
               << "\n";
        }
        sc << "storm_window_start_tow_s," << std::setprecision(3) << win_start << "\n";
        sc << "storm_window_end_tow_s," << win_end << "\n";
        sc << "peak_dI_L1_m," << std::setprecision(4) << peak_dI_L1 << "\n";
        sc << "code_sigma_zen_m," << sigma_zen << "\n";
        sc << "phase_sigma_zen_m," << sigma_phase << "\n";
        sc << "mp_tau_s," << std::setprecision(1) << mp_tau_s << "\n";
        sc << "n_cycle_slips," << n_slips << "\n";
        sc << "n_cycle_slips_flagged," << n_slips_flagged << "\n";
        sc << "week," << truth.front().time.week << "\n";
        sc << "first_tow_s," << std::setprecision(3) << truth.front().time.tow << "\n";
        sc << "interval_s," << tcfg.interval_s << "\n";
        sc << "n_epochs," << epochs_written << "\n";
    }
    {
        std::ofstream js(outdir / "scenario_summary.json");
        js << std::fixed;
        js << "{\n";
        js << "  \"scenario\": \"SAR vessel adrift, South Pacific near Fiji\",\n";
        js << "  \"date\": \"" << fd.dir << " (" << fd.iso << ")\",\n";
        js << "  \"iono_background\": \"" << scfg.iono.background_source << "\",\n";
        js << "  \"station\": \"" << hdr.marker_name << "\",\n";
        js << "  \"kinematic\": " << (kinematic ? "true" : "false") << ",\n";
        js << "  \"week\": " << truth.front().time.week << ",\n";
        js << "  \"first_tow_s\": " << std::setprecision(3) << truth.front().time.tow << ",\n";
        js << "  \"interval_s\": " << tcfg.interval_s << ",\n";
        js << "  \"n_epochs\": " << epochs_written << ",\n";
        // Station-keeping prefix (trajectory.static_prefix_s).  Counted from the
        // truth rather than from the config, so it reports what the data contains,
        // not what was requested.  The window (n_epochs / tow range) is disclosed to
        // students (the brief tells them the vessel was holding position), while
        // position_* is the grading key for the fixed-point solution and is excluded
        // from make_brief.py's whitelist like every other truth.
        std::size_t n_static = 0;
        for (const auto& s : truth) {
            if ((s.ecef - truth.front().ecef).norm() <= 1.0e-6) ++n_static;
            else break;
        }
        if (n_static >= 2) {
            const auto& s0 = truth.front();
            const auto& sN = truth[n_static - 1];
            js << "  \"static_segment\": {\n";
            js << "    \"n_epochs\": " << n_static << ",\n";
            js << "    \"tow_start_s\": " << std::setprecision(3) << s0.time.tow << ",\n";
            js << "    \"tow_end_s\": " << sN.time.tow << ",\n";
            js << "    \"duration_s\": " << (sN.time.tow - s0.time.tow) << ",\n";
            js << "    \"position_ecef_m\": [" << std::setprecision(4) << s0.ecef.x() << ", "
               << s0.ecef.y() << ", " << s0.ecef.z() << "],\n";
            js << "    \"position_llh_deg_m\": [" << std::setprecision(7) << s0.lat_deg << ", "
               << s0.lon_deg << ", " << std::setprecision(3) << s0.h_m << "]\n";
            js << "  },\n";
        }
        js << "  \"signals\": {\"GPS\": [\"C1C\",\"L1C\",\"C2W\",\"L2W\"], \"Galileo\": [\"C1C\",\"L1C\",\"C5Q\",\"L5Q\"]},\n";
        // The ambiguity regime is a fixture property that changes what an estimator
        // experiment can conclude (a zero-target cold start begins inside its capture
        // region under "field" and ~4 decades outside it under "legacy"), so the fixture
        // on disk declares it.  Otherwise a grader must infer it from
        // truth_ambiguities.csv, and an accidental mixed-regime A/B reads as an estimator
        // result rather than as two different datasets.  Emitted only when not legacy, so
        // every existing fixture regenerates byte-identically (same rule as
        // truth_slips.csv's `source` column, which appears only when scripted slips exist).
        if (scfg.ambiguity.regime != sim::AmbiguityRegime::Legacy) {
            js << "  \"ambiguity\": {\"regime\": \"field\", \"half_width_cycles\": "
               << scfg.ambiguity.half_width_cycles << "},\n";
        }
        js << "  \"truth\": {\n";
        js << "    \"gps_galileo_isb_m\": " << std::setprecision(4) << isb_true_m << ",\n";
        js << "    \"zwd_m\": " << (epoch_counter ? zwd_sum / epoch_counter : zwd_true_m) << ",\n";
        js << "    \"zwd_time_varying\": " << (zwd_varies ? "true" : "false") << ",\n";
        if (zwd_varies) {
            js << "    \"zwd_min_m\": " << zwd_min_seen << ",\n";
            js << "    \"zwd_max_m\": " << zwd_max_seen << ",\n";
            js << "    \"zwd_series\": \"truth_tropo.csv\",\n";
        }
        js << "    \"final_position_ecef_m\": [" << std::setprecision(4) << truth.back().ecef.x() << ", "
           << truth.back().ecef.y() << ", " << truth.back().ecef.z() << "],\n";
        js << "    \"final_position_llh_deg_m\": [" << std::setprecision(7) << truth.back().lat_deg << ", "
           << truth.back().lon_deg << ", " << std::setprecision(3) << truth.back().h_m << "]\n";
        js << "  },\n";
        js << "  \"storm\": {\n";
        js << "    \"window_tow_s\": [" << std::setprecision(3) << win_start << ", " << win_end << "],\n";
        js << "    \"peak_slant_dI_L1_m\": " << std::setprecision(4) << peak_dI_L1 << ",\n";
        js << "    \"peak_sat\": \"" << peak_dI_sat << "\", \"peak_tow_s\": " << std::setprecision(3) << peak_dI_tow << ",\n";
        if (!gim_file_name.empty())
            js << "    \"gim_file\": \"" << gim_file_name << "\",\n";
        // Human-readable twin of the top-level "iono_background" above.  It follows
        // iono.background_source, not only whether the nav file carries Klobuchar
        // coefficients: a GIM-background run still has GPSA/GPSB in its nav file, so
        // keying off hasKlobuchar() alone would label every GIM instance "broadcast
        // Klobuchar".  make_brief.py falls back to this string when the top-level key
        // is absent, so a wrong value here can reach the student brief (it matches
        // case-insensitively on "gim").
        js << "    \"quiet_background\": \""
           << (background == Background::Gim      ? "GIM (IONEX)"
             : background == Background::Analytic ? "analytic"
             : (eph.hasKlobuchar() ? "broadcast Klobuchar"
                                   : "analytic (no broadcast Klobuchar)")) << "\"\n";
        js << "  },\n";
        js << "  \"noise\": {\"code_sigma_zen_m\": " << std::setprecision(4) << sigma_zen
           << ", \"phase_sigma_zen_m\": " << sigma_phase << ", \"seed\": " << scfg.noise.seed << "},\n";
        js << "  \"multipath\": {\"model\": \"Gauss-Markov\", \"tau_s\": " << std::setprecision(1) << mp_tau_s
           << ", \"code_amp_zenith_m\": {\"GPS_L1\": " << std::setprecision(3) << plans[0].bands[0].mp_code_amp
           << ", \"GPS_L2\": " << plans[0].bands[1].mp_code_amp << ", \"GAL_E1\": " << plans[1].bands[0].mp_code_amp
           << ", \"GAL_E5a\": " << plans[1].bands[1].mp_code_amp << "}"
           << ", \"phase_amp_zenith_m\": " << std::setprecision(4) << mp_phase_amp
           << ", \"elev_factor_cap_deg\": " << std::setprecision(1) << scfg.multipath.elev_factor_cap_deg
           << ", \"peak_code_m\": " << std::setprecision(3) << max_mp_code << "},\n";
        // Coloured phase error (card SFR).  Emitted only when active, so an off run's
        // summary is byte-identical; every number below sets its own precision and the
        // next object's numbers set theirs.  The seed is the configured (unsalted) value,
        // like the noise seed above; the station salt is applied on top of it.
        if (want_phase_colour) {
            js << "  \"phase_colour\": {\"model\": \"Gauss-Markov, per line of sight, band-common, phase only\""
               << ", \"sigma_zen_m\": " << std::setprecision(6) << phase_colour.sigmaZenM()
               << ", \"tau_s\": " << std::setprecision(3) << phase_colour.tauS()
               << ", \"elev_factor_floor_deg\": " << phase_colour.elevFloorDeg()
               << ", \"seed\": " << scfg.phase_colour.seed
               << ", \"n_lines_of_sight\": " << phase_colour.starts()
               << ", \"n_samples\": " << n_phase_colour
               << ", \"max_abs_m\": " << std::setprecision(6) << max_abs_phase_colour_m
               << ", \"rms_m\": "
               << (n_phase_colour ? std::sqrt(sumsq_phase_colour_m2 / static_cast<double>(n_phase_colour)) : 0.0)
               << ", \"detail\": \"truth_phase_colour.csv\"},\n";
        }
        js << "  \"cycle_slips\": {\"count\": " << n_slips << ", \"flagged_lli\": " << n_slips_flagged
           << ", \"unflagged\": " << (n_slips - n_slips_flagged) << ", \"detail\": \"truth_slips.csv\"";
        // The answer-key record of where the scripted slips landed: one entry per
        // applied repetition, with resolved epochs.  Emitted only when scripted events
        // exist, so a summary without scripted slips is byte-identical (R2).
        if (has_scripted) {
            js << ",\n    \"scripted\": {\"count\": " << n_slips_scripted
               << ", \"flagged_lli\": " << n_slips_scripted_flagged << ", \"events\": [";
            for (std::size_t i = 0; i < scripted_applied.size(); ++i) {
                const auto& ev = scripted_applied[i];
                js << (i ? ", " : "") << "{\"sat\": \"" << ev.sat << "\", \"signal\": \"" << ev.signal
                   << "\", \"epoch\": " << ev.epoch << ", \"tow\": " << std::setprecision(3) << ev.tow
                   << ", \"delta_cycles\": " << ev.delta_cycles
                   << ", \"lli\": " << (ev.lli ? "true" : "false")
                   << ", \"elev_deg\": " << std::setprecision(2) << ev.elev_deg << "}";
            }
            js << "]}";
        }
        js << "},\n";
        if (scfg.corrections.ocean_tide) {
            // "unmodelled_peak_up_m" is the headline number: 0 for the frozen field
            // (cancels by construction), non-zero only when the field varies along track.
            js << "  \"ocean_loading\": {\"source\": \"EOT20 (radial)\", \"mode\": \""
               << (eph.hasOceanTideAlongTrack() ? "along_track" : "frozen_at_approx_position")
               << "\", \"anchors\": " << eph.oceanTideAnchorCount()
               << ", \"anchor_spacing_km\": " << std::setprecision(2)
               << scfg.corrections.otl_anchor_spacing_km
               << ", \"peak_up_m\": " << std::setprecision(4) << otl_peak_abs_inj
               << ", \"unmodelled_peak_up_m\": " << otl_peak_abs_dup
               << ", \"series\": \"truth_otl.csv\"},\n";
        }
        if (scfg.corrections.signal_bias) {
            // Observable-specific biases.  gps_if_code_bias_m is the quantity of interest:
            // the range error of an OSB-ignorant ionosphere-free PPP, per GPS satellite,
            // because the data carry C1C while the precise clock is C1W/C2W-referenced.
            // It cancels in the graded loop (enable_code_phase_bias), so it moves no
            // gate; a dataset without it would bias an OSB-aware external service.
            js << "  \"signal_bias\": {\"source\": \"COD MGX OSB\", \"file\": \"" << bia.filename().string()
               << "\", \"applied\": true, \"n_signals\": " << static_cast<int>(bias_seen.size())
               << ", \"max_abs_osb_m\": " << std::setprecision(6) << max_abs_osb_m
               << ", \"gps_if_code_bias_m\": [" << gps_if_osb_min << ", " << gps_if_osb_max << "]"
               << ", \"clock_ref_obs\": {\"GPS\": \"C1W/C2W\", \"Galileo\": \"C1C/C5Q\"}"
               << ", \"detail\": \"truth_bias.csv\"},\n";
        }
        // Receiver clock model.  Emitted only when the clock is more than a constant
        // bias, so every existing instance's summary stays byte-identical.  This is
        // answer-key material for Part A: a drifting/wandering clock makes one clock
        // parameter per epoch necessary (n = 3 + n_epochs), and these numbers measure
        // how wrong a single shared clock offset is.  The truth clock moves
        // drift_over_arc_m metres across the arc, of which rw_sigma_arc_m cannot be
        // represented by any low-order polynomial in time.  Per-epoch truth is in
        // truth_clock.csv (clock_bias_s / clock_bias_m).
        if (scfg.trajectory.clock_drift_sps != 0.0 || scfg.trajectory.clock_rw_q_m2ps > 0.0) {
            const double c_ms = Constants::SPEED_OF_LIGHT;
            // Elapsed arc from the epoch count, not a tow difference: a tow subtraction
            // would go negative across a GPS week rollover and hand sqrt() a NaN.
            const double arc_s = (truth.size() > 1)
                                     ? (truth.size() - 1) * scfg.trajectory.interval_s : 0.0;
            js << "  \"receiver_clock\": {\"model\": \"bias0 + drift*t + random_walk\""
               << ", \"bias0_s\": " << std::setprecision(12) << scfg.trajectory.clock_bias0_s
               << ", \"drift_sps\": " << scfg.trajectory.clock_drift_sps
               << ", \"drift_mps\": " << std::setprecision(6) << scfg.trajectory.clock_drift_sps * c_ms
               << ", \"rw_q_m2ps\": " << scfg.trajectory.clock_rw_q_m2ps
               << ", \"rw_sigma_per_epoch_m\": " << std::setprecision(4)
               << std::sqrt(scfg.trajectory.clock_rw_q_m2ps * scfg.trajectory.interval_s)
               << ", \"rw_sigma_arc_m\": " << std::sqrt(scfg.trajectory.clock_rw_q_m2ps * arc_s)
               << ", \"seed\": " << scfg.trajectory.clock_seed
               << ", \"bias_first_m\": " << std::setprecision(4) << truth.front().clock_bias_s * c_ms
               << ", \"bias_last_m\": " << truth.back().clock_bias_s * c_ms
               << ", \"drift_over_arc_m\": "
               << (truth.back().clock_bias_s - truth.front().clock_bias_s) * c_ms
               << ", \"detail\": \"truth_clock.csv\"},\n";
        }
        if (scfg.corrections.phase_windup) {
            // Carrier-phase wind-up.  Accumulated (unwrapped) within each tracked arc,
            // so a wind-up-aware estimator keeps time-constant integer ambiguities;
            // phase-only and band-common in cycles.  It cancels in the graded loop
            // (enable_phase_windup + accumulate_phase_windup), and an external
            // full-stack service (CSRS-PPP) models it unconditionally.
            js << "  \"phase_windup\": {\"applied\": true, \"accumulated\": true"
               << ", \"max_abs_cycles\": " << std::setprecision(6) << max_abs_windup_cycles
               << ", \"detail\": \"truth_windup.csv\"},\n";
        }
        js << "  \"verifier_gates\": {\"k_sigma\": 3, \"ppp_3d_max_m\": 0.10, \"isb_tol_m\": 0.10, \"zwd_tol_m\": 0.02}\n";
        js << "}\n";
    }

    const double span_m = (truth.back().ecef - truth.front().ecef).norm();
    std::cout << "[" << tag << "] wrote " << obs_path.string() << "\n";
    std::cout << "[" << tag << "] epochs: " << epochs_written << "/" << truth.size()
              << ", total GPS+Galileo obs: " << total_obs << "\n";
    std::cout << std::setprecision(5) << "[" << tag << "] trajectory: start ("
              << truth.front().lat_deg << ", " << truth.front().lon_deg << ") -> end ("
              << truth.back().lat_deg << ", " << truth.back().lon_deg << "); start-to-end "
              << std::setprecision(1) << span_m << " m; cruise " << truth.back().speed_mps << " m/s\n";
    std::cout << std::setprecision(6) << "[" << tag
              << "] worst noise-free per-epoch |rx_est - truth| = " << worst_clean * 1000.0 << " mm\n";
    std::cout << "[" << tag << "] worst noise-free |isb_est - truth| = " << worst_isb_clean * 1000.0
              << " mm (injected GPS->Gal ISB " << std::setprecision(3) << isb_true_m << " m)\n";
    std::cout << std::setprecision(6) << "[" << tag
              << "] worst noisy per-epoch |rx_est - truth| = " << worst_noisy << " m\n";
    std::cout << "[" << tag << "] visibility: min sats = " << min_sats << " (GPS " << min_gps
              << " + Gal " << min_gal << "), worst PDOP = " << std::setprecision(2) << worst_pdop << "\n";
    std::cout << "[" << tag << "] injected truth: ZWD = " << std::setprecision(3)
              << (epoch_counter ? zwd_sum / epoch_counter : zwd_true_m) << " m";
    if (zwd_varies)
        std::cout << " MEAN (time-varying " << zwd_min_seen << " .. " << zwd_max_seen
                  << " m; per-epoch truth in truth_tropo.csv)";
    std::cout << " (ZHD cancels via reuse), GPS->Gal ISB = " << isb_true_m << " m\n";
    std::cout << std::setprecision(3) << "[" << tag << "] iono storm: window TOW ["
              << icfg.window_start_tow_s << ", " << (icfg.window_start_tow_s + icfg.window_duration_s)
              << "], quiet VTEC " << icfg.quiet_vtec_tecu << " TECU, peak disturbance slant dI_L1 = "
              << peak_dI_L1 << " m @ " << peak_dI_sat << " tow " << peak_dI_tow << "\n";
    std::cout << std::setprecision(4) << "[" << tag
              << "] dual-freq IF iono cancellation residual (noise-free) = " << worst_if_resid * 1.0e3
              << " mm (should be ~0; storm cancels in IF)\n";
    if (scfg.corrections.ocean_tide) {
        // Formatted through a local stream: cout is in default float mode here, and
        // switching it to std::fixed would reformat every line that follows.
        std::ostringstream otl_msg;
        otl_msg << std::fixed << std::setprecision(2) << "[" << tag
                << "] ocean loading: peak radial " << otl_peak_abs_inj * 1.0e3
                << " mm, peak UNMODELLED radial " << otl_peak_abs_dup * 1.0e3 << " mm ("
                << (eph.hasOceanTideAlongTrack()
                        ? "along-track field; this is what the processor cannot model"
                        : "frozen at APPROX POSITION; cancels by construction")
                << ")";
        std::cout << otl_msg.str() << "\n";
    }
    if (scfg.corrections.signal_bias) {
        std::ostringstream bmsg;
        bmsg << std::fixed << std::setprecision(3) << "[" << tag
             << "] signal biases (COD MGX OSB): " << bias_seen.size() << " (sat,signal) applied, max |OSB| "
             << max_abs_osb_m << " m; GPS iono-free code bias " << gps_if_osb_min << " .. " << gps_if_osb_max
             << " m over " << gps_if_osb_n << " sats (cancels via enable_code_phase_bias; truth_bias.csv)";
        std::cout << bmsg.str() << "\n";
    }
    if (scfg.corrections.phase_windup) {
        std::cout << std::setprecision(3) << "[" << tag
                  << "] phase wind-up (Wu 1993, accumulated): max |cycles| " << max_abs_windup_cycles
                  << " -- phase-only, band-common cycles; cancels via enable_phase_windup"
                     " + accumulate_phase_windup (truth_windup.csv)\n";
    }
    std::cout << std::setprecision(3) << "[" << tag << "] marine multipath (Gauss-Markov tau=" << mp_tau_s
              << "s): peak code MP = " << max_mp_code << " m; cycle slips: " << n_slips << " ("
              << n_slips_flagged << " flagged LLI=1, " << (n_slips - n_slips_flagged) << " must-detect)";
    if (n_slips_scripted > 0)
        std::cout << "; scripted: " << n_slips_scripted << " (" << n_slips_scripted_flagged << " flagged)";
    std::cout << "\n";
    if (want_phase_colour) {
        // Formatted through a local stream, like the ocean-loading line, so cout's
        // float mode for the lines that follow is untouched.
        std::ostringstream pc_msg;
        pc_msg << std::fixed << std::setprecision(2) << "[" << tag
               << "] coloured phase error (Gauss-Markov per line of sight, tau="
               << phase_colour.tauS() << " s, sigma_zen=" << phase_colour.sigmaZenM() * 1.0e3
               << " mm, floor " << phase_colour.elevFloorDeg() << " deg): "
               << phase_colour.starts() << " lines of sight, rms "
               << (n_phase_colour ? std::sqrt(sumsq_phase_colour_m2 / static_cast<double>(n_phase_colour)) : 0.0) * 1.0e3
               << " mm, max |e| " << max_abs_phase_colour_m * 1.0e3
               << " mm -- band-common, phase only (truth_phase_colour.csv)";
        std::cout << pc_msg.str() << "\n";
    }
    if (sf_quiet_n > 0 && sf_storm_n > 0) {
        std::cout << std::setprecision(3) << "[" << tag
                  << "] single-freq SPP (GPS L1 + Klobuchar): quiet 3D mean " << sf_quiet_sum / sf_quiet_n
                  << " m (max " << sf_quiet_max << "), STORM 3D mean " << sf_storm_sum / sf_storm_n
                  << " m (max " << sf_storm_max << ") -- storm biases single-freq, cancels in dual-freq\n";
    } else if (sf_storm_n > 0) {
        // Whole-arc storm window (a maximum-storm-content instance): there is no
        // within-arc quiet control, so the storm-vs-quiet contrast gate below cannot be
        // evaluated; the gradable single-frequency signal is the absolute degradation
        // throughout (a quiet day sits at ~1-2 m).
        std::cout << std::setprecision(3) << "[" << tag
                  << "] single-freq SPP (GPS L1 + Klobuchar): storm window spans the WHOLE arc — 3D mean "
                  << sf_storm_sum / sf_storm_n << " m (max " << sf_storm_max
                  << ") throughout; no within-arc quiet control\n";
    }

    int failures = 0;
    // R5: a scripted event that never fired (the satellite never satisfied its trigger)
    // or stopped mid-repeat (the arc ended) is a hard error.  The outputs above are
    // already written, but a requested exam fixture that is missing must fail the run.
    // Each problem is named on stderr.
    for (const std::string& problem : sched.unfinished()) {
        std::cerr << "[" << tag << "] FAIL: " << problem << "\n";
        ++failures;
    }
    // isfinite first: `NaN > 1e-6` is false, so a non-finite STEC would pass the test.
    if (!std::isfinite(worst_if_resid) || worst_if_resid > 1.0e-6) {
        std::cerr << "[" << tag << "] FAIL: IF iono cancellation residual " << worst_if_resid
                  << " m exceeds 1 um — per-band iono law/frequencies inconsistent with the IF combination\n";
        ++failures;
    }
    // A dataset that mixes a GIM background with a Klobuchar stand-in on some rays is
    // not a GIM dataset, and nothing downstream could detect the mix.
    if (gim_fallback_count > 0) {
        std::cerr << "[" << tag << "] FAIL: " << gim_fallback_count
                  << " ray(s) fell back to Klobuchar because the GIM query was invalid —"
                     " the injected background is not uniformly the GIM\n";
        ++failures;
    }
    if (storm_scale > 0.0 && peak_dI_L1 < 0.3) {
        std::cerr << "[" << tag << "] FAIL: peak disturbance slant dI_L1 " << peak_dI_L1
                  << " m < 0.3 m — storm absent/misconfigured (no gradable signal)\n";
        ++failures;
    }
    if (peak_dI_L1 > 9.0) {
        std::cout << "[" << tag << "] WARN: peak disturbance slant dI_L1 " << peak_dI_L1
                  << " m > 9 m — single-freq SPP may hit outlier gating (DESIGN §6.3 risk)\n";
    }
    if (storm_scale > 0.0 && sf_storm_n > 0 && sf_quiet_n > 0 &&
        (sf_storm_sum / sf_storm_n) <= (sf_quiet_sum / sf_quiet_n)) {
        std::cerr << "[" << tag << "] FAIL: single-freq storm-window 3D error not elevated vs quiet"
                  << " — the storm should bias single-freq (it cancels only in dual-freq)\n";
        ++failures;
    }
    if (epochs_written == 0) {
        std::cerr << "[" << tag << "] FAIL: no epochs written\n";
        ++failures;
    }
    if (worst_clean > 1.0e-3) { // 1 mm forward-model consistency gate
        std::cerr << "[" << tag << "] FAIL: noise-free recovery " << worst_clean
                  << " m exceeds 1 mm — forward model and SPP are inconsistent\n";
        ++failures;
    }
    // An armed satellite-clock error that injected nothing is a wiring failure, not a
    // quiet run (the same distinction the event grader makes).  A step that injects
    // zero would be graded downstream as an undetected event and would read as a guard
    // defect, so the channel is checked here before anything grades against it.
    if (sat_clk_err.active()) {
        if (!sat_clk_err.writeTruth(outdir / "truth_satclock.csv")) {
            std::cerr << "[" << tag << "] FAIL: cannot write truth_satclock.csv\n";
            ++failures;
        }
        if (sat_clk_err.peakAbsMetres() <= 0.0) {
            std::cerr << "[" << tag << "] FAIL: sat_clock is armed but the generated peak "
                         "|error| is 0 — the channel is INERT and every injected event "
                         "would grade as undetected\n";
            ++failures;
        }
    }
    if (worst_isb_clean > 1.0e-3) { // the forward model must carry the ISB consistently
        std::cerr << "[" << tag << "] FAIL: noise-free ISB recovery off by " << worst_isb_clean
                  << " m (> 1 mm) — ISB not injected/inverted consistently\n";
        ++failures;
    }
    const double noisy_bound = std::max(15.0, 50.0 * sigma_zen); // scales with code sigma (0.3 m => 15 m)
    if (worst_noisy > noisy_bound) { // sanity bound for the configured code SPP per epoch
        std::cerr << "[" << tag << "] FAIL: noisy recovery " << worst_noisy << " m implausibly large (> "
                  << noisy_bound << " m for code sigma " << sigma_zen << " m)\n";
        ++failures;
    }
    if (min_sats < 5) {
        std::cerr << "[" << tag << "] FAIL: only " << min_sats << " sats at the worst epoch (need >= 5)\n";
        ++failures;
    }
    if (min_gal < 1) {
        std::cerr << "[" << tag << "] FAIL: no Galileo sats at some epoch — ISB unobservable\n";
        ++failures;
    }
    if (worst_pdop > 10.0) {
        std::cerr << "[" << tag << "] FAIL: worst PDOP " << worst_pdop << " > 10 (geometry unsolvable)\n";
        ++failures;
    }
    if (failures == 0) {
        std::cout << "[" << tag << "] PASS - real-orbit GPS+Galileo synthesis inverts to truth (position + ISB)\n";
        return 0;
    }
    return 1;
}

// Generate the truth-referenced exam dashboard (pyviz/run_report.py --truth-dir)
// for a completed `all` loop.  Follows Application::generateReportFor_, without the
// report-python override: python = baked venv -> PATH.  On Windows the whole quoted
// command is wrapped in an extra pair of quotes because std::system runs it through
// cmd /c, which strips the outer quotes.  Non-fatal: a missing pyviz/python only
// warns.  The last line printed is the dashboard path (SPEC §4.7).
void generateExamDashboard(const std::filesystem::path& truth_dir,
                           const std::filesystem::path& logs_dir) {
    namespace fs = std::filesystem;
    const fs::path root   = fs::path(GNSS_PROJECT_ROOT);
    const fs::path script = root / "pyviz" / "run_report.py";
    const fs::path out     = fs::absolute(logs_dir) / "dashboard.html";
    if (!fs::exists(script)) {
        std::cerr << "[all] dashboard: " << script.string()
                  << " not found - skipping (see pyviz/README.md)\n";
        return;
    }
    const fs::path venv_py = root / "venv" / "Scripts" / "python.exe";
    const std::string python = fs::exists(venv_py) ? venv_py.string() : std::string("python");

    // Every path is quoted (spaces); --truth-dir enables the exam_* viewers; the report
    // is offline and interactive (plotly + inline JS) for the grader archive.
    // --exclude drops viewers that are redundant or unhelpful on the exam dashboard:
    //   exam_otl_truth        (ocean loading is a mm-level error-budget item, not a figure)
    //   plot_satellite_signals (signal-usage strip)
    //   dops                  (duplicates the DOP panel already in exam_visibility above)
    //   ambiguities           (raw float ambiguities are unreadable without time-differencing)
    //   slip_gate_strip       (internal slip-detector QC strip)
    std::string inner = "\"" + python + "\" \"" + script.string() + "\" \""
                      + fs::absolute(logs_dir).string() + "\" --truth-dir \""
                      + fs::absolute(truth_dir).string() + "\" -o \"" + out.string()
                      + "\" --backend plotly --plotly-js inline"
                      + " --exclude exam_otl_truth plot_satellite_signals dops"
                        " ambiguities slip_gate_strip";
#ifdef _WIN32
    const std::string cmd = "\"" + inner + "\"";
#else
    const std::string cmd = inner;
#endif
    std::cout << "[all] dashboard: generating " << out.string() << " (plotly, inline JS)\n";
    const int rc = std::system(cmd.c_str());
    if (rc != 0)
        std::cerr << "[all] dashboard generation exited " << rc
                  << " (is pyviz set up? see pyviz/README.md) - non-fatal\n";
    std::cout << out.string() << "\n";   // dashboard path = last line
}

} // namespace

int main(int argc, char** argv) {
    // An optional "--sim-config <path>" (or "--sim-config=<path>") is removed from the
    // argument list before positional parsing, so the harness contract
    // (synth-* <outdir> <isb> <storm_scale>) is unchanged.
    std::string sim_config_path;
    std::vector<std::string> a;
    for (int i = 0; i < argc; ++i) {
        const std::string s = argv[i];
        if (s == "--sim-config") {
            if (i + 1 >= argc) { std::cerr << "[gnss_simulator] --sim-config requires a path\n"; return 2; }
            sim_config_path = argv[++i];
            continue;
        }
        if (s.rfind("--sim-config=", 0) == 0) { sim_config_path = s.substr(13); continue; }
        a.push_back(s);
    }
    const int ac = static_cast<int>(a.size());

    const std::string cmd = ac > 1 ? a[1] : "selftest";
    auto outdirArg = [&](const std::filesystem::path& dflt) {
        return ac > 2 ? std::filesystem::path(a[2]) : dflt;
    };
    if (cmd == "selftest") {
        return runSelfTest(outdirArg(std::filesystem::temp_directory_path() / "gnss_sim_selftest"));
    }
    if (cmd == "probe-orbit") {
        return runProbeOrbit(outdirArg(std::filesystem::temp_directory_path() / "gnss_sim_probe"));
    }
    if (cmd == "verify") {
        if (ac < 4) {
            std::cerr << "usage: gnss_simulator verify <truth_dir> <run_logs_dir>\n";
            return 2;
        }
        sim::VerifierConfig vcfg;
        vcfg.truth_dir = a[2];
        vcfg.run_logs_dir = a[3];
        const sim::VerifierResult res = sim::Verifier::run(vcfg);
        for (const auto& line : res.report) std::cout << line << "\n";
        // Machine-readable verdict for the dashboard (SPEC §4.6), written in addition
        // to the printed report above, which is unchanged.
        const std::filesystem::path repjson = vcfg.run_logs_dir / "verify_report.json";
        if (sim::Verifier::writeJsonReport(res, repjson))
            std::cout << "  wrote " << repjson.string() << "\n";
        return res.pass ? 0 : 1;
    }

    // Synthesis knobs resolve in order: struct defaults, then the --sim-config file (if
    // any), then legacy positional overrides for isb_m and storm_scale so existing
    // harness invocations keep working.  The positional default (omitted => 8.0 /
    // 1.0) equals the struct default, so omitting everything reproduces the
    // default synthesis byte-for-byte.
    sim::SimConfig scfg;
    if (!sim_config_path.empty()) {
        std::string err;
        if (!sim::loadSimConfig(sim_config_path, scfg, &err)) {
            std::cerr << "[gnss_simulator] " << err << "\n";
            return 2;
        }
        std::cout << "[gnss_simulator] sim config: " << sim_config_path << "\n";
    }
    auto applyPositionalOverrides = [&](int isb_idx, int storm_idx) {
        if (ac > isb_idx)   scfg.biases.isb_gps_gal_m = std::atof(a[isb_idx].c_str());
        if (ac > storm_idx) scfg.iono.storm_scale     = std::atof(a[storm_idx].c_str());
    };

    if (cmd == "synth-static" || cmd == "synth-kinematic") {
        const bool kin = (cmd == "synth-kinematic");
        applyPositionalOverrides(/*isb=*/3, /*storm=*/4);
        const std::filesystem::path dflt =
            std::filesystem::temp_directory_path() / (kin ? "gnss_sim_kine" : "gnss_sim_synth");
        return runSynth(outdirArg(dflt), kin, scfg);
    }
    if (cmd == "synth-network") {
        // ---- The synthetic minimum network fixture ----------------------------
        // docs/SPEC_synthetic_minimum_network.md.  Fans the roster out as N child
        // processes, one per station, because an in-process loop cannot work: a second
        // EphemerisProvider re-registers the spdlog logger name and the catch handler
        // re-throws out of the process, and a shared provider carries three
        // station-dependent quantities with no per-station setter (the phase wind-up
        // accumulator is keyed by satellite ID alone and returns the stored value
        // within an epoch, ocean loading is frozen at one site, and the receiver
        // antenna type is one string).  `all` uses child processes for the same reason.
        if (ac < 3) {
            std::cerr << "usage: gnss_simulator synth-network <output_dir> --sim-config <sim.json>\n"
                         "  (the sim config must carry a non-empty \"network_stations\" roster)\n";
            return 2;
        }
        const std::filesystem::path outdir(a[2]);
        if (scfg.network_stations.empty()) {
            std::cerr << "[synth-network] FAIL: the sim config has no 'network_stations' roster "
                         "— use synth-static for a single receiver\n";
            return 2;
        }
        if (sim_config_path.empty()) {
            std::cerr << "[synth-network] FAIL: --sim-config is required (the roster lives in it, "
                         "and each child is derived from it)\n";
            return 2;
        }
        std::error_code ec;
        std::filesystem::create_directories(outdir, ec);

        // Absolute storm anchor, computed once from the roster so every station samples
        // one field at its own pierce point.  Default: the roster centroid plus the
        // configured offsets, which reproduces the single-station intent ("a touch east
        // of the receiver") at network scale.  An explicit iono.center_*_deg takes
        // precedence.
        double anchor_lat = scfg.iono.center_lat_deg, anchor_lon = scfg.iono.center_lon_deg;
        if (!std::isfinite(anchor_lat) || !std::isfinite(anchor_lon)) {
            double slat = 0.0, slon = 0.0;
            for (const auto& st : scfg.network_stations) { slat += st.lat_deg; slon += st.lon_deg; }
            const double n = static_cast<double>(scfg.network_stations.size());
            anchor_lat = slat / n + scfg.iono.center_lat_offset_deg;
            anchor_lon = slon / n + scfg.iono.center_lon_offset_deg;
        }
        std::cout << "[synth-network] " << scfg.network_stations.size()
                  << " stations; storm anchor " << std::fixed << std::setprecision(4)
                  << anchor_lat << ", " << anchor_lon << " (absolute)\n";

        // The fixture manifest: what the producer config and the grader both read.
        std::ofstream man(outdir / "network_fixture.json");
        if (!man) {
            std::cerr << "[synth-network] FAIL: cannot write network_fixture.json\n";
            return 1;
        }
        // Fixed notation, 4 decimals.  position_ecef is the coordinate at which the
        // producer holds the station (CORS geometry), so it must be truth to sub-mm.
        // Default stream formatting prints ~6 significant digits, i.e. "-1.267e+06": a
        // metre-to-kilometre error that looks like an ordinary number and would anchor
        // the entire network off truth without triggering any check.
        man << std::fixed << std::setprecision(4);
        man << "{\n  \"date\": {\"year\": " << scfg.date.year << ", \"doy\" : " << scfg.date.doy
            << "},\n  \"interval_s\": " << scfg.trajectory.interval_s
            << ",\n  \"n_epochs\": " << scfg.trajectory.n_epochs
            << ",\n  \"storm_anchor_deg\": [" << anchor_lat << ", " << anchor_lon
            << "],\n  \"stations\": [\n";

        int idx = 0, n_fail = 0;
        for (const auto& st : scfg.network_stations) {
            const std::filesystem::path sdir = outdir / st.name;
            const std::filesystem::path ccfg = outdir / ("_child_" + st.name + ".json");
            std::string werr;
            if (!sim::writeStationConfig(sim_config_path, st, anchor_lat, anchor_lon, ccfg, &werr)) {
                std::cerr << "[synth-network] FAIL: " << werr << "\n";
                return 1;
            }
            std::cout << "[synth-network] (" << (idx + 1) << "/" << scfg.network_stations.size()
                      << ") " << st.name << " -> " << sdir.string()
                      << "  salt=" << sim::stationSeedSalt(st.name) << "\n";
            // Double-outer-quote so Windows cmd preserves the inner per-arg quotes.
            const std::string cmdline = "\"\"" + std::filesystem::path(a[0]).string() +
                                        "\" synth-static \"" + sdir.string() +
                                        "\" --sim-config \"" + ccfg.string() + "\"\"";
            const int rc = std::system(cmdline.c_str());
            if (rc != 0) {
                std::cerr << "[synth-network] station " << st.name << " exited " << rc << "\n";
                ++n_fail;
                continue;
            }
            // Truth ECEF for the producer's network.stations[].position_ecef: row 1 of
            // this station's own truth_position.csv.  A static run holds it for the whole
            // arc, so the producer's fixed-coordinate CORS posture is exact truth.
            double x = 0.0, y = 0.0, z = 0.0;
            {
                std::ifstream tp(sdir / "truth_position.csv");
                std::string line;
                std::getline(tp, line);                 // header
                if (std::getline(tp, line)) {
                    std::stringstream ss(line);
                    std::string f;
                    int col = 0;
                    while (std::getline(ss, f, ',')) {
                        if (col == 2) x = std::atof(f.c_str());
                        else if (col == 3) y = std::atof(f.c_str());
                        else if (col == 4) z = std::atof(f.c_str());
                        ++col;
                    }
                }
            }
            // Plausibility guard.  A mis-parsed column yields an ordinary-looking number
            // at which the producer would then hold the station, and no gate downstream
            // can tell a 1 km coordinate error from a real one, because a held
            // coordinate is not estimated.  A ground station's geocentric radius is
            // ~6357-6378 km; anything outside (6300, 6500) km is a parse failure.
            const double r_geo = std::sqrt(x * x + y * y + z * z);
            if (!(r_geo > 6.30e6 && r_geo < 6.50e6)) {
                std::cerr << "[synth-network] FAIL: " << st.name
                          << " truth ECEF is implausible (|r| = " << std::fixed
                          << std::setprecision(1) << r_geo
                          << " m); truth_position.csv missing or mis-parsed\n";
                ++n_fail;
                continue;
            }
            const std::filesystem::path rnx =
                sdir / (st.name + "_R_" + makeFixtureDate(scfg.date.year, scfg.date.doy).yyyydoy +
                        "0000_01D_30S_MO.rnx");
            if (!std::filesystem::exists(rnx)) {
                std::cerr << "[synth-network] FAIL: " << st.name << " emitted no RINEX at "
                          << rnx.string() << "\n";
                ++n_fail;
                continue;
            }
            if (idx++) man << ",\n";
            man << "    {\"name\": \"" << st.name << "\", \"rinex_path\": \""
                << [&] { std::string p = rnx.string();
                         for (auto& ch : p) if (ch == '\\') ch = '/';
                         return p; }()
                << "\", \"position_ecef\": [" << std::setprecision(4) << x << ", " << y << ", "
                << z << "], \"consumer\": " << (st.consumer ? "true" : "false")
                << ", \"truth_dir\": \""
                << [&] { std::string p = sdir.string();
                         for (auto& ch : p) if (ch == '\\') ch = '/';
                         return p; }() << "\"}";
        }
        man << "\n  ]\n}\n";
        man.close();

        if (n_fail) {
            std::cerr << "[synth-network] FAIL: " << n_fail << " of "
                      << scfg.network_stations.size() << " stations failed\n";
            return 1;
        }
        std::cout << "[synth-network] OK: " << idx << " stations -> "
                  << (outdir / "network_fixture.json").string() << "\n";
        return 0;
    }
    if (cmd == "spp-reference") {
        // Part-B reference runs against an already-synthesized truth dir; the sim
        // config supplies the fixture date (and the OSB opt-in), so pass the
        // --sim-config the dataset was synthesized with.
        if (ac < 3) {
            std::cerr << "usage: gnss_simulator spp-reference <truth_dir> [--sim-config <sim.json>]\n";
            return 2;
        }
        return runSppReference(std::filesystem::path(a[2]), scfg);
    }
    if (cmd == "all") {
        // One-shot loop: synthesize -> spp-reference (child process; Part-B reference
        // solutions) -> run gnss_processor (out-of-process, per-run CWD because its
        // log dir is hardcoded to ./logs) -> verify.  The config's
        // files.observation_path has to point at <output_dir>'s synthesized RINEX.
        if (ac < 5) {
            std::cerr << "usage: gnss_simulator all <static|kinematic> <output_dir> <config>"
                         " [processor_exe] [isb_m] [storm_scale]   [--sim-config <sim.json>]\n";
            return 2;
        }
        const std::string mode = a[2];
        if (mode != "static" && mode != "kinematic") {
            std::cerr << "all: mode must be 'static' or 'kinematic'\n";
            return 2;
        }
        const std::filesystem::path outdir = a[3];
        const std::filesystem::path config = a[4];
        const std::filesystem::path proc = ac > 5
            ? std::filesystem::path(a[5])
            : std::filesystem::path(a[0]).parent_path() / "gnss_processor.exe"; // sibling of this exe
        applyPositionalOverrides(/*isb=*/6, /*storm=*/7);

        std::cout << "[all] (1/4) synthesize (" << mode << ") -> " << outdir.string() << "\n";
        if (runSynth(outdir, mode == "kinematic", scfg) != 0) {
            std::cerr << "[all] synthesis failed\n";
            return 1;
        }

        // (2/4) Part-B SPP reference runs (truth_spp.csv + summary), run as a child
        // process rather than in-process: LogManager/spdlog logger names and the
        // product caches are process-global singletons, runSynth's provider has already
        // claimed them in this process, and a second LogManager aborts (its console
        // fallback re-registers the same logger name unguarded).  A failure here fails
        // the run: an instance whose Part-B reference cannot be computed (missing
        // nav/products or a broken RINEX) is not a valid exam artifact.
        std::cout << "[all] (2/4) spp-reference -> " << (outdir / "truth_spp.csv").string() << "\n";
        {
            std::string sp_cmd = "\"\"" + std::filesystem::path(a[0]).string() +
                                 "\" spp-reference \"" + outdir.string() + "\"";
            if (!sim_config_path.empty()) sp_cmd += " --sim-config \"" + sim_config_path + "\"";
            sp_cmd += "\"";
            const int sec = std::system(sp_cmd.c_str());
            if (sec != 0) {
                std::cerr << "[all] spp-reference exited " << sec << "\n";
                return 1;
            }
        }

        const std::filesystem::path rundir = outdir / "run";
        std::error_code ec;
        std::filesystem::create_directories(rundir, ec);
        std::cout << "[all] (3/4) gnss_processor " << config.string() << " (cwd " << rundir.string() << ")\n";
        if (!std::filesystem::exists(proc)) {
            std::cerr << "[all] processor not found: " << proc.string() << " (pass it as arg 5)\n";
            return 1;
        }
        const auto cwd0 = std::filesystem::current_path(ec);
        std::filesystem::current_path(rundir, ec);
        // Double-outer-quote so Windows cmd preserves the inner per-arg quotes.
        const std::string sys_cmd = "\"\"" + proc.string() + "\" \"" + config.string() + "\"\"";
        const int pec = std::system(sys_cmd.c_str());
        std::filesystem::current_path(cwd0, ec);
        if (pec != 0) {
            std::cerr << "[all] gnss_processor exited " << pec << "\n";
            return 1;
        }

        std::cout << "[all] (4/4) verify\n";
        sim::VerifierConfig vcfg;
        vcfg.truth_dir = outdir;
        vcfg.run_logs_dir = rundir / "logs";
        const sim::VerifierResult res = sim::Verifier::run(vcfg);
        for (const auto& line : res.report) std::cout << line << "\n";
        sim::Verifier::writeJsonReport(res, vcfg.run_logs_dir / "verify_report.json");

        // The exam dashboard is generated on every run, including a verify failure
        // (where it is most needed); the exit code still reflects synth/processor/verify.
        generateExamDashboard(outdir, vcfg.run_logs_dir);
        return res.pass ? 0 : 1;
    }
    std::cerr << "usage: " << (argc > 0 ? argv[0] : "gnss_simulator")
              << " <selftest|probe-orbit|synth-static|synth-kinematic|spp-reference|verify|all> ...\n"
              << "  synth-static|synth-kinematic [output_dir] [isb_m] [storm_scale] [--sim-config <sim.json>]\n"
              << "  synth-network <output_dir> --sim-config <sim.json>\n"
              << "  spp-reference <truth_dir> [--sim-config <sim.json>]\n"
              << "  verify <truth_dir> <run_logs_dir>\n"
              << "  all <static|kinematic> <output_dir> <config> [processor_exe] [isb_m] [storm_scale] [--sim-config <sim.json>]\n";
    return 2;
}
