#include "rinex/RinexParser.hpp"
#include "products/ProductParser.hpp"
#include "math/TimeUtils.hpp"
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <algorithm>
#include <cctype>
#include <set>
#include <spdlog/spdlog.h>
#include "common/Constants.hpp"
#include "common/PppTypes.hpp"

namespace GnssProcessor {

    namespace {
        /**
         * @brief Translate a RINEX 2.x 2-character observation code to RINEX 3.x 3-character form.
         *
         * RINEX 2.x uses two-character observation codes (e.g., "C1", "L1", "P2"), where the
         * tracking attribute (C/A vs P(Y) vs L2C, etc.) is implicit and depends on the
         * receiver and constellation. RINEX 3.x makes the attribute explicit as the third
         * character (e.g., "C1C", "C1W", "C2L"). This function applies the most common
         * mapping for each (system, frequency-band) pair, matching what mainstream
         * geodetic receivers (e.g., Trimble NETR9, Septentrio PolaRx) emit. Pseudoranges
         * recorded as P-codes ("P1", "P2") map to the encrypted/precise attributes
         * ("C1W"/"C2W" for GPS, "C1P"/"C2P" for GLONASS).
         *
         * Where the v2 code is already 3 characters (i.e., already v3), it is returned
         * unchanged. Where the v2 code length is not 2, it is also returned unchanged.
         *
         * @param v2_code Two-character RINEX 2.x observation code (e.g., "C1", "L2", "P1").
         * @param system  GNSS system the observation belongs to.
         * @return Three-character RINEX 3.x observation code, or the input if it could not be translated.
         */
        std::string translateV2ObsCodeToV3(const std::string& v2_code, System system) {
            if (v2_code.size() != 2) return v2_code;
            const char obs_type = v2_code[0];
            const char freq     = v2_code[1];

            // Validate first char is one of the legal RINEX measurement types.
            if (obs_type != 'C' && obs_type != 'L' && obs_type != 'D' &&
                obs_type != 'S' && obs_type != 'P') {
                return v2_code;
            }

            // RINEX 2.x P-code: encrypted P(Y) on GPS, P-code on GLONASS.
            if (obs_type == 'P') {
                char buf[4] = { 'C', freq, '\0', '\0' };
                if (system == System::GLONASS) buf[2] = 'P';
                else                            buf[2] = 'W'; // GPS or unknown -> Z-tracking
                return std::string(buf);
            }

            // Determine the attribute character per (system, obs-type, frequency band).
            //
            // Note on the GPS L2 disambiguation: a RINEX 2.x mixed file can list both "C2"
            // (L2 civilian, typically L2C-L on modern survey receivers) and "P2" (L2 P(Y)
            // tracked via Z-tracking) in the same epoch. Both refer to L2 codes but on
            // different signals, so they must NOT collapse onto the same v3 code. We map
            // P2 -> C2W (canonical Z-tracking attribute) and C2 -> C2L (L2C-L attribute);
            // the carrier phase / Doppler / SNR observables on L2 stay paired with the
            // P-code track (L2W/D2W/S2W), which matches what NETR9-class receivers report.
            char attr = 'C';
            switch (system) {
            case System::GPS:
                if (obs_type == 'C') {
                    // L2 C-code is the L2C-L civilian channel; everything else is C/A or L5-Q.
                    if      (freq == '1') attr = 'C';
                    else if (freq == '2') attr = 'L';
                    else if (freq == '5') attr = 'Q';
                    else                  attr = 'C';
                } else {
                    // L/D/S codes track the carrier paired with the (Z-tracked) P-code on L2.
                    if      (freq == '1') attr = 'C';
                    else if (freq == '2') attr = 'W';
                    else if (freq == '5') attr = 'Q';
                    else                  attr = 'C';
                }
                break;
            case System::GLONASS:
                // G1/G2 C/A -> C, G3 (CDMA L3OC) -> I.
                if (freq == '3') attr = 'I';
                else             attr = 'C';
                break;
            case System::Galileo:
                // E1/E5a/E5b/E6/E5(a+b) all default to combined I+Q (X) tracking.
                attr = 'X';
                break;
            case System::BeiDou:
                // B1I (RINEX2 freq 2), B2I (freq 7), B3I (freq 6) all use I attribute.
                attr = 'I';
                break;
            case System::QZSS:
                // L1 C/A -> C, L2C/L5/L6 -> X (combined).
                attr = (freq == '1') ? 'C' : 'X';
                break;
            case System::SBAS:
                // L1 C/A -> C, L5 I-channel -> I.
                attr = (freq == '5') ? 'I' : 'C';
                break;
            default:
                attr = 'C';
                break;
            }

            char buf[4] = { obs_type, freq, attr, '\0' };
            return std::string(buf);
        }

