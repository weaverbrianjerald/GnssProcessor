/*!
 * \file PppModel.cpp
 * \brief Implementation of PppModel: PPP/PPP-RTK observation modelling and corrections.
 *
 * This file implements the PppModel methods responsible for initializing PPP state,
 * computing satellite/receiver geometry, applying corrections (orbits, clocks, tides,
 * troposphere, ionosphere, antenna) and forming ionosphere-free combinations used by
 * downstream measurement/estimation components.
 *
 * The implementation is decomposed into focused helpers:
 *  - Signal selection:   buildFrequencyBandMap, selectUncombinedSignals,
 *                        selectIonoFreePairs, selectSingleSignalFallback.
 *  - Shared corrections: computeBaseModeledValue, computePoleTideProjection.
 *  - Observation paths:  processIonoFreeObservation / processIonoFreePair,
 *                        processUncombinedObservation.
 *
 * Logging is performed via the provided LogManager instance and the model relies on
 * ProductParser, RinexParser and StateManager for external data and state bookkeeping.
 */

#include "config/GnssConfig.hpp"
#include "model/PppModel.hpp"
#include "common/PppTypes.hpp"
#include "rinex/RinexParser.hpp"
#include "filter/StateManager.hpp"
#include "products/ProductParser.hpp"
#include "common/Constants.hpp"
#include "math/Coordinates.hpp"
#include "measurement/MeasurementModel.hpp"
#include "math/TimeUtils.hpp"
#include "products/ProductCache.hpp"
#include <algorithm>
#include <fstream>
#include <set>
#include <sstream>

namespace GnssProcessor {

// ===========================================================================
// File-scope frequency / band mapping tables
// ===========================================================================

namespace {

/// Map system display name to the single-char key used in ClockRefMap.
char systemNameToClockRefChar(const std::string& sys) {
    if (sys == "GPS")     return 'G';
    if (sys == "Galileo") return 'E';
    if (sys == "GLONASS") return 'R';
    if (sys == "BeiDou")  return 'C';
    if (sys == "QZSS")    return 'J';
    if (sys == "SBAS")    return 'S';
    return '\0';
}

/// Maps RINEX observation code prefixes to frequency band names per system.
const std::map<std::string, std::vector<std::pair<std::string, std::string>>>& getSystemToBands() {
    static const std::map<std::string, std::vector<std::pair<std::string, std::string>>> table = {
        {"GPS",     {{"C1", "L1"}, {"L1", "L1"}, {"C2", "L2"}, {"L2", "L2"}, {"C5", "L5"}, {"L5", "L5"}}},
        {"Galileo", {{"C1", "E1"}, {"L1", "E1"}, {"C5", "E5a"}, {"L5", "E5a"}, {"C7", "E5b"}, {"L7", "E5b"}, {"C6", "E6"}, {"L6", "E6"}, {"C8", "E5"}, {"L8", "E5"}}},
        {"GLONASS", {{"C1", "G1"}, {"L1", "G1"}, {"C2", "G2"}, {"L2", "G2"}, {"C3", "G3"}, {"L3", "G3"}, {"C4", "G1a"}, {"L4", "G1a"}, {"C6", "G2a"}, {"L6", "G2a"}}},
        {"BeiDou",  {{"C1", "B1C"}, {"L1", "B1C"}, {"C2", "B1I"}, {"L2", "B1I"}, {"C5", "B2a"}, {"L5", "B2a"}, {"C7", "B2b"}, {"L7", "B2b"}, {"C8", "B2ab"}, {"L8", "B2ab"}, {"C6", "B3"}, {"L6", "B3"}}},
        {"QZSS",    {{"C1", "L1"}, {"L1", "L1"}, {"C2", "L2"}, {"L2", "L2"}, {"C5", "L5"}, {"L5", "L5"}, {"C6", "L6"}, {"L6", "L6"}}},
        {"SBAS",    {{"C1", "L1"}, {"L1", "L1"}, {"C5", "L5"}, {"L5", "L5"}}}
    };
    return table;
}

/// Maps system name + SignalMode to the target frequency bands for pair formation.
const std::map<std::string, std::map<SignalMode, std::vector<std::string>>>& getBandMap() {
    static const std::map<std::string, std::map<SignalMode, std::vector<std::string>>> table = {
        {"GPS",     {{SignalMode::Dual, {"L1", "L2"}}, {SignalMode::Triple, {"L1", "L2", "L5"}}, {SignalMode::All, {"L1", "L2", "L5"}}}},
        {"Galileo", {{SignalMode::Dual, {"E1", "E5a"}}, {SignalMode::Triple, {"E1", "E5a", "E5b"}}, {SignalMode::All, {"E1", "E5a", "E5b", "E6", "E5"}}}},
        {"GLONASS", {{SignalMode::Dual, {"G1", "G2"}}, {SignalMode::Triple, {"G1", "G2", "G3"}}, {SignalMode::All, {"G1", "G2", "G3", "G1a", "G2a"}}}},
        {"BeiDou",  {{SignalMode::Dual, {"B1I", "B3"}}, {SignalMode::Triple, {"B1I", "B3", "B2a"}}, {SignalMode::All, {"B1C", "B1I", "B2a", "B2b", "B2ab", "B3"}}}},
        {"QZSS",    {{SignalMode::Dual, {"L1", "L2"}}, {SignalMode::Triple, {"L1", "L2", "L5"}}, {SignalMode::All, {"L1", "L2", "L5", "L6"}}}},
        {"SBAS",    {{SignalMode::Dual, {"L1", "L5"}}, {SignalMode::Triple, {"L1", "L5"}}, {SignalMode::All, {"L1", "L5"}}}}
    };
    return table;
}

/// Human-readable label for a SignalMode value.
const char* signalModeLabel(SignalMode m) {
    switch (m) {
    case SignalMode::Dual:   return "Dual";
    case SignalMode::Triple: return "Triple";
    case SignalMode::All:    return "All";
    default:                 return "Unknown";
    }
}

} // anonymous namespace

    /**
     * @brief Construct a PppModel instance.
     *
     * Initializes internal helpers (geometry, ionosphere, antenna, corrections) and
     * captures references to configuration, product parser, rinex parser and state manager.
     *
     * @param config Read-only configuration object.
     * @param product_parser Product parser providing external products (SP3/CLK/...).
     * @param log_manager Logging manager used for informational and diagnostic output.
     * @param rinex_parser RINEX parser providing observation metadata and receiver info.
     * @param state_manager StateManager used for ambiguity bookkeeping and state access.
     */
    PppModel::PppModel(const GnssConfig& config, ProductParser& product_parser,
        LogManager& log_manager, const RinexParser& rinex_parser,
        StateManager& state_manager)
        : config_(config),
        product_parser_(product_parser),
        log_manager_(log_manager),
        rinex_parser_(rinex_parser),
        state_manager_(state_manager),
        ppp_utils_(log_manager),
        // Pass cache reference to geometry
        geometry_(config, ProductCache::getInstance(), rinex_parser, log_manager),
        // GnssCorrections uses cache references
        gnss_corrections_(log_manager, config,
            ProductCache::getInstance().getClocks(),
            ProductCache::getInstance().getErps(),
            ProductCache::getInstance().getBiases(),
            rinex_parser),
        // ionosphere_ is std::optional — constructed in the body below
        // only when the Klobuchar fallback is actually requested.
        antenna_(config, product_parser, log_manager, rinex_parser),
        stochastic_model_(config, log_manager),
        signal_priorities_(config.getSignalPriorities()),
        // Initialize reference from cache
        klobuchar_coeffs_(ProductCache::getInstance().getKlobucharCoeffs())
    {
        // Optional Klobuchar ionospheric-delay model.  Skipped (along with
        // its inner Geometry construction and coefficient parse) for the
        // common configs where it would be a no-op anyway: iono-free
        // combinations, DD short-baseline, and PPP-RTK uncombined (per-
        // satellite slant iono states).  Set
        // model_corrections.enable_klobuchar_ionosphere=true for single-
        // frequency PPP runs that genuinely need the fallback.
        if (config.getModelCorrections().enable_klobuchar_ionosphere) {
            ionosphere_.emplace(config, log_manager, product_parser, rinex_parser);
        }

        // Connect GLONASS frequency slots from RINEX parser to PppUtils
        ppp_utils_.setGlonassSlots(rinex_parser_.getGlonassFreqSlots());
        log_manager_.getLogger("ppp_model")->info(
            "PppModel initialized: mode={}, elev_mask={}°, klobuchar_coeffs={}",
            config.getProcessingMode() == ProcessingMode::PPP ? "PPP" : "PPP-RTK",
            config.getElevationMaskDeg(),
            klobuchar_coeffs_.size());
    }

