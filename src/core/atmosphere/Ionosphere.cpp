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
     * @brief Ionospheric helper implementation: Klobuchar coefficient loading and ionospheric/tropospheric estimators.
     *
     * Implements loading of Klobuchar coefficients from RINEX/BRDC products and
     * provides Klobuchar single-frequency delay computation and a simple
     * initial zenith ionospheric delay estimate used for PPP initialization.
     */

    /**
     * @brief Construct an Ionosphere helper.
     *
     * Initializes geometry/model helpers and attempts to load Klobuchar
     * coefficients via configured product paths.
     *
     * @param config Read-only reference to configuration.
     * @param log_mgr Reference to LogManager for logging.
     * @param parser ProductParser used to locate/parse auxiliary products.
     * @param rinex RinexParser reference used by geometry/parser helpers.
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
     * Searches for the RNX product configured in `config_`, opens the file,
     * scans the RINEX header for Klobuchar alpha (GPSA/GAL) and beta (GPSB)
     * lines and stores eight coefficients (alpha[4], beta[4]) in
     * `klobuchar_coeffs_`. If parsing fails or the file doesn't exist, the
     * coefficient vector is left empty and warnings are logged.
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
                // RINEX inherits Fortran's 'D' exponent marker, and real files use both:
                // IGS BRDC for 2024 DOY 129 writes "2.5146E-08" while DOY 131 writes
                // "3.4459D-08".  operator>>(double) accepts only 'E', so without this
                // normalisation a perfectly valid nav file yields ZERO coefficients and
                // Klobuchar silently disappears.  (RinexNavParser::parseRinexDouble does
                // the same for nav DATA records; the header path had been missed.)
                std::string norm = line;
                for (char& c : norm)
                    if (c == 'D' || c == 'd') c = 'E';
                std::istringstream iss(norm);
                std::string type;
                iss >> type;
                // GPS Klobuchar α only.  A RINEX 3 "GAL" line is Galileo
                // NeQuick-G (ai0..ai2), not Klobuchar α — see the matching
                // note in ProductParser::parseKlobuchar.
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
     * for a single-frequency observation. The implementation expects
     * `klobuchar_coeffs_` to contain 8 values (alpha[4], beta[4]).
     *
     * @param time GNSS epoch (GPS week/tow).
     * @param rx_pos Receiver ECEF position in meters.
     * @param sat_pos Satellite ECEF position in meters.
     * @param frequency Signal frequency in Hz (used for frequency scaling).
     * @return Estimated ionospheric delay in meters.
     */
    double Ionosphere::computeKlobucharDelay(const GnssTime& time,
        const Eigen::Vector3d& rx_pos,
        const Eigen::Vector3d& sat_pos,
        double frequency) const {
        double a0 = klobuchar_coeffs_[0], a1 = klobuchar_coeffs_[1], a2 = klobuchar_coeffs_[2], a3 = klobuchar_coeffs_[3];
        double b0 = klobuchar_coeffs_[4], b1 = klobuchar_coeffs_[5], b2 = klobuchar_coeffs_[6], b3 = klobuchar_coeffs_[7];

        double t = fmod(time.tow, TimeUtils::SECONDS_IN_DAY);
        // FIXED 2026-08-19: phi previously came from asin(z/|r|), which is the
        // GEOCENTRIC latitude, while the comment -- and IS-GPS-200, which
        // specifies the user geodetic latitude phi_u in semicircles -- call for
        // the GEODETIC one.  The two differ by up to 0.192 deg at 45 deg latitude
        // (~0.0011 semicircles into phi_m).  Route through the tree's single
        // ellipsoid solver, as every other latitude consumer does; that also
        // handles the p ~ 0 polar case the raw asin left as a NaN.  Longitude is
        // identical in both conventions, so that line is only re-labelled.
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
     * When Klobuchar coefficients are available this routine applies the
     * Klobuchar model to produce a zenith delay (meters). If coefficients
     * are missing a conservative default (5 meters) is returned.
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
        // FIXED 2026-08-19: phi previously came from asin(z/|r|), which is the
        // GEOCENTRIC latitude, while the comment -- and IS-GPS-200, which
        // specifies the user geodetic latitude phi_u in semicircles -- call for
        // the GEODETIC one.  The two differ by up to 0.192 deg at 45 deg latitude
        // (~0.0011 semicircles into phi_m).  Route through the tree's single
        // ellipsoid solver, as every other latitude consumer does; that also
        // handles the p ~ 0 polar case the raw asin left as a NaN.  Longitude is
        // identical in both conventions, so that line is only re-labelled.
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