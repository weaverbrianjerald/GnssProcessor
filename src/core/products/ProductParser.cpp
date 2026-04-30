/*!
 * \file ProductParser.cpp
 * \brief GNSS product file parsers (SP3, CLK, ATX, BIA, ERP, Klobuchar).
 *
 * Organization:
 *  - Shared utilities
 *  - Construction / configuration
 *  - Path resolution
 *  - SP3 orbit parsing
 *  - CLK clock parsing
 *  - ATX antenna parsing
 *  - BIA bias parsing
 *  - ERP parsing
 *  - Klobuchar parsing
 *  - Product loading orchestration
 */

#include "products/ProductParser.hpp"
#include "common/Constants.hpp"
#include <charconv>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <netcdf.h>
#include <algorithm>
#include <iomanip>
#include <regex>
#include <string_view>

namespace GnssProcessor {

    // =================================================================
    // Fast text-parsing helpers (anonymous namespace, file-local)
    // =================================================================
    //
    // The hot paths in parseSp3 / parseClk used to iterate every record
    // through std::istringstream and operator>>, which on MSVC pays the
    // locale + stream-buffer overhead per token (it dominated startup
    // cost for daily SP3+CLK product loads).  These helpers replace that
    // with std::from_chars over a std::string_view of the line, skipping
    // ASCII whitespace and advancing the view past each parsed field.
    //
    // std::from_chars avoids the C/C++ locale entirely, has no
    // allocations, and on MSVC2019+ supports double parsing.
    namespace {

        inline void skipWs(std::string_view& sv) {
            size_t i = 0;
            while (i < sv.size()
                   && (sv[i] == ' ' || sv[i] == '\t')) ++i;
            sv.remove_prefix(i);
        }

        inline bool parseInt(std::string_view& sv, int& out) {
            skipWs(sv);
            if (sv.empty()) return false;
            const char* first = sv.data();
            const char* last  = first + sv.size();
            auto [ptr, ec] = std::from_chars(first, last, out);
            if (ec != std::errc{}) return false;
            sv.remove_prefix(static_cast<size_t>(ptr - first));
            return true;
        }

        inline bool parseDouble(std::string_view& sv, double& out) {
            skipWs(sv);
            if (sv.empty()) return false;
            const char* first = sv.data();
            const char* last  = first + sv.size();
            auto [ptr, ec] = std::from_chars(first, last, out);
            if (ec != std::errc{}) return false;
            sv.remove_prefix(static_cast<size_t>(ptr - first));
            return true;
        }

        inline bool parseToken(std::string_view& sv, std::string_view& out) {
            skipWs(sv);
            if (sv.empty()) return false;
            size_t i = 0;
            while (i < sv.size()
                   && sv[i] != ' ' && sv[i] != '\t') ++i;
            out = sv.substr(0, i);
            sv.remove_prefix(i);
            return !out.empty();
        }

    } // namespace

    // =================================================================
    // Shared utilities
    // =================================================================

    std::string ProductParser::sanitize(const std::string& s) {
        std::string out;
        out.reserve(s.size());
        for (char c : s)
            if (!std::isspace(static_cast<unsigned char>(c))) out.push_back(c);
        return out;
    }

    void ProductParser::rtrim(std::string& s) {
        auto pos = s.find_last_not_of(" \t\r\n");
        s.erase(pos == std::string::npos ? 0 : pos + 1);
    }

    bool ProductParser::isValidPrn(const std::string& prn) {
        return prn.size() == 3 &&
            (prn[0] == 'G' || prn[0] == 'R' || prn[0] == 'E' ||
             prn[0] == 'C' || prn[0] == 'J' || prn[0] == 'S') &&
            std::isdigit(static_cast<unsigned char>(prn[1])) &&
            std::isdigit(static_cast<unsigned char>(prn[2]));
    }

    std::string ProductParser::joinPaths(const std::vector<std::filesystem::path>& paths) {
        std::string result;
        for (const auto& p : paths) {
            if (!result.empty()) result += ", ";
            result += p.string();
        }
        return result;
    }

    // =================================================================
    // Construction / configuration
    // =================================================================

    ProductParser::ProductParser(const GnssConfig& config, LogManager& log_manager)
        : config_(config), log_manager_(log_manager), rinex_parser_(nullptr), planetary_(log_manager) {
        log_manager_.getLogger("product_parser")->info("ProductParser initialized.");
    }

    void ProductParser::setRinexParser(RinexParser& rinex) {
        rinex_parser_ = &rinex;
        log_manager_.getLogger("product_parser")->trace("RinexParser set for ProductParser");
    }

    // =================================================================
    // Path resolution
    // =================================================================

    std::vector<std::filesystem::path> ProductParser::getPathsByExtension(const std::string& ext) const {
        std::vector<std::filesystem::path> paths;
        for (const auto& path : config_.getProductPaths())
            if (path.extension() == ext && std::filesystem::exists(path))
                paths.push_back(path);
        return paths;
    }

    std::vector<std::filesystem::path> ProductParser::getOrbitPaths() const {
        auto paths = getPathsByExtension(".SP3");
        log_manager_.getLogger("product_parser")->info("Retrieved {} SP3 file paths", paths.size());
        return paths;
    }

    std::vector<std::filesystem::path> ProductParser::getClockPaths() const {
        auto paths = getPathsByExtension(".CLK");
        log_manager_.getLogger("product_parser")->info("Retrieved {} CLK file paths", paths.size());
        return paths;
    }

    std::filesystem::path ProductParser::getProductPath(const std::string& type) const {
        auto path = config_.getProductPath(type);
        if (!path.empty() && std::filesystem::exists(path)) {
            log_manager_.getLogger("product_parser")->trace("Retrieved {} path: {}", type, path.string());
            return path;
        }
        log_manager_.getLogger("product_parser")->warn("Product path for {} not found or does not exist: {}", type, path.string());
        return {};
    }

