#pragma once
#include "rinex/RinexParser.hpp"
#include "math/TimeUtils.hpp"
#include <filesystem>
#include <map>
#include <string>
#include <vector>

namespace GnssProcessor {

/**
 * @file SinexParser.hpp
 * @brief SINEX 2.02 parser declarations for site/epoch/estimate extraction.
 *
 * This header declares lightweight types used to represent core SINEX entities and a
 * parser class that reads SINEX files and extracts SITE/ID, SOLUTION/EPOCHS and
 * SOLUTION/ESTIMATE blocks.
 *
 * The parser produces:
 *  - a list of sites with ECEF coordinates,
 *  - a list of solution epochs with associated technique,
 *  - a list of parameter estimates (position components, clocks, etc.).
 *
 * The parser is intentionally minimal: it focuses on extracting values used by the
 * rest of the processing pipeline. Consumers should validate semantic correctness
 * (e.g., coordinate frames, units) if required.
 */

/**
 * @struct SinexSite
 * @brief Representation of a SINEX site identification record from the SITE/ID block.
 *
 * Fields correspond to commonly found SINEX columns:
 *  - `site_code`: 4-character site identifier (e.g., "GCGO").
 *  - `point_code`: site point code (often "00" or similar).
 *  - `country_code`: ISO-like country code (e.g., "USA").
 *  - `coordinates`: ECEF coordinates in meters (X, Y, Z).
 */
struct SinexSite {
    std::string site_code;    ///< 4-char site code (e.g., "GCGO")
    std::string point_code;   ///< Point code (e.g., "00")
    std::string country_code; ///< Country code (e.g., "USA")
    std::array<double, 3> coordinates; ///< ECEF coordinates (X, Y, Z in meters)
};

/**
 * @struct SinexEpoch
 * @brief Representation of a solution epoch extracted from the SOLUTION/EPOCHS block.
 *
 * - `time` holds the epoch expressed as a `GnssTime`.
 * - `technique` holds the technique identifier (e.g., "C" for GNSS).
 */
struct SinexEpoch {
    GnssTime time;        ///< Epoch time as GnssTime
    std::string technique; ///< Observation technique (e.g., "C" for GNSS)
};

/**
 * @struct SinexEstimate
 * @brief Representation of a parameter estimate from the SOLUTION/ESTIMATE block.
 *
 * Typical `parameter_type` values: "STAX", "STAY", "STAZ" (station position), "CLCK" (clock),
 * "ORBX" (satellite orbit), etc.
 *
 * Units are context dependent:
 *  - Position components: meters.
 *  - Clock: seconds.
 *  - Std. dev.: same units as `value`.
 */
struct SinexEstimate {
    std::string parameter_type; ///< Parameter type (e.g., "STAX", "ORBX", "CLCK")
    std::string site_code;      ///< Site or satellite code (e.g., "GCGO", "G01")
    GnssTime time;              ///< Epoch at which the estimate applies
    double value;               ///< Estimated value (units depend on parameter_type)
    double std_dev;             ///< Standard deviation (same units as value)
};

/**
 * @class SinexParser
 * @brief Parser for SINEX 2.02 files.
 *
 * Constructs with the path to a SINEX file and exposes a simple `parse()` method that
 * fills internal containers accessible via getters. The parser is not thread-safe.
 */
class SinexParser {
public:
    /**
     * @brief Create a parser for the given SINEX file.
     * @param file_path Path to the SINEX file to parse.
     */
    explicit SinexParser(const std::filesystem::path& file_path);

    /**
     * @brief Parse the SINEX file held by the parser.
     * @return True on success, false on failure (I/O errors or unexpected format).
     *
     * After a successful parse the `getSites()`, `getEpochs()` and `getEstimates()` accessors
     * will return populated vectors. Partial parsing may still leave some containers empty.
     */
    bool parse();

    /**
     * @brief Get parsed site records.
     * @return const reference to a vector of SinexSite entries.
     */
    const std::vector<SinexSite>& getSites() const;

    /**
     * @brief Get parsed solution epochs.
     * @return const reference to a vector of SinexEpoch entries.
     */
    const std::vector<SinexEpoch>& getEpochs() const;

    /**
     * @brief Get parsed parameter estimates.
     * @return const reference to a vector of SinexEstimate entries.
     */
    const std::vector<SinexEstimate>& getEstimates() const;

private:
    std::filesystem::path file_path_; ///< Input SINEX file path
    std::string version_;             ///< SINEX version string (e.g., "2.02")
    std::vector<SinexSite> sites_;    ///< Parsed SITE/ID entries
    std::vector<SinexEpoch> epochs_;  ///< Parsed SOLUTION/EPOCHS entries
    std::vector<SinexEstimate> estimates_; ///< Parsed SOLUTION/ESTIMATE entries

    /**
     * @brief Parse the SINEX header section.
     * @param stream Input file stream positioned at the beginning of the file.
     * @return True if header parsing succeeded; false otherwise.
     *
     * The header parser typically extracts the version and may perform light validation.
     */
    bool parseHeader(std::ifstream& stream);

    /**
     * @brief Parse data blocks such as SITE/ID, SOLUTION/EPOCHS and SOLUTION/ESTIMATE.
     * @param stream Input file stream positioned after the header.
     * @return True on successful parsing of expected blocks; false on format errors.
     */
    bool parseDataBlocks(std::ifstream& stream);

    /**
     * @brief Parse a SINEX time string of the form "YY:DOY:SSSSS" into a GnssTime.
     * @param time_str The SINEX time string to parse.
     * @return Parsed GnssTime (week and tow).
     *
     * Example SINEX time: "14:032:43200.000" where
     *  - "14" is the two-digit year (interpreted according to implementation rules),
     *  - "032" is day-of-year,
     *  - "43200.000" is seconds of day.
     *
     * The implementation converts this to GPS week and seconds-of-week.
     */
    GnssTime parseSinexTime(const std::string& time_str) const;
};

} // namespace GnssProcessor
//#endif // SINEX_PARSER_HPP