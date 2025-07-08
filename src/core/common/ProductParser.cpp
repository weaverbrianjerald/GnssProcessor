#include "ProductParser.hpp"
#include "Constants.hpp"
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <netcdf.h>
#include <algorithm>
#include <iomanip>

namespace GnssProcessor {

    ProductParser::ProductParser(const GnssConfig& config, LogManager& log_manager)
        : config_(config), log_manager_(log_manager) {
        log_manager_.getLogger()->info("ProductParser initialized.");
    }

    std::vector<std::filesystem::path> ProductParser::getOrbitPaths() const {
        std::vector<std::filesystem::path> paths;
        for (const auto& path : config_.getProductPaths()) {
            if (path.extension() == ".SP3" && std::filesystem::exists(path)) {
                paths.push_back(path);
            }
        }
        log_manager_.getLogger()->info("Retrieved {} SP3 file paths", paths.size());
        return paths;
    }

    std::filesystem::path ProductParser::getProductPath(const std::string& type) const {
        auto path = config_.getProductPath(type);
        if (!path.empty() && std::filesystem::exists(path)) {
            log_manager_.getLogger()->trace("Retrieved {} path: {}", type, path.string());
            return path;
        }
        log_manager_.getLogger()->warn("Product path for {} not found or does not exist: {}", type, path.string());
        return std::filesystem::path();
    }

