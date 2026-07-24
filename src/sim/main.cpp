/**
 * @file main.cpp
 * @brief Entry point for the GNSS exam dataset simulator (gnss_simulator).
 *
 * Milestone M1 implements the @c selftest subcommand: it synthesizes a small
 * RINEX-3.04 mixed observation file with @c RinexObsWriter, parses it back with
 * the production @c RinexParser, and asserts every written code/phase value
 * round-trips.  This locks the byte-level writer<->parser contract before any
 * physical observation modelling (orbits, clocks, atmosphere) is added in later
 * milestones.
 *
 * Usage:
 *   gnss_simulator selftest [output_dir]
 */

#include <algorithm>
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

#include "common/Constants.hpp"
#include "config/GnssConfig.hpp"
#include "logging/LogManager.hpp"
#include "math/Coordinates.hpp"
#include "math/TimeUtils.hpp"
#include "model/ModelGeometry.hpp"
#include "products/ProductCache.hpp"
#include "products/ProductParser.hpp"
#include "products/ProductTypes.hpp"
#include "rinex/RinexParser.hpp"
#include "sim/EphemerisProvider.hpp"
#include "sim/IonoDisturbance.hpp"
#include "sim/RinexObsWriter.hpp"
#include "sim/ScriptedSlips.hpp"
#include "sim/SimConfig.hpp"
#include "sim/TrajectoryGenerator.hpp"
#include "sim/Verifier.hpp"

using namespace GnssProcessor;

