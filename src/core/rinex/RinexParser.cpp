#include "RinexParser.hpp"
#include "TimeUtils.hpp"
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <algorithm>
#include <spdlog/spdlog.h>

namespace GnssProcessor {

    /*
    bool GnssTime::operator==(const GnssTime& other) const {
        return week == other.week && std::abs(tow - other.tow) < 1e-6;
    }

    bool GnssTime::operator<(const GnssTime& other) const {
        return week < other.week || (week == other.week && tow < other.tow);
    }
    */

    RinexParser::RinexParser(const GnssConfig& config)
        : config_(config),
        log_manager_(),
        header_(),
        observations_(),
        receiver_antenna_type_("GENERIC"),
        antenna_delta_hen_(Eigen::Vector3d::Zero()),
        receiver_type_("UNKNOWN"),
        receiver_serial_no_("UNKNOWN"),
        marker_name_(""),
        time_of_first_obs_(),
        glonass_freq_slots_() {
        log_manager_.initialize(config.getRinexFilePath().parent_path().parent_path() / "logs", LogLevel::Debug);
        log_manager_.getLogger()->info("RinexParser initialized for file: {}", config_.getRinexFilePath().string());
    }

    bool RinexParser::parse() {
        try {
            std::ifstream file(config_.getRinexFilePath());
            if (!file.is_open()) {
                log_manager_.getLogger()->error("Failed to open RINEX file: {}", config_.getRinexFilePath().string());
                return false;
            }

            observations_.clear();
            std::string line;
            bool in_header = true;
            std::vector<std::string> obs_types;
            std::map<std::string, int> sys_obs_counts;
            marker_name_ = "UNKNOWN";
            Eigen::Vector3d approx_position_xyz_ = Eigen::Vector3d::Zero();
            time_of_first_obs_ = GnssTime{ 0, 0.0 };
            glonass_freq_slots_.clear();
            bool is_rinex_3x = false;
            std::string rinex_version;

            while (std::getline(file, line) && in_header) {
                if (line.length() < 60) continue;
                if (line.find("END OF HEADER") != std::string::npos) {
                    in_header = false;
                    continue;
                }
                if (line.find("RINEX VERSION / TYPE") != std::string::npos) {
                    rinex_version = line.substr(0, 9);
                    is_rinex_3x = rinex_version.find("3.") != std::string::npos;
                    if (!is_rinex_3x && rinex_version.find("2.") == std::string::npos) {
                        log_manager_.getLogger()->error("Unsupported RINEX version: {}", rinex_version);
                        file.close();
                        return false;
                    }
                    log_manager_.getLogger()->debug("RINEX version: {}", rinex_version);
                }
                else if (line.find(is_rinex_3x ? "SYS / # / OBS TYPES" : "# / TYPES OF OBSERV") != std::string::npos) {
                    if (is_rinex_3x) {
                        std::string system = line.substr(0, 1);
                        int num_types;
                        try {
                            num_types = std::stoi(line.substr(3, 3));
                        }
                        catch (const std::exception& e) {
                            log_manager_.getLogger()->warn("Invalid number of obs types for system {}: {}", system, line);
                            continue;
                        }
                        sys_obs_counts[system] = num_types;
                        std::string types = line.substr(6, 54);
                        std::istringstream iss(types);
                        std::string type;
                        while (iss >> type) {
                            obs_types.push_back(system + type);
                        }
                        while (std::getline(file, line) && line.length() >= 60 && line.substr(0, 1) == " " && line.find("SYS / # / OBS TYPES") != std::string::npos) {
                            types = line.substr(6, 54);
                            iss.clear();
                            iss.str(types);
                            while (iss >> type) {
                                obs_types.push_back(system + type);
                            }
                        }
                        in_header = true;
                    }
                    else {
                        int num_types;
                        try {
                            num_types = std::stoi(line.substr(0, 6));
                        }
                        catch (const std::exception& e) {
                            log_manager_.getLogger()->warn("Invalid number of obs types: {}", line);
                            continue;
                        }
                        sys_obs_counts["G"] = num_types; // RINEX 2.x assumes GPS unless specified
                        std::string types = line.substr(10, 50);
                        std::istringstream iss(types);
                        std::string type;
                        while (iss >> type) {
                            obs_types.push_back("G" + type);
                        }
                        while (std::getline(file, line) && line.length() >= 60 && line.substr(0, 10) == "          " && line.find("# / TYPES OF OBSERV") != std::string::npos) {
                            types = line.substr(10, 50);
                            iss.clear();
                            iss.str(types);
                            while (iss >> type) {
                                obs_types.push_back("G" + type);
                            }
                        }
                        in_header = true;
                    }
                }
                else if (line.find("ANT # / TYPE") != std::string::npos) {
                    receiver_antenna_type_ = line.substr(20, 20);
                    receiver_antenna_type_ = receiver_antenna_type_.substr(0, receiver_antenna_type_.find_last_not_of(" ") + 1);
                    if (receiver_antenna_type_.empty()) {
                        receiver_antenna_type_ = "GENERIC";
                        log_manager_.getLogger()->warn("No receiver antenna type found in RINEX header, using default: GENERIC");
                    }
                    else {
                        log_manager_.getLogger()->info("Receiver antenna type: {}", receiver_antenna_type_);
                    }
                }
                else if (line.find("ANTENNA: DELTA H/E/N") != std::string::npos) {
                    std::istringstream iss(line.substr(0, 42));
                    double h, e, n;
                    iss >> h >> e >> n;
                    if (!iss.fail()) {
                        antenna_delta_hen_ = Eigen::Vector3d(h, e, n);
                        log_manager_.getLogger()->info("Antenna H/E/N offsets: ({}, {}, {}) m", h, e, n);
                    }
                    else {
                        log_manager_.getLogger()->warn("Invalid ANTENNA: DELTA H/E/N: {}", line);
                    }
                }
                else if (line.find("REC # / TYPE / VERS") != std::string::npos) {
                    receiver_type_ = line.substr(20, 20);
                    receiver_type_ = receiver_type_.substr(0, receiver_type_.find_last_not_of(" ") + 1);
                    receiver_serial_no_ = line.substr(40, 20);
                    receiver_serial_no_ = receiver_serial_no_.substr(0, receiver_serial_no_.find_last_not_of(" ") + 1);
                    if (receiver_type_.empty()) {
                        receiver_type_ = "UNKNOWN";
                        log_manager_.getLogger()->warn("No receiver type found in RINEX header, using default: UNKNOWN");
                    }
                    else {
                        log_manager_.getLogger()->info("Receiver type: {}, serial: {}", receiver_type_, receiver_serial_no_);
                    }
                }
                else if (line.find("APPROX POSITION XYZ") != std::string::npos) {
                    std::istringstream iss(line.substr(0, 42));
                    double x, y, z;
                    iss >> x >> y >> z;
                    if (!iss.fail()) {
                        approx_position_xyz_ = Eigen::Vector3d(x, y, z);
                        log_manager_.getLogger()->info("Approximate position XYZ: ({}, {}, {}) m", x, y, z);
                    }
                    else {
                        log_manager_.getLogger()->warn("Invalid APPROX POSITION XYZ: {}", line);
                    }
                }
                else if (line.find("MARKER NAME") != std::string::npos) {
                    marker_name_ = line.substr(0, 60);
                    marker_name_ = marker_name_.substr(0, marker_name_.find_last_not_of(" ") + 1);
                    if (marker_name_.empty()) {
                        marker_name_ = "UNKNOWN";
                        log_manager_.getLogger()->warn("No marker name found in RINEX header, using default: UNKNOWN");
                    }
                    else {
                        log_manager_.getLogger()->info("Marker name: {}", marker_name_);
                    }
                }
                else if (line.find("TIME OF FIRST OBS") != std::string::npos) {
                    std::istringstream iss(line.substr(0, 43));
                    int year, month, day, hour, min;
                    double sec;
                    //int num_sats;
                    iss >> year >> month >> day >> hour >> min >> sec;
                    if (!iss.fail()) {
                        GnssTime time = TimeUtils::gregorianToGpsTime(year, month, day, hour, min, sec);
                        //time_of_first_obs_ = TimeUtils::gregorianToGpsTime(year, month, day, hour, min, sec);
                        if (!time.week) { // Set first observation time
                            time_of_first_obs_ = time;
                            log_manager_.getLogger()->info("Time of first observation: TOW {}", time_of_first_obs_.tow);
                        }
                    }
                    else {
                        log_manager_.getLogger()->warn("Invalid TIME OF FIRST OBS: {}", line);
                    }
                }
                else if (line.find(is_rinex_3x ? "GLONASS SLOT / FRQ #" : "GLONASS COD/PHS/BIS") != std::string::npos) {
                    if (is_rinex_3x) {
                        std::istringstream iss(line.substr(0, 60));
                        int num_slots;
                        iss >> num_slots;
                        for (int i = 0; i < num_slots && iss; ++i) {
                            std::string sat;
                            int freq;
                            iss >> sat >> freq;
                            if (!sat.empty() && !iss.fail()) {
                                glonass_freq_slots_[sat] = freq;
                                log_manager_.getLogger()->debug("GLONASS slot {}: frequency channel {}", sat, freq);
                            }
                        }
                    }
                    else {
                        // RINEX 2.x GLONASS frequency slots (simplified, often in BRDC)
                        log_manager_.getLogger()->debug("GLONASS COD/PHS/BIS found, frequency slots to be sourced from BRDC");
                        // Placeholder: Assume default channels or load from BRDC later
                    }
                }
            }

            // Parse observation data
            if (is_rinex_3x) {
                while (std::getline(file, line)) {
                    if (line.length() < 26 || line[0] != '>') continue;
                    std::istringstream iss(line);
                    char marker;
                    int year, month, day, hour, min, flag;
                    double sec;
                    int num_sats;
                    iss >> marker >> year >> month >> day >> hour >> min >> sec >> flag >> num_sats;
                    if (!iss || marker != '>' || flag > 1) {
                        log_manager_.getLogger()->debug("Skipping invalid epoch line: {}", line);
                        for (int i = 0; i < num_sats; ++i) std::getline(file, line);
                        continue;
                    }

                    GnssTime epoch_time = TimeUtils::gregorianToGpsTime(year, month, day, hour, min, sec);

                    for (int i = 0; i < num_sats && std::getline(file, line); ++i) {
                        if (line.length() < 3) {
                            log_manager_.getLogger()->warn("Invalid satellite line: {}", line);
                            continue;
                        }
                        std::string sat_id = line.substr(0, 3);
                        char sys_char = sat_id[0];
                        System system;
                        std::string system_str;
                        switch (sys_char) {
                        case 'G': system = System::GPS; system_str = "GPS"; break;
                        case 'R': system = System::GLONASS; system_str = "GLONASS"; break;
                        case 'E': system = System::Galileo; system_str = "Galileo"; break;
                        case 'C': system = System::BeiDou; system_str = "BeiDou"; break;
                        case 'J': system = System::QZSS; system_str = "QZSS"; break;
                        case 'S': system = System::SBAS; system_str = "SBAS"; break;
                        default:
                            log_manager_.getLogger()->warn("Unknown system for satellite: {}", sat_id);
                            continue;
                        }

                        RinexObservation obs;
                        obs.satellite = sat_id;
                        obs.system = system;
                        obs.time = epoch_time;

                        auto sys_it = sys_obs_counts.find(std::string(1, sys_char));
                        if (sys_it == sys_obs_counts.end()) {
                            log_manager_.getLogger()->warn("No observation types defined for system: {}", sys_char);
                            continue;
                        }
                        int num_types = sys_it->second;
                        std::vector<std::string> sys_obs_types;
                        for (const auto& type : obs_types) {
                            if (type[0] == sys_char) {
                                sys_obs_types.push_back(type.substr(1));
                            }
                        }

                        std::istringstream data_iss(line.substr(3));
                        for (int j = 0; j < num_types; ++j) {
                            std::string value_str, lli_str, snr_str;
                            data_iss >> std::setw(14) >> value_str >> std::setw(1) >> lli_str >> std::setw(1) >> snr_str;
                            if (value_str.empty() || value_str.find_first_not_of(" ") == std::string::npos) {
                                continue;
                            }
                            try {
                                double value = std::stod(value_str);
                                std::string type = sys_obs_types[j];
                                if (system == System::GLONASS && glonass_freq_slots_.count(sat_id)) {
                                    int k = glonass_freq_slots_[sat_id];
                                    type += "_k" + (k >= 0 ? std::to_string(k) : "m" + std::to_string(-k));
                                }
                                obs.measurements[type] = value;
                                double snr = snr_str.empty() ? 50.0 : std::stod(snr_str);
                                double variance = type[0] == 'C' ? 0.01 : 0.0001;
                                if (snr > 0 && snr < 60) {
                                    variance *= (1.0 + 1.0 / std::pow(10.0, snr / 10.0));
                                }
                                obs.variances[type] = variance;
                                if (!lli_str.empty()) {
                                    obs.lli_flags[type] = std::stoi(lli_str);
                                }
                                else {
                                    obs.lli_flags[type] = 0;
                                }
                            }
                            catch (const std::exception& e) {
                                log_manager_.getLogger()->debug("Invalid observation value for {} {}: {}", sat_id, sys_obs_types[j], value_str);
                            }
                        }

                        if (!obs.measurements.empty()) {
                            observations_.push_back(obs);
                        }
                    }
                }
            }
            else {
                // RINEX 2.x observation parsing
                while (std::getline(file, line)) {
                    if (line.length() < 26) continue;
                    std::istringstream iss(line.substr(0, 26));
                    int year, month, day, hour, min, flag;
                    double sec;
                    iss >> year >> month >> day >> hour >> min >> sec >> flag;
                    if (!iss || flag > 1) {
                        log_manager_.getLogger()->debug("Skipping invalid epoch line: {}", line);
                        continue;
                    }
                    year = year < 80 ? 2000 + year : 1900 + year; // RINEX 2.x 2-digit year
                    GnssTime epoch_time = TimeUtils::gregorianToGpsTime(year, month, day, hour, min, sec);

                    // Read number of satellites and satellite list
                    std::string sat_list = line.substr(29);
                    std::istringstream sat_iss(sat_list);
                    int num_sats;
                    sat_iss >> num_sats;
                    std::vector<std::string> sat_ids;
                    for (int i = 0; i < num_sats; ++i) {
                        if ((i % 12 == 0 && i > 0) || sat_iss.eof()) {
                            std::getline(file, line);
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
                            log_manager_.getLogger()->warn("Unexpected end of file while reading satellite data for {}", sat_id);
                            break;
                        }
                        char sys_char = sat_id[0];
                        System system;
                        std::string system_str;
                        switch (sys_char) {
                        case 'G': system = System::GPS; system_str = "GPS"; break;
                        case 'R': system = System::GLONASS; system_str = "GLONASS"; break;
                        case 'E': system = System::Galileo; system_str = "Galileo"; break;
                        case 'C': system = System::BeiDou; system_str = "BeiDou"; break;
                        case 'J': system = System::QZSS; system_str = "QZSS"; break;
                        case 'S': system = System::SBAS; system_str = "SBAS"; break;
                        default:
                            log_manager_.getLogger()->warn("Unknown system for satellite: {}", sat_id);
                            continue;
                        }

                        RinexObservation obs;
                        obs.satellite = sat_id;
                        obs.system = system;
                        obs.time = epoch_time;

                        auto sys_it = sys_obs_counts.find("G"); // RINEX 2.x primarily GPS
                        if (sys_it == sys_obs_counts.end()) {
                            log_manager_.getLogger()->warn("No observation types defined for system: G");
                            continue;
                        }
                        int num_types = sys_it->second;
                        std::vector<std::string> sys_obs_types;
                        for (const auto& type : obs_types) {
                            if (type[0] == 'G') {
                                sys_obs_types.push_back(type.substr(1));
                            }
                        }

                        std::istringstream data_iss(line);
                        int types_per_line = 5; // RINEX 2.x: 5 obs per line (16 chars each)
                        for (int j = 0; j < num_types; ++j) {
                            if (j > 0 && j % types_per_line == 0) {
                                if (!std::getline(file, line)) break;
                                data_iss.clear();
                                data_iss.str(line);
                            }
                            std::string value_str, lli_str, snr_str;
                            data_iss >> std::setw(14) >> value_str >> std::setw(1) >> lli_str >> std::setw(1) >> snr_str;
                            if (value_str.empty() || value_str.find_first_not_of(" ") == std::string::npos) {
                                continue;
                            }
                            try {
                                double value = std::stod(value_str);
                                std::string type = sys_obs_types[j];
                                if (system == System::GLONASS && glonass_freq_slots_.count(sat_id)) {
                                    int k = glonass_freq_slots_[sat_id];
                                    type += "_k" + (k >= 0 ? std::to_string(k) : "m" + std::to_string(-k));
                                }
                                obs.measurements[type] = value;
                                double snr = snr_str.empty() ? 50.0 : std::stod(snr_str);
                                double variance = type[0] == 'C' ? 0.01 : 0.0001;
                                if (snr > 0 && snr < 60) {
                                    variance *= (1.0 + 1.0 / std::pow(10.0, snr / 10.0));
                                }
                                obs.variances[type] = variance;
                                if (!lli_str.empty()) {
                                    obs.lli_flags[type] = std::stoi(lli_str);
                                }
                                else {
                                    obs.lli_flags[type] = 0;
                                }
                            }
                            catch (const std::exception& e) {
                                log_manager_.getLogger()->debug("Invalid observation value for {} {}: {}", sat_id, sys_obs_types[j], value_str);
                            }
                        }

                        if (!obs.measurements.empty()) {
                            observations_.push_back(obs);
                        }
                    }
                }
            }

            file.close();
            log_manager_.getLogger()->info("Parsed {} observations from RINEX file", observations_.size());
            return !observations_.empty();
        }
        catch (const std::exception& e) {
            log_manager_.getLogger()->error("Error parsing RINEX file {}: {}", config_.getRinexFilePath().string(), e.what());
            return false;
        }
    }

