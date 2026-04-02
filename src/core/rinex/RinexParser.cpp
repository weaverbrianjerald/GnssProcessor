#include "RinexParser.hpp"
#include "ProductParser.hpp"
#include "TimeUtils.hpp"
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <algorithm>
#include <spdlog/spdlog.h>
#include "Constants.hpp"
#include <PppTypes.hpp>

namespace GnssProcessor {

    /**
     * @brief Construct a RinexParser.
     *
     * @param config Reference to the global GNSS processing configuration.
     * @param log_manager Reference to a LogManager used for logging parser events.
     * @param product_parser Reference to a ProductParser used to access auxiliary products.
     *
     * @details
     * - Initializes internal header/state members to sensible defaults.
     * - Emits an initialization log entry including the selected processing mode.
     * - Registers this RinexParser instance with @p product_parser via
     *   `product_parser.setRinexParser(*this)` so the ProductParser can query
     *   epoch times and other parsed information later.
     *
     * Note: the constructor does not parse any files. Call `parse()` to read the
     * configured RINEX observation file.
     */
    RinexParser::RinexParser(const GnssConfig& config, LogManager& log_manager, ProductParser& product_parser)
        : config_(config), log_manager_(log_manager), product_parser_(product_parser),
        header_(), observations_(), receiver_serial_no_(), time_of_first_obs_(), glonass_freq_slots_() {
        header_.version = 0.0;
        header_.time_system = "GPS";
        header_.interval = std::nullopt;
        header_.approx_position = { 0.0, 0.0, 0.0 };
        header_.antenna_delta_hen = { 0.0, 0.0, 0.0 };
        header_.antenna_azimuth_deg = 0.0;
        header_.antenna_elevation_deg = 0.0;
        log_manager_.getLogger("rinex_parser")->info("RinexParser initialized for mode: {}",
            config_.getProcessingMode() == ProcessingMode::PPP ? "PPP" : "PPP-RTK");
        // Register this RinexParser with the ProductParser so ProductParser can access epoch times later
        product_parser.setRinexParser(*this);
    }