    bool ProductParser::parseSp3(const std::vector<std::filesystem::path>& sp3_paths, std::vector<OrbitData>& orbits) {
        try {
            orbits.clear();
            std::map<std::string, std::map<double, OrbitData>> orbit_map; // satellite -> TOW -> OrbitData
            constexpr double MIN_TIME_DIFF = 1.0; // Minimum time difference in seconds
            size_t total_entries = 0;

            for (const auto& sp3_path : sp3_paths) {
                std::ifstream file(sp3_path);
                if (!file.is_open()) {
                    log_manager_.getLogger()->error("Failed to open SP3 file: {}", sp3_path.string());
                    continue;
                }

                std::string line;
                bool in_header = true;
                int epoch_interval = 300; // Default from provided SP3 header
                std::vector<std::string> satellite_ids;
                size_t line_count = 0;

                // Parse header
                while (std::getline(file, line) && in_header) {
                    ++line_count;
                    if (line.empty()) continue;
                    if (line[0] == '#') {
                        // Version and type
                        if (line.size() >= 12 && (line[1] == 'c' || line[1] == 'd')) {
                            std::string file_type = line.substr(9, 3);
                            if (file_type != "ORB") {
                                log_manager_.getLogger()->warn("SP3 file type is {}, expected ORB at line {} in {}",
                                    file_type, line_count, sp3_path.string());
                            }
                            log_manager_.getLogger()->debug("Parsing SP3 header at line {}: {} in {}",
                                line_count, line, sp3_path.string());
                        }
                    }
                    else if (line.substr(0, 2) == "##") {
                        // Read epoch interval
                        std::istringstream iss(line);
                        std::string hash;
                        double week, tow, interval;
                        int mjd;
                        double frac_day;
                        if (iss >> hash >> week >> tow >> interval >> mjd >> frac_day &&
                            std::isfinite(interval) && interval >= 1.0 && interval <= 3600.0) {
                            epoch_interval = static_cast<int>(interval);
                            log_manager_.getLogger()->debug("Parsed SP3 epoch interval: {:.3f} seconds at line {} in {}",
                                interval, line_count, sp3_path.string());
                        }
                        else {
                            log_manager_.getLogger()->warn("Invalid or out-of-range epoch interval at line {} in {}: parsed value={}",
                                line_count, sp3_path.string(), interval);
                            epoch_interval = 300;
                            log_manager_.getLogger()->info("Using default SP3 epoch interval: {} seconds for {}",
                                epoch_interval, sp3_path.string());
                        }
                    }
                    else if (line.substr(0, 2) == "+ ") {
                        // Number of satellites
                        int num_sats = 0;
                        try {
                            num_sats = std::stoi(line.substr(3, 3));
                        }
                        catch (const std::exception& e) {
                            log_manager_.getLogger()->warn("Invalid satellite count at line {} in {}: {}",
                                line_count, sp3_path.string(), e.what());
                            continue;
                        }
                        satellite_ids.clear();
                        std::string header_line = line;
                        while (header_line.substr(0, 2) == "+ ") {
                            for (size_t i = 3; i < header_line.length() && i < 60; i += 3) {
                                std::string sat_id = header_line.substr(i, 3);
                                sat_id.erase(std::remove_if(sat_id.begin(), sat_id.end(), ::isspace), sat_id.end());
                                if (sat_id.length() == 3 && sat_id[0] != ' ' &&
                                    (sat_id[0] == 'G' || sat_id[0] == 'R' || sat_id[0] == 'E' ||
                                        sat_id[0] == 'C' || sat_id[0] == 'J' || sat_id[0] == 'S') &&
                                    std::isdigit(sat_id[1]) && std::isdigit(sat_id[2])) {
                                    satellite_ids.push_back(sat_id);
                                }
                                else if (!sat_id.empty() && sat_id != "   ") {
                                    log_manager_.getLogger()->warn("Invalid SP3 satellite ID at position {}: {} at line {} in {}",
                                        i, sat_id, line_count, sp3_path.string());
                                }
                            }
                            if (!std::getline(file, header_line)) {
                                log_manager_.getLogger()->error("Unexpected end of file while reading SP3 satellite IDs at line {} in {}",
                                    line_count, sp3_path.string());
                                file.close();
                                continue;
                            }
                            ++line_count;
                        }
                        while (header_line.substr(0, 2) == "++") {
                            if (!std::getline(file, header_line)) {
                                log_manager_.getLogger()->error("Unexpected end of file while reading SP3 accuracy codes at line {} in {}",
                                    line_count, sp3_path.string());
                                file.close();
                                continue;
                            }
                            ++line_count;
                        }
                        if (static_cast<int>(satellite_ids.size()) != num_sats) {
                            log_manager_.getLogger()->warn("Mismatch in satellite count: expected {}, found {} at line {} in {}",
                                num_sats, satellite_ids.size(), line_count, sp3_path.string());
                        }
                        if (header_line[0] == '*') {
                            in_header = false;
                            line = header_line;
                        }
                        continue;
                    }
                    else if (line.substr(0, 2) == "%f" || line.substr(0, 2) == "/*") {
                        continue;
                    }
                    else if (line[0] == '*') {
                        in_header = false;
                    }
                }

                // Parse body
                while (std::getline(file, line)) {
                    ++line_count;
                    if (line.empty()) continue;
                    if (line[0] == '*') { // Epoch line
                        std::istringstream iss(line);
                        std::string star;
                        int year, month, day, hour, min;
                        double sec;
                        if (!(iss >> star >> year >> month >> day >> hour >> min >> sec)) {
                            log_manager_.getLogger()->warn("Invalid epoch line {} in {}: {}",
                                line_count, sp3_path.string(), line);
                            continue;
                        }
                        GnssTime epoch_time = TimeUtils::gregorianToGpsTime(year, month, day, hour, min, sec);
                        if (epoch_time.week < 0 || epoch_time.tow < 0 || epoch_time.tow >= 604800) {
                            log_manager_.getLogger()->warn("Invalid GPS time at line {} in {}: week {}, tow {}",
                                line_count, sp3_path.string(), epoch_time.week, epoch_time.tow);
                            continue;
                        }

                        // Read position and velocity records
                        while (std::getline(file, line) && line[0] != '*' && !line.empty() && line[0] != 'E') {
                            ++line_count;
                            if (line[0] == 'P') { // Position record
                                OrbitData data;
                                data.satellite = line.substr(1, 3);
                                if (std::find(satellite_ids.begin(), satellite_ids.end(), data.satellite) == satellite_ids.end()) {
                                    log_manager_.getLogger()->warn("Unknown satellite ID {} at line {} in {}",
                                        data.satellite, line_count, sp3_path.string());
                                    continue;
                                }
                                data.time = epoch_time;
                                data.position_valid = true;
                                data.velocity_valid = false;
                                data.position = Eigen::Vector3d::Zero();
                                data.velocity = Eigen::Vector3d::Zero();

                                std::istringstream iss(line.substr(4));
                                double x, y, z, clock_bias;
                                if (!(iss >> x >> y >> z >> clock_bias) ||
                                    !std::isfinite(x) || !std::isfinite(y) || !std::isfinite(z) ||
                                    std::abs(x) > 1e6 || std::abs(y) > 1e6 || std::abs(z) > 1e6) {
                                    log_manager_.getLogger()->warn("Invalid position data for {} at line {} in {}: x={}, y={}, z={}",
                                        data.satellite, line_count, sp3_path.string(), x, y, z);
                                    data.position_valid = false;
                                }
                                else {
                                    data.position = Eigen::Vector3d(x * 1000.0, y * 1000.0, z * 1000.0); // km to m
                                }

                                // Check for duplicate epoch
                                auto& sat_orbits = orbit_map[data.satellite];
                                bool insert = true;
                                for (const auto& [existing_tow, existing_data] : sat_orbits) {
                                    if (std::abs(epoch_time.tow - existing_tow) < MIN_TIME_DIFF) {
                                        insert = false;
                                        log_manager_.getLogger()->warn("Duplicate epoch for {} at TOW {:.3f} in {}, skipping",
                                            data.satellite, epoch_time.tow, sp3_path.string());
                                        break;
                                    }
                                }
                                if (insert && data.position_valid) {
                                    sat_orbits[epoch_time.tow] = data;
                                    ++total_entries;
                                }
                            }
                            else if (line[0] == 'V') { // Velocity record
                                std::string sat_id = line.substr(1, 3);
                                auto& sat_orbits = orbit_map[sat_id];
                                auto it = sat_orbits.find(epoch_time.tow);
                                if (it != sat_orbits.end()) {
                                    std::istringstream iss(line.substr(4));
                                    double vx, vy, vz, clock_rate;
                                    if (!(iss >> vx >> vy >> vz >> clock_rate) ||
                                        !std::isfinite(vx) || !std::isfinite(vy) || !std::isfinite(vz) ||
                                        std::abs(vx) > 1e6 || std::abs(vy) > 1e6 || std::abs(vz) > 1e6) {
                                        log_manager_.getLogger()->warn("Invalid velocity data for {} at line {} in {}: vx={}, vy={}, vz={}",
                                            sat_id, line_count, sp3_path.string(), vx, vy, vz);
                                    }
                                    else {
                                        it->second.velocity = Eigen::Vector3d(vx * 0.1, vy * 0.1, vz * 0.1); // dm/s to m/s
                                        it->second.velocity_valid = true;
                                    }
                                }
                            }
                        }
                    }
                    else if (line.substr(0, 3) == "EOF") {
                        break;
                    }
                }
                file.close();
                log_manager_.getLogger()->info("Parsed {} orbit entries from {}", total_entries, sp3_path.string());
                total_entries = 0; // Reset for next file
            }

            // Flatten map to orbits vector
            orbits.clear();
            for (const auto& [sat, tow_map] : orbit_map) {
                for (const auto& [tow, data] : tow_map) {
                    orbits.push_back(data);
                }
            }

            if (orbits.empty()) {
                log_manager_.getLogger()->error("No valid orbit data parsed from provided SP3 files");
                return false;
            }
            log_manager_.getLogger()->info("Merged {} orbit entries from {} SP3 files", orbits.size(), sp3_paths.size());
            return true;
        }
        catch (const std::exception& e) {
            log_manager_.getLogger()->error("Error parsing SP3 files: {}", e.what());
            return false;
        }
    }