    /*const RinexHeader& RinexParser::getHeader() const {
        return header_;
    }*/

    const std::vector<RinexObservation>& RinexParser::getObservations() const {
        return observations_;
    }

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
                dt += 604800.0; // Handle week rollover
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

    bool RinexParser::parseHeader(std::ifstream& stream) {
        std::string line;
        while (std::getline(stream, line)) {
            if (line.find("END OF HEADER") != std::string::npos) {
                break;
            }
            if (line.find("RINEX VERSION") != std::string::npos) {
                try {
                    header_.version = std::stod(line.substr(0, 9));
                    spdlog::info("RINEX version: {}", header_.version);
                    if (header_.version < 3.0 || header_.version >= 4.0) {
                        spdlog::error("Unsupported RINEX version: {}", header_.version);
                        throw std::runtime_error("Unsupported RINEX version");
                    }
                }
                catch (const std::exception& e) {
                    spdlog::error("Invalid RINEX version format: {}", e.what());
                    return false;
                }
            }
            if (line.find("INTERVAL") != std::string::npos) {
                try {
                    header_.interval = std::stod(line.substr(0, 10));
                    spdlog::info("Measurement interval: {}s", header_.interval.value());
                }
                catch (const std::exception& e) {
                    spdlog::warn("Invalid interval format: {}", e.what());
                }
            }
            if (line.find("SYS / # / OBS TYPES") != std::string::npos) {
                std::istringstream iss(line);
                std::string sys;
                iss >> sys;
                System system = sys == "G" ? System::GPS :
                    sys == "E" ? System::Galileo :
                    sys == "R" ? System::GLONASS :
                    sys == "C" ? System::BeiDou :
                    sys == "J" ? System::QZSS :
                    sys == "S" ? System::SBAS : System::Unknown;
                if (system != System::Unknown) {
                    int num_types;
                    iss >> num_types;
                    spdlog::info("System {}: {} observation types", sys, num_types);
                    std::vector<std::string> types;
                    types.reserve(num_types);
                    for (int i = 0; i < num_types; ++i) {
                        if (i % 13 == 0 && i > 0) {
                            std::getline(stream, line); // Continuation line
                            iss.str(line);
                            iss.clear();
                        }
                        std::string type;
                        iss >> type;
                        if (validateObsCode(type, system)) {
                            types.push_back(type);
                        }
                    }
                    header_.obs_types[system] = types;
                }
            }
            if (line.find("REC # / TYPE / VERS") != std::string::npos) {
                header_.receiver_type = line.substr(20, 20);
                spdlog::info("Receiver type: {}", header_.receiver_type);
            }
            if (line.find("ANT # / TYPE") != std::string::npos) {
                header_.antenna_type = line.substr(20, 20);
                spdlog::info("Antenna type: {}", header_.antenna_type);
            }
            if (line.find("APPROX POSITION XYZ") != std::string::npos) {
                try {
                    std::istringstream iss(line);
                    iss >> header_.approx_position[0] >> header_.approx_position[1] >> header_.approx_position[2];
                    spdlog::info("Approx position: ({}, {}, {})",
                        header_.approx_position[0], header_.approx_position[1], header_.approx_position[2]);
                }
                catch (const std::exception& e) {
                    spdlog::warn("Invalid approx position format: {}", e.what());
                }
            }
        }
        if (header_.obs_types.empty()) {
            spdlog::error("No valid observation types found in header");
            return false;
        }
        return true;
    }

