#include "GnssConfig.hpp"
#include "PppModel.hpp"
#include "PppTypes.hpp"
#include "RinexParser.hpp"
#include "StateManager.hpp"
#include "ProductParser.hpp"
#include "Constants.hpp"
#include "MeasurementModel.hpp"
#include "TimeUtils.hpp"
#include <algorithm>
#include <fstream>
#include <sstream>

namespace GnssProcessor {

    PppModel::PppModel(const GnssConfig& config, ProductParser& product_parser, LogManager& log_manager,
        const RinexParser& rinex_parser, StateManager& state_manager)
        : config_(config), product_parser_(product_parser), log_manager_(log_manager),
        rinex_parser_(rinex_parser), state_manager_(state_manager),
        ppp_utils_(log_manager), ionosphere_(config, log_manager, product_parser, rinex_parser),
        geometry_(config, product_parser, rinex_parser, log_manager),
        antenna_(config, product_parser, log_manager, rinex_parser),
        gnss_corrections_(log_manager), signal_priorities_(config.getSignalPriorities()) {
        log_manager_.getLogger("ppp_model")->info("PppModel initialized: mode={}, uncombined={}, elev_mask={}°, {} signal sets",
            config.getProcessingMode() == ProcessingMode::PPP ? "PPP" : "PPP-RTK",
            config.isUncombinedEnabled(), config.getElevationMaskDeg(), config.getSignalPriorities().size());
        klobuchar_coeffs_ = product_parser_.getKlobucharCoeffs();
    }

