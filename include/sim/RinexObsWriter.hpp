#pragma once

/**
 * @file RinexObsWriter.hpp
 * @brief RINEX-3.04 mixed observation file writer for the GNSS exam simulator.
 *
 * The simulator is the inverse of @c GnssProcessor::RinexParser: it serializes
 * synthesized pseudorange / carrier-phase observables into a RINEX-3.04
 * observation file that @c RinexParser::parse() ingests without error.  The
 * byte layout here is matched field-for-field against the parser
 * (src/core/rinex/RinexParser.cpp): the version is read from columns 0-8, the
 * record label from column 60, @c SYS / # / OBS TYPES with the count in columns
 * 3-5 and codes whitespace-tokenized from column 6, the @c '>' epoch line
 * whitespace-tokenized, and each satellite line as a 3-char satellite id
 * followed by fixed 16-character observation fields (F14.3 value, 1-char LLI,
 * 1-char signal-strength) at offset @c 3 + j*16.
 *
 * @note The parser trims trailing whitespace from every line before column
 *       slicing, so a present observation always writes a non-blank
 *       signal-strength digit in the 16th field column; otherwise the final
 *       field of a satellite line would be lost to the trim.
 */

#include <array>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "math/TimeUtils.hpp" // GnssTime

namespace GnssProcessor {
namespace sim {

/**
 * @brief Header metadata for one synthesized observation file.
 *
 * Field defaults produce a self-consistent kinematic marine receiver header.
 * @c obs_types is an ordered list of (system-letter, codes) pairs; the code
 * order is binding: it defines the per-satellite field order on every epoch.
 */
struct ObsHeaderSpec {
    double version{ 3.04 };                       ///< RINEX version (>=3.0 selects the v3 parser branch).
    std::string pgm{ "gnss_simulator" };          ///< Creating program (PGM / RUN BY / DATE, cols 0-19).
    std::string run_by{ "GnssSimExam" };          ///< Run-by agency (cols 20-39).
    std::string run_date{ "00000000 000000 UTC" };///< Creation timestamp (cols 40-59).
    std::string marker_name{ "SARV" };            ///< Marker / station name.
    std::string marker_number{ "SARV00FJI" };     ///< Marker number (optional).
    std::string marker_type{ "KINEMATIC" };       ///< Marker type (cosmetic; no parser handler).
    std::string observer{ "SAR" };                ///< Observer name (cols 0-19).
    std::string agency{ "SouthPacificSAR" };      ///< Agency (cols 20-59).
    std::string rec_number{ "0000000" };          ///< Receiver number (cols 0-19).
    std::string rec_type{ "GENERIC GNSS RX" };    ///< Receiver type (cols 20-39).
    std::string rec_version{ "1.0" };             ///< Receiver firmware version (cols 40-59).
    std::string ant_number{ "0000000" };          ///< Antenna number (cols 0-19).
    std::string ant_type{ "GENERIC ANT     NONE" }; ///< Antenna type, A16 name + A4 radome (cols 20-39).
    std::array<double, 3> approx_xyz{ { 0.0, 0.0, 0.0 } }; ///< Approx ECEF (m); kinematic seed.
    std::array<double, 3> antenna_hen{ { 0.0, 0.0, 0.0 } };///< Antenna H/E/N offsets (m).
    double interval_s{ 30.0 };                    ///< Nominal epoch interval (s).
    GnssTime time_of_first_obs{};                 ///< First epoch (GPS week + TOW).
    std::string time_system{ "GPS" };             ///< Time system tag (cols 48-50 of TIME OF FIRST OBS).
    std::vector<std::pair<char, std::vector<std::string>>> obs_types{}; ///< Ordered codes per system letter.
    int default_snr_flag{ 8 };                    ///< RINEX signal-strength flag (1-9) for present observations.
};

/**
 * @brief One satellite's observations at a single epoch.
 *
 * @c values maps a RINEX-3 observation code to its value: code observations
 * (`C...`) in metres, carrier-phase observations (`L...`) in cycles (already
 * divided by the carrier wavelength).  @c lli optionally carries a loss-of-lock
 * indicator per code; an absent entry (or value <= 0) writes a blank LLI column.
 */
struct SatObs {
    std::string sat_id;                  ///< 3-char satellite id, zero-padded PRN (e.g. "G01", "E11").
    std::map<std::string, double> values;///< Observation code -> value (code [m], phase [cycles]).
    std::map<std::string, int> lli;      ///< Observation code -> LLI flag (optional).
};

/**
 * @brief Streams synthesized observations to a RINEX-3.04 observation file.
 *
 * Usage: @c open() (writes the header), @c writeEpoch() per epoch, then
 * @c close().  Not copyable (owns an @c std::ofstream).
 */
class RinexObsWriter {
public:
    RinexObsWriter() = default;
    RinexObsWriter(const RinexObsWriter&) = delete;
    RinexObsWriter& operator=(const RinexObsWriter&) = delete;

    /**
     * @brief Open @p path, validate the header, and emit the RINEX header block.
     *
     * Rejects filenames containing @c "BRDC" or @c "_MN." (the parser would
     * misroute them to the navigation path) and observation-type lists longer
     * than 13 codes per system (continuation lines are not yet emitted).
     *
     * @param path Output file path; parent directories are created.
     * @param hdr  Header specification (copied).
     * @param err  Optional out-parameter populated with a reason on failure.
     * @return True on success, false otherwise (with @p err set if provided).
     */
    bool open(const std::filesystem::path& path, const ObsHeaderSpec& hdr, std::string* err = nullptr);

    /**
     * @brief Write one epoch: a @c '>' record followed by one line per
     *        satellite that carries at least one observation.
     *
     * Satellites with an empty @c values map are skipped and excluded from the
     * epoch's satellite count.  Observation fields are emitted in the header's
     * declared code order for the satellite's system; an interior missing code
     * writes a blank 16-char field, trailing missing codes are truncated.
     *
     * @param t          Epoch time (GPS week + TOW).
     * @param sats       Satellites and their observations at this epoch.
     * @param epoch_flag RINEX epoch flag (0 = OK; must be <= 1 for the parser).
     */
    void writeEpoch(const GnssTime& t, const std::vector<SatObs>& sats, int epoch_flag = 0);

    /** @brief Flush and close the underlying stream. */
    void close();

    /** @brief True while the output stream is open. */
    bool isOpen() const { return out_.is_open(); }

private:
    std::ofstream out_;  ///< Output stream (owned).
    ObsHeaderSpec hdr_;  ///< Header spec captured at open().

    /** @brief Return the ordered code list for a system letter, or nullptr. */
    const std::vector<std::string>* codesForSystem(char sys_letter) const;

    /** @brief Emit the full RINEX header block from @c hdr_. */
    void writeHeader();

    /**
     * @brief Convert an epoch to calendar fields for the RINEX time tag.
     *
     * Delegates to @c TimeUtils::gpsTimeToGpsScaleCalendar (GPS scale, no leap
     * shift) then snaps the seconds-of-day to the nearest millisecond so the
     * epoch tag lands cleanly on the sampling grid.  The underlying JD-based
     * conversion carries tens of microseconds of jitter; for simulated data on
     * a known grid that jitter is pure noise and is removed here.  An integer
     * recompose keeps minute/hour boundaries exact.
     */
    void calendarFields(const GnssTime& t, int& y, int& m, int& d, int& h, int& mi, double& sec) const;
};

} // namespace sim
} // namespace GnssProcessor