    /**
     * @brief Parse the configured RINEX file (observation or navigation).
     *
     * This routine reads the file at `config_.getObservationPath()` and:
     * - Validates the file path and returns false (logs error) if missing or not present.
     * - Detects navigation RINEX files (filename contains "BRDC" or "_MN.rnx")
     *   and delegates parsing to `product_parser_.parseKlobuchar(...)`. Returns
     *   true on successful delegation.
     * - Parses RINEX header and observation epochs for both RINEX 3.x (">" epoch lines)
     *   and RINEX 2.x formats:
     *     - Populates `header_` fields (version, time system, obs types per system,
     *       approximate ECEF position, antenna offsets/azimuth/elevation, interval,
     *       marker/receiver/antenna metadata).
     *     - Fills `glonass_freq_slots_` from "GLONASS SLOT / FRQ #" header lines.
     *     - Builds `observations_` vector of `RinexObservation`, sets `time_of_first_obs_`.
     *     - Rescales GLONASS measurements via `rescaleGlonassMeasurement(...)` when slot info exists.
     *     - Assigns measurement variances using `config_.getDefaultMeasurementNoise()` and stores LLI flags.
     * - Logs progress, warnings and errors via `log_manager_`.
     * - Catches and logs exceptions; closes the file and returns false on failure.
     *
     * @return true if parsing succeeded (observations were parsed or nav delegation succeeded),
     *         false on any error or when no observations were produced.
     */
    bool RinexParser::parse() {
        std::filesystem::path rinex_path = config_.getObservationPath();
        if (rinex_path.empty() || !std::filesystem::exists(rinex_path)) {
            log_manager_.getLogger("rinex_parser")->error("Observation RINEX file path invalid or does not exist: {}", rinex_path.string());
            return false;
        }

        // Check if the file is a navigation file (e.g., BRDC)
        std::string filename = rinex_path.filename().string();
        if (filename.find("BRDC") != std::string::npos || filename.find("_MN.rnx") != std::string::npos) {
            log_manager_.getLogger("rinex_parser")->debug("Detected navigation RINEX file: {}, delegating to ProductParser", rinex_path.string());
            std::vector<double> klobuchar_coeffs;
            std::vector<SunEphemeris> sun_ephemeris;
            bool success = product_parser_.parseKlobuchar(rinex_path, klobuchar_coeffs);
            if (!success) {
                log_manager_.getLogger("rinex_parser")->error("Failed to parse navigation RINEX file: {}", rinex_path.string());
                return false;
            }
            log_manager_.getLogger("rinex_parser")->info("Parsed navigation RINEX file: {}, Klobuchar coeffs: {}, Sun ephemeris: {}",
                rinex_path.string(), klobuchar_coeffs.size(), sun_ephemeris.size());
            return true;
        }

        std::ifstream file(rinex_path);
        if (!file.is_open()) {
            log_manager_.getLogger("rinex_parser")->error("Failed to open observation RINEX file: {}", rinex_path.string());
            return false;
        }

        try {
            observations_.clear();
            header_ = RinexHeader(); // Reset header
            time_of_first_obs_ = GnssTime{ 0, 0.0 };
            glonass_freq_slots_.clear();
            bool is_rinex_3x = false;
            System current_system = System::Unknown;
            std::vector<std::string> current_obs_types;

            std::string line;
            bool in_header = true;
            bool first_epoch = true;

            while (std::getline(file, line)) {
                // Trim trailing whitespace
                line.erase(line.find_last_not_of(" \t") + 1);

                if (in_header) {
                    // Parse header
                    if (line.find("END OF HEADER") != std::string::npos) {
                        in_header = false;
                        is_rinex_3x = header_.version >= 3.0;
                        continue;
                    }

                    std::string label = line.size() >= 60 ? line.substr(60) : "";
                    if (label.find("RINEX VERSION / TYPE") != std::string::npos) {
                        header_.version = std::stod(line.substr(0, 9));
                    }
                    else if (label.find("MARKER NAME") != std::string::npos) {
                        header_.marker_name = line.substr(0, line.size() < 60 ? line.size() : 60);
                    }
                    else if (label.find("MARKER NUMBER") != std::string::npos) {
                        header_.marker_number = line.substr(0, 20);
                    }
                    else if (label.find("REC # / TYPE / VERS") != std::string::npos) {
                        header_.receiver_type = line.substr(20, 20);
                        receiver_serial_no_ = line.substr(0, 20);
                        header_.receiver_type.erase(header_.receiver_type.find_last_not_of(" \t") + 1);
                        receiver_serial_no_.erase(receiver_serial_no_.find_last_not_of(" \t") + 1);
                    }
                    else if (label.find("ANT # / TYPE") != std::string::npos) {
                        header_.antenna_type = line.substr(20, 20);
                        header_.antenna_type.erase(header_.antenna_type.find_last_not_of(" \t") + 1);
                        log_manager_.getLogger("rinex_parser")->info("Receiver antenna type: {}", header_.antenna_type);
                    }
                    else if (label.find("APPROX POSITION XYZ") != std::string::npos) {
                        double x = std::stod(line.substr(0, 14));
                        double y = std::stod(line.substr(14, 14));
                        double z = std::stod(line.substr(28, 14));
                        header_.approx_position = { x, y, z };
                    }
                    else if (label.find("ANTENNA: DELTA H/E/N") != std::string::npos) {
                        double h = std::stod(line.substr(0, 14));
                        double e = std::stod(line.substr(14, 14));
                        double n = std::stod(line.substr(28, 14));
                        header_.antenna_delta_hen = { h, e, n };
                    }
                    else if (label.find("ANTENNA: AZIMUTH") != std::string::npos) {
                        header_.antenna_azimuth_deg = std::stod(line.substr(0, 14));
                    }
                    else if (label.find("ANTENNA: ELEVATION") != std::string::npos) {
                        header_.antenna_elevation_deg = std::stod(line.substr(0, 14));
                    }
                    else if (label.find("SYS / # / OBS TYPES") != std::string::npos) {
                        std::string sys = line.substr(0, 1);
                        current_system = sys == "G" ? System::GPS :
                            sys == "E" ? System::Galileo :
                            sys == "R" ? System::GLONASS :
                            sys == "C" ? System::BeiDou :
                            sys == "J" ? System::QZSS :
                            sys == "S" ? System::SBAS : System::Unknown;
                        if (current_system == System::Unknown) {
                            log_manager_.getLogger("rinex_parser")->warn("Unknown system identifier '{}'", sys);
                            continue;
                        }
                        int num_types = 0;
                        try {
                            num_types = std::stoi(line.substr(3, 3));
                        }
                        catch (const std::exception& e) {
                            log_manager_.getLogger("rinex_parser")->warn("Invalid number of observation types: {}", e.what());
                            continue;
                        }
                        current_obs_types.clear();
                        std::istringstream iss(line.substr(6, 54)); // Read up to column 60
                        std::string type;
                        while (iss >> type) {
                            if (type.find_first_of(" \t\n") != std::string::npos) continue; // Skip whitespace
                            if (auto valid_type = validateObsCode(type, current_system)) {
                                current_obs_types.push_back(*valid_type);
                            }
                            else {
                                log_manager_.getLogger("rinex_parser")->warn("Including invalid observation code '{}' for system {}", type, sys);
                                current_obs_types.push_back(type);
                            }
                        }
                        // Handle multi-line observation types
                        while (current_obs_types.size() < static_cast<size_t>(num_types) && std::getline(file, line)) {
                            line.erase(line.find_last_not_of(" \t") + 1);
                            // Break if a new system is encountered (system identifier in first column)
                            if (!line.empty() && (line[0] == 'G' || line[0] == 'E' || line[0] == 'R' ||
                                line[0] == 'C' || line[0] == 'J' || line[0] == 'S')) {
                                file.seekg(-static_cast<std::streamoff>(line.length() + 1), std::ios::cur); // Rewind to process new system
                                break;
                            }
                            // Break if a non-continuation header line is encountered (e.g., GLONASS SLOT / FRQ #)
                            if (line.size() >= 60 && line.substr(60).find_first_not_of(" \t") != std::string::npos &&
                                line.substr(60).find("SYS / # / OBS TYPES") == std::string::npos) {
                                file.seekg(-static_cast<std::streamoff>(line.length() + 1), std::ios::cur); // Rewind for next header line
                                break;
                            }
                            // Process continuation line
                            iss.clear();
                            iss.str(line.substr(0, line.size() < 60 ? line.size() : 60));
                            while (iss >> type) {
                                if (type.find_first_of(" \t\n") != std::string::npos) continue; // Skip whitespace
                                if (auto valid_type = validateObsCode(type, current_system)) {
                                    current_obs_types.push_back(*valid_type);
                                }
                                else {
                                    log_manager_.getLogger("rinex_parser")->warn("Including invalid observation code '{}' for system {}", type, sys);
                                    current_obs_types.push_back(type);
                                }
                            }
                        }
                        if (current_obs_types.size() != static_cast<size_t>(num_types)) {
                            log_manager_.getLogger("rinex_parser")->warn("Expected {} observation types for system {}, got {}", num_types, sys, current_obs_types.size());
                        }
                        if (!current_obs_types.empty()) {
                            header_.obs_types[current_system] = current_obs_types;
                            std::string obs_types_str;
                            for (size_t i = 0; i < current_obs_types.size(); ++i) {
                                obs_types_str += current_obs_types[i];
                                if (i < current_obs_types.size() - 1) obs_types_str += ", ";
                            }
                            log_manager_.getLogger("rinex_parser")->info("Parsed {} observation types for system {}: {}", current_obs_types.size(), sys, obs_types_str);
                        }
                    }
                    else if (label.find("INTERVAL") != std::string::npos) {
                        header_.interval = std::stod(line.substr(0, 10));
                    }
                    else if (label.find("TIME OF FIRST OBS") != std::string::npos) {
                        // RINEX 3: time system is at columns 49-51 of this record
                        if (line.size() >= 51) {
                            std::string ts = line.substr(48, 3);
                            ts.erase(ts.find_last_not_of(" \t") + 1);
                            ts.erase(0, ts.find_first_not_of(" \t"));
                            if (!ts.empty()) {
                                header_.time_system = ts;
                            }
                        }
                        int year = std::stoi(line.substr(0, 6));
                        int month = std::stoi(line.substr(6, 6));
                        int day = std::stoi(line.substr(12, 6));
                        int hour = std::stoi(line.substr(18, 6));
                        int minute = std::stoi(line.substr(24, 6));
                        double second = std::stod(line.substr(30, 13));
                        time_of_first_obs_ = TimeUtils::GnssTimeFromDate(year, month, day, hour, minute, second, header_.time_system);
                    }
                    else if (label.find("GLONASS SLOT / FRQ #") != std::string::npos) {
                        std::istringstream iss(line.substr(0, 60));
                        int num_slots;
                        iss >> num_slots;
                        for (int i = 0; i < num_slots; ++i) {
                            if (iss.eof() && std::getline(file, line)) {
                                iss.clear();
                                iss.str(line.substr(0, 60));
                            }
                            std::string sat;
                            int freq;
                            iss >> sat >> freq;
                            if (sat.size() >= 3 && sat[0] == 'R') {
                                glonass_freq_slots_[sat] = freq;
                            }
                        }
                    }
                }
                else {
                    // Parse observation data
                    if (is_rinex_3x) {
                        if (line.length() < 26 || line[0] != '>') continue;
                        std::istringstream iss(line);
                        char marker;
                        int year, month, day, hour, min, flag;
                        double sec;
                        int num_sats;
                        iss >> marker >> year >> month >> day >> hour >> min >> sec >> flag >> num_sats;
                        if (!iss || marker != '>' || flag > 1) {
                            log_manager_.getLogger("rinex_parser")->debug("Skipping invalid epoch line: {}", line);
                            for (int i = 0; i < num_sats; ++i) std::getline(file, line);
                            continue;
                        }
                        GnssTime epoch_time = TimeUtils::GnssTimeFromDate(year, month, day, hour, min, sec, header_.time_system);
                        if (first_epoch) {
                            time_of_first_obs_ = epoch_time;
                            first_epoch = false;
                        }

                        for (int i = 0; i < num_sats && std::getline(file, line); ++i) {
                            if (line.length() < 3) {
                                log_manager_.getLogger("rinex_parser")->warn("Invalid satellite line: {}", line);
                                continue;
                            }
                            std::string sat_id = line.substr(0, 3);
                            System system = sat_id[0] == 'G' ? System::GPS :
                                sat_id[0] == 'E' ? System::Galileo :
                                sat_id[0] == 'R' ? System::GLONASS :
                                sat_id[0] == 'C' ? System::BeiDou :
                                sat_id[0] == 'J' ? System::QZSS :
                                sat_id[0] == 'S' ? System::SBAS : System::Unknown;
                            if (system == System::Unknown) {
                                log_manager_.getLogger("rinex_parser")->warn("Unknown system for satellite: {}", sat_id);
                                continue;
                            }

                            RinexObservation obs;
                            obs.satellite = sat_id;
                            obs.system = system;
                            obs.time = epoch_time;
                            obs.receiver_id = header_.marker_name;

                            auto obs_types = header_.obs_types[system];
                            std::istringstream data_iss(line.substr(3));
                            for (size_t j = 0; j < obs_types.size(); ++j) {
                                // Each observation field is 16 characters (14 for value, 1 for LLI, 1 for SNR)
                                size_t start_pos = 3 + j * 16;
                                if (start_pos + 16 > line.length()) {
                                    continue; // Incomplete observation data
                                }
                                std::string field = line.substr(start_pos, 16);
                                std::string value_str = field.substr(0, 14);
                                std::string lli_str = field.substr(14, 1);
                                std::string snr_str = field.substr(15, 1);

                                // Trim whitespace from value_str
                                value_str.erase(0, value_str.find_first_not_of(" \t"));
                                value_str.erase(value_str.find_last_not_of(" \t") + 1);
                                if (value_str.empty()) {
                                    continue; // Empty observation
                                }
                                try {
                                    double value = std::stod(value_str);
                                    /*
                                    if (system == System::GLONASS && glonass_freq_slots_.count(sat_id)) {
                                        value = rescaleGlonassMeasurement(value, obs_types[j], sat_id);
                                    }
                                    */
                                    obs.measurements[obs_types[j]] = value;

                                    const auto& n = config_.getDefaultMeasurementNoise();
                                    const double default_code_var = n.code_std_m * n.code_std_m;
                                    const double default_phase_var = n.phase_std_m * n.phase_std_m;

                                    double variance = (obs_types[j][0] == 'C') ? default_code_var : default_phase_var;

                                    obs.variances[obs_types[j]] = variance;
                                    if (!lli_str.empty() && lli_str.find_first_of("0123456789") != std::string::npos) {
                                        obs.lli_flags[obs_types[j]] = std::stoi(lli_str);
                                    }
                                    else {
                                        obs.lli_flags[obs_types[j]] = 0;
                                    }
                                }
                                catch (const std::exception& e) {
                                    log_manager_.getLogger("rinex_parser")->debug("Invalid observation value for {} {}: {}", sat_id, obs_types[j], e.what());
                                }
                            }
                            if (!obs.measurements.empty()) {
                                observations_.push_back(obs);
                            }
                        }
                    }
                    else {
                        // RINEX 2.x parsing
                        if (line.length() < 26) continue;
                        std::istringstream iss(line.substr(0, 26));
                        int year, month, day, hour, min, flag;
                        double sec;
                        iss >> year >> month >> day >> hour >> min >> sec >> flag;
                        if (!iss || flag > 1) {
                            log_manager_.getLogger("rinex_parser")->debug("Skipping invalid epoch line: {}", line);
                            continue;
                        }
                        year = year < 80 ? 2000 + year : 1900 + year; // RINEX 2.x 2-digit year
                        GnssTime epoch_time = TimeUtils::GnssTimeFromDate(year, month, day, hour, min, sec, header_.time_system);
                        if (first_epoch) {
                            time_of_first_obs_ = epoch_time;
                            first_epoch = false;
                        }

                        std::string sat_list = line.substr(29);
                        std::istringstream sat_iss(sat_list);
                        int num_sats;
                        sat_iss >> num_sats;
                        std::vector<std::string> sat_ids;
                        for (int i = 0; i < num_sats; ++i) {
                            if ((i % 12 == 0 && i > 0) || sat_iss.eof()) {
                                if (!std::getline(file, line)) break;
                                sat_iss.clear();
                                sat_iss.str(line);
                            }
                            std::string sat_id;
                            sat_iss >> sat_id;
                            if (sat_id.length() == 2) sat_id = "G" + sat_id; // RINEX 2.x omits system for GPS
                            else if (sat_id[0] == 'R' && sat_id.length() == 2) sat_id = "R0" + sat_id[1];
                            sat_ids.push_back(sat_id);
                        }

                        for (const auto& sat_id : sat_ids) {
                            if (!std::getline(file, line)) {
                                log_manager_.getLogger("rinex_parser")->warn("Unexpected end of file while reading satellite data for {}", sat_id);
                                break;
                            }
                            System system = sat_id[0] == 'G' ? System::GPS :
                                sat_id[0] == 'R' ? System::GLONASS :
                                sat_id[0] == 'E' ? System::Galileo :
                                sat_id[0] == 'C' ? System::BeiDou :
                                sat_id[0] == 'J' ? System::QZSS :
                                sat_id[0] == 'S' ? System::SBAS : System::Unknown;
                            if (system == System::Unknown) {
                                log_manager_.getLogger("rinex_parser")->warn("Unknown system for satellite: {}", sat_id);
                                continue;
                            }

                            RinexObservation obs;
                            obs.satellite = sat_id;
                            obs.system = system;
                            obs.time = epoch_time;
                            obs.receiver_id = header_.marker_name;

                            auto obs_types = header_.obs_types[system];
                            std::istringstream data_iss(line);
                            int types_per_line = 5; // RINEX 2.x: 5 obs per line
                            for (size_t j = 0; j < obs_types.size(); ++j) {
                                if (j > 0 && j % types_per_line == 0) {
                                    if (!std::getline(file, line)) break;
                                    data_iss.clear();
                                    data_iss.str(line);
                                }
                                std::string value_str, lli_str, snr_str;
                                data_iss >> std::setw(14) >> value_str >> std::setw(1) >> lli_str >> std::setw(1) >> snr_str;
                                if (value_str.empty() || value_str.find_first_not_of(" \t") == std::string::npos) {
                                    continue;
                                }
                                try {
                                    double value = std::stod(value_str);
                                    /*
                                    if (system == System::GLONASS && glonass_freq_slots_.count(sat_id)) {
                                        value = rescaleGlonassMeasurement(value, obs_types[j], sat_id);
                                    }
                                    */
                                    obs.measurements[obs_types[j]] = value;

                                    const auto& n = config_.getDefaultMeasurementNoise();
                                    const double default_code_var = n.code_std_m * n.code_std_m;
                                    const double default_phase_var = n.phase_std_m * n.phase_std_m;

                                    double variance = (obs_types[j][0] == 'C') ? default_code_var : default_phase_var;

                                    obs.variances[obs_types[j]] = variance;
                                    if (!lli_str.empty() && lli_str.find_first_of("0123456789") != std::string::npos) {
                                        obs.lli_flags[obs_types[j]] = std::stoi(lli_str);
                                    }
                                    else {
                                        obs.lli_flags[obs_types[j]] = 0;
                                    }
                                }
                                catch (const std::exception& e) {
                                    log_manager_.getLogger("rinex_parser")->debug("Invalid observation value for {} {}: {}", sat_id, obs_types[j], e.what());
                                }
                            }
                            if (!obs.measurements.empty()) {
                                observations_.push_back(obs);
                            }
                        }
                    }
                }
            }
            file.close();
            log_manager_.getLogger("rinex_parser")->info("Parsed RINEX file: {}, {} observations, version: {}, time_system: {}, "
                "marker_name: {}, marker_number: {}, antenna_type: {}, antenna_hen: ({:.3f}, {:.3f}, {:.3f}), "
                "azimuth: {:.3f} deg, elevation: {:.3f} deg, mode: {}",
                rinex_path.string(), observations_.size(), header_.version, header_.time_system,
                header_.marker_name, header_.marker_number, header_.antenna_type,
                header_.antenna_delta_hen[0], header_.antenna_delta_hen[1], header_.antenna_delta_hen[2],
                header_.antenna_azimuth_deg, header_.antenna_elevation_deg,
                config_.getProcessingMode() == ProcessingMode::PPP ? "PPP" : "PPP-RTK");
            return !observations_.empty();
        }
        catch (const std::exception& e) {
            log_manager_.getLogger("rinex_parser")->error("Error parsing RINEX file {}: {}", product_parser_.getProductPath("RNX").string(), e.what());
            file.close();
            return false;
        }
    }

