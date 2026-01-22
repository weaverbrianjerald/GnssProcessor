#include "ModelGeometry.hpp"
#include "ProductParser.hpp"
#include "Constants.hpp"
#include <algorithm>
#include <cmath>
#include <numeric>

namespace GnssProcessor {

    namespace Model {

        // Anonymous namespace for file-local utilities
        namespace {

            /**
             * @brief Convert a GNSS time to continuous GPS seconds for sorting / interpolation.
             *
             * Builds a monotonic scalar time value by converting the GPS week + time-of-week
             * representation into seconds since the GPS epoch:
             *   continuous_seconds = week * 604800 + tow
             *
             * @param t GNSS time containing `week` and `tow` (seconds into the week).
             * @return Continuous GPS seconds as a double.
             *
             * @note Placed in an anonymous namespace and marked `inline` to give internal
             * linkage and avoid ODR issues across translation units.
             */
            inline double continuousGpsSeconds(const GnssTime& t) {
                return static_cast<double>(t.week) * 604800.0 + t.tow;
            }

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

                // Update GnssCorrections to use freshly loaded vectors.
                corrections_.setClocksAndErps(clocks_, erps_);
            }
            catch (const std::exception& e) {
                log_manager_.getLogger("geometry")->error("Exception in Geometry::loadData(): {}", e.what());
            }
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
         * @brief Interpolate satellite ECEF position from available orbit products.
         *
         * Interpolates a satellite position at the requested epoch using loaded orbit
         * product samples (`orbits_`). The implementation uses a numerically-stable
         * barycentric Lagrange interpolant on a selected subset of nodes (IGS-recommended
         * 9-point / order-8 when available, falling back to a minimum of 4 points).
         *
         * Behaviour and notes:
         * - Input `satellite` is a satellite identifier string (e.g. "G01", "E11").
         * - Input `time` is the desired epoch (GPS week + time-of-week) for the position.
         * - Returned vector is the satellite ECEF position in meters.
         * - If insufficient or invalid orbit nodes are available the function returns
         *   `Eigen::Vector3d::Zero()` as an error indicator (callers should check).
         * - The routine filters out orbit samples with non-finite or out-of-range norms,
         *   sorts nodes by proximity to the query time and trims the set to a stable
         *   interpolation subset. It also guards against excessive interpolation span
         *   (data gaps) and will shrink to the minimum-point fallback when required.
         * - Extrapolation is possible if the query time lies outside the available node
         *   times; the implementation logs a warning when extrapolation exceeds a half
         *   SP3 interval and may degrade accuracy.
         * - The method returns the interpolated ECEF position. If the interpolation
         *   produces non-finite values the first valid node is returned as a safe fallback.
         *
         * @param[in] satellite Satellite identifier string (e.g. "G01", "E11").
         * @param[in] time      Epoch at which the position is requested (GPS week + tow).
         * @return Satellite ECEF position (meters). `Eigen::Vector3d::Zero()` indicates failure.
         */
        Eigen::Vector3d Geometry::getSatellitePosition(const std::string& satellite, const GnssTime& time) const {
            // IGS-recommended: 9-point (order-8) Lagrange on SP3 positions.
            // Fallback to >=4 points (cubic+) if fewer are available.
            // 
            // SP3 files typically have 15-minute (900s) epochs. CLK files have 30-second epochs.
            // RINEX observations can be at any rate (commonly 1s, 30s, or 300s).
            // This method interpolates SP3 positions to the exact RINEX observation time.
            constexpr size_t IGS_POINTS = 9;       // order-8 polynomial (IGS recommendation)
            constexpr size_t MIN_POINTS = 4;       // order-3 fallback (minimum for reasonable accuracy)
            constexpr double MAX_SPAN = 7200.0;    // 2 hours total span limit (prevents oscillations)
            constexpr double SP3_INTERVAL = 900.0; // Typical SP3 epoch interval (15 minutes)
            constexpr double MIN_POS_NORM = 1.5e7;
            constexpr double MAX_POS_NORM = 4.0e7;

            if (satellite.empty() || orbits_.empty()) {
                log_manager_.getLogger("geometry")->warn("Position interpolation: invalid satellite or no orbit data");
                return Eigen::Vector3d::Zero();
            }

            const double t_query = continuousGpsSeconds(time);

            // Collect all valid nodes for this satellite
            std::vector<const OrbitData*> nodes;
            nodes.reserve(IGS_POINTS * 2);
            for (const auto& o : orbits_) {
                if (o.satellite != satellite || !o.position_valid || !o.position.allFinite()) continue;
                const double t = continuousGpsSeconds(o.time);
                const double norm = o.position.norm();
                if (norm <= MIN_POS_NORM || norm >= MAX_POS_NORM) continue;
                nodes.push_back(&o);
            }
            if (nodes.size() < MIN_POINTS) {
                log_manager_.getLogger("geometry")->warn("Position interpolation: insufficient nodes ({}) for {} at week {} tow {:.3f}",
                    nodes.size(), satellite, time.week, time.tow);
                return Eigen::Vector3d::Zero();
            }

            // Sort by proximity to query, then trim to at most IGS_POINTS
            std::sort(nodes.begin(), nodes.end(), [t_query](const OrbitData* a, const OrbitData* b) {
                return std::abs(continuousGpsSeconds(a->time) - t_query) <
                    std::abs(continuousGpsSeconds(b->time) - t_query);
                });
            if (nodes.size() > IGS_POINTS) nodes.resize(IGS_POINTS);

            // Sort chronologically for interpolation stability
            std::sort(nodes.begin(), nodes.end(), [](const OrbitData* a, const OrbitData* b) {
                return continuousGpsSeconds(a->time) < continuousGpsSeconds(b->time);
                });

            // Check for extrapolation (query outside interpolation window)
            const double t_first = continuousGpsSeconds(nodes.front()->time);
            const double t_last = continuousGpsSeconds(nodes.back()->time);
            if (t_query < t_first || t_query > t_last) {
                const double extrap_dist = (t_query < t_first) ? (t_first - t_query) : (t_query - t_last);
                // Warn if extrapolating more than half an SP3 interval (7.5 minutes)
                if (extrap_dist > SP3_INTERVAL / 2.0) {
                    log_manager_.getLogger("geometry")->warn(
                        "Position extrapolation for {} at week {} tow {:.3f}: {:.1f}s outside SP3 window [{:.1f}, {:.1f}]s - accuracy degraded",
                        satellite, time.week, time.tow, extrap_dist,
                        nodes.front()->time.tow, nodes.back()->time.tow);
                }
                else {
                    log_manager_.getLogger("geometry")->debug(
                        "Position extrapolation for {} at week {} tow {:.3f}: {:.1f}s outside window",
                        satellite, time.week, time.tow, extrap_dist);
                }
            }

            // Enforce maximum total span to limit oscillations
            const double span = t_last - t_first;
            if (span > MAX_SPAN) {
                // Span is too large (likely data gap). Re-select ONLY the closest MIN_POINTS.
                std::sort(nodes.begin(), nodes.end(), [t_query](const OrbitData* a, const OrbitData* b) {
                    return std::abs(continuousGpsSeconds(a->time) - t_query) <
                        std::abs(continuousGpsSeconds(b->time) - t_query);
                    });

                // Aggressively shrink to minimum points to respect span as much as possible
                if (nodes.size() > MIN_POINTS) {
                    nodes.resize(MIN_POINTS);
                }

                std::sort(nodes.begin(), nodes.end(), [](const OrbitData* a, const OrbitData* b) {
                    return continuousGpsSeconds(a->time) < continuousGpsSeconds(b->time);
                    });
            }

            const size_t n = nodes.size();
            std::vector<double> x(n);
            std::vector<Eigen::Vector3d> y(n);
            for (size_t i = 0; i < n; ++i) {
                x[i] = continuousGpsSeconds(nodes[i]->time);
                y[i] = nodes[i]->position;
            }

            auto w = barycentricWeights(x);
            Eigen::Vector3d interp = barycentricEval(x, w, y, t_query);
            if (!interp.allFinite()) {
                log_manager_.getLogger("geometry")->warn("Position interpolation produced non-finite for {} - fallback first node", satellite);
                return y.front();
            }

            // Diagnostic logging for position interpolation
            log_manager_.getLogger("geometry")->debug(
                "Orbit interp for {} at tow={:.3f}: used {} pts, span=[{:.3f}, {:.3f}]s (dt={:.1f}s), "
                "pos=({:.3f}, {:.3f}, {:.3f}) km, norm={:.3f} km",
                satellite, time.tow, n,
                nodes.front()->time.tow, nodes.back()->time.tow,
                x.back() - x.front(),
                interp.x() / 1000.0, interp.y() / 1000.0, interp.z() / 1000.0,
                interp.norm() / 1000.0);

            return interp;
        }