    bool RinexParser::parseObservations(std::ifstream& stream) {
        std::string line;
        while (std::getline(stream, line)) {
            if (line[0] == '>') { // Epoch flag
                // Parse epoch time
                int year, month, day, hour, min;
                double sec;
                int flag;
                std::istringstream iss(line.substr(1));
                iss >> year >> month >> day >> hour >> min >> sec >> flag;
                GnssTime time = TimeUtils::gregorianToGpsTime(year, month, day, hour, min, sec);

                // Read number of satellites
                int num_sats;
                iss >> num_sats;
                spdlog::debug("Number of satellites: {}", num_sats);

                // Parse observations for each satellite
                for (int i = 0; i < num_sats; ++i) {
                    std::getline(stream, line);
                    RinexObservation obs;
                    obs.time = time;
                    obs.satellite = line.substr(0, 3);
                    obs.system = obs.satellite[0] == 'G' ? System::GPS :
                        obs.satellite[0] == 'E' ? System::Galileo :
                        obs.satellite[0] == 'R' ? System::GLONASS :
                        obs.satellite[0] == 'C' ? System::BeiDou :
                        obs.satellite[0] == 'J' ? System::QZSS :
                        obs.satellite[0] == 'S' ? System::SBAS : System::Unknown;

                    // Parse measurements
                    std::istringstream obs_iss(line.substr(3));
                    auto& types = header_.obs_types[obs.system];
                    for (const auto& type : types) {
                        std::string value_str;
                        std::getline(obs_iss, value_str, ' ');
                        value_str.erase(std::remove_if(value_str.begin(), value_str.end(), isspace), value_str.end());
                        try {
                            if (!value_str.empty()) {
                                double value = std::stod(value_str);
                                obs.measurements[type] = value;
                                spdlog::trace("Sat {}: {} = {}", obs.satellite, type, value);
                            }
                        }
                        catch (const std::exception& e) {
                            spdlog::debug("Invalid measurement for {} (type {}): {}", obs.satellite, type, e.what());
                        }
                    }
                    observations_.push_back(obs);
                }
            }
        }
        if (observations_.empty()) {
            spdlog::error("No observations parsed");
            return false;
        }
        return true;
    }

