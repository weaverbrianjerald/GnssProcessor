#pragma once
#include "RinexParser.hpp"
#include "TimeUtils.hpp"
#include <filesystem>
#include <map>
#include <string>
#include <vector>

namespace GnssProcessor {

// Structure for SINEX site identification
struct SinexSite {
    std::string site_code; // 4-char site code (e.g., "GCGO")
    std::string point_code; // Point code (e.g., "00")
    std::string country_code; // Country code (e.g., "USA")
    std::array<double, 3> coordinates; // ECEF coordinates (X, Y, Z in meters)
};

// Structure for SINEX solution epoch
struct SinexEpoch {
    GnssTime time; // Epoch time
    std::string technique; // Observation technique (e.g., "C" for GNSS)
};

// Structure for SINEX parameter estimate
struct SinexEstimate {
    std::string parameter_type; // e.g., "STAX", "ORBX", "CLCK"
    std::string site_code; // Site or satellite code (e.g., "GCGO", "G01")
    GnssTime time; // Epoch time
    double value; // Estimated value (e.g., meters for position, seconds for clock)
    double std_dev; // Standard deviation
};

// SinexParser class for SINEX 2.02 files
class SinexParser {
public:
    explicit SinexParser(const std::filesystem::path& file_path);

    // Parse the SINEX file
    bool parse();

    // Get parsed sites
    const std::vector<SinexSite>& getSites() const;

    // Get parsed epochs
    const std::vector<SinexEpoch>& getEpochs() const;

    // Get parsed estimates
    const std::vector<SinexEstimate>& getEstimates() const;

private:
    std::filesystem::path file_path_;
    std::string version_; // SINEX version (e.g., "2.02")
    std::vector<SinexSite> sites_;
    std::vector<SinexEpoch> epochs_;
    std::vector<SinexEstimate> estimates_;

    // Parse header line
    bool parseHeader(std::ifstream& stream);

    // Parse data blocks (SITE/ID, SOLUTION/EPOCHS, SOLUTION/ESTIMATE)
    bool parseDataBlocks(std::ifstream& stream);

    // Parse time in SINEX format (YY:DOY:SSSSS)
    GnssTime parseSinexTime(const std::string& time_str) const;
};

} // namespace GnssProcessor
//#endif // SINEX_PARSER_HPP