#pragma once
#ifndef PPPMODEL_HPP
#define PPPMODEL_HPP

/**
 * @file PppModel.hpp
 * @brief PPP/PPP-RTK measurement modelling and correction utilities.
 *
 * The PppModel encapsulates geometry, atmospheric models and GNSS corrections
 * required to form modeled observations (code and carrier phase) used by the
 * estimator. It depends on configuration, product data, a RINEX parser and the
 * StateManager for retrieving/setting state-related information.
 */

#include "GnssConfig.hpp"
#include "ProductParser.hpp"
#include "LogManager.hpp"
#include "RinexParser.hpp"
#include "StateManager.hpp"
#include "PppTypes.hpp"
#include "Planetary.hpp"
#include "Ionosphere.hpp"
#include "Antenna.hpp"
#include "ModelGeometry.hpp"
#include <Eigen/Dense>
#include <vector>
#include <map>
#include <string>
#include <deque>

namespace GnssProcessor {

    /**
     * @class PppModel
     * @brief High-level PPP model that computes corrected observations and delays.
     *
     * Responsibilities:
     *  - Combine geometry and correction models (satellite orbits/clocks, tides, loading).
     *  - Apply troposphere and ionosphere models and compute iono-free combinations.
     *  - Provide helpers for GLONASS frequency rescaling and signal selection.
     *
     * PppModel is intended to be used by measurement processing code which
     * supplies RINEX observations and a current PPP state; the model returns
     * per-observation modeled values and ancillary diagnostics.
     */
    class PppModel {
    public:
        /**
         * @brief Construct the PppModel.
         *
         * @param config Read-only configuration providing defaults and parameters.
         * @param product_parser Parser/loader for external products (SP3, CLK, ERP, etc.).
         * @param log_manager Reference to the logging manager used for diagnostics.
         * @param rinex_parser Reference to the RINEX parser providing observation metadata.
         * @param state_manager Reference to StateManager for accessing/adding ambiguities and state values.
         */
        PppModel(const GnssConfig& config, ProductParser& product_parser, LogManager& log_manager,
            const RinexParser& rinex_parser, StateManager& state_manager);

        /**
         * @brief Initialize a fresh PPP state structure from configuration defaults.
         *
         * Sets initial position, clock, troposphere and ionosphere entries in \p state
         * to suitable defaults (and initializes ambiguity container if required).
         *
         * @param state Output PppState to initialize.
         */
        void initializeState(PppState& state);

        /**
         * @brief Compute modeled observations for a set of RINEX observations.
         *
         * For each input observation this routine computes satellite geometry,
         * applies clock/orbit/bias/troposphere/ionosphere corrections and fills
         * the output vector \p modeled_obs with modeled observables ready for
         * residual formation against measured values.
         *
         * @param observations Vector of RINEX observation records for the epoch.
         * @param state Current PPP state used for geometry and corrections (will not be modified).
         * @param modeled_obs Output vector filled with modeled observations (one entry per processed observable).
         * @return true if computation succeeded for at least one observation; false on catastrophic failure.
         */
        bool computeModels(const std::vector<RinexObservation>& observations, PppState& state, std::vector<PppObservation>& modeled_obs);

        /**
         * @brief Compute ionospheric delay for a single observation path.
         *
         * Uses the internal Ionosphere model (or Klobuchar fallback) to estimate
         * the slant ionospheric delay (meters) for the specified satellite/receiver geometry
         * and signal frequency type.
         *
         * @param time GNSS time of the observation.
         * @param rx_pos Receiver ECEF position (meters).
         * @param sat_pos Satellite ECEF position (meters).
         * @param freq_type Frequency or signal identifier string (e.g., "L1", "C1C").
         * @param system GNSS system enumeration (GPS, GLONASS, Galileo, etc.).
         * @param measurements Map of measurement code/phase values (if model requires observed values).
         * @return Estimated ionospheric delay along the ray path in meters.
         */
        double computeIonosphericDelay(const GnssTime& time, const Eigen::Vector3d& rx_pos,
            const Eigen::Vector3d& sat_pos, const std::string& freq_type,
            System system, const std::map<std::string, double>& measurements) const;