namespace {

/// Carrier wavelength (m) for the (system, RINEX code) pairs exercised by M1.
double wavelengthFor(char sys, const std::string& code) {
    const char band = code.size() >= 2 ? code[1] : '?';
    if (sys == 'G') {
        if (band == '1') return Constants::GPS_L1_WAVELENGTH;
        if (band == '2') return Constants::GPS_L2_WAVELENGTH;
        if (band == '5') return Constants::GPS_L5_WAVELENGTH;
    } else if (sys == 'E') {
        if (band == '1') return Constants::GAL_E1_WAVELENGTH;
        if (band == '5') return Constants::GAL_E5A_WAVELENGTH;
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
    // accumulates ~tens of microseconds, but a 30 s RINEX must land exactly on
    // HH:MM:00 / HH:MM:30.  (The full scenario generator builds integer TOW
    // grids directly; this is the M1 stand-in.)
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

// M2 spike: validate the precise SP3/CLK -> satellite position/clock chain.
//
// Stands up the real Application-style product chain (ProductParser ->
// ProductCache -> Model::Geometry) against the bundled offline fixture
// data/2024-129/, then cross-checks Geometry::getSatellitePV against a
// tabulated SP3 node and getSatelliteClockCorrection against a CLK sample.
// Critically asserts the clock is NOT broadcast-grade — if it were, the
// processor would drop carrier phase and the PPP half of the exam would die
// silently (DESIGN.md SS5.4).
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
    cache.clearCache(); // singleton — start from a clean slate
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
// M2 forward observation model (static receiver, GPS L1 code).
//
// Mirrors the processor's geometry conventions EXACTLY so synthesized
// observations invert against the verifier (ModelGeometry.cpp):
//   - emission time : obs.time - P/c - sat_clk   (pseudorange-based; rx clock cancels)
//   - Sagnac        : theta = omega_E * (||s_em - rx|| / c); sat_pos = R_z(theta) * s_em
//                     R_z = [[ cos, sin, 0],[-sin, cos, 0],[0,0,1]]
//   - model         : P = range + c*dt_r - c*sat_clk  (+noise)
// Iono/tropo/relativity are intentionally OMITTED here; the internal SPP check
// uses the identical model, so this validates the geometry/clock/Sagnac/
// emission machinery in isolation.  Matching the processor's full term set (or
// disabling those terms symmetrically) is the next, end-to-end step.
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
// frame (Sagnac), using the geometric time-of-flight for the rotation angle —
// exactly as Geometry::computeSatelliteGeometry does it.
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
                        const sim::SimConfig::Corrections& corr, double& m7_corr_out) {
    const double c = Constants::SPEED_OF_LIGHT;

    // Seed P from a rough range at obs.time; also serves as a coverage pre-check.
    Eigen::Vector3d s0, v0;
    if (!eph.positionVelocity(sat, obs_time, s0, v0)) return false;
    double P = (sagnacRotate(s0, rx) - rx).norm();

    Eigen::Vector3d sat_pos = Eigen::Vector3d::Zero();
    Eigen::Vector3d s_em = Eigen::Vector3d::Zero(), v_em = Eigen::Vector3d::Zero(); // emission state
    double sat_clk = 0.0;
    for (int it = 0; it < 8; ++it) {
        const GnssTime approx_em = addSeconds(obs_time, -P / c);
        const double sc_approx = eph.clock(sat, approx_em);
        const GnssTime em = addSeconds(obs_time, -(P / c + sc_approx));
        if (!eph.positionVelocity(sat, em, s_em, v_em)) return false;
        bool bcast = false;
        sat_clk = eph.clock(sat, em, &bcast);
        if (std::abs(sat_clk) < 1e-12) return false;
        sat_pos = sagnacRotate(s_em, rx);
        const double range = (sat_pos - rx).norm();
        const double P_new = range + cdt_r_m - c * sat_clk;
        const bool converged = std::abs(P_new - P) < 1e-6;
        P = P_new;
        if (converged) break;
    }
    const auto topo = Model::Geometry::computeTopocentric(rx, sat_pos);
    P_out = P;
    sat_pos_out = sat_pos;      // reception-frame (Sagnac-rotated)
    sat_vel_out = v_em;         // emission-frame velocity (unrotated) — matches the
                                // processor's geom.sat_vel for the antenna attitude frame
    sat_clk_out = sat_clk;
    elev_deg_out = topo.elevation_deg;
    az_deg_out = topo.azimuth_deg;

    // M7 full-correction stack (non-dispersive; added to code+phase like tropo, but
    // NOT to the bare internal SPP).  Relativity uses the emission-time (pre-Sagnac)
    // state; the range/site terms use the reception-frame (rotated) sat_pos + rx.
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
// rows reference receiver clock + ISB (mirroring how the real processor adds a
// per-system time-offset state, MeasurementModel.cpp:1088-1096).
struct SppObs {
    std::string sat;
    double P;
    bool is_gal;
};

// Processor-equivalent single-point code solution: recover (rx ECEF, c*dt_r, and
// — when any Galileo row is present — the GPS->Galileo ISB) from {sat,
// pseudorange} via Gauss-Newton, using the same emission/Sagnac/clock model as
// the synthesizer.  With Galileo present the normal system is 5x5
// (x,y,z,cdt,isb); GPS-only collapses to 4x4.
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

// M2/M3: synthesize dual-frequency GPS observations from real precise orbits for
// a truth trajectory (static point or kinematic vessel) and verify each epoch
// inverts to its truth position — noise-free (forward-model consistency < 1 mm)
// and with elevation-weighted noise.  The bare model (no iono/tropo/relativity)
// is recovered end-to-end by the real processor with corrections OFF.  A
// visibility/DOP pre-flight is logged per epoch so unsolvable geometry is caught.
// ---------------------------------------------------------------------------
//  Fixture date -> everything that must move with it.
//
//  ONE derivation feeds the fixture directory, every product filename, the emitted
//  RINEX filename, the product-day edge guard and the scenario_summary date.  They
//  are coupled and must move ATOMICALLY: an arc whose start_tow_s moves to another
//  day while the emitted filename still names the old one makes the processor
//  resolve the WRONG day's orbits against the new epochs — a multi-day
//  extrapolation that throws nothing and merely looks like a physics problem.
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
    // boundary (no leap-second shift) — the same boundary the products are cut on.
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
    // IGS20 ANTEX for the antenna PCO/PCV terms — loaded into the provider ONLY when
    // the sim config requests one (corrections.sat_antenna for the satellite half,
    // corrections.rx_antenna_type for the receiver half).  It is the same file the
    // processor auto-loads in internal mode (files.internal_data_root/static/atx/
    // igs20.atx), so the injected antenna terms cancel against the processor's own
    // model when the matching enable_satellite_pco/pcv + enable_receiver_pco/pcv are on.
    const std::filesystem::path atx = root / "data" / "static" / "atx" / "igs20.atx";
    // The day's Earth Rotation Parameters (polar motion xp/yp + UT1-UTC).  Loaded
    // into the provider whenever a correction that consumes ERP geometry is
    // requested.  Two consumers: (1) pole tide reads xp/yp directly — with no ERP,
    // GnssCorrections::computePoleTideCorrection returns 0, so the injected pole
    // tide would be 0 while the processor (which auto-globs the obs-day .erp in
    // internal mode) still subtracts a real ~mm-cm term, i.e. the reuse would NOT
    // cancel; and (2) the Sun/Moon frame that drives the solid-earth tide and the
    // satellite-antenna yaw attitude is evaluated from the same ERP.  This is the
    // identical file the processor loads (PathResolver globs the obs-day .erp), so
    // loading it here makes pole tide actually cancel and makes the solid-tide /
    // antenna Sun frame bit-for-bit what the processor computes.
    const std::filesystem::path erp = base / "products" / ("COD0MGXFIN_" + fd.yyyydoy + "0000_01D_12H_ERP.ERP");
    // The day's Bias-SINEX (.BIA) OSB product, loaded into the provider ONLY when
    // corrections.signal_bias is requested.  It is the same COD MGX file the processor
    // auto-globs in internal mode, so the observable-specific biases the sim ADDS to each
    // signal cancel against the processor's own getBiasCorrection once the run config
    // sets model_corrections.enable_code_phase_bias.
    const std::filesystem::path bia = base / "products" / ("COD0MGXFIN_" + fd.yyyydoy + "0000_01D_01D_OSB.BIA");
    for (const std::filesystem::path* p : { &setup_obs, &sp3, &clk }) {
        if (!std::filesystem::exists(*p)) {
            std::cerr << "[" << tag << "] FAIL: missing fixture " << p->string() << "\n";
            return 1;
        }
    }

    // EITHER antenna term needs the ATX.  The RECEIVER term is requested simply by
    // naming a receiver antenna type — the same string that will be declared in the
    // RINEX "ANT # / TYPE" header, so the file can never advertise one antenna while
    // carrying another's pattern.
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
    // consume ERP-derived geometry (see the erp declaration above), so load the ERP
    // whenever any of them is requested — matching the processor, which globs the
    // obs-day .erp and applies it under the corresponding enable_* flags.
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

    // Observable-specific signal biases need the day's .BIA.  Add it to the provider's
    // product set so ProductParser::parseBia loads the OSBs into the shared cache (from
    // which geometry_'s GnssCorrections reads them); fail loud on a missing fixture
    // rather than silently inject nothing while the run config still applies the
    // processor's own copy (which would NOT cancel).
    const bool want_signal_bias = scfg.corrections.signal_bias;
    if (want_signal_bias) {
        if (!std::filesystem::exists(bia)) {
            std::cerr << "[" << tag << "] FAIL: corrections.signal_bias is set but the .BIA "
                         "product is missing: " << bia.string() << "\n";
            return 1;
        }
        products.push_back(bia);
    }

    sim::EphemerisProvider eph(setup_obs, products, outdir / "logs", nav,
                               scfg.corrections.rx_antenna_type);
    if (!eph.ok()) {
        std::cerr << "[" << tag << "] FAIL: EphemerisProvider setup\n";
        return 1;
    }
    // Fail loud rather than silently inject nothing if the ATX yielded no antennas.
    if (want_antenna && !eph.hasAntenna()) {
        std::cerr << "[" << tag << "] FAIL: an antenna correction is set but no antenna model "
                     "loaded from " << atx.string() << "\n";
        return 1;
    }
    // Likewise fail loud if an ERP-consuming correction is on but no ERP entry
    // parsed — otherwise pole tide would silently inject 0 (asymmetric with the
    // processor) and the solid-tide / antenna Sun frame would drift by the polar
    // motion the processor still applies.
    if (need_erp && eph.erpNodeCount() == 0) {
        std::cerr << "[" << tag << "] FAIL: ERP-consuming correction set but no ERP entries loaded from "
                  << erp.string() << "\n";
        return 1;
    }
    // Fail loud if signal biases were requested but the .BIA yielded no entries —
    // otherwise the sim would inject 0 while the processor's enable_code_phase_bias
    // still applies the real OSBs, an asymmetry that does NOT cancel.
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
    tcfg.heave_amp_m = scfg.trajectory.heave_amp_m;
    tcfg.heave_period_s = scfg.trajectory.heave_period_s;
    tcfg.clock_bias0_s = scfg.trajectory.clock_bias0_s;
    tcfg.clock_drift_sps = scfg.trajectory.clock_drift_sps;
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
    // FROZEN (default): coefficients are synthesized ONCE at the first truth epoch's
    // ECEF — the same point the sim writes as the RINEX APPROX POSITION XYZ, which is
    // exactly where Model::Geometry::loadData will synthesize the processor's own set.
    // Both sides therefore evaluate an identical frozen 11-constituent model and the
    // term cancels even though the vessel moves ~100 km.
    //
    // ALONG-TRACK (corrections.ocean_tide_along_track): the EOT20 field is sampled at
    // anchors down the trajectory, so the sim injects the loading at where the vessel
    // IS while the processor keeps modelling the coefficients it froze at the APPROX
    // POSITION.  That asymmetry is the point — it is the one M7 term that does not
    // cancel, and truth_otl.csv records exactly how much is left behind.
    //
    // Either way, hard-fail rather than silently inject nothing (the processor's
    // enable_ocean_tide_loading defaults TRUE, so a silent miss here would leave a real
    // cm-level term uncancelled).
    // ONE expression for "the point the processor will freeze its site models at": it
    // feeds both the OTL reference below and hdr.approx_xyz further down.  They were
    // two independent `truth.front().ecef` expressions, and nothing would have caught
    // them drifting apart — which would silently decouple the sim's OTL reference from
    // the APPROX POSITION the processor actually reads out of the RINEX header.
    Eigen::Vector3d approx_pos = truth.front().ecef;
    // APPROX POSITION policy (SPEC R2).  "truth" (default) leaves it as the exact
    // trajectory start — which equals row 1 of truth_position.csv, a disclosure leak for
    // a student handout.  "quantized" rounds the disclosed last-known position to a coarse
    // lat/lon grid and drops the height, DETERMINISTICALLY (no RNG draw — it must not
    // perturb any shared stream).  This runs BEFORE approx_pos feeds the OTL frozen
    // reference and hdr.approx_xyz, so the sim freezes ocean loading at the SAME point the
    // processor reads out of the RINEX header (R2.2); a ~110 m shift moves the EOT20 field
    // sample negligibly (its grid is 0.125 deg).
    if (scfg.rinex.approx_position_mode == "quantized") {
        const double d2r = Constants::PI / 180.0;
        const Coordinates::Llh llh = Coordinates::ecefToLlh(approx_pos);
        const double q = scfg.rinex.approx_quantize_deg;
        const double lat_q = std::round((llh.lat_rad / d2r) / q) * q;   // lat_rad/d2r = degrees
        const double lon_q = std::round((llh.lon_rad / d2r) / q) * q;
        // Height rounds to the nearest METRE rather than being forced to 0: a floating
        // vessel sits at the sea SURFACE, whose ellipsoidal height is +geoid undulation
        // (~+48 m near the hero LKP, EGM2008) — a zeroed header would disclose a fix
        // ~50 m below the water and gut the Part-E height-systems question.
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
    // The bundled precise products are SINGLE-DAY files (e.g. DOY 129 = 2024-05-08 =
    // GPS week 2313, tow [259200, 345600]): a 05M SP3 whose last node is exactly the
    // day end and a 30S CLK whose last record is 30 s before it.  An arc that reaches
    // an edge cannot be centred in the Lagrange orbit window and (at the end) has no
    // trailing clock node, so orbits/clocks degrade into extrapolation there.
    //
    // Running fully OUTSIDE the product day is a hard FAIL, not a warning: it means the
    // arc and the fixture date disagree, so every epoch would silently interpolate the
    // wrong day's orbits.  Merely sitting NEAR an edge stays a warning (accuracy trap).
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

    std::mt19937_64 rng(scfg.noise.seed);
    std::normal_distribution<double> gauss(0.0, 1.0);
    const double sigma_zen = scfg.noise.code_sigma_zen_m, sigma_phase = scfg.noise.phase_sigma_zen_m;
    // --- Truth wet zenith delay (humid tropical environment) -------------------
    // Optionally TIME-VARYING: a diurnal sinusoid plus a random walk.  The walk is
    // drawn once per EPOCH from a DEDICATED stream (never `rng`/`aux_rng`), so tuning
    // the troposphere cannot shift the measurement-noise or multipath/slip draws — the
    // storm-on/off control and byte-identity both survive.  With the variation knobs at
    // their 0 defaults zwd_ep stays exactly scfg.atmosphere.zwd_m every epoch.
    const auto& atm = scfg.atmosphere;
    const bool zwd_varies = (atm.zwd_diurnal_amp_m > 0.0) || (atm.zwd_rw_sigma_m > 0.0);
    std::mt19937_64 tropo_rng(atm.seed);
    double zwd_rw_state = 0.0;          // accumulated random walk (m)
    double zwd_ep = atm.zwd_m;          // this epoch's truth ZWD (m)
    double zwd_sum = 0.0, zwd_min_seen = 1e9, zwd_max_seen = -1e9;
    const double zwd_true_m = atm.zwd_m;                   // arc MEAN when varying; the value when constant
    const double isb_true_m = scfg.biases.isb_gps_gal_m;   // GPS->Galileo inter-system bias (truth state, DESIGN §5.6)
    const double storm_scale = scfg.iono.storm_scale;      // multiplies storm amplitudes (0 = storm off, same RNG)

    // Multi-GNSS signal plan: GPS L1/L2 (C1C/L1C, C2W/L2W) + Galileo E1/E5a
    // (C1C/L1C, C5Q/L5Q), dual frequency.  Galileo E1 frequency == GPS L1 and
    // E5a == GPS L5 exactly (Constants.hpp:104,107); the GPS<->Galileo difference
    // is injected NOT as a frequency offset but as the receiver ISB on every
    // Galileo code+phase row (GPS is the reference system, ISB = 0).
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
    // per-band zenith amplitudes come from SimConfig (smartphone cases bump these).
    const std::vector<SysPlan> plans = {
        { 'G', 32,
          { { "C1C", "L1C", Constants::GPS_L1_WAVELENGTH, scfg.multipath.code_amp_gps_l1_m },
            { "C2W", "L2W", Constants::GPS_L2_WAVELENGTH, scfg.multipath.code_amp_gps_l2_m } },
          0.0 },
        { 'E', 36,
          { { "C1C", "L1C", Constants::GAL_E1_WAVELENGTH, scfg.multipath.code_amp_gal_e1_m },
            { "C5Q", "L5Q", Constants::GAL_E5A_WAVELENGTH, scfg.multipath.code_amp_gal_e5a_m } },
          isb_true_m },
    };

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
    icfg.center_lat0_deg = tcfg.lat0_deg + scfg.iono.center_lat_offset_deg;
    icfg.center_lon0_deg = tcfg.lon0_deg + scfg.iono.center_lon_offset_deg; // default +0.20 = a touch east
    // storm_scale multiplies the STORM amplitudes only (1.0 = nominal storm;
    // 0.0 = no storm but the SAME RNG draws and the SAME quiet Klobuchar
    // background, for a controlled storm-on/off comparison that isolates the
    // pure storm effect).  The quiet background is the reused broadcast Klobuchar
    // (per sat, below), not scaled here.
    icfg.sed_amp_tecu *= storm_scale;
    icfg.tid_amp_tecu *= storm_scale;
    icfg.scint_sigma_phi_max_rad *= storm_scale;
    const sim::IonoDisturbance iono(icfg);
    // Per-satellite storm participation.  Precedence: (1) explicit iono.tid_pulses ->
    // any satellite named by a pulse is disturbed; (2) iono.tid_sats -> exactly those
    // PRNs; (3) a deterministic (RNG-free) per-sat hash over storm_active_fraction.
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
    // Sum this satellite's EXPLICIT TID pulses (vertical TECU) at epoch tow.  Returns
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
    // "klobuchar" (default) reuses the broadcast field so a single-frequency student's
    // own Klobuchar cancels it EXACTLY and only the storm survives.  "gim" samples the
    // real CODE IONEX for the fixture day: Klobuchar then removes only part of it, and
    // the (GIM - Klobuchar) remainder stays in the single-frequency data as a genuine,
    // non-removable model residual.  Either way the injected first-order term is ONE
    // slant STEC scaled by 1/f^2 per band, so the iono-free combination still cancels.
    enum class Background { Klobuchar, Gim, Analytic };
    const Background background =
        scfg.iono.background_source == "gim"      ? Background::Gim :
        scfg.iono.background_source == "analytic" ? Background::Analytic : Background::Klobuchar;

    if (background == Background::Gim) {
        std::filesystem::path gim_path = scfg.iono.gim_path;
        if (gim_path.empty())
            gim_path = base / "products" / ("COD0OPSFIN_" + fd.yyyydoy + "0000_01D_01H_GIM.INX");
        if (!eph.loadGim(gim_path)) {
            std::cerr << "[" << tag << "] FAIL: iono.background_source=\"gim\" but the IONEX "
                      << gim_path.string() << " could not be loaded.\n";
            return 1;
        }
        // The GIM background does NOT cancel against Klobuchar — that residual IS the
        // single-frequency payload — so the broadcast field must still be present to
        // subtract.  Without it the Part-B storyline silently degrades to nothing.
        if (!eph.hasKlobuchar()) {
            std::cerr << "[" << tag << "] FAIL: iono.background_source=\"gim\" requires a broadcast "
                         "Klobuchar field (GPSA/GPSB) to form the single-frequency residual, but "
                      << nav.string() << " has none.\n";
            return 1;
        }
        // Epochs outside the map span are silently CLAMPED to the endpoint map AND still
        // longitude-rotated — silently wrong truth.  Refuse rather than extrapolate.
        GnssTime g0{}, g1{};
        if (!eph.gimEpochSpan(g0, g1)) {
            std::cerr << "[" << tag << "] FAIL: loaded IONEX carries no maps.\n";
            return 1;
        }
        const double a0 = truth.front().time.tow, a1 = truth.back().time.tow;
        if (truth.front().time.week != g0.week || truth.back().time.week != g1.week ||
            a0 < g0.tow || a1 > g1.tow) {
            std::cerr << "[" << tag << "] FAIL: arc [" << a0 << ", " << a1 << "] (week "
                      << truth.front().time.week << ") is not strictly inside the IONEX map span ["
                      << g0.tow << ", " << g1.tow << "] (week " << g0.week
                      << ") — the GIM would be clamped and longitude-rotated.\n";
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
    // Counts rays where the GIM query failed and Klobuchar stood in.  A run that
    // silently mixes two backgrounds is not a GIM dataset — fail at the end if > 0.
    long long gim_fallback_count = 0;

    // --- Marine multipath (Gauss-Markov) + cycle slips (mixed flagging). -------
    // Drawn from a SEPARATE RNG stream so tuning these does not move the white
    // measurement noise (and vice versa), keeping the storm-on/off control exact.
    std::mt19937_64 aux_rng(scfg.multipath.seed);
    std::normal_distribution<double> ngm(0.0, 1.0);
    std::uniform_real_distribution<double> uni(0.0, 1.0);
    const double mp_tau_s = scfg.multipath.tau_s;                 // multipath correlation time
    const double mp_phi = std::exp(-tcfg.interval_s / mp_tau_s);  // ~0.905 at 30 s
    const double mp_phase_amp = scfg.multipath.phase_amp_zen_m;   // zenith phase multipath (m)
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
        // phase noise and multipath are elevation-amplified, so a slip must be LARGE
        // at low elevation to stay clear of that noise (otherwise it is unrecoverable
        // and biases the float ambiguity), and may be smaller (~10 cyc) at high
        // elevation where the noise is quiet.  Keeps the slip detectable everywhere.
        const double s = std::sin(std::max(el_deg, scfg.noise.elev_factor_floor_deg) * deg);
        const double span = scfg.slips.max_cycles_zenith - scfg.slips.min_cycles_zenith;
        const double base = scfg.slips.min_cycles_zenith + uni(aux_rng) * span; // nominal cycles at zenith
        int mag = static_cast<int>(std::lround(base / s));
        if (mag < scfg.slips.min_magnitude) mag = scfg.slips.min_magnitude;
        return (uni(aux_rng) < 0.5) ? -mag : mag;
    };

    sim::ObsHeaderSpec hdr;
    hdr.run_date = "20260624 000000 UTC";
    hdr.marker_type = kinematic ? "KINEMATIC" : "GEODETIC";
    hdr.approx_xyz = { approx_pos.x(), approx_pos.y(), approx_pos.z() };
    hdr.interval_s = tcfg.interval_s;
    hdr.time_of_first_obs = tcfg.t0;
    // Declare the antenna whose pattern was actually injected.  SINGLE SOURCE OF TRUTH:
    // this is the same string handed to Antenna's receiver lookup, so the header and the
    // injection cannot drift apart.  Padded to the RINEX field width (16-char model +
    // 4-char radome) so the radome lands in cols 37-40 as the IGS convention requires.
    // Left at the writer's uncalibrated placeholder when no receiver antenna is
    // modelled — that is deliberate: an external service finding no calibration applies
    // NOTHING (CSRS-PPP "Case 4", NULL reference PCO), which is exactly right for a
    // dataset carrying no receiver antenna term.
    if (want_rx_antenna) {
        hdr.ant_type = scfg.corrections.rx_antenna_type;
        hdr.ant_type.resize(20, ' ');
    }
    hdr.obs_types = {
        { 'G', { "C1C", "L1C", "C2W", "L2W" } },
        { 'E', { "C1C", "L1C", "C5Q", "L5Q" } },
    };
    // RINEX header realism / leak knobs (SPEC R2).  Each override is applied ONLY when the
    // config supplies it, so an absent/default rinex section keeps every writer default
    // and the file is byte-identical (R2.1).
    if (!scfg.rinex.pgm.empty())      hdr.pgm      = scfg.rinex.pgm;
    if (!scfg.rinex.run_by.empty())   hdr.run_by   = scfg.rinex.run_by;
    if (!scfg.rinex.observer.empty()) hdr.observer = scfg.rinex.observer;
    if (!scfg.rinex.agency.empty())   hdr.agency   = scfg.rinex.agency;
    if (scfg.rinex.file_date_mode == "collection_end") {
        // DATE field = UTC datetime of the last epoch.  GPS -> UTC by subtracting the
        // leap offset; the calendar conversion reuses the SAME TimeUtils path the writer
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

    // Emitted filename carries the SAME date as the fixture products it was built
    // against — the processor resolves its product day from this name.
    const std::filesystem::path obs_path = outdir / ("SARV_R_" + fd.yyyydoy + "0000_01D_30S_MO.rnx");
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
    // Verifier has ONE contract to read; truth_scalars.csv is a key,value file and
    // cannot carry a series.  Mirrors truth_clock.csv, which already publishes a
    // per-epoch truth series for a filter state.
    std::ofstream tro_csv(outdir / "truth_tropo.csv");
    tro_csv << "week,tow_s,zwd_m,zhd_m\n" << std::fixed;

    // Per-epoch ocean-loading truth.  Only meaningful when OTL is injected at all, so
    // the file is emitted only then (a run without it has no ocean-loading truth to
    // publish, and an all-zero file would read as "modelled perfectly" rather than
    // "not simulated").  Like truth_tropo.csv (and unlike truth_position.csv, which is
    // gated on a solvable epoch) it carries one row per TRAJECTORY epoch, so join it on
    // tow rather than by row index.  inj_up is the loading the vessel actually felt;
    // mod_up is what
    // the processor models from its APPROX-POSITION coefficients; d_up = inj - mod is
    // the unmodelled remainder, identically zero unless the field varies along track.
    // Displacements are purely radial (EOT20 carries the height load only), so the local
    // up component is the whole signal; a satellite at elevation el sees -d_up*sin(el)
    // of range error, which is EXACTLY the range partial wrt the up coordinate — so d_up
    // lands on the HEIGHT estimate, not on the clock or ZWD, and leaves no post-fit
    // residual.  dist_from_approx_km is the STRAIGHT-LINE distance from the APPROX
    // POSITION, not path length — on a track that doubles back it is much smaller than
    // the distance sailed, and it is the one that predicts d_up.
    //
    // CAVEAT on mod_up: it is what the processor is EXPECTED to model — the sim's own
    // frozen coefficients — not an observation of what it actually loaded.  The two sides
    // fail asymmetrically: the sim hard-fails on a missing/unreadable EOT20 grid, but
    // Model::Geometry::loadData just logs and installs nothing (and both the geometry and
    // ocean_tide loggers default OFF), so a bad data_download.nc_path, a zero APPROX
    // POSITION, or a station-info/BLQ source winning the precedence would silently leave
    // the processor modelling ZERO.  d_up would then understate the real unmodelled term
    // by the whole ~50 mm frozen part.  Confirm with logging.enable_ocean_tide before
    // trusting a residual attributed to along-track loading.
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
    amb_csv << "satellite,signal,N_int,wavelength_m\n" << std::fixed << std::setprecision(6);
    std::set<std::string> amb_seen;
    auto ambN = [](const std::string& sat, int prn, int band_idx) -> int {
        // Deterministic, distinct per (sat, band); no separate satellite phase bias
        // is injected, so the recovered float ambiguity differences equal these.
        return (sat[0] == 'E' ? 700000 : 100000) + prn * 1000 + band_idx * 137;
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
        bias_csv << "satellite,signal,osb_ns,osb_m\n" << std::fixed << std::setprecision(6);
    }
    std::set<std::string> bias_seen;
    std::map<std::string, double> osb_by_key; // "G01|C1C" -> OSB (m)
    double max_abs_osb_m = 0.0;

    // Per-(epoch,sat) ionosphere truth: STEC decomposition + per-L1 delay.
    std::ofstream atmo_csv(outdir / "truth_atmo.csv");
    // The first 14 columns are frozen — existing tooling (pyviz/conftest.py,
    // exam_iono_truth.py) pins that header.  New columns are APPENDED:
    //   obliquity          the storm field's vertical->slant factor, published so a
    //                      double-mapped GIM background is auditable from the truth file
    //                      (no other gate can see that error — it cancels in iono-free).
    //   I_klob_L1_m        what a single-frequency student's broadcast Klobuchar removes.
    //   sf_iono_resid_m    what they CANNOT remove: injected quiet L1 delay - Klobuchar.
    //                      Exactly 0 for background_source="klobuchar"/"analytic".
    //   gim_*              the GIM's own geocentric IPP on ITS shell (450 km for CODE)
    //                      and the IONEX RMS as a slant-TEC sigma; 0 when no GIM.
    atmo_csv << "week,tow_s,satellite,elev_deg,az_deg,ipp_lat_deg,ipp_lon_deg,"
                "stec_quiet_tecu,dstec_sed_tecu,dstec_tid_tecu,stec_total_tecu,"
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

    // Scripted "gotcha" slips (SPEC_scripted_slips.md).  When the list is empty
    // (absent/`[]`), the schedule never fires and every scripted-conditional
    // output below collapses to today's format -> byte-identical (R2).
    const bool has_scripted = !scfg.slips.scripted.empty();
    sim::ScriptedSlipSchedule sched(scfg.slips.scripted);
    int n_slips_scripted = 0, n_slips_scripted_flagged = 0;
    struct ScriptedApplied {
        std::string sat, signal;
        int epoch; double tow; int delta_cycles; bool lli; double elev_deg;
    };
    std::vector<ScriptedApplied> scripted_applied; // one entry per applied repetition

    std::ofstream slip_csv(outdir / "truth_slips.csv");
    // The trailing ",source" column (random|scripted) is added ONLY when scripted
    // events exist; otherwise the file is written exactly as before (7 columns).
    slip_csv << "week,tow_s,satellite,signal,delta_cycles,flagged,elev_deg"
             << (has_scripted ? ",source" : "") << "\n" << std::fixed;
    // ONE row writer for both the random and scripted paths so their formatting can
    // never diverge (the shared stream interleaves setprecision(3)/(2)).
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
        // Computed ONCE per epoch, outside the satellite loop: the random walk must
        // advance per epoch, not per visible satellite (otherwise it would depend on
        // the sky and the storm-on/off control would diverge).
        if (zwd_varies) {
            // Mean-reverting Gauss-Markov term (same shape as the marine multipath).  A
            // PURE random walk is wrong here: over a 12 h arc it wanders unboundedly and
            // drags the arc mean off the configured value (measured: 3 mm/epoch moved the
            // mean 0.15 -> 0.25 m).  With phi = exp(-dt/tau) this is stationary with
            // 1-sigma = zwd_rw_sigma_m, so the mean stays put.
            if (atm.zwd_rw_sigma_m > 0.0) {
                const double phi = std::exp(-scfg.trajectory.interval_s / atm.zwd_rw_tau_s);
                zwd_rw_state = phi * zwd_rw_state
                             + atm.zwd_rw_sigma_m * std::sqrt(std::max(0.0, 1.0 - phi * phi))
                                                  * gauss(tropo_rng);
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
        // ZHD depends only on (position, time), not on the satellite, so hoist it out
        // of the satellite loop — same value, one call, and it is what truth_tropo.csv
        // records.
        const double zhd_ep = eph.tropoZhd(r_rx, t);
        tro_csv << t.week << "," << std::setprecision(3) << t.tow << ","
                << std::setprecision(6) << zwd_ep << "," << zhd_ep << "\n";

        // ---- This epoch's ocean-loading truth --------------------------------
        // Same hoist as the ZHD: the site displacement depends on (position, time)
        // only, not on the satellite.  Projected onto the local up so the file records
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
                Eigen::Vector3d sat_pos, sat_vel;
                if (!forwardPseudorange(eph, sat, t, r_rx, cdt_r_m, P, sat_pos, sat_vel, sat_clk, elev, az,
                                        scfg.corrections, m7)) continue;
                if (elev < mask_deg) continue;

                const bool is_gal = (plan.sys == 'E');
                // Internal SPP sees bare geometry+clocks (+ISB on Galileo); it
                // recovers position, the receiver clock, and the ISB.
                clean.push_back({ sat, P + plan.isb_m, is_gal });
                vis_satpos.push_back(sat_pos);
                if (!sat_list.empty()) sat_list += " ";
                sat_list += sat;
                if (is_gal) ++n_gal; else ++n_gps;

                // Troposphere via reuse: hydrostatic ZHD·mf_d (the SAME model the
                // processor subtracts -> cancels) + truth wet ZWD·mf_w (the state the
                // processor estimates).  Non-dispersive: same +delay on code & phase.
                const double zhd = zhd_ep;   // hoisted: same (position,time), satellite-independent
                double mf_d = 0.0, mf_w = 0.0;
                eph.tropoMapping(elev, r_rx, t, mf_d, mf_w);
                const double tropo = zhd * mf_d + zwd_ep * mf_w;

                // Dual-frequency code + phase, elevation-weighted noise.  Both bands
                // share the geometric range (no iono yet); each carries an integer
                // ambiguity.  The GPS->Galileo ISB is a common +offset on every
                // Galileo code & phase row (GPS is the reference, ISB = 0).
                // Quiet background, per iono.background_source.  The storm (SED+TID) is
                // layered on top in evaluate().
                //
                // I_klob_L1 is what a single-frequency student's own broadcast Klobuchar
                // removes.  Under "klobuchar" the injected background IS this number, so
                // it cancels exactly and sf_iono_residual is the LITERAL 0.0 below — not a
                // computed difference, because round-tripping through
                // *f^2/(40.3e16) then *40.3e16/f^2 is not bit-exact and would silently
                // break byte-identity against every existing dataset.
                double I_klob_L1 = 0.0;
                if (eph.hasKlobuchar())
                    I_klob_L1 = eph.klobucharDelayMetres(r_rx, sat_pos, t, f_L1);

                double quiet_slant_stec = 0.0;
                double sf_iono_residual = 0.0;   // metres of L1 iono the student CANNOT remove
                double gim_std_tecu = 0.0, gim_ipp_lat = 0.0, gim_ipp_lon = 0.0;
                switch (background) {
                case Background::Gim: {
                    double stec = 0.0;
                    if (eph.gimSlantStec(r_rx, sat_pos, t, stec, gim_std_tecu, gim_ipp_lat, gim_ipp_lon)) {
                        quiet_slant_stec = stec;   // ALREADY SLANT — must NOT be mapped again.
                    } else {
                        // Fall back IN PLACE — never `continue`, which would desync every
                        // downstream RNG draw and diverge the whole arc.
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
                    // Byte-identical to the original expression, including the analytic
                    // fallback when the nav file carries no GPSA/GPSB.
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
                                                           tidVtecExplicit(sat, t.tow));
                const double sigphi = iono.scintSigmaPhiRad(samp, elev) * sgain; // no scintillation on stable sats
                const double dI_L1 = sim::IonoDisturbance::slantDelayMetres(
                    samp.dstec_sed_tecu + samp.dstec_tid_tecu, f_L1); // disturbance-only L1 delay
                if (std::abs(dI_L1) > peak_dI_L1) {
                    peak_dI_L1 = std::abs(dI_L1);
                    peak_dI_tow = t.tow;
                    peak_dI_sat = sat;
                }
                atmo_csv << t.week << "," << std::setprecision(3) << t.tow << "," << sat << ","
                         << std::setprecision(4) << elev << "," << az << "," << samp.ipp_lat_deg << ","
                         << samp.ipp_lon_deg << "," << samp.stec_quiet_tecu << "," << samp.dstec_sed_tecu
                         << "," << samp.dstec_tid_tecu << "," << samp.stec_total_tecu << ","
                         << sim::IonoDisturbance::slantDelayMetres(samp.stec_total_tecu, f_L1) << ","
                         << dI_L1 << "," << (samp.in_storm_window ? 1 : 0) << ","
                         << samp.obliquity << "," << I_klob_L1 << "," << sf_iono_residual << ","
                         << gim_ipp_lat << "," << gim_ipp_lon << "," << gim_std_tecu << "\n";

                const double el_factor = 1.0 / std::sin(std::max(elev, scfg.noise.elev_factor_floor_deg) * deg);
                const double sc = sigma_zen * el_factor;   // code sigma (m)
                const double sp = sigma_phase * el_factor; // phase sigma (m)

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
                    // specific (does NOT cancel in IF) -> sets the colored residual floor.
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
                        // Mixed flagging, ALTERNATING so a flagged/unflagged mix is
                        // guaranteed even when only a few slips occur: ~half carry LLI=1
                        // (cooperative receiver), the rest must be detected.  Magnitudes
                        // are scaled to the elevation noise floor, so even the unflagged
                        // slips stay clear of the noise and are recoverable via MW / GF.
                        const bool flagged = (n_slips % 2 == 0);
                        if (flagged) { slip_lli = 1; ++n_slips_flagged; }
                        ++n_slips;
                        writeSlipRow(t, sat, band.phase, d, flagged, elev, /*scripted=*/false);
                    }
                    // Scripted "gotcha" slips: deterministic, hand-authored events
                    // applied AFTER the random block.  MUST NOT touch aux_rng,
                    // slip_last_epoch, or n_slips (that would shift every downstream
                    // random draw and break byte-identity/additivity) — only
                    // slip_accum, the LLI OR (slip_lli), its own counters, the CSV.
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
                    // Satellite antenna PCO/PCV (M7 antenna): band-dependent (Galileo
                    // E01 != E05 PCO; PCV varies with nadir), so injected PER SIGNAL
                    // here rather than folded into the non-dispersive m7.  Add the same
                    // signed getAntennaCorrection() the processor ADDS to its modeled
                    // value (PppModel::computeBaseModeledValue, "+ antenna_corr");
                    // enabling model_corrections.enable_satellite_pco/pcv cancels it back
                    // to truth.  PCO/PCV are identical for code and phase (wind-up off),
                    // so one call per band serves both.  0 unless enabled in the sim config.
                    const double ant = want_antenna
                        ? eph.antennaMetres(sat, band.code, r_rx, sat_pos, sat_vel, elev, az, t)
                        : 0.0;
                    // Observable-specific signal biases (OSB): band- AND observable-
                    // dependent (the code and phase OSBs of one band differ, e.g.
                    // C1C != L1C), so queried once per RINEX code and added to the
                    // matching row.  Same signed getBiasCorrection() the processor ADDS
                    // to its modeled value ("+ bias_corr"); enabling
                    // model_corrections.enable_code_phase_bias cancels it back to truth.
                    // 0 unless corrections.signal_bias is set (then row.values is
                    // byte-identical: x + 0.0 == x).  Recorded once per (sat, signal).
                    double bias_code = 0.0, bias_phase = 0.0;
                    if (want_signal_bias) {
                        bias_code  = eph.signalBiasMetres(sat, band.code,  t);
                        bias_phase = eph.signalBiasMetres(sat, band.phase, t);
                        auto record = [&](const std::string& sig, double osb_m) {
                            const std::string key = sat + "|" + sig;
                            if (bias_seen.insert(key).second) {
                                osb_by_key[key] = osb_m;
                                max_abs_osb_m = std::max(max_abs_osb_m, std::abs(osb_m));
                                bias_csv << sat << "," << sig << ","
                                         << osb_m / (Constants::SPEED_OF_LIGHT * 1e-9) << ","
                                         << osb_m << "\n";
                            }
                        };
                        record(band.code,  bias_code);
                        record(band.phase, bias_phase);
                    }
                    // +I on code (group delay), -I on phase (advance); same slant STEC
                    // scaled by 1/f^2 so the dual-frequency IF combination cancels it.
                    // m7 = the M7 full-correction stack (relativity/Shapiro/solid-earth/
                    // pole tide), non-dispersive like tropo; 0 unless enabled in the sim
                    // config.  The processor with the matching corrections ON cancels it.
                    row.values[band.code] = P + tropo + m7 + ant + bias_code + plan.isb_m + I_b + nc + mp_code;
                    row.values[band.phase] =
                        (P + tropo + m7 + ant + bias_phase + plan.isb_m - I_b + band.wavelength * N + np + np_scint + mp_phase) /
                        band.wavelength;
                    if (slip_lli) row.lli[band.phase] = 1; // loss-of-lock flag for flagged slips
                    const std::string akey = sat + "/" + band.phase;
                    if (amb_seen.insert(akey).second) // record the INITIAL integer (pre-slip)
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
                // broadcast Klobuchar AND the true troposphere removed (as a competent
                // single-freq student would), leaving range + code noise + everything
                // Klobuchar cannot take out:
                //   dI_L1            the sub-grid storm overlay (SED/TID), always above it;
                //   sf_iono_residual the injected quiet background minus Klobuchar — zero
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

        // Per-epoch noise-free inversion: must recover this epoch's true position
        // AND the injected ISB (forward model consistency including the ISB).
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

    // Signal-bias payload: the ionosphere-free CODE bias an OSB-ignorant PPP would suffer
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
        // zwd_m is the ARC MEAN when the wet delay varies (the per-epoch series lives in
        // truth_tropo.csv, which is what the Verifier grades); it equals the injected
        // constant when it does not.  Kept so the single-value student grader still works.
        sc << "zwd_m," << (epoch_counter ? zwd_sum / epoch_counter : zwd_true_m) << "\n";
        sc << "zwd_time_varying," << (zwd_varies ? 1 : 0) << "\n";
        if (zwd_varies) {
            sc << "zwd_min_m," << zwd_min_seen << "\n";
            sc << "zwd_max_m," << zwd_max_seen << "\n";
        }
        // Ocean loading.  Emitted only when injected, so OTL-off datasets keep their
        // byte-identical truth_scalars.csv.  Without these the term would be reachable
        // only by opening truth_otl.csv by hand: the student grader reads this file and
        // scenario_summary.json's "truth" object, and neither carried anything about it.
        if (scfg.corrections.ocean_tide) {
            sc << "otl_along_track," << (eph.hasOceanTideAlongTrack() ? 1 : 0) << "\n";
            sc << "otl_peak_up_m," << std::setprecision(6) << otl_peak_abs_inj << "\n";
            // 0 in frozen mode (cancels by construction); the whole point of the
            // along-track field is that this is NOT 0.
            sc << "otl_unmodelled_peak_up_m," << otl_peak_abs_dup << "\n";
        }
        // Signal biases (OSB).  Emitted only when injected, so bias-off datasets keep a
        // byte-identical truth_scalars.csv.  The IF-code range is the payload: the
        // per-GPS-satellite ionosphere-free code bias an OSB-ignorant PPP leaves in.
        if (scfg.corrections.signal_bias) {
            sc << "signal_bias_max_abs_osb_m," << std::setprecision(6) << max_abs_osb_m << "\n";
            sc << "signal_bias_gps_if_code_min_m," << gps_if_osb_min << "\n";
            sc << "signal_bias_gps_if_code_max_m," << gps_if_osb_max << "\n";
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
        js << "  \"signals\": {\"GPS\": [\"C1C\",\"L1C\",\"C2W\",\"L2W\"], \"Galileo\": [\"C1C\",\"L1C\",\"C5Q\",\"L5Q\"]},\n";
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
        js << "    \"quiet_background\": \"" << (eph.hasKlobuchar() ? "broadcast Klobuchar" : "analytic") << "\"\n";
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
        js << "  \"cycle_slips\": {\"count\": " << n_slips << ", \"flagged_lli\": " << n_slips_flagged
           << ", \"unflagged\": " << (n_slips - n_slips_flagged) << ", \"detail\": \"truth_slips.csv\"";
        // The answer-key record of where the scripted gotchas actually landed: one
        // entry per applied repetition, resolved epochs.  Only emitted when scripted
        // events exist, so the no-scripted summary is byte-identical to today (R2).
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
            // Observable-specific biases.  gps_if_code_bias_m is the payload: the range
            // an OSB-ignorant ionosphere-free PPP would be off by, per GPS satellite,
            // because it logs C1C while the precise clock is C1W/C2W-referenced.  It
            // CANCELS in the graded loop (enable_code_phase_bias), so it moves no gate;
            // it is the reason a dataset WITHOUT it biases an OSB-aware external service.
            js << "  \"signal_bias\": {\"source\": \"COD MGX OSB\", \"file\": \"" << bia.filename().string()
               << "\", \"applied\": true, \"n_signals\": " << static_cast<int>(bias_seen.size())
               << ", \"max_abs_osb_m\": " << std::setprecision(6) << max_abs_osb_m
               << ", \"gps_if_code_bias_m\": [" << gps_if_osb_min << ", " << gps_if_osb_max << "]"
               << ", \"clock_ref_obs\": {\"GPS\": \"C1W/C2W\", \"Galileo\": \"C1C/C5Q\"}"
               << ", \"detail\": \"truth_bias.csv\"},\n";
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
        // Formatted through a local stream: cout is left in default float mode here and
        // switching it to std::fixed would silently reformat every line that follows.
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
    std::cout << std::setprecision(3) << "[" << tag << "] marine multipath (Gauss-Markov tau=" << mp_tau_s
              << "s): peak code MP = " << max_mp_code << " m; cycle slips: " << n_slips << " ("
              << n_slips_flagged << " flagged LLI=1, " << (n_slips - n_slips_flagged) << " must-detect)";
    if (n_slips_scripted > 0)
        std::cout << "; scripted: " << n_slips_scripted << " (" << n_slips_scripted_flagged << " flagged)";
    std::cout << "\n";
    if (sf_quiet_n > 0 && sf_storm_n > 0) {
        std::cout << std::setprecision(3) << "[" << tag
                  << "] single-freq SPP (GPS L1 + Klobuchar): quiet 3D mean " << sf_quiet_sum / sf_quiet_n
                  << " m (max " << sf_quiet_max << "), STORM 3D mean " << sf_storm_sum / sf_storm_n
                  << " m (max " << sf_storm_max << ") -- storm biases single-freq, cancels in dual-freq\n";
    }

    int failures = 0;
    // R5: a scripted event that never fired (sat never satisfied its trigger, or the
    // arc ended mid-repeat) is a HARD error — the outputs above are already written,
    // but an exam fixture must never silently not exist.  Name each on stderr.
    for (const std::string& problem : sched.unfinished()) {
        std::cerr << "[" << tag << "] FAIL: " << problem << "\n";
        ++failures;
    }
    // isfinite first: `NaN > 1e-6` is FALSE, so a non-finite STEC would sail through.
    if (!std::isfinite(worst_if_resid) || worst_if_resid > 1.0e-6) {
        std::cerr << "[" << tag << "] FAIL: IF iono cancellation residual " << worst_if_resid
                  << " m exceeds 1 um — per-band iono law/frequencies inconsistent with the IF combination\n";
        ++failures;
    }
    // A dataset that silently mixes a GIM background with a Klobuchar stand-in on some
    // rays is not a GIM dataset, and nothing downstream could tell.
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
    if (worst_isb_clean > 1.0e-3) { // forward model must carry the ISB consistently
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
// for a completed `all` loop.  Mirrors Application::generateReportFor_ exactly:
// python = report override -> baked venv -> PATH; the whole quoted command wrapped
// in an extra pair because Windows std::system pipes through cmd /c (which strips
// the outer quotes).  Non-fatal: a missing pyviz/python only warns.  The last line
// printed is the dashboard path (SPEC §4.7).
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

    // Quote every path (spaces); embed --truth-dir so the exam_* viewers light up;
    // offline interactive report (plotly + inline JS) for the grader archive.
    // --exclude drops viewers that are redundant or unhelpful on the exam dashboard:
    //   exam_otl_truth        (ocean loading is a mm-level error-budget item, not a figure)
    //   plot_satellite_signals(signal-usage strip)
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
    // Pull an optional "--sim-config <path>" (or "--sim-config=<path>") out of the
    // argument list BEFORE positional parsing, so the harness contract
    // (synth-* <outdir> <isb> <storm_scale>) is preserved unchanged.
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
        // Machine-readable verdict for the dashboard (SPEC §4.6) — additive; the
        // printed report above is unchanged.
        const std::filesystem::path repjson = vcfg.run_logs_dir / "verify_report.json";
        if (sim::Verifier::writeJsonReport(res, repjson))
            std::cout << "  wrote " << repjson.string() << "\n";
        return res.pass ? 0 : 1;
    }

    // Resolve the synthesis knobs: struct defaults, then the --sim-config file (if
    // any), then legacy positional overrides for isb_m and storm_scale so existing
    // harness invocations keep working.  The positional default (omitted => 8.0 /
    // 1.0) equals the struct default, so omitting everything reproduces the
    // original synthesis byte-for-byte.
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
    if (cmd == "all") {
        // One-shot loop: synthesize -> run gnss_processor (out-of-process, per-run
        // CWD because its log dir is hardcoded to ./logs) -> verify.  The config's
        // files.observation_path MUST point at <output_dir>'s synthesized RINEX.
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

        std::cout << "[all] (1/3) synthesize (" << mode << ") -> " << outdir.string() << "\n";
        if (runSynth(outdir, mode == "kinematic", scfg) != 0) {
            std::cerr << "[all] synthesis failed\n";
            return 1;
        }

        const std::filesystem::path rundir = outdir / "run";
        std::error_code ec;
        std::filesystem::create_directories(rundir, ec);
        std::cout << "[all] (2/3) gnss_processor " << config.string() << " (cwd " << rundir.string() << ")\n";
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

        std::cout << "[all] (3/3) verify\n";
        sim::VerifierConfig vcfg;
        vcfg.truth_dir = outdir;
        vcfg.run_logs_dir = rundir / "logs";
        const sim::VerifierResult res = sim::Verifier::run(vcfg);
        for (const auto& line : res.report) std::cout << line << "\n";
        sim::Verifier::writeJsonReport(res, vcfg.run_logs_dir / "verify_report.json");

        // Exam dashboard ALWAYS (even on verify FAIL — a failing run needs the
        // dashboard most); the exit code still reflects synth/processor/verify.
        generateExamDashboard(outdir, vcfg.run_logs_dir);
        return res.pass ? 0 : 1;
    }
    std::cerr << "usage: " << (argc > 0 ? argv[0] : "gnss_simulator")
              << " <selftest|probe-orbit|synth-static|synth-kinematic|verify|all> ...\n"
              << "  synth-static|synth-kinematic [output_dir] [isb_m] [storm_scale] [--sim-config <sim.json>]\n"
              << "  verify <truth_dir> <run_logs_dir>\n"
              << "  all <static|kinematic> <output_dir> <config> [processor_exe] [isb_m] [storm_scale] [--sim-config <sim.json>]\n";
    return 2;
}
