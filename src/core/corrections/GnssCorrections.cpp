#include "GnssCorrections.hpp"
#include "Coordinates.hpp"
#include "Constants.hpp"
#include "GnssConfig.hpp"
#include "RinexParser.hpp"
#include "Interpolate.hpp"
#include <algorithm>
#include <cmath>
#include <numeric>
#include <fmt/format.h>

namespace GnssProcessor {

    /**
     * @brief Minimal constructor — create GnssCorrections with only logging.
     *
     * Constructs a GnssCorrections object that holds a reference to the provided
     * LogManager for diagnostic output. No product data (clocks, ERPs, biases,
     * orbits) are populated by this constructor; internal containers are empty
     * and optional external pointers (config_, rinex_parser_) are set to nullptr.
     *
     * Use this constructor when product files are not available at creation time.
     * Callers may populate time-dependent product containers later via
     * setClocksAndErps(...) or use the full constructor when product context is
     * available.
     *
     * @param log_mgr Reference to the LogManager used for logging and diagnostics.
     */
    GnssCorrections::GnssCorrections(LogManager& log_mgr)
        : log_manager_(log_mgr),
        clocks_(), erps_(), biases_(), orbits_(),
        config_(nullptr), rinex_parser_(nullptr),
        ocean_tide_model_(log_mgr) {
        log_manager_.getLogger("corrections")->trace("GnssCorrections constructed (minimal)");
        buildClockIndex();
    }

    /**
     * @brief Construct GnssCorrections with preloaded satellite clocks and ERPs.
     *
     * Initializes the GnssCorrections object with provided satellite clock samples
     * and Earth rotation parameter (ERP) entries. The supplied vectors are copied
     * into the object's internal containers. Optional data (biases, orbits) remain
     * empty and external pointers (config_, rinex_parser_) are set to nullptr.
     *
     * Typical use: call this constructor from product/geometry parsers that already
     * have parsed CLK and ERP products and want to immediately use correction
     * routines that depend on those products.
     *
     * @param log_mgr Reference to the LogManager used for logging and diagnostics.
     * @param clocks Vector of parsed satellite clock samples (@ref ClockData). May be empty.
     * @param erps Vector of parsed Earth rotation parameter samples (@ref ErpData). May be empty.
     */
    GnssCorrections::GnssCorrections(LogManager& log_mgr,
        const std::vector<ClockData>& clocks,
        const std::vector<ErpData>& erps)
        : log_manager_(log_mgr),
        clocks_(clocks), erps_(erps), biases_(), orbits_(),
        config_(nullptr), rinex_parser_(nullptr),
        ocean_tide_model_(log_mgr) {
        log_manager_.getLogger("corrections")->trace("GnssCorrections constructed with clocks and ERPs: {} clocks, {} erps",
            clocks_.size(), erps_.size());
        buildClockIndex();
    }

    /**
     * @brief Full constructor with configuration, product data and RINEX access.
     *
     * Constructs a fully-initialized GnssCorrections instance by copying time-dependent
     * product containers (clocks, erps, biases) into internal storage and retaining
     * non-owning pointers to external helpers for configuration and RINEX access.
     *
     * The constructor:
     * - Copies `clocks`, `erps` and `biases` into the object's internal vectors.
     * - Stores a pointer to `config` (non-owning) in `config_` for runtime options.
     * - Stores a pointer to `rinex` (non-owning) in `rinex_parser_` to access
     *   receiver/antenna metadata when needed.
     *
     * Important: `config` and `rinex` are stored as non-owning pointers. The caller
     * must ensure these referenced objects remain valid for the lifetime of this
     * GnssCorrections instance.
     *
     * @param log_mgr Reference to the LogManager used for logging and diagnostics.
     * @param config Reference to processing configuration (stored as non-owning pointer).
     * @param clocks Vector of parsed satellite clock samples (copied into the object).
     * @param erps Vector of parsed Earth rotation parameters (copied into the object).
     * @param biases Vector of parsed bias entries (copied into the object).
     * @param rinex Reference to a RinexParser providing receiver/antenna metadata (stored as non-owning pointer).
     */
    GnssCorrections::GnssCorrections(LogManager& log_mgr,
        const GnssConfig& config,
        const std::vector<ClockData>& clocks,
        const std::vector<ErpData>& erps,
        const std::vector<BiasData>& biases,
        const RinexParser& rinex)
        : log_manager_(log_mgr),
        clocks_(clocks), erps_(erps), biases_(biases), orbits_(),
        config_(&config), rinex_parser_(&rinex),
        ocean_tide_model_(log_mgr) {
        log_manager_.getLogger("corrections")->trace("GnssCorrections constructed (full) with {} clocks, {} erps, {} biases",
            clocks_.size(), erps_.size(), biases_.size());
        buildClockIndex();
    }

    /**
     * @brief Get satellite clock correction in seconds using available clock products.
     *
     * Validates inputs, attempts to interpolate a clock value from stored CLK samples
     * and applies basic sanity checks before returning the result. This routine
     * provides a best-effort correction for the requested satellite/epoch and will
     * return 0.0 if inputs are invalid, no clock data is available, or the
     * interpolated value is non-finite or outside expected limits.
     *
     * @param satellite Satellite identifier string (e.g., "G01", "E11").
     * @param time Epoch for which the clock correction is requested (GnssTime: week + tow).
     * @return Clock correction in seconds. Returns 0.0 on invalid input, missing data,
     *         interpolation failure or excessively large/non-finite clock values.
     *
     * @note The function delegates interpolation to interpolateClock(...) and logs
     *       diagnostic messages. Typical acceptable clock magnitudes are constrained
     *       to about ±1e-3 seconds (1 ms) and tighter checks are applied to the
     *       interpolated result before it is returned.
     */
    double GnssCorrections::getSatelliteClockCorrection(const std::string& satellite, const GnssTime& time) const {
        // Validating inputs
        if (satellite.empty()) {
            log_manager_.getLogger("ppp_model")->warn("Empty satellite ID for clock correction at TOW {:.3f}", time.tow);
            return 0.0;
        }
        if (time.tow < 0.0 || time.week < 0) {
            log_manager_.getLogger("ppp_model")->warn("Invalid time for clock correction for satellite {}: week {}, TOW {:.3f}",
                satellite, time.week, time.tow);
            return 0.0;
        }
        if (clocks_.empty()) {
            log_manager_.getLogger("ppp_model")->warn("No clock data available for satellite {} at TOW {:.3f}", satellite, time.tow);
            return 0.0;
        }

        // Interpolate clock correction
        double clock_corr = interpolateClock(satellite, time, clocks_);
        //std::string system_str = satellite.substr(0, 1) == "G" ? "GPS" : satellite.substr(0, 1) == "E" ? "Galileo" : "Unknown";
        std::string system_str = PppUtils::getSystemString(satellite);
        if (std::isfinite(clock_corr) && std::abs(clock_corr) < 1e-3) { // Max 1 ms
            log_manager_.getLogger("ppp_model")->trace("Interpolated clock correction for {} ({}) at TOW {:.3f}: {:.3e} s",
                satellite, system_str, time.tow, clock_corr);
            return clock_corr;
        }

        log_manager_.getLogger("ppp_model")->warn("Invalid clock correction for {} ({}) at TOW {:.3f}: {:.3e} s",
            satellite, system_str, time.tow, clock_corr);
        return 0.0;
    }

    void GnssCorrections::buildClockIndex() {
        clock_index_.clear();
        clock_index_.reserve(100);  // Typical number of satellites

        for (const auto& c : clocks_) {
            clock_index_[c.satellite].push_back(&c);
        }

        // Sort each satellite's clocks chronologically
        for (auto& [sat, vec] : clock_index_) {
            std::sort(vec.begin(), vec.end(),
                [](const ClockData* a, const ClockData* b) {
                    return (a->time.week * TimeUtils::SECONDS_IN_WEEK + a->time.tow) <
                        (b->time.week * TimeUtils::SECONDS_IN_WEEK + b->time.tow);
                });
        }

        log_manager_.getLogger("corrections")->debug(
            "Built clock index: {} satellites indexed", clock_index_.size());
    }

