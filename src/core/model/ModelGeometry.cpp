#include "ModelGeometry.hpp"
#include "ProductParser.hpp"
#include "Constants.hpp"
#include <algorithm>
#include <cmath>
#include <numeric>

namespace GnssProcessor {

    // Note: initialization order follows member declaration order in the header.
    Geometry::Geometry(const GnssConfig& config, ProductParser& parser, const RinexParser& rinex, LogManager& log_mgr)
        : corrections_(log_mgr, parser.getClocks(), parser.getErps()), // declared first
        config_(config),
        planetary_(log_mgr),
        troposphere_(config, log_mgr),
        product_parser_(parser),
        log_manager_(log_mgr),
        rinex_parser_(rinex),
        elevation_threshold_(config.getElevationMaskDeg()),
        orbits_(), clocks_(), antennas_(), biases_(), erps_(),
        klobuchar_coeffs_(), sun_ephemeris_(), last_interpolated_velocity_(),
        signal_priorities_(config.getSignalPriorities()) {

        // Populate product-dependent containers (SP3, CLK, ATX, BIA, ERP, RNX/Klobuchar)
        loadData();

        log_manager_.getLogger("geometry")->info("Geometry model initialized: {} orbits, {} clocks, elev_mask={:.1f}°",
            orbits_.size(), clocks_.size(), elevation_threshold_);
    }

    void Geometry::loadData() {
        // Pull products from parser into local storage
        // Use ProductParser getters (assumed to exist) to populate orbits_, clocks_, antennas_, biases_, erps_, klobuchar_coeffs_
        try {
            // Orbits (SP3)
            std::vector<OrbitData> parsed_orbits;
            auto sp3_paths = product_parser_.getOrbitPaths();
            if (!sp3_paths.empty()) {
                if (product_parser_.parseSp3(sp3_paths, parsed_orbits)) {
                    orbits_ = std::move(parsed_orbits);
                    log_manager_.getLogger("geometry")->info("Loaded {} orbit entries", orbits_.size());
                }
                else {
                    log_manager_.getLogger("geometry")->warn("Failed to parse SP3 orbit files");
                }
            }
            else {
                log_manager_.getLogger("geometry")->warn("No SP3 product paths provided");
            }

            // Clocks (CLK)
            std::filesystem::path clk_path = product_parser_.getProductPath("CLK");
            if (!clk_path.empty()) {
                std::vector<ClockData> parsed_clocks;
                if (product_parser_.parseClk(clk_path, parsed_clocks)) {
                    clocks_ = std::move(parsed_clocks);
                    log_manager_.getLogger("geometry")->info("Loaded {} clock entries", clocks_.size());
                }
                else {
                    log_manager_.getLogger("geometry")->warn("Failed to parse CLK file {}", clk_path.string());
                }
            }

            // ATX (antenna)
            std::filesystem::path atx_path = product_parser_.getProductPath("ATX");
            if (!atx_path.empty()) {
                std::vector<AntennaData> parsed_ant;
                if (product_parser_.parseAtx(atx_path, parsed_ant)) {
                    antennas_ = std::move(parsed_ant);
                    log_manager_.getLogger("geometry")->info("Loaded {} antenna entries", antennas_.size());
                }
                else {
                    log_manager_.getLogger("geometry")->warn("Failed to parse ATX file {}", atx_path.string());
                }
            }

            // BIA (biases)
            std::filesystem::path bia_path = product_parser_.getProductPath("BIA");
            if (!bia_path.empty()) {
                std::vector<BiasData> parsed_biases;
                if (product_parser_.parseBia(bia_path, parsed_biases)) {
                    biases_ = std::move(parsed_biases);
                    log_manager_.getLogger("geometry")->info("Loaded {} bias entries", biases_.size());
                }
                else {
                    log_manager_.getLogger("geometry")->warn("Failed to parse BIA file {}", bia_path.string());
                }
            }

            // ERP
            std::filesystem::path erp_path = product_parser_.getProductPath("ERP");
            if (!erp_path.empty()) {
                std::vector<ErpData> parsed_erps;
                if (product_parser_.parseErp(erp_path, parsed_erps)) {
                    erps_ = std::move(parsed_erps);
                    log_manager_.getLogger("geometry")->info("Loaded {} ERP entries", erps_.size());
                }
                else {
                    log_manager_.getLogger("geometry")->warn("Failed to parse ERP file {}", erp_path.string());
                }
            }

            // RNX - Klobuchar
            std::filesystem::path rnx_path = product_parser_.getProductPath("RNX");
            if (!rnx_path.empty()) {
                std::vector<double> parsed_klob;
                if (product_parser_.parseKlobuchar(rnx_path, parsed_klob)) {
                    klobuchar_coeffs_ = std::move(parsed_klob);
                    log_manager_.getLogger("geometry")->info("Loaded Klobuchar coefficients ({} values) from {}", klobuchar_coeffs_.size(), rnx_path.string());
                }
                else {
                    log_manager_.getLogger("geometry")->warn("Failed to parse Klobuchar coefficients from {}", rnx_path.string());
                }
            }

            // After loading products, update GnssCorrections internal data if necessary.
            // GnssCorrections currently stores a copy of clocks_/erps_ provided at construction.
            // Update GnssCorrections to use the freshly loaded vectors.
            corrections_.setClocksAndErps(clocks_, erps_);
        }
        catch (const std::exception& e) {
            log_manager_.getLogger("geometry")->error("Exception in Geometry::loadData(): {}", e.what());
        }
    }