        /**
         * @brief Estimate satellite ECEF velocity by numerical differentiation.
         *
         * Computes the satellite velocity at the requested epoch by central finite
         * differences of the interpolated position:
         *   v(t) ≈ [p(t + h) - p(t - h)] / (2h)
         *
         * The routine:
         * - Interpolates positions at t ± h using `getSatellitePosition`.
         * - Uses a small step `h` (0.5 s in the implementation) to balance accuracy
         *   and numerical stability.
         * - Validates interpolated positions and the resulting velocity; on failure
         *   it returns `Eigen::Vector3d::Zero()` and logs a warning.
         *
         * @param[in] satellite Satellite identifier string (e.g. "G01", "E11").
         * @param[in] time      Epoch at which the velocity is requested (GPS week + tow).
         * @return Satellite ECEF velocity in meters/second. Returns `Eigen::Vector3d::Zero()`
         *         if interpolation fails or the computed velocity is non-finite / implausible.
         *
         * @note The implementation guards against GPS week/TOW wrap-around when forming
         *       t ± h, and rejects results whose norm exceeds an expected maximum (`MAX_VEL_NORM`)
         *       to avoid returning spurious velocities from bad orbit data.
         */
        Eigen::Vector3d Geometry::getSatelliteVelocity(const std::string& satellite, const GnssTime& time) const {
            // Derive velocity by numerical differentiation of the position interpolant.
            // We compute: v(t) ≈ [p(t+h) - p(t-h)] / (2h)
            // where p(t) is the interpolated position and h is a small time step.
            //
            // This is more stable than the barycentric derivative formula when dealing
            // with large time values (GPS seconds ~10^9).
            constexpr double h = 0.5;  // 0.5 second step for central difference
            constexpr double MAX_VEL_NORM = 15000.0;  // Maximum expected velocity ~4 km/s

            if (satellite.empty() || orbits_.empty()) {
                log_manager_.getLogger("geometry")->warn("Velocity interpolation: invalid satellite or no orbit data");
                return Eigen::Vector3d::Zero();
            }

            // Compute position at t+h and t-h
            GnssTime t_plus = time;
            t_plus.tow += h;
            if (t_plus.tow >= 604800.0) { t_plus.tow -= 604800.0; t_plus.week += 1; }

            GnssTime t_minus = time;
            t_minus.tow -= h;
            if (t_minus.tow < 0.0) { t_minus.tow += 604800.0; t_minus.week -= 1; }

            Eigen::Vector3d pos_plus = getSatellitePosition(satellite, t_plus);
            Eigen::Vector3d pos_minus = getSatellitePosition(satellite, t_minus);

            if (!pos_plus.allFinite() || !pos_minus.allFinite() ||
                pos_plus.norm() < 1e3 || pos_minus.norm() < 1e3) {
                log_manager_.getLogger("geometry")->warn("Velocity interpolation: position interpolation failed for {} at tow {:.3f}",
                    satellite, time.tow);
                return Eigen::Vector3d::Zero();
            }

            // Central difference: v = (p(t+h) - p(t-h)) / (2h)
            Eigen::Vector3d vel = (pos_plus - pos_minus) / (2.0 * h);

            if (!vel.allFinite() || vel.norm() > MAX_VEL_NORM) {
                log_manager_.getLogger("geometry")->warn("Velocity interpolation: invalid result for {} at tow {:.3f}: norm={:.1f} m/s",
                    satellite, time.tow, vel.norm());
                return Eigen::Vector3d::Zero();
            }

            // Diagnostic logging
            log_manager_.getLogger("geometry")->debug(
                "Velocity interp for {} at tow={:.3f}: vel=({:.3f}, {:.3f}, {:.3f}) m/s, norm={:.3f} m/s, method=central_diff",
                satellite, time.tow, vel.x(), vel.y(), vel.z(), vel.norm());

            return vel;
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
            for (int iter = 0; iter < max_iter && tof_delta > 1e-8; ++iter) {
                // Emission time = reception time - TOF (in GPS system time)
                GnssTime emission_time = obs.time;
                emission_time.tow -= tof;
                if (emission_time.tow < 0) { emission_time.tow += 604800.0; emission_time.week -= 1; }

                // Get satellite clock correction at emission time.
                // This is dt_sat such that: t_satellite_proper = t_GPS - dt_sat
                // A positive dt_sat means the satellite clock is ahead of GPS time.
                sat_clock_em = corrections_.getSatelliteClockCorrection(obs.satellite, emission_time); // [s]

                // For orbit interpolation, we need the satellite position at the GPS system time
                // when the signal was transmitted. This is emission_time (already in GPS system time).
                // We do NOT adjust orbit lookup by sat_clock - the SP3 is in GPS system time.
                sat_pos_em = getSatellitePosition(obs.satellite, emission_time);
                sat_vel_em = getSatelliteVelocity(obs.satellite, emission_time);

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

            if (tof_delta > 1e-7) {
                log_manager_.getLogger("geometry")->warn("TOF iteration failed to converge for {}: delta={:.3e} s",
                    obs.satellite, tof_delta);
                return false;
            }

            // Diagnostic: Log final emission time and satellite PVT details
            {
                GnssTime emission_time = obs.time;
                emission_time.tow -= tof;
                if (emission_time.tow < 0) { emission_time.tow += 604800.0; emission_time.week -= 1; }

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
            double lat = std::asin(rx_pos_ecef.z() / r);
            double lon = std::atan2(rx_pos_ecef.y(), rx_pos_ecef.x());

            double slat = std::sin(lat), clat = std::cos(lat);
            double slon = std::sin(lon), clon = std::cos(lon);

            Eigen::Matrix3d R;
            R << -slon, clon, 0.0,
                -slat * clon, -slat * slon, clat,
                clat* clon, clat* slon, slat;

            Eigen::Vector3d los_enu = R * (delta / range);

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
                elev_deg, az_deg, lat * 180.0 / Constants::PI);

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