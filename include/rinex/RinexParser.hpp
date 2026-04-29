#pragma once

#include <array>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <vector>
#include "config/GnssConfig.hpp"
#include "logging/LogManager.hpp"
#include "math/TimeUtils.hpp"
#include "common/Constants.hpp"
#include "common/PppTypes.hpp"
#include <Eigen/Dense>

namespace GnssProcessor {
    /**
     * @file RinexParser.hpp
     * @brief RINEX observation file parser declarations.
     *
     * Provides types and the @c RinexParser class used to parse RINEX
     * observation files (RINEX 2.x / 3.x style) and expose header metadata
     * and epoch observations for downstream processing.
     */

    // Forward declaration to avoid circular dependency
    class ProductParser;

    /**
     * @brief Holds a single epoch observation for one receiver.
     *
     * Measurements are stored by RINEX observation code (e.g., "C1C", "L1C").
     * Variances are expressed in squared meters for code/phase measures where
     * applicable. Loss-of-lock indicators (LLI) follow RINEX conventions and
     * are stored per-observation-code as integer flags.
     */
    struct RinexObservation {
        std::string satellite;                     /**< Satellite ID (e.g., "G01" or "G01" for GPS) */
        System system;                             /**< GNSS system enum for the satellite */
        GnssTime time;                             /**< Epoch time of this observation */
        std::map<std::string, double> measurements;/**< Map of observation code -> observed value */
        std::map<std::string, double> variances;   /**< Map of observation code -> variance (m^2) */
        std::map<std::string, int> lli_flags;      /**< Map of observation code -> loss-of-lock indicator */
        std::string receiver_id;                   /**< Receiver identifier (useful in network files) */
    };

    /**
     * @brief Parsed header fields from a RINEX observation file.
     *
     * Contains common RINEX header metadata required by processing routines:
     * version, time system, declared observation types per GNSS, receiver/antenna
     * descriptions, approximate receiver position (ECEF) and antenna offsets.
     */
    struct RinexHeader {
        double version{ 0.0 }; /**< RINEX file version (e.g., 3.04) */
        std::string time_system{ "GPS" }; /**< Declared time system (GPS, GAL, etc.) */
        std::map<System, std::vector<std::string>> obs_types; /**< Observation types per system (order preserved as declared) */
        std::optional<double> interval; /**< Measurement interval in seconds (if present in header) */
        std::string receiver_type; /**< Receiver model/type string (e.g., "TRIMBLE NETR9") */
        std::string antenna_type;  /**< Antenna type/model string (e.g., "TRM59800.00") */
        std::array<double, 3> approx_position{ 0.0, 0.0, 0.0 }; /**< Approx. receiver ECEF position (meters) */
        std::array<double, 3> antenna_delta_hen{ 0.0, 0.0, 0.0 }; /**< Antenna H/E/N offsets in meters */
        std::string marker_name;   /**< Marker name (station identifier) */
        std::string marker_number; /**< Marker number (optional station number) */
        double antenna_azimuth_deg{ 0.0 };   /**< Antenna azimuth orientation in degrees (if available) */
        double antenna_elevation_deg{ 0.0 }; /**< Antenna elevation orientation in degrees (if available) */
    };

    /**
     * @brief Parser for RINEX observation files.
     *
     * The parser reads header metadata and epoch observations and exposes
     * convenient accessors to header fields and parsed epochs. It interacts
     * with a @c ProductParser to locate/parse auxiliary products when needed.
     */
    class RinexParser {
    public:
        /**
         * @brief Construct a RINEX parser.
         * @param config Reference to the global GNSS processing configuration.
         * @param log_manager Reference to a log manager for parser logging.
         * @param product_parser Reference to product parser used for auxiliary data.
         */
        explicit RinexParser(const GnssConfig& config, LogManager& log_manager, ProductParser& product_parser);

        /**
         * @brief Parse the configured RINEX observation file.
         * @details Parses the header and all observation epochs. Parsed data is
         *          stored internally and accessible via accessors.
         * @return True on successful parse, false on error (errors are logged).
         */
        bool parse();

        /**
         * @brief Get the parsed RINEX header.
         * @return Const reference to the stored @c RinexHeader.
         */
        const RinexHeader& getHeader() const { return header_; }

        /**
         * @brief Get the parsed observation epochs.
         * @return Const reference to internal vector of @c RinexObservation entries.
         */
        const std::vector<RinexObservation>& getObservations() const { return observations_; }

        /**
         * @brief Validate that measurement interval is reasonable.
         * @details Acceptable intervals: 1 Hz (1.0 s), 30 s, etc. This routine
         *          enforces configured constraints (max 100 Hz, min 300 s) and
         *          returns false when header interval is outside expected bounds.
         * @return True if measurement interval is valid or not specified, false otherwise.
         */
        bool validateMeasurementRate() const;

        /*
         * Receiver/antenna metadata accessors
         */