    /**
     * @brief Interpolate satellite clock bias at a requested epoch using CLK samples.
     *
     * Performs a robust clock interpolation for `satellite` at `time` using entries
     * in `clocks`. The routine:
     *  - converts GnssTime to continuous seconds (GPS week + TOW),
     *  - selects candidate clock samples for the satellite within a search window,
     *  - sorts samples chronologically and selects a centered interpolation window,
     *  - uses a 4-point barycentric Lagrange interpolant (cubic) when possible,
     *  - falls back to linear interpolation for 2 points or to nearest-sample behavior
     *    when only one valid node is available,
     *  - reduces to a 2-point linear extrapolation when the query lies outside the
     *    node span to avoid high-order extrapolation oscillation.
     *
     * The implementation applies sanity checks (finite values, outlier clock bias
     * rejection, node spacing tolerance) and returns 0.0 for invalid inputs or when
     * a safe interpolant cannot be produced.
     *
     * @param satellite Satellite identifier string (e.g., "G01", "E11"). Must be non-empty.
     * @param time      Query epoch (GnssTime: week + tow) at which the clock is requested.
     * @param clocks    Container of available ClockData samples (may be unsorted).
     * @return Interpolated clock bias in seconds. Returns 0.0 on invalid input, no valid
     *         nodes, or when interpolation produces a non-finite / unsafe result.
     *
     * @details
     * - Typical CLK epoch spacing is ~30 s; cubic interpolation is used to reduce
     *   interpolation error at sub-epoch query times. For extrapolation the routine
     *   prefers linear behaviour to limit oscillation.
     * - Duplicate or nearly-equal node times (|Δt| < 1e-9 s) are treated as invalid
     *   for barycentric weight computation and such nodes are effectively skipped.
     * - Time conversion uses continuousGpsSeconds() (week*604800 + tow) and no
     *   leap-second adjustment is performed here.
     *
     * @note Complexity: O(m + n log n) where m is number of `clocks` scanned and n
     *       is the number of candidate nodes retained for interpolation (n ≤ 4 in normal use).
     * @note The function logs diagnostic information via the LogManager and assumes
     *       the calling object owns a valid logger.
     * @see barycentricWeights(), barycentricEval()
     */
    double GnssCorrections::interpolateClock(const std::string& satellite,
        const GnssTime& time,
        const std::vector<ClockData>& clocks) const
    {
        const SatelliteClockInterpolationMode mode = config_ ? config_->getSatelliteClockInterpolationMode() : SatelliteClockInterpolationMode::Linear;
        const size_t INTERP_POINTS = (mode == SatelliteClockInterpolationMode::Linear) ? 2 : 9;

        constexpr double MAX_WINDOW = 3600.0;
        constexpr double MAX_CLOCK_BIAS = 1e9;

        if (satellite.empty()) {
            log_manager_.getLogger("corrections")->debug("Clock interp: empty satellite ID");
            return 0.0;
        }

        auto it = clock_index_.find(satellite);
        if (it == clock_index_.end() || it->second.empty()) {
            log_manager_.getLogger("corrections")->debug(
                "Clock interp for {}: no clock data in index", satellite);
            return 0.0;
        }

        const auto& sat_clocks = it->second;
        const double t_query = TimeUtils::continuousGpsSeconds(time);

        // Find the nearest node to t_query (not just the first node >= t_query).
        // Using the nearest node as the center anchor ensures the interpolation
        // window only shifts when the query crosses the MIDPOINT between two
        // consecutive CLK epochs. This eliminates the sawtooth discontinuity
        // that occurs with lower_bound-based centering, where the window jumps
        // by one node every CLK interval (producing cm-level interpolation
        // discontinuities at each CLK epoch boundary).
        auto upper_it = std::lower_bound(sat_clocks.begin(), sat_clocks.end(), t_query,
            [](const ClockData* c, double t) {
                return TimeUtils::continuousGpsSeconds(c->time) < t;
            });

        // Determine the index of the nearest node to t_query
        size_t nearest_idx = 0;
        if (upper_it == sat_clocks.end()) {
            nearest_idx = sat_clocks.size() - 1;
        }
        else if (upper_it == sat_clocks.begin()) {
            nearest_idx = 0;
        }
        else {
            size_t hi = std::distance(sat_clocks.begin(), upper_it);
            size_t lo = hi - 1;
            double t_lo = TimeUtils::continuousGpsSeconds(sat_clocks[lo]->time);
            double t_hi = TimeUtils::continuousGpsSeconds(sat_clocks[hi]->time);
            nearest_idx = (t_query - t_lo <= t_hi - t_query) ? lo : hi;
        }

        // Center the window on the nearest node
        size_t half_before = INTERP_POINTS / 2;
        size_t start_idx = 0;
        if (nearest_idx > half_before) {
            start_idx = nearest_idx - half_before;
        }
        if (start_idx + INTERP_POINTS > sat_clocks.size()) {
            start_idx = (sat_clocks.size() > INTERP_POINTS) ? sat_clocks.size() - INTERP_POINTS : 0;
        }
        size_t end_idx = std::min(start_idx + INTERP_POINTS, sat_clocks.size());

        // Collect and validate nodes
        std::vector<const ClockData*> nodes;
        nodes.reserve(INTERP_POINTS);

        for (size_t i = start_idx; i < end_idx; ++i) {
            const ClockData* c = sat_clocks[i];
            if (!std::isfinite(c->clock_bias)) continue;
            if (std::abs(c->clock_bias) > MAX_CLOCK_BIAS) continue;

            double tc = TimeUtils::continuousGpsSeconds(c->time);
            if (std::abs(tc - t_query) < MAX_WINDOW) {
                nodes.push_back(c);
            }
        }

        if (nodes.empty()) {
            log_manager_.getLogger("corrections")->warn(
                "Clock interp for {} at tow={:.3f}: no valid nodes in window",
                satellite, time.tow);
            return 0.0;
        }

        // Handle extrapolation: always use linear to avoid oscillation
        bool extrapolating = false;
        if (nodes.size() >= 2) {
            double t_first = TimeUtils::continuousGpsSeconds(nodes.front()->time);
            double t_last = TimeUtils::continuousGpsSeconds(nodes.back()->time);
            if (t_query < t_first || t_query > t_last) {
                extrapolating = true;
                if (nodes.size() > 2) {
                    std::sort(nodes.begin(), nodes.end(), [t_query](const ClockData* a, const ClockData* b) {
                        return std::abs(TimeUtils::continuousGpsSeconds(a->time) - t_query) <
                            std::abs(TimeUtils::continuousGpsSeconds(b->time) - t_query);
                        });
                    nodes.resize(2);
                    std::sort(nodes.begin(), nodes.end(), [](const ClockData* a, const ClockData* b) {
                        return TimeUtils::continuousGpsSeconds(a->time) < TimeUtils::continuousGpsSeconds(b->time);
                        });
                }
                log_manager_.getLogger("corrections")->debug(
                    "Clock extrapolation for {} at tow={:.3f}: using linear with {} pts",
                    satellite, time.tow, nodes.size());
            }
        }

        // Build interpolation arrays
        const size_t n = nodes.size();
        if (n == 0) return 0.0;
        if (n == 1) return nodes[0]->clock_bias;

        std::vector<double> x(n);
        std::vector<double> y(n);
        for (size_t i = 0; i < n; ++i) {
            x[i] = TimeUtils::continuousGpsSeconds(nodes[i]->time);
            y[i] = nodes[i]->clock_bias;
        }

        // Perform interpolation based on mode and available points
        double result = 0.0;

        if (n == 2 || mode == SatelliteClockInterpolationMode::Linear) {
            result = Interpolate::linearInterp(x, y, t_query);

            log_manager_.getLogger("corrections")->debug(
                "Clock interp (linear) for {} at tow={:.3f}: {} pts, span={:.1f}s, "
                "result={:.3e}s ({:.3f}m), extrap={}",
                satellite, time.tow, n, x.back() - x.front(),
                result, result * Constants::SPEED_OF_LIGHT,
                extrapolating ? "YES" : "no");
        }
        else {
            std::vector<double> w = Interpolate::barycentricWeights(x);
            result = Interpolate::barycentricEval(x, w, y, t_query);

            log_manager_.getLogger("corrections")->debug(
                "Clock interp (lagrange) for {} at tow={:.3f}: {} pts, span={:.1f}s, "
                "result={:.3e}s ({:.3f}m), extrap={}",
                satellite, time.tow, n, x.back() - x.front(),
                result, result * Constants::SPEED_OF_LIGHT,
                extrapolating ? "YES" : "no");
        }

        return result;
    }



    /**
     * @brief Compute the relativistic satellite clock correction (seconds).
     *
     * Computes the classical relativistic correction used in GNSS clock modelling:
     *   dtr = -2 * (r · v) / c^2
     * where r and v are the satellite ECEF position (meters) and velocity (meters/second)
     * at transmit time, and c is the speed of light. The result is returned in seconds.
     *
     * @param satellite Satellite identifier (e.g., "G01"). Used only for logging/diagnostics.
     * @param sat_pos   Satellite ECEF position vector in meters.
     * @param sat_vel   Satellite ECEF velocity vector in meters per second.
     * @return Relativistic clock correction in seconds. Returns 0.0 if inputs are invalid,
     *         non-finite, or produce an implausibly large correction (sanity-checked).
     *
     * @remarks
     * - The function expects `sat_pos` in meters and `sat_vel` in meters/second (ECEF).
     * - Typical magnitudes are on the order of tens of nanoseconds; callers should
     *   convert to meters (range equivalent) by multiplying with the speed of light when needed.
     * - This routine does not apply additional Schwarzschild-style terms; those are
     *   not part of per-epoch PPP clock corrections here.
     * - Logging is emitted for invalid or out-of-range inputs.
     * @see Constants::SPEED_OF_LIGHT
     */
    double GnssCorrections::computeRelativisticCorrection(const std::string& satellite, const Eigen::Vector3d& sat_pos,
        const Eigen::Vector3d& sat_vel) const {
        // GNSS relativistic correction used in PPP is the well-known term:
        //
        //   dtr = -2 * (r · v) / c^2   [seconds]
        //
        // where r and v are the satellite ECEF position/velocity at transmit time.
        // Do NOT add a "Schwarzschild / -2GM/(c^2 r)" term here; that is not applied as a per-epoch correction in
        // standard GNSS PPP observation modeling (it is already absorbed by clock/product definitions and constants).
        constexpr double C_SQUARED = Constants::SPEED_OF_LIGHT * Constants::SPEED_OF_LIGHT;

        // Conservative sanity limits
        constexpr double MAX_CORRECTION = 1e-6;        // 1 microsecond (~300 m), far above expected (~tens of ns)
        constexpr double MIN_POSITION_NORM = 1.8e7;    // ~20,000 km
        constexpr double MAX_POSITION_NORM = 3.2e7;    // ~32,000 km
        constexpr double MAX_VELOCITY_NORM = 1.5e4;    // ~15 km/s

        std::string system_str = PppUtils::getSystemString(satellite);

        if (!sat_pos.allFinite() || !sat_vel.allFinite()) {
            log_manager_.getLogger("ppp_model")->warn(
                "Invalid relativistic inputs for {} ({}): sat_pos=({:.3f}, {:.3f}, {:.3f}), sat_vel=({:.3f}, {:.3f}, {:.3f})",
                satellite, system_str, sat_pos.x(), sat_pos.y(), sat_pos.z(), sat_vel.x(), sat_vel.y(), sat_vel.z());
            return 0.0;
        }

        const double pos_norm = sat_pos.norm();
        const double vel_norm = sat_vel.norm();
        if (pos_norm < MIN_POSITION_NORM || pos_norm > MAX_POSITION_NORM || vel_norm > MAX_VELOCITY_NORM) {
            log_manager_.getLogger("ppp_model")->warn(
                "Unrealistic relativistic input norms for {} ({}): pos_norm={:.3e} m, vel_norm={:.3e} m/s",
                satellite, system_str, pos_norm, vel_norm);
            return 0.0;
        }

        const double r_dot_v = sat_pos.dot(sat_vel);
        const double correction = -2.0 * r_dot_v / C_SQUARED;

        if (!std::isfinite(correction) || std::abs(correction) > MAX_CORRECTION) {
            log_manager_.getLogger("ppp_model")->warn(
                "Excessive relativistic correction for {} ({}): {:.3e} s, r_dot_v={:.3e}",
                satellite, system_str, correction, r_dot_v);
            return 0.0;
        }

        log_manager_.getLogger("ppp_model")->trace(
            "Relativistic correction for {} ({}): {:.3e} s, r_dot_v={:.3e}",
            satellite, system_str, correction, r_dot_v);

        return correction;
    }