    /**
     * @brief Initialize a PppState with sensible defaults from RINEX and configuration.
     *
     * Populates position (optionally applying receiver antenna H/E/N offset from RINEX),
     * epoch time, clock bias, troposphere ZWD and (for PPP-RTK) a starting ionospheric zenith delay.
     * If an existing state looks valid it is left unchanged.
     *
     * @param state Output PppState to initialize (modified in-place).
     */
    void PppModel::initializeState(PppState& state) {
        size_t core_size = 5; // Position(3) + Clock(1) + Tropo(1) for both PPP and PPP-RTK
        if (state.position.norm() == 0.0 || state.position.norm() > 1e9 || state.position.hasNaN() ||
            state.covariance.rows() != static_cast<long>(core_size)) {
            // If a user-supplied reference position is configured, use it directly
            // (bypasses RINEX APPROX POSITION and antenna offset). This is intended
            // for fixed-station residual analysis with zero position process noise.
            if (config_.hasReferencePosition()) {
                const auto& rp = config_.getReferencePositionEcef();
                state.position = Eigen::Vector3d(rp[0], rp[1], rp[2]);
                log_manager_.getLogger("ppp_model")->info(
                    "Using user-supplied reference position: ({:.4f}, {:.4f}, {:.4f}) m",
                    state.position.x(), state.position.y(), state.position.z());
            }
            else if (Eigen::Vector3d marker_pos = rinex_parser_.getApproxPositionXyz();
                     marker_pos.norm() > 1e3 && !marker_pos.hasNaN()) {
                // Apply antenna H/E/N offset to get antenna phase center position.
                // RINEX APPROX POSITION XYZ is the marker position.
                // ANTENNA: DELTA H/E/N gives the offset from marker to ARP in local frame.
                // Note: H = Up, E = East, N = North
                Eigen::Vector3d hen = rinex_parser_.getAntennaDeltaHen();

                if (hen.norm() > 1e-6) {
                    // Use centralized Coordinates for proper geodetic conversion
                    Coordinates::Llh marker_llh = Coordinates::ecefToLlh(marker_pos);

                    // Get ENU→ECEF rotation matrix (columns are E, N, U in ECEF)
                    Eigen::Matrix3d R_enu_to_ecef = Coordinates::enuToEcefMatrix(
                        marker_llh.lat_rad, marker_llh.lon_rad);

                    // RINEX stores H/E/N (Height=Up, East, North), reorder to ENU
                    // hen = (H, E, N) = (Up, East, North)
                    // enu = (East, North, Up) = (hen[1], hen[2], hen[0])
                    Eigen::Vector3d enu_offset(hen(1), hen(2), hen(0));

                    // Convert to ECEF and add to marker position
                    Eigen::Vector3d ecef_offset = R_enu_to_ecef * enu_offset;
                    state.position = marker_pos + ecef_offset;

                    log_manager_.getLogger("ppp_model")->info(
                        "Applied antenna H/E/N offset ({:.4f}, {:.4f}, {:.4f}) m to marker position, "
                        "ARP ECEF: ({:.3f}, {:.3f}, {:.3f}) m, lat={:.6f}°, lon={:.6f}°",
                        hen(0), hen(1), hen(2),
                        state.position.x(), state.position.y(), state.position.z(),
                        marker_llh.lat_rad * 180.0 / Constants::PI,
                        marker_llh.lon_rad * 180.0 / Constants::PI);
                }
                else {
                    state.position = marker_pos;
                    log_manager_.getLogger("ppp_model")->info(
                        "Initialized position from RINEX marker (no antenna offset): ({:.3f}, {:.3f}, {:.3f}) m",
                        marker_pos.x(), marker_pos.y(), marker_pos.z());
                }
            }
            else if (!config_.hasReferencePosition()) {
                state.position = Eigen::Vector3d::Zero();
                log_manager_.getLogger("ppp_model")->warn("Invalid or zero RINEX approximate position, using default: (0, 0, 0) m");
            }

            GnssTime init_time = rinex_parser_.getTimeOfFirstObs();
            if (init_time.tow > 0.0 && init_time.week > 0) {
                state.epoch_time = init_time;
                log_manager_.getLogger("ppp_model")->info("Initialized state epoch time from RINEX: week {}, TOW {:.3f}",
                    init_time.week, init_time.tow);
            }
            else {
                state.epoch_time = GnssTime{ 0, 0.0 };
                log_manager_.getLogger("ppp_model")->warn("Invalid RINEX time of first observation, using default: TOW 0.0");
            }

            state.clock_bias = 0.0;
            // Initialize ZWD to a climatological a priori value.
            // Typical ZWD at sea level ranges from ~0.05 m (dry/cold) to ~0.35 m (humid/tropical).
            // A reasonable mid-latitude default is ~0.1 m. The filter will refine this estimate.
            state.troposphere_zwd = 0.1;
            log_manager_.getLogger("ppp_model")->info("Initialized troposphere ZWD to climatological default: {:.3f} m", state.troposphere_zwd);
            if (config_.getProcessingMode() == ProcessingMode::PPP_RTK) {
                // Per-satellite slant ionosphere states are created dynamically
                // in MeasurementModel as satellites become visible. No single
                // zenith iono state is needed.
                log_manager_.getLogger("ppp_model")->info("PPP-RTK mode: per-satellite ionosphere states will be created dynamically");
            }
        }
        else {
            log_manager_.getLogger("ppp_model")->debug("State position already initialized: ({:.3f}, {:.3f}, {:.3f}) m, norm: {:.3f}",
                state.position.x(), state.position.y(), state.position.z(), state.position.norm());
        }
    }

