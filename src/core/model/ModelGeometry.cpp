#include "ModelGeometry.hpp"
#include "ProductParser.hpp"
#include "Constants.hpp"
#include "Coordinates.hpp"
#include "ProductCache.hpp"
#include "Interpolate.hpp"
#include <algorithm>
#include <cmath>
#include <numeric>
#include <unordered_map>

namespace GnssProcessor {

    namespace Model {

        // Anonymous namespace for file-local utilities
        namespace {

            /**
             * @brief Compute barycentric Lagrange interpolation weights for nodes `x`.
             *
             * Given a vector of node abscissas `x = {x0, x1, ..., x_{n-1}}` this routine
             * returns barycentric weights `w` suitable for use in the barycentric
             * interpolation formula:
             *    p(xq) = ( sum_j w[j] * y[j] / (xq - x[j]) ) / ( sum_j w[j] / (xq - x[j]) )
             *
             * The implementation computes the product of (x[j] - x[k]) for k != j in
             * log-space to avoid overflow/underflow when x values are large (for example
             * continuous GPS seconds on the order of 1e9). The sign of the product is
             * tracked separately. Extremely large or small log-products are clamped to
             * prevent numeric exceptions; if any two nodes are closer than 1e-9, the
             * corresponding weight is treated as invalid and left zero.
             *
             * @param x Vector of node x-coordinates. Values must be distinct for meaningful weights.
             * @return Vector of barycentric weights of the same length as `x`.
             *         If `x` is empty an empty vector is returned. If `x` has one element
             *         a vector `{1.0}` is returned.
             */
            std::vector<double> barycentricWeights(const std::vector<double>& x) {
                size_t n = x.size();
                if (n == 0) return {};
                if (n == 1) return { 1.0 };

                std::vector<double> w(n, 0.0);

                // Compute weights in log-space to avoid overflow/underflow
                for (size_t j = 0; j < n; ++j) {
                    double log_abs_prod = 0.0;
                    int sign = 1;
                    bool valid = true;

                    for (size_t k = 0; k < n; ++k) {
                        if (k != j) {
                            double diff = x[j] - x[k];
                            if (std::abs(diff) < 1e-9) {
                                valid = false;
                                break;
                            }
                            log_abs_prod += std::log(std::abs(diff));
                            if (diff < 0) sign = -sign;
                        }
                    }

                    if (valid) {
                        // w[j] = 1 / prod = sign * exp(-log_abs_prod)
                        // Clamp to prevent extreme values
                        if (log_abs_prod > 700.0) {
                            w[j] = 0.0; // Underflow: product too large, weight negligible
                        }
                        else if (log_abs_prod < -700.0) {
                            w[j] = sign * 1e300; // Overflow protection
                        }
                        else {
                            w[j] = sign * std::exp(-log_abs_prod);
                        }
                    }
                }
                return w;
            }

            // Evaluate barycentric Lagrange polynomial
            template<typename VecType>
            VecType barycentricEval(const std::vector<double>& x,
                const std::vector<double>& w,
                const std::vector<VecType>& y,
                double xq) {
                double den = 0.0;
                VecType num = VecType::Zero();
                for (size_t j = 0; j < x.size(); ++j) {
                    double diff = xq - x[j];
                    if (std::abs(diff) < 1e-12) {
                        return y[j]; // Exact node
                    }
                    double term = w[j] / diff;
                    num += term * y[j];
                    den += term;
                }
                return (den == 0.0) ? num : (num / den);
            }
        } // namespace

