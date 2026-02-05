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

    // Anonymous namespace for file-local utilities
    namespace {

        /**
         * @brief Compute barycentric Lagrange interpolation weights for a set of nodes.
         *
         * Produces barycentric weights w_j proportional to
         *   1 / prod_{k != j} (x_j - x_k)
         * which are used by the barycentric Lagrange formula to evaluate the
         * interpolation polynomial stably:
         *   p(x_q) = (sum_j (w_j/(x_q - x_j))*y_j) / (sum_j (w_j/(x_q - x_j)))
         *
         * This implementation computes the product in log-space to reduce overflow
         * / underflow when the product of differences is very large or very small.
         * If two abscissae are numerically identical (|x_j - x_k| < 1e-9), the
         * corresponding weight is left as zero and the caller should avoid duplicate
         * nodes. For a single node the routine returns {1.0}.
         *
         * @param x Vector of node abscissae (x_0 .. x_{n-1}). Values should be distinct.
         * @return Vector of barycentric weights of same length as `x`. Returns empty
         *         vector if `x` is empty.
         *
         * @note Complexity: O(n^2). The function attempts to preserve sign information
         *       while computing magnitude in log-space and clamps extremely large/small
         *       results to avoid floating-point overflow/NaN (implementation thresholds).
         * @thread_safety Safe to call concurrently with independent inputs (no static data).
         */
        std::vector<double> barycentricWeights(const std::vector<double>& x) {
            size_t n = x.size();
            if (n == 0) return {};
            if (n == 1) return { 1.0 };

            std::vector<double> w(n, 0.0);

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
                    if (log_abs_prod > 700.0) {
                        w[j] = 0.0;
                    } else if (log_abs_prod < -700.0) {
                        w[j] = sign * 1e300;
                    } else {
                        w[j] = sign * std::exp(-log_abs_prod);
                    }
                }
            }
            return w;
        }

        /**
         * @brief Evaluate the barycentric Lagrange interpolant at a query point.
         *
         * Uses precomputed barycentric weights to evaluate the interpolation polynomial
         * in a numerically stable O(n) fashion:
         *   p(x_q) = (sum_j (w_j/(x_q - x_j)) * y_j) / (sum_j (w_j/(x_q - x_j)))
         *
         * If the query abscissa `xq` coincides (within tolerance) with a node `x[j]`,
         * the corresponding `y[j]` is returned directly to avoid division by zero.
         *
         * @param x  Vector of node abscissae (x_0 .. x_{n-1}). Must have same length as `w` and `y`.
         * @param w  Vector of barycentric weights computed for `x` (see barycentricWeights()).
         * @param y  Vector of node ordinates (y_0 .. y_{n-1}).
         * @param xq Query abscissa where the polynomial is evaluated.
         * @return   Interpolated value p(xq). If the denominator is (nearly) zero the
         *           function returns 0.0 as a safe fallback.
         *
         * @note The function treats an exact node match when |xq - x[j]| < 1e-12 and
         *       returns y[j] to preserve node values exactly.
         * @note Complexity: O(n). The caller is responsible for ensuring `x`, `w`, and
         *       `y` are consistent (same length and corresponding entries).
         * @thread_safety Safe to call concurrently with independent inputs (no static data).
         */
        double barycentricEval(const std::vector<double>& x,
            const std::vector<double>& w,
            const std::vector<double>& y,
            double xq) {
            double num = 0.0;
            double den = 0.0;
            for (size_t j = 0; j < x.size(); ++j) {
                double diff = xq - x[j];
                if (std::abs(diff) < 1e-12) {
                    return y[j]; // Exact node
                }
                double term = w[j] / diff;
                num += term * y[j];
                den += term;
            }
            return (den == 0.0) ? 0.0 : (num / den);
        }
    }

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
        config_(nullptr), rinex_parser_(nullptr) {
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
        config_(nullptr), rinex_parser_(nullptr) {
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
        config_(&config), rinex_parser_(&rinex) {
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
        // Validate inputs
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
        // Interpolation parameters
        //const SatelliteClockInterpolationMode mode = config_.getSatelliteClockInterpolationMode();
        const SatelliteClockInterpolationMode mode = config_ ? config_->getSatelliteClockInterpolationMode() : SatelliteClockInterpolationMode::Linear;
        const size_t INTERP_POINTS = (mode == SatelliteClockInterpolationMode::Linear) ? 2 : 4;

        constexpr double MAX_WINDOW = 7200.0;     // Maximum window to search for nodes (2 hours)
        constexpr double MAX_CLOCK_BIAS = 1e-3;   // 1 ms sanity limit

        if (satellite.empty()) {
            log_manager_.getLogger("corrections")->debug("Clock interp: empty satellite ID");
            return 0.0;
        }

        // O(1) satellite lookup
        auto it = clock_index_.find(satellite);
        if (it == clock_index_.end() || it->second.empty()) {
            log_manager_.getLogger("corrections")->debug(
                "Clock interp for {}: no clock data in index", satellite);
            return 0.0;
        }

        const auto& sat_clocks = it->second;  // Already sorted chronologically
        const double t_query = TimeUtils::continuousGpsSeconds(time);

        // Binary search for first clock >= t_query  
        auto upper_it = std::lower_bound(sat_clocks.begin(), sat_clocks.end(), t_query,
            [](const ClockData* c, double t) {
                return TimeUtils::continuousGpsSeconds(c->time) < t;
            });

        size_t upper_idx = std::distance(sat_clocks.begin(), upper_it);

        // Use a stable centered window
        size_t half_before = INTERP_POINTS / 2;
        size_t start_idx = (upper_idx > half_before) ? (upper_idx - half_before) : 0;

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
                // For extrapolation, always use 2 closest points (linear)
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
            // Linear interpolation
            result = Interpolate::linearInterp(x, y, t_query);

            log_manager_.getLogger("corrections")->debug(
                "Clock interp (linear) for {} at tow={:.3f}: {} pts, span={:.1f}s, "
                "result={:.3e}s ({:.3f}m), extrap={}",
                satellite, time.tow, n, x.back() - x.front(),
                result, result * Constants::SPEED_OF_LIGHT,
                extrapolating ? "YES" : "no");
        }
        else {
            // Lagrange interpolation for 3+ points
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
     *  - converts polar motion parameters xp/yp (assumed in arcseconds) to radians,
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
     * @note The routine expects ERP xp/yp values in arcseconds and converts them to
     *       radians using the factor 4.848136811e-6. The simplified coefficients
     *       (radial: -0.033, north: -0.009, east: -0.009) follow common lightweight
     *       pole-tide approximations (IERS-like conventions).
     * @note The function performs input validation and clamps results (caller may
     *       inspect logs for diagnostic messages). Output norm is checked (max 0.1 m).
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

        // Find closest ERP data
        auto it = std::min_element(erps_.begin(), erps_.end(),
            [&time](const ErpData& a, const ErpData& b) {
                return std::abs(a.time.tow - time.tow) < std::abs(b.time.tow - time.tow);
            });
        if (it == erps_.end() || !std::isfinite(it->xp) || !std::isfinite(it->yp)) {
            log_manager_.getLogger("ppp_model")->trace("No valid ERP data for pole tide correction at TOW {:.3f}", time.tow);
            return Eigen::Vector3d::Zero();
        }

        // Compute pole tide correction (simplified model, IERS 2010 conventions)
        double phi = std::asin(rx_pos.z() / rx_pos.norm()); // Geodetic latitude (radians)
        double lambda = std::atan2(rx_pos.y(), rx_pos.x()); // Geodetic longitude (radians)
        double xp = it->xp * 4.848136811e-6; // Convert arcsec to radians
        double yp = it->yp * 4.848136811e-6; // Convert arcsec to radians

        // Pole tide displacement in radial, north, east directions (meters)
        double R = rx_pos.norm();
        double radial = -0.033 * std::sin(2.0 * phi) * (xp * std::cos(lambda) + yp * std::sin(lambda));
        double north = -0.009 * std::cos(2.0 * phi) * (xp * std::sin(lambda) - yp * std::cos(lambda));
        double east = -0.009 * std::sin(phi) * (xp * std::cos(lambda) + yp * std::sin(lambda));

        // Convert to ECEF coordinates
        Eigen::Vector3d correction(
            (radial * std::cos(phi) * std::cos(lambda) - north * std::sin(phi) * std::cos(lambda) - east * std::sin(lambda)),
            (radial * std::cos(phi) * std::sin(lambda) - north * std::sin(phi) * std::sin(lambda) + east * std::cos(lambda)),
            (radial * std::sin(phi) + north * std::cos(phi))
        );

        if (!correction.allFinite() || correction.norm() > 0.1) { // Max 10 cm
            log_manager_.getLogger("ppp_model")->warn("Invalid pole tide correction at TOW {:.3f}: ({:.3e}, {:.3e}, {:.3e}) m, norm={:.3e}",
                time.tow, correction.x(), correction.y(), correction.z(), correction.norm());
            return Eigen::Vector3d::Zero();
        }

        log_manager_.getLogger("ppp_model")->trace("Pole tide correction at TOW {:.3f}: ({:.3e}, {:.3e}, {:.3e}) m, xp={:.3e} rad, yp={:.3e} rad",
            time.tow, correction.x(), correction.y(), correction.z(), xp, yp);
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
        // Validate inputs
        if (!rx_pos.allFinite() || rx_pos.norm() < 1e3) {
            log_manager_.getLogger("ppp_model")->warn("Invalid receiver position for ocean tide correction at TOW {:.3f}: rx_pos=({:.3f}, {:.3f}, {:.3f}), norm={:.3e}",
                time.tow, rx_pos.x(), rx_pos.y(), rx_pos.z(), rx_pos.norm());
            return Eigen::Vector3d::Zero();
        }
        if (time.tow < 0.0 || time.week < 0) {
            log_manager_.getLogger("ppp_model")->warn("Invalid time for ocean tide correction: week {}, TOW {:.3f}",
                time.week, time.tow);
            return Eigen::Vector3d::Zero();
        }

        // Placeholder: Simplified ocean tide correction (requires external tidal model data)
        // Using a basic sinusoidal model based on position and time
        double phi = std::asin(rx_pos.z() / rx_pos.norm()); // Geodetic latitude (radians)
        double t = std::fmod(time.tow, TimeUtils::SECONDS_IN_DAY); // Seconds of day
        double amplitude = 0.01; // Typical ocean tide amplitude: 1 cm
        double period = 12.4206 * 3600.0; // M2 tidal constituent period (s)
        double radial = amplitude * std::cos(2.0 * Constants::PI * t / period) * std::cos(phi);
        double north = amplitude * std::sin(2.0 * Constants::PI * t / period) * std::sin(phi);
        double east = 0.0; // Simplified, no east component

        // Convert to ECEF coordinates
        double lambda = std::atan2(rx_pos.y(), rx_pos.x());
        Eigen::Vector3d correction(
            (radial * std::cos(phi) * std::cos(lambda) - north * std::sin(phi) * std::cos(lambda) - east * std::sin(lambda)),
            (radial * std::cos(phi) * std::sin(lambda) - north * std::sin(phi) * std::sin(lambda) + east * std::cos(lambda)),
            (radial * std::sin(phi) + north * std::cos(phi))
        );

        if (!correction.allFinite() || correction.norm() > 0.1) { // Max 10 cm
            log_manager_.getLogger("ppp_model")->warn("Invalid ocean tide correction at TOW {:.3f}: ({:.3e}, {:.3e}, {:.3e}) m, norm={:.3e}",
                time.tow, correction.x(), correction.y(), correction.z(), correction.norm());
            return Eigen::Vector3d::Zero();
        }

        log_manager_.getLogger("ppp_model")->trace("Ocean tide correction at TOW {:.3f}: ({:.3e}, {:.3e}, {:.3e}) m, amplitude={:.3e} m",
            time.tow, correction.x(), correction.y(), correction.z(), amplitude);
        return correction;
    }

    /**
     * @brief Compute solid Earth tide radial displacement along the receiver->satellite line-of-sight.
     *
     * Implements a simplified IERS-2010 style solid Earth tide model (degree 2 + degree 3
     * terms) and returns the displacement projected onto the receiver->satellite line-of-sight.
     * The implementation:
     *  - converts receiver ECEF to an approximate geodetic latitude (iterative, stable),
     *  - computes degree-2 and degree-3 tidal contributions from the Sun and Moon using
     *    nominal Love numbers and mass ratios,
     *  - projects the total displacement onto the receiver->satellite line-of-sight and
     *    returns that scalar displacement in meters.
     *
     * @param receiver_pos Receiver ECEF position vector (meters).
     * @param sat_pos      Satellite ECEF position vector (meters).
     * @param sun_pos      Sun ECEF position vector (meters).
     * @param moon_pos     Moon ECEF position vector (meters).
     * @return Radial displacement in meters to apply to the geometric range (projection of
     *         tidal displacement onto the line-of-sight). Returns 0.0 for invalid or
     *         non-finite inputs or when intermediate values are outside safe bounds.
     *
     * @details
     * - Uses nominal Love numbers (h2,l2,h3,l3) and simplified latitude-dependent
     *   corrections. Degree-3 contribution is included (moon-dominated in simplified form).
     * - Geodetic latitude is derived from ECEF with a short iterative scheme to ensure
     *   numerical stability near the poles.
     * - The routine performs input validation and logs diagnostics; it is intended for
     *   use in per-satellite, per-epoch correction routines where lightweight tide
     *   corrections are acceptable.
     *
     * @note The returned value is the displacement along the line-of-sight (meters),
     *       not a full ECEF displacement vector.
     * @thread_safety Safe to call concurrently with independent inputs (no static state).
     * @see computePoleTideCorrection(), computeOceanTideCorrection()
     */
    double GnssCorrections::computeSolidEarthTide(const Eigen::Vector3d& receiver_pos, const Eigen::Vector3d& sat_pos,
        const Eigen::Vector3d& sun_pos, const Eigen::Vector3d& moon_pos) const {
        // IERS 2010: Solid Earth tide (degree 2 + degree 3 terms).
        //
        // This implementation uses:
        //  1) WGS-84 geodetic latitude (not geocentric latitude derived from ECEF norm)
        //  2) The standard IERS 2010 approach (zero-frequency free-space Love numbers: h2,l2 and h3,l3)
        //
        // Output is the displacement projection onto the receiver->satellite line-of-sight [m].

        // IERS 2010 nominal Love numbers (latitude-dependent corrections omitted here; still a big improvement)
        constexpr double h2 = 0.6078;
        constexpr double l2 = 0.0847;
        constexpr double h3 = 0.292;
        constexpr double l3 = 0.015;

        // Mass ratios (IERS 2010): (M_sun / M_earth), (M_moon / M_earth)
        constexpr double MS2E = 332946.0;
        constexpr double MM2E = 0.01230002;

        // WGS-84 ellipsoid
        const double a = Constants::Ellipsoid::WGS84::a; // semi-major axis

        if (!receiver_pos.allFinite() || !sat_pos.allFinite() || !sun_pos.allFinite() || !moon_pos.allFinite()) {
            log_manager_.getLogger("ppp_model")->warn("Solid Earth tide: non-finite inputs");
            return 0.0;
        }

        const Eigen::Vector3d rho = sat_pos - receiver_pos;
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
                // Degree-2 terms: P2 = (3 cos^2 - 1)/2
                const double P2 = (3.0 * cos_psi * cos_psi - 1.0) / 2.0;

                // Tangential direction component
                const Eigen::Vector3d t = ub - cos_psi * u;

                // Scale factor: (Re^4 / d^3) * (M_body/M_earth)
                const double scale = mass_ratio * std::pow(a, 4) / std::pow(d, 3);

                // IERS 2010: displacement = h2 * u * P2 + 3*l2*cosψ*t
                return scale * ((h2_eff * u) * P2 + (3.0 * l2_eff * cos_psi) * t);
            }
            else {
                // Degree-3 terms: P3 = (5 cos^3 - 3 cos)/2, dP3/dψ -> (5/2 cos^2 - 3/2)
                const double P3 = (5.0 * cos_psi * cos_psi * cos_psi - 3.0 * cos_psi) / 2.0;
                const double dP3 = (5.0 / 2.0) * cos_psi * cos_psi - (3.0 / 2.0);

                const Eigen::Vector3d t = ub - cos_psi * u;

                // Scale factor: (Re^5 / d^4) * (M_body/M_earth)
                const double scale = mass_ratio * std::pow(a, 5) / std::pow(d, 4);

                // IERS 2010: displacement = h3*u*P3 + l3*dP3*t
                return scale * ((h3 * u) * P3 + (l3 * dP3) * t);
            }
        };

        const Eigen::Vector3d d2s = tideBody(sun_pos, MS2E, 2);
        const Eigen::Vector3d d2m = tideBody(moon_pos, MM2E, 2);
        const Eigen::Vector3d d3m = tideBody(moon_pos, MM2E, 3); // IERS: degree-3 usually moon-only in simplified PPP implementations

        const Eigen::Vector3d disp = d2s + d2m + d3m;

        const double stide = disp.dot(los);
        if (!std::isfinite(stide)) return 0.0;

        // Keep logging lightweight; this is per-sat per-epoch
        log_manager_.getLogger("ppp_model")->trace("Solid Earth tide: {:.4f} m (lat_geod={:.6f} deg)", stide, lat * 180.0 / Constants::PI);

        return stide;
    }

    /**
     * @brief Estimate atmospheric loading radial displacement at a receiver position.
     *
     * Provides a simplified, lightweight estimate of the vertical (radial) station
     * displacement due to atmospheric mass loading. The implementation uses a
     * small sinusoidal model (diurnal) scaled by latitude as a placeholder when
     * full atmospheric-loading grids or models are unavailable.
     *
     * @param rx_pos Receiver ECEF position vector (meters). Must be finite and non-degenerate.
     * @param time   Epoch at which the loading is evaluated (GnssTime: week + tow).
     * @return Estimated radial displacement in meters due to atmospheric loading.
     *         Returns 0.0 for invalid inputs. Typical magnitude returned by this
     *         placeholder is on the order of millimetres (default amplitude = 1 mm).
     *
     * @details
     * - This routine computes an approximate geodetic latitude from the ECEF
     *   position and evaluates a single diurnal sinusoid:
     *     correction = amplitude * cos(2π * t_day / 86400) * cos(latitude)
     *   where `amplitude` defaults to 0.001 m (1 mm).
     * - The result is a scalar radial displacement (meters). The implementation
     *   intentionally does not attempt to model frequency-dependent loading or
     *   use load Love numbers / ocean-tide coupling — use a proper loading model
     *   for production/precise applications.
     *
     * @note This function is a placeholder to allow downstream code to compile/run
     *       in absence of external atmospheric-loading data. Replace with a
     *       gridded or model-based implementation (e.g., IERS-compliant) for
     *       accurate geodetic positioning.
     * @thread_safety Safe to call concurrently with independent inputs (no static state).
     */
    double GnssCorrections::computeAtmosphericLoadingCorrection(const Eigen::Vector3d& rx_pos,
        const GnssTime& time) const {
        // Placeholder: Simplified atmospheric loading correction
        // Model: Small sinusoidal variation based on time and position latitude
        // Note: Full implementation requires external loading models (e.g., IERS grids)
        double latitude = std::asin(rx_pos.z() / rx_pos.norm()) * 180.0 / Constants::PI; // Approx. latitude (degrees)
        double t = fmod(time.tow, TimeUtils::SECONDS_IN_DAY); // Seconds of day
        double amplitude = 0.001; // 1 mm amplitude (typical)
        double correction = amplitude * std::cos(2.0 * Constants::PI * t / TimeUtils::SECONDS_IN_DAY) * std::cos(latitude * Constants::PI / 180.0);
        log_manager_.getLogger("ppp_model")->trace("Atmospheric loading correction: {} m", correction);
        return correction;
    }

    /**
     * @brief Retrieve a bias correction for a satellite/signal at a given epoch.
     *
     * Searches the internal bias table for an entry that applies to the requested
     * `satellite` and `freq_type` and whose validity interval contains `time`.
     * If a matching entry is found the stored `BiasData::bias` value is returned.
     * If inputs are invalid, no matching entry exists, or the found entry contains
     * non-finite values the function returns 0.0.
     *
     * @param satellite Satellite identifier or receiver id (e.g. "G01" or station id).
     * @param freq_type Signal/frequency label (e.g. "L1", "C1") for which the bias applies.
     * @param time      Epoch at which the bias is requested (GnssTime: week + tow).
     * @return Bias value (same units as stored in BiasData::bias, typically meters).
     *         Returns 0.0 when inputs are invalid or no applicable bias is found.
     *
     * @note The search compares the bias validity interval (@c BiasData::start_time / @c BiasData::end_time)
     *       against the supplied `time` (see implementation for exact comparison semantics).
     *       Callers must ensure that the provided `time` and stored bias intervals are
     *       comparable (same time reference/representation).
     * @note Behaviour when multiple bias entries overlap is implementation-defined
     *       (the current implementation returns the first matching entry found).
     *
     * @see BiasData, GnssTime
     */
    double GnssCorrections::getBiasCorrection(const std::string& satellite,
        const std::string& freq_type,
        const GnssTime& time) const {
        // Validate inputs
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

        // Convert query time to continuous GPS seconds for robust interval comparison
        const double t_query = TimeUtils::continuousGpsSeconds(time);

        // Find bias data: use continuous time (week + tow) for interval comparisons
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

        // Helpful diagnostic if biases exist globally but none matched — aids debugging
        if (!biases_.empty()) {
            // log at debug to avoid spam; change level as needed when investigating
            log_manager_.getLogger("ppp_model")->debug("No matching bias entry found for {} {} at continuous_time={:.3f}. Known bias entries={}",
                satellite, freq_type, t_query, biases_.size());
        }

        log_manager_.getLogger("ppp_model")->trace("No bias correction for {} ({}) {} at TOW {:.3f}",
            satellite, PppUtils::getSystemString(satellite),
            freq_type, time.tow);
        return 0.0;
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
        const GnssTime& time) const {

        // Phase wind-up correction for carrier phase
        // LOS unit vector from SATELLITE to RECEIVER (matching MATLAB convention)
        Eigen::Vector3d k = (rx_pos - sat_pos);
        double k_norm = k.norm();
        if (k_norm == 0.0) {
            log_manager_.getLogger("ppp_model")->trace("Phase wind-up for {}: degenerate LOS", satellite);
            return 0.0;
        }
        k /= k_norm;

        // Satellite body frame (yaw-steering model)
        // z_s points from satellite toward Earth center
        Eigen::Vector3d z_s = -sat_pos.normalized();
        
        // Get satellite velocity for attitude
        Eigen::Vector3d sat_vel = Eigen::Vector3d::Zero();
        if (!orbits_.empty()) {
            auto orbit_it = std::find_if(orbits_.begin(), orbits_.end(),
                [&satellite](const OrbitData& o) { return o.satellite == satellite; });
            if (orbit_it != orbits_.end() && orbit_it->velocity_valid) {
                sat_vel = orbit_it->velocity;
            }
        }

        // Sun direction (fallback to +X if not available)
        Eigen::Vector3d e_sun = Eigen::Vector3d(1.0, 0.0, 0.0);
        // TODO: Use planetary_.interpolateSunPosition(time) if available

        // y_s = cross(z_s, e_sun) following RTKLIB/MATLAB convention
        Eigen::Vector3d y_s = z_s.cross(e_sun);
        if (y_s.norm() < 1e-8) {
            y_s = sat_vel.norm() > 1e-8 ? sat_vel.normalized().cross(z_s) : Eigen::Vector3d(0, 1, 0);
        }
        y_s.normalize();
    
        // x_s completes right-handed frame
        Eigen::Vector3d x_s = y_s.cross(z_s);
        x_s.normalize();

        // Receiver local frame (ENU-like)
        // Compute geodetic lat/lon for receiver
        double rx_norm = rx_pos.norm();
        if (rx_norm < 1e3) {
            return 0.0;
        }
        double phi = std::asin(rx_pos.z() / rx_norm);  // latitude
        double lam = std::atan2(rx_pos.y(), rx_pos.x()); // longitude

        // Receiver dipole directions (north and west in ECEF)
        Eigen::Vector3d x_r(-std::sin(phi) * std::cos(lam), 
                        -std::sin(phi) * std::sin(lam), 
                         std::cos(phi));  // North
        Eigen::Vector3d y_r( std::sin(lam), 
                        -std::cos(lam), 
                         0.0);            // West

        // Effective dipoles (MATLAB convention)
        // Ds = xs - k*(k·xs) - cross(k, ys)   <- satellite
        // Dr = xr - k*(k·xr) + cross(k, yr)   <- receiver
        Eigen::Vector3d Ds = x_s - k * (k.dot(x_s)) - k.cross(y_s);
        Eigen::Vector3d Dr = x_r - k * (k.dot(x_r)) + k.cross(y_r);

        double Ds_norm = Ds.norm();
        double Dr_norm = Dr.norm();
        if (Ds_norm < 1e-10 || Dr_norm < 1e-10) {
            log_manager_.getLogger("ppp_model")->trace("Phase wind-up for {}: degenerate dipole", satellite);
            return 0.0;
        }

        // Compute angle between effective dipoles
        double alpha = Ds.dot(Dr) / (Ds_norm * Dr_norm);
        alpha = std::clamp(alpha, -1.0, 1.0);  // Numerical safety

        double windUp_c = std::acos(alpha);  // radians

        // Determine sign using cross product
        if (k.dot(Ds.cross(Dr)) < 0.0) {
            windUp_c = -windUp_c;
        }

        // TODO: Add phase unwrapping with previous epoch storage
        // windUp_c = 2*PI * round((windUp_p - windUp_c) / (2*PI)) + windUp_c;

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

} // namespace GnssProcessor