    /**
     * @brief Compute modeled PPP observations for a set of RINEX observations.
     *
     * For each input observation this routine computes satellite geometry and applies
     * corrections, selects usable signals and builds ionosphere-free (or uncombined)
     * modeled observables. Outputs are appended to \p modeled_obs.
     *
     * @param observations Vector of RinexObservation records for the epoch.
     * @param state Current PppState used for modelling (read-only).
     * @param modeled_obs Output vector filled with PppObservation entries for processed observations.
     * @return true if at least one observation was successfully modeled, false otherwise.
     */
    bool PppModel::computeModels(const std::vector<RinexObservation>& observations, PppState& state, std::vector<PppObservation>& modeled_obs) {
        if (observations.empty()) {
            log_manager_.getLogger("ppp_model")->error("No observations provided for PPP modeling");
            return false;
        }

        modeled_obs.clear();
        modeled_obs.reserve(observations.size());

        for (const auto& obs : observations) {
            // --- Compute satellite geometry & corrections ---
            SatGeometry geom;
            bool geom_ok = geometry_.computeSatelliteGeometry(
                obs, state,
                geom.sat_pos, geom.rx_pos, geom.elevation, geom.azimuth,
                geom.los, geom.range, geom.tropo_zhd,
                geom.sat_clock, geom.shapiro_delay, geom.sagnac_corr,
                geom.atm_loading_corr, geom.relativistic_corr, geom.solid_tide,
                geom.mf_w, geom.mf_d, geom.sat_vel);

            if (!geom_ok) {
                log_manager_.getLogger("ppp_model")->debug("Geometry failed for {} at TOW {:.3f}", obs.satellite, obs.time.tow);
                continue;
            }

            // --- Populate base PppObservation fields ---
            PppObservation ppp_obs;
            ppp_obs.satellite = obs.satellite;
            ppp_obs.system = obs.system;
            ppp_obs.time = obs.time;
            ppp_obs.observed = obs.measurements;
            ppp_obs.variances = obs.variances;
            ppp_obs.lli_flags = obs.lli_flags;

            ppp_obs.sat_pos = geom.sat_pos;
            ppp_obs.sat_vel = geom.sat_vel;
            ppp_obs.rx_pos_linearization = geom.rx_pos;
            ppp_obs.elevation = geom.elevation;
            ppp_obs.azimuth = geom.azimuth;
            ppp_obs.los = geom.los;
            ppp_obs.range = geom.range;
            ppp_obs.mf_w = geom.mf_w;
            ppp_obs.mf_d = geom.mf_d;

            log_manager_.getLogger("ppp_model")->trace(
                "Stored geometry for {} at TOW {:.3f}: sat_pos=({:.3f}, {:.3f}, {:.3f}) m, "
                "elevation={:.3f} deg, azimuth={:.3f} deg, los=({:.3f}, {:.3f}, {:.3f}), range={:.3f} m",
                obs.satellite, obs.time.tow,
                geom.sat_pos.x(), geom.sat_pos.y(), geom.sat_pos.z(),
                geom.elevation, geom.azimuth,
                geom.los.x(), geom.los.y(), geom.los.z(), geom.range);

            // --- Validate system ---
            std::string system_str = PppUtils::getSystemString(obs.system);
            if (system_str == "Unknown") {
                log_manager_.getLogger("ppp_model")->warn("Unknown system for satellite {} at TOW {:.3f}", obs.satellite, obs.time.tow);
                continue;
            }

            auto obs_types = selectObservationTypes(obs.measurements, obs.system, system_str);
            if (obs_types.empty()) {
                log_manager_.getLogger("ppp_model")->warn("No valid observation types for system {} for satellite {}", system_str, obs.satellite);
                continue;
            }

            // --- Signal selection ---
            std::vector<std::pair<std::string, std::string>> selected_signals;
            bool is_iono_free;
            std::string comb_type;
            if (!selectSignals(obs, system_str, selected_signals, is_iono_free, comb_type)) {
                continue;
            }

            // --- Process observation ---
            if (config_.isUncombinedEnabled()) {
                // Build the set of signal keys that were actually selected for
                // positioning so that observed only contains those signals.
                // The KF measurement model must not see unselected bands.
                std::set<std::string> selected_keys;
                for (const auto& sp : selected_signals) {
                    if (!sp.first.empty())  selected_keys.insert(sp.first);
                    if (!sp.second.empty()) selected_keys.insert(sp.second);
                }

                // Preserve ALL raw RINEX code+phase measurements in raw_observed
                // regardless of signal_mode.  The MW cycle slip detector uses
                // raw_observed to form all possible dual-frequency MW pairs,
                // enabling multi-frequency slip detection (e.g. L1/L2/L5) even
                // when signal_mode restricts the KF to dual-frequency only.
                for (const auto& [key, val] : ppp_obs.observed) {
                    if (key.size() >= 2 && (key[0] == 'C' || key[0] == 'L') &&
                        key.find('_') == std::string::npos) {
                        ppp_obs.raw_observed[key] = val;
                    }
                }

                if (!processUncombinedObservation(obs, state, ppp_obs, system_str, selected_signals, geom)) {
                    continue;
                }

                // Strip observed of any raw RINEX keys that were not selected,
                // so the KF measurement model only sees signals that match the
                // configured signal_mode for positioning.
                for (auto it = ppp_obs.observed.begin(); it != ppp_obs.observed.end(); ) {
                    const auto& k = it->first;
                    if (k.size() >= 2 && (k[0] == 'C' || k[0] == 'L') &&
                        k.find('_') == std::string::npos &&
                        !selected_keys.count(k)) {
                        it = ppp_obs.observed.erase(it);
                    } else {
                        ++it;
                    }
                }
            }

            if (is_iono_free && !config_.isUncombinedEnabled()) {
                if (!processIonoFreeObservation(obs, state, ppp_obs, system_str, selected_signals, comb_type, geom)) {
                    continue;
                }
            }

            // --- Model Doppler observables (range-rate in m/s) ---
            if (config_.estimateVelocity()) {
                const double range_rate_sat = geom.los.dot(geom.sat_vel);
                for (const auto& [key, val] : obs.measurements) {
                    if (key.empty() || key[0] != 'D') continue;
                    const double wavelength = ppp_utils_.getWavelength(obs.system, key, obs.satellite);
                    if (wavelength <= 0.0) continue;

                    // Convert RINEX Doppler (Hz) to range-rate (m/s).
                    // Positive Doppler in RINEX = approaching = decreasing range.
                    const double observed_rr = -val * wavelength;

                    ppp_obs.observed[key] = observed_rr;
                    ppp_obs.modeled[key] = range_rate_sat;

                    // Variance: default Doppler variance converted to (m/s)^2.
                    const auto& n = config_.getDefaultMeasurementNoise();
                    const double default_var = (n.doppler_std_m * wavelength) * (n.doppler_std_m * wavelength);
                    double var = obs.variances.count(key) ? obs.variances.at(key) * wavelength * wavelength : default_var;
                    var = stochastic_model_.computeVariance(key, obs.measurements, geom.elevation, var);
                    ppp_obs.variances[key] = var;

                    log_manager_.getLogger("ppp_model")->trace(
                        "Doppler {} for {}: obs_rr={:.4f} m/s, mod_rr={:.4f} m/s, var={:.6f}",
                        key, obs.satellite, observed_rr, range_rate_sat, var);
                }
            }

            if (!ppp_obs.modeled.empty()) {
                modeled_obs.push_back(ppp_obs);
            }
        }

        if (modeled_obs.empty()) {
            log_manager_.getLogger("ppp_model")->error("No valid PPP observations modeled");
            return false;
        }
        log_manager_.getLogger("ppp_model")->info("Modeled {} PPP observations for mode {}", modeled_obs.size(),
            config_.getProcessingMode() == ProcessingMode::PPP ? "PPP" : "PPP-RTK");
        return true;
    }

    /**
     * @brief Selects candidate signal pairs for processing for a single observation.
     *
     * Uses configured signal priority lists and band mappings to choose code/phase
     * pairs suitable for ionosphere-free combinations or single/uncombined processing.
     *
     * @param obs Input RinexObservation record.
     * @param system_str Human-readable GNSS system string (e.g., "GPS").
     * @param selected_signals Output vector populated with (signal1, signal2) pairs. For single/uncombined selections one member may be empty.
     * @param is_iono_free Output flag set to true when an iono-free combination (dual or triple) is available.
     * @param comb_type Output string describing the combination type ("single","dual","triple","uncombined").
     * @return true if useful signals were selected; false otherwise.
     */
    // ===================================================================
    // Signal selection — decomposed helpers
    // ===================================================================

