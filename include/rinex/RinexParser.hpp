#pragma once

#include <array>
#include <climits>
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
#include "common/ObsValueMap.hpp"
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
     *
     * The three channels are @c ObsValueMap rather than @c std::map: they share
     * a single per-(file,system) obs-code layout (so the obs-code strings live
     * once, not once per node per observation) and store values in a flat array.
     * The container mirrors the @c std::map API used across the pipeline, so
     * lookups, iteration and @c operator[] writes are unchanged at call sites.
     * See @c common/ObsValueMap.hpp.
     */
    struct RinexObservation {
        std::string satellite;                     /**< Satellite ID (e.g., "G01" or "G01" for GPS) */
        System system;                             /**< GNSS system enum for the satellite */
        GnssTime time;                             /**< Epoch time of this observation */
        ObsValueMap<double> measurements;          /**< Observation code -> observed value */
        ObsValueMap<double> variances;             /**< Observation code -> variance (m^2) */
        ObsValueMap<int> lli_flags;                /**< Observation code -> loss-of-lock indicator */
        /**
         * @brief Phase-signal code ("L1C", ...) -> decoded lock time (ms).
         *
         * Populated ONLY by the RTCM MSM decoder (DF402 basic / DF407 extended
         * tables) so the epoch-assembly layer can derive the 0x1 loss-of-lock
         * LLI bit statefully via @c LockTimeTracker (RTB-3, armed by
         * @c realtime.msm_lli_from_locktime).  Empty (default-constructed, no
         * allocation) for RINEX-parsed and SBF-decoded observations, and
         * deliberately NOT attached by @c attachLayout — the RINEX bulk path
         * never writes it, so pre-sizing it there would only cost memory.
         */
        ObsValueMap<double> lock_time_ms;
        std::string receiver_id;                   /**< Receiver identifier (useful in network files) */
        int glonass_freq_channel{ INT_MIN };       /**< GLONASS FDMA frequency-channel number k (-7..+6),
                                                        carried from RTCM DF419 / 1020 DF039 so the FDMA
                                                        wavelength can be resolved per satellite.  INT_MIN
                                                        means "unknown" (non-GLONASS, or FCN not supplied). */

        /**
         * @brief Point all three channels at a shared per-system obs-code
         *        layout and size their value stores to it (all-absent).
         *
         * Used by the RINEX parser once the system's declared obs-type order is
         * known: subsequent @c operator[] writes for any declared code hit an
         * existing index with no per-observation string allocation and no
         * mutation of the shared layout.  Streaming producers (SBF/RTCM) skip
         * this and let @c operator[] grow a private layout on first write.
         */
        void attachLayout(const std::shared_ptr<ObsCodeIndex>& layout) {
            measurements.attachLayout(layout);
            variances.attachLayout(layout);
            lli_flags.attachLayout(layout);
        }
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

        /** Per-(system, observation-code) phase shift in cycles, parsed from
         *  RINEX 3 "SYS / PHASE SHIFT" header records.  The receiver applies
         *  these shifts to align inter-tracking-mode phase observations to
         *  a common reference convention.  The parser subtracts the recorded
         *  shift from the incoming phase value during observation parsing,
         *  yielding an unshifted phase consistent across receivers (and
         *  consistent with the convention BIA products are calibrated for).
         *  Outer key: System (GPS, Galileo, ...).  Inner key: full obs code
         *  (e.g. "L2W", "L8X").  Value: shift in cycles.  Records with an
         *  empty satellite list apply system-wide; per-satellite overrides
         *  are not currently supported (rare in practice). */
        std::map<System, std::map<std::string, double>> phase_shifts;
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
         * @brief Clip parsing to the first @p span_s seconds of observations.
         *
         * RINEX observation records are time-ordered, so once an epoch is more
         * than @p span_s past the file's first observation epoch the remainder
         * of the file is irrelevant to a windowed run and @c parse() stops
         * reading.  This avoids materializing (and copying) the discarded tail
         * for runs that only consume a short span (e.g. a network solve bounded
         * by @c processing.max_duration_s).  The header is always parsed in
         * full.
         *
         * @param span_s Maximum span in seconds from the first observation
         *               epoch to retain.  A value @c <= 0 (the default) means
         *               "no window" — parse the entire file, preserving the
         *               historical behavior for every caller that does not opt
         *               in.
         */
        void setMaxObservationSpanSeconds(double span_s) { max_obs_span_s_ = span_s; }

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

        /**
         * @brief Override the receiver antenna type.
         *
         * For live streams (SBF/RTCM) that carry no RINEX `ANT # / TYPE`
         * record, the header antenna type is empty and no receiver ATX entry
         * can be matched.  This setter seeds the field from configuration so
         * receiver PCO/PCV (and the `require_atx_coverage` gate) can resolve.
         * The post-processing path never calls this — the parsed RINEX header
         * always wins.
         *
         * @param type Antenna type string, e.g. "LEIAR25         NONE".
         */
        void setReceiverAntennaType(const std::string& type) { header_.antenna_type = type; }

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

        /** @brief Get approximate receiver ECEF position (meters).
         *
         * This is the header `APPROX POSITION XYZ` verbatim — by RINEX convention
         * the **marker/monument**, not the antenna.  Prefer @c getArpPositionXyz
         * for anything that seeds or compares against a position STATE.
         */
        Eigen::Vector3d getApproxPositionXyz() const {
            return Eigen::Vector3d(header_.approx_position[0], header_.approx_position[1], header_.approx_position[2]);
        }

        /**
         * @brief Get the antenna reference point ECEF position from the header (meters).
         *
         * `APPROX POSITION XYZ` reduced through `ANTENNA: DELTA H/E/N` via
         * @c Coordinates::markerToArp.  This is the frame every position state in
         * the engine lives in (ATX PCO/PCV are ARP-relative), so it — not the raw
         * marker — is what a filter seed, a rover a-priori, or a truth comparison
         * wants.  Returns the marker unchanged when the header carries no (or a
         * zero) eccentricity, which keeps live streams and 0/0/0 stations exact.
         *
         * @return ARP ECEF position (meters); zero vector when the header carried
         *         no `APPROX POSITION XYZ` (callers already guard on this).
         */
        Eigen::Vector3d getArpPositionXyz() const;

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
        long long zero_sentinels_skipped_{0}; /**< RINEX 3 written-0.0 values skipped this parse (guard armed via processing.rinex_skip_zero_values; logged at parse end — invariant #4 observability). */

        /**
         * @brief Per-system shared obs-code layouts, built lazily during parse.
         *
         * Keyed by GNSS system; each entry holds the declared (v3-translated)
         * obs-code order shared by every observation of that system.  Cleared at
         * the start of each @c parse().  The @c shared_ptr is copied into every
         * @c RinexObservation, so the layout outlives the parser when
         * observations are moved/copied into longer-lived stores.
         */
        std::map<System, std::shared_ptr<ObsCodeIndex>> obs_layouts_;

        /**
         * @brief Fetch (or build and cache) the shared layout for @p system.
         * @param system    GNSS system the observation belongs to.
         * @param v3_codes  Declared obs codes in header order, already in their
         *                  RINEX-3 form (verbatim for v3 files, translated for v2).
         */
        std::shared_ptr<ObsCodeIndex> getObsLayout(System system,
                                                   const std::vector<std::string>& v3_codes) {
            auto it = obs_layouts_.find(system);
            if (it != obs_layouts_.end()) return it->second;
            auto layout = ObsCodeIndex::fromCodes(v3_codes);
            obs_layouts_.emplace(system, layout);
            return layout;
        }
        std::string receiver_serial_no_; /**< Receiver serial number parsed from header (if any). */
        GnssTime time_of_first_obs_;     /**< Time of first observation epoch parsed. */
        double max_obs_span_s_{ 0.0 };   /**< Parse-window span (s) from the first epoch; <=0 means unbounded. */

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