    std::optional<std::string> RinexParser::validateObsCode(const std::string& code, System system) const {
        // RINEX 3.x observation code format: [CLDS][1-9][A-Z]
        if (code.length() != 3 || (code[0] != 'C' && code[0] != 'L' && code[0] != 'D' && code[0] != 'S') ||
            !std::isdigit(code[1]) || !std::isupper(code[2])) {
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
                // B1: I, Q, I+Q
                "C2I", "C2Q", "C2X", "L2I", "L2Q", "L2X", "D2I", "D2Q", "D2X", "S2I", "S2Q", "S2X",
                // B2: I, Q, I+Q
                "C7I", "C7Q", "C7X", "L7I", "L7Q", "L7X", "D7I", "D7Q", "D7X", "S7I", "S7Q", "S7X",
                // B3: I, Q, I+Q
                "C6I", "C6Q", "C6X", "L6I", "L6Q", "L6X", "D6I", "D6Q", "D6X", "S6I", "S6Q", "S6X",
                // B1C: A, B, A+B
                "C1D", "C1P", "C1X", "L1D", "L1P", "L1X", "D1D", "D1P", "D1X", "S1D", "S1P", "S1X"
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
        if (it == valid_codes.end()) return std::nullopt;
        return std::find(it->second.begin(), it->second.end(), code) != it->second.end()
            ? std::optional<std::string>{code} : std::nullopt;
    }

    //Eigen::Vector3d RinexParser::getApproxPositionXyz() const {
    //    return approx_position_xyz_; // Return stored approximate position
    //}

} // namespace GnssProcessor
//#endif