    bool PppModel::buildFrequencyBandMap(
        const std::map<std::string, double>& measurements,
        const std::string& system_str,
        const std::vector<std::string>& signals,
        std::map<std::string, std::pair<std::string, std::string>>& freq_band_to_signal,
        std::vector<std::string>& freq_bands) const {

        freq_band_to_signal.clear();
        freq_bands.clear();

        const auto& system_to_bands = getSystemToBands();
        auto bands_it = system_to_bands.find(system_str);
        if (bands_it == system_to_bands.end()) return false;

        const auto& band_map = getBandMap();
        if (band_map.find(system_str) == band_map.end()) return false;

        // Determine which frequency bands are allowed for this system.
        //
        // In uncombined PPP-RTK mode with lock_signals_to_clock_ref enabled
        // (the default), only the two bands that match the BIA satellite-clock
        // reference observables are permitted.  This avoids extra IFB states
        // for non-reference bands and keeps the measurement model strictly
        // consistent with the clock/bias products.
        //
        // When the lock is disabled, or no clock-ref info is available for
        // this system, we fall back to the previous behaviour (SignalMode::All
        // for uncombined, or the configured signal_mode for combined).
        std::vector<std::string> clock_ref_bands;  // populated only when lock is active
        const std::vector<std::string>* allowed_bands_ptr = nullptr;
        // When the clock-ref lock is active, maps each RINEX band digit
        // (e.g. '1', '2') to the required tracking attribute (e.g. 'W')
        // so that only signals matching the clock-reference observables
        // are accepted.  Empty when the lock is inactive.
        std::map<char, char> clock_ref_attr;

        const bool use_clock_ref_lock = config_.isUncombinedEnabled()
                                     && config_.lockSignalsToClockRef();

        if (use_clock_ref_lock) {
            // Try to derive the two allowed bands from the BIA clock-reference signals
            const char sys_char = systemNameToClockRefChar(system_str);
            const auto& clock_refs = ProductCache::getInstance().getClockRefSignals();
            auto crit = clock_refs.find(sys_char);
            if (crit != clock_refs.end()) {
                const auto& ref = crit->second;
                // Build RINEX prefix from the clock-ref signal (e.g. "C1W" → "C1")
                // and look up the band name via the system_to_bands table.
                // Also extract the tracking attribute per band digit so that
                // only signals with a matching attribute are accepted later.
                for (const auto* sig : { &ref.sig1, &ref.sig2 }) {
                    if (sig->size() < 2) continue;
                    const std::string prefix = sig->substr(0, 2); // e.g. "C1"
                    for (const auto& [pat, band] : bands_it->second) {
                        if (pat == prefix) {
                            if (std::find(clock_ref_bands.begin(), clock_ref_bands.end(), band)
                                == clock_ref_bands.end()) {
                                clock_ref_bands.push_back(band);
                            }
                            break;
                        }
                    }
                    // Record required tracking attribute for this band digit
                    // (e.g. "C1W" → band digit '1', attribute 'W').
                    if (sig->size() >= 3) {
                        clock_ref_attr[(*sig)[1]] = (*sig)[2];
                    }
                }
            }
            if (!clock_ref_bands.empty()) {
                allowed_bands_ptr = &clock_ref_bands;
                log_manager_.getLogger("ppp_model")->trace(
                    "Clock-ref lock active for {}: allowed bands = [{}], attributes = [{}]",
                    system_str, [&]{
                        std::string s;
                        for (size_t i = 0; i < clock_ref_bands.size(); ++i) {
                            if (i) s += ", ";
                            s += clock_ref_bands[i];
                        }
                        return s;
                    }(), [&]{
                        std::string s;
                        for (auto it = clock_ref_attr.begin(); it != clock_ref_attr.end(); ++it) {
                            if (it != clock_ref_attr.begin()) s += ", ";
                            s += it->first;
                            s += ':';
                            s += it->second;
                        }
                        return s;
                    }());
            }
        }

        // Fall back to the static band map when clock-ref lock is not active
        // or when no clock-ref info was available for this system.
        // Respect the configured signal_mode so the user can control how many
        // frequency bands enter the filter (dual, triple or all).
        if (!allowed_bands_ptr) {
            allowed_bands_ptr = &band_map.at(system_str).at(config_.getSignalMode());
        }

        const auto& allowed_bands = *allowed_bands_ptr;

        // Scans the priority-ordered signal list and populates
        // freq_band_to_signal / freq_bands for every signal that passes
        // the band and (optionally) attribute filters.
        auto populate = [&](const std::map<char, char>& attr_filter) {
            for (const auto& sig : signals) {
                if (!measurements.count(sig)) continue;
                // When an attribute filter is active, reject signals whose
                // tracking attribute does not match the clock-reference
                // observable for the corresponding band digit.
                // Signal format is "{type}{band}{attr}" (e.g. "C1W").
                if (!attr_filter.empty() && sig.size() >= 3) {
                    auto ait = attr_filter.find(sig[1]);
                    if (ait != attr_filter.end() && sig[2] != ait->second)
                        continue;
                }
                for (const auto& [pattern, band] : bands_it->second) {
                    if (std::find(allowed_bands.begin(), allowed_bands.end(), band) == allowed_bands.end())
                        continue;
                    if (sig.find(pattern) != 0) continue;

                    if (freq_band_to_signal.find(band) == freq_band_to_signal.end()) {
                        freq_band_to_signal[band] = { "", "" };
                        freq_bands.push_back(band);
                    }
                    auto& [best_code, best_phase] = freq_band_to_signal[band];
                    if (sig[0] == 'C' && (best_code.empty() ||
                        std::find(signals.begin(), signals.end(), sig) <
                        std::find(signals.begin(), signals.end(), best_code))) {
                        best_code = sig;
                    }
                    if (sig[0] == 'L' && (best_phase.empty() ||
                        std::find(signals.begin(), signals.end(), sig) <
                        std::find(signals.begin(), signals.end(), best_phase))) {
                        best_phase = sig;
                    }
                }
            }
        };

        // First pass: strict attribute match against clock-ref signals.
        populate(clock_ref_attr);

        // If the strict attribute filter produced no results (the receiver
        // does not track the exact clock-ref tracking mode), retry without
        // the attribute constraint so the system is not excluded entirely.
        // The band restriction is still enforced.
        if (freq_bands.empty() && !clock_ref_attr.empty()) {
            log_manager_.getLogger("ppp_model")->debug(
                "Clock-ref attribute lock produced no signals for {}; "
                "relaxing to band-only lock", system_str);
            static const std::map<char, char> no_attr;
            populate(no_attr);
        }

        return !freq_bands.empty();
    }

    bool PppModel::selectUncombinedSignals(
        const RinexObservation& obs, const std::string& system_str,
        const std::map<std::string, std::pair<std::string, std::string>>& freq_band_to_signal,
        const std::vector<std::string>& freq_bands,
        std::vector<std::pair<std::string, std::string>>& selected_signals) const {

        selected_signals.clear();
        for (const auto& band : freq_bands) {
            auto band_it = freq_band_to_signal.find(band);
            if (band_it == freq_band_to_signal.end()) continue;
            const auto& [best_code, best_phase] = band_it->second;
            bool found_code = false, found_phase = false;
            if (!best_code.empty() && obs.measurements.count(best_code)) {
                selected_signals.emplace_back(best_code, "");
                found_code = true;
            }
            if (!best_phase.empty() && obs.measurements.count(best_phase)) {
                selected_signals.emplace_back(best_phase, "");
                found_phase = true;
            }
            log_manager_.getLogger("ppp_model")->trace(
                "Uncombined band {} for {} {}: code={} ({}), phase={} ({})",
                band, obs.satellite, system_str,
                best_code, found_code ? "ok" : "missing",
                best_phase, found_phase ? "ok" : "missing");
        }
        return !selected_signals.empty();
    }