    /**
     * @brief Compute the Shapiro (relativistic path) delay in meters.
     *
     * Computes the additional path length caused by spacetime curvature (primarily
     * due to Earth's gravitational potential) using the standard Shapiro formula:
     *   rpath = (2 * GM / c^2) * ln((r_sat + r_rec + r_sr) / (r_sat + r_rec - r_sr))
     * where:
     *  - GM is the Earth's gravitational constant (m^3/s^2),
     *  - c is the speed of light (m/s),
     *  - r_sat = |sat_pos|, r_rec = |rx_pos|, r_sr = |sat_pos - rx_pos|.
     *
     * @param satellite Satellite identifier string (e.g., "G01"). Used for logging.
     * @param rx_pos    Receiver ECEF position vector (meters).
     * @param sat_pos   Satellite ECEF position vector (meters).
     * @return Shapiro delay in meters. Returns 0.0 for invalid/non-finite inputs,
     *         unrealistic geometry (e.g., too-small range), or when the computed
     *         delay is non-finite or exceeds safe bounds.
     *
     * @note The routine uses @c Constants::GRAVITATIONAL_CONSTANT and
     *       @c Constants::SPEED_OF_LIGHT. Inputs are expected in meters (ECEF).
     * @note This implementation performs input validation and clamps/returns 0.0
     *       on suspicious values (see surrounding implementation for thresholds).
     * @see https://gssc.esa.int/navipedia/index.php/Relativistic_Path_Range_Effect
     */
    double GnssCorrections::computeShapiroDelay(const std::string& satellite, const Eigen::Vector3d& rx_pos,
        const Eigen::Vector3d& sat_pos) const {
        // Relativistic path range effect (Shapiro delay)
        // Reference: https://gssc.esa.int/navipedia/index.php/Relativistic_Path_Range_Effect
        // Formula: rpath = (2*mu/c²) * ln((rsat + rrec + rrs) / (rsat + rrec - rrs))
        // Output: delay in meters
        
        constexpr double GM = Constants::GRAVITATIONAL_CONSTANT; // GM = 3.986005e14 m³/s² (WGS-84)
        constexpr double C = Constants::SPEED_OF_LIGHT;          // c = 299792458 m/s
        constexpr double C_SQUARED = C * C;
        constexpr double MIN_POSITION_NORM = 1.8e7; // approx. 20,000 km
        constexpr double MAX_POSITION_NORM = 3.2e7; // approx. 32,000 km
        constexpr double MAX_DELAY = 0.03;          // Max ~30 mm (typical is ~2 cm)

        // Validate inputs
        std::string system_str = PppUtils::getSystemString(satellite);
        if (!rx_pos.allFinite() || !sat_pos.allFinite()) {
            log_manager_.getLogger("ppp_model")->warn("Invalid Shapiro delay inputs for {} ({}): rx_pos=({:.3f}, {:.3f}, {:.3f}), sat_pos=({:.3f}, {:.3f}, {:.3f})",
                satellite, system_str, rx_pos.x(), rx_pos.y(), rx_pos.z(), sat_pos.x(), sat_pos.y(), sat_pos.z());
            return 0.0;
        }
        
        const double rsat = sat_pos.norm();           // Satellite position norm
        const double rrec = rx_pos.norm();            // Receiver position norm
        const double rrs = (sat_pos - rx_pos).norm(); // Range (satellite to receiver)
        
        if (rrs < 1e3 || rsat < MIN_POSITION_NORM || rsat > MAX_POSITION_NORM) {
            log_manager_.getLogger("ppp_model")->warn("Unrealistic Shapiro delay norms for {} ({}): range={:.3e} m, sat_norm={:.3e} m",
                satellite, system_str, rrs, rsat);
            return 0.0;
        }

        // Compute Shapiro delay in meters (matches MATLAB reference exactly)
        // rpath = (2*mu/c²) * ln((rsat + rrec + rrs) / (rsat + rrec - rrs))
        const double delay = (2.0 * GM / C_SQUARED) * std::log((rsat + rrec + rrs) / (rsat + rrec - rrs));
        
        if (!std::isfinite(delay) || std::abs(delay) > MAX_DELAY) {
            log_manager_.getLogger("ppp_model")->warn("Invalid Shapiro delay for {} ({}): {:.3e} m, range={:.3e} m, sat_norm={:.3e} m, rx_norm={:.3e} m",
                satellite, system_str, delay, rrs, rsat, rrec);
            return 0.0;
        }

        log_manager_.getLogger("ppp_model")->trace("Shapiro delay for {} ({}): {:.6f} m, range={:.3e} m, sat_norm={:.3e} m, rx_norm={:.3e} m",
            satellite, system_str, delay, rrs, rsat, rrec);
        return delay;
    }

    /**
     * @brief Compute the Sagnac correction (meters) due to Earth's rotation.
     *
     * Computes the path-length correction caused by the rotation of the Earth
     * during signal propagation using the standard Sagnac approximation:
     *   correction = (ω_E / c) * (x_s * y_r - y_s * x_r)
     *
     * where ω_E is Earth's angular velocity and c is the speed of light. Inputs
     * are expected as ECEF position vectors in meters.
     *
     * @param rx_pos Receiver ECEF position vector (meters).
     * @param sat_pos Satellite ECEF position vector (meters).
     * @return Sagnac correction in meters. Returns 0.0 for invalid/non-finite inputs
     *         or when input norms are outside conservative expected ranges.
     *
     * @note Uses @c Constants::SPEED_OF_LIGHT. The implementation applies input
     *       validation and logs warnings for unrealistic norms.
     */
    double GnssCorrections::computeSagnacCorrection(const Eigen::Vector3d& rx_pos,
        const Eigen::Vector3d& sat_pos) const {
        // Sagnac effect: Correction for Earth's rotation during signal propagation
        // Formula: (ω_E / c) * (x_s * y_r - y_s * x_r), where ω_E is Earth's angular velocity, c is speed of light
        constexpr double EARTH_ANGULAR_VELOCITY = 7.2921151467e-5; // rad/s
        constexpr double C = Constants::SPEED_OF_LIGHT; // m/s
        constexpr double MAX_SAGNAC_CORRECTION = 50.0; // Max expected correction in meters
        constexpr double MIN_RX_NORM = 6e6; // Earth radius approx. 6.3e6 m
        constexpr double MAX_RX_NORM = 7e6;
        constexpr double MIN_SAT_NORM = 2e7; // Typical GNSS orbit approx. 2e7 m
        constexpr double MAX_SAT_NORM = 3e7;

        // Validate inputs
        if (!rx_pos.allFinite() || !sat_pos.allFinite()) {
            log_manager_.getLogger("ppp_model")->warn("Invalid Sagnac inputs: rx_pos=({}, {}, {}), sat_pos=({}, {}, {})",
                rx_pos.x(), rx_pos.y(), rx_pos.z(), sat_pos.x(), sat_pos.y(), sat_pos.z());
            return 0.0;
        }
        double rx_norm = rx_pos.norm();
        double sat_norm = sat_pos.norm();
        if (rx_norm < MIN_RX_NORM || rx_norm > MAX_RX_NORM || sat_norm < MIN_SAT_NORM || sat_norm > MAX_SAT_NORM) {
            log_manager_.getLogger("ppp_model")->warn("Unrealistic Sagnac input norms: rx_norm={:.3e} m, sat_norm={:.3e} m, "
                "rx_pos=({}, {}, {}), sat_pos=({}, {}, {})",
                rx_norm, sat_norm, rx_pos.x(), rx_pos.y(), rx_pos.z(), sat_pos.x(), sat_pos.y(), sat_pos.z());
            return 0.0;
        }

        // Compute correction: (ω_E / c) * (x_s * y_r - y_s * x_r)
        double cross_term = sat_pos.x() * rx_pos.y() - sat_pos.y() * rx_pos.x();
        double correction = (EARTH_ANGULAR_VELOCITY / C) * cross_term;
        if (!std::isfinite(correction) || std::abs(correction) > MAX_SAGNAC_CORRECTION) {
            log_manager_.getLogger("ppp_model")->warn("Excessive Sagnac correction: {:.3e} m, cross_term: {:.3e}, "
                "rx_pos=({}, {}, {}), sat_pos=({}, {}, {})",
                correction, cross_term, rx_pos.x(), rx_pos.y(), rx_pos.z(), sat_pos.x(), sat_pos.y(), sat_pos.z());
            return 0.0;
        }

        log_manager_.getLogger("ppp_model")->trace("Sagnac correction: {:.3f} m, cross_term: {:.3e}, "
            "rx_pos=({}, {}, {}), sat_pos=({}, {}, {})",
            correction, cross_term, rx_pos.x(), rx_pos.y(), rx_pos.z(), sat_pos.x(), sat_pos.y(), sat_pos.z());
        return correction;
    }

