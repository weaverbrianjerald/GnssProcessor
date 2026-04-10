#pragma once


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
#include "StochasticModel.hpp"
#include <Eigen/Dense>
#include <vector>
#include <map>
#include <string>
#include <deque>

namespace GnssProcessor {

    // -----------------------------------------------------------------------
    // Satellite geometry result bundle
    // -----------------------------------------------------------------------

    /**
     * @brief Groups all output values produced by a single call to
     *        Model::Geometry::computeSatelliteGeometry().
     *
     * Avoids passing 15+ individual references/values through the processing
     * pipeline and makes the interface between computeModels() and the
     * observation-processing helpers self-documenting.
     */
    struct SatGeometry {
        Eigen::Vector3d sat_pos  = Eigen::Vector3d::Zero();
        Eigen::Vector3d sat_vel  = Eigen::Vector3d::Zero();
        Eigen::Vector3d rx_pos   = Eigen::Vector3d::Zero();
        Eigen::Vector3d los      = Eigen::Vector3d::Zero();
        double elevation         = 0.0;
        double azimuth           = 0.0;
        double range             = 0.0;
        double tropo_zhd         = 0.0;
        double sat_clock         = 0.0;
        double shapiro_delay     = 0.0;
        double sagnac_corr       = 0.0;
        double atm_loading_corr  = 0.0;
        double relativistic_corr = 0.0;
        double solid_tide        = 0.0;
        double mf_w              = 0.0;
        double mf_d              = 0.0;
    };

    // -----------------------------------------------------------------------
    // PppModel
    // -----------------------------------------------------------------------

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
         * @brief Check whether the BIA product has an entry for a satellite+signal pair.
         *
         * Delegates to GnssCorrections::hasBiasEntry().
         *
         * @param satellite Satellite identifier (e.g. "G01").
         * @param signal    RINEX observation code (e.g. "C7Q").
         * @return True if BIA coverage exists for the pair.
         */
        bool hasBiasEntry(const std::string& satellite, const std::string& signal) const {
            return gnss_corrections_.hasBiasEntry(satellite, signal);
        }

        /**
         * @brief Check whether the receiver antenna has ATX coverage for a signal.
         *
         * Delegates to Antenna::hasReceiverAtxEntry().
         *
         * @param signal RINEX observation code (e.g. "L5Q").
         * @param system GNSS system of the observation.
         * @return True if the receiver antenna model has PCO data for this signal.
         */
        bool hasReceiverAtxEntry(const std::string& signal, System system) const {
            return antenna_.hasReceiverAtxEntry(signal, system);
        }

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
        StochasticModelWeight stochastic_model_; /**< Stochastic (weighting) model for variance computation. */

        const std::map<std::string, std::vector<std::string>> signal_priorities_; /**< Signal priority lists (by system). */
        std::vector<double> klobuchar_coeffs_;  /**< Klobuchar coefficients (if available) */

        // -------------------------------------------------------------------
        // Signal selection helpers (decomposed from selectSignals)
        // -------------------------------------------------------------------

        /**
         * @brief Top-level signal selection: chooses observation pairs or singles.
         *
         * Delegates to buildFrequencyBandMap(), selectUncombinedSignals(),
         * selectIonoFreePairs() and selectSingleSignalFallback() as needed.
         */
        bool selectSignals(const RinexObservation& obs, const std::string& system_str,
            std::vector<std::pair<std::string, std::string>>& selected_signals,
            bool& is_iono_free, std::string& comb_type);

        /**
         * @brief Map available RINEX measurements to frequency bands.
         *
         * For each configured signal priority that is present in \p measurements,
         * records the best code and best phase signal per frequency band.
         *
         * @param measurements Available measurements.
         * @param system_str   GNSS system name (e.g. "GPS").
         * @param signals      Priority-ordered signal list for the system.
         * @param[out] freq_band_to_signal  Map from band name to (best_code, best_phase).
         * @param[out] freq_bands           Frequency bands in discovery order.
         * @return true if at least one band was populated.
         */
        bool buildFrequencyBandMap(const std::map<std::string, double>& measurements,
            const std::string& system_str,
            const std::vector<std::string>& signals,
            std::map<std::string, std::pair<std::string, std::string>>& freq_band_to_signal,
            std::vector<std::string>& freq_bands) const;

