#include "ProductParser.hpp"
#include "Constants.hpp"
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <netcdf.h>
#include <algorithm>
#include <iomanip>
#include <regex>

namespace GnssProcessor {

    namespace {
        std::string sanitizeAntennaType(const std::string& in) {
            std::string out;
            out.reserve(in.size());
            for (char c : in) {
                if (!std::isspace(static_cast<unsigned char>(c))) out.push_back(c);
            }
            return out;
        }
    }

    ProductParser::ProductParser(const GnssConfig& config, LogManager& log_manager)
        : config_(config), log_manager_(log_manager), rinex_parser_(nullptr), planetary_(log_manager) {
        log_manager_.getLogger("product_parser")->info("ProductParser initialized.");
    }

    void ProductParser::setRinexParser(RinexParser& rinex) {
        rinex_parser_ = &rinex;
        log_manager_.getLogger("product_parser")->trace("RinexParser set for ProductParser");
    }

    std::vector<std::filesystem::path> ProductParser::getOrbitPaths() const {
        std::vector<std::filesystem::path> paths;
        for (const auto& path : config_.getProductPaths()) {
            if (path.extension() == ".SP3" && std::filesystem::exists(path)) {
                paths.push_back(path);
            }
        }
        log_manager_.getLogger("product_parser")->info("Retrieved {} SP3 file paths", paths.size());
        return paths;
    }

    std::filesystem::path ProductParser::getProductPath(const std::string& type) const {
        auto path = config_.getProductPath(type);
        if (!path.empty() && std::filesystem::exists(path)) {
            log_manager_.getLogger("product_parser")->trace("Retrieved {} path: {}", type, path.string());
            return path;
        }
        log_manager_.getLogger("product_parser")->warn("Product path for {} not found or does not exist: {}", type, path.string());
        return std::filesystem::path();
    }

