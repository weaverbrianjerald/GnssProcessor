/*!
 * \file PppModel.cpp
 * \brief Implementation of PppModel: PPP/PPP-RTK observation modelling and corrections.
 *
 * This file implements the PppModel methods responsible for initializing PPP state,
 * computing satellite/receiver geometry, applying corrections (orbits, clocks, tides,
 * troposphere, ionosphere, antenna) and forming ionosphere-free combinations used by
 * downstream measurement/estimation components.
 *
 * Logging is performed via the provided LogManager instance and the model relies on
 * ProductParser, RinexParser and StateManager for external data and state bookkeeping.
 */

#include "GnssConfig.hpp"
#include "PppModel.hpp"
#include "PppTypes.hpp"
#include "RinexParser.hpp"
#include "StateManager.hpp"
#include "ProductParser.hpp"
#include "Constants.hpp"
#include "Coordinates.hpp"
#include "MeasurementModel.hpp"
#include "TimeUtils.hpp"
#include "ProductCache.hpp"
#include <algorithm>
#include <fstream>
#include <sstream>

namespace GnssProcessor {

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
        ionosphere_(config, log_manager, product_parser, rinex_parser),
        antenna_(config, product_parser, log_manager, rinex_parser),
        signal_priorities_(config.getSignalPriorities()),
        // Initialize reference from cache
        klobuchar_coeffs_(ProductCache::getInstance().getKlobucharCoeffs())
    {
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
        size_t core_size = config_.getProcessingMode() == ProcessingMode::PPP ? 5 : 6;
        if (state.position.norm() == 0.0 || state.position.norm() > 1e9 || state.position.hasNaN() ||
            state.covariance.rows() != static_cast<long>(core_size)) {
            Eigen::Vector3d marker_pos = rinex_parser_.getApproxPositionXyz();
            if (marker_pos.norm() > 1e3 && !marker_pos.hasNaN()) {
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
            else {
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
                if (state.position.norm() > 1e3 && !state.position.hasNaN() && init_time.tow > 0.0 && klobuchar_coeffs_.size() == 8) {
                    state.ionosphere_zenith_delay = ionosphere_.computeInitialIonoDelay(init_time, state.position);
                    log_manager_.getLogger("ppp_model")->info("Initialized ionospheric zenith delay from Klobuchar: {:.3f} m",
                        state.ionosphere_zenith_delay);
                }
                else {
                    state.ionosphere_zenith_delay = 5.0;
                    log_manager_.getLogger("ppp_model")->warn("Using default ionospheric zenith delay: 5.0 m (insufficient Klobuchar data)");
                }
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
            PppObservation ppp_obs;
            ppp_obs.satellite = obs.satellite;
            ppp_obs.system = obs.system;
            ppp_obs.time = obs.time;
            ppp_obs.observed = obs.measurements;
            ppp_obs.variances = obs.variances;
            ppp_obs.lli_flags = obs.lli_flags;

            ppp_obs.sat_pos = Eigen::Vector3d::Zero();
            ppp_obs.rx_pos_linearization = Eigen::Vector3d::Zero();
            ppp_obs.elevation = 0.0;
            ppp_obs.azimuth = 0.0;
            ppp_obs.los = Eigen::Vector3d::Zero();
            ppp_obs.range = 0.0;
            ppp_obs.mf_w = 0.0;
            ppp_obs.mf_d = 0.0;

            // Range corrections
            double elevation, azimuth, range, tropo_zhd, sat_clock, shapiro_delay, sagnac_corr, atm_loading_corr, relativistic_corr, solid_tide, mf_w, mf_d;

            // Satellite PVT
            Eigen::Vector3d sat_pos, sat_vel, rx_pos, los;
            bool geom_ok = geometry_.computeSatelliteGeometry(obs, state, sat_pos, rx_pos, elevation, azimuth, los, range, tropo_zhd,
                sat_clock, shapiro_delay, sagnac_corr, atm_loading_corr, relativistic_corr, solid_tide, mf_w, mf_d, sat_vel);

			// TODO: separate rangeCorrections from computeSatelliteGeometry

            if (!geom_ok) {
                log_manager_.getLogger("ppp_model")->debug( "Geometry failed for {} at TOW {:.3f}", obs.satellite, obs.time.tow);
                continue;
            }

            ppp_obs.sat_pos = sat_pos;
            ppp_obs.rx_pos_linearization = rx_pos;
            ppp_obs.elevation = elevation;
            ppp_obs.azimuth = azimuth;
            ppp_obs.los = los;
            ppp_obs.range = range;
            ppp_obs.mf_w = mf_w;
            ppp_obs.mf_d = mf_d;

            log_manager_.getLogger("ppp_model")->trace("Stored geometry for {} at TOW {:.3f}: sat_pos=({:.3f}, {:.3f}, {:.3f}) m, "
                "elevation={:.3f} deg, azimuth={:.3f} deg, los=({:.3f}, {:.3f}, {:.3f}), range={:.3f} m",
                obs.satellite, obs.time.tow, sat_pos.x(), sat_pos.y(), sat_pos.z(), elevation, azimuth, los.x(), los.y(), los.z(), range);

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

            std::string rx_antenna_type = rinex_parser_.getReceiverAntennaType();

            std::vector<std::pair<std::string, std::string>> selected_signals;
            bool is_iono_free;
            std::string comb_type;
            if (!selectSignals(obs, system_str, selected_signals, is_iono_free, comb_type)) {
                continue;
            }

            if (config_.isUncombinedEnabled()) {
                // Uncombined path not yet implemented
            }

            if (is_iono_free && !config_.isUncombinedEnabled()) {
                if (!processIonoFreeObservation(obs, state, ppp_obs, system_str, selected_signals, comb_type, rx_pos, sat_pos,
                    sat_vel, elevation, azimuth, rx_antenna_type, range, tropo_zhd, sat_clock,
                    shapiro_delay, sagnac_corr, atm_loading_corr, relativistic_corr, solid_tide)) {
                    continue;
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
    bool PppModel::selectSignals(const RinexObservation& obs, const std::string& system_str,
        std::vector<std::pair<std::string, std::string>>& selected_signals,
        bool& is_iono_free, std::string& comb_type) {
        auto it = signal_priorities_.find(system_str);
        const auto& signals = it != signal_priorities_.end() ? it->second : std::vector<std::string>{};
        if (signals.empty()) {
            log_manager_.getLogger("ppp_model")->warn("No signal priorities for system {}, skipping satellite {}", system_str, obs.satellite);
            return false;
        }

        static const std::map<std::string, std::vector<std::pair<std::string, std::string>>> system_to_bands = {
            {"GPS", {{"C1", "L1"}, {"L1", "L1"}, {"C2", "L2"}, {"L2", "L2"}, {"C5", "L5"}, {"L5", "L5"}}},
            {"Galileo", {{"C1", "E1"}, {"L1", "E1"}, {"C5", "E5a"}, {"L5", "E5a"}, {"C7", "E5b"}, {"L7", "E5b"}, {"C6", "E6"}, {"L6", "E6"}, {"C8", "E5"}, {"L8", "E5"}}},
            {"GLONASS", {{"C1", "G1"}, {"L1", "G1"}, {"C2", "G2"}, {"L2", "G2"}, {"C3", "G3"}, {"L3", "G3"}}},
            {"BeiDou", {{"C1", "B1C"}, {"L1", "B1C"}, {"C2", "B1I"}, {"L2", "B1I"}, {"C7", "B2"}, {"L7", "B2"}, {"C6", "B3"}, {"L6", "B3"}}},
            {"QZSS", {{"C1", "L1"}, {"L1", "L1"}, {"C2", "L2"}, {"L2", "L2"}, {"C5", "L5"}, {"L5", "L5"}, {"C6", "L6"}, {"L6", "L6"}}},
            {"SBAS", {{"C1", "L1"}, {"L1", "L1"}, {"C5", "L5"}, {"L5", "L5"}}}
        };

        std::map<std::string, std::pair<std::string, std::string>> freq_band_to_signal;
        std::vector<std::string> freq_bands;
        auto bands_it = system_to_bands.find(system_str);
        if (bands_it == system_to_bands.end()) {
            log_manager_.getLogger("ppp_model")->warn("No frequency band mappings for system {}, skipping satellite {}", system_str, obs.satellite);
            return false;
        }

        static const std::map<std::string, std::map<SignalMode, std::vector<std::string>>> band_map = {
            {"GPS", {{SignalMode::Dual, {"L1", "L2"}}, {SignalMode::Triple, {"L1", "L2", "L5"}}, {SignalMode::All, {"L1", "L2", "L5"}}}},
            {"Galileo", {{SignalMode::Dual, {"E1", "E5a"}}, {SignalMode::Triple, {"E1", "E5a", "E5b"}}, {SignalMode::All, {"E1", "E5a", "E5b", "E6", "E5"}}}},
            {"GLONASS", {{SignalMode::Dual, {"G1", "G2"}}, {SignalMode::Triple, {"G1", "G2", "G3"}}, {SignalMode::All, {"G1", "G2", "G3"}}}},
            {"BeiDou", {{SignalMode::Dual, {"B1I", "B3"}}, {SignalMode::Triple, {"B1I", "B3", "B2"}}, {SignalMode::All, {"B1C", "B1I", "B2", "B3"}}}},
            {"QZSS", {{SignalMode::Dual, {"L1", "L2"}}, {SignalMode::Triple, {"L1", "L2", "L5"}}, {SignalMode::All, {"L1", "L2", "L5", "L6"}}}},
            {"SBAS", {{SignalMode::Dual, {"L1", "L5"}}, {SignalMode::Triple, {"L1", "L5"}}, {SignalMode::All, {"L1", "L5"}}}}
        };

        for (const auto& sig : signals) {
            if (!obs.measurements.count(sig)) continue;
            for (const auto& [pattern, band] : bands_it->second) {
                if (std::find(band_map.at(system_str).at(config_.getSignalMode()).begin(),
                    band_map.at(system_str).at(config_.getSignalMode()).end(), band) ==
                    band_map.at(system_str).at(config_.getSignalMode()).end()) {
                    continue;
                }
                if (sig.find(pattern) == 0) {
                    if (freq_band_to_signal.find(band) == freq_band_to_signal.end()) {
                        freq_band_to_signal[band] = { "", "" };
                        freq_bands.push_back(band);
                    }
                    if (sig[0] == 'C' && (freq_band_to_signal[band].first.empty() ||
                        std::find(signals.begin(), signals.end(), sig) <
                        std::find(signals.begin(), signals.end(), freq_band_to_signal[band].first))) {
                        freq_band_to_signal[band].first = sig;
                    }
                    if (sig[0] == 'L' && (freq_band_to_signal[band].second.empty() ||
                        std::find(signals.begin(), signals.end(), sig) <
                        std::find(signals.begin(), signals.end(), freq_band_to_signal[band].second))) {
                        freq_band_to_signal[band].second = sig;
                    }
                }
            }
        }

        std::string available_signals;
        for (const auto& [band, sig_pair] : freq_band_to_signal) {
            available_signals += band + ":(" + (sig_pair.first.empty() ? "none" : sig_pair.first) + "," +
                (sig_pair.second.empty() ? "none" : sig_pair.second) + ")";
            if (band != freq_bands.back()) available_signals += ", ";
        }
        log_manager_.getLogger("ppp_model")->debug("Available signals for {} {}: [{}], signal_mode={}",
            obs.satellite, system_str, available_signals.empty() ? "none" : available_signals,
            config_.getSignalMode() == SignalMode::Dual ? "Dual" :
            config_.getSignalMode() == SignalMode::Triple ? "Triple" : "All");

        selected_signals.clear();
        is_iono_free = freq_bands.size() >= 2 && !config_.isUncombinedEnabled();
        comb_type = freq_bands.size() >= 3 ? "triple" : freq_bands.size() == 2 ? "dual" : "single";

        if (config_.isUncombinedEnabled()) {
            for (const auto& sig : signals) {
                if (obs.measurements.count(sig)) {
                    selected_signals.emplace_back(sig, "");
                }
            }
            if (selected_signals.empty()) {
                log_manager_.getLogger("ppp_model")->warn(
                    "No valid signals for {} {} at TOW {:.3f}",
                    obs.satellite, system_str, obs.time.tow
                );
                return false;
            }
            comb_type = "uncombined";
        }
        else if (is_iono_free && band_map.count(system_str)) {
            auto target_bands = band_map.at(system_str).at(config_.getSignalMode());
            std::vector<std::pair<std::string, std::string>> candidate_pairs;
            for (size_t i = 0; i < target_bands.size(); ++i) {
                for (size_t j = i + 1; j < target_bands.size(); ++j) {
                    const auto& b1 = target_bands[i];
                    const auto& b2 = target_bands[j];

                    const auto sm = config_.getSignalMode();
                    const bool use_anchor_policy = (sm == SignalMode::Triple) || (sm == SignalMode::All);
                    if (use_anchor_policy && target_bands.size() >= 2) {
                        const std::string& anchor_band = target_bands.front();
                        const bool has_anchor = (b1 == anchor_band) || (b2 == anchor_band);
                        if (!has_anchor) {
                            log_manager_.getLogger("ppp_model")->trace(
                                "Skipping non-anchor band pair for {} {} due to anchor policy: {} / {} (anchor={})",
                                obs.satellite, system_str, b1, b2, anchor_band);
                            continue;
                        }
                    }

                    if (freq_band_to_signal.count(b1) && freq_band_to_signal.count(b2)) {
                        const auto& s1 = freq_band_to_signal.at(b1);
                        const auto& s2 = freq_band_to_signal.at(b2);
                        if (!s1.first.empty() && !s2.first.empty()) {
                            candidate_pairs.emplace_back(s1.first, s2.first);
                            log_manager_.getLogger("ppp_model")->trace(
                                "Selected iono-free code pair for {} {}: {} / {}, bands: {} / {}, signal_mode={}",
                                obs.satellite, system_str, s1.first, s2.first, b1, b2,
                                config_.getSignalMode() == SignalMode::Dual ? "Dual" :
                                config_.getSignalMode() == SignalMode::Triple ? "Triple" : "All"
                            );
                        }
                        if (!s1.second.empty() && !s2.second.empty()) {
                            candidate_pairs.emplace_back(s1.second, s2.second);
                            log_manager_.getLogger("ppp_model")->trace(
                                "Selected iono-free phase pair for {} {}: {} / {}, bands: {} / {}, signal_mode={}",
                                obs.satellite, system_str, s1.second, s2.second, b1, b2,
                                config_.getSignalMode() == SignalMode::Dual ? "Dual" :
                                config_.getSignalMode() == SignalMode::Triple ? "Triple" : "All"
                            );
                        }
                        else if (s1.second.empty() || s2.second.empty()) {
                            log_manager_.getLogger("ppp_model")->debug(
                                "Missing phase signal for {} {}: band {} phase={}, band {} phase={}, signal_mode={}",
                                obs.satellite, system_str, b1, s1.second, b2, s2.second,
                                config_.getSignalMode() == SignalMode::Dual ? "Dual" :
                                config_.getSignalMode() == SignalMode::Triple ? "Triple" : "All"
                            );
                        }
                    }
                    else {
                        log_manager_.getLogger("ppp_model")->debug(
                            "Missing band signals for {} {}: band {} present={}, band {} present={}, signal_mode={}",
                            obs.satellite, system_str, b1, freq_band_to_signal.count(b1), b2, freq_band_to_signal.count(b2),
                            config_.getSignalMode() == SignalMode::Dual ? "Dual" :
                            config_.getSignalMode() == SignalMode::Triple ? "Triple" : "All"
                        );
                    }
                }
            }
			// selected_signals = std::move(candidate_pairs); // keep for future, as this results in combinations like L1W_L2W and L1X_L2X being selected together
            // choose best candidate pair according to configured priority (lower index = higher priority)
            if (!candidate_pairs.empty()) {
                // Separate candidate pairs into code and phase candidates
                std::vector<std::pair<std::string, std::string>> code_candidates;
                std::vector<std::pair<std::string, std::string>> phase_candidates;
                for (const auto& p : candidate_pairs) {
                    if (!p.first.empty() && p.first[0] == 'C') {
                        code_candidates.push_back(p);
                    }
                    else if (!p.first.empty() && p.first[0] == 'L') {
                        phase_candidates.push_back(p);
                    }
                }

                // Select best code pair
                if (!code_candidates.empty()) {
                    auto best_code = code_candidates.front();
                    int best_code_score = std::numeric_limits<int>::max();
                    for (const auto& p : code_candidates) {
                        auto it1 = std::find(signals.begin(), signals.end(), p.first);
                        auto it2 = std::find(signals.begin(), signals.end(), p.second);
                        int idx1 = (it1 == signals.end()) ? std::numeric_limits<int>::max() / 2 : static_cast<int>(std::distance(signals.begin(), it1));
                        int idx2 = (it2 == signals.end()) ? std::numeric_limits<int>::max() / 2 : static_cast<int>(std::distance(signals.begin(), it2));
                        int score = idx1 + idx2;
                        if (score < best_code_score) {
                            best_code_score = score;
                            best_code = p;
                        }
                    }
                    selected_signals.push_back(best_code);
                }

                // Select best phase pair
                if (!phase_candidates.empty()) {
                    auto best_phase = phase_candidates.front();
                    int best_phase_score = std::numeric_limits<int>::max();
                    for (const auto& p : phase_candidates) {
                        auto it1 = std::find(signals.begin(), signals.end(), p.first);
                        auto it2 = std::find(signals.begin(), signals.end(), p.second);
                        int idx1 = (it1 == signals.end()) ? std::numeric_limits<int>::max() / 2 : static_cast<int>(std::distance(signals.begin(), it1));
                        int idx2 = (it2 == signals.end()) ? std::numeric_limits<int>::max() / 2 : static_cast<int>(std::distance(signals.begin(), it2));
                        int score = idx1 + idx2;
                        if (score < best_phase_score) {
                            best_phase_score = score;
                            best_phase = p;
                        }
                    }
                    selected_signals.push_back(best_phase);
                }
            }
            else {
                selected_signals.clear();
            }
            if (selected_signals.empty()) {
                comb_type = "single";
                is_iono_free = false;
            }
        }

        if (!is_iono_free && selected_signals.empty()) {
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
            comb_type = "single";
        }

        if (selected_signals.empty()) {
            log_manager_.getLogger("ppp_model")->warn("No valid signals for {} {} at TOW {:.3f}", obs.satellite, system_str, obs.time.tow);
            return false;
        }

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
    bool PppModel::processIonoFreeObservation(const RinexObservation& obs, PppState& state,
        PppObservation& ppp_obs, const std::string& system_str,
        const std::vector<std::pair<std::string, std::string>> selected_signals,
        const std::string& comb_type, const Eigen::Vector3d& rx_pos,
        const Eigen::Vector3d& sat_pos, const Eigen::Vector3d& sat_vel,
        double elevation, double azimuth,
        const std::string& rx_antenna_type, double range, double tropo_zhd,
        double sat_clock, double shapiro_delay, double sagnac_corr,
        double atm_loading_corr, double relativistic_corr, double solid_tide) {

        size_t num_freq = (comb_type == "triple") ? 3 : (comb_type == "dual") ? 2 : 0;
        if (num_freq == 0) {
            log_manager_.getLogger("ppp_model")->warn("Invalid combination type {} for {}", comb_type, obs.satellite);
            return false;
        }

        if (selected_signals.size() > num_freq * 2 || selected_signals.size() < 2) {
            log_manager_.getLogger("ppp_model")->warn(
                "Invalid number of signal pairs for {} combination for {} (expected up to {}, got {})",
                comb_type, obs.satellite, num_freq * 2, selected_signals.size());
            return false;
        }

        const double slant_dry_delay = tropo_zhd * ppp_obs.mf_d;

        // Compute pole tide correction once (station-level, signal-independent).
        // The ECEF displacement is projected onto the satellite->receiver LOS (-los)
        // to match the sign convention used by solid Earth tide and OTL corrections.
        const Eigen::Vector3d pole_tide_ecef = gnss_corrections_.computePoleTideCorrection(rx_pos, obs.time);
        const double pole_tide_corr = pole_tide_ecef.dot(-ppp_obs.los);

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
            }
            else if (pair.first[0] == 'L' && pair.second[0] == 'L') {
                phase_pairs.push_back(pair);
            }
            else {
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

        auto process_pair = [&](const std::pair<std::string, std::string>& sig_pair, char meas_type) -> bool {
            const std::string ref_type = sig_pair.first + "_" + sig_pair.second;

            if (meas_type == 'L') {
                //return false; // phase disabled
            }

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

                const double antenna_corr = antenna_.getAntennaCorrection(
                    obs.satellite, type, obs.system, rx_pos, sat_pos, sat_vel,
                    elevation, azimuth, rx_antenna_type, obs.time,
                    geometry_.getCachedSunPosition());

                const double bias_corr = gnss_corrections_.getBiasCorrection(obs.satellite, type, obs.time);

                modeled_obs[j] = range
                    - (sat_clock * Constants::SPEED_OF_LIGHT)
                    - (relativistic_corr * Constants::SPEED_OF_LIGHT)
                    + slant_dry_delay
                    + antenna_corr
                    + bias_corr
                    + shapiro_delay
                    + atm_loading_corr
                    + solid_tide
                    + pole_tide_corr;

                const auto& n = config_.getDefaultMeasurementNoise();
                const double default_code_var = n.code_std_m * n.code_std_m;
                const double default_phase_var = n.phase_std_m * n.phase_std_m;

                variances[j] = obs.variances.count(type) ? obs.variances.at(type)
                    : (meas_type == 'C' ? default_code_var : default_phase_var);

                frequencies[j] = ppp_utils_.getFrequency(type, obs.system, obs.satellite);
                if (frequencies[j] <= 0.0) {
                    log_manager_.getLogger("ppp_model")->warn( "Invalid frequency for type {} on system {} for satellite {}", type, system_str, obs.satellite);
                    return false;
                }
            }

            // TODO: replace the following with formIonoFreeMeasurements(obs, obs.system, selected_signals, ... );

            const double f1_sq = frequencies[0] * frequencies[0];
            const double f2_sq = frequencies[1] * frequencies[1];
            const double denom = (f1_sq - f2_sq);
            if (!std::isfinite(denom) || std::abs(denom) < 1e-12) {
                log_manager_.getLogger("ppp_model")->warn( "Invalid denominator for iono-free pair {} for {}", ref_type, obs.satellite);
                return false;
            }

            const double c1 = f1_sq / denom;
            const double c2 = -f2_sq / denom;
            if (!std::isfinite(c1) || !std::isfinite(c2)) {
                log_manager_.getLogger("ppp_model")->warn( "Non-finite coefficients for dual-frequency pair {} for {}", ref_type, obs.satellite);
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

            const double sin_el = std::sin(elevation * Constants::PI / 180.0);
            if (sin_el > 1e-6) {
                variance_value = variance_value / (sin_el * sin_el);
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
            };

        bool success = false;

        for (const auto& p : code_pairs) {
            success = process_pair(p, 'C') || success;
        }
        for (const auto& p : phase_pairs) {
            success = process_pair(p, 'L') || success;
        }

        // Remove all raw RINEX measurements from ppp_obs.observed to prevent confusion
        // downstream. Only the combined iono-free keys (e.g., "C1W_C2W") should remain.
        // Raw measurements are identified by not containing '_' in their key.
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

    bool PppModel::formIonoFreeMeasurements(const RinexObservation& obs, System system,
        const std::vector<std::pair<std::string, std::string>>& selected_signals,
        std::map<std::string, double>& out_observed,
        std::map<std::string, double>& out_variances) const {

        out_observed.clear();
        out_variances.clear();

        const auto& n = config_.getDefaultMeasurementNoise();
        const double default_code_var = n.code_std_m * n.code_std_m;
        const double default_phase_var = n.phase_std_m * n.phase_std_m;

        for (const auto& pair : selected_signals) {
            if (pair.first.empty() || pair.second.empty()) {
                log_manager_.getLogger("ppp_model")->trace("Skipping pair with empty member: {}/{}", pair.first, pair.second);
                continue;
            }
            // must be same measurement family (both code or both phase) for LC formation here
            if (pair.first[0] != pair.second[0]) {
                log_manager_.getLogger("ppp_model")->warn("Mixed-signal pair unsupported for IF formation: {} / {}", pair.first, pair.second);
                continue;
            }
            if (!obs.measurements.count(pair.first) || !obs.measurements.count(pair.second)) {
                log_manager_.getLogger("ppp_model")->trace("Missing measurement(s) for pair {} / {} on {}", pair.first, pair.second, obs.satellite);
                continue;
            }

            // read raw measurements and variances (convert phase cycles to meters here)
            double meas_vals[2] = { 0.0, 0.0 };
            double var_vals[2] = { 0.0, 0.0 };
            bool is_phase = (pair.first[0] == 'L');

            for (size_t j = 0; j < 2; ++j) {
                const std::string& type = (j == 0 ? pair.first : pair.second);
                double meas = obs.measurements.at(type);
                if (is_phase) {
                    double wl = ppp_utils_.getWavelength(obs.system, type);
                    if (wl <= 0.0) {
                        log_manager_.getLogger("ppp_model")->warn("Invalid wavelength for phase type {} on {}", type, obs.satellite);
                        meas_vals[0] = meas_vals[1] = 0.0;
                        break;
                    }
                    meas *= wl; // cycles -> meters
                    var_vals[j] = obs.variances.count(type) ? obs.variances.at(type) : default_phase_var;
                }
                else {
                    var_vals[j] = obs.variances.count(type) ? obs.variances.at(type) : default_code_var;
                }
                meas_vals[j] = meas;
            }

            if (meas_vals[0] == 0.0 && meas_vals[1] == 0.0) {
                continue;
            }

            // compute frequency-dependent IF coefficients
            double f1 = ppp_utils_.getFrequency(pair.first, system, obs.satellite);
            double f2 = ppp_utils_.getFrequency(pair.second, system, obs.satellite);
            if (f1 <= 0.0 || f2 <= 0.0 || std::abs(f1 - f2) < 1e-9) {
                log_manager_.getLogger("ppp_model")->warn("Invalid frequencies for IF formation: {} ({:.3f} MHz), {} ({:.3f} MHz)",
                    pair.first, f1 / 1e6, pair.second, f2 / 1e6);
                continue;
            }

            const double f1_sq = f1 * f1;
            const double f2_sq = f2 * f2;
            const double denom = (f1_sq - f2_sq);
            if (!std::isfinite(denom) || std::abs(denom) < 1e-12) {
                log_manager_.getLogger("ppp_model")->warn("Denominator too small for IF coefficients for pair {}_{} on {}", pair.first, pair.second, obs.satellite);
                continue;
            }
            const double c1 = f1_sq / denom;
            const double c2 = -f2_sq / denom;
            if (!std::isfinite(c1) || !std::isfinite(c2)) {
                log_manager_.getLogger("ppp_model")->warn("Non-finite IF coefficients for pair {}_{} on {}", pair.first, pair.second, obs.satellite);
                continue;
            }

            const std::string ref = pair.first + "_" + pair.second;
            const double observed_comb = c1 * meas_vals[0] + c2 * meas_vals[1];
            const double var_comb = c1 * c1 * var_vals[0] + c2 * c2 * var_vals[1];

            if (!std::isfinite(observed_comb) || !std::isfinite(var_comb)) {
                log_manager_.getLogger("ppp_model")->warn("Non-finite IF result for {} on {}", ref, obs.satellite);
                continue;
            }

            out_observed[ref] = observed_comb;
            out_variances[ref] = var_comb; // pre elevation mapping (mapping done later)
            log_manager_.getLogger("ppp_model")->trace("Formed IF {} for {}: observed={:.3f} m, var={:.6f}", ref, obs.satellite, observed_comb, var_comb);
        }

        return !out_observed.empty();
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
        if (klobuchar_coeffs_.size() != 8) {
            log_manager_.getLogger("ppp_model")->warn("No Klobuchar coefficients available for {}", freq_type);
            return 0.0;
        }
        double delay = ionosphere_.computeKlobucharDelay(time, rx_pos, sat_pos, freq);
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