    /**
     * @brief Compute pole tide displacement (ECEF) for a receiver position at an epoch.
     *
     * Computes the pole tide station displacement in ECEF coordinates using a
     * simplified IERS-style model and Earth rotation parameter (ERP) entries
     * available in the object's ERP container. The implementation:
     *  - selects the ERP sample closest in time to `time`,
     *  - converts polar motion parameters xp/yp (stored in arcseconds) to radians,
     *  - subtracts the IERS 2010 mean pole to obtain m1/m2 (radians),
     *  - computes radial, north and east displacements using simplified coefficients,
     *  - converts the radial/north/east displacement to ECEF and returns it.
     *
     * This is a small, station-level displacement (typically millimetres or less)
     * to account for polar motion-driven Earth deformations.
     *
     * @param rx_pos Receiver ECEF position vector (meters). Must be finite and > 1e3 m.
     * @param time   Epoch for which the pole tide correction is required (GnssTime: week + tow).
     * @return ECEF displacement vector in meters to be applied to station coordinates.
     *         Returns a zero vector on invalid inputs, missing/invalid ERP data, or
     *         if the computed correction is non-finite or exceeds conservative limits.
     *
     * @note The routine expects ERP xp/yp values in arcseconds (converted at parse
     *       time from the native ERP file units). The mean pole is computed in
     *       milliarcseconds per the IERS 2010 linear model. The simplified
     *       coefficients (radial: -0.033, north: -0.009, east: -0.009) follow
     *       IERS 2010 conventions (Section 7.1.4, Eq. 7.26).
     * @note Output norm is checked (max 0.05 m = 50 mm, which is extremely
     *       conservative — realistic pole tide displacements are < ~25 mm).
     * @thread_safety Safe to call concurrently with independent inputs (no static state).
     * @see ErpData, GnssTime
     */
    Eigen::Vector3d GnssCorrections::computePoleTideCorrection(const Eigen::Vector3d& rx_pos, const GnssTime& time) const {
        // Validate inputs
        if (!rx_pos.allFinite() || rx_pos.norm() < 1e3) {
            log_manager_.getLogger("ppp_model")->warn("Invalid receiver position for pole tide correction at TOW {:.3f}: rx_pos=({:.3f}, {:.3f}, {:.3f}), norm={:.3e}",
                time.tow, rx_pos.x(), rx_pos.y(), rx_pos.z(), rx_pos.norm());
            return Eigen::Vector3d::Zero();
        }
        if (time.tow < 0.0 || time.week < 0) {
            log_manager_.getLogger("ppp_model")->warn("Invalid time for pole tide correction: week {}, TOW {:.3f}",
                time.week, time.tow);
            return Eigen::Vector3d::Zero();
        }
        if (erps_.empty()) {
            log_manager_.getLogger("ppp_model")->warn("No ERP data available for pole tide correction at TOW {:.3f}", time.tow);
            return Eigen::Vector3d::Zero();
        }

        // Find closest ERP data (use continuous GPS seconds to handle week boundaries)
        const double t_query = TimeUtils::continuousGpsSeconds(time);
        auto it = std::min_element(erps_.begin(), erps_.end(),
            [t_query](const ErpData& a, const ErpData& b) {
                return std::abs(TimeUtils::continuousGpsSeconds(a.time) - t_query) <
                    std::abs(TimeUtils::continuousGpsSeconds(b.time) - t_query);
            });
        if (it == erps_.end() || !std::isfinite(it->xp) || !std::isfinite(it->yp)) {
            log_manager_.getLogger("ppp_model")->trace("No valid ERP data for pole tide correction at TOW {:.3f}", time.tow);
            return Eigen::Vector3d::Zero();
        }

        // Use proper geodetic latitude from centralized Coordinates (not geocentric asin(z/r))
        const Coordinates::Llh llh = Coordinates::ecefToLlh(rx_pos);
        double phi = llh.lat_rad;     // Geodetic latitude (radians)
        double lambda = llh.lon_rad;  // Geodetic longitude (radians)

        // IERS 2010 mean pole (linear model, epoch 2000.0)
        // Reference: IERS Conventions (2010), Section 7.1.4
        // xp_mean = 55.974 + 1.8243 * (t - 2000.0) mas
        // yp_mean = 346.346 + 1.7896 * (t - 2000.0) mas
        const double mjd = TimeUtils::gpsTimeToMjd(time);
        const double decimal_year = TimeUtils::mjdToDecimalYear(mjd);

        const double xp_mean_mas = 55.974 + 1.8243 * (decimal_year - 2000.0);   // milliarcseconds
        const double yp_mean_mas = 346.346 + 1.7896 * (decimal_year - 2000.0);  // milliarcseconds

        // Convert ERP values (arcseconds) and mean pole (milliarcseconds) to radians
        constexpr double AS2RAD = 4.848136811e-6;   // arcsecond to radians
        constexpr double MAS2RAD = AS2RAD * 1e-3;   // milliarcsecond to radians

        double m1 = (it->xp * AS2RAD) - (xp_mean_mas * MAS2RAD);  // xp - xp_mean (radians)
        double m2 = -(it->yp * AS2RAD) + (yp_mean_mas * MAS2RAD); // -(yp - yp_mean) (radians, IERS sign convention)

        // Pole tide displacement in radial, north, east directions (meters)
        // IERS 2010, Eq. 7.26 (simplified, using nominal Love/Shida numbers)
        double radial = -0.033 * std::sin(2.0 * phi) * (m1 * std::cos(lambda) + m2 * std::sin(lambda));
        double north = -0.009 * std::cos(2.0 * phi) * (m1 * std::cos(lambda) + m2 * std::sin(lambda));
        double east = 0.009 * std::cos(phi) * (m1 * std::sin(lambda) - m2 * std::cos(lambda));

        // Convert to ECEF coordinates
        Eigen::Vector3d correction(
            (radial * std::cos(phi) * std::cos(lambda) - north * std::sin(phi) * std::cos(lambda) - east * std::sin(lambda)),
            (radial * std::cos(phi) * std::sin(lambda) - north * std::sin(phi) * std::sin(lambda) + east * std::cos(lambda)),
            (radial * std::sin(phi) + north * std::cos(phi))
        );

        if (!correction.allFinite() || correction.norm() > 0.05) { // Max 50 mm (conservative limit)
            log_manager_.getLogger("ppp_model")->warn("Invalid pole tide correction at TOW {:.3f}: ({:.3e}, {:.3e}, {:.3e}) m, norm={:.3e}",
                time.tow, correction.x(), correction.y(), correction.z(), correction.norm());
            return Eigen::Vector3d::Zero();
        }

        log_manager_.getLogger("ppp_model")->trace("Pole tide correction at TOW {:.3f}: ({:.3e}, {:.3e}, {:.3e}) m, m1={:.3e} rad, m2={:.3e} rad",
            time.tow, correction.x(), correction.y(), correction.z(), m1, m2);
        return correction;
    }

    /**
     * @brief Compute a simplified ocean-tide loading displacement (ECEF).
     *
     * Provides a lightweight, placeholder ocean tide correction using a simple
     * sinusoidal M2-like constituent. The routine computes a small station
     * displacement (radial, north, east) based on approximate geodetic latitude
     * and time-of-day, then converts that displacement into ECEF coordinates.
     *
     * This implementation is intentionally simple and intended for testing or
     * where full ocean-loading models are not available. For production/precise
     * results use a full ocean-loading model (tidal constituents + load Love
     * numbers, or external gridded loading data).
     *
     * @param rx_pos Receiver ECEF position vector (meters). Must be finite and non-degenerate.
     * @param time   Epoch for which the ocean tide is evaluated (`GnssTime`: week + tow).
     * @return ECEF displacement vector (`Eigen::Vector3d`) in meters. Returns
     *         `Eigen::Vector3d::Zero()` on invalid inputs or if the computed
     *         correction is non-finite or exceeds conservative limits.
     *
     * @details
     * - The routine computes geodetic latitude as `asin(z / |r|)` (approximate).
     * - A single sinusoidal constituent is used:
     *     amplitude * cos(2π * t / period) scaled by latitude for the radial/north
     *   displacement (no east component in this simplified model).
     * - Default amplitude used here is 0.01 m (1 cm) and `period` corresponds to
     *   the M2 principal lunar tidal constituent (≈ 12.4206 hours).
     *
     * @note This is a simplified placeholder. Replace with a proper ocean-loading
     *       model (e.g., FES, GOT, or IERS-conformant loading grids) for accurate
     *       station displacements. Output norm is checked (max 0.1 m) and the
     *       function logs/warns on suspicious inputs.
     * @thread_safety Safe to call concurrently with independent inputs (no static state).
     */
    Eigen::Vector3d GnssCorrections::computeOceanTideCorrection(const Eigen::Vector3d& rx_pos, const GnssTime& time) const {
        return ocean_tide_model_.computeDisplacement(rx_pos, time);
    }

    void GnssCorrections::setOceanTideCoefficients(const OtlStationCoefficients& coeffs) {
        ocean_tide_model_.setStationCoefficients(coeffs);
    }

    bool GnssCorrections::loadOceanTideFromBLQ(const std::string& blq_path, const std::string& station_name) {
        OtlStationCoefficients coeffs;
        if (ocean_tide_model_.parseBLQ(blq_path, station_name, coeffs)) {
            ocean_tide_model_.setStationCoefficients(coeffs);
            return true;
        }
        return false;
    }

    bool GnssCorrections::hasOceanTideCoefficients() const {
        return ocean_tide_model_.hasStationCoefficients();
    }

    /**
     * @brief Estimate atmospheric loading radial displacement at a receiver position.
     */
    double GnssCorrections::computeAtmosphericLoadingCorrection(const Eigen::Vector3d& rx_pos,
        const GnssTime& time) const {
        double latitude = std::asin(rx_pos.z() / rx_pos.norm()) * 180.0 / Constants::PI;
        double t = fmod(time.tow, TimeUtils::SECONDS_IN_DAY);
        double amplitude = 0.001;
        double correction = amplitude * std::cos(2.0 * Constants::PI * t / TimeUtils::SECONDS_IN_DAY) * std::cos(latitude * Constants::PI / 180.0);
        log_manager_.getLogger("ppp_model")->trace("Atmospheric loading correction: {} m", correction);
        return correction;
    }

    /**
     * @brief Retrieve a bias correction for a satellite/signal at a given epoch.
     */
    double GnssCorrections::getBiasCorrection(const std::string& satellite,
        const std::string& freq_type,
        const GnssTime& time) const {
        if (satellite.empty() || freq_type.empty()) {
            log_manager_.getLogger("ppp_model")->warn("Invalid inputs for bias correction: satellite='{}', freq_type='{}' at TOW {:.3f}",
                satellite, freq_type, time.tow);
            return 0.0;
        }
        if (time.tow < 0.0 || time.week < 0) {
            log_manager_.getLogger("ppp_model")->warn("Invalid time for bias correction for satellite {}: week {}, TOW {:.3f}",
                satellite, time.week, time.tow);
            return 0.0;
        }

        const double t_query = TimeUtils::continuousGpsSeconds(time);

        auto it = std::find_if(biases_.begin(), biases_.end(),
            [&satellite, &freq_type, t_query](const BiasData& b) {
                if (b.satellite != satellite) return false;
                if (b.signal != freq_type) return false;
                double t_start = TimeUtils::continuousGpsSeconds(b.start_time);
                double t_end = TimeUtils::continuousGpsSeconds(b.end_time);
                return (t_start <= t_query && t_query <= t_end);
            });
        if (it != biases_.end()) {
            log_manager_.getLogger("ppp_model")->trace("Bias correction for {} ({}) {} at TOW {:.3f}: {:.3e} m",
                satellite, PppUtils::getSystemString(satellite),
                freq_type, time.tow, it->bias);
            return it->bias;
        }

        if (!biases_.empty()) {
            log_manager_.getLogger("ppp_model")->debug("No matching bias entry found for {} {} at continuous_time={:.3f}. Known bias entries={}",
                satellite, freq_type, t_query, biases_.size());
        }

        log_manager_.getLogger("ppp_model")->trace("No bias correction for {} ({}) {} at TOW {:.3f}",
            satellite, PppUtils::getSystemString(satellite),
            freq_type, time.tow);
        return 0.0;
    }

