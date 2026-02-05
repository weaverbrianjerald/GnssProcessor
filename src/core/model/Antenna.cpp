#include "Antenna.hpp"
#include "ProductCache.hpp"
#include <Eigen/Dense>

namespace GnssProcessor {

    /**
     * @file Antenna.cpp
     * @brief Implementation of Antenna corrections: PCO/PCV interpolation, phase wind-up,
     *        and antenna data loading from ATX products.
     */

    /**
     * @brief Construct an Antenna helper.
     *
     * Loads antenna models from products (ATX) via the provided ProductParser and
     * initializes internal helpers used to compute antenna corrections.
     *
     * @param config Configuration reference (read-only).
     * @param product_parser Product parser used to access auxiliary files.
     * @param log_mgr Log manager used for warnings/errors and tracing.
     * @param rinex_parser Reference to the RINEX parser for receiver metadata.
     */
    Antenna::Antenna(const GnssConfig& config, ProductParser& product_parser,
        LogManager& log_mgr, const RinexParser& rinex_parser)
        : log_manager_(log_mgr),
        config_(config),
        gnss_corrections_(log_mgr),
        rinex_parser_(rinex_parser),
        product_parser_(product_parser),
        ppp_utils_(log_mgr),
        planetary_(log_mgr),
        // Initialize reference from cache - NO COPY
        antennas_(ProductCache::getInstance().getAntennas())
    {
        // No need to call loadAntennaData() - we already have reference
        log_manager_.getLogger("antenna_model")->info(
            "Antenna init (zero-copy): {} types, rx={}",
            antennas_.size(), getReceiverAntennaType());
    }