    // --- Remaining member function implementations (unchanged) ---
    double Geometry::computeGeometricRange(const Eigen::Vector3d& rx_pos, const Eigen::Vector3d& sat_pos) const {
        return (sat_pos - rx_pos).norm(); // meters
    }

    bool Geometry::computeSatelliteGeometry(const RinexObservation& obs, const PppState& state,
        Eigen::Vector3d& sat_pos,         // [m] satellite ECEF position (at reception time, after rotation)
        Eigen::Vector3d& rx_pos,          // [m] receiver ECEF position (input state)
        double& elevation,                // [deg]
        double& azimuth,                  // [deg]
        Eigen::Vector3d& los,             // unit vector from receiver to satellite
        double& range,                    // [m] geometric distance
        double& tropo_delay,              // modeled tropospheric zenith hydrostatic (dry) delay [m]
        double& sat_clock,                // [s] emission-time clock bias (incl. relativistic corr)
        double& shapiro_delay,            // [m]
        double& sagnac_corr,              // [m]
        double& atm_loading_corr,         // [m]
        double& relativistic_corr,        // [s]
        double& solid_tide,               // [m]
        double& mf_w,                     // [-]
        double& mf_d                      // [-]
    ) const
    {
        const double c = Constants::SPEED_OF_LIGHT;
        const double omegaE = Constants::EARTH_ANGULAR_VELOCITY; // rad/s
        double leap_seconds = config_.getLeapSeconds(); // from config

        // --- Step 1: Validate receiver position ---
        rx_pos = state.position;
        if (rx_pos.norm() < 1e3 || !rx_pos.allFinite()) {
            log_manager_.getLogger("geometry")->warn("Invalid receiver position for {} at TOW {:.3f}",
                obs.satellite, obs.time.tow);
            return false;
        }

        // Initial satellite position (approx. reception time)
        Eigen::Vector3d sat_pos_init = getSatellitePosition(obs.satellite, obs.time);
        if (sat_pos_init.norm() < 1e3 || !sat_pos_init.allFinite()) {
            log_manager_.getLogger("geometry")->warn("Invalid initial satellite position for {} at TOW {:.3f}",
                obs.satellite, obs.time.tow);
            return false;
        }

        // Initial time-of-flight (TOF) guess
        double tof = (sat_pos_init - rx_pos).norm() / c; // [s]
        double tof_delta = 1.0;
        constexpr int max_iter = 5;

        Eigen::Vector3d sat_pos_em, sat_vel_em;
        double sat_clock_em = 0.0;

        // --- Step 2: Iterative TOF refinement ---
        for (int iter = 0; iter < max_iter && tof_delta > 1e-8; ++iter) {
            // Emission time = reception time - TOF
            GnssTime emission_time = obs.time;
            emission_time.tow -= tof;
            if (emission_time.tow < 0) {  // week rollover
                emission_time.tow += 604800.0;
                emission_time.week -= 1;
            }

            // Satellite position/velocity/clock at emission time
            sat_pos_em = getSatellitePosition(obs.satellite, emission_time);   // ECEF [m]
            sat_vel_em = getSatelliteVelocity(obs.satellite, emission_time);   // ECEF [m/s]
            sat_clock_em = corrections_.getSatelliteClockCorrection(obs.satellite, emission_time); // [s]

            if (sat_pos_em.norm() < 1e3 || !sat_pos_em.allFinite() || !std::isfinite(sat_clock_em)) {
                log_manager_.getLogger("geometry")->warn("Invalid satellite data at emission time for {} at TOW {:.3f}",
                    obs.satellite, emission_time.tow);
                return false;
            }

            // --- Step 3: Relativistic correction (applied to satellite clock) ---
            relativistic_corr = corrections_.computeRelativisticCorrection(obs.satellite, sat_pos_em, sat_vel_em); // [s]
            sat_clock = sat_clock_em + relativistic_corr; // [s]

            // --- Step 4: Earth rotation correction (emission → reception) ---
            double theta = omegaE * tof;
            Eigen::Matrix3d R;
            R << cos(theta), -sin(theta), 0,
                sin(theta), cos(theta), 0,
                0, 0, 1;
            sat_pos = R * sat_pos_em; // rotated to reception time

            // --- Step 5: Update TOF ---
            double new_range = (sat_pos - rx_pos).norm(); // [m]
            double new_tof = new_range / c;             // [s]
            tof_delta = std::abs(new_tof - tof);
            tof = new_tof;

            log_manager_.getLogger("geometry")->trace(
                "TOF iteration {} for {}: TOF={:.3e} s, range={:.3f} m, delta={:.3e} s",
                iter, obs.satellite, tof, new_range, tof_delta);
        }

        if (tof_delta > 1e-7) {
            log_manager_.getLogger("geometry")->warn("TOF iteration failed to converge for {}: delta={:.3e} s",
                obs.satellite, tof_delta);
            return false;
        }

        // --- Step 6: Final geometry ---
        range = (sat_pos - rx_pos).norm(); // [m]
        los = (sat_pos - rx_pos) / range;
        Topocentric topo = computeTopocentric(rx_pos, sat_pos);
        elevation = topo.elevation_deg;
        azimuth = topo.azimuth_deg;

        if (elevation < elevation_threshold_) {
            log_manager_.getLogger("geometry")->debug(
                "Satellite {} below elevation threshold: {:.3f} deg < {:.3f} deg",
                obs.satellite, elevation, elevation_threshold_);
            return false;
        }

        // --- Step 7: Propagation corrections ---
        troposphere_.getTroposphereMapping(elevation, mf_d, mf_w);
        double zhd = troposphere_.computeSaastamoinenZHD(rx_pos); // zenith hydrostatic (dry) component
        tropo_delay = zhd;
        shapiro_delay = corrections_.computeShapiroDelay(obs.satellite, rx_pos, sat_pos);
        sagnac_corr = corrections_.computeSagnacCorrection(rx_pos, sat_pos);
        atm_loading_corr = corrections_.computeAtmosphericLoadingCorrection(rx_pos, obs.time);

        double mjd = TimeUtils::gpsTimeToJulianDate(obs.time.week, obs.time.tow);
        double mjd_tt = TimeUtils::gpsMjdToTTMjd(mjd, "GPS", config_.getLeapSeconds()); // Use GPS for all
        Eigen::Vector3d sun_pos = planetary_.computeSunPosition(mjd_tt);
        Eigen::Vector3d moon_pos = planetary_.computeMoonPosition(mjd_tt);
        solid_tide = corrections_.computeSolidEarthTide(rx_pos, sat_pos, sun_pos, moon_pos);

        // --- Diagnostics ---
        log_manager_.getLogger("geometry")->trace(
            "Satellite geometry for {} at TOW {:.3f}: range={:.3f} m, elev={:.3f} deg, az={:.3f} deg, "
            "tropo={:.3f} m, sat_clock={:.3e} s ({:.3f} m), shapiro={:.3e} m, sagnac={:.3f} m, "
            "atm_load={:.3f} m, relativistic={:.3e} s ({:.3f} m), solid_tide={:.3f} m",
            obs.satellite, obs.time.tow, range, elevation, azimuth,
            tropo_delay, sat_clock, sat_clock * c,
            shapiro_delay, sagnac_corr, atm_loading_corr,
            relativistic_corr, relativistic_corr * c, solid_tide);

        return true;
    }