    /**
     * @brief Compute solid Earth tide displacement along the receiver->satellite line-of-sight.
     *
     * Implements the IERS 2010 solid Earth tide model (Conventions Section 7.1.1):
     *   Step 1: degree 2 + degree 3 with nominal Love numbers and body positions
     *   Step 2: frequency-dependent corrections from Tables 7.3a (long-period + diurnal)
     *           and 7.3b (semi-diurnal)
     *
     * Tide system: This function produces the **conventional tide-free** displacement,
     * consistent with IGS (SP3/CLK products, ITRF coordinates), IERS 2010 Conventions,
     * and RTKLIB. The permanent (zero-frequency) tide deformation is included in the
     * Step 2 long-period (m=0) corrections via the 055.555 constituent, so the full
     * output removes the permanent tide from the station coordinates — matching the
     * conventional tide-free system used by all standard GNSS processing software.
     *
     * Returns the displacement projected onto the receiver->satellite LOS in meters.
     *
     * @param receiver_pos Receiver ECEF position vector (meters).
     * @param sat_pos      Satellite ECEF position vector (meters).
     * @param sun_pos      Sun ECEF position vector (meters).
     * @param moon_pos     Moon ECEF position vector (meters).
     * @param time         Epoch for Step 2 Doodson argument computation (GnssTime: week + tow).
     * @return LOS displacement in meters. Returns 0.0 for invalid inputs.
     *
     * @see IERS Conventions (2010), Section 7.1.1, Tables 7.3a and 7.3b
     * @see RTKLIB tidedisp() for equivalent implementation
     */
    double GnssCorrections::computeSolidEarthTide(const Eigen::Vector3d& receiver_pos, const Eigen::Vector3d& sat_pos,
        const Eigen::Vector3d& sun_pos, const Eigen::Vector3d& moon_pos,
        const GnssTime& time) const {
        // IERS 2010 nominal Love numbers
        constexpr double h2 = 0.6078;
        constexpr double l2 = 0.0847;
        constexpr double h3 = 0.292;
        constexpr double l3 = 0.015;

        // Mass ratios (IERS 2010): (M_sun / M_earth), (M_moon / M_earth)
        constexpr double MS2E = 332946.0;
        constexpr double MM2E = 0.01230002;

        // WGS-84 ellipsoid
        const double a = Constants::Ellipsoid::WGS84::a;

        if (!receiver_pos.allFinite() || !sat_pos.allFinite() || !sun_pos.allFinite() || !moon_pos.allFinite()) {
            log_manager_.getLogger("ppp_model")->warn("Solid Earth tide: non-finite inputs");
            return 0.0;
        }

        // LOS from satellite toward receiver (sat->rx), matching the MATLAB convention.
        const Eigen::Vector3d rho = receiver_pos - sat_pos;
        const double rho_norm = rho.norm();
        if (!(rho_norm > 0.0)) return 0.0;
        const Eigen::Vector3d los = rho / rho_norm;

        const double r = receiver_pos.norm();
        if (!(r > 0.0)) return 0.0;

        // Geodetic lat/lon/height using centralized Coordinates
        const Coordinates::Llh llh = Coordinates::ecefToLlh(receiver_pos);
        const double lat = llh.lat_rad;
        const double lambda = llh.lon_rad;

        const double sin_lat = std::sin(lat);
        const double cos_lat = std::cos(lat);
        const double trm = 3.0 * sin_lat * sin_lat - 1.0;

        // Small latitude-dependent corrections (IERS 2010, simplified)
        const double dh = -0.0006 * trm;
        const double dl =  0.0002 * trm;

        const double h2_eff = h2 + dh;
        const double l2_eff = l2 + dl;

        // Receiver unit vector
        const Eigen::Vector3d u = receiver_pos / r;

        auto tideBody = [&](const Eigen::Vector3d& body_pos, double mass_ratio, int deg) -> Eigen::Vector3d {
            const double d = body_pos.norm();
            if (!(d > 0.0)) return Eigen::Vector3d::Zero();

            const Eigen::Vector3d ub = body_pos / d;
            const double cos_psi = ub.dot(u);

            if (deg == 2) {
                const double P2 = (3.0 * cos_psi * cos_psi - 1.0) / 2.0;
                const Eigen::Vector3d t = ub - cos_psi * u;
                const double scale = mass_ratio * std::pow(a, 4) / std::pow(d, 3);
                return scale * ((h2_eff * u) * P2 + (3.0 * l2_eff * cos_psi) * t);
            }
            else {
                const double P3 = (5.0 * cos_psi * cos_psi * cos_psi - 3.0 * cos_psi) / 2.0;
                const double dP3 = (15.0 / 2.0) * cos_psi * cos_psi - (3.0 / 2.0);
                const Eigen::Vector3d t = ub - cos_psi * u;
                const double scale = mass_ratio * std::pow(a, 5) / std::pow(d, 4);
                return scale * ((h3 * u) * P3 + (l3 * dP3) * t);
            }
        };

        // Step 1: degree-2 Sun + Moon, degree-3 Moon
        const Eigen::Vector3d d2s = tideBody(sun_pos, MS2E, 2);
        const Eigen::Vector3d d2m = tideBody(moon_pos, MM2E, 2);
        const Eigen::Vector3d d3m = tideBody(moon_pos, MM2E, 3);

        Eigen::Vector3d disp = d2s + d2m + d3m;

        // =====================================================================
        // IERS 2010 Step 2: Frequency-dependent corrections
        // =====================================================================
        // Compute proper Delaunay/Doodson arguments from the epoch and evaluate
        // per-constituent corrections from Tables 7.3a (diurnal) and 7.3b
        // (semi-diurnal). Each constituent has its own tidal argument and
        // tabulated Love number correction (delta_h, delta_l), avoiding the
        // incorrect approach of lumping all corrections into a single sinusoid.
        {
            const double t_centuries = TimeUtils::gpsTimeToTtCenturies(time);

            auto fund_args = computeFundamentalArguments(t_centuries);
            Eigen::Vector3d step2 = computeStep2Corrections(fund_args, lat, lambda, u);

            if (step2.allFinite()) {
                disp += step2;
            }
        }

        const double stide = disp.dot(los);
        if (!std::isfinite(stide)) return 0.0;

        log_manager_.getLogger("ppp_model")->trace("Solid Earth tide: {:.4f} m (lat_geod={:.6f} deg)", stide, lat * 180.0 / Constants::PI);

        return stide;
    }

    // =========================================================================
    // IERS 2010 Step 2 helpers: Delaunay arguments and constituent tables
    // =========================================================================

    std::array<double, 6> GnssCorrections::computeFundamentalArguments(double t_centuries) {
        constexpr double AS2R = Constants::PI / 180.0 / 3600.0; // arcsecond to radian

        const double T = t_centuries;
        const double T2 = T * T;
        const double T3 = T2 * T;
        const double T4 = T3 * T;

        // l: Mean anomaly of the Moon (IERS 2010, Eq. 5.43)
        double l = std::fmod(
            485868.249036 + 1717915923.2178 * T + 31.8792 * T2
            + 0.051635 * T3 - 0.00024470 * T4, 1296000.0) * AS2R;

        // l': Mean anomaly of the Sun (IERS 2010, Eq. 5.44)
        double lp = std::fmod(
            1287104.79305 + 129596581.0481 * T - 0.5532 * T2
            + 0.000136 * T3 - 0.00001149 * T4, 1296000.0) * AS2R;

        // F: Mean argument of latitude of the Moon (IERS 2010, Eq. 5.45)
        double F = std::fmod(
            335779.526232 + 1739527262.8478 * T - 12.7512 * T2
            - 0.001037 * T3 + 0.00000417 * T4, 1296000.0) * AS2R;

        // D: Mean elongation of Moon from Sun (IERS 2010, Eq. 5.46)
        double D = std::fmod(
            1072260.70369 + 1602961601.2090 * T - 6.3706 * T2
            + 0.006593 * T3 - 0.00003169 * T4, 1296000.0) * AS2R;

        // Omega: Longitude of ascending node of Moon (IERS 2010, Eq. 5.47)
        double Omega = std::fmod(
            450160.398036 - 6962890.5431 * T + 7.4722 * T2
            + 0.007702 * T3 - 0.00005939 * T4, 1296000.0) * AS2R;

        // GMST (IERS 2010, Eq. 5.32 / Capitaine et al. 2003)
        // GMST = ERA + polynomial equation of the equinoxes approximation
        // The standard GMST polynomial requires UT1 centuries, not TT centuries.
        // We approximate JD_UT1 from the TT centuries by subtracting the TT-GPS
        // offset (51.184 s), which is accurate to ~1 s (the GPS-UT1 difference).
        // Using TT directly would introduce ~69 s of error (~17" in GMST).
        double JD_UT1_approx = TimeUtils::J2000_JD + (T * TimeUtils::DAYS_PER_JULIAN_CENTURY)
                             - TimeUtils::TT_GPS_OFFSET / static_cast<double>(TimeUtils::SECONDS_IN_DAY);
        double T_UT1 = (JD_UT1_approx - TimeUtils::J2000_JD) / TimeUtils::DAYS_PER_JULIAN_CENTURY;
        double T_UT1_2 = T_UT1 * T_UT1;
        double T_UT1_3 = T_UT1_2 * T_UT1;
        double gmst_sec = std::fmod(
            67310.54841
            + (3155760000.0 + 8640184.812866) * T_UT1  // 876600*3600 + 8640184.812866
            + 0.093104 * T_UT1_2
            - 6.2e-6 * T_UT1_3, 86400.0);
        // Convert seconds to radians: 2*pi radians = 86400 seconds of sidereal time
        double gmst = gmst_sec * (2.0 * Constants::PI / 86400.0);

        return { l, lp, F, D, Omega, gmst };
    }