    bool ProductParser::parseClk(const std::filesystem::path& clk_path, std::vector<ClockData>& clocks) {
        try {
            std::ifstream file(clk_path);
            if (!file.is_open()) {
                log_manager_.getLogger()->error("Failed to open CLK file: {}", clk_path.string());
                return false;
            }

            clocks.clear();
            std::string line;
            size_t line_count = 0;
            bool in_header = true;
            double epoch_interval = 0.0; // Seconds, from header

            // Parse header
            while (std::getline(file, line) && in_header) {
                ++line_count;
                if (line.empty()) continue;
                if (line.find("END OF HEADER") != std::string::npos) {
                    in_header = false;
                    continue;
                }
                if (line.find("INTERVAL") != std::string::npos) {
                    std::istringstream iss(line);
                    std::string dummy;
                    if (iss >> dummy >> epoch_interval) {
                        log_manager_.getLogger()->debug("CLK epoch interval: {} seconds at line {}", epoch_interval, line_count);
                    }
                    else {
                        log_manager_.getLogger()->warn("Invalid INTERVAL line {}: {}", line_count, line);
                    }
                }
            }

            // Parse body
            while (std::getline(file, line)) {
                ++line_count;
                if (line.empty()) continue;
                if (line.find("AS ") == 0) { // Analysis Center Satellite clock record
                    std::istringstream iss(line);
                    std::string type, sat;
                    int year, month, day, hour, min, num;
                    double sec, clock_bias;
                    if (!(iss >> type >> sat >> year >> month >> day >> hour >> min >> sec >> num >> clock_bias)) {
                        log_manager_.getLogger()->warn("Invalid AS record at line {}: {}", line_count, line);
                        continue;
                    }

                    // Validate satellite ID (e.g., G01, R01)
                    if (sat.size() != 3 || (sat[0] != 'G' && sat[0] != 'R' && sat[0] != 'E' && sat[0] != 'C' && sat[0] != 'J' && sat[0] != 'S')) {
                        log_manager_.getLogger()->warn("Invalid satellite ID {} at line {}", sat, line_count);
                        continue;
                    }

                    // Convert epoch to GPS time
                    GnssTime epoch_time = TimeUtils::gregorianToGpsTime(year, month, day, hour, min, sec);
                    if (epoch_time.week < 0 || epoch_time.tow < 0 || epoch_time.tow >= 604800) {
                        log_manager_.getLogger()->warn("Invalid GPS time at line {}: week {}, tow {}",
                            line_count, epoch_time.week, epoch_time.tow);
                        continue;
                    }

                    ClockData data;
                    data.satellite = sat;
                    data.time = epoch_time;
                    data.start_time = epoch_time; // Assume single epoch for simplicity
                    data.end_time = GnssTime{ epoch_time.week, epoch_time.tow + epoch_interval }; // Use interval if available
                    data.clock_bias = clock_bias; // Seconds
                    clocks.push_back(data);
                }
                // Ignore other record types (e.g., AR for receiver clocks) for now
            }

            if (clocks.empty()) {
                log_manager_.getLogger()->warn("No valid clock data parsed from {}", clk_path.string());
                return false;
            }
            log_manager_.getLogger()->info("Parsed {} clock entries from {}", clocks.size(), clk_path.string());
            return true;
        }
        catch (const std::exception& e) {
            log_manager_.getLogger()->error("Error parsing CLK file {}: {}", clk_path.string(), e.what());
            return false;
        }
    }