    Topocentric Geometry::computeTopocentric(const Eigen::Vector3d& rx_pos_ecef,
        const Eigen::Vector3d& sat_pos_ecef) const
    {
        Topocentric out;

        Eigen::Vector3d delta = sat_pos_ecef - rx_pos_ecef;
        double range = delta.norm();
        if (range < 1.0) {
            // degenerate range
            log_manager_.getLogger("geometry")->warn("Degenerate range in topocentric computation: range={:.3f} m", range);
            return out;
        }

        // Simple geodetic approximation (sufficient for receiver near Earth's surface)
        double r = rx_pos_ecef.norm();
        if (r < 1e-6) {
            log_manager_.getLogger("geometry")->warn("Invalid receiver ECEF magnitude in topocentric computation");
            return out;
        }
        double lat = std::asin(rx_pos_ecef.z() / r);
        double lon = std::atan2(rx_pos_ecef.y(), rx_pos_ecef.x());

        double slat = std::sin(lat), clat = std::cos(lat);
        double slon = std::sin(lon), clon = std::cos(lon);

        // ECEF -> ENU rotation matrix
        Eigen::Matrix3d R;
        R << -slon, clon, 0.0,
            -slat * clon, -slat * slon, clat,
            clat* clon, clat* slon, slat;

        Eigen::Vector3d los_enu = R * (delta / range);

        // Elevation: asin(up component)
        double up = std::clamp(los_enu.z(), -1.0, 1.0);
        double elev_rad = std::asin(up);
        double elev_deg = elev_rad * 180.0 / Constants::PI;
        elev_deg = std::max(0.0, std::min(90.0, elev_deg)); // clamp to [0,90]

        // Azimuth: atan2(East, North) -> atan2(x, y)
        double az_rad = std::atan2(los_enu.x(), los_enu.y());
        double az_deg = az_rad * 180.0 / Constants::PI;
        if (az_deg < 0.0) az_deg += 360.0;

        out.elevation_deg = elev_deg;
        out.azimuth_deg = az_deg;

        // trace-level logging only
        log_manager_.getLogger("geometry")->trace("Topocentric: elev={:.3f}°, az={:.3f}° (lat≈{:.6f}°)",
            elev_deg, az_deg, lat * 180.0 / Constants::PI);

        return out;
    }