        /**
         * @brief Strip leading and trailing ASCII whitespace (incl. CR) in place.
         */
        void trimInPlace(std::string& s) {
            const auto first = s.find_first_not_of(" \t\r\n");
            if (first == std::string::npos) { s.clear(); return; }
            const auto last = s.find_last_not_of(" \t\r\n");
            s = s.substr(first, last - first + 1);
        }

        /**
         * @brief Parse an integer from a fixed-width substring; returns false on blank/invalid.
         */
        bool parseFixedInt(const std::string& line, size_t off, size_t len, int& out) {
            if (off >= line.size()) return false;
            std::string s = line.substr(off, std::min(len, line.size() - off));
            trimInPlace(s);
            if (s.empty()) return false;
            try { out = std::stoi(s); return true; }
            catch (...) { return false; }
        }

        /**
         * @brief Parse a double from a fixed-width substring; returns false on blank/invalid.
         */
        bool parseFixedDouble(const std::string& line, size_t off, size_t len, double& out) {
            if (off >= line.size()) return false;
            std::string s = line.substr(off, std::min(len, line.size() - off));
            trimInPlace(s);
            if (s.empty()) return false;
            try { out = std::stod(s); return true; }
            catch (...) { return false; }
        }

        /**
         * @brief Map first-character system identifier to System enum.
         */
        System systemFromSatPrefix(char c) {
            switch (c) {
            case 'G': return System::GPS;
            case 'R': return System::GLONASS;
            case 'E': return System::Galileo;
            case 'C': return System::BeiDou;
            case 'J': return System::QZSS;
            case 'S': return System::SBAS;
            default:  return System::Unknown;
            }
        }
    } // anonymous namespace


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
            bool success = product_parser_.parseKlobuchar(rinex_path, klobuchar_coeffs);
            if (!success) {
                log_manager_.getLogger("rinex_parser")->error("Failed to parse navigation RINEX file: {}", rinex_path.string());
                return false;
            }
            log_manager_.getLogger("rinex_parser")->info("Parsed navigation RINEX file: {}, Klobuchar coeffs: {}",
                rinex_path.string(), klobuchar_coeffs.size());
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
                    else if (label.find("# / TYPES OF OBSERV") != std::string::npos) {
                        // RINEX 2.x observation type list (single, shared across all systems).
                        // Format: I6 num_types, then up to 9 codes per line in 6-char fields
                        // (4 leading spaces + A2 code) starting at column 6 and ending at
                        // column 60. Continuation lines repeat the format with cols 0-5 blank.
                        int num_types = 0;
                        if (!parseFixedInt(line, 0, 6, num_types) || num_types <= 0) {
                            log_manager_.getLogger("rinex_parser")->warn(
                                "Invalid # / TYPES OF OBSERV count in line: {}", line);
                            continue;
                        }

                        std::vector<std::string> v2_obs_types;
                        v2_obs_types.reserve(static_cast<size_t>(num_types));

                        auto extractCodesFromLine = [&](const std::string& src) {
                            // Codes occupy 6-char fields starting at column 6, up to 9 per line.
                            for (size_t off = 6; off + 6 <= 60 &&
                                 v2_obs_types.size() < static_cast<size_t>(num_types); off += 6) {
                                if (off + 6 > src.size()) break;
                                std::string code = src.substr(off + 4, 2);
                                trimInPlace(code);
                                if (code.size() == 2) {
                                    v2_obs_types.push_back(code);
                                }
                            }
                        };

                        extractCodesFromLine(line);

                        // Read continuation lines until we have all declared types or
                        // hit a non-continuation line.
                        while (v2_obs_types.size() < static_cast<size_t>(num_types)) {
                            std::string cont;
                            if (!std::getline(file, cont)) break;
                            // Strip trailing CR so column positions are reliable.
                            if (!cont.empty() && cont.back() == '\r') cont.pop_back();

                            // Sanity check: continuation lines should also carry the same label.
                            const std::string clabel = cont.size() >= 60 ? cont.substr(60) : "";
                            if (clabel.find("# / TYPES OF OBSERV") == std::string::npos) {
                                log_manager_.getLogger("rinex_parser")->warn(
                                    "Expected # / TYPES OF OBSERV continuation; got '{}'.  "
                                    "Header parsing may be incomplete.", cont);
                                break;
                            }
                            extractCodesFromLine(cont);
                        }

                        if (v2_obs_types.size() != static_cast<size_t>(num_types)) {
                            log_manager_.getLogger("rinex_parser")->warn(
                                "Expected {} v2 observation types, parsed {}",
                                num_types, v2_obs_types.size());
                        }

                        // RINEX 2.x is observation-type-shared: assign the same list to every
                        // system the file may contain. Mixed (M) files use these for all
                        // constellations; pure (G/R/E/C/J/S) files only ever exercise one.
                        static const System kAllSystems[] = {
                            System::GPS, System::GLONASS, System::Galileo,
                            System::BeiDou, System::QZSS, System::SBAS,
                        };
                        for (System sys : kAllSystems) {
                            header_.obs_types[sys] = v2_obs_types;
                        }

                        std::string types_str;
                        for (size_t i = 0; i < v2_obs_types.size(); ++i) {
                            types_str += v2_obs_types[i];
                            if (i + 1 < v2_obs_types.size()) types_str += ", ";
                        }
                        log_manager_.getLogger("rinex_parser")->info(
                            "Parsed {} RINEX 2.x observation types (shared across systems): {}",
                            v2_obs_types.size(), types_str);
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
                    else if (label.find("SYS / PHASE SHIFT") != std::string::npos) {
                        // RINEX 3.x record (cols, 1-based):
                        //   1     : system letter (G, E, R, C, J, S)
                        //   2     : blank
                        //   3-5   : observation code (e.g., "L2W", "L8X")
                        //   6     : blank
                        //   7-14  : phase shift in cycles, F8.5 (may be blank → 0)
                        //   15    : blank
                        //   16-17 : number of satellites listed (I2; 0 = applies to all)
                        //   18-58 : satellite list (10 × A4, may continue on next line)
                        //   60-79 : "SYS / PHASE SHIFT"
                        //
                        // We honor only the system-wide shifts (sat list empty
                        // or zero count); per-sat overrides are uncommon in
                        // practice and not supported here.  When a record's
                        // shift field is blank, the receiver did not record a
                        // shift for that observation code — treat as zero.
                        if (line.size() >= 5) {
                            const std::string sys = line.substr(0, 1);
                            const System sys_enum =
                                sys == "G" ? System::GPS :
                                sys == "E" ? System::Galileo :
                                sys == "R" ? System::GLONASS :
                                sys == "C" ? System::BeiDou :
                                sys == "J" ? System::QZSS :
                                sys == "S" ? System::SBAS : System::Unknown;
                            std::string obs_code = line.substr(2, 3);
                            obs_code.erase(obs_code.find_last_not_of(" \t") + 1);
                            obs_code.erase(0, obs_code.find_first_not_of(" \t"));

                            double shift_cyc = 0.0;
                            if (line.size() >= 14) {
                                std::string shift_str = line.substr(6, 8);
                                shift_str.erase(0, shift_str.find_first_not_of(" \t"));
                                shift_str.erase(shift_str.find_last_not_of(" \t") + 1);
                                if (!shift_str.empty()) {
                                    try {
                                        shift_cyc = std::stod(shift_str);
                                    } catch (const std::exception&) {
                                        log_manager_.getLogger("rinex_parser")->warn(
                                            "Invalid SYS / PHASE SHIFT value '{}' for {} {}; skipping",
                                            shift_str, sys, obs_code);
                                        continue;
                                    }
                                }
                            }

                            int n_sats = 0;
                            if (line.size() >= 17) {
                                std::string nsats_str = line.substr(15, 2);
                                nsats_str.erase(0, nsats_str.find_first_not_of(" \t"));
                                nsats_str.erase(nsats_str.find_last_not_of(" \t") + 1);
                                if (!nsats_str.empty()) {
                                    try {
                                        n_sats = std::stoi(nsats_str);
                                    } catch (const std::exception&) {
                                        n_sats = 0;
                                    }
                                }
                            }

                            if (sys_enum == System::Unknown || obs_code.empty()) {
                                log_manager_.getLogger("rinex_parser")->debug(
                                    "Skipping malformed SYS / PHASE SHIFT record: '{}'", line);
                                // Still consume continuation lines if any.
                            } else if (n_sats > 0) {
                                log_manager_.getLogger("rinex_parser")->info(
                                    "SYS / PHASE SHIFT for {} {} has per-sat list (n={}); "
                                    "per-sat overrides are not applied (system-wide only).",
                                    sys, obs_code, n_sats);
                            } else if (shift_cyc != 0.0) {
                                header_.phase_shifts[sys_enum][obs_code] = shift_cyc;
                                log_manager_.getLogger("rinex_parser")->info(
                                    "SYS / PHASE SHIFT: {} {} = {:+.5f} cyc (will be subtracted from raw phase)",
                                    sys, obs_code, shift_cyc);
                            }

                            // Skip any continuation lines: the spec packs 10
                            // sat IDs per row; if more are listed, additional
                            // SYS / PHASE SHIFT-labeled records follow with a
                            // blank system column.  We never use the per-sat
                            // list, so swallow them rather than mis-parsing.
                            int remaining_sats = n_sats - 10;
                            while (remaining_sats > 0 && std::getline(file, line)) {
                                line.erase(line.find_last_not_of(" \t") + 1);
                                remaining_sats -= 10;
                            }
                        }
                    }
                    else if (label.find("GLONASS SLOT / FRQ #") != std::string::npos) {
                        // Header carries up to 8 (sat, freq) pairs per record; for >8 slots,
                        // continuation records have a blank count column.  Probe iss first
                        // and refill on extraction failure so we don't lose the first sat
                        // of each continuation to a stale (non-eof) trailing-whitespace state.
                        std::istringstream iss(line.substr(0, 60));
                        int num_slots;
                        iss >> num_slots;
                        for (int i = 0; i < num_slots; ++i) {
                            std::string sat;
                            int freq;
                            if (!(iss >> sat >> freq)) {
                                iss.clear();
                                if (!std::getline(file, line)) break;
                                iss.str(line.substr(0, std::min<size_t>(60, line.length())));
                                if (!(iss >> sat >> freq)) break;
                            }
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

                        // Parse-window clip: RINEX epochs are time-ordered, so once we
                        // pass the requested span past the first epoch the rest of the
                        // file is discarded — stop reading instead of materializing it.
                        if (max_obs_span_s_ > 0.0 &&
                            TimeUtils::continuousGpsSeconds(epoch_time) -
                                TimeUtils::continuousGpsSeconds(time_of_first_obs_) > max_obs_span_s_) {
                            break;
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
                                // Apply the processing.rinex_types filter: skip any
                                // observation whose RINEX-3 type letter (C/L/D/S)
                                // is not enabled.  Done before field extraction so
                                // we pay no per-obs decoding cost for filtered types.
                                if (obs_types[j].empty() ||
                                    !config_.isRinexTypeEnabled(obs_types[j][0])) {
                                    continue;
                                }
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
                                    // Optionally subtract the receiver-applied SYS / PHASE SHIFT
                                    // from raw phase observations (cycles).  Off by default —
                                    // most modern BIA products (CODE, CAS, GFZ) are calibrated
                                    // against the receiver-shifted phases as recorded in RINEX,
                                    // so leaving the shifts in place is the standard PPP
                                    // convention.  Opt in via @c model_corrections.apply_phase_shifts
                                    // when running with bias products explicitly calibrated for
                                    // the unshifted convention.  Code observations are unaffected
                                    // either way.
                                    if (config_.getModelCorrections().apply_phase_shifts
                                        && !obs_types[j].empty() && obs_types[j][0] == 'L') {
                                        auto sys_it = header_.phase_shifts.find(system);
                                        if (sys_it != header_.phase_shifts.end()) {
                                            auto code_it = sys_it->second.find(obs_types[j]);
                                            if (code_it != sys_it->second.end()) {
                                                value -= code_it->second;
                                            }
                                        }
                                    }
                                    obs.measurements[obs_types[j]] = value;

                                    const auto& n = config_.getDefaultMeasurementNoise();
                                    const double default_code_var = n.code_std_m * n.code_std_m;
                                    const double default_phase_var = n.phase_std_m * n.phase_std_m;
                                    const double default_doppler_var = n.doppler_std_m * n.doppler_std_m;

                                    double variance = default_code_var;
                                    if (!obs_types[j].empty()) {
                                        switch (obs_types[j][0]) {
                                        case 'L': variance = default_phase_var; break;
                                        case 'D': variance = default_doppler_var; break;
                                        case 'S': variance = 1.0; break;
                                        default:  variance = default_code_var; break;
                                        }
                                    }

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
                        // RINEX 2.x epoch + observation parsing.
                        //
                        // Epoch record (FORTRAN 1-indexed columns; 0-indexed below):
                        //   col 1-2   : 2-digit year
                        //   col 4-5   : month
                        //   col 7-8   : day
                        //   col 10-11 : hour
                        //   col 13-14 : minute
                        //   col 16-26 : seconds (F11.7)
                        //   col 28    : epoch flag (0 ok, 1 power fail, 2-5 events, 6 cycle slip)
                        //   col 29-31 : number of satellites (or records-to-follow for events)
                        //   col 32+   : satellite list, A1 + I2.2 per sat, max 12 per line.
                        //               Continuation lines pad cols 0-31 with spaces.
                        //
                        // Per-satellite observation block: ceil(N_obs_types / 5) lines, each
                        // with up to 5 observations laid out as F14.3, I1, I1 (16 chars each).
                        // 0.0 means "data not available".

                        // Strip a trailing CR so column-based slicing is robust on CRLF files.
                        if (!line.empty() && line.back() == '\r') line.pop_back();

                        // An epoch record starts with a space and has digits at columns 1-2
                        // (the 2-digit year). Anything else (orphan obs lines, blank rows from
                        // truncated event records, etc.) is silently skipped.
                        if (line.size() < 32 ||
                            !std::isdigit(static_cast<unsigned char>(line[1])) ||
                            !std::isdigit(static_cast<unsigned char>(line[2]))) {
                            continue;
                        }

                        int year = 0, month = 0, day = 0, hour = 0, minute = 0;
                        double seconds = 0.0;
                        int epoch_flag = 0, num_records = 0;

                        if (!parseFixedInt   (line,  1,  2, year)    ||
                            !parseFixedInt   (line,  4,  2, month)   ||
                            !parseFixedInt   (line,  7,  2, day)     ||
                            !parseFixedInt   (line, 10,  2, hour)    ||
                            !parseFixedInt   (line, 13,  2, minute)  ||
                            !parseFixedDouble(line, 15, 11, seconds)) {
                            log_manager_.getLogger("rinex_parser")->debug(
                                "Skipping malformed v2 epoch line: {}", line);
                            continue;
                        }
                        // The flag and num-records columns may be blank for sparse files;
                        // both default to zero on parse failure (which is the benign case).
                        parseFixedInt(line, 28, 1, epoch_flag);
                        parseFixedInt(line, 29, 3, num_records);

                        // Event records (flag 2-5) carry num_records *header-style* lines that
                        // we don't currently consume; skip them so we stay aligned.
                        if (epoch_flag >= 2 && epoch_flag <= 5) {
                            for (int i = 0; i < num_records; ++i) {
                                std::string skipline;
                                if (!std::getline(file, skipline)) break;
                            }
                            continue;
                        }
                        // Cycle-slip flag (6) records list satellites with no obs values; skip.
                        if (epoch_flag == 6) {
                            const int v2_sat_lines = (num_records + 11) / 12 - 1; // continuation lines only
                            for (int i = 0; i < v2_sat_lines; ++i) {
                                std::string skipline;
                                if (!std::getline(file, skipline)) break;
                            }
                            continue;
                        }
                        if (epoch_flag != 0 && epoch_flag != 1) {
                            log_manager_.getLogger("rinex_parser")->debug(
                                "Skipping v2 epoch with unsupported flag {}", epoch_flag);
                            continue;
                        }

                        // RINEX 2.x convention: years <80 belong to the 2000s, otherwise 1900s.
                        year = (year < 80) ? (2000 + year) : (1900 + year);

                        GnssTime epoch_time = TimeUtils::GnssTimeFromDate(
                            year, month, day, hour, minute, seconds, header_.time_system);
                        if (first_epoch) {
                            time_of_first_obs_ = epoch_time;
                            first_epoch = false;
                        }

                        // Parse-window clip (see the v3 path): stop once we pass the
                        // requested span past the first epoch — the time-ordered tail
                        // is discarded rather than parsed and copied.
                        if (max_obs_span_s_ > 0.0 &&
                            TimeUtils::continuousGpsSeconds(epoch_time) -
                                TimeUtils::continuousGpsSeconds(time_of_first_obs_) > max_obs_span_s_) {
                            break;
                        }

                        // Pull satellite IDs from the epoch line (cols 32+) plus continuation
                        // lines (also at cols 32+). Each ID is 3 chars: system letter + 2-digit PRN.
                        // Pre-RINEX-2.10 GPS-only files may emit a leading space in place of the
                        // system letter ("  5") — convert those to "G05".
                        std::vector<std::string> sat_ids;
                        sat_ids.reserve(static_cast<size_t>(num_records));

                        auto extractSatsFromLine = [&](const std::string& src) {
                            for (size_t pos = 32;
                                 pos + 3 <= src.size() &&
                                 sat_ids.size() < static_cast<size_t>(num_records);
                                 pos += 3) {
                                std::string sat = src.substr(pos, 3);
                                // All-blank slot signals the end of the on-line list.
                                if (sat.find_first_not_of(' ') == std::string::npos) break;

                                // Normalize blanks/system-letter quirks:
                                //   "  5" or " 5"  -> "G05"   (legacy GPS-only files)
                                //   "G 5"          -> "G05"
                                //   "G 5"  pieces  -> already 3 chars, just zero-pad PRN
                                if (sat[0] == ' ') sat[0] = 'G';
                                if (sat[1] == ' ') sat[1] = '0';
                                if (sat.size() < 3 || sat[2] == ' ') {
                                    // Defensive: shouldn't happen if file is well-formed.
                                    log_manager_.getLogger("rinex_parser")->debug(
                                        "Malformed sat slot at col {} in: {}", pos, src);
                                    continue;
                                }
                                sat_ids.push_back(sat);
                            }
                        };
                        extractSatsFromLine(line);

                        while (sat_ids.size() < static_cast<size_t>(num_records)) {
                            std::string contline;
                            if (!std::getline(file, contline)) break;
                            if (!contline.empty() && contline.back() == '\r') contline.pop_back();
                            // Continuation lines pad cols 0-31 with spaces; if they're shorter,
                            // pad so col 32 is reachable.
                            if (contline.size() < 32) contline.resize(32, ' ');
                            extractSatsFromLine(contline);
                        }

                        if (sat_ids.size() != static_cast<size_t>(num_records)) {
                            log_manager_.getLogger("rinex_parser")->warn(
                                "Epoch declared {} satellites; recovered {} IDs",
                                num_records, sat_ids.size());
                        }

                        // RINEX 2.x is type-shared. Use any populated entry; default to GPS.
                        const auto& shared_obs_types = header_.obs_types[System::GPS];
                        if (shared_obs_types.empty()) {
                            log_manager_.getLogger("rinex_parser")->error(
                                "No # / TYPES OF OBSERV declared in v2 header; cannot parse epoch.");
                            // Without an obs-types declaration we can't know how many lines
                            // per satellite to consume; bail on this epoch.
                            continue;
                        }
                        const size_t n_obs_types = shared_obs_types.size();
                        const size_t lines_per_sat = (n_obs_types + 4) / 5; // ceil(N/5)

                        const auto& noise_cfg = config_.getDefaultMeasurementNoise();
                        const double default_code_var    = noise_cfg.code_std_m    * noise_cfg.code_std_m;
                        const double default_phase_var   = noise_cfg.phase_std_m   * noise_cfg.phase_std_m;
                        const double default_doppler_var = noise_cfg.doppler_std_m * noise_cfg.doppler_std_m;

                        for (const auto& sat_id : sat_ids) {
                            // Slurp the per-satellite obs block into a single padded buffer so
                            // we can index obs[j] at a fixed offset regardless of trailing
                            // whitespace stripping in the source file.
                            std::string obs_buf;
                            obs_buf.reserve(80 * lines_per_sat);
                            bool short_block = false;
                            for (size_t k = 0; k < lines_per_sat; ++k) {
                                std::string oline;
                                if (!std::getline(file, oline)) {
                                    short_block = true;
                                    break;
                                }
                                if (!oline.empty() && oline.back() == '\r') oline.pop_back();
                                if (oline.size() < 80) oline.resize(80, ' ');
                                obs_buf += oline;
                            }
                            if (short_block) {
                                log_manager_.getLogger("rinex_parser")->warn(
                                    "Unexpected EOF while reading obs block for {}", sat_id);
                                break;
                            }

                            const System system = systemFromSatPrefix(sat_id[0]);
                            if (system == System::Unknown) {
                                log_manager_.getLogger("rinex_parser")->debug(
                                    "Unknown system for satellite '{}'; skipping epoch row", sat_id);
                                continue;
                            }

                            RinexObservation obs;
                            obs.satellite   = sat_id;
                            obs.system      = system;
                            obs.time        = epoch_time;
                            obs.receiver_id = header_.marker_name;

                            for (size_t j = 0; j < n_obs_types; ++j) {
                                // Apply the processing.rinex_types filter.  RINEX 2.x
                                // pseudoranges flagged as 'P' (encrypted P(Y) or
                                // GLONASS P) translate to 'C' codes in the v3 form
                                // the downstream pipeline expects, so map 'P' to
                                // 'C' before checking the filter; otherwise users
                                // setting rinex_types="CL" against a v2 file with
                                // P1/P2 would silently lose all pseudoranges.
                                if (!shared_obs_types[j].empty()) {
                                    char tletter = shared_obs_types[j][0];
                                    if (tletter == 'P') tletter = 'C';
                                    if (!config_.isRinexTypeEnabled(tletter)) continue;
                                }
                                const size_t line_idx = j / 5;
                                const size_t slot_idx = j % 5;
                                const size_t base = line_idx * 80 + slot_idx * 16;
                                if (base + 16 > obs_buf.size()) break;

                                std::string value_str = obs_buf.substr(base, 14);
                                const char lli_c = obs_buf[base + 14];
                                // (SNR strength indicator is at base+15; not currently consumed.)

                                trimInPlace(value_str);
                                if (value_str.empty()) continue;

                                double value = 0.0;
                                try { value = std::stod(value_str); }
                                catch (const std::exception&) {
                                    log_manager_.getLogger("rinex_parser")->debug(
                                        "Invalid v2 obs value '{}' for {} {}",
                                        value_str, sat_id, shared_obs_types[j]);
                                    continue;
                                }
                                // Per RINEX 2.x: 0.0 indicates "observation not made".
                                if (value == 0.0) continue;

                                // Translate the v2 2-char code into the v3 3-char form the
                                // downstream pipeline (PppModel, MeasurementModel, etc.) expects.
                                const std::string v3_code =
                                    translateV2ObsCodeToV3(shared_obs_types[j], system);

                                obs.measurements[v3_code] = value;

                                double variance = default_code_var;
                                switch (v3_code[0]) {
                                case 'L': variance = default_phase_var;   break;
                                case 'D': variance = default_doppler_var; break;
                                case 'S': variance = 1.0;                 break;
                                case 'C':
                                default:  variance = default_code_var;    break;
                                }
                                obs.variances[v3_code] = variance;

                                int lli = 0;
                                if (lli_c >= '0' && lli_c <= '9') lli = lli_c - '0';
                                obs.lli_flags[v3_code] = lli;
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