        /** @brief Get receiver antenna type string from header. */
        std::string getReceiverAntennaType() const { return header_.antenna_type; }

        /** @brief Get antenna H/E/N offsets as Eigen vector (meters). */
        Eigen::Vector3d getAntennaDeltaHen() const {
            return Eigen::Vector3d(header_.antenna_delta_hen[0], header_.antenna_delta_hen[1], header_.antenna_delta_hen[2]);
        }

        /** @brief Get antenna azimuth (degrees) from header. */
        double getReceiverAntennaAzimuthDeg() const { return header_.antenna_azimuth_deg; }

        /** @brief Get antenna elevation (degrees) from header. */
        double getReceiverAntennaElevationDeg() const { return header_.antenna_elevation_deg; }

        /** @brief Get receiver type/model string from header. */
        std::string getReceiverType() const { return header_.receiver_type; }

        /** @brief Get receiver serial number parsed from header (if present). */
        std::string getReceiverSerialNo() const { return receiver_serial_no_; }

        /** @brief Get marker name from header. */
        std::string getMarkerName() const { return header_.marker_name; }

        /** @brief Get marker number from header. */
        std::string getMarkerNumber() const { return header_.marker_number; }

        /** @brief Get approximate receiver ECEF position (meters). */
        Eigen::Vector3d getApproxPositionXyz() const {
            return Eigen::Vector3d(header_.approx_position[0], header_.approx_position[1], header_.approx_position[2]);
        }

        /** @brief Get the time of the first parsed observation epoch. */
        GnssTime getTimeOfFirstObs() const { return time_of_first_obs_; }

        /**
         * @brief Get GLONASS frequency slot mapping.
         * @return Map from satellite id string (e.g., "R01") to GLONASS frequency channel.
         */
        const std::map<std::string, int>& getGlonassFreqSlots() const { return glonass_freq_slots_; }

        /**
         * @brief Rescale GLONASS raw measurement to reference frequency.
         * @param observed_value Observed value to be rescaled (typically phase/code).
         * @param freq_type Frequency designator (e.g., "L1", "L2").
         * @param satellite Satellite identifier string (e.g., "R01").
         * @return Rescaled measurement value (same units as input).
         */
        /*
        double rescaleGlonassMeasurement(double observed_value, const std::string& freq_type,
            const std::string& satellite) const;
        */

        /**
         * @brief Return list of epoch times parsed from observations.
         * @return Vector of @c GnssTime values corresponding to observation epochs.
         */
        std::vector<GnssTime> getEpochTimes() const;

        /**
         * @brief Default GLONASS frequency channel assignments.
         * Use as fallback when RINEX header lacks GLONASS SLOT/FRQ records.
         * Note: These may change over time as satellites are replaced.
         */
        /*
        const std::unordered_map<std::string, int> DEFAULT_GLONASS_CHANNELS = {
            {"R01",  1}, {"R02", -4}, {"R03",  5}, {"R04",  6},
            {"R05",  1}, {"R06", -4}, {"R07",  5}, {"R08",  6},
            {"R09", -2}, {"R10", -7}, {"R11",  0}, {"R12", -1},
            {"R13", -2}, {"R14", -7}, {"R15",  0}, {"R16", -1},
            {"R17",  4}, {"R18", -3}, {"R19",  3}, {"R20",  2},
            {"R21",  4}, {"R22", -3}, {"R23",  3}, {"R24",  2},
        };
        */

        std::map<std::string, int> glonass_freq_slots_; /**< GLONASS satellite -> frequency slot mapping. */

    private:
        const GnssConfig& config_;       /**< Configuration reference (not owned). */
        LogManager& log_manager_;        /**< Log manager reference (not owned). */
        ProductParser& product_parser_;  /**< Product parser reference (not owned). */

        RinexHeader header_;             /**< Parsed header metadata. */
        std::vector<RinexObservation> observations_; /**< Parsed observation epochs. */
        std::string receiver_serial_no_; /**< Receiver serial number parsed from header (if any). */
        GnssTime time_of_first_obs_;     /**< Time of first observation epoch parsed. */

        /**
         * @brief Validate a RINEX 3.x observation code.
         * @param code Observation code string to validate (e.g., "C1C").
         * @param system GNSS system enum for which the code is validated.
         * @return Optional normalized observation code on success, or std::nullopt on invalid code.
         */
        std::optional<std::string> validateObsCode(const std::string& code, System system) const;

        /**
         * @brief Get GLONASS frequency (Hz) for a given satellite and frequency type.
         * @param freq_type Frequency label (e.g., "L1", "L2").
         * @param satellite Satellite identifier string (e.g., "R01").
         * @return Frequency in Hz used to rescale GLONASS measurements.
         */
        double getGlonassFrequency(const std::string& freq_type, const std::string& satellite) const;
    };

} // namespace GnssProcessor