        /**
         * @brief Construct the Geometry helper used for satellite/receiver geometry and corrections.
         *
         * This constructor initializes internal models and product-dependent containers
         * required by the PPP geometry routines (orbits, clocks, antenna models, ERP, etc.).
         * Heavy product loading is performed by the private `loadData()` method which is
         * invoked from the constructor implementation.
         *
         * @param config Read-only processing configuration providing masks, product paths and options.
         * @param parser ProductParser used to locate and parse auxiliary products (SP3, CLK, ATX, BIA, ERP, RNX, ...).
         * @param rinex Reference to the parsed RINEX data (observations/header metadata) required by some loaders.
         * @param log_mgr LogManager used for diagnostic, warning and error messages.
         *
         * @note Member initialization order follows the declaration order in the header;
         *       the constructor delegates product parsing to `loadData()` to avoid inline
         *       initialization ordering issues and to centralize error handling.
         */
        Geometry::Geometry(const GnssConfig& config, ProductCache& cache,
            const RinexParser& rinex, LogManager& log_mgr)
            : corrections_(log_mgr, cache.getClocks(), cache.getErps()),
            config_(config),
            planetary_(log_mgr),
            troposphere_(config, log_mgr),
            product_cache_(cache),
            log_manager_(log_mgr),
            rinex_parser_(rinex),
            elevation_threshold_(config.getElevationMaskDeg()),
            signal_priorities_(config.getSignalPriorities()),
            // Initialize references from cache - NO COPIES
            orbits_(cache.getOrbits()),
            clocks_(cache.getClocks()),
            antennas_(cache.getAntennas()),
            biases_(cache.getBiases()),
            erps_(cache.getErps()),
            klobuchar_coeffs_(cache.getKlobucharCoeffs())
        {
            // No need to copy data - we now hold references to cache data

            // Update GnssCorrections to use cache references directly
            //corrections_.setClocksAndErps(cache.getClocks(), cache.getErps());

            log_manager_.getLogger("geometry")->info(
                "Geometry model initialized (zero-copy): {} orbits, {} clocks, elev_mask={:.1f}°",
                orbits_.size(), clocks_.size(), elevation_threshold_
            );
            // Build indices from the cached product vectors so lookups are O(1)
            loadData();
        }

        /**
         * @brief Load auxiliary GNSS product and model data into Geometry internal containers.
         *
         * Populates the member containers used by geometry and correction routines:
         * - `orbits_` (SP3 orbit entries)
         * - `clocks_` (CLK satellite clocks)
         * - `antennas_` (ATX antenna models)
         * - `biases_` (BIA biases)
         * - `erps_` (ERP entries)
         * - `klobuchar_coeffs_` (RINEX Klobuchar coefficients)
         *
         * The method queries `product_parser_` for product paths and calls the
         * corresponding parse functions. Successfully parsed data is moved into the
         * member vectors; parsing failures generate warnings and leave previous data
         * unchanged where appropriate. After loading, `corrections_.setClocksAndErps`
         * is invoked to update internal correction models.
         *
         * All parsing errors and unexpected exceptions are logged via `log_manager_`.
         * This function catches exceptions thrown by parsers and logs them as errors;
         * it does not propagate exceptions.
         *
         * @note Called from the constructor to initialize product-dependent data.
         */
        void Geometry::loadData() {
            log_manager_.getLogger("geometry")->debug("Begin satellite orbit index");

            orbit_index_.clear();

            // Build orbit index: satellite -> vector of pointers to OrbitData
            for (const OrbitData& od : orbits_) {
                if (od.satellite.empty()) continue;
                orbit_index_[od.satellite].push_back(&od);
            }

            // Sort per-satellite entries by time (ascending) to make interpolation/search predictable
            for (auto& kv : orbit_index_) {
                auto& vec = kv.second;
                std::sort(vec.begin(), vec.end(), [](const OrbitData* a, const OrbitData* b) {
                    return TimeUtils::continuousGpsSeconds(a->time) < TimeUtils::continuousGpsSeconds(b->time);
                    });
            }

            log_manager_.getLogger("geometry")->info("Built orbit index for {} satellites", orbit_index_.size());
        }

        /**
         * @brief Compute geometric range between receiver and satellite.
         *
         * Computes the Euclidean distance between receiver and satellite ECEF
         * coordinates using a straightforward vector norm:
         *   range = ||sat_pos - rx_pos||
         *
         * @param rx_pos Receiver ECEF position (meters).
         * @param sat_pos Satellite ECEF position (meters).
         * @return Geometric range in meters.
         *
         * @note This function does not apply Earth rotation, relativistic, or other
         *       propagation corrections — it returns the straight-line distance in ECEF.
         */
        double Geometry::computeGeometricRange(const Eigen::Vector3d& rx_pos, const Eigen::Vector3d& sat_pos) const {
            return (sat_pos - rx_pos).norm(); // meters
        }