    void PppModel::initializeState(PppState& state) {
        size_t core_size = config_.getProcessingMode() == ProcessingMode::PPP ? 5 : 6;
        if (state.position.norm() == 0.0 || state.position.norm() > 1e9 || state.position.hasNaN() ||
            state.covariance.rows() != static_cast<long>(core_size)) {
            Eigen::Vector3d init_pos = rinex_parser_.getApproxPositionXyz();
            if (init_pos.norm() > 1e3 && !init_pos.hasNaN()) {
                state.position = init_pos;
                log_manager_.getLogger("ppp_model")->info("Initialized position from RINEX: ({:.3f}, {:.3f}, {:.3f}) m, norm: {:.3f}",
                    init_pos.x(), init_pos.y(), init_pos.z(), init_pos.norm());
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
            state.troposphere_zwd = 0.0;
            if (config_.getProcessingMode() == ProcessingMode::PPP_RTK) {
                if (init_pos.norm() > 1e3 && !init_pos.hasNaN() && init_time.tow > 0.0 && klobuchar_coeffs_.size() == 8) {
                    state.ionosphere_zenith_delay = ionosphere_.computeInitialIonoDelay(init_time, init_pos);
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
            ppp_obs.elevation = 0.0;
            ppp_obs.azimuth = 0.0;
            ppp_obs.los = Eigen::Vector3d::Zero();
            ppp_obs.range = 0.0;
            ppp_obs.mf_w = 0.0;
            ppp_obs.mf_d = 0.0;

            Eigen::Vector3d sat_pos, rx_pos, los;
            double elevation, azimuth, range, tropo_zhd, sat_clock, shapiro_delay, sagnac_corr, atm_loading_corr, relativistic_corr, solid_tide, mf_w, mf_d;
            if (!geometry_.computeSatelliteGeometry(obs, state, sat_pos, rx_pos, elevation, azimuth, los, range, tropo_zhd,
                sat_clock, shapiro_delay, sagnac_corr, atm_loading_corr, relativistic_corr, solid_tide, mf_w, mf_d)) {
                continue;
            }

            ppp_obs.sat_pos = sat_pos;
            ppp_obs.elevation = elevation;
            ppp_obs.azimuth = azimuth;
            ppp_obs.los = los;
            ppp_obs.range = range;
            ppp_obs.mf_w = mf_w;
            ppp_obs.mf_d = mf_d;

            log_manager_.getLogger("ppp_model")->trace("Stored geometry for {} at TOW {:.3f}: sat_pos=({:.3f}, {:.3f}, {:.3f}) m, "
                "elevation={:.3f} deg, azimuth={:.3f} deg, los=({:.3f}, {:.3f}, {:.3f}), range={:.3f} m",
                obs.satellite, obs.time.tow,
                sat_pos.x(), sat_pos.y(), sat_pos.z(),
                elevation, azimuth,
                los.x(), los.y(), los.z(), range);

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

            if (config_.isUncombinedEnabled()) {
                // Uncombined path not yet implemented
            }

            std::vector<std::pair<std::string, std::string>> selected_signals;
            bool is_iono_free;
            std::string comb_type;
            if (!selectSignals(obs, system_str, selected_signals, is_iono_free, comb_type)) {
                continue;
            }
            if (is_iono_free && !config_.isUncombinedEnabled()) {
                if (!processIonoFreeObservation(obs, state, ppp_obs, system_str, selected_signals, comb_type, rx_pos, sat_pos,
                    elevation, azimuth, rx_antenna_type, range, tropo_zhd, sat_clock,
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
            selected_signals = std::move(candidate_pairs);
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

    bool PppModel::processIonoFreeObservation(const RinexObservation& obs, PppState& state,
        PppObservation& ppp_obs, const std::string& system_str,
        const std::vector<std::pair<std::string, std::string>> selected_signals,
        const std::string& comb_type, const Eigen::Vector3d& rx_pos,
        const Eigen::Vector3d& sat_pos, double elevation, double azimuth,
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
                return false; // phase disabled
            }

            std::vector<double> modeled_obs(2, 0.0);
            std::vector<double> variances(2, 0.0);
            std::vector<double> frequencies(2, 0.0);
            std::vector<double> wavelengths(2, 0.0);

            if (meas_type == 'L') {
                wavelengths[0] = ppp_utils_.getWavelength(obs.system, sig_pair.first);
                wavelengths[1] = ppp_utils_.getWavelength(obs.system, sig_pair.second);
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
                    obs.satellite, type, obs.system, rx_pos, sat_pos,
                    elevation, azimuth, rx_antenna_type, obs.time);

                const double bias_corr = gnss_corrections_.getBiasCorrection(obs.satellite, type, obs.time);

                modeled_obs[j] = range - (sat_clock * Constants::SPEED_OF_LIGHT) + slant_dry_delay +
                    antenna_corr - bias_corr + shapiro_delay + sagnac_corr + atm_loading_corr + solid_tide;

                const auto& n = config_.getDefaultMeasurementNoise();
                const double default_code_var = n.code_std_m * n.code_std_m;
                const double default_phase_var = n.phase_std_m * n.phase_std_m;

                variances[j] = obs.variances.count(type) ? obs.variances.at(type)
                    : (meas_type == 'C' ? default_code_var : default_phase_var);

                frequencies[j] = ppp_utils_.getFrequency(type, obs.system, obs.satellite);
                if (frequencies[j] <= 0.0) {
                    log_manager_.getLogger("ppp_model")->warn(
                        "Invalid frequency for type {} on system {} for satellite {}",
                        type, system_str, obs.satellite);
                    return false;
                }
            }

            const double f1_sq = frequencies[0] * frequencies[0];
            const double f2_sq = frequencies[1] * frequencies[1];
            const double denom = (f1_sq - f2_sq);
            if (!std::isfinite(denom) || std::abs(denom) < 1e-12) {
                log_manager_.getLogger("ppp_model")->warn(
                    "Invalid denominator for iono-free pair {} for {}",
                    ref_type, obs.satellite);
                return false;
            }

            const double c1 = f1_sq / denom;
            const double c2 = -f2_sq / denom;
            if (!std::isfinite(c1) || !std::isfinite(c2)) {
                log_manager_.getLogger("ppp_model")->warn(
                    "Non-finite coefficients for dual-frequency pair {} for {}",
                    ref_type, obs.satellite);
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

        return success;
    }

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

    const RinexParser& PppModel::getRinexParser() const {
        return rinex_parser_;
    }

} // namespace GnssProcessor
//#endif