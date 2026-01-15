#include "Antenna.hpp"
#include <Eigen/Dense>

namespace GnssProcessor {

    Antenna::Antenna(const GnssConfig& config, ProductParser& product_parser, LogManager& log_mgr, const RinexParser& rinex_parser)
        : log_manager_(log_mgr),
        config_(config),
        gnss_corrections_(log_mgr),
        antennas_(),
        rinex_parser_(rinex_parser),
        product_parser_(product_parser),
        ppp_utils_(log_mgr) {
        loadAntennaData();
        log_manager_.getLogger("antenna_model")->info("Antenna init: {} types, rx={}",
            antennas_.size(), getReceiverAntennaType());
    }

    double Antenna::getAntennaCorrection(const std::string& satellite,
        const std::string& freq_type,
        System system,
        const Eigen::Vector3d& rx_pos,
        const Eigen::Vector3d& sat_pos,
        double elevation,
        double azimuth,
        const std::string& rx_antenna_type,
        const GnssTime& time) const {

        double correction = 0.0;
        bool is_phase = freq_type[0] == 'L';
        double wavelength = is_phase ? ppp_utils_.getWavelength(system, freq_type) : 0.0;

        if (!rx_pos.allFinite() || !sat_pos.allFinite() || !std::isfinite(elevation) || !std::isfinite(azimuth)) {
            log_manager_.getLogger("antenna")->warn("Invalid inputs for antenna correction: satellite={}, freq_type={}, "
                "rx_pos=({}, {}, {}), sat_pos=({}, {}, {}), elevation={:.3f}, azimuth={:.3f}",
                satellite, freq_type, rx_pos.x(), rx_pos.y(), rx_pos.z(),
                sat_pos.x(), sat_pos.y(), sat_pos.z(), elevation, azimuth);
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

        Eigen::Vector3d los = (sat_pos - rx_pos);
        double los_norm = los.norm();
        if (los_norm == 0.0) {
            log_manager_.getLogger("antenna")->warn("Invalid LOS for satellite {}, freq_type={} at TOW {:.3f}", satellite, freq_type, time.tow);
            return 0.0;
        }
        los /= los_norm;

        // Satellite antenna selection with validity interval
        // Note: valid_until == GnssTime{0,0.0} is treated as "open-ended"
        const AntennaData* sat_entry = nullptr;
        for (const auto& a : antennas_) {
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
                    break;
                }
            }
            else if (!sat_entry) {
                sat_entry = &a; // fallback if no validity info
            }
        }

        double sat_pco_corr = 0.0, sat_pcv_corr = 0.0;
        if (sat_entry && sat_entry->pco.count(atx_freq)) {
            Eigen::Vector3d pco = sat_entry->pco.at(atx_freq) * 1e-3;
            if (pco.allFinite()) {
                sat_pco_corr = pco.dot(los);
                correction += sat_pco_corr;
            } else {
                log_manager_.getLogger("antenna")->warn("Invalid satellite PCO for {} {} atx_freq={} at TOW {:.3f}",
                    satellite, sat_entry->type, atx_freq, time.tow);
            }

            if (sat_entry->pcv.count(atx_freq) && !sat_entry->pcv.at(atx_freq).empty()) {
                const auto& pcv_table = sat_entry->pcv.at(atx_freq);
                double zenith = 90.0 - elevation;
                double dazi = sat_entry->dazi > 0.0 ? sat_entry->dazi : 5.0;
                double dzen = sat_entry->dzen > 0.0 ? sat_entry->dzen : 5.0;
                double pcv = interpolatePcv(pcv_table, zenith, azimuth, dazi, dzen) * 1e-3;
                if (std::isfinite(pcv)) {
                    sat_pcv_corr = pcv;
                    correction += sat_pcv_corr;
                } else {
                    log_manager_.getLogger("antenna")->warn("Invalid satellite PCV for {} {} atx_freq={} at TOW {:.3f}",
                        satellite, sat_entry->type, atx_freq, time.tow);
                }
            }

            if (sat_entry->valid_from.week > 0 && sat_entry->valid_until.week > 0) {
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
        // Trim a trailing underscore if the input ended with whitespace
        if (!rx_type_underscore.empty() && rx_type_underscore.back() == '_')
            rx_type_underscore.pop_back();

        // Prefer exact matches; then try underscore-normalized; finally fall back to substring if unique
        const AntennaData* rx_entry = nullptr;

        for (const auto& a : antennas_) {
            if (a.type == rx_type_sanitized || a.type == rx_type_underscore) {
                rx_entry = &a;
                break;
            }
        }

        if (!rx_entry) {
            // Collect potential substring matches (try both keys)
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
                rx_pco_corr = pco.dot(los);
                correction -= rx_pco_corr;
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
                    correction -= rx_pcv_corr;
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
                correction += wind_up_corr;
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

    void Antenna::loadAntennaData() {
        try {
            std::filesystem::path atx_path = product_parser_.getProductPath("ATX");
            if (atx_path.empty()) {
                log_manager_.getLogger("antenna_model")->warn("No ATX product path provided, skipping antenna load");
                return;
            }
            std::vector<AntennaData> parsed_antennas;
            if (!product_parser_.parseAtx(atx_path, parsed_antennas)) {
                log_manager_.getLogger("antenna_model")->warn("Failed to parse ATX file: {}", atx_path.string());
                return;
            }
            antennas_ = std::move(parsed_antennas);
            log_manager_.getLogger("antenna_model")->info("Loaded {} antenna entries from {}", antennas_.size(), atx_path.string());
        }
        catch (const std::exception& e) {
            log_manager_.getLogger("antenna_model")->error("Exception loading antenna data: {}", e.what());
        }
        catch (...) {
            log_manager_.getLogger("antenna_model")->error("Unknown exception loading antenna data");
        }
    }

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