    Eigen::Vector3d GnssCorrections::computeStep2Corrections(
        const std::array<double, 6>& fund_args,
        double lat, double lon, const Eigen::Vector3d& u)
    {
        // IERS 2010 Technical Note 36, Section 7.1.1, Tables 7.3a and 7.3b
        //
        // Each constituent has Doodson multipliers [tau, s, h, p, N', ps] that form
        // the tidal argument theta_f from the fundamental arguments, plus
        // in-phase and out-of-phase Love number corrections (delta_h, delta_l), avoiding the
        // incorrect approach of lumping all corrections into a single sinusoid.
        //
        // The displacement corrections are (IERS 2010, Eq. 7.12):
        //
        // Diurnal (m=1):
        //   dr   = sqrt(3)*sin(theta)*[ dR_f_ip*cos(theta_f) + dR_f_op*sin(theta_f) ]
        //   dn   = sqrt(3)*cos(theta)*[ dT_f_ip*cos(theta_f) + dT_f_op*sin(theta_f) ] * 3*sin(lat)*cos(lat)/(cos(2*lat)... )
        //   de   = -sqrt(3)*[dT_f_ip*sin(theta_f) - dT_f_op*cos(theta_f)]
        //
        // Actually, the IERS convention uses geocentric co-latitude theta = pi/2 - phi_c.
        // For simplicity and consistency with the rest of this codebase (which uses
        // geodetic latitude), we use geodetic latitude and the standard displacement
        // formulas from IERS 2010 Section 7.1.1s.
        //
        // The IERS 2010 displacement formulas for degree 2 Step 2 corrections are:
        //
        //   For m=0 (long-period, zonal):
        //     dr = (dRf_ip*cos(theta_f) + dRf_op*sin(theta_f)) * (3*sin^2(phi) - 1)/2
        //     dn = (dTf_ip*cos(theta_f) + dTf_op*sin(theta_f)) * sin(2*phi)/2
        //     de = 0
        //
        //   For m=1 (diurnal, tesseral):
        //     dr = (dRf_ip*cos(theta_f) + dRf_op*sin(theta_f)) * sin(2*phi)
        //     dn = (dTf_ip*cos(theta_f) + dTf_op*sin(theta_f)) * cos(2*phi)
        //     de = -(dTf_ip*sin(theta_f) - dTf_op*cos(theta_f)) * sin(phi)
        //
        //   For m=2 (semi-diurnal, sectorial):
        //     dr = (dRf_ip*cos(theta_f) + dRf_op*sin(theta_f)) * cos^2(phi)
        //     dn = -(dTf_ip*cos(theta_f) + dTf_op*sin(theta_f)) * sin(2*phi)/2
        //     de = -(dTf_ip*sin(theta_f) - dTf_op*cos(theta_f)) * cos(phi)
        //
        // where theta_f is the tidal argument from Doodson multipliers and
        // phi is the station latitude. The corrections dRf_ip, dRf_op, dTf_ip,
        // dTf_op are tabulated in IERS 2010 Tables 7.3a (diurnal) and 7.3b (semi-diurnal).

        const double l     = fund_args[0];
        const double lp    = fund_args[1];
        const double F     = fund_args[2];
        const double D     = fund_args[3];
        const double Omega = fund_args[4];
        const double gmst  = fund_args[5];

        const double sin_lat = std::sin(lat);
        const double cos_lat = std::cos(lat);
        const double sin_2lat = 2.0 * sin_lat * cos_lat;
        const double cos_2lat = cos_lat * cos_lat - sin_lat * sin_lat;

        // Doodson variables from Delaunay arguments:
        // tau = GMST + pi - s (lunar hour angle at Greenwich)
        // s   = F + Omega     (mean longitude of Moon)
        // h   = s - D         (mean longitude of Sun)
        // p   = s - l         (longitude of lunar perigee)
        // N'  = -Omega        (negative of lunar node longitude)
        // ps  = h + l'        (longitude of solar perigee, rarely needed)
        const double s_dood  = F + Omega;
        const double h_dood  = s_dood - D;
        const double p_dood  = s_dood - l;
        const double Np_dood = -Omega;
        const double ps_dood = h_dood + lp;
        const double tau     = gmst + Constants::PI - s_dood;

        // Helper lambda to compute tidal argument from Doodson multipliers
        // Argument = n1*tau + n2*s + n3*h + n4*p + n5*N' + n6*ps
        auto tidalArg = [&](int n1, int n2, int n3, int n4, int n5, int n6) -> double {
            return n1 * tau + n2 * s_dood + n3 * h_dood + n4 * p_dood + n5 * Np_dood + n6 * ps_dood;
        };

        // =====================================================================
        // IERS 2010 Table 7.3a: Long-period band (m=0, zonal) corrections
        // =====================================================================
        // These are the degree-2 zonal (long-period) frequency-dependent
        // corrections from IERS 2010 Table 7.3a. The m=0 band includes the
        // permanent tide constituent (055.555) which makes the total output
        // consistent with the conventional tide-free system used by IGS/ITRF.
        //
        // Columns: Doodson [tau, s, h, p, N', ps], dR_ip, dR_op, dT_ip, dT_op
        // Units: millimeters (converted to meters below)
        //
        // IERS 2010 Eq. 7.12 for m=0 (long-period/zonal):
        //   dr = (dRf_ip*cos(theta_f) + dRf_op*sin(theta_f)) * (3*sin^2(phi) - 1)/2
        //   dn = (dTf_ip*cos(theta_f) + dTf_op*sin(theta_f)) * sin(2*phi)/2
        //   de = 0
        struct LongPeriodEntry {
            int n[6];       // Doodson multipliers [tau, s, h, p, N', ps]
            double dR_ip;   // In-phase radial correction [mm]
            double dR_op;   // Out-of-phase radial correction [mm]
            double dT_ip;   // In-phase transverse correction [mm]
            double dT_op;   // Out-of-phase transverse correction [mm]
        };

        // IERS 2010 Table 7.3a long-period constituents (all entries from TN36)
        // Doodson multipliers [tau, s, h, p, N', ps] decoded from standard Doodson numbers
        static constexpr LongPeriodEntry LONGPERIOD_TABLE[] = {
            // Doodson:  tau  s  h  p  N' ps    dR_ip  dR_op  dT_ip  dT_op
            {{  0,  0,  0,  0,  0,  0 },   0.47,  0.23,   0.16,  0.07 },  // (zero-freq / permanent) (055.555)
            {{  0,  0,  0,  1,  0,  0 },  -0.20, -0.12,  -0.07, -0.05 },  // Mm  (055.655)
            {{  0,  0,  0,  2,  0,  0 },  -0.11, -0.08,  -0.04, -0.03 },  //     (055.755)
            {{  0,  0,  2, -2,  0,  0 },  -0.13, -0.11,  -0.05, -0.04 },  //     (057.455)
            {{  0,  0,  2,  0,  0,  0 },  -4.89,  0.00,  -1.68,  0.00 },  // Ssa (057.555) — DOMINANT
            {{  0,  0,  2,  0,  0, -1 },   0.14,  0.00,   0.05,  0.00 },  //     (057.554)
            {{  0,  1, -2,  1,  0,  0 },   0.09,  0.06,   0.03,  0.02 },  //     (063.655)
            {{  0,  1,  0, -1, -1,  0 },   0.10,  0.02,   0.03,  0.01 },  //     (065.445)
            {{  0,  1,  0, -1,  0,  0 },   0.66,  0.12,   0.23,  0.04 },  // Mf  (065.455)
            {{  0,  1,  0, -1,  1,  0 },   0.05,  0.01,   0.02,  0.00 },  //     (065.465)
            {{  0,  1,  0,  1,  0,  0 },  -0.14, -0.02,  -0.05, -0.01 },  //     (065.655)
            {{  0,  1,  0,  1,  1,  0 },  -0.05,  0.00,  -0.02,  0.00 },  //     (065.665)
            {{  0,  2, -2,  0,  0,  0 },  -0.06, -0.03,  -0.02, -0.01 },  //     (073.555)
            {{  0,  2,  0,  0,  0,  0 },  -0.06, -0.03,  -0.02, -0.01 },  //     (075.555)
            {{  0,  2,  0,  0,  1,  0 },  -0.04, -0.02,  -0.01, -0.01 },  //     (075.565)
        };
        static constexpr size_t N_LONGPERIOD = sizeof(LONGPERIOD_TABLE) / sizeof(LONGPERIOD_TABLE[0]);

        // =====================================================================
        // IERS 2010 Table 7.3a: Diurnal band (m=1) corrections
        // =====================================================================
        // Columns: Doodson [tau, s, h, p, N', ps], dR_ip, dR_op, dT_ip, dT_op
        // Units: millimeters (converted to meters below)
        struct DiurnalEntry {
            int n[6];       // Doodson multipliers [tau, s, h, p, N', ps]
            double dR_ip;   // In-phase radial correction [mm]
            double dR_op;   // Out-of-phase radial correction [mm]
            double dT_ip;   // In-phase transverse correction [mm]
            double dT_op;   // Out-of-phase transverse correction [mm]
        };

        // IERS 2010 Table 7.3a diurnal constituents (all entries from TN36)
        // Doodson multipliers [tau, s, h, p, N', ps] decoded from standard Doodson numbers
        static constexpr DiurnalEntry DIURNAL_TABLE[] = {
            // Doodson:  tau  s  h  p  N' ps    dR_ip  dR_op  dT_ip  dT_op
            {{  1, -3,  2,  0,  0,  0 },  -0.05,  0.00,  -0.01,  0.00 },  //    (125.755)
            {{  1, -2,  2,  0,  0,  0 },  -0.06,  0.00,  -0.01,  0.00 },  //    (127.555)
            {{  1, -2,  0,  1, -1,  0 },  -0.11,  0.01,  -0.02,  0.01 },  //    (135.645)
            {{  1, -2,  0,  1,  0,  0 },  -0.72,  0.06,  -0.13,  0.04 },  // Q1 (135.655)
            {{  1, -2,  0,  1,  1,  0 },  -0.03,  0.00,  -0.01,  0.00 },  //    (135.665)
            {{  1, -2,  2, -1,  0,  0 },  -0.05,  0.00,  -0.01,  0.00 },  // rho1 (137.455)
            {{  1, -1,  0,  0, -1,  0 },   0.05,  0.00,   0.01,  0.00 },  //    (145.545)
            {{  1, -1,  0,  0,  0,  0 },   0.10, -0.05,   0.02, -0.03 },  // O1 (145.555)
            {{  1, -1,  0,  2,  0,  0 },  -0.05,  0.00,  -0.01,  0.00 },  //    (145.755)
            {{  1, -1,  2,  0,  0,  0 },   0.07,  0.00,   0.01,  0.00 },  //    (147.555)
            {{  1,  0,  0, -1,  0,  0 },  -1.31, -0.07,  -0.24,  0.03 },  // M1 (155.455)
            {{  1,  0,  0,  0,  0,  0 },  -0.05,  0.00,  -0.01,  0.00 },  //    (155.555)
            {{  1,  0,  0,  1,  0,  0 },   0.25,  0.00,   0.05,  0.00 },  //    (155.655)
            {{  1,  0,  0,  1,  1,  0 },   0.04,  0.00,   0.01,  0.00 },  //    (155.665)
            {{  1,  0,  2, -1,  0,  0 },   0.07,  0.00,   0.01,  0.00 },  // chi1 (157.455)
            {{  1,  1, -3,  0,  0,  1 },  -0.72, -0.33,  -0.13, -0.19 },  // pi1 (162.556)
            {{  1,  1, -2,  0,  0,  0 }, -13.20,  0.00,  -2.42,  0.00 },  // P1 (163.555)
            {{  1,  1, -2,  0,  0,  1 },  -0.05,  0.00,  -0.01,  0.00 },  //    (163.556)
            {{  1,  1, -1,  0,  0, -1 },  -0.10,  0.05,  -0.02,  0.03 },  //    (164.554)
            {{  1,  1, -1,  0,  0,  1 },  -0.05,  0.00,  -0.01,  0.00 },  //    (164.556)
            {{  1,  1,  0, -2, -1,  0 },   0.04, -0.02,   0.01, -0.01 },  //    (165.345)
            {{  1,  1,  0,  0,  0,  0 },   0.10, -0.05,   0.02, -0.03 },  // K1 (165.555)
            {{  1,  1,  0,  2,  0,  0 },  -0.07,  0.04,  -0.01,  0.02 },  //    (165.755)
        };
        static constexpr size_t N_DIURNAL = sizeof(DIURNAL_TABLE) / sizeof(DIURNAL_TABLE[0]);

        // =====================================================================
        // IERS 2010 Table 7.3b: Semi-diurnal band (m=2) corrections
        // =====================================================================
        struct SemidiurnalEntry {
            int n[6];       // Doodson multipliers
            double dR_ip;   // In-phase radial [mm]
            double dR_op;   // Out-of-phase radial [mm]
            double dT_ip;   // In-phase transverse [mm]
            double dT_op;   // Out-of-phase transverse [mm]
        };

        // IERS 2010 Table 7.3b semi-diurnal constituents
        // Doodson multipliers [tau, s, h, p, N', ps] decoded from standard Doodson numbers
        static constexpr SemidiurnalEntry SEMIDIURNAL_TABLE[] = {
            // Doodson:  tau  s  h  p  N' ps    dR_ip  dR_op  dT_ip  dT_op
            {{  2, -1,  0,  1,  0,  0 },  -0.02,  0.00,   0.00,  0.00 },  // N2 (245.655)
            {{  2, -1,  2, -1,  0,  0 },   0.00,  0.00,   0.00,  0.00 },  //    (247.455)
            {{  2,  0,  0,  0,  0,  0 },  -0.31, -0.07,  -0.04, -0.01 },  // M2 (255.555)
            {{  2,  2, -2,  0,  0,  0 },  -0.08,  0.00,  -0.01,  0.01 },  // S2 (273.555)
            {{  2,  2,  0,  0,  0,  0 },  -0.17,  0.00,  -0.02,  0.01 },  // K2 (275.555)
        };
        static constexpr size_t N_SEMIDIURNAL = sizeof(SEMIDIURNAL_TABLE) / sizeof(SEMIDIURNAL_TABLE[0]);

        // Accumulate displacement in local (radial, north, east) frame
        double dr = 0.0;
        double dn = 0.0;
        double de = 0.0;

        // Precompute latitude factors for m=0 (long-period)
        const double sin2_lat = sin_lat * sin_lat;
        const double lp_radial_factor = (3.0 * sin2_lat - 1.0) / 2.0;  // (3sin²φ - 1)/2
        const double lp_north_factor  = sin_2lat / 2.0;                 // sin(2φ)/2

        // --- Long-period (m=0, zonal) contributions ---
        // IERS 2010 Eq. 7.12 for m=0: no longitude dependence (no m*lambda added),
        // no east component. The 055.555 (zero-frequency) constituent provides the
        // permanent tide correction that defines the conventional tide-free system.
        for (size_t i = 0; i < N_LONGPERIOD; ++i) {
            const auto& e = LONGPERIOD_TABLE[i];
            double theta_f = tidalArg(e.n[0], e.n[1], e.n[2], e.n[3], e.n[4], e.n[5]);
            // m=0: no longitude term added

            double cos_tf = std::cos(theta_f);
            double sin_tf = std::sin(theta_f);

            // Convert mm -> m
            double dRf_ip = e.dR_ip * 1e-3;
            double dRf_op = e.dR_op * 1e-3;
            double dTf_ip = e.dT_ip * 1e-3;
            double dTf_op = e.dT_op * 1e-3;

            double R_term = dRf_ip * cos_tf + dRf_op * sin_tf;
            double T_term_cos = dTf_ip * cos_tf + dTf_op * sin_tf;

            dr += R_term * lp_radial_factor;
            dn += T_term_cos * lp_north_factor;
            // de += 0 for m=0 (no east component)
        }

        // --- Diurnal (m=1) contributions ---
        for (size_t i = 0; i < N_DIURNAL; ++i) {
            const auto& e = DIURNAL_TABLE[i];
            double theta_f = tidalArg(e.n[0], e.n[1], e.n[2], e.n[3], e.n[4], e.n[5]);
            theta_f += lon;  // Add m*lambda for diurnal (m=1)

            double cos_tf = std::cos(theta_f);
            double sin_tf = std::sin(theta_f);

            // Convert mm -> m
            double dRf_ip = e.dR_ip * 1e-3;
            double dRf_op = e.dR_op * 1e-3;
            double dTf_ip = e.dT_ip * 1e-3;
            double dTf_op = e.dT_op * 1e-3;

            // IERS 2010 Eq. 7.12 for m=1 (diurnal):
            //   dr = (dRf_ip*cos + dRf_op*sin) * sin(2*phi)
            //   dn = (dTf_ip*cos + dTf_op*sin) * cos(2*phi)
            //   de = -(dTf_ip*sin - dTf_op*cos) * sin(phi)
            double R_term = dRf_ip * cos_tf + dRf_op * sin_tf;
            double T_term_cos = dTf_ip * cos_tf + dTf_op * sin_tf;
            double T_term_sin = dTf_ip * sin_tf - dTf_op * cos_tf;

            dr += R_term * sin_2lat;
            dn += T_term_cos * cos_2lat;
            de -= T_term_sin * sin_lat;
        }

        // --- Semi-diurnal (m=2) contributions ---
        for (size_t i = 0; i < N_SEMIDIURNAL; ++i) {
            const auto& e = SEMIDIURNAL_TABLE[i];
            double theta_f = tidalArg(e.n[0], e.n[1], e.n[2], e.n[3], e.n[4], e.n[5]);
            theta_f += 2.0 * lon;  // Add m*lambda for semi-diurnal (m=2)

            double cos_tf = std::cos(theta_f);
            double sin_tf = std::sin(theta_f);

            double dRf_ip = e.dR_ip * 1e-3;
            double dRf_op = e.dR_op * 1e-3;
            double dTf_ip = e.dT_ip * 1e-3;
            double dTf_op = e.dT_op * 1e-3;

            // IERS 2010 Eq. 7.12 for m=2 (semi-diurnal):
            //   dr = (dRf_ip*cos + dRf_op*sin) * cos^2(phi)
            //   dn = (dTf_ip*cos + dTf_op*sin) * sin(2*phi)/2
            //   de = -(dTf_ip*sin - dTf_op*cos) * cos(phi)
            double R_term = dRf_ip * cos_tf + dRf_op * sin_tf;
            double T_term_cos = dTf_ip * cos_tf + dTf_op * sin_tf;
            double T_term_sin = dTf_ip * sin_tf - dTf_op * cos_tf;

            dr += R_term * cos_lat * cos_lat;
            dn -= T_term_cos * sin_2lat / 2.0;
            de -= T_term_sin * cos_lat;
        }

        // Convert from local (radial, north, east) to ECEF
        // Radial: u (unit geocentric vector)
        // North: (-sin(lat)*cos(lon), -sin(lat)*sin(lon), cos(lat))
        // East:  (-sin(lon), cos(lon), 0)
        double cos_lon = std::cos(lon);
        double sin_lon = std::sin(lon);

        Eigen::Vector3d e_north(-sin_lat * cos_lon, -sin_lat * sin_lon, cos_lat);
        Eigen::Vector3d e_east(-sin_lon, cos_lon, 0.0);

        return dr * u + dn * e_north + de * e_east;
    }