    /**
     * @brief Validate measurement rate and epoch intervals parsed from the RINEX file.
     *
     * Checks the header-declared measurement interval (if present) and validates
     * consecutive epoch intervals computed from `observations_`.
     *
     * Behavior:
     * - Returns true immediately when `observations_` is empty.
     * - If `header_.interval` is present, verifies it lies in [0.01, 300.0] seconds;
     *   logs an error if outside that range and a warning when it differs from the
     *   expected 30.0 s test value.
     * - Iterates consecutive observations (skips identical epoch times), computes
     *   delta-t using the difference of TOW values, and accounts for GPS week
     *   rollover by adding 604800.0 s when dt < 0.0.
     * - Ensures each epoch interval dt is within [0.01, 300.0] s; logs errors on
     *   invalid intervals and warnings when dt deviates from 30.0 s.
     *
     * @note Uses spdlog for logging messages.
     *
     * @return true if the header interval (if present) and all epoch intervals are
     *         within allowed ranges; false if any invalid interval is detected.
     */
    bool RinexParser::validateMeasurementRate() const {
        if (observations_.empty()) return true;

        // Check header interval (expect ~30s for test file)
        if (header_.interval.has_value()) {
            double interval = header_.interval.value();
            if (interval < 0.01 || interval > 300.0) {
                spdlog::error("Header interval {}s is outside allowed range [0.01s, 300s]", interval);
                return false;
            }
            if (std::abs(interval - 30.0) > 1e-3) {
                spdlog::warn("Header interval {}s differs from expected 30s for test file", interval);
            }
        }

        // Validate epoch intervals
        for (size_t i = 1; i < observations_.size(); ++i) {
            if (observations_[i].time == observations_[i - 1].time) continue;
            double dt = observations_[i].time.tow - observations_[i - 1].time.tow;
            if (dt < 0.0) {
                dt += TimeUtils::SECONDS_IN_WEEK; // Handle week rollover
            }
            if (dt < 0.01 || dt > 300.0) {
                spdlog::error("Epoch interval {}s is outside allowed range [0.01s, 300s]", dt);
                return false;
            }
            if (std::abs(dt - 30.0) > 1e-3) {
                spdlog::warn("Epoch interval {}s differs from expected 30s", dt);
            }
        }
        return true;
    }