    // =================================================================
    // SP3 orbit parsing
    // =================================================================

    bool ProductParser::parseSp3(const std::vector<std::filesystem::path>& sp3_paths, std::vector<OrbitData>& orbits) {
        try {
            orbits.clear();
            std::map<std::string, std::map<GnssTime, OrbitData>> orbit_map;
            constexpr double MIN_TIME_DIFF = 1.0;
            size_t total_entries = 0;
            auto logger = log_manager_.getLogger("product_parser");

            for (const auto& sp3_path : sp3_paths) {
                std::ifstream file(sp3_path);
                if (!file.is_open()) {
                    logger->error("Failed to open SP3 file: {}", sp3_path.string());
                    continue;
                }

                std::string line;
                bool in_header = true;
                int epoch_interval = 300;
                std::vector<std::string> satellite_ids;
                size_t line_count = 0;

                // --- SP3 header ---
                while (std::getline(file, line) && in_header) {
                    ++line_count;
                    if (line.empty()) continue;

                    if (line[0] == '#') {
                        if (line.size() >= 12 && (line[1] == 'c' || line[1] == 'd'))
                            logger->debug("Parsing SP3 header at line {}: {} in {}", line_count, line, sp3_path.string());
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
                            logger->debug("Parsed SP3 epoch interval: {:.3f} seconds at line {} in {}", interval, line_count, sp3_path.string());
                        } else {
                            logger->warn("Invalid or out-of-range epoch interval at line {} in {}", line_count, sp3_path.string());
                            epoch_interval = 300;
                            logger->info("Using default SP3 epoch interval: {} seconds for {}", epoch_interval, sp3_path.string());
                        }
                    }
                    else if (line.substr(0, 2) == "+ ") {
                        int num_sats = 0;
                        try { num_sats = std::stoi(line.substr(3, 3)); }
                        catch (const std::exception& e) {
                            logger->warn("Invalid satellite count at line {} in {}: {}", line_count, sp3_path.string(), e.what());
                            continue;
                        }
                        satellite_ids.clear();
                        std::string header_line = line;
                        while (header_line.substr(0, 2) == "+ ") {
                            for (size_t i = 3; i < header_line.length() && i < 60; i += 3) {
                                std::string sat_id = header_line.substr(i, 3);
                                sat_id.erase(std::remove_if(sat_id.begin(), sat_id.end(), ::isspace), sat_id.end());
                                if (isValidPrn(sat_id))
                                    satellite_ids.push_back(sat_id);
                            }
                            if (!std::getline(file, header_line)) {
                                logger->error("Unexpected end of file while reading SP3 satellite IDs at line {} in {}", line_count, sp3_path.string());
                                file.close();
                                continue;
                            }
                            ++line_count;
                        }
                        while (header_line.substr(0, 2) == "++") {
                            if (!std::getline(file, header_line)) {
                                logger->error("Unexpected end of file while reading SP3 accuracy codes at line {} in {}", line_count, sp3_path.string());
                                file.close();
                                continue;
                            }
                            ++line_count;
                        }
                        if (static_cast<int>(satellite_ids.size()) != num_sats)
                            logger->warn("Mismatch in satellite count: expected {}, found {} at line {} in {}",
                                num_sats, satellite_ids.size(), line_count, sp3_path.string());
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

                // --- SP3 data records ---
                bool have_line = !in_header && !line.empty();
                while (have_line || std::getline(file, line)) {
                    if (!have_line) ++line_count;
                    have_line = false;
                    if (line.empty()) continue;

                    if (line[0] == '*') {
                        // SP3 epoch line: "*  YYYY MM DD HH MM SS.SSSSSSSS"
                        std::string_view sv(line);
                        sv.remove_prefix(1);  // skip leading '*'
                        int year, month, day, hour, min;
                        double sec;
                        if (!parseInt(sv, year) || !parseInt(sv, month)
                            || !parseInt(sv, day)  || !parseInt(sv, hour)
                            || !parseInt(sv, min)  || !parseDouble(sv, sec)) {
                            logger->warn("Invalid epoch line {} in {}: {}", line_count, sp3_path.string(), line);
                            continue;
                        }
                        GnssTime epoch_time = TimeUtils::gregorianGpsToGpsTime(year, month, day, hour, min, sec);
                        if (epoch_time.week < 0 || epoch_time.tow < 0 || epoch_time.tow >= TimeUtils::SECONDS_IN_WEEK) {
                            logger->warn("Invalid GPS time at line {} in {}: week {}, tow {}", line_count, sp3_path.string(), epoch_time.week, epoch_time.tow);
                            continue;
                        }

                        while (std::getline(file, line)) {
                            ++line_count;
                            if (line.empty() || line[0] == '*' || line[0] == 'E') {
                                have_line = true;
                                break;
                            }
                            if (line[0] == 'P') {
                                OrbitData data;
                                data.satellite = line.substr(1, 3);
                                if (std::find(satellite_ids.begin(), satellite_ids.end(), data.satellite) == satellite_ids.end()) {
                                    logger->warn("Unknown satellite ID {} at line {} in {}", data.satellite, line_count, sp3_path.string());
                                    continue;
                                }
                                data.time = epoch_time;
                                data.position_valid = true;
                                data.velocity_valid = false;
                                data.position = Eigen::Vector3d::Zero();
                                data.velocity = Eigen::Vector3d::Zero();

                                // SP3 P record fields start at column 4: "PG02  XXXX YYYY ZZZZ CCCC"
                                std::string_view psv(line.data() + 4, line.size() - 4);
                                double x, y, z, clock_bias;
                                if (!parseDouble(psv, x) || !parseDouble(psv, y)
                                    || !parseDouble(psv, z) || !parseDouble(psv, clock_bias) ||
                                    !std::isfinite(x) || !std::isfinite(y) || !std::isfinite(z) ||
                                    std::abs(x) > 1e6 || std::abs(y) > 1e6 || std::abs(z) > 1e6) {
                                    logger->warn("Invalid position data for {} at line {} in {}: x={}, y={}, z={}",
                                        data.satellite, line_count, sp3_path.string(), x, y, z);
                                    data.position_valid = false;
                                } else {
                                    data.position = Eigen::Vector3d(x * 1000.0, y * 1000.0, z * 1000.0);
                                }

                                auto& sat_orbits = orbit_map[data.satellite];
                                bool insert = true;
                                for (const auto& [existing_time, existing_data] : sat_orbits) {
                                    if (std::abs(TimeUtils::continuousGpsSeconds(epoch_time) - TimeUtils::continuousGpsSeconds(existing_time)) < MIN_TIME_DIFF) {
                                        insert = false;
                                        logger->warn("Duplicate epoch for {} at week {} TOW {:.3f} in {}, skipping",
                                            data.satellite, epoch_time.week, epoch_time.tow, sp3_path.string());
                                        break;
                                    }
                                }
                                if (insert && data.position_valid) {
                                    sat_orbits[epoch_time] = data;
                                    ++total_entries;
                                }
                            }
                            else if (line[0] == 'V') {
                                std::string sat_id = line.substr(1, 3);
                                auto& sat_orbits = orbit_map[sat_id];
                                auto it = sat_orbits.find(epoch_time);
                                if (it != sat_orbits.end()) {
                                    // SP3 V record fields also start at column 4.
                                    std::string_view vsv(line.data() + 4, line.size() - 4);
                                    double vx, vy, vz, clock_rate;
                                    if (!parseDouble(vsv, vx) || !parseDouble(vsv, vy)
                                        || !parseDouble(vsv, vz) || !parseDouble(vsv, clock_rate) ||
                                        !std::isfinite(vx) || !std::isfinite(vy) || !std::isfinite(vz) ||
                                        std::abs(vx) > 1e6 || std::abs(vy) > 1e6 || std::abs(vz) > 1e6) {
                                        logger->warn("Invalid velocity data for {} at line {} in {}: vx={}, vy={}, vz={}",
                                            sat_id, line_count, sp3_path.string(), vx, vy, vz);
                                    } else {
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
                logger->info("Parsed {} orbit entries from {}", total_entries, sp3_path.string());
                total_entries = 0;
            }

            orbits.clear();
            for (const auto& [sat, time_map] : orbit_map)
                for (const auto& [time, data] : time_map)
                    orbits.push_back(data);

            if (orbits.empty()) {
                logger->error("No valid orbit data parsed from provided SP3 files");
                return false;
            }
            logger->info("Merged {} orbit entries from {} SP3 files", orbits.size(), sp3_paths.size());
            return true;
        }
        catch (const std::exception& e) {
            log_manager_.getLogger("product_parser")->error("Error parsing SP3 files: {}", e.what());
            return false;
        }
    }

    // =================================================================
    // CLK clock parsing
    // =================================================================

    bool ProductParser::parseClk(const std::vector<std::filesystem::path>& clk_paths, std::vector<ClockData>& clocks) {
        try {
            clocks.clear();
            constexpr double MIN_TIME_DIFF = 1.0;
            std::map<std::string, std::map<double, ClockData>> clock_map;
            size_t total_entries = 0;
            auto logger = log_manager_.getLogger("product_parser");

            for (const auto& clk_path : clk_paths) {
                std::ifstream file(clk_path);
                if (!file.is_open()) {
                    logger->error("Failed to open CLK file: {}", clk_path.string());
                    continue;
                }

                std::string line;
                size_t line_count = 0;
                bool in_header = true;
                double epoch_interval = 0.0;

                while (std::getline(file, line) && in_header) {
                    ++line_count;
                    if (line.empty()) continue;
                    if (line.find("END OF HEADER") != std::string::npos) { in_header = false; continue; }
                    if (line.find("INTERVAL") != std::string::npos) {
                        std::istringstream iss(line);
                        double interval_val;
                        if (iss >> interval_val) {
                            epoch_interval = interval_val;
                            logger->debug("CLK epoch interval: {} seconds at line {} in {}", epoch_interval, line_count, clk_path.string());
                        } else {
                            logger->warn("Invalid INTERVAL line {} in {}: {}", line_count, clk_path.string(), line);
                        }
                    }
                }

                size_t file_entries = 0;
                while (std::getline(file, line)) {
                    ++line_count;
                    if (line.empty() || line.compare(0, 3, "AS ") != 0) continue;

                    // Skip "AS " prefix and parse the rest with from_chars.
                    std::string_view sv(line);
                    sv.remove_prefix(3);
                    std::string_view sat_sv;
                    int year, month, day, hour, min, num;
                    double sec, clock_bias;
                    if (!parseToken(sv, sat_sv)
                        || !parseInt(sv, year)  || !parseInt(sv, month)
                        || !parseInt(sv, day)   || !parseInt(sv, hour)
                        || !parseInt(sv, min)   || !parseDouble(sv, sec)
                        || !parseInt(sv, num)   || !parseDouble(sv, clock_bias)) {
                        logger->warn("Invalid AS record at line {} in {}: {}", line_count, clk_path.string(), line);
                        continue;
                    }
                    std::string sat(sat_sv);

                    if (!isValidPrn(sat)) {
                        logger->warn("Invalid satellite ID {} at line {} in {}", sat, line_count, clk_path.string());
                        continue;
                    }

                    GnssTime epoch_time = TimeUtils::gregorianGpsToGpsTime(year, month, day, hour, min, sec);
                    if (epoch_time.week < 0 || epoch_time.tow < 0 || epoch_time.tow >= TimeUtils::SECONDS_IN_WEEK) {
                        logger->warn("Invalid GPS time at line {} in {}: week {}, tow {}", line_count, clk_path.string(), epoch_time.week, epoch_time.tow);
                        continue;
                    }

                    const double t_cont = TimeUtils::continuousGpsSeconds(epoch_time);
                    auto& sat_clocks = clock_map[sat];
                    auto lb = sat_clocks.lower_bound(t_cont - MIN_TIME_DIFF);
                    if (lb != sat_clocks.end() && std::abs(lb->first - t_cont) < MIN_TIME_DIFF)
                        continue;

                    ClockData data;
                    data.satellite = sat;
                    data.time = epoch_time;
                    data.start_time = epoch_time;
                    data.end_time = GnssTime{ epoch_time.week, epoch_time.tow + epoch_interval };
                    data.clock_bias = clock_bias;
                    sat_clocks[t_cont] = data;
                    ++file_entries;
                }

                file.close();
                total_entries += file_entries;
                logger->info("Parsed {} clock entries from {}", file_entries, clk_path.string());
            }

            clocks.clear();
            clocks.reserve(total_entries);
            for (const auto& [sat, time_map] : clock_map)
                for (const auto& [t, data] : time_map)
                    clocks.push_back(data);

            if (clocks.empty()) {
                logger->error("No valid clock data parsed from provided CLK files");
                return false;
            }
            logger->info("Merged {} clock entries from {} CLK files", clocks.size(), clk_paths.size());
            return true;
        }
        catch (const std::exception& e) {
            log_manager_.getLogger("product_parser")->error("Error parsing CLK files: {}", e.what());
            return false;
        }
    }

    // =================================================================
    // ATX antenna parsing
    // =================================================================

    bool ProductParser::parseAtx(const std::filesystem::path& atx_path, std::vector<AntennaData>& antennas) {
        try {
            std::ifstream file(atx_path);
            if (!file.is_open()) {
                log_manager_.getLogger("product_parser")->error("Failed to open ATX file: {}", atx_path.string());
                return false;
            }

            antennas.clear();
            auto logger = log_manager_.getLogger("product_parser");

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

            auto parse_valid_line = [&](const std::string& src) -> GnssTime {
                if (src.find("VALID FROM") != std::string::npos || src.find("VALID UNTIL") != std::string::npos) {
                    std::istringstream iss(src.substr(0, 50));
                    int year, month, day, hour, minute;
                    double second;
                    if (iss >> year >> month >> day >> hour >> minute >> second) {
                        if (year >= 1980 && month >= 1 && month <= 12 && day >= 1 && day <= 31 &&
                            hour >= 0 && hour <= 23 && minute >= 0 && minute <= 59 && second >= 0.0 && second < 61.0) {
                            try { return TimeUtils::gregorianGpsToGpsTime(year, month, day, hour, minute, second); }
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
                        try { return TimeUtils::gregorianGpsToGpsTime(year, month, day, hour, minute, double(second)); }
                        catch (...) { return GnssTime{ 0,0.0 }; }
                    }
                }
                return GnssTime{ 0,0.0 };
            };

            std::regex freqCodeRegex(R"(([GREJCIS]\d{2}))", std::regex_constants::icase);

            // Per-frequency PCV accumulators
            std::vector<double> current_noazi;
            std::vector<std::pair<double, std::vector<double>>> current_az_rows;

            auto reset_freq_accumulators = [&]() {
                current_noazi.clear();
                current_az_rows.clear();
            };

            auto finalize_frequency = [&]() {
                if (current_freq.empty()) return;

                if (!current_az_rows.empty()) {
                    const size_t az_count = current_az_rows.size();
                    const size_t zen_count = current_az_rows.front().second.size();
                    if (az_count == 0 || zen_count == 0) {
                        current.pcv[current_freq].clear();
                        reset_freq_accumulators();
                        return;
                    }
                    for (const auto& [azi, vals] : current_az_rows)
                        if (vals.size() != zen_count)
                            logger->warn("Inconsistent PCV row length for {} {}: expected {}, got {} (az={:.1f})",
                                current.type, current_freq, zen_count, vals.size(), azi);

                    std::vector<std::vector<double>> pcv_matrix(zen_count, std::vector<double>(az_count, 0.0));
                    for (size_t a = 0; a < az_count; ++a) {
                        const auto& vals = current_az_rows[a].second;
                        for (size_t z = 0; z < std::min(vals.size(), zen_count); ++z)
                            pcv_matrix[z][a] = vals[z];
                    }
                    current.pcv[current_freq] = std::move(pcv_matrix);
                    logger->trace("Finalized AZI PCV for {} {}: zen={}, az={}", current.type, current_freq, zen_count, az_count);
                }
                else if (!current_noazi.empty()) {
                    std::vector<std::vector<double>> pcv_matrix;
                    pcv_matrix.reserve(current_noazi.size());
                    for (double v : current_noazi)
                        pcv_matrix.push_back({ v });
                    current.pcv[current_freq] = std::move(pcv_matrix);
                    logger->trace("Finalized NOAZI PCV for {} {}: zen_samples={}", current.type, current_freq, current_noazi.size());
                }
                else {
                    logger->debug("No PCV data for {} {}", current.type, current_freq);
                }

                reset_freq_accumulators();
                current_freq.clear();
            };

            while (std::getline(file, line)) {
                ++line_count;
                if (line.empty()) continue;

                // --- Block delimiters ---
                if (line.find("START OF ANTENNA") != std::string::npos) {
                    in_antenna = true;
                    in_freq = false;
                    current = AntennaData();
                    current_freq.clear();
                    reset_freq_accumulators();
                    continue;
                }
                if (line.find("END OF ANTENNA") != std::string::npos) {
                    if (in_freq) finalize_frequency();
                    if (in_antenna && !current.type.empty()) {
                        antennas.push_back(current);
                        logger->trace("Stored antenna {} (valid_from week={}, tow={:.3f}, valid_until week={}, tow={:.3f})",
                            current.type, current.valid_from.week, current.valid_from.tow,
                            current.valid_until.week, current.valid_until.tow);
                    }
                    in_antenna = false;
                    in_freq = false;
                    continue;
                }

                if (!in_antenna) continue;

                // --- TYPE / SERIAL NO ---
                if (line.find("TYPE / SERIAL NO") != std::string::npos) {
                    std::string field1 = line.substr(0, 20);
                    std::string field2 = (line.size() >= 40) ? line.substr(20, 20) : "";
                    std::string field3 = (line.size() >= 60) ? line.substr(40, 20) : "";
                    trim(field1);
                    trim(field2);
                    trim(field3);

                    // Find PRN (3-char) in field2 and SVN (4-char) in field3
                    std::string prn_token, svn_token;

                    {
                        std::istringstream tss(field2);
                        std::string tok;
                        while (tss >> tok)
                            if (isValidPrn(tok)) prn_token = tok;
                    }
                    if (!field3.empty()) {
                        std::istringstream tss(field3);
                        std::string tok;
                        while (tss >> tok) {
                            if (tok.size() == 4 &&
                                (tok[0] == 'G' || tok[0] == 'R' || tok[0] == 'E' ||
                                 tok[0] == 'C' || tok[0] == 'J' || tok[0] == 'S') &&
                                std::isdigit(tok[1]) && std::isdigit(tok[2]) && std::isdigit(tok[3])) {
                                svn_token = tok;
                                break;
                            }
                        }
                    }

                    if (!prn_token.empty()) {
                        std::string block_norm = sanitize(field1);
                        std::string svn_norm = sanitize(svn_token);
                        current.type = !svn_norm.empty()
                            ? prn_token + "_" + block_norm + "_" + svn_norm
                            : prn_token + "_" + block_norm;
                        logger->debug("Parsed satellite antenna: PRN={}, Block={}, SVN={} -> type='{}'",
                            prn_token, field1, svn_token, current.type);
                    } else {
                        std::string model = field1;
                        std::string serial = field2;
                        if (serial.empty()) {
                            std::istringstream iss(field1);
                            std::vector<std::string> parts;
                            std::string p;
                            while (iss >> p) parts.push_back(p);
                            if (parts.size() >= 2) { model = parts.front(); serial = parts.back(); }
                        }
                        model = sanitize(model);
                        serial = sanitize(serial);
                        current.type = serial.empty() ? model : (model + "_" + serial);
                    }
                    logger->trace("Parsed antenna type '{}'", current.type);
                    continue;
                }

                // --- Validity ---
                if (line.find("VALID FROM") != std::string::npos) {
                    current.valid_from = parse_valid_line(line);
                    if (current.valid_from.week == 0)
                        logger->warn("Failed to parse VALID FROM at line {}", line_count);
                    continue;
                }
                if (line.find("VALID UNTIL") != std::string::npos) {
                    current.valid_until = parse_valid_line(line);
                    if (current.valid_until.week == 0)
                        logger->warn("Failed to parse VALID UNTIL at line {}", line_count);
                    continue;
                }

                // --- DAZI ---
                if (line.find("DAZI") != std::string::npos) {
                    std::string_view sv(line);
                    double dazi;
                    if (parseDouble(sv, dazi) && std::isfinite(dazi)) current.dazi = dazi;
                    continue;
                }

                // --- ZEN1 / ZEN2 / DZEN ---
                if (line.find("ZEN1 / ZEN2 / DZEN") != std::string::npos) {
                    std::string_view sv(line);
                    double zen1, zen2, dzen;
                    if (parseDouble(sv, zen1) && parseDouble(sv, zen2) && parseDouble(sv, dzen)
                        && std::isfinite(zen1) && std::isfinite(zen2) && std::isfinite(dzen)) {
                        current.zen1 = zen1;
                        current.dzen = dzen;
                    }
                    continue;
                }

                // --- Frequency delimiters ---
                if (line.find("START OF FREQUENCY") != std::string::npos) {
                    if (in_freq) finalize_frequency();
                    in_freq = true;
                    reset_freq_accumulators();
                    std::smatch m;
                    if (std::regex_search(line, m, freqCodeRegex))
                        current_freq = sanitize(m.str());
                    if (current_freq.empty()) {
                        logger->warn("Could not detect frequency code at line {}", line_count);
                        in_freq = false;
                        continue;
                    }
                    current.pcv[current_freq] = {};
                    continue;
                }
                if (line.find("END OF FREQUENCY") != std::string::npos) {
                    if (in_freq) finalize_frequency();
                    in_freq = false;
                    continue;
                }

                if (!in_freq || current_freq.empty()) continue;

                // --- PCO ---
                if (line.find("NORTH / EAST / UP") != std::string::npos) {
                    const size_t header_pos = line.find("NORTH");
                    std::string_view sv(line.data(), header_pos);
                    double n, e, u;
                    if (parseDouble(sv, n) && parseDouble(sv, e) && parseDouble(sv, u)
                        && std::isfinite(n) && std::isfinite(e) && std::isfinite(u)) {
                        current.pco[current_freq] = Eigen::Vector3d(n, e, u);
                        logger->trace("PCO {} {}: N/E/U=({:.2f},{:.2f},{:.2f}) mm", current.type, current_freq, n, e, u);
                    } else {
                        logger->warn("Invalid PCO line for {} {} at {}", current.type, current_freq, line_count);
                    }
                    continue;
                }

                if (line.find("COMMENT") != std::string::npos) continue;

                // --- PCV data rows ---
                // Hot loop: ~thousands of rows per antenna × frequency,
                // millions per ATX file.  Replaced istringstream tokenize +
                // try/catch stod with from_chars over a string_view —
                // ~5–10× faster on MSVC (no locale, no exception SEH frames).
                std::string_view sv(line);
                skipWs(sv);
                if (sv.empty()) continue;

                // Probe the first token as a string view (no copy).
                std::string_view first_tok;
                {
                    size_t i = 0;
                    while (i < sv.size() && sv[i] != ' ' && sv[i] != '\t') ++i;
                    first_tok = sv.substr(0, i);
                    sv.remove_prefix(i);
                }

                if (first_tok == "NOAZI") {
                    // Remaining doubles → NOAZI table.
                    while (true) {
                        skipWs(sv);
                        if (sv.empty()) break;
                        const char* fp = sv.data();
                        const char* lp = fp + sv.size();
                        double v;
                        auto [ptr, ec] = std::from_chars(fp, lp, v);
                        if (ec != std::errc{}) break;
                        if (std::isfinite(v)) current_noazi.push_back(v);
                        sv.remove_prefix(static_cast<size_t>(ptr - fp));
                    }
                    continue;
                }

                // Azimuth row: try to parse first_tok as the azimuth.
                double first_val;
                {
                    const char* fp = first_tok.data();
                    const char* lp = fp + first_tok.size();
                    auto [ptr, ec] = std::from_chars(fp, lp, first_val);
                    if (ec != std::errc{} || !std::isfinite(first_val)) continue;
                }
                std::vector<double> vals;
                vals.reserve(64);
                while (true) {
                    skipWs(sv);
                    if (sv.empty()) break;
                    const char* fp = sv.data();
                    const char* lp = fp + sv.size();
                    double v;
                    auto [ptr, ec] = std::from_chars(fp, lp, v);
                    if (ec != std::errc{}) break;
                    if (std::isfinite(v)) vals.push_back(v);
                    sv.remove_prefix(static_cast<size_t>(ptr - fp));
                }
                if (!vals.empty())
                    current_az_rows.emplace_back(first_val, std::move(vals));
            }

            if (in_freq) finalize_frequency();
            file.close();

            if (antennas.empty()) {
                logger->error("No antenna entries parsed from {}", atx_path.string());
                return false;
            }
            for (const auto& a : antennas)
                for (const auto& kv : a.pco)
                    if (!a.pcv.count(kv.first) || a.pcv.at(kv.first).empty())
                        logger->debug("Antenna {} freq {} has PCO but no PCV", a.type, kv.first);

            logger->info("Parsed {} antenna entries from {}", antennas.size(), atx_path.string());
            return true;
        }
        catch (const std::exception& e) {
            log_manager_.getLogger("product_parser")->error("ATX parse error {}: {}", atx_path.string(), e.what());
            return false;
        }
    }

    // =================================================================
    // BIA bias parsing
    // =================================================================

    bool ProductParser::parseBia(const std::filesystem::path& bia_path, std::vector<BiasData>& biases,
                                ClockRefMap& clock_refs) {
        try {
            std::ifstream file(bia_path);
            if (!file.is_open()) {
                log_manager_.getLogger("product_parser")->error("Failed to open BIA file: {}", bia_path.string());
                return false;
            }

            biases.clear();
            clock_refs.clear();
            auto logger = log_manager_.getLogger("product_parser");
            std::string line;
            size_t line_count = 0;
            bool in_description_block = false;
            bool in_solution_block = false;

            auto parse_bias_time = [](const std::string& time_str) -> GnssTime {
                if (time_str.length() < 14 || time_str[4] != ':' || time_str[8] != ':')
                    return GnssTime{ 0, 0.0 };
                try {
                    int year = std::stoi(time_str.substr(0, 4));
                    int doy = std::stoi(time_str.substr(5, 3));
                    double seconds = std::stod(time_str.substr(9));
                    if (year < 1980 || doy < 1 || doy > 366 || seconds < 0.0 || seconds >= TimeUtils::SECONDS_IN_DAY)
                        return GnssTime{ 0, 0.0 };

                    int month = 1, day = doy;
                    bool is_leap = (year % 4 == 0 && (year % 100 != 0 || year % 400 == 0));
                    int days_in_month[] = { 31, is_leap ? 29 : 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
                    while (day > days_in_month[month - 1]) {
                        day -= days_in_month[month - 1];
                        if (++month > 12) return GnssTime{ 0, 0.0 };
                    }

                    int hour = static_cast<int>(seconds / 3600.0);
                    seconds -= hour * 3600.0;
                    int minute = static_cast<int>(seconds / 60.0);
                    seconds -= minute * 60.0;

                    if (!TimeUtils::isValidGregorianDate(year, month, day, hour, minute, seconds))
                        return GnssTime{ 0, 0.0 };
                    return TimeUtils::gregorianGpsToGpsTime(year, month, day, hour, minute, seconds);
                }
                catch (const std::exception&) { return GnssTime{ 0, 0.0 }; }
            };

            while (std::getline(file, line)) {
                ++line_count;
                line.erase(0, line.find_first_not_of(" \t\r\n"));
                line.erase(line.find_last_not_of(" \t\r\n") + 1);
                if (line.empty() || line[0] == '*') continue;

                // --- BIAS/DESCRIPTION block: extract clock reference observables ---
                if (line.find("+BIAS/DESCRIPTION") != std::string::npos) {
                    in_description_block = true;
                    continue;
                }
                if (line.find("-BIAS/DESCRIPTION") != std::string::npos) {
                    in_description_block = false;
                    continue;
                }
                if (in_description_block) {
                    // SATELLITE_CLOCK_REFERENCE_OBSERVABLES   G  C1W  C2W
                    if (line.find("SATELLITE_CLOCK_REFERENCE_OBSERVABLES") != std::string::npos) {
                        // Tokenize everything after the keyword
                        const auto kw_end = line.find("SATELLITE_CLOCK_REFERENCE_OBSERVABLES")
                                          + std::string("SATELLITE_CLOCK_REFERENCE_OBSERVABLES").size();
                        std::istringstream iss(line.substr(kw_end));
                        std::string sys_char, s1, s2;
                        if (iss >> sys_char >> s1 >> s2) {
                            if (sys_char.size() == 1 && s1.size() == 3 && s2.size() == 3) {
                                clock_refs[sys_char[0]] = ClockRefSignals{ s1, s2 };
                                logger->info("BIA clock reference: system={} sig1={} sig2={}",
                                    sys_char, s1, s2);
                            }
                        }
                    }
                    continue;
                }

                // --- BIAS/SOLUTION block ---
                if (line.find("+BIAS/SOLUTION") != std::string::npos) {
                    in_solution_block = true;
                    logger->debug("Started parsing BIAS/SOLUTION block at line {}", line_count);
                    continue;
                }
                if (line.find("-BIAS/SOLUTION") != std::string::npos) {
                    in_solution_block = false;
                    logger->debug("Ended parsing BIAS/SOLUTION block at line {}", line_count);
                    break;
                }

                if (!in_solution_block || line.length() < 4 || line.substr(0, 4) != "OSB ") {
                    if (in_solution_block)
                        logger->debug("Non-OSB line in BIAS/SOLUTION block at line {}: {}", line_count, line);
                    continue;
                }

                // Fixed-width field extraction
                auto field = [&](size_t start, size_t len) -> std::string {
                    if (line.length() < start + len) return {};
                    std::string s = line.substr(start, len);
                    rtrim(s);
                    return s;
                };

                std::string type     = field(0, 4);
                std::string svn      = field(5, 4);
                std::string prn      = field(10, 3);
                std::string station  = field(14, 9);
                std::string obs1     = field(24, 4);
                std::string obs2     = field(29, 4);
                std::string start    = field(34, 14);
                std::string end      = field(49, 14);
                std::string unit     = field(64, 4);
                std::string bias_str = field(69, 21);
                std::string std_str  = field(91, 13);

                if (!station.empty()) continue;
                if (type != "OSB" || prn.length() != 3 || obs1.empty() || start.empty() || end.empty() || unit.empty() || bias_str.empty()) {
                    logger->warn("Invalid OSB line at line {}: {}", line_count, line);
                    continue;
                }

                double bias, std_dev = 0;
                try {
                    bias = std::stod(bias_str);
                    if (!std_str.empty()) std_dev = std::stod(std_str);
                }
                catch (const std::exception&) {
                    logger->warn("Invalid bias in OSB line at line {}: {}", line_count, line);
                    continue;
                }
                if (!std::isfinite(bias) || !std::isfinite(std_dev)) {
                    logger->warn("Non-finite bias or std_dev in OSB line at line {}: {}", line_count, line);
                    continue;
                }

                std::string signal = obs2.empty() ? obs1 : obs2;

                if (!isValidPrn(prn)) {
                    logger->warn("Invalid satellite PRN {} at line {}: {}", prn, line_count, line);
                    continue;
                }
                if (signal.length() != 3 || (signal[0] != 'C' && signal[0] != 'L' && signal[0] != 'D' && signal[0] != 'S') ||
                    !std::isdigit(signal[1]) || !std::isupper(signal[2])) {
                    logger->warn("Invalid observation code {} at line {}: {}", signal, line_count, line);
                    continue;
                }
                if (unit != "ns") {
                    logger->warn("Unexpected unit {} at line {}, expected ns: {}", unit, line_count, line);
                    continue;
                }

                BiasData data;
                data.satellite = prn;
                data.signal = signal;
                data.bias = bias * Constants::SPEED_OF_LIGHT * 1e-9;
                data.std_dev = std_dev * Constants::SPEED_OF_LIGHT * 1e-9;
                data.start_time = parse_bias_time(start);
                data.end_time = parse_bias_time(end);

                if (data.start_time.week <= 0 || data.end_time.week <= 0 ||
                    data.start_time.tow < 0.0 || data.end_time.tow < 0.0) {
                    logger->warn("Invalid time range for satellite {} signal {} at line {}: start={}, end={}",
                        data.satellite, data.signal, line_count, start, end);
                    continue;
                }
                logger->trace("Parsed BIA OSB line {} -> svn='{}', prn='{}', signal='{}', bias_ns={:.6f}, bias_m={:.9f}, start='{}', end='{}'",
                    line_count, svn, prn, signal, bias, data.bias, start, end);
                biases.push_back(data);
            }

            file.close();
            if (biases.empty()) {
                logger->error("No valid bias data parsed from {}", bia_path.string());
                return false;
            }
            logger->info("Parsed {} bias entries from {}", biases.size(), bia_path.string());
            return true;
        }
        catch (const std::exception& e) {
            log_manager_.getLogger("product_parser")->error("Error parsing BIA file {}: {}", bia_path.string(), e.what());
            return false;
        }
    }

    // =================================================================
    // ERP parsing
    // =================================================================

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
                if (line.empty() || line.find_first_of("0123456789") == std::string::npos) continue;
                std::istringstream iss(line);
                double mjd, xp, yp, ut1_utc;
                if (!(iss >> mjd >> xp >> yp >> ut1_utc)) continue;

                double days_since_epoch = mjd - TimeUtils::GPS_EPOCH_MJD;
                double total_seconds = days_since_epoch * TimeUtils::SECONDS_IN_DAY;
                int week = static_cast<int>(total_seconds / TimeUtils::SECONDS_IN_WEEK);
                double tow = total_seconds - week * static_cast<double>(TimeUtils::SECONDS_IN_WEEK);

                ErpData data;
                data.time = GnssTime{ week, tow };
                data.xp = xp * 1e-6;        // 1e-6 arcsec -> arcsec
                data.yp = yp * 1e-6;
                data.ut1_utc = ut1_utc * 1e-7; // 1e-7 s -> s
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

    // =================================================================
    // Klobuchar parsing
    // =================================================================

    bool ProductParser::parseKlobuchar(const std::filesystem::path& brdc_path, std::vector<double>& klobuchar_coeffs) {
        try {
            std::ifstream file(brdc_path);
            if (!file.is_open()) {
                log_manager_.getLogger("product_parser")->error("Failed to open BRDC file for Klobuchar coefficients: {}", brdc_path.string());
                return false;
            }

            klobuchar_coeffs.clear();
            auto logger = log_manager_.getLogger("product_parser");
            std::string line;
            size_t line_count = 0;
            bool in_header = true;

            auto parse_coeffs = [&](const std::string& label, std::istringstream& iss) {
                double c0, c1, c2, c3;
                if (iss >> c0 >> c1 >> c2 >> c3) {
                    if (std::isfinite(c0) && std::isfinite(c1) && std::isfinite(c2) && std::isfinite(c3)) {
                        klobuchar_coeffs.push_back(c0);
                        klobuchar_coeffs.push_back(c1);
                        klobuchar_coeffs.push_back(c2);
                        klobuchar_coeffs.push_back(c3);
                    } else {
                        logger->warn("Non-finite Klobuchar {} coefficients at line {}: {}", label, line_count, line);
                    }
                } else {
                    logger->warn("Invalid Klobuchar {} coefficients at line {}: {}", label, line_count, line);
                }
            };

            while (std::getline(file, line)) {
                ++line_count;
                if (in_header && line.find("END OF HEADER") != std::string::npos) { in_header = false; continue; }
                if (in_header && line.find("IONOSPHERIC CORR") != std::string::npos) {
                    std::istringstream iss(line);
                    std::string type;
                    iss >> type;
                    if (type == "GPSA" || type == "GAL") parse_coeffs("alpha", iss);
                    else if (type == "GPSB") parse_coeffs("beta", iss);
                }
            }

            file.close();
            if (klobuchar_coeffs.size() != 8) {
                logger->error("Incomplete Klobuchar coefficients from {}: expected 8, got {}", brdc_path.string(), klobuchar_coeffs.size());
                klobuchar_coeffs.clear();
                return false;
            }
            logger->info("Parsed 8 Klobuchar coefficients from {}: alpha=[{:.3e}, {:.3e}, {:.3e}, {:.3e}], beta=[{:.3e}, {:.3e}, {:.3e}, {:.3e}]",
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

    // =================================================================
    // Product loading orchestration
    // =================================================================

    void ProductParser::loadProducts() {
        auto ppp_log = log_manager_.getLogger("ppp_model");

        // Helper: load a multi-file product (SP3, CLK)
        auto loadMulti = [&](const char* label, auto getPaths, auto parseFn, auto& dest) {
            auto paths = (this->*getPaths)();
            if (paths.empty()) { ppp_log->warn("No {} file paths provided", label); return; }
            if ((this->*parseFn)(paths, dest))
                ppp_log->info("Loaded {} files: [{}]", label, joinPaths(paths));
            else
                ppp_log->error("Failed to load {} files: [{}]", label, joinPaths(paths));
        };

        // Helper: load a single-file product (ATX, BIA, ERP, RNX)
        auto loadSingle = [&](const char* ext, const char* label, auto parseFn, auto& dest) {
            auto path = getProductPath(ext);
            if (path.empty()) { ppp_log->warn("No {} file path provided", label); return; }
            if ((this->*parseFn)(path, dest))
                ppp_log->info("Loaded {} file: {}", label, path.string());
            else
                ppp_log->error("Failed to load {} file: {}", label, path.string());
        };

        loadMulti("SP3",  &ProductParser::getOrbitPaths, &ProductParser::parseSp3, orbits_);
        loadMulti("CLK",  &ProductParser::getClockPaths, &ProductParser::parseClk, clocks_);
        loadSingle("ATX", "ATX", &ProductParser::parseAtx, antennas_);

        // BIA: special case  parseBia also extracts the clock reference map
        {
            auto path = getProductPath("BIA");
            if (path.empty()) { ppp_log->warn("No BIA file path provided"); }
            else if (parseBia(path, biases_, clock_refs_))
                ppp_log->info("Loaded BIA file: {}", path.string());
            else
                ppp_log->error("Failed to load BIA file: {}", path.string());
        }

        loadSingle("ERP", "ERP", &ProductParser::parseErp, erps_);

        auto rnx_path = getProductPath("RNX");
        if (!rnx_path.empty()) {
            if (parseKlobuchar(rnx_path, klobuchar_coeffs_))
                ppp_log->info("Loaded Klobuchar coefficients from RNX file: {}, coeffs size: {}", rnx_path.string(), klobuchar_coeffs_.size());
            else
                ppp_log->error("Failed to load Klobuchar coefficients from RNX file: {}", rnx_path.string());
        } else {
            ppp_log->warn("No RNX file path provided for Klobuchar or Sun ephemeris");
        }

        std::vector<GnssTime> epoch_times = rinex_parser_->getEpochTimes();
        if (planetary_.parseSunEphemeris(epoch_times, sun_ephemeris_))
            ppp_log->info("Computed Sun ephemeris for {} observation epochs", sun_ephemeris_.size());
        else
            ppp_log->error("Failed to compute Sun ephemeris for observation epochs");
    }

} // namespace GnssProcessor