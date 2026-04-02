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

        // (Unused anonymous-namespace interpolation helpers removed;
        //  all interpolation now goes through Interpolate:: class methods.)

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
            : corrections_(log_mgr, config, cache.getClocks(), cache.getErps(), cache.getBiases(), rinex),
            config_(config),
            planetary_(log_mgr),
            troposphere_(config, log_mgr),
            product_cache_(cache),
            log_manager_(log_mgr),
            rinex_parser_(rinex),
            elevation_threshold_(config.getElevationMaskDeg()),
            signal_priorities_(config.getSignalPriorities()),
            // Initialize references from cache
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
                "Geometry model initialized: {} orbits, {} clocks, elev_mask={:.1f} deg", orbits_.size(), clocks_.size(), elevation_threshold_
            );
            // Build indices from the cached product vectors so lookups are O(1)
            loadData();
        }

        /**
         * @brief Build internal indices and load auxiliary data after construction.
         *
         * Builds the `orbit_index_` lookup table from the cached orbit vectors
         * so that per-satellite orbit lookups are O(1). Also loads optional
         * ocean tidal loading (OTL) BLQ coefficients when configured.
         *
         * All parsing errors and unexpected exceptions are logged via `log_manager_`.
         *
         * @note Called from the constructor after member initialization.
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

            // Attempt to load ocean tidal loading coefficients from BLQ file
            auto otl_path = config_.getOtlBlqPath();
            if (!otl_path.empty()) {
                std::string station_name = rinex_parser_.getHeader().marker_name;
                if (!station_name.empty()) {
                    if (corrections_.loadOceanTideFromBLQ(otl_path.string(), station_name)) {
                        log_manager_.getLogger("geometry")->info(
                            "Loaded OTL BLQ coefficients for station '{}'", station_name);
                    } else {
                        log_manager_.getLogger("geometry")->warn(
                            "Could not load OTL BLQ coefficients for station '{}' from '{}'",
                            station_name, otl_path.string());
                    }
                } else {
                    log_manager_.getLogger("geometry")->warn(
                        "OTL BLQ path configured but RINEX marker name is empty; using approximate OTL model");
                }
            } else {
                log_manager_.getLogger("geometry")->info(
                    "No OTL BLQ file configured; using approximate ocean tidal loading model");
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
         * @brief Compute satellite geometry and apply standard propagation corrections for an observation.
         *
         * Performs the end-to-end geometry computation required by the PPP observation model:
         * - Computes an ionosphere-free code pseudorange (if dual-frequency available) or
         *   falls back to a single code pseudorange for initial signal transit time;
         * - Computes the satellite position at the signal transmission time using a
         *   single-pass approach: seeds TOF from pseudorange, corrects with satellite
         *   clock, then evaluates the orbit and clock once at the corrected epoch;
         * - Applies Earth rotation (Sagnac correction) using the geometric range;
         * - Computes line-of-sight, geometric range, elevation and azimuth (degrees);
         * - Computes troposphere mapping factors, zenith hydrostatic delay (ZHD), Shapiro delay,
         *   solid Earth tide and other diagnostic corrections (atmospheric loading, Sagnac, ...).
         *
         * The receiver clock bias embedded in the pseudorange creates a common-mode
         * emission-time error (~ms) across all satellites that maps entirely into the
         * receiver clock state in the estimator, so no geometric iteration is needed.
         *
         * @param[in]  obs               RINEX observation record containing satellite id, epoch and measurements.
         * @param[in]  state             Current PPP state (receiver position, troposphere/state variables, ...).
         * @param[out] sat_pos           Satellite ECEF position at reception-time (meters).
         * @param[out] rx_pos            Receiver ECEF position (meters).
         * @param[out] elevation         Satellite elevation at receiver in degrees.
         * @param[out] azimuth           Satellite azimuth at receiver in degrees (0..360).
         * @param[out] los               Line‑of‑sight unit vector from receiver to satellite (ECEF).
         * @param[out] range             Geometric range between receiver and satellite (meters).
         * @param[out] tropo_delay       Total tropospheric delay (meters) (ZHD and mapping applied as implemented).
         * @param[out] sat_clock         Satellite clock correction from CLK products (seconds). This is the
         *                                    raw interpolated value and does NOT include the relativistic periodic
         *                                    term (-2r·v/c²). Callers must subtract relativistic_corr separately
         *                                    in the observation model.
         * @param[out] shapiro_delay     Shapiro gravitational path delay (meters).
         * @param[out] sagnac_corr       Sagnac correction (meters). When emission-time rotation is applied this is implicitly included (returned for diagnostics).
         * @param[out] atm_loading_corr  Atmospheric loading correction (meters).
         * @param[out] relativistic_corr Relativistic periodic correction -2(r·v)/c² (seconds). This term
         *                                    is NOT included in the interpolated sat_clock value and must be
         *                                    subtracted (after conversion to meters) in the observation model.
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

            // --- Step 1b: Extract pseudorange from observation ---
            // Use a code pseudorange to seed the initial TOF estimate. The pseudorange
            // contains the geometric range plus receiver clock, satellite clock, tropo,
            // iono, etc. — but it is a much better initial TOF approximation than the
            // geometric range computed from an orbit evaluated at reception time.
            //
            // If two code observations on different frequencies are available, form an
            // ionosphere-free combination to remove the ~10 m iono contribution.
            // Otherwise fall back to any single code pseudorange.
            double pseudorange = 0.0;
            bool have_pseudorange = false;
            {
                // Collect available code observations (keys starting with 'C')
                std::vector<std::pair<std::string, double>> code_obs;
                for (const auto& [key, val] : obs.measurements) {
                    if (!key.empty() && key[0] == 'C' && std::isfinite(val) && val > 1e6) {
                        code_obs.emplace_back(key, val);
                    }
                }

                if (code_obs.size() >= 2) {
                    // Try to form an ionosphere-free pseudorange from two code signals
                    // on different frequency bands. We look at signal_priorities_ for
                    // the system to find two signals on different bands.
                    std::string sys_str = PppUtils::getSystemString(obs.system);
                    auto prio_it = signal_priorities_.find(sys_str);
                    std::string sig1, sig2;
                    if (prio_it != signal_priorities_.end()) {
                        // Find first two code signals on different bands from priority list
                        for (const auto& sig : prio_it->second) {
                            if (sig.empty() || sig[0] != 'C') continue;
                            if (obs.measurements.count(sig) == 0) continue;
                            double v = obs.measurements.at(sig);
                            if (!std::isfinite(v) || v < 1e6) continue;
                            // Determine band from the second character (e.g., C1x -> band 1, C2x -> band 2)
                            char band = sig.size() > 1 ? sig[1] : '0';
                            if (sig1.empty()) {
                                sig1 = sig;
                            } else {
                                char band1 = sig1.size() > 1 ? sig1[1] : '0';
                                if (band != band1) {
                                    sig2 = sig;
                                    break;
                                }
                            }
                        }
                    }

                    if (!sig1.empty() && !sig2.empty()) {
                        // Form ionosphere-free combination: P_IF = (f1²·P1 - f2²·P2) / (f1² - f2²)
                        PppUtils utils(log_manager_);
                        double f1 = utils.getFrequency(sig1, obs.system, obs.satellite);
                        double f2 = utils.getFrequency(sig2, obs.system, obs.satellite);
                        if (f1 > 0.0 && f2 > 0.0 && std::abs(f1 - f2) > 1e6) {
                            double p1 = obs.measurements.at(sig1);
                            double p2 = obs.measurements.at(sig2);
                            double f1sq = f1 * f1;
                            double f2sq = f2 * f2;
                            pseudorange = (f1sq * p1 - f2sq * p2) / (f1sq - f2sq);
                            have_pseudorange = std::isfinite(pseudorange) && pseudorange > 1e6;
                            if (have_pseudorange) {
                                log_manager_.getLogger("geometry")->trace(
                                    "Using IF pseudorange for {} TOF seed: {:.3f} m ({}={:.3f}, {}={:.3f})",
                                    obs.satellite, pseudorange, sig1, p1, sig2, p2);
                            }
                        }
                    }

                    if (!have_pseudorange) {
                        // Fallback: use the first valid code pseudorange
                        pseudorange = code_obs.front().second;
                        have_pseudorange = true;
                        log_manager_.getLogger("geometry")->trace(
                            "Using single pseudorange for {} TOF seed: {:.3f} m ({})",
                            obs.satellite, pseudorange, code_obs.front().first);
                    }
                } else if (code_obs.size() == 1) {
                    pseudorange = code_obs.front().second;
                    have_pseudorange = true;
                    log_manager_.getLogger("geometry")->trace(
                        "Using single pseudorange for {} TOF seed: {:.3f} m ({})",
                        obs.satellite, pseudorange, code_obs.front().first);
                }
            }

            // --- Step 1c: Require dual-frequency code pseudorange ---
            // Following the reference implementation, if we don't have a pseudorange
            // we cannot reliably compute emission time. Skip observation.
            if (!have_pseudorange) {
                log_manager_.getLogger("geometry")->warn(
                    "No code pseudorange available for {} at TOW {:.3f} — skipping observation",
                    obs.satellite, obs.time.tow);
                return false;
            }

            // --- Step 2: Emission-time computation (single-pass, RTKLIB-style) ---
            //
            // Compute the satellite position and clock at the signal transmission
            // time using a single-pass approach (no geometric iteration needed):
            //   1. tof   = P_IF / c                      (pseudorange seed)
            //   2. t_em  = t_rx - tof                     (approximate emission time)
            //   3. dt_sat = clock(t_em)                   (satellite clock at approx time)
            //   4. t_em  = t_rx - tof - dt_sat            (corrected GPS transmission time)
            //   5. orbit(t_em) → sat_pos, sat_vel         (satellite PVT at transmission)
            //   6. dt_sat = clock(t_em)                   (re-evaluate clock at corrected time)
            //   7. tf    = |sat_pos - rx_pos| / c         (geometric TOF for Sagnac only)
            //   8. rotate sat_pos by ωe·tf                (Earth rotation correction)
            //
            // The receiver clock bias embedded in P/c causes a common-mode
            // emission-time error (~ms) across all satellites. This maps entirely
            // into the receiver clock state in the Kalman filter and does not
            // degrade the position solution. Iterating on geometric range would
            // remove this common-mode offset, but would introduce geometry-
            // dependent errors when the receiver position is imprecise.

            double tof = pseudorange / c; // initial signal transit time [s]
            Eigen::Vector3d sat_pos_em = Eigen::Vector3d::Zero();
            Eigen::Vector3d sat_vel_em = Eigen::Vector3d::Zero();
            double sat_clock_em = 0.0;
            GnssTime emission_time{};

            // Step 2a: Approximate emission time from pseudorange
            emission_time = obs.time;
            emission_time.tow -= tof;
            if (emission_time.tow < 0) { emission_time.tow += TimeUtils::SECONDS_IN_WEEK; emission_time.week -= 1; }

            // Step 2b: Get satellite clock at approximate emission time
            sat_clock_em = corrections_.getSatelliteClockCorrection(obs.satellite, emission_time); // [s]

            if (std::abs(sat_clock_em) < 1e-12) {
                log_manager_.getLogger("geometry")->warn(
                    "Satellite clock correction is zero/invalid for {} at emission TOW {:.3f} - skipping observation",
                    obs.satellite, emission_time.tow);
                return false;
            }

            // Step 2c: Correct emission time by satellite clock (GPS transmission time)
            emission_time = obs.time;
            emission_time.tow -= (tof + sat_clock_em);
            if (emission_time.tow < 0) { emission_time.tow += TimeUtils::SECONDS_IN_WEEK; emission_time.week -= 1; }
            if (emission_time.tow >= TimeUtils::SECONDS_IN_WEEK) { emission_time.tow -= TimeUtils::SECONDS_IN_WEEK; emission_time.week += 1; }

            // Step 2d: Interpolate satellite orbit at corrected emission time
            auto [pos_em, vel_em] = getSatellitePV(obs.satellite, emission_time);

            if (pos_em.norm() < 1e3 || !pos_em.allFinite()) {
                log_manager_.getLogger("geometry")->warn("Invalid satellite position at emission time for {} at TOW {:.3f}",
                    obs.satellite, emission_time.tow);
                return false;
            }

            sat_pos_em = pos_em;
            sat_vel_em = vel_em;

            // Step 2e: Re-evaluate satellite clock at corrected emission time
            sat_clock_em = corrections_.getSatelliteClockCorrection(obs.satellite, emission_time); // [s]

            if (std::abs(sat_clock_em) < 1e-12) {
                log_manager_.getLogger("geometry")->warn(
                    "Satellite clock correction is zero/invalid for {} at corrected emission TOW {:.3f} - skipping observation",
                    obs.satellite, emission_time.tow);
                return false;
            }

            // Step 2f: Earth rotation correction (Sagnac)
            // Use geometric TOF from satellite position to receiver for the rotation angle
            {
                double geom_tof = (sat_pos_em - rx_pos).norm() / c;
                double theta = omegaE * geom_tof;
                double cos_theta = std::cos(theta);
                double sin_theta = std::sin(theta);
                Eigen::Matrix3d R_earth;
                R_earth << cos_theta, sin_theta, 0.0,
                    -sin_theta, cos_theta, 0.0,
                    0.0, 0.0, 1.0;
                sat_pos = R_earth * sat_pos_em; // reception-time ECEF
            }

            // Relativistic correction (periodic term): dtr = -2(r·v)/c²
            relativistic_corr = corrections_.computeRelativisticCorrection(obs.satellite, sat_pos_em, sat_vel_em);

            // Output satellite clock
            sat_clock = sat_clock_em;

            // Diagnostic log
            {
                double r_dot_v = sat_pos_em.dot(sat_vel_em);

                log_manager_.getLogger("geometry")->debug(
                    "Geometry {} at rx_tow={:.3f}: emission_tow={:.3f}, tof={:.9f}s ({:.3f}m), "
                    "sat_clk={:.3e}s, rel_corr={:.3e}s, "
                    "r_dot_v={:.3e}, vel_norm={:.1f}m/s",
                    obs.satellite, obs.time.tow, emission_time.tow, tof, tof * c,
                    sat_clock_em, relativistic_corr,
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
            troposphere_.getTroposphereMapping(elevation, rx_pos, obs.time, mf_d, mf_w);
            double zhd = troposphere_.computeSaastamoinenZHD(rx_pos); // zenith hydrostatic (dry) component
            tropo_delay = zhd;
            shapiro_delay = corrections_.computeShapiroDelay(obs.satellite, rx_pos, sat_pos);

            // Sagnac correction is implicitly included via the Earth rotation of sat_pos.
            // Compute the equivalent scalar value for diagnostic logging:
            //   sagnac ≈ ωe/c * (x_s·y_r - y_s·x_r)
            sagnac_corr = (omegaE / c) * (sat_pos_em.x() * rx_pos.y() - sat_pos_em.y() * rx_pos.x());

            atm_loading_corr = 0.0; // placeholder

            // Ocean tidal loading correction
            Eigen::Vector3d otl_ecef = corrections_.computeOceanTideCorrection(rx_pos, obs.time);
            double otl_corr = otl_ecef.dot(-los);

            // Sun and Moon positions depend only on the epoch, not on the satellite.
            // Cache them so the expensive rigorous model (IAU 1976 precession,
            // IAU 1980 nutation with 106 terms, GAST, polar motion) is evaluated
            // once per epoch rather than once per satellite.
            {
                double cached_t = TimeUtils::continuousGpsSeconds(cached_sun_moon_epoch_);
                double obs_t    = TimeUtils::continuousGpsSeconds(obs.time);
                if (std::abs(obs_t - cached_t) > 1e-3) {
                    planetary_.computeSunMoonPosition(obs.time, erps_,
                        cached_sun_pos_, cached_moon_pos_);
                    cached_sun_moon_epoch_ = obs.time;
                }
            }
            const Eigen::Vector3d& sun_pos  = cached_sun_pos_;
            const Eigen::Vector3d& moon_pos = cached_moon_pos_;
            //solid_tide = corrections_.computeSolidEarthTide(rx_pos, sat_pos, sun_pos, moon_pos, obs.time) + otl_corr; // disable OTL while debugging IGS station AMC4
            solid_tide = corrections_.computeSolidEarthTide(rx_pos, sat_pos, sun_pos, moon_pos, obs.time);

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
            constexpr size_t IGS_POINTS = 11;
            constexpr size_t MIN_POINTS = 9;
            constexpr double MIN_POS_NORM = 1.5e6;
            constexpr double MAX_POS_NORM = 4.0e8;
            constexpr double MAX_VEL_NORM = 50000.0;

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

            // Detect the SP3 epoch interval from the actual orbit data rather than
            // assuming a fixed 900 s (15 min) or 300 s (5 min) spacing. Use the
            // median of the first few consecutive-node spacings for robustness
            // against occasional gaps.
            double sp3_interval = 300.0; // fallback = 5-minutes
            if (sat_orbits.size() >= 2) {
                std::vector<double> spacings;
                spacings.reserve(std::min<size_t>(sat_orbits.size() - 1, 10));
                for (size_t i = 0; i + 1 < sat_orbits.size() && spacings.size() < 10; ++i) {
                    double dt = TimeUtils::continuousGpsSeconds(sat_orbits[i + 1]->time)
                              - TimeUtils::continuousGpsSeconds(sat_orbits[i]->time);
                    if (dt > 0.0) spacings.push_back(dt);
                }
                if (!spacings.empty()) {
                    std::sort(spacings.begin(), spacings.end());
                    sp3_interval = spacings[spacings.size() / 2]; // median
                }
            }
            const double MAX_SPAN = sp3_interval * (IGS_POINTS - 1) + 1.0; // no gaps allowed (11 contiguous nodes = 10 intervals)

            // Find the nearest node to t_query (not just the first node >= t_query).
            // Using the nearest node as the center anchor ensures the interpolation
            // window only shifts when the query crosses the MIDPOINT between two
            // consecutive SP3 epochs. This eliminates the sawtooth discontinuity
            // that occurs with lower_bound-based centering, where the window jumps
            // by one node every SP3 interval (producing cm-level interpolation
            // discontinuities at each SP3 epoch boundary).
            auto upper_it = std::lower_bound(sat_orbits.begin(), sat_orbits.end(), t_query,
                [](const OrbitData* o, double t) {
                    return TimeUtils::continuousGpsSeconds(o->time) < t;
                });

            // Determine the index of the nearest node to t_query
            size_t nearest_idx = 0;
            if (upper_it == sat_orbits.end()) {
                nearest_idx = sat_orbits.size() - 1;
            } else if (upper_it == sat_orbits.begin()) {
                nearest_idx = 0;
            } else {
                size_t hi = std::distance(sat_orbits.begin(), upper_it);
                size_t lo = hi - 1;
                double t_lo = TimeUtils::continuousGpsSeconds(sat_orbits[lo]->time);
                double t_hi = TimeUtils::continuousGpsSeconds(sat_orbits[hi]->time);
                nearest_idx = (t_query - t_lo <= t_hi - t_query) ? lo : hi;
            }

            // Center the window on the nearest node.
            // For IGS_POINTS=11 (odd): half_before=5, so window is [nearest-5, nearest+5].
            // The window shifts only when nearest_idx changes, which happens at the
            // midpoint between SP3 epochs — not at the epoch itself.
            size_t half_before = IGS_POINTS / 2;

            size_t start_idx = 0;
            if (nearest_idx > half_before) {
                start_idx = nearest_idx - half_before;
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
                if (extrap_dist > sp3_interval / 2.0) {
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
                    // Use Catmull-Rom (non-monotone) slopes, NOT monotone Hermite.
                    // Satellite ECEF coordinates are sinusoidal; monotone Hermite
                    // zeroes the slope at every local extremum, producing grossly
                    // incorrect velocities at those points.
                    auto mx = Interpolate::catmullRomSlopes(x, yx);
                    auto my = Interpolate::catmullRomSlopes(x, yy);
                    auto mz = Interpolate::catmullRomSlopes(x, yz);

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
            // Use the rigorous cached Sun position if available for this epoch,
            // otherwise fall back to the approximate model.
            Eigen::Vector3d sun_pos = Eigen::Vector3d::Zero();
            {
                double cached_t = TimeUtils::continuousGpsSeconds(cached_sun_moon_epoch_);
                double query_t  = TimeUtils::continuousGpsSeconds(time);
                if (std::abs(query_t - cached_t) < 1e-3 && cached_sun_pos_.norm() > 1e6) {
                    sun_pos = cached_sun_pos_;
                } else {
                    // Use the rigorous IAU 1976/1980 model and update the cache
                    Eigen::Vector3d moon_tmp = Eigen::Vector3d::Zero();
                    planetary_.computeSunMoonPosition(time, erps_, cached_sun_pos_, moon_tmp);
                    cached_moon_pos_ = moon_tmp;
                    cached_sun_moon_epoch_ = time;
                    sun_pos = cached_sun_pos_;
                }
            }
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

        /**
         * @brief Reset phase wind-up state for a specific satellite.
         *
         * Delegates to the internal GnssCorrections object to clear the stored
         * previous wind-up angle for the specified satellite.
         *
         * @param satellite Satellite identifier string (e.g. "G01").
         */
        void Geometry::resetPhaseWindUp(const std::string& satellite) {
            corrections_.resetPhaseWindUp(satellite);
        }

        /**
         * @brief Reset phase wind-up state for all satellites.
         *
         * Delegates to the internal GnssCorrections object to clear all stored
         * previous wind-up angles.
         */
        void Geometry::resetAllPhaseWindUp() {
            corrections_.resetAllPhaseWindUp();
        }

    }
} // namespace GnssProcessor} // namespace GnssProcessor