    // Anonymous namespace for file-local utilities
    namespace {
        // Build continuous GPS seconds for sorting/interpolation
        inline double continuousGpsSeconds(const GnssTime& t) {
            return static_cast<double>(t.week) * 604800.0 + t.tow;
        }

        // Barycentric Lagrange weights
        std::vector<double> barycentricWeights(const std::vector<double>& x) {
            size_t n = x.size();
            std::vector<double> w(n, 1.0);
            for (size_t j = 0; j < n; ++j) {
                double prod = 1.0;
                for (size_t k = 0; k < n; ++k) {
                    if (k != j) {
                        double diff = x[j] - x[k];
                        if (std::abs(diff) < 1e-9) { prod = 0.0; break; }
                        prod *= diff;
                    }
                }
                w[j] = (prod == 0.0) ? 0.0 : 1.0 / prod;
            }
            return w;
        }

        // Evaluate barycentric Lagrange polynomial
        template<typename VecType>
        VecType barycentricEval(const std::vector<double>& x,
            const std::vector<double>& w,
            const std::vector<VecType>& y,
            double xq) {
            double num_scalar = 0.0;
            double den_scalar = 0.0;
            VecType num_vec = VecType::Zero();
            for (size_t j = 0; j < x.size(); ++j) {
                double diff = xq - x[j];
                if (std::abs(diff) < 1e-12) {
                    return y[j]; // Exact node
                }
                double term = w[j] / diff;
                num_scalar += term;
                num_vec += term * y[j];
                den_scalar += term;
            }
            // Scalar case: return y if VecType is double; for Eigen::Vector3d we used vector accumulation.
            return num_vec / num_scalar;
        }
    }

