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

#include "config/GnssConfig.hpp"
#include "products/ProductParser.hpp"
#include "logging/LogManager.hpp"
#include "rinex/RinexParser.hpp"
#include "filter/StateManager.hpp"
#include "common/PppTypes.hpp"
#include "model/Planetary.hpp"
#include "atmosphere/Ionosphere.hpp"
#include "atmosphere/GlobalIonosphereMap.hpp"
#include "atmosphere/HigherOrderIonosphere.hpp"
#include "model/Antenna.hpp"
#include "model/ModelGeometry.hpp"
#include "model/StochasticModel.hpp"
#include <Eigen/Dense>
#include <filesystem>
#include <optional>
#include <vector>
#include <map>
#include <set>
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
        double otl_corr          = 0.0;   ///< Ocean tidal loading site displacement projected onto -LOS (m).
        double apl_corr          = 0.0;   ///< Atmospheric pressure loading (S1/S2 thermal tide) projected onto -LOS (m).
        double relativistic_corr = 0.0;
        double solid_tide        = 0.0;
        double mf_w              = 0.0;
        double mf_d              = 0.0;
        bool   broadcast_only    = false;   ///< Clock is broadcast-grade (metre level) → code-only (phase dropped).
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
         * @param drop_reasons Optional per-satellite drop-reason map.  When
         *        non-null, every satellite that fails to make it into
         *        @p modeled_obs gets an entry naming the reason
         *        (BelowMask / NoProduct / NoModel) so callers (notably the
         *        satellite-status trace logger) can surface actionable
         *        failure modes instead of lumping every drop into a single
         *        bucket.  Pass @c nullptr if drop tracking isn't needed.
         * @return true if computation succeeded for at least one observation; false on catastrophic failure.
         */
        bool computeModels(const std::vector<RinexObservation>& observations,
                           PppState& state,
                           std::vector<PppObservation>& modeled_obs,
                           std::map<std::string, DropReason>* drop_reasons = nullptr);

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
         * @return Reference to the Geometry instance.
         */
        Model::Geometry& getGeometry() { return geometry_; }

        /**
         * @brief Enable the phase wind-up diagnostic CSV trace.
         *
         * Convenience method that delegates to the internal Antenna /
         * GnssCorrections instance.  Call once at startup when
         * logging.enable_windup_trace is set.
         *
         * @param log_dir Directory where windup_trace.csv will be created.
         * @return True if the trace file was opened successfully.
         */
        bool enableWindUpTracing(const std::filesystem::path& log_dir) {
            return antenna_.enableWindUpTracing(log_dir);
        }

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
         * @brief Get read-only access to the internal GnssCorrections instance.
         * @return Const reference to the corrections helper.
         */
        const GnssCorrections& getCorrections() const { return gnss_corrections_; }

        /**
         * @brief Update the GLONASS FDMA frequency-slot map (satellite → channel k).
         *
         * Mirrors the constructor's one-shot seed from the RINEX "GLONASS SLOT /
         * FRQ #" header, but for the real-time path where slot assignments arrive
         * incrementally from the broadcast stream (RTCM 1020 DF039 / SBF GLONav).
         * Forwards to @c PppUtils so per-satellite GLONASS wavelengths resolve
         * from the live channel assignments instead of the hard-coded default
         * table.  Idempotent; safe to call whenever fresh ephemeris arrives.
         *
         * @param slots Map of GLONASS satellite id ("R01") to FDMA channel k (-7..+6).
         */
        void setGlonassSlots(const std::map<std::string, int>& slots) {
            ppp_utils_.setGlonassSlots(slots);
        }

        /**
         * @brief GLONASS FDMA channel k for a satellite, from the SEEDED slot map.
         *
         * Delegates to the model's own @c PppUtils, which @c setGlonassSlots has
         * populated from the RINEX header / live broadcast — so callers that hold
         * only a @c const @c PppModel& (e.g. StateInitializer) resolve the true
         * current channel rather than their own un-seeded default table.
         *
         * @param satellite GLONASS satellite id ("R07").
         * @return Channel k in [-7,+6], or -999 if the slot is unknown.
         */
        int getGlonassChannel(const std::string& satellite) const {
            return ppp_utils_.getGlonassChannel(satellite);
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
         * @brief Check whether both satellite and receiver ATX entries
         *        are fully populated for a (sat, signal) pair.
         *
         * Forwards to @c Antenna::hasFullAtxCoverage.  Used by the AR
         * pipeline to gate WL/NL constraints on signals lacking strict
         * ATX coverage when the user enables
         * @c require_atx_coverage_for_ar.
         *
         * @param satellite Satellite identifier (e.g. "G05").
         * @param signal    RINEX-3 observation code (e.g. "L5Q").
         * @param system    GNSS system enum.
         * @return True iff both sat and rx have finite PCO and PCV.
         */
        bool hasFullAtxCoverage(const std::string& satellite,
                                const std::string& signal,
                                System system) const {
            return antenna_.hasFullAtxCoverage(satellite, signal, system);
        }

        /**
         * @brief Check whether the ATX correction for a (sat, signal) is
         *        being computed via the frequency-fallback substitution
         *        chain rather than from an exact entry on both sides.
         *
         * Forwards to @c Antenna::wasAtxSubstituted.  Used by the AR-side
         * gate (@c restrict_ar_for_substituted_atx) and the per-ambiguity
         * process noise loosening logic (@c loosen_amb_q_for_substituted_atx).
         */
        bool wasAtxSubstituted(const std::string& satellite,
                                const std::string& signal,
                                System system) const {
            return antenna_.wasAtxSubstituted(satellite, signal, system);
        }

        /**
         * @brief Access the parsed ATX antenna entries.
         * @return Const reference to the antenna data vector.
         */
        const std::vector<AntennaData>& getAntennas() const {
            return antenna_.getAntennas();
        }

        /**
         * @brief Inject the GIM used for the higher-order ionosphere correction.
         *
         * The pointer is held non-owning; the @c GlobalIonosphereMap
         * lifetime is managed by @c Application (post-processing) or by
         * the baseline / real-time drivers.  Pass @c nullptr to disable
         * the correction (returns 0 m from
         * @c computeHigherOrderIonoCorrection regardless of the config
         * toggle).
         *
         * Safe to call multiple times.  Idempotent.  Has no effect unless
         * @c model_corrections.enable_higher_order_ionosphere is true.
         *
         * @param gim Pointer to a loaded GIM, or @c nullptr.
         */
        void setGim(const GlobalIonosphereMap* gim) { gim_ = gim; }

        /**
         * @brief Inject a non-owning SSR-correction store override (DI seam for
         *        the PPP-RTK closed-loop harness).
         *
         * Forwards to the internal @c Model::Geometry (SSR clock + orbit overlay)
         * and the model's own @c GnssCorrections.  Pass @c nullptr (the default
         * state) to use the process-wide singleton — every non-harness caller is
         * byte-identical.  The network closed-loop consumer points this at the
         * producer's OWNED store so a held-out station consumes the network's
         * freshly-emitted satellite clock without polluting global state.
         *
         * Delegates to @ref setSsrOrbitStore + @ref setSsrClockStore (RTA-8:
         * the two overlays are independently routable); this coupled form is
         * retained for every pre-split caller — notably the closed-loop
         * harness — and behaves exactly as before.
         *
         * @param store Non-owning store pointer (must outlive this model) or
         *        @c nullptr for the singleton.
         */
        void setSsrStore(const SsrCorrectionStore* store) {
            setSsrOrbitStore(store);
            setSsrClockStore(store);
        }

        /**
         * @brief RTA-8: route ONLY the SSR ORBIT overlay at @p store.
         *
         * Forwards to @c Model::Geometry::setSsrOrbitStore (the RAC-delta
         * application in @c getSatellitePV).  The clock path — geometry's
         * internal @c GnssCorrections and the model's own @c gnss_corrections_
         * — keeps its current routing.  This is how the standalone
         * network-consumer wiring keeps orbit corrections on the external RTS
         * stream (singleton) while clocks come from the injected
         * network-product store
         * (@c processing.network_consumer.orbit_source = "external").
         *
         * @param store Non-owning store pointer (must outlive this model) or
         *        @c nullptr for the singleton.
         */
        void setSsrOrbitStore(const SsrCorrectionStore* store) {
            geometry_.setSsrOrbitStore(store);
        }

        /**
         * @brief RTA-8: route ONLY the SSR CLOCK overlay at @p store.
         *
         * Covers BOTH satellite-clock consumers behind this model: geometry's
         * internal @c GnssCorrections (emission-time refinement) and the
         * model's own @c gnss_corrections_ (the per-observation clock in
         * @c computeModels) — the identical pair the coupled @ref setSsrStore
         * always routed together, minus the orbit overlay.
         *
         * @param store Non-owning store pointer (must outlive this model) or
         *        @c nullptr for the singleton.
         */
        void setSsrClockStore(const SsrCorrectionStore* store) {
            geometry_.setSsrClockStore(store);
            gnss_corrections_.setSsrStore(store);
        }

        /**
         * @brief Compute the per-signal I₂ + I₃ correction (m) to add
         *        to the modeled observable.
         *
         * Returns 0 in any of:
         *   - @c model_corrections.enable_higher_order_ionosphere is false;
         *   - no GIM has been injected (or the GIM failed to load);
         *   - the GIM has no usable VTEC for this line of sight (below
         *     horizon, sentinel cell, etc.).
         *
         * @param time         Epoch time of the observation.
         * @param rx_pos       Receiver ECEF position (m).
         * @param sat_pos      Satellite ECEF position (m).
         * @param frequency_hz Carrier frequency of the signal (Hz).
         * @param meas_type    @c 'C' for code (positive group delay) or
         *                     @c 'L' for phase (negative, scaled by 1/2
         *                     for I₂ and 1/3 for I₃).
         * @return Signed correction in metres.  Add directly to the
         *         per-signal modeled value.
         */
        double computeHigherOrderIonoCorrection(const GnssTime& time,
                                                const Eigen::Vector3d& rx_pos,
                                                const Eigen::Vector3d& sat_pos,
                                                double frequency_hz,
                                                char meas_type) const;

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
        std::optional<Ionosphere> ionosphere_; /**< Klobuchar fallback model.  Constructed only when @c model_corrections.enable_klobuchar_ionosphere is true; std::nullopt otherwise.  Constructing it pulls in another @c Model::Geometry plus a Klobuchar coefficient parse, both of which are wasted in IF / DD / PPP-RTK-uncombined where Klobuchar is never queried. */
        const GlobalIonosphereMap* gim_{ nullptr }; /**< Non-owning pointer to the GIM that supplies absolute STEC and IPP geometry for the higher-order correction.  Injected via @c setGim() by the Application driver once both the PppModel and the GIM exist.  Nullptr when higher-order ionosphere is disabled or no IONEX product resolved — in which case @c computeHigherOrderIonoCorrection returns 0 m and the modeled values reduce to the first-order-only path. */
        HigherOrderIonosphere higher_order_iono_; /**< Stateless physics helper for the second- and third-order ionospheric delay formulae.  Always constructed; @c computeHigherOrderIonoCorrection consults @c gim_ and the config toggle to decide whether to evaluate. */
        Model::Geometry geometry_;             /**< Geometry and satellite interpolation helpers. */
        Antenna antenna_;                      /**< Antenna model for receiver and satellite corrections. */
        GnssCorrections gnss_corrections_;     /**< Container for clock/orbit/bias/erp corrections. */
        StochasticModelWeight stochastic_model_; /**< Stochastic (weighting) model for variance computation. */

        const std::map<std::string, std::vector<std::string>> signal_priorities_; /**< Signal priority lists (by system). */
        std::vector<double> klobuchar_coeffs_;  /**< Klobuchar coefficients (if available) */

        /// Satellites already logged as phase-gated (broadcast-grade clock →
        /// code-only).  Without this the info line fires once per epoch per
        /// satellite for the whole run; we log once per satellite per session.
        mutable std::set<std::string> sats_phase_gated_;

        /**
         * @brief Per-constellation canonical signal cache.
         *
         * Keyed by constellation name ("GPS", "Galileo", ...) →
         * (band name, observation-type char {'C','L'}) → canonical signal
         * code (e.g. "C1W", "L5Q").  Populated by @c decideConstellationCanonicals
         * on the first epoch that the constellation has observations, then
         * locked for the rest of the run.  Used by @c buildFrequencyBandMap to
         * keep the canonical pick stable across epochs — so a brief dropout
         * of the priority canonical at low elevation doesn't switch the KF to
         * a different tracking attribute (e.g. L1W → L1C) and leak an
         * inter-channel-bias-contaminated ambiguity state.
         */
        std::map<std::string,
                 std::map<std::pair<std::string, char>, std::string>>
                constellation_canonical_;
        std::set<std::string> constellation_canonical_decided_; /**< Constellations whose canonical set is locked. */

        /**
         * @brief Decide the per-constellation canonical signal set from
         *        the first epoch's observations.
         *
         * For each constellation present in @p observations that hasn't
         * been decided yet, counts how many satellites observe each
         * priority-list signal at this epoch.  For each (band, obs_type)
         * the signal with the highest observation count wins; ties are
         * broken by priority order in @c signal_priorities_.  The
         * result is cached in @c constellation_canonical_ and the
         * constellation is marked decided so subsequent epochs are
         * no-ops.
         *
         * @param observations Observations for the current epoch.
         */
        void decideConstellationCanonicals(const std::vector<RinexObservation>& observations);

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
        bool buildFrequencyBandMap(const ObsValueMap<double>& measurements,
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
        std::vector<std::string> selectObservationTypes(const ObsValueMap<double>& measurements,
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
         * @brief Additive phase-variance term (m²) for a (sat, signal)
         *        whose receiver ATX correction was frequency-fallback-
         *        substituted.
         *
         * Returns 0 for code rows ('C…'), for non-substituted signals,
         * or when the feature is disabled
         * (@c model_corrections.atx_substitution_phase_std_m ≤ 0, the
         * default).  Otherwise returns @f$(\sigma_\mathrm{atx}/
         * \sin\,\mathrm{el})^2@f$ — a variance addend (independent error
         * sources add in variance) summed onto the stochastic-model
         * phase variance *before* any iono-free combination, so the IF
         * coefficients propagate it automatically.
         *
         * Targets the azimuthally-symmetric, elevation-dependent
         * receiver-PCV residual left by a substituted antenna pattern:
         * it is common across satellites and aliases into ZWD / station
         * height, and is *not* the per-satellite slowly-varying bias the
         * @c ambiguity_q_substituted_atx random walk already absorbs.
         * See that field's and @c atx_substitution_phase_std_m's
         * documentation for the full code-vs-ambiguity rationale.
         *
         * @param signal        RINEX-3 code (e.g. "L8X"); 'C…' rows return 0.
         * @param satellite     Satellite identifier (e.g. "E18").
         * @param system        GNSS system enum.
         * @param elevation_deg Satellite elevation (degrees).
         * @return Additive phase variance (m²), or 0 when not applicable.
         */
        double phaseAtxSubstitutionVarInflation(const std::string& signal,
            const std::string& satellite,
            System system,
            double elevation_deg) const;

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

    /**
     * @brief RINEX band digits activated by a @c FrequencyMode for a constellation.
     *
     * Free function exposed for use by signal-selection logic outside of
     * @c PppModel (most prominently @c MwSlipDetector when filtering raw
     * observations by frequency band).  Returns the set of digits that
     * appear in the second character of RINEX-3 observation codes — e.g.
     * GPS Dual → {'1', '2'}; Galileo Dual → {'1', '5'} (E1 + E5a); GPS
     * Triple → {'1', '2', '5'}.
     *
     * @param constellation Constellation name ("GPS", "Galileo", ...).
     * @param mode          Frequency mode (Dual / Triple / All).
     * @return Set of RINEX band digits; empty if the constellation is unknown.
     */
    std::set<char> getAllowedBandDigits(const std::string& constellation, FrequencyMode mode);

} // namespace GnssProcessor