    bool PppModel::selectIonoFreePairs(
        const RinexObservation& obs, const std::string& system_str,
        const std::vector<std::string>& signals,
        const std::map<std::string, std::pair<std::string, std::string>>& freq_band_to_signal,
        const std::vector<std::string>& freq_bands,
        std::vector<std::pair<std::string, std::string>>& selected_signals) const {

        selected_signals.clear();
        const auto& band_map = getBandMap();
        if (!band_map.count(system_str)) return false;

        auto target_bands = band_map.at(system_str).at(config_.getSignalMode());
        std::vector<std::pair<std::string, std::string>> candidate_pairs;

        for (size_t i = 0; i < target_bands.size(); ++i) {
            for (size_t j = i + 1; j < target_bands.size(); ++j) {
                const auto& b1 = target_bands[i];
                const auto& b2 = target_bands[j];

                // Anchor policy: in triple/all modes, at least one band must be the anchor (first band).
                const auto sm = config_.getSignalMode();
                const bool use_anchor = (sm == SignalMode::Triple || sm == SignalMode::All) && target_bands.size() >= 2;
                if (use_anchor && b1 != target_bands.front() && b2 != target_bands.front()) {
                    log_manager_.getLogger("ppp_model")->trace(
                        "Skipping non-anchor band pair for {} {}: {} / {} (anchor={})",
                        obs.satellite, system_str, b1, b2, target_bands.front());
                    continue;
                }

                if (!freq_band_to_signal.count(b1) || !freq_band_to_signal.count(b2)) {
                    log_manager_.getLogger("ppp_model")->debug(
                        "Missing band signals for {} {}: band {} present={}, band {} present={}, signal_mode={}",
                        obs.satellite, system_str, b1, freq_band_to_signal.count(b1),
                        b2, freq_band_to_signal.count(b2), signalModeLabel(sm));
                    continue;
                }

                const auto& s1 = freq_band_to_signal.at(b1);
                const auto& s2 = freq_band_to_signal.at(b2);

                if (!s1.first.empty() && !s2.first.empty()) {
                    candidate_pairs.emplace_back(s1.first, s2.first);
                    log_manager_.getLogger("ppp_model")->trace(
                        "Selected iono-free code pair for {} {}: {} / {}, bands: {} / {}, signal_mode={}",
                        obs.satellite, system_str, s1.first, s2.first, b1, b2, signalModeLabel(sm));
                }
                if (!s1.second.empty() && !s2.second.empty()) {
                    candidate_pairs.emplace_back(s1.second, s2.second);
                    log_manager_.getLogger("ppp_model")->trace(
                        "Selected iono-free phase pair for {} {}: {} / {}, bands: {} / {}, signal_mode={}",
                        obs.satellite, system_str, s1.second, s2.second, b1, b2, signalModeLabel(sm));
                } else if (s1.second.empty() || s2.second.empty()) {
                    log_manager_.getLogger("ppp_model")->debug(
                        "Missing phase signal for {} {}: band {} phase={}, band {} phase={}, signal_mode={}",
                        obs.satellite, system_str, b1, s1.second, b2, s2.second, signalModeLabel(sm));
                }
            }
        }

        if (candidate_pairs.empty()) return false;

        // Score each pair by signal priority (lower is better).
        auto pair_score = [&](const std::pair<std::string, std::string>& p) -> int {
            auto it1 = std::find(signals.begin(), signals.end(), p.first);
            auto it2 = std::find(signals.begin(), signals.end(), p.second);
            int idx1 = (it1 == signals.end()) ? std::numeric_limits<int>::max() / 2 : static_cast<int>(std::distance(signals.begin(), it1));
            int idx2 = (it2 == signals.end()) ? std::numeric_limits<int>::max() / 2 : static_cast<int>(std::distance(signals.begin(), it2));
            return idx1 + idx2;
        };

        std::vector<std::pair<std::string, std::string>> code_cands, phase_cands;
        for (const auto& p : candidate_pairs) {
            if (!p.first.empty() && p.first[0] == 'C') code_cands.push_back(p);
            else if (!p.first.empty() && p.first[0] == 'L') phase_cands.push_back(p);
        }

        // Sort by priority (best first) for deterministic ordering.
        // The first pair is the primary combination used by the MW slip
        // detector; additional pairs provide independent geometry.
        std::sort(code_cands.begin(), code_cands.end(),
            [&](const auto& a, const auto& b) { return pair_score(a) < pair_score(b); });
        std::sort(phase_cands.begin(), phase_cands.end(),
            [&](const auto& a, const auto& b) { return pair_score(a) < pair_score(b); });

        for (const auto& p : code_cands)  selected_signals.push_back(p);
        for (const auto& p : phase_cands) selected_signals.push_back(p);

        return !selected_signals.empty();
    }

    void PppModel::selectSingleSignalFallback(
        const RinexObservation& obs,
        const std::vector<std::string>& signals,
        std::vector<std::pair<std::string, std::string>>& selected_signals) const {

        selected_signals.clear();
        for (const auto& sig : signals) {
            if (obs.measurements.count(sig) && sig[0] == 'C') {
                selected_signals.emplace_back(sig, "");
                break;
            }
        }
        for (const auto& sig : signals) {
            if (obs.measurements.count(sig) && sig[0] == 'L') {
                selected_signals.emplace_back("", sig);
                break;
            }
        }
    }

    // ===================================================================
    // selectSignals — top-level orchestrator
    // ===================================================================

    bool PppModel::selectSignals(const RinexObservation& obs, const std::string& system_str,
        std::vector<std::pair<std::string, std::string>>& selected_signals,
        bool& is_iono_free, std::string& comb_type) {

        auto it = signal_priorities_.find(system_str);
        const auto& signals = it != signal_priorities_.end() ? it->second : std::vector<std::string>{};
        if (signals.empty()) {
            log_manager_.getLogger("ppp_model")->warn("No signal priorities for system {}, skipping satellite {}", system_str, obs.satellite);
            return false;
        }

        // Build frequency band map from available measurements.
        std::map<std::string, std::pair<std::string, std::string>> freq_band_to_signal;
        std::vector<std::string> freq_bands;
        if (!buildFrequencyBandMap(obs.measurements, system_str, signals, freq_band_to_signal, freq_bands)) {
            log_manager_.getLogger("ppp_model")->warn("No frequency band mappings for system {}, skipping satellite {}", system_str, obs.satellite);
            return false;
        }

        // Log available signals.
        {
            std::string available_signals;
            for (const auto& [band, sig_pair] : freq_band_to_signal) {
                available_signals += band + ":(" + (sig_pair.first.empty() ? "none" : sig_pair.first) + "," +
                    (sig_pair.second.empty() ? "none" : sig_pair.second) + ")";
                if (band != freq_bands.back()) available_signals += ", ";
            }
            const auto effective_mode = config_.isUncombinedEnabled() ? SignalMode::All : config_.getSignalMode();
            const char* band_note = (config_.isUncombinedEnabled() && config_.lockSignalsToClockRef())
                                  ? " clock-ref-locked" : "";
            log_manager_.getLogger("ppp_model")->debug("Available signals for {} {}: [{}], signal_mode={} (effective={}{})",
                obs.satellite, system_str,
                available_signals.empty() ? "none" : available_signals,
                signalModeLabel(config_.getSignalMode()),
                signalModeLabel(effective_mode), band_note);
        }

        selected_signals.clear();
        is_iono_free = freq_bands.size() >= 2 && !config_.isUncombinedEnabled();
        comb_type = freq_bands.size() >= 3 ? "triple" : freq_bands.size() == 2 ? "dual" : "single";

        if (config_.isUncombinedEnabled()) {
            if (!selectUncombinedSignals(obs, system_str, freq_band_to_signal, freq_bands, selected_signals)) {
                log_manager_.getLogger("ppp_model")->warn("No valid signals for {} {} at TOW {:.3f}",
                    obs.satellite, system_str, obs.time.tow);
                return false;
            }
            comb_type = "uncombined";
        } else if (is_iono_free) {
            if (!selectIonoFreePairs(obs, system_str, signals, freq_band_to_signal, freq_bands, selected_signals)) {
                comb_type = "single";
                is_iono_free = false;
            }
        }

        if (!is_iono_free && selected_signals.empty()) {
            selectSingleSignalFallback(obs, signals, selected_signals);
            comb_type = "single";
        }

        if (selected_signals.empty()) {
            log_manager_.getLogger("ppp_model")->warn("No valid signals for {} {} at TOW {:.3f}", obs.satellite, system_str, obs.time.tow);
            return false;
        }

        // Log final selection.
        std::string signal_list;
        for (size_t i = 0; i < selected_signals.size(); ++i) {
            signal_list += (selected_signals[i].first.empty() ? "" : selected_signals[i].first) +
                (selected_signals[i].second.empty() ? "" : (selected_signals[i].first.empty() ? "" : "/") + selected_signals[i].second);
            if (i < selected_signals.size() - 1) signal_list += ",";
        }
        log_manager_.getLogger("ppp_model")->trace("Selected signals for {} {}: [{}], comb_type={}, is_iono_free={}, mode={}",
            obs.satellite, system_str, signal_list.empty() ? "none" : signal_list, comb_type, is_iono_free,
            config_.getProcessingMode() == ProcessingMode::PPP ? "PPP" : "PPP-RTK");

        return true;
    }