    /**
     * @brief Compute antenna correction (meters) for a satellite/receiver observation.
     *
     * This function computes and returns the combined antenna correction including:
     * - Satellite phase center offset (PCO) projected onto the satellite-to-receiver line-of-sight.
     * - Satellite phase center variation (PCV) interpolated from the antenna PCV grid.
     * - Receiver PCO/PCV corrections (subtracted from the measured range).
     * - Phase wind-up correction for carrier-phase observations.
     *
     * The implementation:
     * - Maps internal frequency labels to ATX frequency keys based on GNSS system.
     * - Computes a satellite attitude frame using yaw-steering (velocity + Sun direction).
     * - Matches antenna models by satellite identifier and by receiver type (with simple normalization).
     * - Safely handles missing data and logs warnings/traces for diagnostics.
     *
     * @param satellite Satellite identifier string (e.g. "G01", "R05").
     * @param freq_type Frequency label used by processing (e.g. "L1", "C1").
     * @param system GNSS system enum for the satellite.
     * @param rx_pos Receiver ECEF position (meters).
     * @param sat_pos Satellite ECEF position (meters).
     * @param sat_vel Satellite ECEF velocity (meters/sec).
     * @param elevation Receiver-measured elevation angle (degrees).
     * @param azimuth Receiver-measured azimuth angle (degrees).
     * @param rx_antenna_type Receiver antenna model/type string (from RINEX header or configuration).
     * @param time Epoch time at which the correction is computed.
     * @return Antenna correction in meters. Positive values follow the convention used by the caller.
     */
    double Antenna::getAntennaCorrection(const std::string& satellite,
        const std::string& freq_type,
        System system,
        const Eigen::Vector3d& rx_pos,
        const Eigen::Vector3d& sat_pos,
        const Eigen::Vector3d& sat_vel,
        double elevation,
        double azimuth,
        const std::string& rx_antenna_type,
        const GnssTime& time) const {

        double correction = 0.0;
        bool is_phase = freq_type[0] == 'L';
        double wavelength = is_phase ? ppp_utils_.getWavelength(system, freq_type, satellite) : 0.0;

        if (!rx_pos.allFinite() || !sat_pos.allFinite() || !sat_vel.allFinite() || !std::isfinite(elevation) || !std::isfinite(azimuth)) {
            log_manager_.getLogger("antenna")->warn("Invalid inputs for antenna correction: satellite={}, freq_type={}, "
                "rx_pos=({}, {}, {}), sat_pos=({}, {}, {}), sat_vel=({}, {}, {}), elevation={:.3f}, azimuth={:.3f}",
                satellite, freq_type, rx_pos.x(), rx_pos.y(), rx_pos.z(),
                sat_pos.x(), sat_pos.y(), sat_pos.z(),
                sat_vel.x(), sat_vel.y(), sat_vel.z(),
                elevation, azimuth);
            return 0.0;
        }
        if (elevation < 0.0 || elevation > 90.0 || azimuth < 0.0 || azimuth >= 360.0) {
            log_manager_.getLogger("antenna")->warn("Out-of-range elevation ({:.3f}) or azimuth ({:.3f}) for satellite {}, freq_type={}",
                elevation, azimuth, satellite, freq_type);
            return 0.0;
        }

        std::string atx_freq;
        if (system == System::GPS) {
            if (freq_type.find("C1") == 0 || freq_type.find("L1") == 0) { atx_freq = "G01"; }
            else if (freq_type.find("C2") == 0 || freq_type.find("L2") == 0) { atx_freq = "G02"; }
            else if (freq_type.find("C5") == 0 || freq_type.find("L5") == 0) { atx_freq = "G05"; }
        }
        else if (system == System::Galileo) {
            if (freq_type.find("C1") == 0 || freq_type.find("L1") == 0) { atx_freq = "E01"; }
            else if (freq_type.find("C5") == 0 || freq_type.find("L5") == 0) { atx_freq = "E05"; }
            else if (freq_type.find("C7") == 0 || freq_type.find("L7") == 0) { atx_freq = "E07"; }
            else if (freq_type.find("C6") == 0 || freq_type.find("L6") == 0) { atx_freq = "E06"; }
            else if (freq_type.find("C8") == 0 || freq_type.find("L8") == 0) { atx_freq = "E08"; }
        }
        else if (system == System::GLONASS) {
            if (freq_type.find("C1") == 0 || freq_type.find("L1") == 0) { atx_freq = "R01"; }
            else if (freq_type.find("C2") == 0 || freq_type.find("L2") == 0) { atx_freq = "R02"; }
            else if (freq_type.find("C3") == 0 || freq_type.find("L3") == 0) { atx_freq = "R03"; }
        }
        else if (system == System::BeiDou) {
            if (freq_type.find("C2") == 0 || freq_type.find("L2") == 0) { atx_freq = "C02"; }
            else if (freq_type.find("C7") == 0 || freq_type.find("L7") == 0) { atx_freq = "C07"; }
            else if (freq_type.find("C6") == 0 || freq_type.find("L6") == 0) { atx_freq = "C06"; }
            else if (freq_type.find("C1") == 0 || freq_type.find("L1") == 0) { atx_freq = "C01"; }
            else if (freq_type.find("C5") == 0 || freq_type.find("L5") == 0) { atx_freq = "C05"; }
            else if (freq_type.find("C8") == 0 || freq_type.find("L8") == 0) { atx_freq = "C08"; }
        }
        else if (system == System::QZSS) {
            if (freq_type.find("C1") == 0 || freq_type.find("L1") == 0) { atx_freq = "J01"; }
            else if (freq_type.find("C2") == 0 || freq_type.find("L2") == 0) { atx_freq = "J02"; }
            else if (freq_type.find("C5") == 0 || freq_type.find("L5") == 0) { atx_freq = "J05"; }
            else if (freq_type.find("C6") == 0 || freq_type.find("L6") == 0) { atx_freq = "J06"; }
        }
        else if (system == System::SBAS) {
            if (freq_type.find("C1") == 0 || freq_type.find("L1") == 0) { atx_freq = "S01"; }
            else if (freq_type.find("C5") == 0 || freq_type.find("L5") == 0) { atx_freq = "S05"; }
        }

        if (atx_freq.empty()) {
            log_manager_.getLogger("antenna")->warn("No ATX frequency mapping for satellite {}, freq_type={}, system={}",
                satellite, freq_type, static_cast<int>(system));
            return 0.0;
        }

        // LOS from receiver to satellite (ECEF)
        Eigen::Vector3d los_rx2sat = (sat_pos - rx_pos);
        double los_norm = los_rx2sat.norm();
        if (los_norm == 0.0) {
            log_manager_.getLogger("antenna")->warn("Invalid LOS for satellite {}, freq_type={} at TOW {:.3f}", satellite, freq_type, time.tow);
            return 0.0;
        }
        los_rx2sat /= los_norm;

        // Satellite attitude (yaw-steering: vel + Sun)
        Eigen::Vector3d sun_pos = planetary_.interpolateSunPosition(time);
        if (sun_pos.norm() < 1e6) {
            sun_pos = Eigen::Vector3d(1.496e11, 0.0, 0.0); // fallback 1 AU
            log_manager_.getLogger("antenna")->trace("Antenna: missing Sun ephemeris at TOW {:.3f}, using fallback", time.tow);
        }
        Eigen::Vector3d e_sun = (sun_pos - sat_pos).normalized();
        Eigen::Vector3d z_s = -sat_pos.normalized(); // to Earth
        Eigen::Vector3d vhat = sat_vel.norm() > 1e-8 ? sat_vel.normalized() : Eigen::Vector3d::Zero();
        Eigen::Vector3d y_s = vhat.cross(e_sun);
        if (y_s.norm() < 1e-8) {
            y_s = z_s.cross(e_sun); // fallback
        }
        y_s.normalize();
        Eigen::Vector3d x_s = y_s.cross(z_s);
        if (x_s.norm() < 1e-8) {
            x_s = Eigen::Vector3d(1, 0, 0); // final fallback
        }
        x_s.normalize();

        Eigen::Matrix3d ecef_to_sat;
        ecef_to_sat.row(0) = x_s.transpose();
        ecef_to_sat.row(1) = y_s.transpose();
        ecef_to_sat.row(2) = z_s.transpose();

        // Vector from satellite to receiver in sat frame
        Eigen::Vector3d sat2rx_ecef = -los_rx2sat; // from sat to rx
        Eigen::Vector3d sat2rx_sat = ecef_to_sat * sat2rx_ecef;

        // Satellite antenna selection with validity interval
        const AntennaData* sat_entry = nullptr;
        for (const auto& a : antennas_) {
            // Match satellite PRN at the start of the type string
            // Format: "PRN_BLOCK_SVN" e.g. "G04_BLOCKIIIA_G074"
            const bool id_match =
                (a.type.find(satellite + "_") == 0) ||
                (a.type.find("_" + satellite + "_") != std::string::npos);
            if (!id_match) continue;

            const bool has_valid_from = (a.valid_from.week > 0);
            const bool has_valid_until = (a.valid_until.week > 0);

            if (has_valid_from) {
                const bool after_start = !(time < a.valid_from);
                const bool before_end = has_valid_until ? (time < a.valid_until) : true;

                if (after_start && before_end) {
                    sat_entry = &a;
                    log_manager_.getLogger("antenna")->debug(
                        "Matched satellite antenna for {}: type='{}' valid [{:d}:{:.0f}, {:d}:{:.0f})",
                        satellite, a.type, 
                        a.valid_from.week, a.valid_from.tow,
                        a.valid_until.week, a.valid_until.tow);
                    break;
                }
            }
            else if (!sat_entry) {
                sat_entry = &a; // fallback if no validity info
                log_manager_.getLogger("antenna")->debug(
                    "Matched satellite antenna for {} (no validity): type='{}'",
                    satellite, a.type);
            }
        }

        if (!sat_entry) {
            log_manager_.getLogger("antenna")->warn(
                "No satellite antenna entry found for {} at {:d}:{:.3f}",
                satellite, time.week, time.tow);
        }

        double sat_pco_corr = 0.0, sat_pcv_corr = 0.0;
        if (sat_entry && sat_entry->pco.count(atx_freq)) {
            Eigen::Vector3d pco_sat = sat_entry->pco.at(atx_freq) * 1e-3; // in sat frame
            if (pco_sat.allFinite()) {
                sat_pco_corr = pco_sat.dot(sat2rx_sat);
                //correction += sat_pco_corr;
                correction -= sat_pco_corr;
            } else {
                log_manager_.getLogger("antenna")->warn("Invalid satellite PCO for {} {} atx_freq={} at TOW {:.3f}",
                    satellite, sat_entry->type, atx_freq, time.tow);
            }

            if (sat_entry->pcv.count(atx_freq) && !sat_entry->pcv.at(atx_freq).empty()) {
                const auto& pcv_table = sat_entry->pcv.at(atx_freq);
                double zenith = std::acos(std::clamp(sat2rx_sat.z(), -1.0, 1.0)) * 180.0 / Constants::PI;
                double az_sat = std::atan2(sat2rx_sat.y(), sat2rx_sat.x()) * 180.0 / Constants::PI;
                if (az_sat < 0.0) az_sat += 360.0;
                double dazi = sat_entry->dazi > 0.0 ? sat_entry->dazi : 5.0;
                double dzen = sat_entry->dzen > 0.0 ? sat_entry->dzen : 5.0;
                double pcv = interpolatePcv(pcv_table, zenith, az_sat, dazi, dzen) * 1e-3;
                if (std::isfinite(pcv)) {
                    sat_pcv_corr = pcv;
                    correction -= sat_pcv_corr;
                } else {
                    log_manager_.getLogger("antenna")->warn("Invalid satellite PCV for {} {} atx_freq={} at TOW {:.3f}",
                        satellite, sat_entry->type, atx_freq, time.tow);
                }
            }

            if (sat_entry->valid_from.week > 0) {
                log_manager_.getLogger("antenna")->trace(
                    "Satellite antenna {} valid [{:d}:{:.3f},{:d}:{:.3f}) used for {} at {:d}:{:.3f}",
                    sat_entry->type,
                    sat_entry->valid_from.week, sat_entry->valid_from.tow,
                    sat_entry->valid_until.week, sat_entry->valid_until.tow,
                    satellite, time.week, time.tow);
            }
        }

        // Receiver antenna correction (sanitized + safer matching)
        double rx_pco_corr = 0.0, rx_pcv_corr = 0.0;

        // Sanitize input (remove whitespace)
        std::string rx_type_sanitized;
        rx_type_sanitized.reserve(rx_antenna_type.size());
        for (char c : rx_antenna_type) {
            if (!std::isspace(static_cast<unsigned char>(c))) rx_type_sanitized.push_back(c);
        }

        // ATX receiver antenna types are stored as "MODEL_SERIAL" (underscore separator),
        // while RINEX may provide "MODEL SERIAL" (space separator). Build an underscore-normalized
        // variant to improve matching.
        std::string rx_type_underscore;
        rx_type_underscore.reserve(rx_antenna_type.size());
        for (char c : rx_antenna_type) {
            if (std::isspace(static_cast<unsigned char>(c))) {
                if (!rx_type_underscore.empty() && rx_type_underscore.back() != '_')
                    rx_type_underscore.push_back('_');
            } else {
                rx_type_underscore.push_back(c);
            }
        }
        if (!rx_type_underscore.empty() && rx_type_underscore.back() == '_')
            rx_type_underscore.pop_back();

        const AntennaData* rx_entry = nullptr;

        for (const auto& a : antennas_) {
            if (a.type == rx_type_sanitized || a.type == rx_type_underscore) {
                rx_entry = &a;
                break;
            }
        }

        if (!rx_entry) {
            std::vector<const AntennaData*> candidates;
            candidates.reserve(4);

            for (const auto& a : antennas_) {
                if ((!rx_type_sanitized.empty() && a.type.find(rx_type_sanitized) != std::string::npos) ||
                    (!rx_type_underscore.empty() && a.type.find(rx_type_underscore) != std::string::npos)) {
                    candidates.push_back(&a);
                }
            }

            if (candidates.size() == 1) {
                rx_entry = candidates.front();
            } else if (candidates.size() > 1) {
                log_manager_.getLogger("antenna")->warn(
                    "Ambiguous receiver antenna match for '{}' (normalized='{}'), {} candidates: using first '{}'",
                    rx_type_sanitized, rx_type_underscore, candidates.size(), candidates.front()->type);
                rx_entry = candidates.front();
            }
        }

        if (rx_entry && rx_entry->pco.count(atx_freq)) {
            Eigen::Vector3d pco = rx_entry->pco.at(atx_freq) * 1e-3;
            if (pco.allFinite()) {
                rx_pco_corr = pco.dot(los_rx2sat);
                correction += rx_pco_corr;
            } else {
                log_manager_.getLogger("antenna")->warn("Invalid receiver PCO for {} {} atx_freq={} at TOW {:.3f}",
                    satellite, rx_type_sanitized, atx_freq, time.tow);
            }

            if (rx_entry->pcv.count(atx_freq) && !rx_entry->pcv.at(atx_freq).empty()) {
                const auto& pcv_table = rx_entry->pcv.at(atx_freq);
                double zenith = 90.0 - elevation;
                double dazi = rx_entry->dazi > 0.0 ? rx_entry->dazi : 5.0;
                double dzen = rx_entry->dzen > 0.0 ? rx_entry->dzen : 5.0;
                double pcv = interpolatePcv(pcv_table, zenith, azimuth, dazi, dzen) * 1e-3;
                if (std::isfinite(pcv)) {
                    rx_pcv_corr = pcv;
                    correction += rx_pcv_corr;
                } else {
                    log_manager_.getLogger("antenna")->warn("Invalid receiver PCV for {} {} atx_freq={} at TOW {:.3f}",
                        satellite, rx_type_sanitized, atx_freq, time.tow);
                }
            }
        }

        double wind_up_corr = 0.0;
        if (is_phase && wavelength > 0.0) {
            double wind_up = gnss_corrections_.computePhaseWindUp(satellite, system, rx_pos, sat_pos, wavelength, time);
            if (std::isfinite(wind_up)) {
                wind_up_corr = wind_up;
                correction -= wind_up_corr;
            } else {
                log_manager_.getLogger("antenna")->warn("Invalid phase wind-up for {} {} at TOW {:.3f}", satellite, freq_type, time.tow);
            }
        }

        if (std::isfinite(correction) && correction != 0.0) {
            log_manager_.getLogger("antenna")->trace("Antenna correction for {} {}: total={:.6f} m (sat_pco={:.6f}, sat_pcv={:.6f}, "
                "rx_pco={:.6f}, rx_pcv={:.6f}, wind_up={:.6f}) at TOW {:.3f}, dazi={:.3f}, dzen={:.3f}",
                satellite, freq_type, correction, sat_pco_corr, sat_pcv_corr,
                -rx_pco_corr, -rx_pcv_corr, wind_up_corr, time.tow,
                sat_entry ? sat_entry->dazi : 5.0, sat_entry ? sat_entry->dzen : 5.0);
        } else {
            log_manager_.getLogger("antenna")->trace("No antenna correction for {} {} at TOW {:.3f}", satellite, freq_type, time.tow);
        }
        return correction;
    }

