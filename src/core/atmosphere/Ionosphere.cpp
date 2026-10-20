#include "atmosphere/Ionosphere.hpp"
#include "common/Constants.hpp"
#include "math/Coordinates.hpp"
#include "products/ProductCache.hpp"
#include <fstream>
#include <sstream>
#include <filesystem>

namespace GnssProcessor {

    /**
     * @file Ionosphere.cpp
     * @brief Ionospheric helper implementation: Klobuchar coefficient loading and ionospheric delay estimators.
     *
     * Loads the GPS Klobuchar coefficients from the RINEX/BRDC navigation product
     * and computes the Klobuchar single-frequency slant delay and a coarse zenith
     * ionospheric delay used for PPP initialization.
     */

    /**
     * @brief Construct an Ionosphere helper.
     *
     * Initializes the geometry helper, takes the Klobuchar coefficients from
     * ProductCache, and parses them from the configured RNX product when the
     * cache holds none.
     *
     * @param config Read-only reference to configuration.
     * @param log_mgr Reference to LogManager for logging.
     * @param parser ProductParser; not used by this constructor.
     * @param rinex RinexParser reference passed to the geometry helper.
     */
    Ionosphere::Ionosphere(const GnssConfig& config, LogManager& log_mgr, ProductParser& parser, const RinexParser& rinex)
        : log_manager_(log_mgr), config_(config),
        model_geometry_(config, ProductCache::getInstance(), rinex, log_mgr), klobuchar_coeffs_{} {
        // Prefer cached Klobuchar coefficients; fall back to parsing RNX if cache empty.
        auto& cache = ProductCache::getInstance();
        klobuchar_coeffs_ = cache.getKlobucharCoeffs();
        if (klobuchar_coeffs_.empty()) {
            loadKlobucharCoeffs();
        }

        log_manager_.getLogger("ionosphere")->info("Ionosphere model initialized: Klobuchar coeffs loaded ({} values), mode={}",
            klobuchar_coeffs_.size(), config_.getProcessingMode() == ProcessingMode::PPP ? "PPP" : "PPP-RTK");
    }

    /**
     * @brief Load Klobuchar coefficients from configured RINEX/BRDC product.
     *
     * Opens the RNX product configured in `config_`, scans the RINEX header
     * for the GPS Klobuchar alpha (GPSA) and beta (GPSB) lines and stores eight
     * coefficients (alpha[4], beta[4]) in `klobuchar_coeffs_`. If the path is
     * missing, the file cannot be opened, or the header does not yield exactly
     * eight finite coefficients, the vector is left empty and a warning or
     * error is logged.
     */
    void Ionosphere::loadKlobucharCoeffs() {
        klobuchar_coeffs_.clear();

        // Get RINEX/BRDC path from configuration
        std::filesystem::path rnx_path = config_.getProductPath("RNX");
        if (rnx_path.empty() || !std::filesystem::exists(rnx_path)) {
            log_manager_.getLogger("ionosphere")->warn("No RNX/BRDC product path configured or file does not exist for Klobuchar coefficients");
            return;
        }

        std::ifstream file(rnx_path);
        if (!file.is_open()) {
            log_manager_.getLogger("ionosphere")->error("Failed to open RNX/BRDC file for Klobuchar coefficients: {}", rnx_path.string());
            return;
        }

        std::string line;
        bool in_header = true;
        size_t line_count = 0;

        while (std::getline(file, line)) {
            ++line_count;
            if (in_header && line.find("END OF HEADER") != std::string::npos) {
                in_header = false;
                continue;
            }
            if (in_header && line.find("IONOSPHERIC CORR") != std::string::npos) {
                // RINEX inherits Fortran's 'D' exponent marker, and IGS BRDC files use
                // either 'E' ("2.5146E-08") or 'D' ("3.4459D-08").  operator>>(double)
                // accepts only 'E', so 'D' is normalised to 'E' here; otherwise a valid
                // nav file yields no coefficients and the Klobuchar model is not
                // available.  RinexNavParser::parseRinexDouble does the same for the nav
                // data records.
                std::string norm = line;
                for (char& c : norm)
                    if (c == 'D' || c == 'd') c = 'E';
                std::istringstream iss(norm);
                std::string type;
                iss >> type;
                // GPS Klobuchar α and β only.  A RINEX 3 "GAL" line holds Galileo
                // NeQuick-G coefficients (ai0..ai2), not Klobuchar α; see the
                // matching note in ProductParser::parseKlobuchar.
                if (type == "GPSA") { // alpha
                    double a0, a1, a2, a3;
                    if (iss >> a0 >> a1 >> a2 >> a3) {
                        if (std::isfinite(a0) && std::isfinite(a1) && std::isfinite(a2) && std::isfinite(a3)) {
                            klobuchar_coeffs_.push_back(a0);
                            klobuchar_coeffs_.push_back(a1);
                            klobuchar_coeffs_.push_back(a2);
                            klobuchar_coeffs_.push_back(a3);
                        }
                        else {
                            log_manager_.getLogger("ionosphere")->warn("Non-finite Klobuchar alpha coefficients at line {}: {}", line_count, line);
                        }
                    }
                    else {
                        log_manager_.getLogger("ionosphere")->warn("Invalid Klobuchar alpha coefficients at line {}: {}", line_count, line);
                    }
                }
                else if (type == "GPSB") { // beta
                    double b0, b1, b2, b3;
                    if (iss >> b0 >> b1 >> b2 >> b3) {
                        if (std::isfinite(b0) && std::isfinite(b1) && std::isfinite(b2) && std::isfinite(b3)) {
                            klobuchar_coeffs_.push_back(b0);
                            klobuchar_coeffs_.push_back(b1);
                            klobuchar_coeffs_.push_back(b2);
                            klobuchar_coeffs_.push_back(b3);
                        }
                        else {
                            log_manager_.getLogger("ionosphere")->warn("Non-finite Klobuchar beta coefficients at line {}: {}", line_count, line);
                        }
                    }
                    else {
                        log_manager_.getLogger("ionosphere")->warn("Invalid Klobuchar beta coefficients at line {}: {}", line_count, line);
                    }
                }
            }
        }

        file.close();

        if (klobuchar_coeffs_.size() != 8) {
            log_manager_.getLogger("ionosphere")->warn("Incomplete Klobuchar coefficients from {}: expected 8, got {}. Clearing coefficients and using defaults.",
                rnx_path.string(), klobuchar_coeffs_.size());
            klobuchar_coeffs_.clear();
        }
        else {
            log_manager_.getLogger("ionosphere")->info("Loaded Klobuchar coefficients from {}: alpha=[{:.3e}, {:.3e}, {:.3e}, {:.3e}], beta=[{:.3e}, {:.3e}, {:.3e}, {:.3e}]",
                rnx_path.string(),
                klobuchar_coeffs_[0], klobuchar_coeffs_[1], klobuchar_coeffs_[2], klobuchar_coeffs_[3],
                klobuchar_coeffs_[4], klobuchar_coeffs_[5], klobuchar_coeffs_[6], klobuchar_coeffs_[7]);
        }
    }