        /**
         * @brief Compute satellite geometry and apply standard propagation corrections for an observation.
         *
         * Performs the end-to-end geometry computation required by the PPP observation model:
         * - Interpolates satellite PVT at the signal emission epoch;
         * - Iterates Time-Of-Flight (TOF) to account for signal travel and Earth rotation;
         * - Applies satellite clock (CLK) and relativistic periodic corrections;
         * - Rotates emission-time satellite position to reception-time ECEF;
         * - Computes line-of-sight, geometric range, elevation and azimuth (degrees);
         * - Computes troposphere mapping factors, zenith hydrostatic delay (ZHD), Shapiro delay,
         *   solid Earth tide and other diagnostic corrections (atmospheric loading, Sagnac, ...).
         *
         * @param[in]  obs               RINEX observation record containing satellite id and epoch.
         * @param[in]  state             Current PPP state (receiver position, troposphere/state variables, ...).
         * @param[out] sat_pos           Satellite ECEF position at reception-time (meters).
         * @param[out] rx_pos            Receiver ECEF position (meters).
         * @param[out] elevation         Satellite elevation at receiver in degrees.
         * @param[out] azimuth           Satellite azimuth at receiver in degrees (0..360).
         * @param[out] los               Line‑of‑sight unit vector from receiver to satellite (ECEF).
         * @param[out] range             Geometric range between receiver and satellite (meters).
         * @param[out] tropo_delay       Total tropospheric delay (meters) (ZHD and mapping applied as implemented).
         * @param[out] sat_clock         Total satellite clock correction (seconds) (CLK value + relativistic periodic term).
         * @param[out] shapiro_delay     Shapiro gravitational path delay (meters).
         * @param[out] sagnac_corr       Sagnac correction (meters). When emission-time rotation is applied this is implicitly included (returned for diagnostics).
         * @param[out] atm_loading_corr  Atmospheric loading correction (meters).
         * @param[out] relativistic_corr Relativistic periodic correction applied to satellite clock (seconds).
         * @param[out] solid_tide        Solid Earth tide radial correction (meters).
         * @param[out] mf_w              Tropospheric wet mapping factor (unitless).
         * @param[out] mf_d              Tropospheric dry/hydrostatic mapping factor (unitless).
         * @param[out] sat_vel           Satellite ECEF velocity at emission time (meters/second).
         * @return True on success; false if geometry/corrections could not be computed
         *         (invalid receiver/satellite data, insufficient orbit/clock nodes, below elevation mask, or non-convergence).
         */
        bool Geometry::computeSatelliteGeometry(const RinexObservation& obs, const PppState& state,
            Eigen::Vector3d& sat_pos,
            Eigen::Vector3d& rx_pos,
            double& elevation,
            double& azimuth,
            Eigen::Vector3d& los,
            double& range,
            double& tropo_delay,
            double& sat_clock,
            double& shapiro_delay,
            double& sagnac_corr,
            double& atm_loading_corr,
            double& relativistic_corr,
            double& solid_tide,
            double& mf_w,
            double& mf_d,
            Eigen::Vector3d& sat_vel) const
        {
			// TODO: Use health flags from BRDC/SP3/CLK to reject unhealthy satellites
            // Requires loading health data into cache and exposing it here.
			// For now, we can hardcode a known unhealthy satellite (launched into wrong orbit).
            if (obs.satellite == "E14") return false;

            const double c = Constants::SPEED_OF_LIGHT;
            const double omegaE = Constants::EARTH_ANGULAR_VELOCITY; // rad/s

            // --- Step 1: Validate receiver position ---
            rx_pos = state.position;
            if (rx_pos.norm() < 1e3 || !rx_pos.allFinite()) {
                log_manager_.getLogger("geometry")->warn("Invalid receiver position for {} at TOW {:.3f}",
                    obs.satellite, obs.time.tow);
                return false;
            }

            // Initial satellite position (approx. reception time)
            Eigen::Vector3d sat_pos_init = getSatellitePV(obs.satellite, obs.time).first;
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
            // 
            // The emission time computation follows the standard PPP approach:
            //   t_emission = t_reception - TOF
            //
            // Important: SP3 orbits and CLK files are tabulated in GPS system time.
            // The satellite clock correction from CLK files gives the offset:
            //   t_satellite_proper = t_GPS_system - dt_sat
            //
            // For orbit lookup, we use GPS system time (emission_time).
            // The satellite clock correction is applied to the pseudorange equation.
            //
            double d_tof = 1e-16;
            for (int iter = 0; iter < max_iter && tof_delta > d_tof; ++iter) {
                // Emission time = reception time - TOF (in GPS system time)
                GnssTime emission_time = obs.time;
                emission_time.tow -= tof;
                if (emission_time.tow < 0) { emission_time.tow += TimeUtils::SECONDS_IN_WEEK; emission_time.week -= 1; }

                // Get satellite clock correction at emission time.
                // This is dt_sat such that: t_satellite_proper = t_GPS - dt_sat
                // A positive dt_sat means the satellite clock is ahead of GPS time.
                sat_clock_em = corrections_.getSatelliteClockCorrection(obs.satellite, emission_time); // [s]

                // Reject observations with failed clock interpolation.
                // A zero clock typically indicates missing clock data (interpolation returned 0.0 as fallback).
                // Valid satellite clocks are typically in the range of microseconds to milliseconds,
                // so |sat_clock| < 1e-12 s (1 picosecond) is almost certainly invalid.
                if (std::abs(sat_clock_em) < 1e-12) {
                    log_manager_.getLogger("geometry")->warn(
                        "Satellite clock correction is zero/invalid for {} at emission TOW {:.3f} - skipping observation",
                        obs.satellite, emission_time.tow);
                    return false;
                }

                // For orbit interpolation, we need the satellite position at the GPS system time
                // when the signal was transmitted. This is emission_time (already in GPS system time).
                // We do NOT adjust orbit lookup by sat_clock - the SP3 is in GPS system time.
                auto [sat_pos_em_new, sat_vel_em_new] = getSatellitePV(obs.satellite, emission_time);
                sat_pos_em = sat_pos_em_new;
                sat_vel_em = sat_vel_em_new;

                if (sat_pos_em.norm() < 1e3 || !sat_pos_em.allFinite() || !std::isfinite(sat_clock_em)) {
                    log_manager_.getLogger("geometry")->warn("Invalid satellite data at emission time for {} at TOW {:.3f}",
                        obs.satellite, emission_time.tow);
                    return false;
                }

                // Relativistic correction (applied to satellite clock)
                // This is the periodic relativistic effect: dtr = -2(r·v)/c²
                relativistic_corr = corrections_.computeRelativisticCorrection(obs.satellite, sat_pos_em, sat_vel_em);

                // Total satellite clock correction (CLK file value + relativistic periodic term)
                sat_clock = sat_clock_em + relativistic_corr;
                //sat_clock = sat_clock_em;

                // --- Earth rotation correction ---
                // Rotate satellite position from emission-time ECEF to reception-time ECEF.
                // This accounts for Earth's rotation during signal propagation.
                // The Sagnac effect is implicitly included when using the rotated position.
                double theta = omegaE * tof;
                double cos_theta = std::cos(theta);
                double sin_theta = std::sin(theta);
                Eigen::Matrix3d R_earth;
                R_earth << cos_theta, sin_theta, 0.0,
                    -sin_theta, cos_theta, 0.0,
                    0.0, 0.0, 1.0;
                sat_pos = R_earth * sat_pos_em; // Now in reception-time ECEF frame

                // Update TOF based on geometric range in reception-time frame
                double new_range = (sat_pos - rx_pos).norm();
                double new_tof = new_range / c;
                tof_delta = std::abs(new_tof - tof);
                tof = new_tof;
            }

            if (tof_delta > d_tof) {
                log_manager_.getLogger("geometry")->warn("TOF iteration failed to converge for {}: delta={:.3e} s",
                    obs.satellite, tof_delta);
                return false;
            }

            // Diagnostic: Log final emission time and satellite PVT details
            {
                GnssTime emission_time = obs.time;
                emission_time.tow -= tof;
                if (emission_time.tow < 0) { emission_time.tow += TimeUtils::SECONDS_IN_WEEK; emission_time.week -= 1; }

                // Compute r·v for relativistic check
                double r_dot_v = sat_pos_em.dot(sat_vel_em);

                log_manager_.getLogger("geometry")->debug(
                    "Geometry {} at rx_tow={:.3f}: emission_tow={:.3f}, tof={:.6f}s ({:.3f}m), "
                    "sat_clk_raw={:.3e}s, rel_corr={:.3e}s, sat_clk_total={:.3e}s ({:.3f}m), "
                    "r_dot_v={:.3e}, vel_norm={:.1f}m/s",
                    obs.satellite, obs.time.tow, emission_time.tow, tof, tof * c,
                    sat_clock_em, relativistic_corr, sat_clock, sat_clock * c,
                    r_dot_v, sat_vel_em.norm());
            }

            // --- Final geometry (all in reception-time ECEF frame) ---
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

            // --- Propagation corrections ---
            troposphere_.getTroposphereMapping(elevation, mf_d, mf_w);
            double zhd = troposphere_.computeSaastamoinenZHD(rx_pos); // zenith hydrostatic (dry) component
            tropo_delay = zhd;
            shapiro_delay = corrections_.computeShapiroDelay(obs.satellite, rx_pos, sat_pos);

            // Sagnac correction is zero when using rotated satellite position (already accounted for)
            // But we still compute it for logging/diagnostic purposes
            sagnac_corr = 0.0; // Implicitly included in Earth rotation
            //sagnac_corr = corrections_.computeSagnacCorrection(rx_pos, sat_pos); // Implicitly included in Earth rotation

            atm_loading_corr = 0.0; // placeholder

            double mjd = TimeUtils::gpsTimeToJulianDate(obs.time.week, obs.time.tow);
            double mjd_tt = TimeUtils::gpsMjdToTTMjd(mjd, "GPS", config_.getLeapSeconds());
            Eigen::Vector3d sun_pos = planetary_.computeSunPosition(mjd_tt);
            Eigen::Vector3d moon_pos = planetary_.computeMoonPosition(mjd_tt);
            solid_tide = corrections_.computeSolidEarthTide(rx_pos, sat_pos, sun_pos, moon_pos);

            // Output emission-time velocity (for relativistic correction, phase wind-up, etc.)
            sat_vel = sat_vel_em;

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

        /**
        * @brief Compute satellite position and velocity together efficiently.
        *
        * This combined function performs the index lookup, node filtering, and sorting
        * only ONCE, then evaluates the interpolant at three points (t, t+h, t-h) to
        * get both position and velocity.
        *
        * Performance: ~3x faster than calling getSatellitePosition + getSatelliteVelocity
        * separately, since we avoid redundant index lookups and sorting.
        *
        * @param[in] satellite Satellite identifier string (e.g. "G01", "E11").
        * @param[in] time      Epoch at which position/velocity are requested.
        * @return Pair of {position (m), velocity (m/s)}. Returns {Zero, Zero} on failure.
        */
        std::pair<Eigen::Vector3d, Eigen::Vector3d> Geometry::getSatellitePV(
            const std::string& satellite, const GnssTime& time) const
        {
            constexpr size_t IGS_POINTS = 9;
            constexpr size_t MIN_POINTS = 4;
            constexpr double MAX_SPAN = 7200.0;
            constexpr double SP3_INTERVAL = 900.0;
            constexpr double MIN_POS_NORM = 1.5e7;
            constexpr double MAX_POS_NORM = 4.0e7;
            constexpr double MAX_VEL_NORM = 15000.0;

            const auto zero_result = std::make_pair(Eigen::Vector3d::Zero(), Eigen::Vector3d::Zero());

            if (satellite.empty()) {
                log_manager_.getLogger("geometry")->warn("getSatellitePV: empty satellite ID");
                return zero_result;
            }

            // O(1) index lookup
            auto it = orbit_index_.find(satellite);
            if (it == orbit_index_.end()) {
                log_manager_.getLogger("geometry")->warn("getSatellitePV: no orbit data for {}", satellite);
                return zero_result;
            }

            const auto& sat_orbits = it->second;  // Already sorted chronologically in loadData()
            const double t_query = TimeUtils::continuousGpsSeconds(time);

            // Binary search for the first node >= t_query
            auto upper_it = std::lower_bound(sat_orbits.begin(), sat_orbits.end(), t_query,
                [](const OrbitData* o, double t) {
                    return TimeUtils::continuousGpsSeconds(o->time) < t;
                });

            size_t upper_idx = std::distance(sat_orbits.begin(), upper_it);

            // Center the window around the query time
            // For IGS_POINTS=9: half_before=4, so window is [upper_idx-4, upper_idx+4]
            size_t half_before = IGS_POINTS / 2;

            size_t start_idx = 0;
            if (upper_idx > half_before) {
                start_idx = upper_idx - half_before;
            }
            // Clamp to valid range
            if (start_idx + IGS_POINTS > sat_orbits.size()) {
                start_idx = sat_orbits.size() > IGS_POINTS ? sat_orbits.size() - IGS_POINTS : 0;
            }
            size_t end_idx = std::min(start_idx + IGS_POINTS, sat_orbits.size());

            // Collect valid nodes from the fixed window
            std::vector<const OrbitData*> nodes;
            nodes.reserve(IGS_POINTS);

            for (size_t i = start_idx; i < end_idx; ++i) {
                const OrbitData* o = sat_orbits[i];
                if (!o->position_valid || !o->position.allFinite()) continue;
                double norm = o->position.norm();
                if (norm <= MIN_POS_NORM || norm >= MAX_POS_NORM) continue;
                nodes.push_back(o);
            }

            if (nodes.size() < MIN_POINTS) {
                log_manager_.getLogger("geometry")->warn(
                    "getSatellitePV: insufficient valid nodes ({}) for {} at week {} tow {:.3f}",
                    nodes.size(), satellite, time.week, time.tow);
                return zero_result;
            }

            // Nodes are already in chronological order (from sorted sat_orbits)

            // === CHECK FOR EXTRAPOLATION ===
            const double t_first = TimeUtils::continuousGpsSeconds(nodes.front()->time);
            const double t_last = TimeUtils::continuousGpsSeconds(nodes.back()->time);

            if (t_query < t_first || t_query > t_last) {
                const double extrap_dist = (t_query < t_first) ? (t_first - t_query) : (t_query - t_last);
                if (extrap_dist > SP3_INTERVAL / 2.0) {
                    log_manager_.getLogger("geometry")->warn(
                        "Position extrapolation for {} at week {} tow {:.3f}: {:.1f}s outside SP3 window [{:.1f}, {:.1f}]s",
                        satellite, time.week, time.tow, extrap_dist,
                        nodes.front()->time.tow, nodes.back()->time.tow);
                }
            }

            // === CHECK FOR DATA GAPS ===
            const double span = t_last - t_first;
            if (span > MAX_SPAN) {
                // Re-select with minimum points closest to query
                std::sort(nodes.begin(), nodes.end(), [t_query](const OrbitData* a, const OrbitData* b) {
                    return std::abs(TimeUtils::continuousGpsSeconds(a->time) - t_query) <
                        std::abs(TimeUtils::continuousGpsSeconds(b->time) - t_query);
                    });
                if (nodes.size() > MIN_POINTS) {
                    nodes.resize(MIN_POINTS);
                }
                std::sort(nodes.begin(), nodes.end(), [](const OrbitData* a, const OrbitData* b) {
                    return TimeUtils::continuousGpsSeconds(a->time) < TimeUtils::continuousGpsSeconds(b->time);
                    });
            }

            // === BUILD INTERPOLATION ARRAYS ===
            const size_t n = nodes.size();
            std::vector<double> x(n);
            std::vector<double> yx(n), yy(n), yz(n);
            for (size_t i = 0; i < n; ++i) {
                x[i] = TimeUtils::continuousGpsSeconds(nodes[i]->time);
                yx[i] = nodes[i]->position.x();
                yy[i] = nodes[i]->position.y();
                yz[i] = nodes[i]->position.z();
            }

            Eigen::Vector3d pos = Eigen::Vector3d::Zero();
            Eigen::Vector3d vel = Eigen::Vector3d::Zero();

            // Position interpolation uses barycentric Lagrange interpolation
            auto w = Interpolate::barycentricWeights(x);
            pos.x() = Interpolate::barycentricEval(x, w, yx, t_query);
            pos.y() = Interpolate::barycentricEval(x, w, yy, t_query);
            pos.z() = Interpolate::barycentricEval(x, w, yz, t_query);

            // Velocity computation depends on interpolation mode
            try {
                SatelliteInterpolationMode mode = config_.getSatelliteInterpolationMode();

                if (mode == SatelliteInterpolationMode::Lagrange) {
                    vel.x() = Interpolate::barycentricDeriv(x, w, yx, t_query);
                    vel.y() = Interpolate::barycentricDeriv(x, w, yy, t_query);
                    vel.z() = Interpolate::barycentricDeriv(x, w, yz, t_query);
                }
                else if (mode == SatelliteInterpolationMode::Hermite) {
                    auto mx = Interpolate::monotoneHermiteSlopes(x, yx);
                    auto my = Interpolate::monotoneHermiteSlopes(x, yy);
                    auto mz = Interpolate::monotoneHermiteSlopes(x, yz);

                    if (mx.size() == n && my.size() == n && mz.size() == n) {
                        vel.x() = Interpolate::hermiteDeriv(x, yx, mx, t_query);
                        vel.y() = Interpolate::hermiteDeriv(x, yy, my, t_query);
                        vel.z() = Interpolate::hermiteDeriv(x, yz, mz, t_query);
                    }
                    else {
                        log_manager_.getLogger("geometry")->warn(
                            "getSatellitePV: Hermite slopes computation failed for {}", satellite);
                        return zero_result;
                    }
                }
                else if (mode == SatelliteInterpolationMode::Spline) {
                    auto cx = Interpolate::naturalSplineSecondDerivs(x, yx);
                    auto cy = Interpolate::naturalSplineSecondDerivs(x, yy);
                    auto cz = Interpolate::naturalSplineSecondDerivs(x, yz);

                    if (cx.size() == n && cy.size() == n && cz.size() == n) {
                        vel.x() = Interpolate::splineDeriv(x, yx, cx, t_query);
                        vel.y() = Interpolate::splineDeriv(x, yy, cy, t_query);
                        vel.z() = Interpolate::splineDeriv(x, yz, cz, t_query);
                    }
                    else {
                        log_manager_.getLogger("geometry")->warn(
                            "getSatellitePV: Spline coefficients computation failed for {}", satellite);
                        return zero_result;
                    }
                }
                else {
                    log_manager_.getLogger("geometry")->error(
                        "getSatellitePV: unknown interpolation mode for {}", satellite);
                    return zero_result;
                }
            }
            catch (const std::exception& ex) {
                log_manager_.getLogger("geometry")->warn(
                    "getSatellitePV: exception during interpolation for {}: {}", satellite, ex.what());
                return zero_result;
            }

            // Validate position
            if (!pos.allFinite()) {
                log_manager_.getLogger("geometry")->warn(
                    "getSatellitePV: non-finite position for {} at tow {:.3f}", satellite, time.tow);
                return zero_result;
            }

            double pos_norm = pos.norm();
            if (pos_norm <= MIN_POS_NORM || pos_norm >= MAX_POS_NORM) {
                log_manager_.getLogger("geometry")->warn(
                    "getSatellitePV: position out of range for {} at tow {:.3f}: norm={:.1f} m",
                    satellite, time.tow, pos_norm);
                return zero_result;
            }

            // Validate velocity
            if (!vel.allFinite() || vel.norm() > MAX_VEL_NORM) {
                log_manager_.getLogger("geometry")->warn(
                    "getSatellitePV: invalid velocity for {} at tow {:.3f}: norm={:.1f} m/s",
                    satellite, time.tow, vel.norm());
                vel = Eigen::Vector3d::Zero();
            }

            log_manager_.getLogger("geometry")->debug(
                "getSatellitePV for {} at tow={:.3f}: pos=({:.3f}, {:.3f}, {:.3f}) km, "
                "vel=({:.3f}, {:.3f}, {:.3f}) m/s, |v|={:.3f} m/s, nodes={}, window=[{:.1f},{:.1f}]",
                satellite, time.tow,
                pos.x() / 1000.0, pos.y() / 1000.0, pos.z() / 1000.0,
                vel.x(), vel.y(), vel.z(), vel.norm(),
                n, nodes.front()->time.tow, nodes.back()->time.tow);

            return { pos, vel };
        }



        /**
         * @brief Compute topocentric elevation and azimuth from ECEF coordinates.
         *
         * Converts the vector from receiver to satellite into a local East-North-Up (ENU)
         * frame at the receiver and returns topocentric angles in degrees:
         *  - `Topocentric::elevation_deg` in range [0, 90]
         *  - `Topocentric::azimuth_deg` in range [0, 360)
         *
         * The routine:
         *  - computes the line vector delta = sat_pos_ecef - rx_pos_ecef,
         *  - forms the ENU rotation from receiver geocentric latitude/longitude,
         *  - projects the unit LOS into ENU and computes elevation = asin(up) and azimuth = atan2(E, N).
         *
         * On degenerate input (near-zero range or invalid receiver magnitude) the function
         * returns a default `Topocentric` with zeroed angles and logs a warning.
         *
         * @param[in] rx_pos_ecef  Receiver ECEF position (meters).
         * @param[in] sat_pos_ecef Satellite ECEF position (meters).
         * @return Topocentric containing elevation and azimuth in degrees.
         */
        Topocentric Geometry::computeTopocentric(const Eigen::Vector3d& rx_pos_ecef,
            const Eigen::Vector3d& sat_pos_ecef) const {
            Topocentric out;

            Eigen::Vector3d delta = sat_pos_ecef - rx_pos_ecef;
            double range = delta.norm();
            if (range < 1.0) {
                log_manager_.getLogger("geometry")->warn("Degenerate range in topocentric computation: range={:.3f} m", range);
                return out;
            }

            double r = rx_pos_ecef.norm();
            if (r < 1e-6) {
                log_manager_.getLogger("geometry")->warn("Invalid receiver ECEF magnitude in topocentric computation");
                return out;
            }

            // Use geodetic latitude/longitude (from Coordinates) — more accurate than geocentric estimate.
            Coordinates::Llh ref_llh = Coordinates::ecefToLlh(rx_pos_ecef);

            // Convert delta (ECEF) to local ENU. ecefToEnu returns ENU = R^T * vec_ecef.
            // We want unit LOS in ENU, so normalize after conversion.
            Eigen::Vector3d los_enu = Coordinates::ecefToEnu(delta, ref_llh) / range;

            double up = std::clamp(los_enu.z(), -1.0, 1.0);
            double elev_rad = std::asin(up);
            double elev_deg = elev_rad * 180.0 / Constants::PI;
            elev_deg = std::max(0.0, std::min(90.0, elev_deg));

            double az_rad = std::atan2(los_enu.x(), los_enu.y());
            double az_deg = az_rad * 180.0 / Constants::PI;
            if (az_deg < 0.0) az_deg += 360.0;

            out.elevation_deg = elev_deg;
            out.azimuth_deg = az_deg;

            log_manager_.getLogger("geometry")->trace("Topocentric: elev={:.3f}°, az={:.3f}° (lat≈{:.6f}°)",
                elev_deg, az_deg, ref_llh.lat_rad * 180.0 / Constants::PI);

            return out;
        }

        /**
         * @brief Compute satellite antenna attitude unit vectors (yaw‑steering model).
         *
         * Computes unit vectors describing the satellite antenna attitude used for
         * antenna phase-center variation (PCV) corrections and phase wind-up modeling.
         * The routine implements a yaw-steering style attitude calculation using the
         * satellite velocity and the Sun direction:
         *  - `d_s`  : body-to-sun / boresight-like unit vector (ECEF)
         *  - `y_s`  : cross-track unit vector (ECEF)
         *
         * Behavior and safeguards:
         *  - Uses interpolated Sun position for the provided epoch (`time`).
         *  - Handles degenerate geometry (e.g., velocity nearly parallel to Sun vector)
         *    by falling back to a stable orthogonal axis to avoid NaNs.
         *  - Output vectors are normalized unit vectors.
         *
         * @param[in]  system  GNSS system identifier (enum) to allow system-specific rules.
         * @param[in]  sat_pos Satellite ECEF position (meters).
         * @param[in]  sat_vel Satellite ECEF velocity (meters/second).
         * @param[out] d_s     Body-to-sun / boresight unit vector (ECEF).
         * @param[out] y_s     Cross-track unit vector (ECEF).
         * @param[in]  time    Epoch corresponding to satellite state (GPS week + tow).
         *
         * @note Callers should ensure `sat_pos` and `sat_vel` are finite and non-zero;
         *       the implementation performs defensive checks and selects fallbacks when necessary.
         */
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

    }
} // namespace GnssProcessor