        /**
         * @brief Rescale GLONASS observation values to a common frequency reference.
         *
         * GLONASS uses frequency-division multiple access; this helper rescales
         * the raw measurement to the nominal frequency reference expected by the
         * rest of the processing chain.
         *
         * @param observed_data Raw observed value (meters for code/phase) to rescale.
         * @param freq_type Frequency identifier string (e.g., "L1", "L2").
         * @param system GNSS system (should be GLONASS for valid rescaling).
         * @param satellite Satellite identifier string (e.g., "R01").
         * @return Rescaled measurement in meters.
         */
        double rescaleGlonassObservation(double observed_data, const std::string& freq_type,
            System system, const std::string& satellite) const;

        /**
         * @brief Access the RINEX parser used by this model.
         * @return Const reference to the configured RinexParser.
         */
        const RinexParser& getRinexParser() const;

        /**
         * @brief Compute ionosphere-free (LC) combination from two signals.
         *
         * Produces the standard ionosphere-free linear combination of two frequency
         * observations (e.g., L1/L2 or C1C/C2W), returning the combined observable in meters.
         *
         * @param measurements Map of measurement name -> value (meters).
         * @param system GNSS system of the satellite producing the measurements.
         * @param signal1 First signal identifier (string key present in \p measurements).
         * @param signal2 Second signal identifier (string key present in \p measurements).
         * @return Ionosphere-free combined observable value in meters.
         */
        double computeIonoFreeCombination(const std::map<std::string, double>& measurements,
            System system, const std::string& signal1, const std::string& signal2) const;

        /**
         * @brief Get read-only access to the internal geometry helper.
         * @return Const reference to the Geometry instance used for satellite/receiver geometry computations.
         */
        const Model::Geometry& getGeometry() const { return geometry_; }

        /**
         * @brief Get mutable access to the internal geometry helper.
         * @return Reference to the Geometry instance, allowing modifications such as resetting phase wind-up state.
         */
        Model::Geometry& getGeometry() { return geometry_; }

        /**
         * @brief Reset phase wind-up state for a specific satellite.
         * @param satellite Satellite identifier string (e.g. "G01").
         *
         * Convenience method that delegates to the internal Geometry/GnssCorrections.
         * Call this when a cycle slip or ambiguity reset is detected.
         */
        void resetPhaseWindUp(const std::string& satellite) { antenna_.resetPhaseWindUp(satellite); }

        /**
         * @brief Reset phase wind-up state for all satellites.
         *
         * Convenience method that delegates to the internal Geometry/GnssCorrections.
         * Call this on a complete filter reset.
         */
        void resetAllPhaseWindUp() { antenna_.resetAllPhaseWindUp(); }

        /**
         * @brief Default destructor.
         */
        ~PppModel() = default;

    private:
        const GnssConfig& config_;             /**< Reference to configuration (read-only). */
        ProductParser& product_parser_;        /**< Product parser providing SP3/CLK/ERP/etc. */
        LogManager& log_manager_;              /**< Logging manager used for diagnostic messages. */
        const RinexParser& rinex_parser_;      /**< RINEX parser for observation metadata. */
        StateManager& state_manager_;          /**< State manager for ambiguity and state bookkeeping. */

        PppUtils ppp_utils_;                   /**< Utility helpers for PPP math and constants. */
        Ionosphere ionosphere_;                /**< Ionospheric model instance. */
        Model::Geometry geometry_;             /**< Geometry and satellite interpolation helpers. */
        Antenna antenna_;                      /**< Antenna model for receiver and satellite corrections. */
        GnssCorrections gnss_corrections_;     /**< Container for clock/orbit/bias/erp corrections. */

        const std::map<std::string, std::vector<std::string>> signal_priorities_; /**< Signal priority lists (by system). */
        std::vector<double> klobuchar_coeffs_;  /**< Klobuchar coefficients (if available) */

        /**
         * @brief Select best signals to use for this observation record.
         *
         * Based on configured signal priorities and the observation contents,
         * this routine selects pairs or single signals to attempt processing.
         *
         * @param obs Input RINEX observation record.
         * @param system_str String name of the GNSS system (e.g., "GPS").
         * @param selected_signals Output vector of selected signals. Each element is
         *        a pair: (primary_signal, secondary_signal) where secondary may be empty for single-signal processing.
         * @param is_iono_free Output flag set to true when the selected pair represents an iono-free combination.
         * @param comb_type Output string describing the combination type (e.g., "LC" for iono-free).
         * @return true if at least one useful signal selection was made, false otherwise.
         */
        bool selectSignals(const RinexObservation& obs, const std::string& system_str,
            std::vector<std::pair<std::string, std::string>>& selected_signals,
            bool& is_iono_free, std::string& comb_type);