    /**
     * @brief Compute the phase wind-up correction for carrier-phase measurements.
     *
     * Computes the phase wind-up induced path-length offset (meters) due to the
     * relative rotation/orientation of the satellite and receiver antennas.
     * The algorithm:
     *  - forms the line-of-sight unit vector from receiver to satellite,
     *  - determines satellite attitude (using cached orbit velocity if available,
     *    otherwise a simple yaw-steering fallback),
     *  - determines receiver antenna dipole from configuration or RINEX header,
     *  - builds effective dipole vectors for satellite and receiver and computes
     *    the relative rotation angle between them,
     *  - converts the rotation (radians) to a distance using the carrier wavelength:
     *      phi = sign * acos(dot(d_s_eff, d_r_eff)) * wavelength / (2*pi)
     *
     * @param satellite Satellite identifier string (e.g., "G01") used for logging/diagnostics.
     * @param system    GNSS system enum corresponding to the satellite (may affect lookups).
     * @param rx_pos    Receiver ECEF position vector (meters).
     * @param sat_pos   Satellite ECEF position vector (meters).
     * @param wavelength Carrier wavelength in meters (used to convert phase angle to meters).
     * @param time      Epoch for any time-dependent inputs (GnssTime: week + tow).
     * @param sun_pos   Sun ECEF position vector (meters). Used for satellite attitude estimation.
     * @param sat_vel   Satellite ECEF velocity vector (meters/second). Used for attitude if available.
     * @return Phase wind-up correction in meters. Returns 0.0 for degenerate geometry,
     *         invalid inputs, or when computed vectors are nearly singular.
     *
     * @note The routine may use optional data provided at construction:
     *       - `orbits_` (to obtain satellite velocity for attitude estimation),
     *       - `config_` (to obtain receiver antenna orientation),
     *       - `rinex_parser_` (to obtain antenna H/E/N offsets).
     * @note The returned value is intended to be added to the carrier-range (meters).
     * @thread_safety Safe to call concurrently with independent inputs (no static state).
     * @see GnssConfig::getReceiverAntennaOrientation(), RinexParser::getAntennaDeltaHen()
     */
    double GnssCorrections::computePhaseWindUp(const std::string& satellite,
        System system,
        const Eigen::Vector3d& rx_pos,
        const Eigen::Vector3d& sat_pos,
        double wavelength,
        const GnssTime& time,
        const Eigen::Vector3d& sun_pos,
        const Eigen::Vector3d& sat_vel) const {

        // Phase wind-up correction for carrier phase
        // Wu et al. (1993) convention:
        //   k  = unit vector from satellite to receiver (sat->rx)
        //   Ds = x_s - k*(k·x_s) + cross(k, y_s)   (satellite effective dipole)
        //   Dr = x_r - k*(k·x_r) + cross(k, y_r)   (receiver effective dipole)
        //   x_r = North, y_r = East
        //   sign: if dot(k, cross(Ds, Dr)) < 0 → windUp = -windUp

        Eigen::Vector3d k_raw = rx_pos - sat_pos;
        double k_raw_norm = k_raw.norm();
        if (k_raw_norm < 1e-3) {
            log_manager_.getLogger("ppp_model")->trace("Phase wind-up for {}: degenerate LOS", satellite);
            return 0.0;
        }
        Eigen::Vector3d k = k_raw / k_raw_norm;  // unit vector sat->rx

        // Satellite body frame using yaw-steering model
        //   ez = -sat_pos/|sat_pos|  (toward Earth / nadir / boresight)
        //   e  = (sun - sat)/|sun - sat|  (Sun direction from satellite)
        //   ey = cross(ez, e)  (cross-track / solar panel axis)
        //   ex = cross(ey, ez) (completes right-handed frame)
        Eigen::Vector3d ez = -sat_pos.normalized();

        Eigen::Vector3d x_s, y_s;

        if (sun_pos.norm() > 1e6) {
            Eigen::Vector3d e_sun = (sun_pos - sat_pos).normalized();

            Eigen::Vector3d ey = ez.cross(e_sun);
            if (ey.norm() < 1e-8) {
                // Eclipse/noon turn fallback: use velocity if available
                Eigen::Vector3d vhat = sat_vel.norm() > 1e-8 ? sat_vel.normalized() : Eigen::Vector3d::UnitY();
                ey = ez.cross(vhat);
            }
            ey.normalize();

            Eigen::Vector3d ex = ey.cross(ez);
            if (ex.norm() < 1e-8) {
                ex = Eigen::Vector3d::UnitX();
            }
            ex.normalize();

            x_s = ex;
            y_s = ey;
        } else {
            // Fallback: velocity-based attitude
            Eigen::Vector3d z_s = -ez; // nadir
            Eigen::Vector3d vel = sat_vel;
            if (vel.norm() < 1e-8 && !orbits_.empty()) {
                auto orbit_it = std::find_if(orbits_.begin(), orbits_.end(),
                    [&satellite](const OrbitData& o) { return o.satellite == satellite; });
                if (orbit_it != orbits_.end() && orbit_it->velocity_valid) {
                    vel = orbit_it->velocity;
                }
            }

            y_s = vel.norm() > 1e-8 ? vel.normalized().cross(z_s) : Eigen::Vector3d::Zero();
            if (y_s.norm() < 1e-8) {
                y_s = Eigen::Vector3d(0, 1, 0);
            }
            y_s.normalize();

            x_s = y_s.cross(z_s);
            if (x_s.norm() < 1e-8) {
                x_s = Eigen::Vector3d::UnitX();
            }
            x_s.normalize();
        }

        // Receiver local frame (North and East)
        double rx_norm = rx_pos.norm();
        if (rx_norm < 1e3) {
            return 0.0;
        }
        Coordinates::Llh rx_llh = Coordinates::ecefToLlh(rx_pos);
        double phi = rx_llh.lat_rad;
        double lam = rx_llh.lon_rad;

        Eigen::Vector3d x_r(-std::sin(phi) * std::cos(lam), -std::sin(phi) * std::sin(lam), std::cos(phi)); // North
        Eigen::Vector3d y_r(-std::sin(lam), std::cos(lam), 0.0); // East

        // Effective dipoles (Wu et al. 1993, Eq. 6-7)
        //   D = x - k*(k·x) + cross(k, y)
        Eigen::Vector3d Ds = x_s - k * (k.dot(x_s)) + k.cross(y_s);
        Eigen::Vector3d Dr = x_r - k * (k.dot(x_r)) + k.cross(y_r);

        double Ds_norm = Ds.norm();
        double Dr_norm = Dr.norm();
        if (Ds_norm < 1e-10 || Dr_norm < 1e-10) {
            log_manager_.getLogger("ppp_model")->trace("Phase wind-up for {}: degenerate dipole", satellite);
            return 0.0;
        }

        // Compute angle between effective dipoles
        double cos_alpha = Ds.dot(Dr) / (Ds_norm * Dr_norm);
        cos_alpha = std::clamp(cos_alpha, -1.0, 1.0);

        double windUp_c = std::acos(cos_alpha);  // radians [0, pi]

        // Determine sign (Wu et al. 1993, Eq. 8)
        if (k.dot(Ds.cross(Dr)) < 0.0) {
            windUp_c = -windUp_c;
        }

        // Maintain continuity with previous epoch to avoid 2π jumps
        auto it = prev_windup_.find(satellite);
        if (it != prev_windup_.end()) {
            double prev = it->second;
            double diff = windUp_c - prev;
            double cycles = std::round(diff / (2.0 * Constants::PI));
            windUp_c = prev + (diff - cycles * 2.0 * Constants::PI);
        }
        prev_windup_[satellite] = windUp_c;

        // Convert radians to meters
        double phi_m = windUp_c * wavelength / (2.0 * Constants::PI);

        log_manager_.getLogger("ppp_model")->trace("Phase wind-up for {}: {:.6f} m ({:.3f} rad)",
            satellite, phi_m, windUp_c);
        return phi_m;
    }