    Eigen::Vector3d Geometry::getSatellitePosition(const std::string& satellite, const GnssTime& time) const {
        constexpr size_t MAX_POINTS = 8;     // practical upper bound
        constexpr size_t MIN_POINTS = 4;     // minimum for stable interpolation
        constexpr double MAX_WINDOW = 7200.0; // seconds
        constexpr double MIN_POS_NORM = 1.5e7;
        constexpr double MAX_POS_NORM = 4.0e7;

        if (satellite.empty() || orbits_.empty()) {
            log_manager_.getLogger("geometry")->warn("Position interpolation: invalid satellite or no orbit data");
            return Eigen::Vector3d::Zero();
        }

        double t_query = continuousGpsSeconds(time);
        std::vector<const OrbitData*> pool;
        pool.reserve(orbits_.size());

        for (const auto& o : orbits_) {
            if (o.satellite != satellite || !o.position_valid || !o.position.allFinite()) continue;
            double t_o = continuousGpsSeconds(o.time);
            double dt = std::abs(t_o - t_query);
            if (dt <= MAX_WINDOW &&
                o.position.norm() > MIN_POS_NORM && o.position.norm() < MAX_POS_NORM) {
                pool.push_back(&o);
            }
        }

        if (pool.empty()) {
            log_manager_.getLogger("geometry")->warn("Position interpolation: no candidates for {} at week {} tow {:.3f}",
                satellite, time.week, time.tow);
            return Eigen::Vector3d::Zero();
        }

        // Sort by proximity
        std::sort(pool.begin(), pool.end(), [t_query](const OrbitData* a, const OrbitData* b) {
            double da = std::abs(continuousGpsSeconds(a->time) - t_query);
            double db = std::abs(continuousGpsSeconds(b->time) - t_query);
            return da < db;
            });

        size_t useN = std::min(MAX_POINTS, pool.size());
        if (useN < MIN_POINTS) {
            // Fallback to nearest
            log_manager_.getLogger("geometry")->debug("Position interpolation: fallback nearest ({} points < {})", useN, MIN_POINTS);
            return pool[0]->position;
        }

        // Choose symmetric set: take half before and half after when possible
        // Already sorted by proximity, but enforce chronological ordering for interpolation nodes
        std::vector<const OrbitData*> chosen(pool.begin(), pool.begin() + useN);
        std::sort(chosen.begin(), chosen.end(), [](const OrbitData* a, const OrbitData* b) {
            double ta = continuousGpsSeconds(a->time);
            double tb = continuousGpsSeconds(b->time);
            return ta < tb;
            });

        std::vector<double> x; x.reserve(useN);
        std::vector<Eigen::Vector3d> y; y.reserve(useN);
        for (const auto* od : chosen) {
            x.push_back(continuousGpsSeconds(od->time));
            y.push_back(od->position);
        }

        auto w = barycentricWeights(x);
        Eigen::Vector3d interp = barycentricEval(x, w, y, t_query);

        if (!interp.allFinite()) {
            log_manager_.getLogger("geometry")->warn("Position interpolation produced non-finite for {} - fallback nearest", satellite);
            return pool[0]->position;
        }

        log_manager_.getLogger("geometry")->trace("Position interpolation {}: used {} points, span {:.1f}s",
            satellite, useN, x.back() - x.front());
        return interp;
    }