    /**
     * @brief Compute Klobuchar ionospheric delay for a single observation.
     *
     * Implements the standard Klobuchar model to estimate ionospheric delay
     * for a single-frequency observation. Without 8 coefficients (alpha[4],
     * beta[4]) there is no model to evaluate, and the delay is 0 m, the same
     * answer @c PppModel gives when it has none.
     *
     * @param time GNSS epoch (GPS week/tow).
     * @param rx_pos Receiver ECEF position in meters.
     * @param sat_pos Satellite ECEF position in meters.
     * @param frequency Signal frequency in Hz (used for frequency scaling).
     * @return Estimated ionospheric delay in meters; 0 when the 8 coefficients are not loaded.
     */
    double Ionosphere::computeKlobucharDelay(const GnssTime& time,
        const Eigen::Vector3d& rx_pos,
        const Eigen::Vector3d& sat_pos,
        double frequency) const {
        if (klobuchar_coeffs_.size() != 8) return 0.0;

        double a0 = klobuchar_coeffs_[0], a1 = klobuchar_coeffs_[1], a2 = klobuchar_coeffs_[2], a3 = klobuchar_coeffs_[3];
        double b0 = klobuchar_coeffs_[4], b1 = klobuchar_coeffs_[5], b2 = klobuchar_coeffs_[6], b3 = klobuchar_coeffs_[7];

        double t = fmod(time.tow, TimeUtils::SECONDS_IN_DAY);
        // IS-GPS-200 specifies the user geodetic latitude phi_u (semicircles).  It
        // comes from Coordinates::ecefToLlh, the ellipsoid solver used by every
        // other latitude consumer, which also handles the polar case (p ~ 0).  The
        // geocentric latitude asin(z/|r|) differs by up to 0.192 deg at 45 deg
        // (about 0.0011 semicircles in phi_m).  Longitude is the same in both
        // conventions.
        const Coordinates::Llh rx_llh = Coordinates::ecefToLlh(rx_pos);
        double phi = rx_llh.lat_rad * 180.0 / Constants::PI;    // Geodetic latitude (degrees)
        double lambda = rx_llh.lon_rad * 180.0 / Constants::PI; // Geodetic longitude (degrees)

        GnssProcessor::Model::Topocentric topo = model_geometry_.computeTopocentric(rx_pos, sat_pos);
        double el = topo.elevation_deg;
        double az = topo.azimuth_deg;
        if (az < 0.0) az += 360.0;

        double psi = 0.0137 / (el / 180.0 + 0.11) - 0.022;
        double phi_i = phi / 180.0 + psi * std::cos(az * Constants::PI / 180.0);
        if (phi_i > 0.416) phi_i = 0.416;
        if (phi_i < -0.416) phi_i = -0.416;
        double lambda_i = lambda / 180.0 + psi * std::sin(az * Constants::PI / 180.0) / std::cos(phi_i * Constants::PI);
        double phi_m = phi_i + 0.064 * std::cos((lambda_i - 1.617) * Constants::PI);

        double t_i = 43200.0 * lambda_i + t;
        t_i = fmod(t_i, TimeUtils::SECONDS_IN_DAY);
        if (t_i < 0) t_i += TimeUtils::SECONDS_IN_DAY;

        double F = 1.0 + 16.0 * std::pow((0.53 - el / 180.0), 3);
        double AMP = a0 + a1 * phi_m + a2 * std::pow(phi_m, 2) + a3 * std::pow(phi_m, 3);
        if (AMP < 0) AMP = 0;
        double PER = b0 + b1 * phi_m + b2 * std::pow(phi_m, 2) + b3 * std::pow(phi_m, 3);
        if (PER < 72000) PER = 72000;
        double X = 2.0 * Constants::PI * (t_i - 50400.0) / PER;
        double delay;
        if (std::abs(X) < 1.57) {
            delay = F * (5e-9 + AMP * (1 - X * X / 2 + X * X * X * X / 24));
        }
        else {
            delay = F * 5e-9;
        }

        double f_L1 = Constants::GPS_L1_FREQ;
        delay *= (f_L1 * f_L1) / (frequency * frequency);
        return delay * Constants::SPEED_OF_LIGHT;
    }