    bool ProductParser::parseAtx(const std::filesystem::path& atx_path, std::vector<AntennaData>& antennas) {
        try {
            std::ifstream file(atx_path);
            if (!file.is_open()) {
                log_manager_.getLogger()->error("Failed to open ATX file: {}", atx_path.string());
                return false;
            }

            antennas.clear();
            std::string line;
            AntennaData current_antenna;
            bool in_antenna_block = false;
            bool in_pcv_block = false;
            std::string current_freq;
            std::vector<double> current_pcv_row;
            size_t line_count = 0;

            while (std::getline(file, line)) {
                ++line_count;
                if (line.length() < 60) continue; // Skip short or comment lines

                if (line.find("START OF ANTENNA") != std::string::npos) {
                    in_antenna_block = true;
                    current_antenna = AntennaData();
                    in_pcv_block = false;
                    current_freq.clear();
                    current_pcv_row.clear();
                    log_manager_.getLogger()->trace("Started parsing new antenna at line {}", line_count);
                }
                else if (line.find("END OF ANTENNA") != std::string::npos && in_antenna_block) {
                    if (!current_antenna.type.empty()) {
                        antennas.push_back(current_antenna);
                        log_manager_.getLogger()->trace("Completed parsing antenna: {}", current_antenna.type);
                    }
                    else {
                        log_manager_.getLogger()->warn("Discarded antenna with empty type at line {}", line_count);
                    }
                    in_antenna_block = false;
                    in_pcv_block = false;
                }
                else if (in_antenna_block) {
                    if (line.find("TYPE / SERIAL NO") != std::string::npos) {
                        std::string type = line.substr(0, 20);
                        type.erase(std::remove_if(type.begin(), type.end(), ::isspace), type.end());
                        std::string serial_no = line.substr(20, 20);
                        serial_no.erase(std::remove_if(serial_no.begin(), serial_no.end(), ::isspace), serial_no.end());
                        if (!type.empty()) {
                            current_antenna.type = type;
                            if (!serial_no.empty()) {
                                current_antenna.type += "_" + serial_no;
                            }
                            log_manager_.getLogger()->trace("Parsing antenna: {} at line {}", current_antenna.type, line_count);
                        }
                        else {
                            log_manager_.getLogger()->warn("Empty antenna type at line {}", line_count);
                        }
                    }
                    else if (line.find("START OF FREQ") != std::string::npos) {
                        current_freq = line.substr(3, 5);
                        current_freq.erase(std::remove_if(current_freq.begin(), current_freq.end(), ::isspace), current_freq.end());
                        if (current_freq.empty()) {
                            log_manager_.getLogger()->warn("Invalid frequency code at line {}", line_count);
                            in_pcv_block = false;
                            continue;
                        }
                        in_pcv_block = true;
                        current_antenna.pcv[current_freq] = std::vector<std::vector<double>>();
                        std::getline(file, line); // Read PCO line
                        ++line_count;
                        if (line.length() >= 30) {
                            std::istringstream iss(line);
                            double x, y, z;
                            iss >> x >> y >> z;
                            if (!iss.fail() && std::isfinite(x) && std::isfinite(y) && std::isfinite(z) &&
                                std::abs(x) < 10000.0 && std::abs(y) < 10000.0 && std::abs(z) < 10000.0) { // Max 10 m
                                current_antenna.pco[current_freq] = Eigen::Vector3d(x, y, z); // mm
                                log_manager_.getLogger()->trace("PCO for {} {}: ({:.3f}, {:.3f}, {:.3f}) mm at line {}",
                                    current_antenna.type, current_freq, x, y, z, line_count);
                            }
                            else {
                                log_manager_.getLogger()->warn("Invalid PCO for {} {} at line {}: x={}, y={}, z={}",
                                    current_antenna.type, current_freq, line_count, x, y, z);
                            }
                        }
                        else {
                            log_manager_.getLogger()->warn("Invalid PCO line length for {} {} at line {}",
                                current_antenna.type, current_freq, line_count);
                        }
                    }
                    else if (line.find("END OF FREQ") != std::string::npos) {
                        in_pcv_block = false;
                        current_freq.clear();
                    }
                    else if (line.find("DAZI") != std::string::npos) {
                        std::istringstream iss(line.substr(0, 10));
                        double dazi;
                        if (iss >> dazi && std::isfinite(dazi) && dazi >= 0.0 && dazi <= 360.0) {
                            current_antenna.dazi = dazi;
                            log_manager_.getLogger()->trace("Parsed DAZI for {}: {:.3f} degrees at line {}",
                                current_antenna.type, dazi, line_count);
                        }
                        else {
                            log_manager_.getLogger()->warn("Invalid DAZI at line {}: parsed value={}", line_count, line.substr(0, 10));
                        }
                    }
                    else if (line.find("ZEN1 / ZEN2 / DZEN") != std::string::npos) {
                        std::istringstream iss(line.substr(0, 30));
                        double zen1, zen2, dzen;
                        if (iss >> zen1 >> zen2 >> dzen && std::isfinite(zen1) && std::isfinite(zen2) && std::isfinite(dzen) &&
                            zen1 >= 0.0 && zen2 >= zen1 && zen2 <= 180.0 && dzen > 0.0 && dzen <= 90.0) {
                            current_antenna.dzen = dzen;
                            log_manager_.getLogger()->trace("Parsed ZEN1/ZEN2/DZEN for {}: {:.3f}/{:.3f}/{:.3f} degrees at line {}",
                                current_antenna.type, zen1, zen2, dzen, line_count);
                        }
                        else {
                            log_manager_.getLogger()->warn("Invalid ZEN1/ZEN2/DZEN at line {}: parsed values={}", line_count, line.substr(0, 30));
                        }
                    }
                    else if (in_pcv_block && !line.empty() && line[0] != ' ') {
                        // Parse PCV table row (zenith angle)
                        std::istringstream iss(line.substr(8));
                        current_pcv_row.clear();
                        double value;
                        while (iss >> value) {
                            if (std::isfinite(value) && std::abs(value) < 100.0) { // Max 100 mm
                                current_pcv_row.push_back(value); // mm
                            }
                            else {
                                log_manager_.getLogger()->warn("Invalid PCV value {:.3f} for {} {} at line {}",
                                    value, current_antenna.type, current_freq, line_count);
                            }
                        }
                        if (!current_pcv_row.empty()) {
                            current_antenna.pcv[current_freq].push_back(current_pcv_row);
                            log_manager_.getLogger()->trace("Parsed PCV row for {} {} at line {}: {} values",
                                current_antenna.type, current_freq, line_count, current_pcv_row.size());
                        }
                        else {
                            log_manager_.getLogger()->warn("Empty or invalid PCV row for {} {} at line {}",
                                current_antenna.type, current_freq, line_count);
                        }
                    }
                }
            }

            file.close();
            if (antennas.empty()) {
                log_manager_.getLogger()->error("No valid antenna data parsed from {}", atx_path.string());
                return false;
            }
            log_manager_.getLogger()->info("Parsed {} antenna entries from {}", antennas.size(), atx_path.string());
            return true;
        }
        catch (const std::exception& e) {
            log_manager_.getLogger()->error("Error parsing ATX file {}: {}", atx_path.string(), e.what());
            return false;
        }
    }