    /**
     * @brief Return the receiver antenna type string from the RINEX parser.
     *
     * The returned string is sanitized (whitespace removed) to ease matching
     * against ATX entries.
     *
     * @return Sanitized receiver antenna type string (empty if unknown).
     */
    std::string Antenna::getReceiverAntennaType() const {
        std::string antenna_type = rinex_parser_.getReceiverAntennaType();
        if (antenna_type.empty()) {
            log_manager_.getLogger("antenna")->warn("Empty receiver antenna type returned from RINEX parser");
            return antenna_type;
        }
        std::string sanitized;
        sanitized.reserve(antenna_type.size());
        for (char c : antenna_type) {
            if (!std::isspace(static_cast<unsigned char>(c))) sanitized.push_back(c);
        }
        return sanitized;
    }

    /**
     * @brief Load antenna data (ATX) via ProductParser.
     *
     * Attempts to locate the ATX product path via ProductParser and parse
     * antenna entries into the internal `antennas_` vector. Errors and
     * exceptions are logged but do not throw.
     */
    void Antenna::loadAntennaData() {
        // Data accessed via reference to ProductCache - nothing to load
        log_manager_.getLogger("antenna_model")->debug(
            "Using cached antenna reference (zero-copy): {} entries",
            antennas_.size());
    }