    /**
     * @brief Compute a coarse initial ionospheric zenith delay for PPP initialization.
     *
     * With 8 Klobuchar coefficients loaded, evaluates the Klobuchar vertical
     * delay at L1 with the receiver itself as the pierce point (no obliquity
     * factor, no pierce-point offset) and returns it in meters. Without them,
     * returns a default of 5 meters.
     *
     * @param time GNSS epoch (GPS week/tow).
     * @param rx_pos Receiver ECEF position in meters.
     * @return Estimated zenith ionospheric delay in meters.
     */
    double Ionosphere::computeInitialIonoDelay(const GnssTime& time, const Eigen::Vector3d& rx_pos) const {
        if (klobuchar_coeffs_.size() != 8) {
            return 5.0; // Default fallback
        }

        // Klobuchar model for zenith delay at receiver position
        double a0 = klobuchar_coeffs_[0], a1 = klobuchar_coeffs_[1], a2 = klobuchar_coeffs_[2], a3 = klobuchar_coeffs_[3];
        double b0 = klobuchar_coeffs_[4], b1 = klobuchar_coeffs_[5], b2 = klobuchar_coeffs_[6], b3 = klobuchar_coeffs_[7];

        double t = fmod(time.tow, TimeUtils::SECONDS_IN_DAY);
        // Geodetic latitude from Coordinates::ecefToLlh, as IS-GPS-200 specifies;
        // see the same note in computeKlobucharDelay.
        const Coordinates::Llh rx_llh = Coordinates::ecefToLlh(rx_pos);
        double phi = rx_llh.lat_rad * 180.0 / Constants::PI;    // Geodetic latitude (degrees)
        double lambda = rx_llh.lon_rad * 180.0 / Constants::PI; // Geodetic longitude (degrees)

        double phi_i = phi / 180.0; // Normalized latitude
        double lambda_i = lambda / 180.0; // Normalized longitude
        double phi_m = phi_i + 0.064 * std::cos((lambda_i - 1.617) * Constants::PI);

        double t_i = 43200.0 * lambda_i + t;
        t_i = fmod(t_i, TimeUtils::SECONDS_IN_DAY);
        if (t_i < 0) t_i += TimeUtils::SECONDS_IN_DAY;

        double AMP = a0 + a1 * phi_m + a2 * std::pow(phi_m, 2) + a3 * std::pow(phi_m, 3);
        if (AMP < 0) AMP = 0;
        double PER = b0 + b1 * phi_m + b2 * std::pow(phi_m, 2) + b3 * std::pow(phi_m, 3);
        if (PER < 72000) PER = 72000;
        double X = 2.0 * Constants::PI * (t_i - 50400.0) / PER;

        double delay;
        if (std::abs(X) < 1.57) {
            delay = 5e-9 + AMP * (1 - X * X / 2 + X * X * X * X / 24);
        }
        else {
            delay = 5e-9;
        }

        // Convert to meters (zenith delay at L1 frequency)
        delay *= Constants::SPEED_OF_LIGHT;
        return delay;
    }

}