    bool ProductParser::parseSp3(const std::vector<std::filesystem::path>& sp3_paths, std::vector<OrbitData>& orbits) {
        try {
            orbits.clear();
            std::map<std::string, std::map<double, OrbitData>> orbit_map;
            constexpr double MIN_TIME_DIFF = 1.0;
            size_t total_entries = 0;

            for (const auto& sp3_path : sp3_paths) {
                std::ifstream file(sp3_path);
                if (!file.is_open()) {
                    log_manager_.getLogger("product_parser")->error("Failed to open SP3 file: {}", sp3_path.string());
                    continue;
                }

                std::string line;
                bool in_header = true;
                int epoch_interval = 300;
                std::vector<std::string> satellite_ids;
                size_t line_count = 0;

                while (std::getline(file, line) && in_header) {
                    ++line_count;
                    if (line.empty()) continue;
                    if (line[0] == '#') {
                        if (line.size() >= 12 && (line[1] == 'c' || line[1] == 'd')) {
                            log_manager_.getLogger("product_parser")->debug("Parsing SP3 header at line {}: {} in {}",
                                line_count, line, sp3_path.string());
                        }
                    }
                    else if (line.substr(0, 2) == "##") {
                        std::istringstream iss(line);
                        std::string hash;
                        double week, tow, interval;
                        int mjd;
                        double frac_day;
                        if (iss >> hash >> week >> tow >> interval >> mjd >> frac_day &&
                            std::isfinite(interval) && interval >= 1.0 && interval <= 3600.0) {
                            epoch_interval = static_cast<int>(interval);
                            log_manager_.getLogger("product_parser")->debug("Parsed SP3 epoch interval: {:.3f} seconds at line {} in {}",
                                interval, line_count, sp3_path.string());
                        }
                        else {
                            log_manager_.getLogger("product_parser")->warn("Invalid or out-of-range epoch interval at line {} in {}: parsed value={}",
                                line_count, sp3_path.string(), interval);
                            epoch_interval = 300;
                            log_manager_.getLogger("product_parser")->info("Using default SP3 epoch interval: {} seconds for {}",
                                epoch_interval, sp3_path.string());
                        }
                    }
                    else if (line.substr(0, 2) == "+ ") {
                        int num_sats = 0;
                        try {
                            num_sats = std::stoi(line.substr(3, 3));
                        }
                        catch (const std::exception& e) {
                            log_manager_.getLogger("product_parser")->warn("Invalid satellite count at line {} in {}: {}",
                                line_count, sp3_path.string(), e.what());
                            continue;
                        }
                        satellite_ids.clear();
                        std::string header_line = line;
                        while (header_line.substr(0, 2) == "+ ") {
                            for (size_t i = 3; i < header_line.length() && i < 60; i += 3) {
                                std::string sat_id = header_line.substr(i, 3);
                                sat_id.erase(std::remove_if(sat_id.begin(), sat_id.end(), ::isspace), sat_id.end());
                                if (sat_id.length() == 3 &&
                                    (sat_id[0] == 'G' || sat_id[0] == 'R' || sat_id[0] == 'E' ||
                                        sat_id[0] == 'C' || sat_id[0] == 'J' || sat_id[0] == 'S') &&
                                    std::isdigit(sat_id[1]) && std::isdigit(sat_id[2])) {
                                    satellite_ids.push_back(sat_id);
                                }
                            }
                            if (!std::getline(file, header_line)) {
                                log_manager_.getLogger("product_parser")->error("Unexpected end of file while reading SP3 satellite IDs at line {} in {}",
                                    line_count, sp3_path.string());
                                file.close();
                                continue;
                            }
                            ++line_count;
                        }
                        while (header_line.substr(0, 2) == "++") {
                            if (!std::getline(file, header_line)) {
                                log_manager_.getLogger("product_parser")->error("Unexpected end of file while reading SP3 accuracy codes at line {} in {}",
                                    line_count, sp3_path.string());
                                file.close();
                                continue;
                            }
                            ++line_count;
                        }
                        if (static_cast<int>(satellite_ids.size()) != num_sats) {
                            log_manager_.getLogger("product_parser")->warn("Mismatch in satellite count: expected {}, found {} at line {} in {}",
                                num_sats, satellite_ids.size(), line_count, sp3_path.string());
                        }
                        if (!header_line.empty() && header_line[0] == '*') {
                            in_header = false;
                            line = header_line;
                        }
                        continue;
                    }
                    else if (line.substr(0, 2) == "%f" || line.substr(0, 2) == "/*") {
                        continue;
                    }
                    else if (!line.empty() && line[0] == '*') {
                        in_header = false;
                    }
                }

                while (std::getline(file, line)) {
                    ++line_count;
                    if (line.empty()) continue;
                    if (line[0] == '*') {
                        std::istringstream iss(line);
                        std::string star;
                        int year, month, day, hour, min;
                        double sec;
                        if (!(iss >> star >> year >> month >> day >> hour >> min >> sec)) {
                            log_manager_.getLogger("product_parser")->warn("Invalid epoch line {} in {}: {}",
                                line_count, sp3_path.string(), line);
                            continue;
                        }
                        GnssTime epoch_time = TimeUtils::gregorianToGpsTime(year, month, day, hour, min, sec);
                        if (epoch_time.week < 0 || epoch_time.tow < 0 || epoch_time.tow >= 604800) {
                            log_manager_.getLogger("product_parser")->warn("Invalid GPS time at line {} in {}: week {}, tow {}",
                                line_count, sp3_path.string(), epoch_time.week, epoch_time.tow);
                            continue;
                        }

                        while (std::getline(file, line) && line[0] != '*' && !line.empty() && line[0] != 'E') {
                            ++line_count;
                            if (line[0] == 'P') {
                                OrbitData data;
                                data.satellite = line.substr(1, 3);
                                if (std::find(satellite_ids.begin(), satellite_ids.end(), data.satellite) == satellite_ids.end()) {
                                    log_manager_.getLogger("product_parser")->warn("Unknown satellite ID {} at line {} in {}",
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
                                    log_manager_.getLogger("product_parser")->warn("Invalid position data for {} at line {} in {}: x={}, y={}, z={}",
                                        data.satellite, line_count, sp3_path.string(), x, y, z);
                                    data.position_valid = false;
                                }
                                else {
                                    data.position = Eigen::Vector3d(x * 1000.0, y * 1000.0, z * 1000.0);
                                }

                                auto& sat_orbits = orbit_map[data.satellite];
                                bool insert = true;
                                for (const auto& [existing_tow, existing_data] : sat_orbits) {
                                    if (std::abs(epoch_time.tow - existing_tow) < MIN_TIME_DIFF) {
                                        insert = false;
                                        log_manager_.getLogger("product_parser")->warn("Duplicate epoch for {} at TOW {:.3f} in {}, skipping",
                                            data.satellite, epoch_time.tow, sp3_path.string());
                                        break;
                                    }
                                }
                                if (insert && data.position_valid) {
                                    sat_orbits[epoch_time.tow] = data;
                                    ++total_entries;
                                }
                            }
                            else if (line[0] == 'V') {
                                std::string sat_id = line.substr(1, 3);
                                auto& sat_orbits = orbit_map[sat_id];
                                auto it = sat_orbits.find(epoch_time.tow);
                                if (it != sat_orbits.end()) {
                                    std::istringstream iss(line.substr(4));
                                    double vx, vy, vz, clock_rate;
                                    if (!(iss >> vx >> vy >> vz >> clock_rate) ||
                                        !std::isfinite(vx) || !std::isfinite(vy) || !std::isfinite(vz) ||
                                        std::abs(vx) > 1e6 || std::abs(vy) > 1e6 || std::abs(vz) > 1e6) {
                                        log_manager_.getLogger("product_parser")->warn("Invalid velocity data for {} at line {} in {}: vx={}, vy={}, vz={}",
                                            sat_id, line_count, sp3_path.string(), vx, vy, vz);
                                    }
                                    else {
                                        it->second.velocity = Eigen::Vector3d(vx * 0.1, vy * 0.1, vz * 0.1);
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
                log_manager_.getLogger("product_parser")->info("Parsed {} orbit entries from {}", total_entries, sp3_path.string());
                total_entries = 0;
            }

            orbits.clear();
            for (const auto& [sat, tow_map] : orbit_map) {
                for (const auto& [tow, data] : tow_map) {
                    orbits.push_back(data);
                }
            }

            if (orbits.empty()) {
                log_manager_.getLogger("product_parser")->error("No valid orbit data parsed from provided SP3 files");
                return false;
            }
            log_manager_.getLogger("product_parser")->info("Merged {} orbit entries from {} SP3 files", orbits.size(), sp3_paths.size());
            return true;
        }
        catch (const std::exception& e) {
            log_manager_.getLogger("product_parser")->error("Error parsing SP3 files: {}", e.what());
            return false;
        }
    }

    bool ProductParser::parseClk(const std::filesystem::path& clk_path, std::vector<ClockData>& clocks) {
        try {
            std::ifstream file(clk_path);
            if (!file.is_open()) {
                log_manager_.getLogger("product_parser")->error("Failed to open CLK file: {}", clk_path.string());
                return false;
            }

            clocks.clear();
            std::string line;
            size_t line_count = 0;
            bool in_header = true;
            double epoch_interval = 0.0;

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
                        log_manager_.getLogger("product_parser")->debug("CLK epoch interval: {} seconds at line {}", epoch_interval, line_count);
                    }
                    else {
                        log_manager_.getLogger("product_parser")->warn("Invalid INTERVAL line {}: {}", line_count, line);
                    }
                }
            }

            while (std::getline(file, line)) {
                ++line_count;
                if (line.empty()) continue;
                if (line.find("AS ") == 0) {
                    std::istringstream iss(line);
                    std::string type, sat;
                    int year, month, day, hour, min, num;
                    double sec, clock_bias;
                    if (!(iss >> type >> sat >> year >> month >> day >> hour >> min >> sec >> num >> clock_bias)) {
                        log_manager_.getLogger("product_parser")->warn("Invalid AS record at line {}: {}", line_count, line);
                        continue;
                    }

                    if (sat.size() != 3 || (sat[0] != 'G' && sat[0] != 'R' && sat[0] != 'E' && sat[0] != 'C' && sat[0] != 'J' && sat[0] != 'S')) {
                        log_manager_.getLogger("product_parser")->warn("Invalid satellite ID {} at line {}", sat, line_count);
                        continue;
                    }

                    GnssTime epoch_time = TimeUtils::gregorianToGpsTime(year, month, day, hour, min, sec);
                    if (epoch_time.week < 0 || epoch_time.tow < 0 || epoch_time.tow >= 604800) {
                        log_manager_.getLogger("product_parser")->warn("Invalid GPS time at line {}: week {}, tow {}",
                            line_count, epoch_time.week, epoch_time.tow);
                        continue;
                    }

                    ClockData data;
                    data.satellite = sat;
                    data.time = epoch_time;
                    data.start_time = epoch_time;
                    data.end_time = GnssTime{ epoch_time.week, epoch_time.tow + epoch_interval };
                    data.clock_bias = clock_bias;
                    clocks.push_back(data);
                }
            }

            if (clocks.empty()) {
                log_manager_.getLogger("product_parser")->warn("No valid clock data parsed from {}", clk_path.string());
                return false;
            }
            log_manager_.getLogger("product_parser")->info("Parsed {} clock entries from {}", clocks.size(), clk_path.string());
            return true;
        }
        catch (const std::exception& e) {
            log_manager_.getLogger("product_parser")->error("Error parsing CLK file {}: {}", clk_path.string(), e.what());
            return false;
        }
    }

    bool ProductParser::parseAtx(const std::filesystem::path& atx_path, std::vector<AntennaData>& antennas) {
        try {
            std::ifstream file(atx_path);
            if (!file.is_open()) {
                log_manager_.getLogger("product_parser")->error("Failed to open ATX file: {}", atx_path.string());
                return false;
            }

            antennas.clear();
            std::string line;
            AntennaData current;
            bool in_antenna = false;
            bool in_freq = false;
            std::string current_freq;
            size_t line_count = 0;

            auto trim = [](std::string& s) {
                s.erase(0, s.find_first_not_of(" \t\r\n"));
                s.erase(s.find_last_not_of(" \t\r\n") + 1);
                };

            // Removes whitespace only (keeps underscore if present)
            auto sanitize = [](const std::string& s) {
                std::string out;
                out.reserve(s.size());
                for (char c : s) if (!std::isspace(static_cast<unsigned char>(c))) out.push_back(c);
                return out;
                };

            auto parse_valid_line = [&](const std::string& src) -> GnssTime {
                if (src.find("VALID FROM") != std::string::npos || src.find("VALID UNTIL") != std::string::npos) {
                    std::istringstream iss(src.substr(0, 50));
                    int year, month, day, hour, minute;
                    double second;
                    if (iss >> year >> month >> day >> hour >> minute >> second) {
                        if (year >= 1980 && month >= 1 && month <= 12 && day >= 1 && day <= 31 &&
                            hour >= 0 && hour <= 23 && minute >= 0 && minute <= 59 && second >= 0.0 && second < 61.0) {
                            try { return TimeUtils::gregorianToGpsTime(year, month, day, hour, minute, second); }
                            catch (...) { return GnssTime{ 0,0.0 }; }
                        }
                    }
                }
                // Textual fallback (rare)
                std::regex rgx(R"(VALID\s+(FROM|UNTIL)\s+(\d{1,2})-([A-Za-z]{3})-(\d{2,4})\s+(\d{2}):(\d{2})(?::(\d{2}))?)");
                std::smatch m;
                if (std::regex_search(src, m, rgx)) {
                    std::map<std::string, int> month_map{
                        {"JAN",1},{"FEB",2},{"MAR",3},{"APR",4},{"MAY",5},{"JUN",6},
                        {"JUL",7},{"AUG",8},{"SEP",9},{"OCT",10},{"NOV",11},{"DEC",12}
                    };
                    int day = std::stoi(m[2].str());
                    std::string mon_str = m[3].str();
                    for (char& c : mon_str) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
                    int month = month_map.count(mon_str) ? month_map[mon_str] : 0;
                    int year = std::stoi(m[4].str());
                    if (m[4].length() == 2) year += (year < 80) ? 2000 : 1900;
                    int hour = std::stoi(m[5].str());
                    int minute = std::stoi(m[6].str());
                    int second = m[7].matched ? std::stoi(m[7].str()) : 0;
                    if (month > 0) {
                        try { return TimeUtils::gregorianToGpsTime(year, month, day, hour, minute, double(second)); }
                        catch (...) { return GnssTime{ 0,0.0 }; }
                    }
                }
                return GnssTime{ 0,0.0 };
                };

            std::regex freqCodeRegex(R"(([GREJCIS]\d{2}))", std::regex_constants::icase);

            // Per-frequency PCV accumulators:
            // - NOAZI: values over zenith
            // - AZ rows: each row begins with azimuth (deg) followed by values over zenith
            std::vector<double> current_noazi;
            std::vector<std::pair<double, std::vector<double>>> current_az_rows;

            auto reset_freq_accumulators = [&]() {
                current_noazi.clear();
                current_az_rows.clear();
                };

            auto finalize_frequency = [&]() {
                if (current_freq.empty()) return;

                // If we have azimuth rows, build [zenith][azimuth]
                if (!current_az_rows.empty()) {
                    const size_t az_count = current_az_rows.size();
                    const size_t zen_count = current_az_rows.front().second.size();
                    if (az_count == 0 || zen_count == 0) {
                        current.pcv[current_freq].clear();
                        reset_freq_accumulators();
                        return;
                    }

                    // Ensure consistent zenith count
                    for (const auto& [azi, vals] : current_az_rows) {
                        if (vals.size() != zen_count) {
                            log_manager_.getLogger("product_parser")->warn(
                                "Inconsistent PCV row length for {} {}: expected {}, got {} (az={:.1f})",
                                current.type, current_freq, zen_count, vals.size(), azi);
                        }
                    }

                    std::vector<std::vector<double>> pcv_matrix(zen_count, std::vector<double>(az_count, 0.0));
                    for (size_t a = 0; a < az_count; ++a) {
                        const auto& vals = current_az_rows[a].second;
                        const size_t n = std::min(vals.size(), zen_count);
                        for (size_t z = 0; z < n; ++z) {
                            pcv_matrix[z][a] = vals[z];
                        }
                    }

                    current.pcv[current_freq] = std::move(pcv_matrix);
                    log_manager_.getLogger("product_parser")->trace(
                        "Finalized AZI PCV for {} {}: zen={}, az={}", current.type, current_freq, zen_count, az_count);
                }
                // Else NOAZI-only: store as [zenith][1]
                else if (!current_noazi.empty()) {
                    std::vector<std::vector<double>> pcv_matrix;
                    pcv_matrix.reserve(current_noazi.size());
                    for (double v : current_noazi) {
                        pcv_matrix.push_back({ v });
                    }
                    current.pcv[current_freq] = std::move(pcv_matrix);
                    log_manager_.getLogger("product_parser")->trace(
                        "Finalized NOAZI PCV for {} {}: zen_samples={}", current.type, current_freq, current_noazi.size());
                }
                else {
                    log_manager_.getLogger("product_parser")->debug("No PCV data for {} {}", current.type, current_freq);
                }

                reset_freq_accumulators();
                current_freq.clear();
                };

            while (std::getline(file, line)) {
                ++line_count;
                if (line.empty()) continue;

                if (line.find("START OF ANTENNA") != std::string::npos) {
                    in_antenna = true;
                    in_freq = false;
                    current = AntennaData();
                    current_freq.clear();
                    reset_freq_accumulators();
                    continue;
                }

                if (line.find("END OF ANTENNA") != std::string::npos) {
                    if (in_freq) {
                        finalize_frequency();
                    }
                    if (in_antenna && !current.type.empty()) {
                        antennas.push_back(current);
                        log_manager_.getLogger("product_parser")->trace(
                            "Stored antenna {} (valid_from week={}, tow={:.3f}, valid_until week={}, tow={:.3f})",
                            current.type,
                            current.valid_from.week, current.valid_from.tow,
                            current.valid_until.week, current.valid_until.tow);
                    }
                    in_antenna = false;
                    in_freq = false;
                    continue;
                }

                if (!in_antenna) continue;

                // TYPE / SERIAL NO
                if (line.find("TYPE / SERIAL NO") != std::string::npos) {
                    std::string left = line.substr(0, line.find("TYPE / SERIAL NO"));
                    trim(left);
                    std::istringstream tss(left);
                    std::vector<std::string> toks;
                    std::string tok;
                    while (tss >> tok) toks.push_back(tok);

                    std::string prn_token, svn_token, block_token;
                    for (size_t i = 0; i < toks.size(); ++i) {
                        const std::string& t = toks[i];
                        if (t.size() == 3 &&
                            (t[0] == 'G' || t[0] == 'R' || t[0] == 'E' || t[0] == 'C' || t[0] == 'J' || t[0] == 'S') &&
                            std::isdigit(t[1]) && std::isdigit(t[2])) {
                            prn_token = t;
                        }
                        if (t.size() == 4 &&
                            (t[0] == 'G' || t[0] == 'R' || t[0] == 'E' || t[0] == 'C' || t[0] == 'J' || t[0] == 'S') &&
                            std::isdigit(t[1]) && std::isdigit(t[2]) && std::isdigit(t[3])) {
                            svn_token = t;
                        }
                        if (t == "BLOCK" && i + 1 < toks.size()) {
                            block_token = "BLOCK_" + toks[i + 1];
                        }
                    }

                    // These are fixed-width fields in the ATX format.
                    std::string field1 = line.substr(0, 20);
                    std::string field2 = (line.size() >= 40) ? line.substr(20, 20) : "";
                    trim(field1);
                    trim(field2);

                    // IMPORTANT: receiver antenna like "JAVRINGANT_DM   SCIS" often lives entirely in field1
                    // (field2 is empty). Normalize it to "MODEL_SERIAL" if possible.
                    if (!prn_token.empty()) {
                        // Satellite antenna: PRN + block + SVN, using field1 as block/model fallback.
                        std::string block_norm = sanitize(block_token.empty() ? field1 : block_token);
                        std::string svn_norm = sanitize(svn_token);
                        if (!svn_norm.empty())
                            current.type = prn_token + "_" + block_norm + "_" + svn_norm;
                        else
                            current.type = prn_token + "_" + block_norm;
                    }
                    else {
                        // Receiver antenna
                        std::string model = field1;
                        std::string serial = field2;

                        if (serial.empty()) {
                            // Try to split field1 into "model serial"
                            std::istringstream iss(field1);
                            std::vector<std::string> parts;
                            std::string p;
                            while (iss >> p) parts.push_back(p);

                            if (parts.size() >= 2) {
                                model = parts.front();
                                serial = parts.back();
                            }
                        }

                        model = sanitize(model);
                        serial = sanitize(serial);
                        current.type = serial.empty() ? model : (model + "_" + serial);
                    }

                    log_manager_.getLogger("product_parser")->trace("Parsed antenna type '{}'", current.type);
                    continue;
                }

                // VALID FROM / VALID UNTIL
                if (line.find("VALID FROM") != std::string::npos) {
                    current.valid_from = parse_valid_line(line);
                    if (current.valid_from.week == 0)
                        log_manager_.getLogger("product_parser")->warn("Failed to parse VALID FROM at line {}", line_count);
                    continue;
                }
                if (line.find("VALID UNTIL") != std::string::npos) {
                    current.valid_until = parse_valid_line(line);
                    if (current.valid_until.week == 0)
                        log_manager_.getLogger("product_parser")->warn("Failed to parse VALID UNTIL at line {}", line_count);
                    continue;
                }

                // DAZI
                if (line.find("DAZI") != std::string::npos) {
                    std::istringstream iss(line);
                    double dazi;
                    if (iss >> dazi && std::isfinite(dazi)) {
                        current.dazi = dazi;
                    }
                    continue;
                }

                // ZEN1 / ZEN2 / DZEN
                if (line.find("ZEN1 / ZEN2 / DZEN") != std::string::npos) {
                    std::istringstream iss(line);
                    double zen1, zen2, dzen;
                    if (iss >> zen1 >> zen2 >> dzen &&
                        std::isfinite(zen1) && std::isfinite(zen2) && std::isfinite(dzen)) {
                        current.dzen = dzen;
                    }
                    continue;
                }

                // START OF FREQUENCY
                if (line.find("START OF FREQUENCY") != std::string::npos) {
                    if (in_freq) {
                        finalize_frequency();
                    }
                    in_freq = true;
                    reset_freq_accumulators();

                    std::smatch m;
                    if (std::regex_search(line, m, freqCodeRegex)) {
                        current_freq = sanitize(m.str());
                    }
                    if (current_freq.empty()) {
                        log_manager_.getLogger("product_parser")->warn("Could not detect frequency code at line {}", line_count);
                        in_freq = false;
                        continue;
                    }
                    current.pcv[current_freq] = {};
                    continue;
                }

                // END OF FREQUENCY
                if (line.find("END OF FREQUENCY") != std::string::npos) {
                    if (in_freq) {
                        finalize_frequency();
                    }
                    in_freq = false;
                    continue;
                }

                if (!in_freq || current_freq.empty()) continue;

                // PCO
                if (line.find("NORTH / EAST / UP") != std::string::npos) {
                    std::string numeric_part = line.substr(0, line.find("NORTH"));
                    std::istringstream iss(numeric_part);
                    double n, e, u;
                    if (iss >> n >> e >> u &&
                        std::isfinite(n) && std::isfinite(e) && std::isfinite(u)) {
                        current.pco[current_freq] = Eigen::Vector3d(n, e, u);
                        log_manager_.getLogger("product_parser")->trace("PCO {} {}: N/E/U=({:.2f},{:.2f},{:.2f}) mm",
                            current.type, current_freq, n, e, u);
                    }
                    else {
                        log_manager_.getLogger("product_parser")->warn("Invalid PCO line for {} {} at {}", current.type, current_freq, line_count);
                    }
                    continue;
                }

                // Ignore COMMENT metadata lines (including PCV set mapping table)
                if (line.find("COMMENT") != std::string::npos) {
                    continue;
                }

                std::string work = line;
                trim(work);
                if (work.empty()) continue;

                std::istringstream iss(work);
                std::vector<std::string> tokens;
                std::string t;
                while (iss >> t) tokens.push_back(t);
                if (tokens.empty()) continue;

                // NOAZI: values over zenith
                if (tokens[0] == "NOAZI") {
                    for (size_t i = 1; i < tokens.size(); ++i) {
                        try {
                            double v = std::stod(tokens[i]);
                            if (std::isfinite(v)) current_noazi.push_back(v);
                        }
                        catch (...) {}
                    }
                    continue;
                }

                // Azimuth row: first token is azimuth (deg), followed by values over zenith
                bool first_is_number = false;
                double first_val = 0.0;
                try {
                    first_val = std::stod(tokens[0]);
                    first_is_number = true;
                }
                catch (...) {
                    first_is_number = false;
                }

                if (first_is_number) {
                    std::vector<double> vals;
                    vals.reserve(tokens.size() - 1);
                    for (size_t i = 1; i < tokens.size(); ++i) {
                        try {
                            double v = std::stod(tokens[i]);
                            if (std::isfinite(v)) vals.push_back(v);
                        }
                        catch (...) {}
                    }
                    if (!vals.empty()) {
                        current_az_rows.emplace_back(first_val, std::move(vals));
                    }
                }
            }

            // File ended mid-antenna or mid-frequency
            if (in_freq) {
                finalize_frequency();
            }

            file.close();

            if (antennas.empty()) {
                log_manager_.getLogger("product_parser")->error("No antenna entries parsed from {}", atx_path.string());
                return false;
            }

            for (const auto& a : antennas) {
                for (const auto& kv : a.pco) {
                    if (!a.pcv.count(kv.first) || a.pcv.at(kv.first).empty()) {
                        log_manager_.getLogger("product_parser")->debug("Antenna {} freq {} has PCO but no PCV", a.type, kv.first);
                    }
                }
            }

            log_manager_.getLogger("product_parser")->info("Parsed {} antenna entries from {}", antennas.size(), atx_path.string());
            return true;
        }
        catch (const std::exception& e) {
            log_manager_.getLogger("product_parser")->error("ATX parse error {}: {}", atx_path.string(), e.what());
            return false;
        }
    }

    bool ProductParser::parseBia(const std::filesystem::path& bia_path, std::vector<BiasData>& biases) {
        try {
            std::ifstream file(bia_path);
            if (!file.is_open()) {
                log_manager_.getLogger("product_parser")->error("Failed to open BIA file: {}", bia_path.string());
                return false;
            }

            biases.clear();
            std::string line;
            size_t line_count = 0;
            bool in_solution_block = false;

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

                    int month = 1, day = doy;
                    bool is_leap = (year % 4 == 0 && (year % 100 != 0 || year % 400 == 0));
                    int days_in_month[] = { 31, is_leap ? 29 : 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
                    while (day > days_in_month[month - 1]) {
                        day -= days_in_month[month - 1];
                        ++month;
                        if (month > 12) {
                            return GnssTime{ 0, 0.0 };
                        }
                    }

                    int hour = static_cast<int>(seconds / 3600.0);
                    seconds -= hour * 3600.0;
                    int minute = static_cast<int>(seconds / 60.0);
                    seconds -= minute * 60.0;

                    if (!TimeUtils::isValidGregorianDate(year, month, day, hour, minute, seconds)) {
                        return GnssTime{ 0, 0.0 };
                    }

                    GnssTime gps_time = TimeUtils::gregorianToGpsTime(year, month, day, hour, minute, seconds);
                    return gps_time;
                }
                catch (const std::exception&) {
                    return GnssTime{ 0, 0.0 };
                }
                };

            while (std::getline(file, line)) {
                ++line_count;
                line.erase(0, line.find_first_not_of(" \t\r\n"));
                line.erase(line.find_last_not_of(" \t\r\n") + 1);
                if (line.empty() || line[0] == '*') {
                    continue;
                }

                if (line.find("+BIAS/SOLUTION") != std::string::npos) {
                    in_solution_block = true;
                    log_manager_.getLogger("product_parser")->debug("Started parsing BIAS/SOLUTION block at line {}", line_count);
                    continue;
                }
                else if (line.find("-BIAS/SOLUTION") != std::string::npos) {
                    in_solution_block = false;
                    log_manager_.getLogger("product_parser")->debug("Ended parsing BIAS/SOLUTION block at line {}", line_count);
                    break;
                }

                if (in_solution_block && line.length() >= 4 && line.substr(0, 4) == "OSB ") {
                    BiasData data;

                    std::string type = line.length() >= 4 ? line.substr(0, 4) : "";
                    std::string svn = line.length() >= 9 ? line.substr(5, 4) : "";
                    std::string prn = line.length() >= 13 ? line.substr(10, 3) : "";
                    std::string station = line.length() >= 23 ? line.substr(14, 9) : "";
                    std::string obs1 = line.length() >= 28 ? line.substr(24, 4) : "";
                    std::string obs2 = line.length() >= 33 ? line.substr(29, 4) : "";
                    std::string start = line.length() >= 48 ? line.substr(34, 14) : "";
                    std::string end = line.length() >= 63 ? line.substr(49, 14) : "";
                    std::string unit = line.length() >= 68 ? line.substr(64, 4) : "";
                    std::string bias_str = line.length() >= 90 ? line.substr(69, 21) : "";
                    std::string std_dev_str = line.length() >= 102 ? line.substr(91, 13) : "";

                    type.erase(type.find_last_not_of(" \t") + 1);
                    svn.erase(svn.find_last_not_of(" \t") + 1);
                    prn.erase(prn.find_last_not_of(" \t") + 1);
                    station.erase(station.find_last_not_of(" \t") + 1);
                    obs1.erase(obs1.find_last_not_of(" \t") + 1);
                    obs2.erase(obs2.find_last_not_of(" \t") + 1);
                    start.erase(start.find_last_not_of(" \t") + 1);
                    end.erase(end.find_last_not_of(" \t") + 1);
                    unit.erase(unit.find_last_not_of(" \t") + 1);
                    bias_str.erase(bias_str.find_last_not_of(" \t") + 1);
                    std_dev_str.erase(std_dev_str.find_last_not_of(" \t") + 1);

                    if (!station.empty()) {
                        continue;
                    }

                    if (type != "OSB" || prn.length() != 3 || obs1.empty() || start.empty() || end.empty() || unit.empty() || bias_str.empty()) {
                        log_manager_.getLogger("product_parser")->warn("Invalid OSB line at line {}: {}", line_count, line);
                        continue;
                    }

                    double bias, std_dev = 0;
                    try {
                        bias = std::stod(bias_str);
                        if (!std_dev_str.empty()) {
                            std_dev = std::stod(std_dev_str);
                        }
                    }
                    catch (const std::exception&) {
                        log_manager_.getLogger("product_parser")->warn("Invalid bias in OSB line at line {}: {}", line_count, line);
                        continue;
                    }
                    if (!std::isfinite(bias) || !std::isfinite(std_dev)) {
                        log_manager_.getLogger("product_parser")->warn("Non-finite bias or std_dev in OSB line at line {}: {}", line_count, line);
                        continue;
                    }

                    std::string signal = obs2.empty() ? obs1 : obs2;

                    if (prn.length() == 3 && (prn[0] == 'G' || prn[0] == 'R' || prn[0] == 'E' ||
                        prn[0] == 'C' || prn[0] == 'J' || prn[0] == 'S') &&
                        std::isdigit(prn[1]) && std::isdigit(prn[2])) {
                        data.satellite = prn;
                    }
                    else {
                        log_manager_.getLogger("product_parser")->warn("Invalid satellite PRN {} at line {}: {}", prn, line_count, line);
                        continue;
                    }

                    if (signal.length() == 3 && (signal[0] == 'C' || signal[0] == 'L' || signal[0] == 'D' || signal[0] == 'S') &&
                        std::isdigit(signal[1]) && std::isupper(signal[2])) {
                        data.signal = signal;
                    }
                    else {
                        log_manager_.getLogger("product_parser")->warn("Invalid observation code {} at line {}: {}", signal, line_count, line);
                        continue;
                    }

                    if (unit != "ns") {
                        log_manager_.getLogger("product_parser")->warn("Unexpected unit {} at line {}, expected ns: {}", unit, line_count, line);
                        continue;
                    }

                    data.bias = bias * Constants::SPEED_OF_LIGHT * 1e-9;
                    data.std_dev = std_dev * Constants::SPEED_OF_LIGHT * 1e-9;
                    data.start_time = parse_bias_time(start);
                    data.end_time = parse_bias_time(end);

                    if (data.start_time.week <= 0 || data.end_time.week <= 0 ||
                        data.start_time.tow < 0.0 || data.end_time.tow < 0.0) {
                        log_manager_.getLogger("product_parser")->warn("Invalid time range for satellite {} signal {} at line {}: start={}, end=",
                            data.satellite, data.signal, line_count, start, end);
                        continue;
                    }

                    biases.push_back(data);
                }
                else if (in_solution_block) {
                    log_manager_.getLogger("product_parser")->debug("Non-OSB line in BIAS/SOLUTION block at line {}: {}", line_count, line);
                }
            }

            file.close();
            if (biases.empty()) {
                log_manager_.getLogger("product_parser")->error("No valid bias data parsed from {}", bia_path.string());
                return false;
            }
            log_manager_.getLogger("product_parser")->info("Parsed {} bias entries from {}", biases.size(), bia_path.string());
            return true;
        }
        catch (const std::exception& e) {
            log_manager_.getLogger("product_parser")->error("Error parsing BIA file {}: {}", bia_path.string(), e.what());
            return false;
        }
    }

    bool ProductParser::parseErp(const std::filesystem::path& erp_path, std::vector<ErpData>& erps) {
        try {
            std::ifstream file(erp_path);
            if (!file.is_open()) {
                log_manager_.getLogger("product_parser")->error("Failed to open ERP file: {}", erp_path.string());
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
            log_manager_.getLogger("product_parser")->info("Parsed {} ERP entries from {}", erps.size(), erp_path.string());
            return !erps.empty();
        }
        catch (const std::exception& e) {
            log_manager_.getLogger("product_parser")->error("Error parsing ERP file {}: {}", erp_path.string(), e.what());
            return false;
        }
    }

    bool ProductParser::parseKlobuchar(const std::filesystem::path& brdc_path, std::vector<double>& klobuchar_coeffs) {
        try {
            std::ifstream file(brdc_path);
            if (!file.is_open()) {
                log_manager_.getLogger("product_parser")->error("Failed to open BRDC file for Klobuchar coefficients: {}", brdc_path.string());
                return false;
            }

            klobuchar_coeffs.clear();
            std::string line;
            size_t line_count = 0;
            bool in_header = true;

            while (std::getline(file, line)) {
                ++line_count;
                if (in_header && line.find("END OF HEADER") != std::string::npos) {
                    in_header = false;
                    continue;
                }
                if (in_header && line.find("IONOSPHERIC CORR") != std::string::npos) {
                    std::istringstream iss(line);
                    std::string type;
                    iss >> type;
                    if (type == "GPSA" || type == "GAL") {
                        double a0, a1, a2, a3;
                        if (iss >> a0 >> a1 >> a2 >> a3) {
                            if (std::isfinite(a0) && std::isfinite(a1) && std::isfinite(a2) && std::isfinite(a3)) {
                                klobuchar_coeffs.push_back(a0);
                                klobuchar_coeffs.push_back(a1);
                                klobuchar_coeffs.push_back(a2);
                                klobuchar_coeffs.push_back(a3);
                            }
                            else {
                                log_manager_.getLogger("product_parser")->warn("Non-finite Klobuchar alpha coefficients at line {}: {}", line_count, line);
                            }
                        }
                        else {
                            log_manager_.getLogger("product_parser")->warn("Invalid Klobuchar alpha coefficients at line {}: {}", line_count, line);
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
                                log_manager_.getLogger("product_parser")->warn("Non-finite Klobuchar beta coefficients at line {}: {}", line_count, line);
                            }
                        }
                        else {
                            log_manager_.getLogger("product_parser")->warn("Invalid Klobuchar beta coefficients at line {}: {}", line_count, line);
                        }
                    }
                }
            }

            file.close();
            if (klobuchar_coeffs.size() != 8) {
                log_manager_.getLogger("product_parser")->error("Incomplete Klobuchar coefficients from {}: expected 8, got {}",
                    brdc_path.string(), klobuchar_coeffs.size());
                klobuchar_coeffs.clear();
                return false;
            }
            log_manager_.getLogger("product_parser")->info("Parsed 8 Klobuchar coefficients from {}: alpha=[{:.3e}, {:.3e}, {:.3e}, {:.3e}], beta=[{:.3e}, {:.3e}, {:.3e}, {:.3e}]",
                brdc_path.string(), klobuchar_coeffs[0], klobuchar_coeffs[1], klobuchar_coeffs[2], klobuchar_coeffs[3],
                klobuchar_coeffs[4], klobuchar_coeffs[5], klobuchar_coeffs[6], klobuchar_coeffs[7]);
            return true;
        }
        catch (const std::exception& e) {
            log_manager_.getLogger("product_parser")->error("Exception parsing Klobuchar coefficients from {}: {}", brdc_path.string(), e.what());
            klobuchar_coeffs.clear();
            return false;
        }
    }

    void ProductParser::loadProducts() {
        auto sp3_paths = getOrbitPaths();
        if (!sp3_paths.empty()) {
            if (parseSp3(sp3_paths, orbits_)) {
                std::string paths_str = std::accumulate(sp3_paths.begin(), sp3_paths.end(), std::string(),
                    [](const std::string& a, const std::filesystem::path& p) {
                        return a.empty() ? p.string() : a + ", " + p.string();
                    });
                log_manager_.getLogger("ppp_model")->info("Loaded SP3 files: [{}]", paths_str);
            }
            else {
                log_manager_.getLogger("ppp_model")->error("Failed to load SP3 files: [{}]",
                    std::accumulate(sp3_paths.begin(), sp3_paths.end(), std::string(),
                        [](const std::string& a, const std::filesystem::path& p) {
                            return a.empty() ? p.string() : a + ", " + p.string();
                        }));
            }
        }
        else {
            log_manager_.getLogger("ppp_model")->warn("No SP3 file paths provided for orbit data");
        }

        std::filesystem::path clk_path = getProductPath("CLK");
        if (!clk_path.empty()) {
            if (parseClk(clk_path, clocks_)) {
                log_manager_.getLogger("ppp_model")->info("Loaded CLK file: {}", clk_path.string());
            }
            else {
                log_manager_.getLogger("ppp_model")->error("Failed to load CLK file: {}", clk_path.string());
            }
        }
        else {
            log_manager_.getLogger("ppp_model")->warn("No CLK file path provided");
        }

        std::filesystem::path atx_path = getProductPath("ATX");
        if (!atx_path.empty()) {
            if (parseAtx(atx_path, antennas_)) {
                log_manager_.getLogger("ppp_model")->info("Loaded ATX file: {}", atx_path.string());
            }
            else {
                log_manager_.getLogger("ppp_model")->error("Failed to load ATX file: {}", atx_path.string());
            }
        }
        else {
            log_manager_.getLogger("ppp_model")->warn("No ATX file path provided");
        }

        std::filesystem::path bia_path = getProductPath("BIA");
        if (!bia_path.empty()) {
            if (parseBia(bia_path, biases_)) {
                log_manager_.getLogger("ppp_model")->info("Loaded BIA file: {}", bia_path.string());
            }
            else {
                log_manager_.getLogger("ppp_model")->error("Failed to load BIA file: {}", bia_path.string());
            }
        }
        else {
            log_manager_.getLogger("ppp_model")->warn("No BIA file path provided");
        }

        std::filesystem::path erp_path = getProductPath("ERP");
        if (!erp_path.empty()) {
            if (parseErp(erp_path, erps_)) {
                log_manager_.getLogger("ppp_model")->info("Loaded ERP file: {}", erp_path.string());
            }
            else {
                log_manager_.getLogger("ppp_model")->error("Failed to load ERP file: {}", erp_path.string());
            }
        }
        else {
            log_manager_.getLogger("ppp_model")->warn("No ERP file path provided");
        }

        std::filesystem::path rnx_path = getProductPath("RNX");
        if (!rnx_path.empty()) {
            if (parseKlobuchar(rnx_path, klobuchar_coeffs_)) {
                log_manager_.getLogger("ppp_model")->info("Loaded Klobuchar coefficients from RNX file: {}, coeffs size: {}",
                    rnx_path.string(), klobuchar_coeffs_.size());
            }
            else {
                log_manager_.getLogger("ppp_model")->error("Failed to load Klobuchar coefficients from RNX file: {}", rnx_path.string());
            }
        }
        else {
            log_manager_.getLogger("ppp_model")->warn("No RNX file path provided for Klobuchar or Sun ephemeris");
        }

        std::vector<GnssTime> epoch_times = rinex_parser_->getEpochTimes();
        if (planetary_.parseSunEphemeris(epoch_times, sun_ephemeris_)) {
            log_manager_.getLogger("ppp_model")->info("Computed Sun ephemeris for {} observation epochs", sun_ephemeris_.size());
        }
        else {
            log_manager_.getLogger("ppp_model")->error("Failed to compute Sun ephemeris for observation epochs");
        }
    }

} // namespace GnssProcessor
//#endif