    /**
     * @brief Interpolate a PCV grid.
     *
     * Supports both azimuth-independent tables (single column) and full 2D
     * grids. Linear interpolation is used between zenith rings and azimuth
     * sectors; azimuth wraps around circularly.
     *
     * @param pcv_table 2D PCV table: rows=zenith rings, cols=azimuth sectors (or 1 for azimuth-independent).
     * @param zenith Zenith angle in degrees (0..90+ as provided by caller).
     * @param azimuth Azimuth angle in degrees (0..360).
     * @param dazi Azimuth spacing in degrees between PCV columns.
     * @param dzen Zenith spacing in degrees between PCV rows.
     * @return Interpolated PCV value in same units as table (meters if table was in meters; caller scales).
     */
    double Antenna::interpolatePcv(const std::vector<std::vector<double>>& pcv_table,
                                   double zenith,
                                   double azimuth,
                                   double dazi,
                                   double dzen) const {
        if (pcv_table.empty() || pcv_table[0].empty()) return 0.0;
        size_t cols = pcv_table[0].size();
        bool az_indep = (cols == 1);

        size_t zen_idx = static_cast<size_t>(zenith / dzen);
        double zen_frac = (zenith - zen_idx * dzen) / dzen;
        if (zen_idx >= pcv_table.size()) {
            zen_idx = pcv_table.size() - 1;
            zen_frac = 0.0;
        }

        if (az_indep) {
            double pcv = pcv_table[zen_idx][0];
            if (zen_idx + 1 < pcv_table.size())
                pcv += zen_frac * (pcv_table[zen_idx + 1][0] - pcv_table[zen_idx][0]);
            return pcv;
        }

        size_t az_idx = static_cast<size_t>(azimuth / dazi);
        double az_frac = (azimuth - az_idx * dazi) / dazi;
        if (az_idx >= cols) {
            az_idx = cols - 1;
            az_frac = 0.0;
        }
        size_t az_next = (az_idx + 1) % cols;

        double pcv00 = pcv_table[zen_idx][az_idx];
        double pcv01 = pcv_table[zen_idx][az_next];
        double pcv10 = pcv_table[zen_idx][az_idx];
        double pcv11 = pcv_table[zen_idx][az_next];
        if (zen_idx + 1 < pcv_table.size()) {
            pcv10 = pcv_table[zen_idx + 1][az_idx];
            pcv11 = pcv_table[zen_idx + 1][az_next];
        }

        double pcv =
            pcv00 * (1 - zen_frac) * (1 - az_frac) +
            pcv10 * zen_frac * (1 - az_frac) +
            pcv01 * (1 - zen_frac) * az_frac +
            pcv11 * zen_frac * az_frac;
        return pcv;
    }

}