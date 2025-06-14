#include "SinexParser.hpp"
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <spdlog/spdlog.h>
#include <regex>

namespace GnssProcessor {

    SinexParser::SinexParser(const std::filesystem::path& file_path)
        : file_path_(file_path), version_("2.02") {
    }

    bool SinexParser::parse() {
        spdlog::info("Parsing SINEX file: {}", file_path_.string());
        std::ifstream stream(file_path_);
        if (!stream.is_open()) {
            spdlog::error("Failed to open SINEX file: {}", file_path_.string());
            throw std::runtime_error("Failed to open SINEX file");
        }

        // Parse header
        if (!parseHeader(stream)) {
            spdlog::error("Failed to parse SINEX header");
            return false;
        }

        // Parse data blocks
        if (!parseDataBlocks(stream)) {
            spdlog::error("Failed to parse SINEX data blocks");
            return false;
        }

        spdlog::info("Successfully parsed SINEX file: {} sites, {} epochs, {} estimates",
            sites_.size(), epochs_.size(), estimates_.size());
        return true;
    }

    const std::vector<SinexSite>& SinexParser::getSites() const {
        return sites_;
    }

    const std::vector<SinexEpoch>& SinexParser::getEpochs() const {
        return epochs_;
    }

    const std::vector<SinexEstimate>& SinexParser::getEstimates() const {
        return estimates_;
    }

    bool SinexParser::parseHeader(std::ifstream& stream) {
        std::string line;
        if (!std::getline(stream, line)) {
            spdlog::error("Empty SINEX file");
            return false;
        }

        // Expected header format: %=SNX 2.02 ...
        if (line.substr(0, 5) != "%=SNX") {
            spdlog::error("Invalid SINEX header: {}", line);
            return false;
        }

        try {
            version_ = line.substr(6, 4);
            if (version_ != "2.02") {
                spdlog::warn("SINEX version {} may not be fully supported", version_);
            }
            spdlog::info("SINEX version: {}", version_);
        }
        catch (const std::exception& e) {
            spdlog::error("Invalid header format: {}", e.what());
            return false;
        }

        return true;
    }

    bool SinexParser::parseDataBlocks(std::ifstream& stream) {
        std::string line;
        std::string current_block;

        while (std::getline(stream, line)) {
            if (line.empty() || line[0] == '*') continue; // Skip comments and empty lines

            if (line[0] == '+') {
                current_block = line.substr(1, line.find(' ', 1) - 1);
                spdlog::debug("Entering block: {}", current_block);
                continue;
            }
            if (line[0] == '-') {
                current_block.clear();
                continue;
            }

            if (current_block == "SITE/ID") {
                SinexSite site;
                try {
                    site.site_code = line.substr(1, 4);
                    site.point_code = line.substr(6, 2);
                    site.country_code = line.substr(16, 3);
                    // Coordinates not in SITE/ID; will be in SOLUTION/ESTIMATE
                    sites_.push_back(site);
                    spdlog::trace("Parsed site: {} {} {}", site.site_code, site.point_code, site.country_code);
                }
                catch (const std::exception& e) {
                    spdlog::warn("Invalid SITE/ID line: {}", e.what());
                }
            }
            else if (current_block == "SOLUTION/EPOCHS") {
                SinexEpoch epoch;
                try {
                    std::string site_code = line.substr(1, 4);
                    std::string time_str = line.substr(14, 11);
                    epoch.time = parseSinexTime(time_str);
                    epoch.technique = line.substr(26, 1);
                    epochs_.push_back(epoch);
                    spdlog::trace("Parsed epoch for {}: week {}, TOW {:.3f}, technique {}",
                        site_code, epoch.time.week, epoch.time.tow, epoch.technique);
                }
                catch (const std::exception& e) {
                    spdlog::warn("Invalid SOLUTION/EPOCHS line: {}", e.what());
                }
            }
            else if (current_block == "SOLUTION/ESTIMATE") {
                SinexEstimate estimate;
                try {
                    estimate.parameter_type = line.substr(7, 6);
                    estimate.site_code = line.substr(14, 4);
                    std::string time_str = line.substr(19, 11);
                    estimate.time = parseSinexTime(time_str);
                    estimate.value = std::stod(line.substr(31, 21));
                    estimate.std_dev = std::stod(line.substr(53, 12));
                    estimates_.push_back(estimate);
                    spdlog::trace("Parsed estimate: {} for {}, value {:.6f}, std_dev {:.6f}",
                        estimate.parameter_type, estimate.site_code, estimate.value, estimate.std_dev);
                }
                catch (const std::exception& e) {
                    spdlog::warn("Invalid SOLUTION/ESTIMATE line: {}", e.what());
                }
            }
        }

        return !sites_.empty() || !epochs_.empty() || !estimates_.empty();
    }

    GnssTime SinexParser::parseSinexTime(const std::string& time_str) const {
        // SINEX time format: YY:DOY:SSSSS (e.g., "24:129:00000")
        static const std::regex time_regex(R"((\d{2}):(\d{3}):(\d{5}))");
        std::smatch match;
        if (!std::regex_match(time_str, match, time_regex)) {
            spdlog::error("Invalid SINEX time format: {}", time_str);
            return GnssTime{ 0, 0.0 };
        }

        try {
            int year = 2000 + std::stoi(match[1].str()); // YY to YYYY
            int doy = std::stoi(match[2].str()); // Day of year
            int seconds = std::stoi(match[3].str()); // Seconds of day

            // Convert DOY to Gregorian date
            int month = 1, day = doy;
            int days_in_month[] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
            if ((year % 4 == 0 && year % 100 != 0) || (year % 400 == 0)) {
                days_in_month[1] = 29; // Leap year
            }
            while (day > days_in_month[month - 1]) {
                day -= days_in_month[month - 1];
                month++;
            }

            int hour = seconds / 3600;
            seconds %= 3600;
            int minute = seconds / 60;
            double second = seconds % 60;

            return TimeUtils::gregorianToGpsTime(year, month, day, hour, minute, second);
        }
        catch (const std::exception& e) {
            spdlog::error("Failed to parse SINEX time {}: {}", time_str, e.what());
            return GnssTime{ 0, 0.0 };
        }
    }

} // namespace GnssProcessor
//#endif