#include "ModelGeometry.hpp"
/*!
 * \file ModelGeometry.cpp
 * \brief Satellite/receiver geometry and correction computations for PPP.
 *
 * The implementation is organized as:
 *  - Construction / product loading
 *  - Private helpers:  extractPseudorange, computeEmissionState, ensureSunMoonCached
 *  - Core geometry:    computeSatelliteGeometry
 *  - Orbit interpolation: getSatellitePV
 *  - Topocentric / attitude / wind-up utilities
 */

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

        // =================================================================
        // Construction / product loading
        // =================================================================

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

        // =================================================================
        // Simple accessors
        // =================================================================

        double Geometry::computeGeometricRange(const Eigen::Vector3d& rx_pos, const Eigen::Vector3d& sat_pos) const {
            return (sat_pos - rx_pos).norm(); // meters
        }


        // =================================================================
        // Private helpers
        // =================================================================

        void Geometry::ensureSunMoonCached(const GnssTime& time) const {
            const double cached_t = TimeUtils::continuousGpsSeconds(cached_sun_moon_epoch_);
            const double query_t  = TimeUtils::continuousGpsSeconds(time);
            if (std::abs(query_t - cached_t) > 1e-3) {
                planetary_.computeSunMoonPosition(time, erps_,
                    cached_sun_pos_, cached_moon_pos_);
                cached_sun_moon_epoch_ = time;
            }
        }

        bool Geometry::extractPseudorange(const RinexObservation& obs, double& pseudorange) const {
            // Collect available code observations (keys starting with 'C')
            std::vector<std::pair<std::string, double>> code_obs;
            for (const auto& [key, val] : obs.measurements)
                if (!key.empty() && key[0] == 'C' && std::isfinite(val) && val > 1e6)
                    code_obs.emplace_back(key, val);

            if (code_obs.empty()) {
                log_manager_.getLogger("geometry")->warn(
                    "No code pseudorange available for {} at TOW {:.3f} — skipping observation",
                    obs.satellite, obs.time.tow);
                return false;
            }

            // Attempt ionosphere-free combination from two code signals on different bands
            if (code_obs.size() >= 2) {
                const std::string sys_str = PppUtils::getSystemString(obs.system);
                auto prio_it = signal_priorities_.find(sys_str);
                std::string sig1, sig2;

                if (prio_it != signal_priorities_.end()) {
                    for (const auto& sig : prio_it->second) {
                        if (sig.empty() || sig[0] != 'C') continue;
                        if (obs.measurements.count(sig) == 0) continue;
                        const double v = obs.measurements.at(sig);
                        if (!std::isfinite(v) || v < 1e6) continue;
                        const char band = sig.size() > 1 ? sig[1] : '0';
                        if (sig1.empty()) {
                            sig1 = sig;
                        } else {
                            if (band != (sig1.size() > 1 ? sig1[1] : '0')) {
                                sig2 = sig;
                                break;
                            }
                        }
                    }
                }

                if (!sig1.empty() && !sig2.empty()) {
                    PppUtils utils(log_manager_);
                    const double f1 = utils.getFrequency(sig1, obs.system, obs.satellite);
                    const double f2 = utils.getFrequency(sig2, obs.system, obs.satellite);
                    if (f1 > 0.0 && f2 > 0.0 && std::abs(f1 - f2) > 1e6) {
                        const double p1 = obs.measurements.at(sig1);
                        const double p2 = obs.measurements.at(sig2);
                        const double f1sq = f1 * f1, f2sq = f2 * f2;
                        const double pr_if = (f1sq * p1 - f2sq * p2) / (f1sq - f2sq);
                        if (std::isfinite(pr_if) && pr_if > 1e6) {
                            pseudorange = pr_if;
                            log_manager_.getLogger("geometry")->trace(
                                "Using IF pseudorange for {} TOF seed: {:.3f} m ({}={:.3f}, {}={:.3f})",
                                obs.satellite, pseudorange, sig1, p1, sig2, p2);
                            return true;
                        }
                    }
                }
            }

            // Fallback: first valid code pseudorange
            pseudorange = code_obs.front().second;
            log_manager_.getLogger("geometry")->trace(
                "Using single pseudorange for {} TOF seed: {:.3f} m ({})",
                obs.satellite, pseudorange, code_obs.front().first);
            return true;
        }

        bool Geometry::computeEmissionState(const RinexObservation& obs,
            const Eigen::Vector3d& rx_pos, double pseudorange,
            Eigen::Vector3d& sat_pos_em, Eigen::Vector3d& sat_vel_em,
            double& sat_clock_out, GnssTime& emission_time) const {

            const double c = Constants::SPEED_OF_LIGHT;
            const double tof = pseudorange / c;

            // Step 1: Approximate emission time from pseudorange
            emission_time = obs.time;
            emission_time.tow -= tof;
            if (emission_time.tow < 0) {
                emission_time.tow += TimeUtils::SECONDS_IN_WEEK;
                emission_time.week -= 1;
            }

            // Step 2: Satellite clock at approximate emission time
            double sat_clk = corrections_.getSatelliteClockCorrection(obs.satellite, emission_time);
            if (std::abs(sat_clk) < 1e-12) {
                log_manager_.getLogger("geometry")->warn(
                    "Satellite clock correction is zero/invalid for {} at emission TOW {:.3f} - skipping",
                    obs.satellite, emission_time.tow);
                return false;
            }

            // Step 3: Corrected GPS transmission time
            emission_time = obs.time;
            emission_time.tow -= (tof + sat_clk);
            if (emission_time.tow < 0) {
                emission_time.tow += TimeUtils::SECONDS_IN_WEEK;
                emission_time.week -= 1;
            }
            if (emission_time.tow >= TimeUtils::SECONDS_IN_WEEK) {
                emission_time.tow -= TimeUtils::SECONDS_IN_WEEK;
                emission_time.week += 1;
            }

            // Step 4: Interpolate satellite orbit at corrected emission time
            auto [pos_em, vel_em] = getSatellitePV(obs.satellite, emission_time);
            if (pos_em.norm() < 1.9e7 || !pos_em.allFinite()) {
                log_manager_.getLogger("geometry")->warn(
                    "Invalid satellite position at emission time for {} at TOW {:.3f}: norm={:.3e} m",
                    obs.satellite, emission_time.tow, pos_em.norm());
                return false;
            }
            sat_pos_em = pos_em;
            sat_vel_em = vel_em;

            // Step 5: Re-evaluate satellite clock at corrected emission time
            sat_clk = corrections_.getSatelliteClockCorrection(obs.satellite, emission_time);
            if (std::abs(sat_clk) < 1e-12) {
                log_manager_.getLogger("geometry")->warn(
                    "Satellite clock is zero/invalid for {} at corrected emission TOW {:.3f} - skipping",
                    obs.satellite, emission_time.tow);
                return false;
            }
            sat_clock_out = sat_clk;
            return true;
        }

        // =================================================================
        // Core geometry computation
        // =================================================================

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
            if (obs.satellite == "E14") return false;

            const double c = Constants::SPEED_OF_LIGHT;
            const double omegaE = Constants::EARTH_ANGULAR_VELOCITY;

            // --- Validate receiver position ---
            rx_pos = state.position;
            if (rx_pos.norm() < 1e3 || !rx_pos.allFinite()) {
                log_manager_.getLogger("geometry")->warn("Invalid receiver position for {} at TOW {:.3f}",
                    obs.satellite, obs.time.tow);
                return false;
            }

            // --- Extract pseudorange ---
            double pseudorange = 0.0;
            if (!extractPseudorange(obs, pseudorange))
                return false;

            // --- Emission-time computation ---
            Eigen::Vector3d sat_pos_em = Eigen::Vector3d::Zero();
            Eigen::Vector3d sat_vel_em = Eigen::Vector3d::Zero();
            double sat_clock_em = 0.0;
            GnssTime emission_time{};

            if (!computeEmissionState(obs, rx_pos, pseudorange, sat_pos_em, sat_vel_em, sat_clock_em, emission_time))
                return false;

            // --- Earth rotation correction (Sagnac) ---
            {
                const double geom_tof = (sat_pos_em - rx_pos).norm() / c;
                const double theta = omegaE * geom_tof;
                Eigen::Matrix3d R_earth;
                R_earth <<  std::cos(theta), std::sin(theta), 0.0,
                           -std::sin(theta), std::cos(theta), 0.0,
                            0.0,             0.0,             1.0;
                sat_pos = R_earth * sat_pos_em;
            }

            // --- Relativistic periodic correction ---
            relativistic_corr = config_.getModelCorrections().enable_relativistic_correction
                ? corrections_.computeRelativisticCorrection(obs.satellite, sat_pos_em, sat_vel_em)
                : 0.0;

            sat_clock = sat_clock_em;

            log_manager_.getLogger("geometry")->debug(
                "Geometry {} at rx_tow={:.3f}: emission_tow={:.3f}, tof={:.9f}s ({:.3f}m), "
                "sat_clk={:.3e}s, rel_corr={:.3e}s, r_dot_v={:.3e}, vel_norm={:.1f}m/s",
                obs.satellite, obs.time.tow, emission_time.tow,
                pseudorange / c, pseudorange,
                sat_clock_em, relativistic_corr,
                sat_pos_em.dot(sat_vel_em), sat_vel_em.norm());

            // --- Final geometry (reception-time ECEF frame) ---
            range = (sat_pos - rx_pos).norm();
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
            tropo_delay = config_.getModelCorrections().enable_troposphere
                ? troposphere_.computeSaastamoinenZHD(rx_pos)
                : 0.0;
            shapiro_delay = config_.getModelCorrections().enable_shapiro_delay
                ? corrections_.computeShapiroDelay(obs.satellite, rx_pos, sat_pos)
                : 0.0;

            // Sagnac (diagnostic — implicitly included via Earth rotation of sat_pos)
            sagnac_corr = (omegaE / c) * (sat_pos_em.x() * rx_pos.y() - sat_pos_em.y() * rx_pos.x());
            atm_loading_corr = 0.0;

            // Ocean tidal loading
            Eigen::Vector3d otl_ecef = config_.getModelCorrections().enable_ocean_tide_loading
                ? corrections_.computeOceanTideCorrection(rx_pos, obs.time)
                : Eigen::Vector3d::Zero();
            double otl_corr = otl_ecef.dot(-los);

            // Solid Earth tide (uses cached Sun/Moon positions)
            ensureSunMoonCached(obs.time);
            solid_tide = config_.getModelCorrections().enable_solid_earth_tide
                ? corrections_.computeSolidEarthTide(rx_pos, sat_pos, cached_sun_pos_, cached_moon_pos_, obs.time)
                : 0.0;

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

        // =================================================================
        // Orbit interpolation
        // =================================================================

        /**
         * @brief Compute satellite position and velocity via Neville interpolation.
         *
         * Selects the IGS_POINTS nearest orbit nodes, validates for gaps and
         * extrapolation, then evaluates position and velocity via Neville's
         * algorithm with nodes sorted by distance from the query point.
         */
        std::pair<Eigen::Vector3d, Eigen::Vector3d> Geometry::getSatellitePV(
            const std::string& satellite, const GnssTime& time) const
        {
            constexpr size_t IGS_POINTS = 11;
            constexpr size_t MIN_POINTS = 9;
            constexpr double MIN_POS_NORM = 1.9e7;   // ~19,000 km (lowest MEO orbit)
            constexpr double MAX_POS_NORM = 4.5e7;   // ~45,000 km (above BeiDou GEO ~42,164 km)
            constexpr double MAX_VEL_NORM = 5.0e3;    // ~5 km/s (GNSS satellites: ~3-4 km/s)

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
            const double MAX_SPAN = sp3_interval * (IGS_POINTS - 1) + 1.0;

            // === STEP 1: Collect ALL valid nodes for this satellite ===
            // Precompute continuous GPS seconds for each orbit node to avoid
            // redundant recomputation inside sort comparators.
            struct NodeEntry {
                const OrbitData* ptr;
                double t;   // precomputed continuousGpsSeconds
            };
            std::vector<NodeEntry> valid_nodes;
            valid_nodes.reserve(sat_orbits.size());

            for (const OrbitData* o : sat_orbits) {
                if (!o->position_valid || !o->position.allFinite()) continue;
                double norm = o->position.norm();
                if (norm <= MIN_POS_NORM || norm >= MAX_POS_NORM) continue;
                valid_nodes.push_back({ o, TimeUtils::continuousGpsSeconds(o->time) });
            }

            if (valid_nodes.size() < MIN_POINTS) {
                log_manager_.getLogger("geometry")->warn(
                    "getSatellitePV: insufficient valid nodes ({}) for {} at week {} tow {:.3f}",
                    valid_nodes.size(), satellite, time.week, time.tow);
                return zero_result;
            }

            // === STEP 2: Select the IGS_POINTS nearest nodes to the query time ===
            // Use partial_sort to efficiently find the closest nodes without fully
            // sorting the entire valid_nodes vector.
            const size_t n_select = std::min(valid_nodes.size(), IGS_POINTS);

            std::partial_sort(valid_nodes.begin(), valid_nodes.begin() + n_select, valid_nodes.end(),
                [t_query](const NodeEntry& a, const NodeEntry& b) {
                    return std::abs(a.t - t_query) < std::abs(b.t - t_query);
                });

            std::vector<NodeEntry> nodes(valid_nodes.begin(), valid_nodes.begin() + n_select);

            if (nodes.size() < MIN_POINTS) {
                log_manager_.getLogger("geometry")->warn(
                    "getSatellitePV: insufficient nearest nodes ({}) for {} at week {} tow {:.3f}",
                    nodes.size(), satellite, time.week, time.tow);
                return zero_result;
            }

            // === STEP 3: Gap and extrapolation checks (chronological order) ===
            // Sort nodes chronologically for gap/extrapolation validation
            std::sort(nodes.begin(), nodes.end(), [](const NodeEntry& a, const NodeEntry& b) {
                return a.t < b.t;
            });

            const double t_first = nodes.front().t;
            const double t_last = nodes.back().t;

            // Check for extrapolation
            if (t_query < t_first || t_query > t_last) {
                const double extrap_dist = (t_query < t_first) ? (t_first - t_query) : (t_query - t_last);
                if (extrap_dist > sp3_interval / 2.0) {
                    log_manager_.getLogger("geometry")->warn(
                        "Position extrapolation for {} at week {} tow {:.3f}: {:.1f}s outside SP3 window [{:.1f}, {:.1f}]s",
                        satellite, time.week, time.tow, extrap_dist,
                        nodes.front().ptr->time.tow, nodes.back().ptr->time.tow);
                }
            }

            // Check for data gaps — if the span is too large, trim to MIN_POINTS closest
            const double span = t_last - t_first;
            if (span > MAX_SPAN) {
                std::sort(nodes.begin(), nodes.end(), [t_query](const NodeEntry& a, const NodeEntry& b) {
                    return std::abs(a.t - t_query) < std::abs(b.t - t_query);
                    });
                if (nodes.size() > MIN_POINTS) {
                    nodes.resize(MIN_POINTS);
                }
                std::sort(nodes.begin(), nodes.end(), [](const NodeEntry& a, const NodeEntry& b) {
                    return a.t < b.t;
                    });

                // Re-check span after trimming — a large gap in the middle could
                // still leave the trimmed set spanning an unreasonable range.
                const double trimmed_span = nodes.back().t - nodes.front().t;
                if (trimmed_span > MAX_SPAN) {
                    log_manager_.getLogger("geometry")->warn(
                        "getSatellitePV: data gap too large after trimming for {} at week {} tow {:.3f}: "
                        "span={:.1f}s exceeds MAX_SPAN={:.1f}s",
                        satellite, time.week, time.tow, trimmed_span, MAX_SPAN);
                    return zero_result;
                }
            }

            // === STEP 4: Build interpolation arrays sorted by distance from query ===
            // Neville's algorithm converges best when nodes are ordered nearest-first.
            const size_t n = nodes.size();

            // Sort by distance from query for Neville's optimal convergence
            std::sort(nodes.begin(), nodes.end(), [t_query](const NodeEntry& a, const NodeEntry& b) {
                return std::abs(a.t - t_query) < std::abs(b.t - t_query);
            });

            std::vector<double> x(n);
            std::vector<double> yx(n), yy(n), yz(n);
            for (size_t i = 0; i < n; ++i) {
                x[i] = nodes[i].t;
                yx[i] = nodes[i].ptr->position.x();
                yy[i] = nodes[i].ptr->position.y();
                yz[i] = nodes[i].ptr->position.z();
            }

            Eigen::Vector3d pos = Eigen::Vector3d::Zero();
            Eigen::Vector3d vel = Eigen::Vector3d::Zero();

            // Neville's algorithm: simultaneous position and velocity interpolation
            try {
                double px, vx, py, vy, pz, vz;
                Interpolate::nevilleInterpDeriv(x, yx, t_query, px, vx);
                Interpolate::nevilleInterpDeriv(x, yy, t_query, py, vy);
                Interpolate::nevilleInterpDeriv(x, yz, t_query, pz, vz);

                pos = Eigen::Vector3d(px, py, pz);
                vel = Eigen::Vector3d(vx, vy, vz);
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

            // Find chronological window bounds for debug logging without re-sorting
            auto [t_min_it, t_max_it] = std::minmax_element(x.begin(), x.end());
            // Map back to node indices for TOW display
            size_t i_min = static_cast<size_t>(t_min_it - x.begin());
            size_t i_max = static_cast<size_t>(t_max_it - x.begin());

            log_manager_.getLogger("geometry")->debug(
                "getSatellitePV for {} at tow={:.3f}: pos=({:.3f}, {:.3f}, {:.3f}) km, "
                "vel=({:.3f}, {:.3f}, {:.3f}) m/s, |v|={:.3f} m/s, nodes={}, window=[{:.1f},{:.1f}]",
                satellite, time.tow,
                pos.x() / 1000.0, pos.y() / 1000.0, pos.z() / 1000.0,
                vel.x(), vel.y(), vel.z(), vel.norm(),
                n, nodes[i_min].ptr->time.tow, nodes[i_max].ptr->time.tow);

            return { pos, vel };
        }

        // =================================================================
        // Topocentric / attitude / wind-up utilities
        // =================================================================

        Topocentric Geometry::computeTopocentric(const Eigen::Vector3d& rx_pos_ecef,
            const Eigen::Vector3d& sat_pos_ecef) const {
            Topocentric out;

            Eigen::Vector3d delta = sat_pos_ecef - rx_pos_ecef;
            double range = delta.norm();
            if (range < 1.0) {
                log_manager_.getLogger("geometry")->warn("Degenerate range in topocentric computation: range={:.3f} m", range);
                return out;
            }
            if (rx_pos_ecef.norm() < 1e-6) {
                log_manager_.getLogger("geometry")->warn("Invalid receiver ECEF magnitude in topocentric computation");
                return out;
            }

            Coordinates::Llh ref_llh = Coordinates::ecefToLlh(rx_pos_ecef);
            Eigen::Vector3d los_enu = Coordinates::ecefToEnu(delta, ref_llh) / range;

            double elev_deg = std::asin(std::clamp(los_enu.z(), -1.0, 1.0)) * 180.0 / Constants::PI;
            elev_deg = std::max(0.0, std::min(90.0, elev_deg));

            double az_deg = std::atan2(los_enu.x(), los_enu.y()) * 180.0 / Constants::PI;
            if (az_deg < 0.0) az_deg += 360.0;

            out.elevation_deg = elev_deg;
            out.azimuth_deg = az_deg;

            log_manager_.getLogger("geometry")->trace("Topocentric: elev={:.3f}°, az={:.3f}° (lat≈{:.6f}°)",
                elev_deg, az_deg, ref_llh.lat_rad * 180.0 / Constants::PI);
            return out;
        }

        void Geometry::computeSatelliteAttitude(System system,
            const Eigen::Vector3d& sat_pos,
            const Eigen::Vector3d& sat_vel,
            Eigen::Vector3d& d_s,
            Eigen::Vector3d& y_s,
            const GnssTime& time) const {

            ensureSunMoonCached(time);
            Eigen::Vector3d sun_pos = cached_sun_pos_;

            if (sun_pos.norm() < 1e6) {
                sun_pos = Eigen::Vector3d(1.496e11, 0.0, 0.0); // 1 AU fallback
                log_manager_.getLogger("geometry")->warn("No Sun ephemeris for time TOW {0}", time.tow);
            }

            Eigen::Vector3d e = (sun_pos - sat_pos).normalized();

            // Yaw-steering attitude
            Eigen::Vector3d y = sat_vel.normalized().cross(e);
            if (y.norm() < 1e-6)
                y = Eigen::Vector3d(0.0, 1.0, 0.0); // collinearity fallback
            y_s = y.normalized();
            d_s = y_s.cross(-sat_pos.normalized()).normalized();
        }

        void Geometry::resetPhaseWindUp(const std::string& satellite) {
            corrections_.resetPhaseWindUp(satellite);
        }

        void Geometry::resetAllPhaseWindUp() {
            corrections_.resetAllPhaseWindUp();
        }

    } // namespace Model

} // namespace GnssProcessor