    /**
     * @brief Validate a RINEX 3.x observation code for a specific GNSS system.
     *
     * Validates the syntactic format of a RINEX 3.x observation code and verifies
     * it is allowed for the provided GNSS system. The expected syntactic pattern
     * is one of the observation-type letters {C,L,D,S}, followed by a digit and
     * an uppercase letter (e.g. "C1C", "L2P").
     *
     * Behavior:
     * - Performs a lightweight syntactic check: length == 3, first char in {C,L,D,S},
     *   second char is a digit, third char is an uppercase letter.
     * - Looks up the provided GNSS system in an internal table of valid codes and
     *   confirms membership. If the system has no table or the code is not listed,
     *   the code is considered invalid.
     * - Logs diagnostic messages at debug level for invalid formats, unknown systems,
     *   or unsupported codes.
     *
     * @param[in] code   Observation code string to validate (e.g., "C1C").
     * @param[in] system GNSS system enum indicating which system's valid set to check.
     * @return std::optional<std::string> containing the (normalized) observation code
     *         on success, or std::nullopt if the code is syntactically invalid or not
     *         permitted for the given system.
     */
    std::optional<std::string> RinexParser::validateObsCode(const std::string& code, System system) const {
        // RINEX 3.x observation code format: [CLDS][1-9][A-Z]
        if (code.length() != 3 || (code[0] != 'C' && code[0] != 'L' && code[0] != 'D' && code[0] != 'S') ||
            !std::isdigit(code[1]) || !std::isupper(code[2])) {
            log_manager_.getLogger("rinex_parser")->debug("Invalid observation code format: {} for system {}", code,
                system == System::GPS ? "GPS" : system == System::Galileo ? "Galileo" :
                system == System::GLONASS ? "GLONASS" : system == System::BeiDou ? "BeiDou" :
                system == System::QZSS ? "QZSS" : system == System::SBAS ? "SBAS" : "Unknown");
            return std::nullopt;
        }

        static const std::map<System, std::vector<std::string>> valid_codes = {
            {System::GPS, {
                // L1: C/A, L1C, P(Y), Z-tracking, M-code
                "C1C", "C1S", "C1L", "C1X", "C1P", "C1W", "C1Y", "C1M",
                "L1C", "L1S", "L1L", "L1X", "L1P", "L1W", "L1Y", "L1M",
                "D1C", "D1S", "D1L", "D1X", "D1P", "D1W", "D1Y", "D1M",
                "S1C", "S1S", "S1L", "S1X", "S1P", "S1W", "S1Y", "S1M",
                // L2: C/A, L2C, P(Y), M-code
                "C2C", "C2S", "C2L", "C2X", "C2P", "C2W", "C2Y", "C2M",
                "L2C", "L2S", "L2L", "L2X", "L2P", "L2W", "L2Y", "L2M",
                "D2C", "D2S", "D2L", "D2X", "D2P", "D2W", "D2Y", "D2M",
                "S2C", "S2S", "S2L", "S2X", "S2P", "S2W", "S2Y", "S2M",
                // L5: I, Q, I+Q
                "C5I", "C5Q", "C5X", "L5I", "L5Q", "L5X", "D5I", "D5Q", "D5X", "S5I", "S5Q", "S5X"
            }},
            {System::Galileo, {
                // E1: B, C, B+C
                "C1B", "C1C", "C1X", "L1B", "L1C", "L1X", "D1B", "D1C", "D1X", "S1B", "S1C", "S1X",
                // E5a: I, Q, I+Q
                "C5I", "C5Q", "C5X", "L5I", "L5Q", "L5X", "D5I", "D5Q", "D5X", "S5I", "S5Q", "S5X",
                // E5b: I, Q, I+Q
                "C7I", "C7Q", "C7X", "L7I", "L7Q", "L7X", "D7I", "D7Q", "D7X", "S7I", "S7Q", "S7X",
                // E6: B, C, B+C
                "C6B", "C6C", "C6X", "L6B", "L6C", "L6X", "D6B", "D6C", "D6X", "S6B", "S6C", "S6X",
                // E5a+b: I, Q, I+Q
                "C8I", "C8Q", "C8X", "L8I", "L8Q", "L8X", "D8I", "D8Q", "D8X", "S8I", "S8Q", "S8X"
            }},
            {System::GLONASS, {
                // G1: C/A, P
                "C1C", "C1P", "L1C", "L1P", "D1C", "D1P", "S1C", "S1P",
                // G2: C/A, P
                "C2C", "C2P", "L2C", "L2P", "D2C", "D2P", "S2C", "S2P",
                // G3: I, Q, I+Q
                "C3I", "C3Q", "C3X", "L3I", "L3Q", "L3X", "D3I", "D3Q", "D3X", "S3I", "S3Q", "S3X"
            }},
            {System::BeiDou, {
                // B1I: I, Q, I+Q
                "C2I", "C2Q", "C2X", "L2I", "L2Q", "L2X", "D2I", "D2Q", "D2X", "S2I", "S2Q", "S2X",
                // B3I: I, Q, I+Q
                "C6I", "C6Q", "C6X", "L6I", "L6Q", "L6X", "D6I", "D6Q", "D6X", "S6I", "S6Q", "S6X",
                // B1C: D, P, D+P
                "C1D", "C1P", "C1X", "L1D", "L1P", "L1X", "D1D", "D1P", "D1X", "S1D", "S1P", "S1X",
                // B2a: D, Z, D+Z
                "C5D", "C5Z", "C5X", "L5D", "L5Z", "L5X", "D5D", "D5Z", "D5X", "S5D", "S5Z", "S5X",
                // B2b: I, Q, I+Q
                "C7I", "C7Q", "C7X", "C7D", "C7Z", "L7I", "L7Q", "L7X", "L7D", "L7Z", "D7I", "D7Q", "D7X", "D7D", "D7Z", "S7I", "S7Q", "S7X", "S7D", "S7Z",
                // B2a+b: I, Q, I+Q
                "C8I", "C8Q", "C8X", "L8I", "L8Q", "L8X", "D8I", "D8Q", "D8X", "S8I", "S8Q", "S8X"
            }},
            {System::QZSS, {
                // L1: C/A, SAIF, L1C
                "C1C", "C1S", "C1L", "C1X", "L1C", "L1S", "L1L", "L1X", "D1C", "D1S", "D1L", "D1X",
                "S1C", "S1S", "S1L", "S1X",
                // L2: L2C
                "C2S", "C2L", "C2X", "L2S", "L2L", "L2X", "D2S", "D2L", "D2X", "S2S", "S2L", "S2X",
                // L5: I, Q, I+Q
                "C5I", "C5Q", "C5X", "L5I", "L5Q", "L5X", "D5I", "D5Q", "D5X", "S5I", "S5Q", "S5X",
                // L6: D, E
                "C6D", "C6E", "C6X", "L6D", "L6E", "L6X", "D6D", "D6E", "D6X", "S6D", "S6E", "S6X"
            }},
            {System::SBAS, {
                // L1: C/A
                "C1C", "C1X", "L1C", "L1X", "D1C", "D1X", "S1C", "S1X",
                // L5: I, Q, I+Q
                "C5I", "C5Q", "C5X", "L5I", "L5Q", "L5X", "D5I", "D5Q", "D5X", "S5I", "S5Q", "S5X"
            }}
        };

        auto it = valid_codes.find(system);
        if (it == valid_codes.end()) {
            log_manager_.getLogger("rinex_parser")->debug("No valid codes defined for system {}", system == System::GPS ? "GPS" :
                system == System::Galileo ? "Galileo" : system == System::GLONASS ? "GLONASS" : system == System::BeiDou ? "BeiDou" :
                system == System::QZSS ? "QZSS" : system == System::SBAS ? "SBAS" : "Unknown");
            return std::nullopt;
        }
        if (std::find(it->second.begin(), it->second.end(), code) != it->second.end()) {
            return std::optional<std::string>{code};
        }
        log_manager_.getLogger("rinex_parser")->debug("Observation code {} not valid for system {}", code, system == System::GPS ? "GPS" :
            system == System::Galileo ? "Galileo" : system == System::GLONASS ? "GLONASS" : system == System::BeiDou ? "BeiDou" :
            system == System::QZSS ? "QZSS" : system == System::SBAS ? "SBAS" : "Unknown");
        return std::nullopt;
    }

    /**
     * @brief Return epoch times extracted from parsed observations.
     *
     * Extracts the `time` field from each stored `RinexObservation` and returns
     * them in the same order as `observations_`. Note that the vector may contain
     * duplicate times because `observations_` stores one entry per satellite per
     * epoch (i.e., multiple observations sharing the same epoch produce repeated
     * times).
     *
     * This method does not modify internal state.
     *
     * @return std::vector<GnssTime> Vector of epoch times (GPS week + TOW) in the
     *         same iteration order as `observations_`.
     *
     * @note For a unique, sorted list of epoch times call `getEpochTimes()` and
     *       post-process the result (e.g., sort + erase unique).
     */
    std::vector<GnssTime> RinexParser::getEpochTimes() const {
        std::vector<GnssTime> epoch_times;
        for (const auto& obs : observations_) {
            epoch_times.push_back(obs.time);
        }
        return epoch_times;
    }

    } // namespace GnssProcessor
    //#endif