    /**
     * @brief Process ionosphere-free or combined observation pairs and populate a PppObservation.
     *
     * Builds ionosphere-free combinations (code and/or phase) from selected signal pairs,
     * computes modeled values, variances and writes them into ppp_obs.modeled / observed / variances.
     *
     * @param obs Input RinexObservation record.
     * @param state Current PPP state used for geometry and corrections.
     * @param ppp_obs Output PppObservation structure to populate.
     * @param system_str GNSS system string (e.g., "GPS").
     * @param selected_signals Vector of (signal1, signal2) pairs chosen for processing.
     * @param comb_type Combination type string ("single","dual","triple","uncombined").
     * @param rx_pos Receiver ECEF position.
     * @param sat_pos Satellite ECEF position.
     * @param sat_vel Satellite ECEF velocity.
     * @param elevation Satellite elevation in degrees.
     * @param azimuth Satellite azimuth in degrees.
     * @param rx_antenna_type Receiver antenna type string for corrections.
     * @param range Geometric range (meters).
     * @param tropo_zhd Zenith hydrostatic delay (meters).
     * @param sat_clock Satellite clock (seconds or converted to meters in code).
     * @param shapiro_delay Shapiro delay (meters).
     * @param sagnac_corr Sagnac correction (meters).
     * @param atm_loading_corr Atmospheric loading correction (meters).
     * @param relativistic_corr Relativistic periodic correction -2(r·v)/c² (seconds). Must be
     *        subtracted from the modeled observable (converted to meters via multiplication
     *        by SPEED_OF_LIGHT) to account for the eccentricity-dependent clock variation
     *        that is not captured by the interpolated CLK product values.
     * @param solid_tide Solid Earth tide correction (meters).
     * @return true if at least one combined observable was successfully formed; false otherwise.
     */
    // ===================================================================
    // Shared correction helpers
    // ===================================================================

    double PppModel::computePoleTideProjection(const Eigen::Vector3d& rx_pos,
        const GnssTime& time,
        const Eigen::Vector3d& los) const {
        if (!config_.getModelCorrections().enable_pole_tide)
            return 0.0;
        const Eigen::Vector3d pole_tide_ecef = gnss_corrections_.computePoleTideCorrection(rx_pos, time);
        return pole_tide_ecef.dot(-los);
    }

    double PppModel::computeBaseModeledValue(const RinexObservation& obs,
        const PppObservation& ppp_obs,
        const SatGeometry& geom,
        const std::string& signal_type) const {

        const double slant_dry_delay = geom.tropo_zhd * ppp_obs.mf_d;

        const std::string rx_antenna_type = rinex_parser_.getReceiverAntennaType();
        const double antenna_corr = antenna_.getAntennaCorrection(
            obs.satellite, signal_type, obs.system,
            geom.rx_pos, geom.sat_pos, geom.sat_vel,
            geom.elevation, geom.azimuth, rx_antenna_type, obs.time,
            geometry_.getCachedSunPosition());

        const double bias_corr = config_.getModelCorrections().enable_code_phase_bias
            ? gnss_corrections_.getBiasCorrection(obs.satellite, signal_type, obs.time)
            : 0.0;

        return geom.range
            - (geom.sat_clock * Constants::SPEED_OF_LIGHT)
            - (geom.relativistic_corr * Constants::SPEED_OF_LIGHT)
            + slant_dry_delay
            + antenna_corr
            + bias_corr
            + geom.shapiro_delay
            + geom.atm_loading_corr
            + geom.solid_tide;
    }

    // ===================================================================
    // Iono-free observation processing
    // ===================================================================

    bool PppModel::processIonoFreePair(const RinexObservation& obs, PppObservation& ppp_obs,
        const std::string& system_str,
        const std::pair<std::string, std::string>& sig_pair,
        char meas_type,
        const SatGeometry& geom,
        double pole_tide_corr,
        const std::string& comb_type) {

        const std::string ref_type = sig_pair.first + "_" + sig_pair.second;

        std::vector<double> modeled_obs(2, 0.0);
        std::vector<double> variances(2, 0.0);
        std::vector<double> frequencies(2, 0.0);
        std::vector<double> wavelengths(2, 0.0);

        if (meas_type == 'L') {
            wavelengths[0] = ppp_utils_.getWavelength(obs.system, sig_pair.first, obs.satellite);
            wavelengths[1] = ppp_utils_.getWavelength(obs.system, sig_pair.second, obs.satellite);
            if (wavelengths[0] <= 0.0 || wavelengths[1] <= 0.0) {
                log_manager_.getLogger("ppp_model")->warn(
                    "Invalid wavelength for pair {} on system {} for satellite {}: wl1={:.3f}, wl2={:.3f} m",
                    ref_type, system_str, obs.satellite, wavelengths[0], wavelengths[1]);
                return false;
            }
        }

        for (size_t j = 0; j < 2; ++j) {
            const std::string& type = (j == 0 ? sig_pair.first : sig_pair.second);

            modeled_obs[j] = computeBaseModeledValue(obs, ppp_obs, geom, type) + pole_tide_corr;

            const auto& n = config_.getDefaultMeasurementNoise();
            const double fallback_var = obs.variances.count(type) ? obs.variances.at(type)
                : (meas_type == 'C' ? n.code_std_m * n.code_std_m : n.phase_std_m * n.phase_std_m);
            variances[j] = stochastic_model_.computeVariance(type, obs.measurements, geom.elevation, fallback_var,
                obs.satellite, antenna_.getAntennas());

            frequencies[j] = ppp_utils_.getFrequency(type, obs.system, obs.satellite);
            if (frequencies[j] <= 0.0) {
                log_manager_.getLogger("ppp_model")->warn("Invalid frequency for type {} on system {} for satellite {}", type, system_str, obs.satellite);
                return false;
            }
        }

        const double f1_sq = frequencies[0] * frequencies[0];
        const double f2_sq = frequencies[1] * frequencies[1];
        const double denom = (f1_sq - f2_sq);
        if (!std::isfinite(denom) || std::abs(denom) < 1e-12) {
            log_manager_.getLogger("ppp_model")->warn("Invalid denominator for iono-free pair {} for {}", ref_type, obs.satellite);
            return false;
        }

        const double c1 = f1_sq / denom;
        const double c2 = -f2_sq / denom;
        if (!std::isfinite(c1) || !std::isfinite(c2)) {
            log_manager_.getLogger("ppp_model")->warn("Non-finite coefficients for dual-frequency pair {} for {}", ref_type, obs.satellite);
            return false;
        }

        const double modeled_value = c1 * modeled_obs[0] + c2 * modeled_obs[1];
        double variance_value = c1 * c1 * variances[0] + c2 * c2 * variances[1];

        double observed_value = 0.0;
        for (size_t j = 0; j < 2; ++j) {
            const std::string& type = (j == 0 ? sig_pair.first : sig_pair.second);
            double meas = obs.measurements.at(type);
            if (meas_type == 'L') {
                meas *= wavelengths[j];
            }
            observed_value += (j == 0 ? c1 : c2) * meas;
        }

        ppp_obs.observed[ref_type] = observed_value;
        ppp_obs.modeled[ref_type] = modeled_value;
        ppp_obs.variances[ref_type] = variance_value;

        log_manager_.getLogger("ppp_model")->trace(
            "Iono-free {} for {} ({}): observed={:.3f} m, modeled={:.3f} m, variance={:.6f}, comb_type={}, mode={}",
            ref_type, obs.satellite, (meas_type == 'C' ? "CODE" : "PHASE"),
            observed_value, modeled_value, variance_value, comb_type,
            config_.getProcessingMode() == ProcessingMode::PPP ? "PPP" : "PPP-RTK");

        return true;
    }