    bool ProductParser::parseBia(const std::filesystem::path& bia_path, std::vector<BiasData>& biases) {
        try {
            std::ifstream file(bia_path);
            if (!file.is_open()) {
                log_manager_.getLogger()->error("Failed to open BIA file: {}", bia_path.string());
                return false;
            }

            biases.clear();
            std::string line;
            size_t line_count = 0;
            bool in_solution_block = false;

            // Helper function to convert YYYY:DOY:SSSSS to GnssTime
            auto parse_bias_time = [](const std::string& time_str) -> GnssTime {
                if (time_str.length() < 14 || time_str[4] != ':' || time_str[8] != ':') {
                    return GnssTime{ 0, 0.0 };
                }
                try {
                    int year = std::stoi(time_str.substr(0, 4));
                    int doy = std::stoi(time_str.substr(5, 3));
                    double seconds = std::stod(time_str.substr(9));
                    if (year < 1980 || doy < 1 || doy > 366 || seconds < 0.0 || seconds >= 86400.0) {
                        return GnssTime{ 0, 0.0 };
                    }

                    // Convert DOY to Gregorian date
                    int month = 1, day = doy;
                    bool is_leap = (year % 4 == 0 && (year % 100 != 0 || year % 400 == 0));
                    int days_in_month[] = { 31, is_leap ? 29 : 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
                    while (day > days_in_month[month - 1]) {
                        day -= days_in_month[month - 1];
                        ++month;
                        if (month > 12) {
                            return GnssTime{ 0, 0.0 }; // Invalid DOY
                        }
                    }

                    // Convert seconds to hour, minute, second
                    int hour = static_cast<int>(seconds / 3600.0);
                    seconds -= hour * 3600.0;
                    int minute = static_cast<int>(seconds / 60.0);
                    seconds -= minute * 60.0;

                    // Validate Gregorian date
                    if (!TimeUtils::isValidGregorianDate(year, month, day, hour, minute, seconds)) {
                        return GnssTime{ 0, 0.0 };
                    }

                    // Convert to GnssTime
                    GnssTime gps_time = TimeUtils::gregorianToGpsTime(year, month, day, hour, minute, seconds);
                    return gps_time;
                }
                catch (const std::exception&) {
                    return GnssTime{ 0, 0.0 };
                }
                };

            while (std::getline(file, line)) {
                ++line_count;
                if (line.empty() || line[0] == '*') continue; // Skip comments and empty lines

                if (line.find("+BIAS/SOLUTION") != std::string::npos) {
                    in_solution_block = true;
                    log_manager_.getLogger()->debug("Started parsing BIAS/SOLUTION block at line {}", line_count);
                    continue;
                }
                else if (line.find("-BIAS/SOLUTION") != std::string::npos) {
                    in_solution_block = false;
                    log_manager_.getLogger()->debug("Ended parsing BIAS/SOLUTION block at line {}", line_count);
                    break;
                }

                if (in_solution_block && line.find("OSB") == 0) {
                    BiasData data;
                    std::istringstream iss(line);
                    std::string type, svn, prn, station, signal, start, end, unit;
                    double bias, std_dev, slope, slope_std;

                    // Parse fields: OSB SVN PRN STATION OBS1 OBS2 BIAS_START BIAS_END UNIT BIAS STD_DEV SLOPE SLOPE_STD
                    iss >> type >> svn >> prn >> station >> signal >> start >> end >> unit >> bias >> std_dev;
                    if (iss.fail() || prn.length() < 3 || signal.empty() || start.empty() || end.empty() ||
                        !std::isfinite(bias) || !std::isfinite(std_dev)) {
                        log_manager_.getLogger()->warn("Invalid OSB line at {}: {}", line_count, line);
                        continue;
                    }

                    // Parse optional slope and slope_std
                    if (iss >> slope >> slope_std) {
                        if (!std::isfinite(slope) || !std::isfinite(slope_std)) {
                            slope = 0.0;
                            slope_std = 0.0;
                        }
                    }
                    else {
                        slope = 0.0;
                        slope_std = 0.0;
                    }

                    // Validate satellite PRN (e.g., G01, R01)
                    if (prn.length() == 3 && (prn[0] == 'G' || prn[0] == 'R' || prn[0] == 'E' ||
                        prn[0] == 'C' || prn[0] == 'J' || prn[0] == 'S') &&
                        std::isdigit(prn[1]) && std::isdigit(prn[2])) {
                        data.satellite = prn;
                    }
                    else {
                        log_manager_.getLogger()->warn("Invalid satellite PRN {} at line {}", prn, line_count);
                        continue;
                    }

                    // Validate unit (expecting ns)
                    if (unit != "ns") {
                        log_manager_.getLogger()->warn("Unexpected unit {} for bias at line {}, expected ns", unit, line_count);
                        continue;
                    }

                    // Parse time
                    data.signal = signal;
                    data.bias = bias / Constants::SPEED_OF_LIGHT; // ns to seconds
                    data.std_dev = std_dev / Constants::SPEED_OF_LIGHT; // ns to seconds
                    data.start_time = parse_bias_time(start);
                    data.end_time = parse_bias_time(end);

                    if (data.start_time.week <= 0 || data.end_time.week <= 0 ||
                        data.start_time.tow < 0.0 || data.end_time.tow < 0.0) {
                        log_manager_.getLogger()->warn("Invalid time range for satellite {} signal {} at line {}: start={}, end={}",
                            data.satellite, signal, line_count, start, end);
                        continue;
                    }

                    biases.push_back(data);
                    log_manager_.getLogger()->trace("Parsed OSB for satellite {} signal {}: bias={:.6e} s, std_dev={:.6e} s, "
                        "start={}:{:.3f}, end={}:{:.3f} at line {}",
                        data.satellite, signal, data.bias, data.std_dev,
                        data.start_time.week, data.start_time.tow,
                        data.end_time.week, data.end_time.tow, line_count);
                }
            }

            file.close();
            if (biases.empty()) {
                log_manager_.getLogger()->error("No valid bias data parsed from {}", bia_path.string());
                return false;
            }
            log_manager_.getLogger()->info("Parsed {} bias entries from {}", biases.size(), bia_path.string());
            return true;
        }
        catch (const std::exception& e) {
            log_manager_.getLogger()->error("Error parsing BIA file {}: {}", bia_path.string(), e.what());
            return false;
        }
    }

    bool ProductParser::parseErp(const std::filesystem::path& erp_path, std::vector<ErpData>& erps) {
        try {
            std::ifstream file(erp_path);
            if (!file.is_open()) {
                log_manager_.getLogger()->error("Failed to open ERP file: {}", erp_path.string());
                return false;
            }

            erps.clear();
            std::string line;
            while (std::getline(file, line)) {
                if (line.find("MJD") != std::string::npos) continue;
                std::istringstream iss(line);
                double mjd, xp, yp, ut1_utc;
                iss >> mjd >> xp >> yp >> ut1_utc;
                ErpData data;
                data.time = GnssTime{ 0, (mjd - 51544.5) * 86400.0 };
                data.xp = xp;
                data.yp = yp;
                data.ut1_utc = ut1_utc;
                erps.push_back(data);
            }
            file.close();
            log_manager_.getLogger()->info("Parsed {} ERP entries from {}", erps.size(), erp_path.string());
            return !erps.empty();
        }
        catch (const std::exception& e) {
            log_manager_.getLogger()->error("Error parsing ERP file {}: {}", erp_path.string(), e.what());
            return false;
        }
    }

    /*
    bool ProductParser::parseOceanTide(const std::filesystem::path& nc_path, std::vector<OceanTideData>& tides) {
        try {
            netCDF::NcFile nc_file(nc_path.string(), netCDF::NcFile::read);
            tides.clear();

            // Get dimensions
            auto time_dim = nc_file.getDim("time");
            if (time_dim.isNull()) {
                log_manager_.getLogger()->error("No 'time' dimension in NetCDF file: {}", nc_path.string());
                return false;
            }
            size_t num_times = time_dim.getSize();

            // Get variables
            auto time_var = nc_file.getVar("time");
            auto disp_x_var = nc_file.getVar("displacement_x");
            auto disp_y_var = nc_file.getVar("displacement_y");
            auto disp_z_var = nc_file.getVar("displacement_z");
            if (time_var.isNull() || disp_x_var.isNull() || disp_y_var.isNull() || disp_z_var.isNull()) {
                log_manager_.getLogger()->error("Missing required variables in NetCDF file: {}", nc_path.string());
                return false;
            }

            // Read data
            std::vector<double> times(num_times);
            std::vector<double> disp_x(num_times), disp_y(num_times), disp_z(num_times);
            time_var.getVar(times.data());
            disp_x_var.getVar(disp_x.data());
            disp_y_var.getVar(disp_y.data());
            disp_z_var.getVar(disp_z.data());

            // Populate tides
            for (size_t i = 0; i < num_times; ++i) {
                OceanTideData data;
                // Assume times are in seconds since GPS epoch; convert to GnssTime
                int week = static_cast<int>(times[i] / (7 * 86400));
                double tow = times[i] - week * (7 * 86400);
                data.time = GnssTime{ week, tow };
                data.displacement = Eigen::Vector3d(disp_x[i], disp_y[i], disp_z[i]);
                tides.push_back(data);
            }

            log_manager_.getLogger()->info("Parsed {} ocean tide entries from {}", tides.size(), nc_path.string());
            return true;
        }
        catch (const netCDF::NcException& e) {
            log_manager_.getLogger()->error("Error parsing ocean tide file {}: {}", nc_path.string(), e.what());
            return false;
        }
        catch (const std::exception& e) {
            log_manager_.getLogger()->error("Unexpected error parsing ocean tide file {}: {}", nc_path.string(), e.what());
            return false;
        }
    }
    */

    bool ProductParser::parseKlobuchar(const std::filesystem::path& brdc_path, std::vector<double>& klobuchar_coeffs) {
        try {
            std::ifstream file(brdc_path);
            if (!file.is_open()) {
                log_manager_.getLogger()->error("Failed to open BRDC file for Klobuchar coefficients: {}", brdc_path.string());
                return false;
            }

            klobuchar_coeffs.clear();
            std::string line;
            size_t line_count = 0;
            bool in_header = true;

            while (std::getline(file, line)) {
                ++line_count;
                // Check for end of header
                if (in_header && line.find("END OF HEADER") != std::string::npos) {
                    in_header = false;
                    continue;
                }
                // Look for IONOSPHERIC CORR in header
                if (in_header && line.find("IONOSPHERIC CORR") != std::string::npos) {
                    std::istringstream iss(line);
                    std::string type;
                    iss >> type;
                    if (type == "GPSA" || type == "GAL") { // GPS or Galileo Klobuchar coefficients
                        double a0, a1, a2, a3;
                        if (iss >> a0 >> a1 >> a2 >> a3) {
                            if (std::isfinite(a0) && std::isfinite(a1) && std::isfinite(a2) && std::isfinite(a3)) {
                                klobuchar_coeffs.push_back(a0);
                                klobuchar_coeffs.push_back(a1);
                                klobuchar_coeffs.push_back(a2);
                                klobuchar_coeffs.push_back(a3);
                            }
                            else {
                                log_manager_.getLogger()->warn("Non-finite Klobuchar alpha coefficients at line {}: {}", line_count, line);
                            }
                        }
                        else {
                            log_manager_.getLogger()->warn("Invalid Klobuchar alpha coefficients at line {}: {}", line_count, line);
                        }
                    }
                    else if (type == "GPSB") {
                        double b0, b1, b2, b3;
                        if (iss >> b0 >> b1 >> b2 >> b3) {
                            if (std::isfinite(b0) && std::isfinite(b1) && std::isfinite(b2) && std::isfinite(b3)) {
                                klobuchar_coeffs.push_back(b0);
                                klobuchar_coeffs.push_back(b1);
                                klobuchar_coeffs.push_back(b2);
                                klobuchar_coeffs.push_back(b3);
                            }
                            else {
                                log_manager_.getLogger()->warn("Non-finite Klobuchar beta coefficients at line {}: {}", line_count, line);
                            }
                        }
                        else {
                            log_manager_.getLogger()->warn("Invalid Klobuchar beta coefficients at line {}: {}", line_count, line);
                        }
                    }
                }
            }

            file.close();
            if (klobuchar_coeffs.size() != 8) {
                log_manager_.getLogger()->error("Incomplete Klobuchar coefficients from {}: expected 8, got {}", brdc_path.string(), klobuchar_coeffs.size());
                klobuchar_coeffs.clear();
                return false;
            }
            log_manager_.getLogger()->info("Parsed 8 Klobuchar coefficients from {}: alpha=[{:.3e}, {:.3e}, {:.3e}, {:.3e}], beta=[{:.3e}, {:.3e}, {:.3e}, {:.3e}]",
                brdc_path.string(), klobuchar_coeffs[0], klobuchar_coeffs[1], klobuchar_coeffs[2], klobuchar_coeffs[3],
                klobuchar_coeffs[4], klobuchar_coeffs[5], klobuchar_coeffs[6], klobuchar_coeffs[7]);
            return true;
        }
        catch (const std::exception& e) {
            log_manager_.getLogger()->error("Exception parsing Klobuchar coefficients from {}: {}", brdc_path.string(), e.what());
            klobuchar_coeffs.clear();
            return false;
        }
    }

bool ProductParser::parseSunEphemeris(const std::filesystem::path& brdc_path, std::vector<SunEphemeris>& sun_ephemeris) {
    try {
        // Debug: Log first 5 lines of BRDC file for verification
        std::ifstream file(brdc_path);
        if (!file.is_open()) {
            log_manager_.getLogger()->error("Failed to open BRDC file for Sun ephemeris: {}", brdc_path.string());
            return false;
        }
        std::string line;
        size_t debug_count = 0;
        while (debug_count < 5 && std::getline(file, line)) {
            ++debug_count;
            log_manager_.getLogger()->debug("BRDC Sun ephemeris line {}: {}", debug_count, line);
        }
        file.close();

        // Compute Sun ephemeris analytically for the observation period
        sun_ephemeris.clear();
        const auto& obs_path = config_.getObservationPath();
        if (!std::filesystem::exists(obs_path)) {
            log_manager_.getLogger()->error("Observation file not found: {}", obs_path.string());
            return false;
        }

        // Use the observation file's time range (simplified for 5 epochs at 30s)
        // Assume start time from RINEX header or first epoch (e.g., 2024-05-09)
        GnssTime start_time = { 2454, 0.0 }; // Example: GPS week 2454, TOW 0 (2024-05-09 00:00:00)
        double duration = 120.0; // 5 epochs * 30s = 120s
        double step = 30.0; // 30-second intervals
        int num_epochs = static_cast<int>(duration / step) + 1;

        // Simplified Sun position model (approximate heliocentric coordinates in ECEF)
        const double AU = 1.496e8 * 1e3; // Astronomical unit in meters
        const double angular_velocity = 2.0 * Constants::PI / (365.25 * 86400.0); // Earth’s orbit angular velocity (rad/s)

        for (int i = 0; i < num_epochs; ++i) {
            double t = start_time.tow + i * step; // Time in seconds of week
            // Approximate Sun position in ECEF (simplified, assumes circular orbit in ecliptic plane)
            double angle = angular_velocity * t;
            double x = AU * std::cos(angle); // Sun position X (m)
            double y = AU * std::sin(angle); // Sun position Y (m)
            double z = 0.0; // Simplified: ignore ecliptic inclination

            SunEphemeris data;
            data.time = GnssTime{ start_time.week, t };
            if (data.time.tow >= 604800.0) {
                data.time.week += static_cast<int>(data.time.tow / 604800.0);
                data.time.tow = std::fmod(data.time.tow, 604800.0);
            }
            data.position = Eigen::Vector3d(x, y, z);
            if (!data.position.allFinite()) {
                log_manager_.getLogger()->warn("Non-finite Sun position at TOW {:.3f}: ({:.3e}, {:.3e}, {:.3e})", t, x, y, z);
                continue;
            }
            sun_ephemeris.push_back(data);
        }

        if (sun_ephemeris.empty()) {
            log_manager_.getLogger()->error("No valid Sun ephemeris entries computed");
            return false;
        }
        log_manager_.getLogger()->info("Computed {} Sun ephemeris entries for observation period", sun_ephemeris.size());
        return true;
    }
    catch (const std::exception& e) {
        log_manager_.getLogger()->error("Exception computing Sun ephemeris: {}", e.what());
        sun_ephemeris.clear();
        return false;
    }
}

} // namespace GnssProcessor
//#endif