        /**
         * @brief Determine which observation types (keys) to process from the measurements map.
         *
         * This helper returns a prioritized list of observation keys (strings) that
         * should be used for modeling given the available measurements and system.
         *
         * @param measurements Map of measurement name -> value.
         * @param system GNSS system of the satellite.
         * @param system_str Human-readable system string used for logging/selection.
         * @return Vector of measurement keys to attempt processing in priority order.
         */
        std::vector<std::string> selectObservationTypes(const std::map<std::string, double>& measurements,
            System system, const std::string& system_str) const;

        /**
         * @brief Process a single iono-free (or composed) observation and fill a PppObservation.
         *
         * This internal routine performs the bulk of per-observation modeling when an
         * ionosphere-free combination has been requested: it computes geometry, applies
         * satellite and receiver corrections, computes troposphere and relativistic terms,
         * and populates \p ppp_obs with modeled values and metadata.
         *
         * @param obs Input RINEX observation record.
         * @param state Current PPP state used for modeling.
         * @param ppp_obs Output modeled observation structure to populate.
         * @param system_str System identifier string (e.g., "GPS").
         * @param selected_signals Vector of selected signal pairs for the observation.
         * @param comb_type Combination type string describing the processing (e.g., "LC").
         * @param rx_pos Receiver ECEF position used for geometry (meters).
         * @param sat_pos Satellite ECEF position (meters).
         * @param sat_vel Satellite ECEF velocity (m/s).
         * @param elevation Elevation angle in radians (or degrees according to project convention).
         * @param azimuth Azimuth angle in radians.
         * @param rx_antenna_type Receiver antenna type string for antenna corrections.
         * @param range Geometric range between receiver and satellite (meters).
         * @param tropo_delay Modeled tropospheric slant delay (meters).
         * @param sat_clock Satellite clock correction (seconds or meters depending on convention).
         * @param shapiro_delay Shapiro delay (meters).
         * @param sagnac_corr Sagnac correction (meters).
         * @param atm_loading_corr Atmospheric loading correction (meters).
         * @param relativistic_corr Relativistic correction (meters).
         * @param solid_tide Solid Earth tide correction (meters).
         * @return true if the observation was successfully processed and \p ppp_obs populated; false otherwise.
         */
        bool processIonoFreeObservation(const RinexObservation& obs, PppState& state,
            PppObservation& ppp_obs, const std::string& system_str,
            const std::vector<std::pair<std::string, std::string>> selected_signals,
            const std::string& comb_type, const Eigen::Vector3d& rx_pos,
            const Eigen::Vector3d& sat_pos, const Eigen::Vector3d& sat_vel,
            double elevation, double azimuth,
            const std::string& rx_antenna_type, double range, double tropo_delay,
            double sat_clock, double shapiro_delay, double sagnac_corr,
            double atm_loading_corr, double relativistic_corr, double solid_tide);

        /**
         * @brief Form ionosphere-free combined measurements (observed + variance).
         *
         * Extracts and forms LC (iono-free) combinations from the provided raw
         * RINEX measurements according to the supplied selected signal pairs.
         * The function converts carrier-phase cycles to meters when required and
         * computes the combination variance (pre elevation/mapping scaling).
         *
         * @param obs Input RINEX observation record.
         * @param system GNSS system enum for frequency lookup.
         * @param selected_signals Vector of (sig1, sig2) pairs to form combinations from.
         * @param out_observed Output map filled with combined observed values (key = "SIG1_SIG2").
         * @param out_variances Output map filled with combined variances (same keys as out_observed).
         * @return true if at least one combined measurement was formed; false otherwise.
         */
        bool formIonoFreeMeasurements(const RinexObservation& obs, System system,
            const std::vector<std::pair<std::string, std::string>>& selected_signals,
            std::map<std::string, double>& out_observed,
            std::map<std::string, double>& out_variances) const;

    };

} // namespace GnssProcessor

#endif // PPPMODEL_HPP