    bool PppModel::processIonoFreeObservation(const RinexObservation& obs, PppState& state,
        PppObservation& ppp_obs, const std::string& system_str,
        const std::vector<std::pair<std::string, std::string>> selected_signals,
        const std::string& comb_type, const SatGeometry& geom) {

        if (comb_type != "dual" && comb_type != "triple") {
            log_manager_.getLogger("ppp_model")->warn("Invalid combination type {} for {}", comb_type, obs.satellite);
            return false;
        }

        if (selected_signals.size() < 2) {
            log_manager_.getLogger("ppp_model")->warn(
                "Too few signal pairs for {} combination for {} (got {})",
                comb_type, obs.satellite, selected_signals.size());
            return false;
        }

        // Validate signal pairs and split into code / phase.
        std::vector<std::pair<std::string, std::string>> code_pairs;
        std::vector<std::pair<std::string, std::string>> phase_pairs;
        code_pairs.reserve(selected_signals.size());
        phase_pairs.reserve(selected_signals.size());

        for (const auto& pair : selected_signals) {
            if (pair.first.empty() || pair.second.empty()) {
                log_manager_.getLogger("ppp_model")->warn(
                    "Empty signal in selected pair for {} {}: first={}, second={}",
                    obs.satellite, system_str, pair.first, pair.second);
                return false;
            }
            if (!obs.measurements.count(pair.first) || !obs.measurements.count(pair.second)) {
                log_manager_.getLogger("ppp_model")->warn(
                    "Missing measurement(s) for {} {}: {} present={}, {} present={}",
                    obs.satellite, system_str,
                    pair.first, obs.measurements.count(pair.first),
                    pair.second, obs.measurements.count(pair.second));
                return false;
            }
            if (pair.first[0] == 'C' && pair.second[0] == 'C') {
                code_pairs.push_back(pair);
            } else if (pair.first[0] == 'L' && pair.second[0] == 'L') {
                phase_pairs.push_back(pair);
            } else {
                log_manager_.getLogger("ppp_model")->warn(
                    "Invalid mixed signal pair for {} {}: {} / {}",
                    obs.satellite, system_str, pair.first, pair.second);
                return false;
            }
        }

        if (code_pairs.empty() && phase_pairs.empty()) {
            log_manager_.getLogger("ppp_model")->warn("No valid iono-free signal pairs for {} {}", obs.satellite, system_str);
            return false;
        }

        const double pole_tide_corr = computePoleTideProjection(geom.rx_pos, obs.time, ppp_obs.los);

        bool success = false;
        for (const auto& p : code_pairs) {
            success = processIonoFreePair(obs, ppp_obs, system_str, p, 'C', geom, pole_tide_corr, comb_type) || success;
        }
        for (const auto& p : phase_pairs) {
            success = processIonoFreePair(obs, ppp_obs, system_str, p, 'L', geom, pole_tide_corr, comb_type) || success;
        }

        // Remove raw RINEX measurements — only combined keys (containing '_') should remain.
        for (auto it = ppp_obs.observed.begin(); it != ppp_obs.observed.end(); ) {
            if (it->first.find('_') == std::string::npos) {
                ppp_obs.raw_observed[it->first] = it->second;
                it = ppp_obs.observed.erase(it);
            } else {
                ++it;
            }
        }

        return success;
    }

    /**
     * @brief Process uncombined observations for PPP-RTK: individual signals without iono-free combination.
     *
     * Each individual code/phase signal is modeled with the same corrections as the iono-free path
     * (range, satellite clock, tropo, antenna, bias, shapiro, solid tide, pole tide, relativistic,
     * atmospheric loading) but without forming dual-frequency combinations. The ionospheric delay
     * is NOT applied here — it becomes an estimated state in the Kalman filter.
     *
     * The modeled value for each signal represents the geometric + correction terms only.
     * The MeasurementModel will add the ionosphere contribution via the per-satellite
     * slant iono state scaled by the frequency-dependent coefficient.
     */
    bool PppModel::processUncombinedObservation(const RinexObservation& obs, PppState& state,
        PppObservation& ppp_obs, const std::string& system_str,
        const std::vector<std::pair<std::string, std::string>>& selected_signals,
        const SatGeometry& geom) {

        const double pole_tide_corr = computePoleTideProjection(geom.rx_pos, obs.time, ppp_obs.los);

        const auto& n = config_.getDefaultMeasurementNoise();
        const double default_code_var = n.code_std_m * n.code_std_m;
        const double default_phase_var = n.phase_std_m * n.phase_std_m;

        bool success = false;

        for (const auto& sig_pair : selected_signals) {
            const std::string& type = sig_pair.first.empty() ? sig_pair.second : sig_pair.first;
            if (type.empty()) continue;

            if (!obs.measurements.count(type)) {
                log_manager_.getLogger("ppp_model")->trace(
                    "Missing measurement for {} on {} in uncombined mode", type, obs.satellite);
                continue;
            }

            // Skip CODE signals that have no BIA product entry when required.
            // Phase measurements are always kept — the float ambiguity absorbs
            // any time-constant satellite phase bias from missing BIA coverage.
            // When estimate_sat_biases is enabled, code signals are also kept
            // because a per-satellite bias state absorbs the uncorrected delay.
            // MW slip detection is unaffected (it uses raw_observed).
            const char meas_type_pre = type[0];
            if (meas_type_pre == 'C'
                && config_.requireBiaForSignals()
                && !config_.estimateSatBiases()
                && config_.getModelCorrections().enable_code_phase_bias
                && !gnss_corrections_.hasBiasEntry(obs.satellite, type)) {
                log_manager_.getLogger("ppp_model")->debug(
                    "Skipping code {} on {} — no BIA entry (require_bia_for_signals=true)",
                    type, obs.satellite);
                continue;
            }

            // Skip signals lacking strict ATX coverage when the user
            // has set @c model_corrections.require_atx_coverage.  Both
            // satellite and receiver antenna entries must supply
            // finite PCO and populated PCV for this signal's exact
            // frequency — no fallback chain.  Used to make the ATX
            // file an authoritative whitelist when "all" signals are
            // requested but only an ATX-curated subset should
            // actually process.
            if (config_.getModelCorrections().require_atx_coverage
                && !antenna_.hasFullAtxCoverage(obs.satellite, type, obs.system)) {
                log_manager_.getLogger("ppp_model")->debug(
                    "Skipping {} on {} — no strict ATX coverage (require_atx_coverage=true)",
                    type, obs.satellite);
                continue;
            }

            const char meas_type = type[0];
            if (meas_type != 'C' && meas_type != 'L') continue;

            const double freq = ppp_utils_.getFrequency(type, obs.system, obs.satellite);
            if (freq <= 0.0) {
                log_manager_.getLogger("ppp_model")->warn(
                    "Invalid frequency for {} on {} {}", type, obs.satellite, system_str);
                continue;
            }

            const double modeled_value = computeBaseModeledValue(obs, ppp_obs, geom, type) + pole_tide_corr;

            double observed_value = obs.measurements.at(type);
            if (meas_type == 'L') {
                const double wavelength = ppp_utils_.getWavelength(obs.system, type, obs.satellite);
                if (wavelength <= 0.0) {
                    log_manager_.getLogger("ppp_model")->warn(
                        "Invalid wavelength for {} on {} {}", type, obs.satellite, system_str);
                    continue;
                }
                observed_value *= wavelength;
            }

            double variance = obs.variances.count(type) ? obs.variances.at(type)
                : (meas_type == 'C' ? default_code_var : default_phase_var);
            variance = stochastic_model_.computeVariance(type, obs.measurements, geom.elevation, variance,
                obs.satellite, antenna_.getAntennas());

            ppp_obs.observed[type] = observed_value;
            ppp_obs.modeled[type] = modeled_value;
            ppp_obs.variances[type] = variance;

            log_manager_.getLogger("ppp_model")->trace(
                "Uncombined {} for {} ({}): observed={:.3f} m, modeled={:.3f} m, variance={:.6f}, freq={:.3f} MHz",
                type, obs.satellite, (meas_type == 'C' ? "CODE" : "PHASE"),
                observed_value, modeled_value, variance, freq / 1e6);

            success = true;
        }

        return success;
    }