    /**
     * @brief Replace internal satellite clock and ERP product containers.
     *
     * Copies the provided satellite clock samples and Earth rotation parameter (ERP)
     * samples into the object's internal storage, replacing any previously stored
     * product data. This is intended to refresh or update time-dependent products
     * at runtime without reconstructing the `GnssCorrections` object.
     *
     * The method performs a deep copy of the provided vectors (caller retains
     * ownership). After updating the internal containers a diagnostic trace log
     * entry is emitted describing the new container sizes.
     *
     * @param clocks Vector of parsed satellite clock samples (@c ClockData). May be empty.
     * @param erps   Vector of parsed Earth rotation parameter samples (@c ErpData). May be empty.
     *
     * @note Thread-safety: This setter is not synchronized internally. If other
     *       threads may concurrently read the product containers (e.g. calling
     *       interpolation/correction routines), callers must ensure appropriate
     *       external synchronization (mutex) to avoid data races.
     * @note Typical use: called after parsing product files (CLK/ERP) to make the
     *       data available to interpolation and correction routines in this class.
     */
    void GnssCorrections::setClocksAndErps(const std::vector<ClockData>& clocks, const std::vector<ErpData>& erps) {
        clocks_ = clocks;
        erps_ = erps;
        buildClockIndex();
        log_manager_.getLogger("corrections")->trace("GnssCorrections updated: {} clocks, {} erps", clocks_.size(), erps_.size());
    }

    /**
     * @brief Reset phase wind-up state for a specific satellite.
     *
     * Clears the stored previous wind-up angle for the specified satellite,
     * forcing the next wind-up computation to start fresh without attempting
     * phase unwrapping relative to a previous value.
     *
     * Call this when a cycle slip or ambiguity reset is detected for the
     * satellite to ensure phase wind-up continuity is not falsely maintained
     * across the discontinuity.
     *
     * @param satellite Satellite identifier string (e.g. "G01").
     */
    void GnssCorrections::resetPhaseWindUp(const std::string& satellite) {
        auto it = prev_windup_.find(satellite);
        if (it != prev_windup_.end()) {
            log_manager_.getLogger("corrections")->debug(
                "Reset phase wind-up for {}: previous value was {:.3f} rad",
                satellite, it->second);
            prev_windup_.erase(it);
        }
    }

    /**
     * @brief Reset phase wind-up state for all satellites.
     *
     * Clears the stored previous wind-up angles for all satellites,
     * forcing all subsequent wind-up computations to start fresh.
     *
     * Call this on a complete filter reset or at the start of a new session
     * to clear all accumulated phase wind-up history.
     */
    void GnssCorrections::resetAllPhaseWindUp() {
        if (!prev_windup_.empty()) {
            log_manager_.getLogger("corrections")->debug(
                "Reset phase wind-up for all {} satellites", prev_windup_.size());
            prev_windup_.clear();
        }
    }

} // namespace GnssProcessor