    Eigen::Vector3d Geometry::getSatelliteVelocity(const std::string& satellite, const GnssTime& time) const {
        constexpr size_t MAX_POINTS = 6;
        constexpr size_t MIN_POINTS = 3;
        constexpr double MAX_WINDOW = 3600.0;
        constexpr double MAX_VEL_NORM = 15000.0;

        if (satellite.empty() || orbits_.empty()) {
            log_manager_.getLogger("geometry")->warn("Velocity interpolation: invalid satellite or no orbit data");
            return Eigen::Vector3d::Zero();
        }

        double t_query = continuousGpsSeconds(time);
        std::vector<const OrbitData*> pool;
        for (const auto& o : orbits_) {
            if (o.satellite != satellite || !o.position_valid || !o.position.allFinite()) continue;
            double dt = std::abs(continuousGpsSeconds(o.time) - t_query);
            if (dt <= MAX_WINDOW) pool.push_back(&o);
        }
        if (pool.empty()) {
            log_manager_.getLogger("geometry")->warn("Velocity interpolation: no candidates for {} at week {} tow {:.3f}", satellite, time.week, time.tow);
            return Eigen::Vector3d::Zero();
        }

        std::sort(pool.begin(), pool.end(), [t_query](const OrbitData* a, const OrbitData* b) {
            double da = std::abs(continuousGpsSeconds(a->time) - t_query);
            double db = std::abs(continuousGpsSeconds(b->time) - t_query);
            return da < db;
            });

        size_t useN = std::min(MAX_POINTS, pool.size());
        std::vector<const OrbitData*> chosen(pool.begin(), pool.begin() + useN);
        std::sort(chosen.begin(), chosen.end(), [](const OrbitData* a, const OrbitData* b) {
            return continuousGpsSeconds(a->time) < continuousGpsSeconds(b->time);
            });

        bool allVelValid = true;
        for (auto* od : chosen) {
            if (!od->velocity_valid || !od->velocity.allFinite() || od->velocity.norm() > MAX_VEL_NORM) {
                allVelValid = false; break;
            }
        }

        // If velocities valid: barycentric interpolate velocities
        if (allVelValid && useN >= MIN_POINTS) {
            std::vector<double> x; x.reserve(useN);
            std::vector<Eigen::Vector3d> v; v.reserve(useN);
            for (auto* od : chosen) {
                x.push_back(continuousGpsSeconds(od->time));
                v.push_back(od->velocity);
            }
            auto w = barycentricWeights(x);
            Eigen::Vector3d vel = barycentricEval(x, w, v, t_query);
            if (vel.allFinite()) {
                log_manager_.getLogger("geometry")->trace("Velocity interpolation {}: barycentric on {} velocity nodes", satellite, useN);
                return vel;
            }
        }

        // Fallback: finite-difference on positions
        if (useN >= MIN_POINTS) {
            // Simple central difference across closest trio
            if (useN >= 3) {
                // pick middle triple
                size_t mid = useN / 2;
                if (mid == 0) mid = 1;
                size_t i0 = mid - 1;
                size_t i2 = std::min(mid + 1, useN - 1);
                double t0 = continuousGpsSeconds(chosen[i0]->time);
                double t2 = continuousGpsSeconds(chosen[i2]->time);
                double dt = t2 - t0;
                if (dt > 0.5) {
                    Eigen::Vector3d vel = (chosen[i2]->position - chosen[i0]->position) / dt;
                    if (vel.allFinite()) {
                        log_manager_.getLogger("geometry")->trace("Velocity interpolation {}: central difference dt={:.1f}s", satellite, dt);
                        return vel;
                    }
                }
            }
        }

        // Final fallback
        log_manager_.getLogger("geometry")->debug("Velocity interpolation fallback nearest for {}", satellite);
        return chosen.front()->velocity_valid ? chosen.front()->velocity : Eigen::Vector3d::Zero();
    }

    void Geometry::computeSatelliteAttitude(System system,
        const Eigen::Vector3d& sat_pos,
        const Eigen::Vector3d& sat_vel,
        Eigen::Vector3d& d_s,
        Eigen::Vector3d& y_s,
        const GnssTime& time) const {
        // Get Sun position
        Eigen::Vector3d sun_pos = planetary_.interpolateSunPosition(time);
        if (sun_pos.norm() < 1e6) { // Fallback if no ephemeris
            sun_pos = Eigen::Vector3d(1.496e11, 0.0, 0.0); // 1 AU along x-axis
            log_manager_.getLogger("geometry")->warn("No Sun ephemeris for time TOW {0}", time.tow);
        }
        Eigen::Vector3d e = (sun_pos - sat_pos).normalized(); // Sun direction

        // Compute yaw-steering attitude
        Eigen::Vector3d y = sat_vel.normalized().cross(e);
        if (y.norm() < 1e-6) { // Handle collinearity
            y = Eigen::Vector3d(0.0, 1.0, 0.0); // Arbitrary fallback
        }
        y_s = y.normalized();
        Eigen::Vector3d z_s = -sat_pos.normalized();
        d_s = y_s.cross(z_s).normalized();
    }

} // namespace GnssProcessor