    /**
     * @brief Compute slant ionospheric delay for a single measurement path.
     *
     * When dual- or triple-frequency iono-free combinations are available the
     * ionospheric delay is effectively removed and this function returns 0.0.
     * Otherwise Klobuchar or the internal ionosphere model is used when available.
     *
     * @param time GNSS time of the observation.
     * @param rx_pos Receiver ECEF position (meters).
     * @param sat_pos Satellite ECEF position (meters).
     * @param freq_type Signal/frequency type string (e.g. "L1").
     * @param system GNSS system enum.
     * @param measurements Map of measurement values by key (used to detect available bands).
     * @return Estimated slant ionospheric delay in meters, or 0.0 if iono-free combination applies or unavailable.
     */
    double PppModel::computeIonosphericDelay(const GnssTime& time,
        const Eigen::Vector3d& rx_pos,
        const Eigen::Vector3d& sat_pos,
        const std::string& freq_type,
        System system,
        const std::map<std::string, double>& measurements) const {
        double freq = ppp_utils_.getFrequency(freq_type, system, "");
        if (freq <= 0.0) {
            log_manager_.getLogger("ppp_model")->warn("Invalid frequency for {} on system {}", freq_type, static_cast<int>(system));
            return 0.0;
        }

        std::string system_str = ppp_utils_.getSystemString(system);
        auto it = signal_priorities_.find(system_str);
        if (it == signal_priorities_.end()) {
            log_manager_.getLogger("ppp_model")->warn("No signal priorities defined for system {}", system_str);
            return 0.0;
        }
        const auto& signals = it->second;

        if (signals.size() >= 3 && measurements.count(signals[0]) && measurements.count(signals[1]) && measurements.count(signals[2])) {
            log_manager_.getLogger("ppp_model")->trace("Triple-frequency iono-free combination available for {}: ionospheric delay=0.0 m", freq_type);
            return 0.0; // Ionosphere-free, no delay
        }
        if (signals.size() >= 2 && measurements.count(signals[0]) && measurements.count(signals[1])) {
            log_manager_.getLogger("ppp_model")->trace("Dual-frequency iono-free combination available for {}: ionospheric delay=0.0 m", freq_type);
            return 0.0; // Ionosphere-free, no delay
        }
        if (!ionosphere_.has_value()) {
            // Klobuchar is gated off in model_corrections — the row needing
            // a single-frequency a-priori delay just gets 0.  In typical
            // post-processed runs this path never fires (IF / DD / PPP-RTK
            // uncombined all bypass it earlier in this function).
            return 0.0;
        }
        if (klobuchar_coeffs_.size() != 8) {
            log_manager_.getLogger("ppp_model")->warn("No Klobuchar coefficients available for {}", freq_type);
            return 0.0;
        }
        double delay = ionosphere_->computeKlobucharDelay(time, rx_pos, sat_pos, freq);
        log_manager_.getLogger("ppp_model")->trace("Computed Klobuchar ionospheric delay for {}: {:.3f} m", freq_type, delay);
        return delay;
    }

    /**
     * @brief Compute the standard ionosphere-free linear combination of two signals.
     *
     * Uses frequency-dependent coefficients to form the LC combination from two measurements.
     *
     * @param measurements Map of measurement key -> value (meters).
     * @param system GNSS system enum.
     * @param signal1 First signal key in measurements.
     * @param signal2 Second signal key in measurements.
     * @return Combined ionosphere-free observable in meters, or 0.0 on error.
     */
    double PppModel::computeIonoFreeCombination(const std::map<std::string, double>& measurements,
        System system,
        const std::string& signal1,
        const std::string& signal2) const {
        if (!measurements.count(signal1) || !measurements.count(signal2)) {
            log_manager_.getLogger("ppp_model")->warn("Missing measurements for signals {} or {} in iono-free combination", signal1, signal2);
            return 0.0;
        }

        double freq1 = ppp_utils_.getFrequency(signal1, system, "");
        double freq2 = ppp_utils_.getFrequency(signal2, system, "");
        if (freq1 <= 0.0 || freq2 <= 0.0 || std::abs(freq1 - freq2) < 1e-6) {
            log_manager_.getLogger("ppp_model")->warn("Invalid or identical frequencies for {} ({:.3f} MHz) and {} ({:.3f} MHz)",
                signal1, freq1 / 1e6, signal2, freq2 / 1e6);
            return 0.0;
        }

        double f1_sq = freq1 * freq1, f2_sq = freq2 * freq2;
        double c1 = f1_sq / (f1_sq - f2_sq);
        double c2 = -f2_sq / (f1_sq - f2_sq);
        if (!std::isfinite(c1) || !std::isfinite(c2)) {
            log_manager_.getLogger("ppp_model")->warn("Non-finite coefficients for iono-free combination: c1={:.3f}, c2={:.3f}", c1, c2);
            return 0.0;
        }

        double combination = c1 * measurements.at(signal1) + c2 * measurements.at(signal2);
        if (!std::isfinite(combination)) {
            log_manager_.getLogger("ppp_model")->warn("Non-finite iono-free combination for signals {} and {}: {:.3f}",
                signal1, signal2, combination);
            return 0.0;
        }

        log_manager_.getLogger("ppp_model")->trace("Computed iono-free combination for signals {} and {}: value={:.3f} m, "
            "freqs=[{:.3f}, {:.3f}] MHz, coeffs=[{:.3f}, {:.3f}]",
            signal1, signal2, combination, freq1 / 1e6, freq2 / 1e6, c1, c2);
        return combination;
    }

    /**
     * @brief Rescale GLONASS observation cycles to a reference frequency.
     *
     * GLONASS uses frequency-division multiple access; measured cycles must often
     * be scaled to a reference frequency to be comparable across satellites.
     *
     * @param observed_data Measured quantity in cycles or meters (depending on caller).
     * @param freq_type Frequency/signal type string.
     * @param system GNSS system enum.
     * @param satellite Satellite identifier string (e.g., "R01").
     * @return Rescaled observation value; returns input if inputs are invalid or system is not GLONASS.
     */
    double PppModel::rescaleGlonassObservation(double observed_data, const std::string& freq_type,
        System system, const std::string& satellite) const {
        if (system != System::GLONASS) {
            return observed_data;
        }

        if (freq_type.empty() || satellite.empty()) {
            log_manager_.getLogger("ppp_model")->warn("Invalid inputs for GLONASS rescaling: freq_type='{}', satellite='{}'",
                freq_type, satellite);
            return observed_data;
        }

        double actual_freq = ppp_utils_.getFrequency(freq_type, system, satellite);
        double ref_freq = ppp_utils_.getReferenceFrequency(freq_type, system);
        if (actual_freq <= 0.0 || ref_freq <= 0.0 || !std::isfinite(actual_freq) || !std::isfinite(ref_freq)) {
            log_manager_.getLogger("ppp_model")->warn("Cannot rescale GLONASS observation for type {}, satellite {}: actual_freq={:.3f} MHz, ref_freq={:.3f} MHz",
                freq_type, satellite, actual_freq / 1e6, ref_freq / 1e6);
            return observed_data;
        }

        double scaled_data = observed_data * (actual_freq / ref_freq);
        log_manager_.getLogger("ppp_model")->trace("Rescaled GLONASS observation for type {}, satellite {}: from {:.3f} to {:.3f} cycles, actual_freq={:.3f} MHz, ref_freq={:.3f} MHz",
            freq_type, satellite, observed_data, scaled_data, actual_freq / 1e6, ref_freq / 1e6);
        return scaled_data;
    }

    /**
     * @brief Select a list of observation keys available in the provided measurements.
     *
     * This helper consults the configured signal priorities and returns a prioritized
     * list of measurement keys (including GLONASS channel-specific keys when applicable).
     *
     * @param measurements Map of measurement key -> value.
     * @param system GNSS system enum.
     * @param system_str System string used to lookup priorities.
     * @return Vector of selected measurement keys in priority order (may be empty).
     */
    std::vector<std::string> PppModel::selectObservationTypes(const std::map<std::string, double>& measurements,
        System system,
        const std::string& system_str) const {
        std::vector<std::string> selected_types;
        auto it = signal_priorities_.find(system_str);
        if (it == signal_priorities_.end()) {
            return selected_types;
        }

        if (system == System::GLONASS) {
            for (const auto& signal : it->second) {
                if (measurements.count(signal)) {
                    selected_types.push_back(signal);
                }
                for (int channel = -7; channel <= 7; ++channel) {
                    std::string type = signal + "_k" + (channel >= 0 ? std::to_string(channel) : "m" + std::to_string(-channel));
                    if (measurements.count(type)) {
                        selected_types.push_back(type);
                    }
                }
            }
        }
        else {
            for (const auto& type : it->second) {
                if (measurements.count(type)) {
                    selected_types.push_back(type);
                }
            }
        }
        return selected_types;
    }

    /**
     * @brief Accessor for the RinexParser used by this model.
     * @return Const reference to the configured RinexParser.
     */
    const RinexParser& PppModel::getRinexParser() const {
        return rinex_parser_;
    }

} // namespace GnssProcessor
//#endif//#endif