        /**
         * @brief Select individual code/phase signals per band (PPP-RTK uncombined).
         */
        bool selectUncombinedSignals(const RinexObservation& obs, const std::string& system_str,
            const std::map<std::string, std::pair<std::string, std::string>>& freq_band_to_signal,
            const std::vector<std::string>& freq_bands,
            std::vector<std::pair<std::string, std::string>>& selected_signals) const;

        /**
         * @brief Select best iono-free code+phase pairs from available bands.
         */
        bool selectIonoFreePairs(const RinexObservation& obs, const std::string& system_str,
            const std::vector<std::string>& signals,
            const std::map<std::string, std::pair<std::string, std::string>>& freq_band_to_signal,
            const std::vector<std::string>& freq_bands,
            std::vector<std::pair<std::string, std::string>>& selected_signals) const;

        /**
         * @brief Fall back to best available single code + single phase signal.
         */
        void selectSingleSignalFallback(const RinexObservation& obs,
            const std::vector<std::string>& signals,
            std::vector<std::pair<std::string, std::string>>& selected_signals) const;

        /**
         * @brief Determine which observation types (keys) to process from the measurements map.
         */
        std::vector<std::string> selectObservationTypes(const std::map<std::string, double>& measurements,
            System system, const std::string& system_str) const;

        // -------------------------------------------------------------------
        // Observation processing helpers
        // -------------------------------------------------------------------

        /**
         * @brief Compute the base modeled range value shared by iono-free and uncombined paths.
         *
         * Includes geometric range, satellite clock, relativistic correction,
         * dry troposphere, antenna PCO/PCV, code/phase bias, Shapiro delay,
         * atmospheric loading, solid tide, and pole tide.
         *
         * @param obs           RINEX observation (for satellite ID, system, time).
         * @param ppp_obs       PppObservation with LOS vector and mapping factors.
         * @param geom          Pre-computed satellite geometry.
         * @param signal_type   Signal key (e.g. "C1W", "L1C") for antenna/bias lookup.
         * @return Base modeled value in meters.
         */
        double computeBaseModeledValue(const RinexObservation& obs,
            const PppObservation& ppp_obs,
            const SatGeometry& geom,
            const std::string& signal_type) const;

        /**
         * @brief Compute pole-tide LOS projection shared by iono-free and uncombined paths.
         * @param rx_pos Receiver ECEF position.
         * @param time   Epoch time.
         * @param los    LOS unit vector (receiver?satellite).
         * @return Pole tide correction projected onto the LOS (meters).
         */
        double computePoleTideProjection(const Eigen::Vector3d& rx_pos,
            const GnssTime& time,
            const Eigen::Vector3d& los) const;

        /**
         * @brief Process a single iono-free (or composed) observation and fill a PppObservation.
         */
        bool processIonoFreeObservation(const RinexObservation& obs, PppState& state,
            PppObservation& ppp_obs, const std::string& system_str,
            const std::vector<std::pair<std::string, std::string>> selected_signals,
            const std::string& comb_type, const SatGeometry& geom);

        /**
         * @brief Process a single iono-free code or phase pair and write results into ppp_obs.
         *
         * Extracted from the inner lambda of processIonoFreeObservation.
         *
         * @return true if the pair was successfully modeled.
         */
        bool processIonoFreePair(const RinexObservation& obs, PppObservation& ppp_obs,
            const std::string& system_str,
            const std::pair<std::string, std::string>& sig_pair,
            char meas_type,
            const SatGeometry& geom,
            double pole_tide_corr,
            const std::string& comb_type);

        /**
         * @brief Process a single uncombined observation and fill a PppObservation.
         */
        bool processUncombinedObservation(const RinexObservation& obs, PppState& state,
            PppObservation& ppp_obs, const std::string& system_str,
            const std::vector<std::pair<std::string, std::string>>& selected_signals,
            const SatGeometry& geom);

    };

} // namespace GnssProcessor