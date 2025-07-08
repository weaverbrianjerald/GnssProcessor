#include "GnssConfig.hpp"
#include "PppModel.hpp"
#include "PppTypes.hpp"
#include "RinexParser.hpp"
#include "StateManager.hpp"
#include "ProductParser.hpp"
#include "Constants.hpp"
#include "MeasurementModel.hpp"
#include <algorithm>
#include <fstream>
#include <sstream>

namespace GnssProcessor {

    PppModel::PppModel(const GnssConfig& config, ProductParser& product_parser, LogManager& log_manager,
        const RinexParser& rinex_parser, StateManager& state_manager)
        : config_(config), product_parser_(product_parser), log_manager_(log_manager),
        rinex_parser_(rinex_parser), state_manager_(state_manager),
        signal_priorities_(config.getSignalPriorities()), elevation_threshold_(config.getElevationMaskDeg()),
        multipath_histories_(), klobuchar_coeffs_(), sun_ephemeris_(), orbits_(), clocks_(), antennas_(), biases_(), erps_() {
        loadProducts();
        log_manager_.getLogger()->info("PppModel initialized with mode: {}, uncombined_enabled: {}, elevation mask: {} degrees, {} signal priority sets",
            config_.getProcessingMode() == ProcessingMode::PPP ? "PPP" : "PPP-RTK",
            config_.isUncombinedEnabled(), elevation_threshold_, signal_priorities_.size());
    }

    void PppModel::loadProducts() {
        auto sp3_paths = product_parser_.getOrbitPaths();
        if (!sp3_paths.empty()) {
            if (product_parser_.parseSp3(sp3_paths, orbits_)) {
                std::string paths_str = std::accumulate(sp3_paths.begin(), sp3_paths.end(), std::string(),
                    [](const std::string& a, const std::filesystem::path& p) {
                        return a.empty() ? p.string() : a + ", " + p.string();
                    });
                log_manager_.getLogger()->info("Loaded SP3 files: [{}]", paths_str);
            }
            else {
                log_manager_.getLogger()->error("Failed to load SP3 files: [{}]",
                    std::accumulate(sp3_paths.begin(), sp3_paths.end(), std::string(),
                        [](const std::string& a, const std::filesystem::path& p) {
                            return a.empty() ? p.string() : a + ", " + p.string();
                        }));
            }
        }
        else {
            log_manager_.getLogger()->warn("No SP3 file paths provided for orbit data");
        }

        std::filesystem::path clk_path = product_parser_.getProductPath("CLK");
        if (!clk_path.empty()) {
            if (product_parser_.parseClk(clk_path, clocks_)) {
                log_manager_.getLogger()->info("Loaded CLK file: {}", clk_path.string());
            }
            else {
                log_manager_.getLogger()->error("Failed to load CLK file: {}", clk_path.string());
            }
        }
        else {
            log_manager_.getLogger()->warn("No CLK file path provided");
        }

        std::filesystem::path atx_path = product_parser_.getProductPath("ATX");
        if (!atx_path.empty()) {
            if (product_parser_.parseAtx(atx_path, antennas_)) {
                log_manager_.getLogger()->info("Loaded ATX file: {}", atx_path.string());
            }
            else {
                log_manager_.getLogger()->error("Failed to load ATX file: {}", atx_path.string());
            }
        }
        else {
            log_manager_.getLogger()->warn("No ATX file path provided");
        }

        std::filesystem::path bia_path = product_parser_.getProductPath("BIA");
        if (!bia_path.empty()) {
            if (product_parser_.parseBia(bia_path, biases_)) {
                log_manager_.getLogger()->info("Loaded BIA file: {}", bia_path.string());
            }
            else {
                log_manager_.getLogger()->error("Failed to load BIA file: {}", bia_path.string());
            }
        }
        else {
            log_manager_.getLogger()->warn("No BIA file path provided");
        }

        std::filesystem::path erp_path = product_parser_.getProductPath("ERP");
        if (!erp_path.empty()) {
            if (product_parser_.parseErp(erp_path, erps_)) {
                log_manager_.getLogger()->info("Loaded ERP file: {}", erp_path.string());
            }
            else {
                log_manager_.getLogger()->error("Failed to load ERP file: {}", erp_path.string());
            }
        }
        else {
            log_manager_.getLogger()->warn("No ERP file path provided");
        }

        std::filesystem::path rnx_path = product_parser_.getProductPath("RNX");
        if (!rnx_path.empty()) {
            if (product_parser_.parseKlobuchar(rnx_path, klobuchar_coeffs_)) {
                log_manager_.getLogger()->info("Loaded Klobuchar coefficients from RNX file: {}, coeffs size: {}",
                    rnx_path.string(), klobuchar_coeffs_.size());
            }
            else {
                log_manager_.getLogger()->error("Failed to load Klobuchar coefficients from RNX file: {}", rnx_path.string());
            }
            if (product_parser_.parseSunEphemeris(rnx_path, sun_ephemeris_)) {
                log_manager_.getLogger()->info("Loaded Sun ephemeris from RNX file: {}, ephemeris size: {}",
                    rnx_path.string(), sun_ephemeris_.size());
            }
            else {
                log_manager_.getLogger()->error("Failed to load Sun ephemeris from RNX file: {}", rnx_path.string());
            }
        }
        else {
            log_manager_.getLogger()->warn("No RNX file path provided for Klobuchar or Sun ephemeris");
        }
    }

    void PppModel::initializeState(PppState& state) {
        size_t core_size = config_.getProcessingMode() == ProcessingMode::PPP ? 5 : 6;
        if (state.position.norm() == 0.0 || state.position.norm() > 1e9 || state.position.hasNaN() ||
            state.covariance.rows() != static_cast<long>(core_size)) {
            Eigen::Vector3d init_pos = rinex_parser_.getApproxPositionXyz();
            if (init_pos.norm() > 1e3 && !init_pos.hasNaN()) {
                state.position = init_pos;
                log_manager_.getLogger()->info("Initialized position from RINEX: ({:.3f}, {:.3f}, {:.3f}) m, norm: {:.3f}",
                    init_pos.x(), init_pos.y(), init_pos.z(), init_pos.norm());
            }
            else {
                state.position = Eigen::Vector3d::Zero();
                log_manager_.getLogger()->warn("Invalid or zero RINEX approximate position, using default: (0, 0, 0) m");
            }

            GnssTime init_time = rinex_parser_.getTimeOfFirstObs();
            if (init_time.tow > 0.0 && init_time.week > 0) {
                state.epoch_time = init_time;
                log_manager_.getLogger()->info("Initialized state epoch time from RINEX: week {}, TOW {:.3f}",
                    init_time.week, init_time.tow);
            }
            else {
                state.epoch_time = GnssTime{ 0, 0.0 };
                log_manager_.getLogger()->warn("Invalid RINEX time of first observation, using default: TOW 0.0");
            }

            state.clock_bias = 0.0;
            state.troposphere_zenith_delay = 2.0;
            if (config_.getProcessingMode() == ProcessingMode::PPP_RTK) {
                if (init_pos.norm() > 1e3 && !init_pos.hasNaN() && init_time.tow > 0.0 && klobuchar_coeffs_.size() == 8) {
                    state.ionosphere_zenith_delay = computeInitialIonoDelay(init_time, init_pos);
                    log_manager_.getLogger()->info("Initialized ionospheric zenith delay from Klobuchar: {:.3f} m",
                        state.ionosphere_zenith_delay);
                }
                else {
                    state.ionosphere_zenith_delay = 5.0;
                    log_manager_.getLogger()->warn("Using default ionospheric zenith delay: 5.0 m (insufficient Klobuchar data)");
                }
            }

            state.covariance = Eigen::MatrixXd::Identity(core_size, core_size);
            const auto& initial_std = config_.getInitialStd();
            double pos_std = initial_std.position;
            double clock_std = initial_std.clock;
            double tropo_std = initial_std.troposphere;
            double iono_std = initial_std.ionosphere;
            if (pos_std <= 0 || clock_std <= 0 || tropo_std <= 0 ||
                (config_.getProcessingMode() == ProcessingMode::PPP_RTK && iono_std <= 0)) {
                log_manager_.getLogger()->warn("Invalid initial standard deviations, using defaults: pos=100, clock=1000, tropo=0.5, iono=5.0");
                pos_std = 100.0;
                clock_std = 1000.0;
                tropo_std = 0.5;
                iono_std = 5.0;
            }
            state.covariance.block(0, 0, 3, 3) *= pos_std * pos_std;
            state.covariance(3, 3) = clock_std * clock_std;
            state.covariance(4, 4) = tropo_std * tropo_std;
            if (config_.getProcessingMode() == ProcessingMode::PPP_RTK) {
                state.covariance(5, 5) = iono_std * iono_std;
            }
            log_manager_.getLogger()->debug("Initialized state covariance: pos_std={:.3f}, clock_std={:.3f}, tropo_std={:.3f}{}",
                pos_std, clock_std, tropo_std,
                config_.getProcessingMode() == ProcessingMode::PPP_RTK ? ", iono_std=" + std::to_string(iono_std) : "");
        }
        else {
            log_manager_.getLogger()->debug("State position already initialized: ({:.3f}, {:.3f}, {:.3f}) m, norm: {:.3f}",
                state.position.x(), state.position.y(), state.position.z(), state.position.norm());
        }
    }

    bool PppModel::computeSatelliteGeometry(const RinexObservation& obs, const PppState& state,
        Eigen::Vector3d& sat_pos, Eigen::Vector3d& rx_pos, double& elevation,
        double& azimuth, Eigen::Vector3d& los, double& range, double& tropo_delay,
        double& sat_clock, double& shapiro_delay, double& sagnac_corr,
        double& atm_loading_corr, double& relativistic_corr) const {
        sat_pos = getSatellitePosition(obs.satellite, obs.time);
        if (sat_pos.norm() < 1e3 || !sat_pos.allFinite()) {
            log_manager_.getLogger()->warn("Invalid satellite position for {} at TOW {:.3f}", obs.satellite, obs.time.tow);
            return false;
        }

        rx_pos = state.position;
        if (rx_pos.norm() < 1e3 || !rx_pos.allFinite()) {
            log_manager_.getLogger()->warn("Invalid receiver position for {} at TOW {:.3f}", obs.satellite, obs.time.tow);
            return false;
        }

        elevation = computeElevation(rx_pos, sat_pos);
        if (elevation < elevation_threshold_) {
            log_manager_.getLogger()->debug("Satellite {} below elevation threshold: {:.3f} deg < {:.3f} deg",
                obs.satellite, elevation, elevation_threshold_);
            return false;
        }

        azimuth = computeAzimuth(rx_pos, sat_pos);
        los = (sat_pos - rx_pos).normalized();
        range = computeGeometricRange(rx_pos, sat_pos);
        tropo_delay = getTroposphericDelay(elevation, state.troposphere_zenith_delay);
        sat_clock = getSatelliteClockCorrection(obs.satellite, obs.time);
        shapiro_delay = computeShapiroDelay(obs.satellite, rx_pos, sat_pos);
        sagnac_corr = computeSagnacCorrection(rx_pos, sat_pos);
        atm_loading_corr = computeAtmosphericLoadingCorrection(rx_pos, obs.time);
        relativistic_corr = computeRelativisticCorrection(obs.satellite, sat_pos, getSatelliteVelocity(obs.satellite, obs.time));

        log_manager_.getLogger()->trace("Satellite geometry for {} at TOW {:.3f}: range={:.3f} m, elevation={:.3f} deg, azimuth={:.3f} deg, "
            "tropo_delay={:.3f} m, sat_clock={:.3e} s, shapiro_delay={:.3e} m, sagnac_corr={:.3f} m, "
            "atm_loading_corr={:.3f} m, relativistic_corr={:.3e} s",
            obs.satellite, obs.time.tow, range, elevation, azimuth, tropo_delay, sat_clock, shapiro_delay, sagnac_corr, atm_loading_corr, relativistic_corr);
        return true;
    }

    double PppModel::computeShapiroDelay(const std::string& satellite, const Eigen::Vector3d& rx_pos,
        const Eigen::Vector3d& sat_pos) const {
        constexpr double GM = Constants::GRAVITATIONAL_CONSTANT; // GM = G * M_Earth (m³/s²)
        constexpr double C_CUBED = Constants::SPEED_OF_LIGHT * Constants::SPEED_OF_LIGHT * Constants::SPEED_OF_LIGHT;
        constexpr double MIN_POSITION_NORM = 1.8e7; // ~20,000 km
        constexpr double MAX_POSITION_NORM = 3.2e7; // ~32,000 km
        constexpr double MAX_DELAY = 1e-4; // Max 0.1 ms

        // Validate inputs
        std::string system_str = satellite.substr(0, 1) == "G" ? "GPS" : satellite.substr(0, 1) == "E" ? "Galileo" : "Unknown";
        if (!rx_pos.allFinite() || !sat_pos.allFinite()) {
            log_manager_.getLogger()->warn("Invalid Shapiro delay inputs for {} ({}): rx_pos=({:.3f}, {:.3f}, {:.3f}), sat_pos=({:.3f}, {:.3f}, {:.3f})",
                satellite, system_str, rx_pos.x(), rx_pos.y(), rx_pos.z(), sat_pos.x(), sat_pos.y(), sat_pos.z());
            return 0.0;
        }
        double r = (sat_pos - rx_pos).norm();
        double sat_norm = sat_pos.norm();
        if (r < 1e3 || sat_norm < MIN_POSITION_NORM || sat_norm > MAX_POSITION_NORM) {
            log_manager_.getLogger()->warn("Unrealistic Shapiro delay norms for {} ({}): range={:.3e} m, sat_norm={:.3e} m",
                satellite, system_str, r, sat_norm);
            return 0.0;
        }

        // Compute Shapiro delay: (2GM/c³) * ln((r + r_s + r_r)/(r + r_s - r_r))
        double r_rx = rx_pos.norm();
        double delay = (2.0 * GM / C_CUBED) * std::log((r + sat_norm + r_rx) / (r + sat_norm - r_rx));
        if (!std::isfinite(delay) || std::abs(delay) > MAX_DELAY) {
            log_manager_.getLogger()->warn("Invalid Shapiro delay for {} ({}): {:.3e} s, range={:.3e} m, sat_norm={:.3e} m, rx_norm={:.3e} m",
                satellite, system_str, delay, r, sat_norm, r_rx);
            return 0.0;
        }

        // Convert to meters
        delay *= Constants::SPEED_OF_LIGHT;
        log_manager_.getLogger()->trace("Shapiro delay for {} ({}): {:.3e} m, range={:.3e} m, sat_norm={:.3e} m, rx_norm={:.3e} m",
            satellite, system_str, delay, r, sat_norm, r_rx);
        return delay;
    }

    bool PppModel::computeModels(const std::vector<RinexObservation>& observations,
        PppState& state,
        std::vector<PppObservation>& modeled_obs) {
        if (observations.empty()) {
            log_manager_.getLogger()->error("No observations provided for PPP modeling");
            return false;
        }

        modeled_obs.clear();
        modeled_obs.reserve(observations.size());

        // Initialize state if needed
        initializeState(state);

        for (const auto& obs : observations) {
            PppObservation ppp_obs;
            ppp_obs.satellite = obs.satellite;
            ppp_obs.system = obs.system;
            ppp_obs.time = obs.time;
            ppp_obs.observed = obs.measurements;
            ppp_obs.variances = obs.variances;
            ppp_obs.lli_flags = obs.lli_flags;

            // Compute satellite geometry and corrections
            Eigen::Vector3d sat_pos, rx_pos, los;
            double elevation, azimuth, range, tropo_delay, sat_clock, shapiro_delay, sagnac_corr, atm_loading_corr, relativistic_corr;
            if (!computeSatelliteGeometry(obs, state, sat_pos, rx_pos, elevation, azimuth, los, range, tropo_delay,
                sat_clock, shapiro_delay, sagnac_corr, atm_loading_corr, relativistic_corr)) {
                continue;
            }

            // Get system string
            std::string system_str = MeasurementModel::getSystemString(obs.system);
            if (system_str == "Unknown") {
                log_manager_.getLogger()->warn("Unknown system for satellite {} at TOW {:.3f}", obs.satellite, obs.time.tow);
                continue;
            }

            // Select observation types
            auto obs_types = selectObservationTypes(obs.measurements, obs.system, system_str);
            if (obs_types.empty()) {
                log_manager_.getLogger()->warn("No valid observation types for system {} for satellite {}", system_str, obs.satellite);
                continue;
            }

            // Get receiver antenna type
            std::string rx_antenna_type = rinex_parser_.getReceiverAntennaType();

            // Process uncombined measurements if enabled
            if (config_.isUncombinedEnabled()) {
                for (const auto& type : obs_types) {
                    if (!processSingleFreqObservation(obs, state, ppp_obs, system_str, type, rx_pos, sat_pos, elevation,
                        azimuth, rx_antenna_type, range, tropo_delay, sat_clock, shapiro_delay,
                        sagnac_corr, atm_loading_corr, relativistic_corr)) {
                        continue;
                    }
                }
            }

            // Process ionosphere-free combination if available
            std::vector<std::string> selected_signals;
            bool is_iono_free;
            std::string comb_type;
            if (!selectSignals(obs, system_str, selected_signals, is_iono_free, comb_type)) {
                continue;
            }
            if (is_iono_free && !config_.isUncombinedEnabled()) {
                if (!processIonoFreeObservation(obs, state, ppp_obs, system_str, selected_signals, comb_type, rx_pos, sat_pos,
                    elevation, azimuth, rx_antenna_type, range, tropo_delay, sat_clock,
                    shapiro_delay, sagnac_corr, atm_loading_corr, relativistic_corr)) {
                    continue;
                }
            }

            if (!ppp_obs.modeled.empty()) {
                modeled_obs.push_back(ppp_obs);
            }
        }

        if (modeled_obs.empty()) {
            log_manager_.getLogger()->error("No valid PPP observations modeled");
            return false;
        }
        log_manager_.getLogger()->info("Modeled {} PPP observations for mode {}", modeled_obs.size(),
            config_.getProcessingMode() == ProcessingMode::PPP ? "PPP" : "PPP-RTK");
        return true;
    }

    bool PppModel::selectSignals(const RinexObservation& obs, const std::string& system_str,
        std::vector<std::string>& selected_signals, bool& is_iono_free,
        std::string& comb_type) {
        auto it = signal_priorities_.find(system_str);
        const auto& signals = (it != signal_priorities_.end()) ? it->second : std::vector<std::string>{};

        // Get available signals
        std::vector<std::string> available_signals;
        for (const auto& sig : signals) {
            if (obs.measurements.count(sig)) {
                available_signals.push_back(sig);
            }
        }

        // Map signals to frequency bands
        std::map<std::string, std::string> freq_band_to_signal;
        std::vector<std::string> freq_bands;
        for (const auto& sig : available_signals) {
            std::string band;
            if (system_str == "GPS") {
                if (sig.find("C1") == 0 || sig.find("L1") == 0) band = "L1";
                else if (sig.find("C2") == 0 || sig.find("L2") == 0) band = "L2";
                else if (sig.find("C5") == 0 || sig.find("L5") == 0) band = "L5";
            }
            else if (system_str == "Galileo") {
                if (sig.find("C1") == 0 || sig.find("L1") == 0) band = "E1";
                else if (sig.find("C5") == 0 || sig.find("L5") == 0) band = "E5a";
                else if (sig.find("C7") == 0 || sig.find("L7") == 0) band = "E5b";
                else if (sig.find("C6") == 0 || sig.find("L6") == 0) band = "E6";
                else if (sig.find("C8") == 0 || sig.find("L8") == 0) band = "E5";
            }
            else if (system_str == "GLONASS") {
                if (sig.find("C1") == 0 || sig.find("L1") == 0) band = "G1";
                else if (sig.find("C2") == 0 || sig.find("L2") == 0) band = "G2";
                else if (sig.find("C3") == 0 || sig.find("L3") == 0) band = "G3";
            }
            else if (system_str == "BeiDou") {
                if (sig.find("C2") == 0 || sig.find("L2") == 0) band = "B1";
                else if (sig.find("C7") == 0 || sig.find("L7") == 0) band = "B2";
                else if (sig.find("C6") == 0 || sig.find("L6") == 0) band = "B3";
                else if (sig.find("C1") == 0 || sig.find("L1") == 0) band = "B1C";
            }
            else if (system_str == "QZSS") {
                if (sig.find("C1") == 0 || sig.find("L1") == 0) band = "L1";
                else if (sig.find("C2") == 0 || sig.find("L2") == 0) band = "L2";
                else if (sig.find("C5") == 0 || sig.find("L5") == 0) band = "L5";
                else if (sig.find("C6") == 0 || sig.find("L6") == 0) band = "L6";
            }
            else if (system_str == "SBAS") {
                if (sig.find("C1") == 0 || sig.find("L1") == 0) band = "L1";
                else if (sig.find("C5") == 0 || sig.find("L5") == 0) band = "L5";
            }
            if (!band.empty() && std::find(freq_bands.begin(), freq_bands.end(), band) == freq_bands.end()) {
                freq_bands.push_back(band);
                // Prefer signals earlier in signal_priorities_
                if (!freq_band_to_signal.count(band) ||
                    std::find(signals.begin(), signals.end(), sig) < std::find(signals.begin(), signals.end(), freq_band_to_signal[band])) {
                    freq_band_to_signal[band] = sig;
                }
            }
        }

        is_iono_free = freq_bands.size() >= 2;
        comb_type = freq_bands.size() >= 3 ? "triple" : freq_bands.size() == 2 ? "dual" : "single";
        if (config_.isUncombinedEnabled()) {
            // For uncombined mode, include all available signals
            selected_signals = available_signals;
            log_manager_.getLogger()->trace("Selected uncombined signals for {} {}: [{}], mode={}",
                obs.satellite, system_str,
                std::accumulate(available_signals.begin(), available_signals.end(), std::string(),
                    [](const std::string& a, const std::string& b) {
                        return a.empty() ? b : a + "," + b;
                    }),
                config_.getProcessingMode() == ProcessingMode::PPP ? "PPP" : "PPP-RTK");
        }
        else if (is_iono_free) {
            if (comb_type == "triple") {
                std::vector<std::string> bands = system_str == "GPS" ? std::vector<std::string>{"L1", "L2", "L5"} :
                    system_str == "Galileo" ? std::vector<std::string>{"E1", "E5a", "E5b"} :
                    system_str == "GLONASS" ? std::vector<std::string>{"G1", "G2", "G3"} :
                    system_str == "BeiDou" ? std::vector<std::string>{"B1", "B2", "B3"} :
                    system_str == "QZSS" ? std::vector<std::string>{"L1", "L2", "L5"} :
                    std::vector<std::string>{ "L1", "L5" };
                for (const auto& band : bands) {
                    if (freq_band_to_signal.count(band)) {
                        selected_signals.push_back(freq_band_to_signal[band]);
                    }
                }
                if (selected_signals.size() < 3) {
                    comb_type = "dual";
                    selected_signals.resize(2);
                }
            }
            if (comb_type == "dual") {
                std::vector<std::string> bands = system_str == "GPS" ? std::vector<std::string>{"L1", "L2"} :
                    system_str == "Galileo" ? std::vector<std::string>{"E1", "E5a"} :
                    system_str == "GLONASS" ? std::vector<std::string>{"G1", "G2"} :
                    system_str == "BeiDou" ? std::vector<std::string>{"B1", "B3"} :
                    system_str == "QZSS" ? std::vector<std::string>{"L1", "L2"} :
                    std::vector<std::string>{ "L1", "L5" };
                selected_signals.clear();
                for (const auto& band : bands) {
                    if (freq_band_to_signal.count(band)) {
                        selected_signals.push_back(freq_band_to_signal[band]);
                    }
                }
                if (selected_signals.size() < 2) {
                    comb_type = "single";
                    selected_signals = available_signals.empty() ? std::vector<std::string>{} :
                        std::vector<std::string>{ available_signals[0] };
                }
            }
            if (comb_type != "single") {
                log_manager_.getLogger()->trace("Selected iono-free signals for {} {}: [{}{}{}], comb_type={}, mode={}",
                    obs.satellite, system_str,
                    selected_signals[0],
                    selected_signals.size() > 1 ? "," + selected_signals[1] : "",
                    selected_signals.size() > 2 ? "," + selected_signals[2] : "",
                    comb_type,
                    config_.getProcessingMode() == ProcessingMode::PPP ? "PPP" : "PPP-RTK");
            }
        }
        else {
            if (!available_signals.empty()) {
                selected_signals = { available_signals[0] };
                log_manager_.getLogger()->trace("Selected single-frequency signal for {} {}: {}, mode={}",
                    obs.satellite, system_str, selected_signals[0],
                    config_.getProcessingMode() == ProcessingMode::PPP ? "PPP" : "PPP-RTK");
            }
            else {
                log_manager_.getLogger()->warn("No valid signals for {} {} at TOW {:.3f}", obs.satellite, system_str, obs.time.tow);
                return false;
            }
        }
        return true;
    }

    bool PppModel::processIonoFreeObservation(const RinexObservation& obs, PppState& state,
        PppObservation& ppp_obs, const std::string& system_str,
        const std::vector<std::string>& selected_signals,
        const std::string& comb_type, const Eigen::Vector3d& rx_pos,
        const Eigen::Vector3d& sat_pos, double elevation, double azimuth,
        const std::string& rx_antenna_type, double range, double tropo_delay,
        double sat_clock, double shapiro_delay, double sagnac_corr,
        double atm_loading_corr, double relativistic_corr) {
        // Handle ionosphere-free combination
        double observed_value = 0.0;
        double modeled_value = 0.0;
        double variance_value = 0.0;
        std::string ref_type;

        if (comb_type == "triple") {
            if (selected_signals.size() < 3) {
                log_manager_.getLogger()->warn("Insufficient signals for triple-frequency combination for {}", obs.satellite);
                return false;
            }
            observed_value = computeTripleIonoFreeCombination(obs.measurements, obs.system,
                selected_signals[0], selected_signals[1], selected_signals[2]);
            std::vector<double> modeled_freqs(3, 0.0);
            std::vector<double> variances(3, 0.0);
            std::vector<double> frequencies(3, 0.0);
            for (size_t i = 0; i < 3; ++i) {
                const auto& type = selected_signals[i];
                double antenna_corr = getAntennaCorrection(obs.satellite, type, obs.system, rx_pos, sat_pos,
                    elevation, azimuth, rx_antenna_type, obs.time);
                double bias_corr = getBiasCorrection(obs.satellite, type, obs.time);
                modeled_freqs[i] = range + state.clock_bias * Constants::SPEED_OF_LIGHT + tropo_delay -
                    (sat_clock + relativistic_corr) * Constants::SPEED_OF_LIGHT +
                    antenna_corr + bias_corr + shapiro_delay + sagnac_corr + atm_loading_corr;
                variances[i] = obs.variances.count(type) ? obs.variances.at(type) : (type[0] == 'C' ? 0.3 * 0.3 : 0.0015 * 0.0015);
                frequencies[i] = getFrequency(type, obs.system, obs.satellite);
                if (frequencies[i] <= 0.0) {
                    log_manager_.getLogger()->warn("Invalid frequency for type {} on system {} for satellite {}",
                        type, system_str, obs.satellite);
                    return false;
                }
            }
            double f1_sq = frequencies[0] * frequencies[0], f2_sq = frequencies[1] * frequencies[1], f3_sq = frequencies[2] * frequencies[2];
            double denom = f1_sq * f2_sq * (f1_sq - f2_sq) + f2_sq * f3_sq * (f2_sq - f3_sq) +
                f3_sq * f1_sq * (f3_sq - f1_sq);
            if (std::abs(denom) < 1e-6) {
                log_manager_.getLogger()->warn("Near-zero denominator for triple-frequency combination for {}", obs.satellite);
                return false;
            }
            double c1 = (f2_sq * f3_sq * (f2_sq - f3_sq)) / denom;
            double c2 = (f3_sq * f1_sq * (f3_sq - f1_sq)) / denom;
            double c3 = (f1_sq * f2_sq * (f1_sq - f2_sq)) / denom;
            if (!std::isfinite(c1) || !std::isfinite(c2) || !std::isfinite(c3)) {
                log_manager_.getLogger()->warn("Non-finite coefficients for triple-frequency combination for {}", obs.satellite);
                return false;
            }
            modeled_value = c1 * modeled_freqs[0] + c2 * modeled_freqs[1] + c3 * modeled_freqs[2];
            variance_value = c1 * c1 * variances[0] + c2 * c2 * variances[1] + c3 * c3 * variances[2];
            ref_type = selected_signals[0] + "_" + selected_signals[1] + "_" + selected_signals[2];

            // Handle iono-free ambiguity for phase measurements
            if (selected_signals[0][0] == 'L' || selected_signals[1][0] == 'L' || selected_signals[2][0] == 'L') {
                std::string amb_key = obs.satellite + "_" + ref_type;
                if (!state.ambiguities.count(amb_key)) {
                    double effective_wavelength = (c1 * getWavelength(obs.system, selected_signals[0]) +
                        c2 * getWavelength(obs.system, selected_signals[1]) +
                        c3 * getWavelength(obs.system, selected_signals[2])) / (c1 + c2 + c3);
                    if (effective_wavelength <= 0.0) {
                        log_manager_.getLogger()->warn("Invalid effective wavelength for {} {}: {:.3f}",
                            obs.satellite, ref_type, effective_wavelength);
                        return false;
                    }
                    state.ambiguities[amb_key] = observed_value - modeled_value;
                    state.ambiguities[amb_key] = std::round(state.ambiguities[amb_key] / effective_wavelength) * effective_wavelength;
                    state_manager_.addAmbiguity(amb_key, state.ambiguities[amb_key]);
                    log_manager_.getLogger()->debug("Initialized iono-free ambiguity for {}: {:.3f} cycles, effective wavelength: {:.3f} m",
                        amb_key, state.ambiguities[amb_key] / effective_wavelength, effective_wavelength);
                }
                modeled_value += state.ambiguities[amb_key];
            }
            log_manager_.getLogger()->trace("Triple-frequency iono-free for {}: observed={:.3f} m, modeled={:.3f} m, "
                "freqs=[{:.3f}, {:.3f}, {:.3f}] MHz, coeffs=[{:.3f}, {:.3f}, {:.3f}], mode={}",
                obs.satellite, observed_value, modeled_value,
                frequencies[0] / 1e6, frequencies[1] / 1e6, frequencies[2] / 1e6, c1, c2, c3,
                config_.getProcessingMode() == ProcessingMode::PPP ? "PPP" : "PPP-RTK");
        }
        else if (comb_type == "dual") {
            if (selected_signals.size() < 2) {
                log_manager_.getLogger()->warn("Insufficient signals for dual-frequency combination for {}", obs.satellite);
                return false;
            }
            observed_value = computeIonoFreeCombination(obs.measurements, obs.system,
                selected_signals[0], selected_signals[1]);
            std::vector<double> modeled_freqs(2, 0.0);
            std::vector<double> variances(2, 0.0);
            std::vector<double> frequencies(2, 0.0);
            for (size_t i = 0; i < 2; ++i) {
                const auto& type = selected_signals[i];
                double antenna_corr = getAntennaCorrection(obs.satellite, type, obs.system, rx_pos, sat_pos,
                    elevation, azimuth, rx_antenna_type, obs.time);
                double bias_corr = getBiasCorrection(obs.satellite, type, obs.time);
                modeled_freqs[i] = range + state.clock_bias * Constants::SPEED_OF_LIGHT + tropo_delay -
                    (sat_clock + relativistic_corr) * Constants::SPEED_OF_LIGHT +
                    antenna_corr + bias_corr + shapiro_delay + sagnac_corr + atm_loading_corr;
                variances[i] = obs.variances.count(type) ? obs.variances.at(type) : (type[0] == 'C' ? 0.3 * 0.3 : 0.0015 * 0.0015);
                frequencies[i] = getFrequency(type, obs.system, obs.satellite);
                if (frequencies[i] <= 0.0) {
                    log_manager_.getLogger()->warn("Invalid frequency for type {} on system {} for satellite {}",
                        type, system_str, obs.satellite);
                    return false;
                }
            }
            double f1_sq = frequencies[0] * frequencies[0], f2_sq = frequencies[1] * frequencies[1];
            double c1 = f1_sq / (f1_sq - f2_sq);
            double c2 = -f2_sq / (f1_sq - f2_sq);
            if (!std::isfinite(c1) || !std::isfinite(c2)) {
                log_manager_.getLogger()->warn("Non-finite coefficients for dual-frequency combination for {}", obs.satellite);
                return false;
            }
            modeled_value = c1 * modeled_freqs[0] + c2 * modeled_freqs[1];
            variance_value = c1 * c1 * variances[0] + c2 * c2 * variances[1];
            ref_type = selected_signals[0] + "_" + selected_signals[1];

            // Handle iono-free ambiguity for phase measurements
            if (selected_signals[0][0] == 'L' || selected_signals[1][0] == 'L') {
                std::string amb_key = obs.satellite + "_" + ref_type;
                if (!state.ambiguities.count(amb_key)) {
                    double effective_wavelength = (c1 * getWavelength(obs.system, selected_signals[0]) +
                        c2 * getWavelength(obs.system, selected_signals[1])) / (c1 + c2);
                    if (effective_wavelength <= 0.0) {
                        log_manager_.getLogger()->warn("Invalid effective wavelength for {} {}: {:.3f}",
                            obs.satellite, ref_type, effective_wavelength);
                        return false;
                    }
                    state.ambiguities[amb_key] = observed_value - modeled_value;
                    state.ambiguities[amb_key] = std::round(state.ambiguities[amb_key] / effective_wavelength) * effective_wavelength;
                    state_manager_.addAmbiguity(amb_key, state.ambiguities[amb_key]);
                    log_manager_.getLogger()->debug("Initialized iono-free ambiguity for {}: {:.3f} cycles, effective wavelength: {:.3f} m",
                        amb_key, state.ambiguities[amb_key] / effective_wavelength, effective_wavelength);
                }
                modeled_value += state.ambiguities[amb_key];
            }
            log_manager_.getLogger()->trace("Dual-frequency iono-free for {}: observed={:.3f} m, modeled={:.3f} m, "
                "freqs=[{:.3f}, {:.3f}] MHz, coeffs=[{:.3f}, {:.3f}], mode={}",
                obs.satellite, observed_value, modeled_value,
                frequencies[0] / 1e6, frequencies[1] / 1e6, c1, c2,
                config_.getProcessingMode() == ProcessingMode::PPP ? "PPP" : "PPP-RTK");
        }

        ppp_obs.modeled[ref_type] = modeled_value;
        ppp_obs.residuals[ref_type] = observed_value - modeled_value;
        ppp_obs.variances[ref_type] = variance_value;

        // Apply multipath mitigation for code observations
        if (ref_type[0] == 'C') {
            double mp_weight = computeMultipathVarianceWeight(ppp_obs, selected_signals[0], elevation);
            ppp_obs.variances[ref_type] *= (1.0 / mp_weight);
            log_manager_.getLogger()->trace("Multipath weight for {} {}: {:.3f}, adjusted variance: {:.6f}",
                obs.satellite, ref_type, mp_weight, ppp_obs.variances[ref_type]);
        }

        log_manager_.getLogger()->trace("Iono-free {} for {}: observed={:.3f} m, modeled={:.3f} m, residual={:.3f} m, comb_type={}, mode={}",
            ref_type, obs.satellite, observed_value, modeled_value, ppp_obs.residuals[ref_type],
            comb_type, config_.getProcessingMode() == ProcessingMode::PPP ? "PPP" : "PPP-RTK");
        return true;
    }

    bool PppModel::processSingleFreqObservation(const RinexObservation& obs, PppState& state,
        PppObservation& ppp_obs, const std::string& system_str,
        const std::string& type, const Eigen::Vector3d& rx_pos,
        const Eigen::Vector3d& sat_pos, double elevation, double azimuth,
        const std::string& rx_antenna_type, double range, double tropo_delay,
        double sat_clock, double shapiro_delay, double sagnac_corr,
        double atm_loading_corr, double relativistic_corr) {
        if (!obs.measurements.count(type)) {
            log_manager_.getLogger()->warn("Missing measurement for {} {} in uncombined mode", obs.satellite, type);
            return false;
        }
        double observed_value = obs.measurements.at(type);
        double modeled_value = 0.0;
        double variance_value = obs.variances.count(type) ? obs.variances.at(type) : (type[0] == 'C' ? 0.3 * 0.3 : 0.0015 * 0.0015);

        try {
            double iono_delay = config_.getProcessingMode() == ProcessingMode::PPP_RTK ?
                computeIonosphericDelay(obs.time, rx_pos, sat_pos, type, obs.system, obs.measurements) :
                computeInitialIonoDelay(obs.time, rx_pos);
            double antenna_corr = getAntennaCorrection(obs.satellite, type, obs.system, rx_pos, sat_pos,
                elevation, azimuth, rx_antenna_type, obs.time);
            double bias_corr = getBiasCorrection(obs.satellite, type, obs.time);
            modeled_value = range + state.clock_bias * Constants::SPEED_OF_LIGHT + tropo_delay -
                (sat_clock + relativistic_corr) * Constants::SPEED_OF_LIGHT + iono_delay +
                antenna_corr + bias_corr + shapiro_delay + sagnac_corr + atm_loading_corr;

            if (type[0] == 'L') {
                std::string amb_key = obs.satellite + "_" + type;
                if (!state.ambiguities.count(amb_key)) {
                    double wavelength = getWavelength(obs.system, type);
                    if (wavelength <= 0.0) {
                        log_manager_.getLogger()->warn("Invalid wavelength for type {} on system {} for satellite {}",
                            type, system_str, obs.satellite);
                        return false;
                    }
                    state.ambiguities[amb_key] = observed_value - modeled_value;
                    state.ambiguities[amb_key] = std::round(state.ambiguities[amb_key] / wavelength) * wavelength;
                    state_manager_.addAmbiguity(amb_key, state.ambiguities[amb_key]);
                    log_manager_.getLogger()->debug("Initialized uncombined ambiguity for {}: {:.3f} cycles, wavelength: {:.3f} m",
                        amb_key, state.ambiguities[amb_key] / wavelength, wavelength);
                }
                modeled_value += state.ambiguities[amb_key];
            }

            ppp_obs.modeled[type] = modeled_value;
            ppp_obs.residuals[type] = observed_value - modeled_value;
            ppp_obs.variances[type] = variance_value;

            // Apply multipath mitigation for code observations
            if (type[0] == 'C') {
                double mp_weight = computeMultipathVarianceWeight(ppp_obs, type, elevation);
                ppp_obs.variances[type] *= (1.0 / mp_weight);
                log_manager_.getLogger()->trace("Multipath weight for {} {}: {:.3f}, adjusted variance: {:.6f}",
                    obs.satellite, type, mp_weight, ppp_obs.variances[type]);
            }

            log_manager_.getLogger()->trace("Uncombined {} for {}: observed={:.3f} m, modeled={:.3f} m, residual={:.3f} m, mode={}",
                type, obs.satellite, observed_value, modeled_value, ppp_obs.residuals[type],
                config_.getProcessingMode() == ProcessingMode::PPP ? "PPP" : "PPP-RTK");
            return true;
        }
        catch (const std::exception& e) {
            log_manager_.getLogger()->warn("Error processing uncombined measurement for {} {}: {}, mode={}",
                obs.satellite, type, e.what(),
                config_.getProcessingMode() == ProcessingMode::PPP ? "PPP" : "PPP-RTK");
            return false;
        }
    }

    double PppModel::computeGeometricRange(const Eigen::Vector3d& rx_pos, const Eigen::Vector3d& sat_pos) const {
        return (sat_pos - rx_pos).norm();
    }

    double PppModel::getTroposphericDelay(double elevation, double zenith_delay) const {
        // Validate inputs
        if (elevation < 0.0 || elevation > 90.0) {
            log_manager_.getLogger()->warn("Invalid elevation for tropospheric delay: {:.3f} deg", elevation);
            return 0.0;
        }
        if (zenith_delay < 0.5 || zenith_delay > 5.0 || !std::isfinite(zenith_delay)) {
            log_manager_.getLogger()->warn("Invalid zenith delay: {:.3f} m (expected 0.5 to 5.0 m)", zenith_delay);
            return 0.0;
        }

        // Compute mapping function (Saastamoinen-based) with numerical stability
        double el_rad = elevation * Constants::PI / 180.0;
        double sin_el = std::sin(el_rad);
        double tan_el = std::tan(el_rad);
        // Clamp denominator to avoid numerical instability at low elevations
        constexpr double MIN_DENOM = 0.01;
        double denom = sin_el + 0.00143 / (tan_el + 0.0445);
        if (denom < MIN_DENOM) {
            log_manager_.getLogger()->debug("Clamped mapping function denominator at {:.3f} for elevation {:.3f} deg", MIN_DENOM, elevation);
            denom = MIN_DENOM;
        }
        double mapping = 1.0 / denom;
        double delay = zenith_delay * mapping;

        // Validate output
        if (!std::isfinite(delay) || delay < 0.0) {
            log_manager_.getLogger()->warn("Non-finite or negative tropospheric delay: {:.3f} m, elevation: {:.3f} deg", delay, elevation);
            return 0.0;
        }

        log_manager_.getLogger()->trace("Computed tropospheric delay: {:.3f} m, elevation: {:.3f} deg, zenith_delay: {:.3f} m, mapping: {:.3f}",
            delay, elevation, zenith_delay, mapping);
        return delay;
    }

    double PppModel::computeIonosphericDelay(const GnssTime& time,
        const Eigen::Vector3d& rx_pos,
        const Eigen::Vector3d& sat_pos,
        const std::string& freq_type,
        System system,
        const std::map<std::string, double>& measurements) const {
        double freq = getFrequency(freq_type, system, "");
        if (freq <= 0.0) {
            log_manager_.getLogger()->warn("Invalid frequency for {} on system {}", freq_type, static_cast<int>(system));
            return 0.0;
        }

        std::string system_str = MeasurementModel::getSystemString(system);
        auto it = signal_priorities_.find(system_str);
        if (it == signal_priorities_.end()) {
            log_manager_.getLogger()->warn("No signal priorities defined for system {}", system_str);
            return 0.0;
        }
        const auto& signals = it->second;

        if (signals.size() >= 3 && measurements.count(signals[0]) && measurements.count(signals[1]) && measurements.count(signals[2])) {
            log_manager_.getLogger()->trace("Triple-frequency iono-free combination available for {}: ionospheric delay=0.0 m", freq_type);
            return 0.0; // Ionosphere-free, no delay
        }
        if (signals.size() >= 2 && measurements.count(signals[0]) && measurements.count(signals[1])) {
            log_manager_.getLogger()->trace("Dual-frequency iono-free combination available for {}: ionospheric delay=0.0 m", freq_type);
            return 0.0; // Ionosphere-free, no delay
        }
        if (klobuchar_coeffs_.size() != 8) {
            log_manager_.getLogger()->warn("No Klobuchar coefficients available for {}", freq_type);
            return 0.0;
        }
        double delay = computeKlobucharDelay(time, rx_pos, sat_pos, freq);
        log_manager_.getLogger()->trace("Computed Klobuchar ionospheric delay for {}: {:.3f} m", freq_type, delay);
        return delay;
    }

    double PppModel::computeKlobucharDelay(const GnssTime& time,
        const Eigen::Vector3d& rx_pos,
        const Eigen::Vector3d& sat_pos,
        double frequency) const {
        double a0 = klobuchar_coeffs_[0], a1 = klobuchar_coeffs_[1], a2 = klobuchar_coeffs_[2], a3 = klobuchar_coeffs_[3];
        double b0 = klobuchar_coeffs_[4], b1 = klobuchar_coeffs_[5], b2 = klobuchar_coeffs_[6], b3 = klobuchar_coeffs_[7];

        double t = fmod(time.tow, 86400.0);
        double phi = std::asin(rx_pos.z() / rx_pos.norm()) * 180.0 / Constants::PI; // Geodetic latitude (degrees)
        double lambda = std::atan2(rx_pos.y(), rx_pos.x()) * 180.0 / Constants::PI; // Geodetic longitude (degrees)
        double el = computeElevation(rx_pos, sat_pos);
        double az = computeAzimuth(rx_pos, sat_pos);
        if (az < 0.0) az += 360.0;

        double psi = 0.0137 / (el / 180.0 + 0.11) - 0.022;
        double phi_i = phi / 180.0 + psi * std::cos(az * Constants::PI / 180.0);
        if (phi_i > 0.416) phi_i = 0.416;
        if (phi_i < -0.416) phi_i = -0.416;
        double lambda_i = lambda / 180.0 + psi * std::sin(az * Constants::PI / 180.0) / std::cos(phi_i * Constants::PI);
        double phi_m = phi_i + 0.064 * std::cos((lambda_i - 1.617) * Constants::PI);

        double t_i = 43200.0 * lambda_i + t;
        t_i = fmod(t_i, 86400.0);
        if (t_i < 0) t_i += 86400.0;

        double F = 1.0 + 16.0 * std::pow((0.53 - el / 180.0), 3);
        double AMP = a0 + a1 * phi_m + a2 * std::pow(phi_m, 2) + a3 * std::pow(phi_m, 3);
        if (AMP < 0) AMP = 0;
        double PER = b0 + b1 * phi_m + b2 * std::pow(phi_m, 2) + b3 * std::pow(phi_m, 3);
        if (PER < 72000) PER = 72000;
        double X = 2.0 * Constants::PI * (t_i - 50400.0) / PER;
        double delay;
        if (std::abs(X) < 1.57) {
            delay = F * (5e-9 + AMP * (1 - X * X / 2 + X * X * X * X / 24));
        }
        else {
            delay = F * 5e-9;
        }

        double f_L1 = Constants::GPS_L1_FREQ;
        delay *= (f_L1 * f_L1) / (frequency * frequency);
        return delay * Constants::SPEED_OF_LIGHT;
    }

    double PppModel::computeIonoFreeCombination(const std::map<std::string, double>& measurements,
        System system,
        const std::string& signal1,
        const std::string& signal2) const {
        if (!measurements.count(signal1) || !measurements.count(signal2)) {
            log_manager_.getLogger()->warn("Missing measurements for signals {} or {} in iono-free combination", signal1, signal2);
            return 0.0;
        }

        double freq1 = getFrequency(signal1, system, "");
        double freq2 = getFrequency(signal2, system, "");
        if (freq1 <= 0.0 || freq2 <= 0.0 || std::abs(freq1 - freq2) < 1e-6) {
            log_manager_.getLogger()->warn("Invalid or identical frequencies for {} ({:.3f} MHz) and {} ({:.3f} MHz)",
                signal1, freq1 / 1e6, signal2, freq2 / 1e6);
            return 0.0;
        }

        double f1_sq = freq1 * freq1, f2_sq = freq2 * freq2;
        double c1 = f1_sq / (f1_sq - f2_sq);
        double c2 = -f2_sq / (f1_sq - f2_sq);
        if (!std::isfinite(c1) || !std::isfinite(c2)) {
            log_manager_.getLogger()->warn("Non-finite coefficients for iono-free combination: c1={:.3f}, c2={:.3f}", c1, c2);
            return 0.0;
        }

        double combination = c1 * measurements.at(signal1) + c2 * measurements.at(signal2);
        if (!std::isfinite(combination)) {
            log_manager_.getLogger()->warn("Non-finite iono-free combination for signals {} and {}: {:.3f}",
                signal1, signal2, combination);
            return 0.0;
        }

        log_manager_.getLogger()->trace("Computed iono-free combination for signals {} and {}: value={:.3f} m, "
            "freqs=[{:.3f}, {:.3f}] MHz, coeffs=[{:.3f}, {:.3f}]",
            signal1, signal2, combination, freq1 / 1e6, freq2 / 1e6, c1, c2);
        return combination;
    }

    double PppModel::computeTripleIonoFreeCombination(const std::map<std::string, double>& measurements,
        System system,
        const std::string& signal1,
        const std::string& signal2,
        const std::string& signal3) const {
        if (!measurements.count(signal1) || !measurements.count(signal2) || !measurements.count(signal3)) {
            log_manager_.getLogger()->warn("Missing measurements for signals {}, {}, or {} in triple-frequency iono-free combination",
                signal1, signal2, signal3);
            return 0.0;
        }

        double freq1 = getFrequency(signal1, system, "");
        double freq2 = getFrequency(signal2, system, "");
        double freq3 = getFrequency(signal3, system, "");
        if (freq1 <= 0.0 || freq2 <= 0.0 || freq3 <= 0.0 ||
            std::abs(freq1 - freq2) < 1e-6 || std::abs(freq2 - freq3) < 1e-6 || std::abs(freq1 - freq3) < 1e-6) {
            log_manager_.getLogger()->warn("Invalid or non-distinct frequencies for {} ({:.3f} MHz), {} ({:.3f} MHz), {} ({:.3f} MHz)",
                signal1, freq1 / 1e6, signal2, freq2 / 1e6, signal3, freq3 / 1e6);
            return 0.0;
        }

        double f1_sq = freq1 * freq1, f2_sq = freq2 * freq2, f3_sq = freq3 * freq3;
        double denom = f1_sq * f2_sq * (f1_sq - f2_sq) + f2_sq * f3_sq * (f2_sq - f3_sq) +
            f3_sq * f1_sq * (f3_sq - f1_sq);
        if (std::abs(denom) < 1e-6) {
            log_manager_.getLogger()->warn("Near-zero denominator in triple-frequency iono-free combination: {:.3e}", denom);
            return 0.0;
        }
        double c1 = (f2_sq * f3_sq * (f2_sq - f3_sq)) / denom;
        double c2 = (f3_sq * f1_sq * (f3_sq - f1_sq)) / denom;
        double c3 = (f1_sq * f2_sq * (f1_sq - f2_sq)) / denom;
        if (!std::isfinite(c1) || !std::isfinite(c2) || !std::isfinite(c3)) {
            log_manager_.getLogger()->warn("Non-finite coefficients for triple-frequency iono-free combination: c1={:.3f}, c2={:.3f}, c3={:.3f}",
                c1, c2, c3);
            return 0.0;
        }

        double combination = c1 * measurements.at(signal1) + c2 * measurements.at(signal2) + c3 * measurements.at(signal3);
        if (!std::isfinite(combination)) {
            log_manager_.getLogger()->warn("Non-finite triple-frequency iono-free combination for signals {}, {}, {}: {:.3f}",
                signal1, signal2, signal3, combination);
            return 0.0;
        }

        log_manager_.getLogger()->trace("Computed triple-frequency iono-free combination for signals {}, {}, {}: value={:.3f} m, "
            "freqs=[{:.3f}, {:.3f}, {:.3f}] MHz, coeffs=[{:.3f}, {:.3f}, {:.3f}]",
            signal1, signal2, signal3, combination, freq1 / 1e6, freq2 / 1e6, freq3 / 1e6, c1, c2, c3);
        return combination;
    }

    double PppModel::getSatelliteClockCorrection(const std::string& satellite, const GnssTime& time) const {
        // Validate inputs
        if (satellite.empty()) {
            log_manager_.getLogger()->warn("Empty satellite ID for clock correction at TOW {:.3f}", time.tow);
            return 0.0;
        }
        if (time.tow < 0.0 || time.week < 0) {
            log_manager_.getLogger()->warn("Invalid time for clock correction for satellite {}: week {}, TOW {:.3f}",
                satellite, time.week, time.tow);
            return 0.0;
        }
        if (clocks_.empty()) {
            log_manager_.getLogger()->warn("No clock data available for satellite {} at TOW {:.3f}", satellite, time.tow);
            return 0.0;
        }

        // Interpolate clock correction
        double clock_corr = interpolateClock(satellite, time, clocks_);
        std::string system_str = satellite.substr(0, 1) == "G" ? "GPS" : satellite.substr(0, 1) == "E" ? "Galileo" : "Unknown";
        if (std::isfinite(clock_corr) && std::abs(clock_corr) < 1e-3) { // Max 1 ms
            log_manager_.getLogger()->trace("Interpolated clock correction for {} ({}) at TOW {:.3f}: {:.3e} s",
                satellite, system_str, time.tow, clock_corr);
            return clock_corr;
        }

        log_manager_.getLogger()->warn("Invalid clock correction for {} ({}) at TOW {:.3f}: {:.3e} s",
            satellite, system_str, time.tow, clock_corr);
        return 0.0;
    }

    double PppModel::interpolateClock(const std::string& satellite, const GnssTime& time,
        const std::vector<ClockData>& clocks) const {
        // Collect candidates within 3600-second window
        std::vector<ClockData> candidates;
        for (const auto& clock : clocks) {
            if (clock.satellite == satellite && std::abs(clock.time.tow - time.tow) < 3600.0 &&
                std::isfinite(clock.clock_bias)) {
                candidates.push_back(clock);
            }
        }

        // Handle insufficient data
        if (candidates.empty()) {
            return 0.0;
        }
        if (candidates.size() == 1) {
            return candidates[0].clock_bias;
        }

        // Sort by time proximity
        std::sort(candidates.begin(), candidates.end(),
            [&time](const ClockData& a, const ClockData& b) {
                return std::abs(a.time.tow - time.tow) < std::abs(b.time.tow - time.tow);
            });
        candidates.resize(2);

        // Linear interpolation
        double t = time.tow;
        double t0 = candidates[0].time.tow;
        double t1 = candidates[1].time.tow;
        double c0 = candidates[0].clock_bias;
        double c1 = candidates[1].clock_bias;

        if (std::abs(t1 - t0) < 1e-3) { // Avoid division by near-zero
            log_manager_.getLogger()->debug("Time difference too small for clock interpolation for {} at TOW {:.3f}: {:.3f} s",
                satellite, time.tow, t1 - t0);
            return c0;
        }

        double frac = (t - t0) / (t1 - t0);
        double clock_corr = c0 + frac * (c1 - c0);
        if (!std::isfinite(clock_corr)) {
            log_manager_.getLogger()->warn("Non-finite clock correction for {} at TOW {:.3f}: {:.3e} s",
                satellite, time.tow, clock_corr);
            return 0.0;
        }

        return clock_corr;
    }

    double PppModel::computeRelativisticCorrection(const std::string& satellite, const Eigen::Vector3d& sat_pos,
        const Eigen::Vector3d& sat_vel) const {
        constexpr double C_SQUARED = Constants::SPEED_OF_LIGHT * Constants::SPEED_OF_LIGHT;
        constexpr double GM = Constants::GRAVITATIONAL_CONSTANT; // GM = G * M_Earth
        constexpr double MAX_CORRECTION = 1e-7; // Max 100 ns
        constexpr double MIN_POSITION_NORM = 1.8e7; // ~20,000 km
        constexpr double MAX_POSITION_NORM = 3.2e7; // ~32,000 km
        constexpr double MAX_VELOCITY_NORM = 1e4; // ~10 km/s

        // Validate inputs
        std::string system_str = satellite.substr(0, 1) == "G" ? "GPS" : satellite.substr(0, 1) == "E" ? "Galileo" : "Unknown";
        if (!sat_pos.allFinite() || !sat_vel.allFinite()) {
            log_manager_.getLogger()->warn("Invalid relativistic inputs for {} ({}): sat_pos=({:.3f}, {:.3f}, {:.3f}), sat_vel=({:.3f}, {:.3f}, {:.3f})",
                satellite, system_str, sat_pos.x(), sat_pos.y(), sat_pos.z(), sat_vel.x(), sat_vel.y(), sat_vel.z());
            return 0.0;
        }
        double pos_norm = sat_pos.norm();
        double vel_norm = sat_vel.norm();
        if (pos_norm < MIN_POSITION_NORM || pos_norm > MAX_POSITION_NORM || vel_norm > MAX_VELOCITY_NORM) {
            log_manager_.getLogger()->warn("Unrealistic relativistic input norms for {} ({}): pos_norm={:.3e} m, vel_norm={:.3e} m/s",
                satellite, system_str, pos_norm, vel_norm);
            return 0.0;
        }

        // Compute special relativistic correction: -2 * (r · v) / c²
        double r_dot_v = sat_pos.dot(sat_vel);
        double special_correction = -2.0 * r_dot_v / C_SQUARED;

        // Compute Schwarzschild (gravitational) correction: -2GM/(c²r)
        double gravitational_correction = -2.0 * GM / (C_SQUARED * pos_norm);

        double correction = special_correction + gravitational_correction;
        if (!std::isfinite(correction) || std::abs(correction) > MAX_CORRECTION) {
            log_manager_.getLogger()->warn("Excessive relativistic correction for {} ({}): {:.3e} s, special={:.3e}, gravitational={:.3e}, r_dot_v={:.3e}",
                satellite, system_str, correction, special_correction, gravitational_correction, r_dot_v);
            return 0.0;
        }

        log_manager_.getLogger()->trace("Relativistic correction for {} ({}): {:.3e} s (special={:.3e}, gravitational={:.3e}), r_dot_v={:.3e}",
            satellite, system_str, correction, special_correction, gravitational_correction, r_dot_v);
        return correction;
    }

    Eigen::Vector3d PppModel::getSatellitePosition(const std::string& satellite, const GnssTime& time) const {
        constexpr int order = 4; // Interpolation order for position
        constexpr double MIN_POSITION_NORM = 1.8e7; // Typical GNSS orbit ~20,000 km
        constexpr double MAX_POSITION_NORM = 3.2e7;
        constexpr double MIN_TIME_DIFF = 1.0; // Minimum time difference between points (s)
        constexpr double MAX_TIME_WINDOW = 3600.0; // Maximum time window for interpolation (s)

        // Validate inputs
        if (satellite.empty()) {
            log_manager_.getLogger()->warn("Empty satellite ID for position interpolation at TOW {:.3f}", time.tow);
            return Eigen::Vector3d::Zero();
        }
        if (orbits_.empty()) {
            log_manager_.getLogger()->warn("No orbit data available for {} at TOW {:.3f}", satellite, time.tow);
            return Eigen::Vector3d::Zero();
        }

        // Find candidate orbit data
        std::vector<OrbitData> candidates;
        std::string system_str = satellite.substr(0, 1) == "G" ? "GPS" : satellite.substr(0, 1) == "E" ? "Galileo" : "Unknown";
        for (const auto& data : orbits_) {
            if (data.satellite == satellite && data.position_valid) {
                double time_diff = std::abs(data.time.tow - time.tow);
                if (time_diff < MAX_TIME_WINDOW && data.position.allFinite() &&
                    data.position.norm() >= MIN_POSITION_NORM && data.position.norm() <= MAX_POSITION_NORM) {
                    candidates.push_back(data);
                }
            }
        }

        // Check if sufficient data points
        if (candidates.size() < order) {
            log_manager_.getLogger()->debug("Insufficient orbit data for {} ({}) at TOW {:.3f}: found {} points, need {}",
                satellite, system_str, time.tow, candidates.size(), order);
            if (!candidates.empty()) {
                auto it = std::min_element(candidates.begin(), candidates.end(),
                    [&time](const OrbitData& a, const OrbitData& b) {
                        return std::abs(a.time.tow - time.tow) < std::abs(b.time.tow - time.tow);
                    });
                log_manager_.getLogger()->trace("Using nearest position for {} ({}) at TOW {:.3f}: ({:.3f}, {:.3f}, {:.3f}) m, norm: {:.3e}",
                    satellite, system_str, it->time.tow, it->position.x(), it->position.y(), it->position.z(), it->position.norm());
                last_interpolated_velocity_.erase(satellite);
                return it->position;
            }
            log_manager_.getLogger()->warn("No valid orbit data for {} ({}) at TOW {:.3f}", satellite, system_str, time.tow);
            last_interpolated_velocity_.erase(satellite);
            return Eigen::Vector3d::Zero();
        }

        // Sort candidates by time proximity
        std::sort(candidates.begin(), candidates.end(),
            [&time](const OrbitData& a, const OrbitData& b) {
                return std::abs(a.time.tow - time.tow) < std::abs(b.time.tow - time.tow);
            });
        candidates.resize(order);

        // Check for sufficient time separation
        for (size_t i = 0; i < candidates.size(); ++i) {
            for (size_t j = i + 1; j < candidates.size(); ++j) {
                if (std::abs(candidates[i].time.tow - candidates[j].time.tow) < MIN_TIME_DIFF) {
                    log_manager_.getLogger()->warn("Time difference too small for {} ({}) at TOW {:.3f}: {:.3f} s between epochs",
                        satellite, system_str, time.tow, std::abs(candidates[i].time.tow - candidates[j].time.tow));
                    last_interpolated_velocity_.erase(satellite);
                    return candidates[0].position;
                }
            }
        }

        // Lagrange interpolation for position
        Eigen::Vector3d interpolated_pos(0.0, 0.0, 0.0);
        double t = time.tow;
        for (size_t i = 0; i < order; ++i) {
            double li = 1.0;
            for (size_t j = 0; j < order; ++j) {
                if (i != j) {
                    li *= (t - candidates[j].time.tow) / (candidates[i].time.tow - candidates[j].time.tow);
                }
            }
            interpolated_pos += li * candidates[i].position;
        }

        // Validate interpolated position
        if (!interpolated_pos.allFinite() ||
            interpolated_pos.norm() < MIN_POSITION_NORM || interpolated_pos.norm() > MAX_POSITION_NORM) {
            log_manager_.getLogger()->warn("Invalid interpolated position for {} ({}) at TOW {:.3f}: ({:.3e}, {:.3e}, {:.3e}) m, norm: {:.3e}",
                satellite, system_str, time.tow, interpolated_pos.x(), interpolated_pos.y(), interpolated_pos.z(), interpolated_pos.norm());
            auto it = std::min_element(candidates.begin(), candidates.end(),
                [&time](const OrbitData& a, const OrbitData& b) {
                    return std::abs(a.time.tow - time.tow) < std::abs(b.time.tow - time.tow);
                });
            last_interpolated_velocity_.erase(satellite);
            return it->position;
        }

        // Log successful interpolation
        log_manager_.getLogger()->trace("Interpolated position for {} ({}) at TOW {:.3f}: ({:.3f}, {:.3f}, {:.3f}) m, norm: {:.3e}",
            satellite, system_str, time.tow, interpolated_pos.x(), interpolated_pos.y(), interpolated_pos.z(), interpolated_pos.norm());
        return interpolated_pos;
    }

    double PppModel::computeElevation(const Eigen::Vector3d& rx_pos, const Eigen::Vector3d& sat_pos) const {
        Eigen::Vector3d los = (sat_pos - rx_pos).normalized();
        double elevation = std::asin(los(2)) * 180.0 / Constants::PI;
        log_manager_.getLogger()->trace("Computed elevation: {:.3f} degrees", elevation);
        return elevation;
    }

    double PppModel::computeAzimuth(const Eigen::Vector3d& rx_pos, const Eigen::Vector3d& sat_pos) const {
        // Compute line-of-sight vector in ECEF
        Eigen::Vector3d los = sat_pos - rx_pos;
        if (los.norm() < 1e-6) {
            log_manager_.getLogger()->warn("Invalid LOS for azimuth computation: rx_pos=({:.3f}, {:.3f}, {:.3f}), sat_pos=({:.3f}, {:.3f}, {:.3f})",
                rx_pos.x(), rx_pos.y(), rx_pos.z(), sat_pos.x(), sat_pos.y(), sat_pos.z());
            return 0.0;
        }

        // Project LOS onto local ENU (East, North, Up) plane
        double lat = std::asin(rx_pos.z() / rx_pos.norm());
        double lon = std::atan2(rx_pos.y(), rx_pos.x());
        Eigen::Matrix3d ecef_to_enu;
        ecef_to_enu << -std::sin(lon), std::cos(lon), 0.0,
            -std::sin(lat) * std::cos(lon), -std::sin(lat) * std::sin(lon), std::cos(lat),
            std::cos(lat)* std::cos(lon), std::cos(lat)* std::sin(lon), std::sin(lat);
        Eigen::Vector3d los_enu = ecef_to_enu * los;

        // Compute azimuth in degrees (0 to 360)
        double azimuth = std::atan2(los_enu.x(), los_enu.y()) * 180.0 / Constants::PI;
        if (azimuth < 0.0) {
            azimuth += 360.0;
        }
        if (!std::isfinite(azimuth) || azimuth < 0.0 || azimuth >= 360.0) {
            log_manager_.getLogger()->warn("Invalid azimuth: {:.3f} deg, rx_pos=({:.3f}, {:.3f}, {:.3f}), sat_pos=({:.3f}, {:.3f}, {:.3f})",
                azimuth, rx_pos.x(), rx_pos.y(), rx_pos.z(), sat_pos.x(), sat_pos.y(), sat_pos.z());
            return 0.0;
        }

        log_manager_.getLogger()->trace("Computed azimuth: {:.3f} degrees", azimuth);
        return azimuth;
    }

    double PppModel::getWavelength(System system, const std::string& freq_type) const {
        // Validate input
        if (freq_type.empty()) {
            log_manager_.getLogger()->warn("Empty frequency type for wavelength computation, system={}",
                MeasurementModel::getSystemString(system));
            return 0.0;
        }

        // Get frequency, passing empty satellite ID as specific satellite not needed here
        double freq = getFrequency(freq_type, system, "");
        if (freq <= 0.0 || !std::isfinite(freq)) {
            log_manager_.getLogger()->warn("Invalid frequency for type {} on system {}: {:.3f} MHz",
                freq_type, MeasurementModel::getSystemString(system), freq / 1e6);
            return 0.0;
        }

        // Compute wavelength (meters) = speed of light / frequency
        double wavelength = Constants::SPEED_OF_LIGHT / freq;
        if (!std::isfinite(wavelength) || wavelength <= 0.0) {
            log_manager_.getLogger()->warn("Invalid wavelength for type {} on system {}: {:.3f} m",
                freq_type, MeasurementModel::getSystemString(system), wavelength);
            return 0.0;
        }

        log_manager_.getLogger()->trace("Computed wavelength for type {} on system {}: {:.3f} m (freq={:.3f} MHz)",
            freq_type, MeasurementModel::getSystemString(system), wavelength, freq / 1e6);
        return wavelength;
    }

    double PppModel::getAntennaCorrection(const std::string& satellite,
        const std::string& freq_type,
        System system,
        const Eigen::Vector3d& rx_pos,
        const Eigen::Vector3d& sat_pos,
        double elevation,
        double azimuth,
        const std::string& rx_antenna_type,
        const GnssTime& time) const {
        // Initialize correction
        double correction = 0.0;
        bool is_phase = freq_type[0] == 'L';
        double wavelength = is_phase ? getWavelength(system, freq_type) : 0.0;

        // Validate inputs
        if (!rx_pos.allFinite() || !sat_pos.allFinite() || !std::isfinite(elevation) || !std::isfinite(azimuth)) {
            log_manager_.getLogger()->warn("Invalid inputs for antenna correction: satellite={}, freq_type={}, "
                "rx_pos=({}, {}, {}), sat_pos=({}, {}, {}), elevation={:.3f}, azimuth={:.3f}",
                satellite, freq_type, rx_pos.x(), rx_pos.y(), rx_pos.z(),
                sat_pos.x(), sat_pos.y(), sat_pos.z(), elevation, azimuth);
            return 0.0;
        }
        if (elevation < 0.0 || elevation > 90.0 || azimuth < 0.0 || azimuth >= 360.0) {
            log_manager_.getLogger()->warn("Out-of-range elevation ({:.3f}) or azimuth ({:.3f}) for satellite {}, freq_type={}",
                elevation, azimuth, satellite, freq_type);
            return 0.0;
        }

        // Map frequency type to ATX frequency code
        std::string atx_freq;
        if (system == System::GPS) {
            if (freq_type.find("C1C") == 0 || freq_type.find("L1C") == 0 || freq_type.find("C1W") == 0 || freq_type.find("L1W") == 0 ||
                freq_type.find("C1S") == 0 || freq_type.find("L1S") == 0 || freq_type.find("C1L") == 0 || freq_type.find("L1L") == 0) {
                atx_freq = "G01";
            }
            else if (freq_type.find("C2W") == 0 || freq_type.find("L2W") == 0 || freq_type.find("C2C") == 0 || freq_type.find("L2C") == 0 ||
                freq_type.find("C2S") == 0 || freq_type.find("L2S") == 0 || freq_type.find("C2L") == 0 || freq_type.find("L2L") == 0) {
                atx_freq = "G02";
            }
            else if (freq_type.find("C5Q") == 0 || freq_type.find("L5Q") == 0 || freq_type.find("C5I") == 0 || freq_type.find("L5I") == 0) {
                atx_freq = "G05";
            }
        }
        else if (system == System::Galileo) {
            if (freq_type.find("C1C") == 0 || freq_type.find("L1C") == 0 || freq_type.find("C1B") == 0 || freq_type.find("L1B") == 0) {
                atx_freq = "E01";
            }
            else if (freq_type.find("C5Q") == 0 || freq_type.find("L5Q") == 0 || freq_type.find("C5I") == 0 || freq_type.find("L5I") == 0) {
                atx_freq = "E05";
            }
            else if (freq_type.find("C7Q") == 0 || freq_type.find("L7Q") == 0 || freq_type.find("C7I") == 0 || freq_type.find("L7I") == 0) {
                atx_freq = "E07";
            }
            else if (freq_type.find("C6C") == 0 || freq_type.find("L6C") == 0 || freq_type.find("C6B") == 0 || freq_type.find("L6B") == 0) {
                atx_freq = "E06";
            }
            else if (freq_type.find("C8Q") == 0 || freq_type.find("L8Q") == 0 || freq_type.find("C8I") == 0 || freq_type.find("L8I") == 0) {
                atx_freq = "E08";
            }
        }
        else if (system == System::GLONASS) {
            const auto& glonass_slots = rinex_parser_.getGlonassFreqSlots();
            auto slot_it = glonass_slots.find(satellite);
            int channel = (slot_it != glonass_slots.end()) ? slot_it->second : 0;
            if (channel < -7 || channel > 6) {
                log_manager_.getLogger()->warn("Invalid GLONASS channel {} for satellite {}, freq_type={}", channel, satellite, freq_type);
                return 0.0;
            }
            std::string channel_suffix = channel >= 0 ? "+" + std::to_string(channel) : std::to_string(channel);
            if (freq_type.find("C1C") == 0 || freq_type.find("L1C") == 0 || freq_type.find("C1P") == 0 || freq_type.find("L1P") == 0) {
                atx_freq = "R01" + channel_suffix; // e.g., R01-4 for channel -4
            }
            else if (freq_type.find("C2C") == 0 || freq_type.find("L2C") == 0 || freq_type.find("C2P") == 0 || freq_type.find("L2P") == 0) {
                atx_freq = "R02" + channel_suffix; // e.g., R02-4
            }
            else if (freq_type.find("C3Q") == 0 || freq_type.find("L3Q") == 0 || freq_type.find("C3I") == 0 || freq_type.find("L3I") == 0) {
                atx_freq = "R03" + channel_suffix; // e.g., R03-4
            }
        }
        else if (system == System::BeiDou) {
            if (freq_type.find("C2I") == 0 || freq_type.find("L2I") == 0 || freq_type.find("C2Q") == 0 || freq_type.find("L2Q") == 0) {
                atx_freq = "C02";
            }
            else if (freq_type.find("C7I") == 0 || freq_type.find("L7I") == 0 || freq_type.find("C7Q") == 0 || freq_type.find("L7Q") == 0) {
                atx_freq = "C07";
            }
            else if (freq_type.find("C6I") == 0 || freq_type.find("L6I") == 0 || freq_type.find("C6Q") == 0 || freq_type.find("L6Q") == 0) {
                atx_freq = "C06";
            }
            else if (freq_type.find("C1D") == 0 || freq_type.find("L1D") == 0 || freq_type.find("C1P") == 0 || freq_type.find("L1P") == 0) {
                atx_freq = "C01";
            }
        }
        else if (system == System::QZSS) {
            if (freq_type.find("C1C") == 0 || freq_type.find("L1C") == 0 || freq_type.find("C1S") == 0 || freq_type.find("L1S") == 0) {
                atx_freq = "J01";
            }
            else if (freq_type.find("C2L") == 0 || freq_type.find("L2L") == 0 || freq_type.find("C2S") == 0 || freq_type.find("L2S") == 0) {
                atx_freq = "J02";
            }
            else if (freq_type.find("C5Q") == 0 || freq_type.find("L5Q") == 0 || freq_type.find("C5I") == 0 || freq_type.find("L5I") == 0) {
                atx_freq = "J05";
            }
            else if (freq_type.find("C6L") == 0 || freq_type.find("L6L") == 0 || freq_type.find("C6E") == 0 || freq_type.find("L6E") == 0) {
                atx_freq = "J06";
            }
        }
        else if (system == System::SBAS) {
            if (freq_type.find("C1C") == 0 || freq_type.find("L1C") == 0) {
                atx_freq = "S01";
            }
            else if (freq_type.find("C5I") == 0 || freq_type.find("L5I") == 0 || freq_type.find("C5Q") == 0 || freq_type.find("L5Q") == 0) {
                atx_freq = "S05";
            }
        }

        if (atx_freq.empty()) {
            log_manager_.getLogger()->warn("No ATX frequency mapping for satellite {}, freq_type={}, system={}",
                satellite, freq_type, static_cast<int>(system));
            return 0.0;
        }

        // Compute line-of-sight vector
        Eigen::Vector3d los = (sat_pos - rx_pos);
        double los_norm = los.norm();
        if (los_norm == 0.0) {
            log_manager_.getLogger()->warn("Invalid LOS for satellite {}, freq_type={} at TOW {:.3f}", satellite, freq_type, time.tow);
            return 0.0;
        }
        los /= los_norm;

        // Satellite antenna correction
        double sat_pco_corr = 0.0, sat_pcv_corr = 0.0;
        std::string sat_antenna_type = satellite;
        auto sat_it = std::find_if(antennas_.begin(), antennas_.end(),
            [&sat_antenna_type](const AntennaData& a) {
                return a.type.find(sat_antenna_type) != std::string::npos;
            });
        if (sat_it != antennas_.end() && sat_it->pco.count(atx_freq)) {
            // PCO correction: Project onto line-of-sight (mm to m)
            Eigen::Vector3d pco = sat_it->pco.at(atx_freq) * 1e-3;
            if (pco.allFinite()) {
                sat_pco_corr = pco.dot(los);
                correction += sat_pco_corr;
            }
            else {
                log_manager_.getLogger()->warn("Invalid satellite PCO for {} {} atx_freq={} at TOW {:.3f}",
                    satellite, sat_antenna_type, atx_freq, time.tow);
            }

            // PCV correction: Interpolate based on zenith and azimuth
            if (sat_it->pcv.count(atx_freq) && !sat_it->pcv.at(atx_freq).empty()) {
                const auto& pcv_table = sat_it->pcv.at(atx_freq);
                double zenith = 90.0 - elevation;
                double dazi = sat_it->dazi > 0.0 ? sat_it->dazi : 5.0; // Use ATX DAZI or default
                double dzen = sat_it->dzen > 0.0 ? sat_it->dzen : 5.0; // Use ATX DZEN or default
                double pcv = interpolatePcv(pcv_table, zenith, azimuth, dazi, dzen) * 1e-3; // mm to m
                if (std::isfinite(pcv)) {
                    sat_pcv_corr = pcv;
                    correction += sat_pcv_corr;
                }
                else {
                    log_manager_.getLogger()->warn("Invalid satellite PCV for {} {} atx_freq={} at TOW {:.3f}, zenith={:.3f}, azimuth={:.3f}, dazi={:.3f}, dzen={:.3f}",
                        satellite, sat_antenna_type, atx_freq, time.tow, zenith, azimuth, dazi, dzen);
                }
            }
        }

        // Receiver antenna correction
        double rx_pco_corr = 0.0, rx_pcv_corr = 0.0;
        auto rx_it = std::find_if(antennas_.begin(), antennas_.end(),
            [&rx_antenna_type](const AntennaData& a) {
                return a.type.find(rx_antenna_type) != std::string::npos;
            });
        if (rx_it != antennas_.end() && rx_it->pco.count(atx_freq)) {
            // PCO correction: Project onto line-of-sight (mm to m)
            Eigen::Vector3d pco = rx_it->pco.at(atx_freq) * 1e-3;
            if (pco.allFinite()) {
                rx_pco_corr = pco.dot(los);
                correction -= rx_pco_corr;
            }
            else {
                log_manager_.getLogger()->warn("Invalid receiver PCO for {} {} atx_freq={} at TOW {:.3f}",
                    satellite, rx_antenna_type, atx_freq, time.tow);
            }

            // PCV correction: Interpolate based on zenith and azimuth
            if (rx_it->pcv.count(atx_freq) && !rx_it->pcv.at(atx_freq).empty()) {
                const auto& pcv_table = rx_it->pcv.at(atx_freq);
                double zenith = 90.0 - elevation;
                double dazi = rx_it->dazi > 0.0 ? rx_it->dazi : 5.0; // Use ATX DAZI or default
                double dzen = rx_it->dzen > 0.0 ? rx_it->dzen : 5.0; // Use ATX DZEN or default
                double pcv = interpolatePcv(pcv_table, zenith, azimuth, dazi, dzen) * 1e-3; // mm to m
                if (std::isfinite(pcv)) {
                    rx_pcv_corr = pcv;
                    correction -= rx_pcv_corr;
                }
                else {
                    log_manager_.getLogger()->warn("Invalid receiver PCV for {} {} atx_freq={} at TOW {:.3f}, zenith={:.3f}, azimuth={:.3f}, dazi={:.3f}, dzen={:.3f}",
                        satellite, rx_antenna_type, atx_freq, time.tow, zenith, azimuth, dazi, dzen);
                }
            }
        }

        // Phase wind-up correction (for carrier phase only)
        double wind_up_corr = 0.0;
        if (is_phase && wavelength > 0.0) {
            double wind_up = computePhaseWindUp(satellite, system, rx_pos, sat_pos, wavelength, time);
            if (std::isfinite(wind_up)) {
                wind_up_corr = wind_up;
                correction += wind_up_corr;
            }
            else {
                log_manager_.getLogger()->warn("Invalid phase wind-up for {} {} at TOW {:.3f}", satellite, freq_type, time.tow);
            }
        }

        // Log detailed correction components
        if (std::isfinite(correction) && correction != 0.0) {
            log_manager_.getLogger()->trace("Antenna correction for {} {}: total={:.6f} m (sat_pco={:.6f}, sat_pcv={:.6f}, "
                "rx_pco={:.6f}, rx_pcv={:.6f}, wind_up={:.6f}) at TOW {:.3f}, dazi={:.3f}, dzen={:.3f}",
                satellite, freq_type, correction, sat_pco_corr, sat_pcv_corr,
                -rx_pco_corr, -rx_pcv_corr, wind_up_corr, time.tow,
                sat_it != antennas_.end() ? sat_it->dazi : 5.0, sat_it != antennas_.end() ? sat_it->dzen : 5.0);
        }
        else {
            log_manager_.getLogger()->trace("No antenna correction for {} {} at TOW {:.3f}", satellite, freq_type, time.tow);
        }
        return correction;
    }

    double PppModel::interpolatePcv(const std::vector<std::vector<double>>& pcv_table,
        double zenith,
        double azimuth,
        double dazi,
        double dzen) const {
        if (pcv_table.empty() || pcv_table[0].empty()) {
            return 0.0;
        }

        // Zenith interpolation
        size_t zen_idx = static_cast<size_t>(zenith / dzen);
        double zen_frac = (zenith - zen_idx * dzen) / dzen;
        if (zen_idx >= pcv_table.size()) {
            zen_idx = pcv_table.size() - 1;
            zen_frac = 0.0;
        }

        // Azimuth interpolation
        size_t az_idx = static_cast<size_t>(azimuth / dazi);
        double az_frac = (azimuth - az_idx * dazi) / dazi;
        size_t num_az = pcv_table[0].size();
        if (num_az <= 1) {
            // No azimuth dependence
            double pcv = pcv_table[zen_idx][0];
            if (zen_idx + 1 < pcv_table.size()) {
                double pcv_next = pcv_table[zen_idx + 1][0];
                pcv = pcv + zen_frac * (pcv_next - pcv);
            }
            return pcv;
        }

        if (az_idx >= num_az) {
            az_idx = num_az - 1;
            az_frac = 0.0;
        }
        size_t az_idx_next = (az_idx + 1) % num_az;
        double az_frac_next = 1.0 - az_frac;

        // Bilinear interpolation
        double pcv00 = pcv_table[zen_idx][az_idx];
        double pcv10 = zen_idx + 1 < pcv_table.size() ? pcv_table[zen_idx + 1][az_idx] : pcv00;
        double pcv01 = pcv_table[zen_idx][az_idx_next];
        double pcv11 = zen_idx + 1 < pcv_table.size() ? pcv_table[zen_idx + 1][az_idx_next] : pcv01;

        double pcv = pcv00 * (1.0 - zen_frac) * az_frac_next +
            pcv10 * zen_frac * az_frac_next +
            pcv01 * (1.0 - zen_frac) * az_frac +
            pcv11 * zen_frac * az_frac;

        return pcv;
    }

    double PppModel::computePhaseWindUp(const std::string& satellite,
        System system,
        const Eigen::Vector3d& rx_pos,
        const Eigen::Vector3d& sat_pos,
        double wavelength,
        const GnssTime& time) const {

        // Phase wind-up correction for carrier phase
        Eigen::Vector3d los = (sat_pos - rx_pos).normalized();
        Eigen::Vector3d k = los;

        // Satellite antenna orientation
        Eigen::Vector3d d_s, y_s;
        Eigen::Vector3d sat_vel(0.0, 0.0, 0.0);
        auto orbit_it = std::find_if(orbits_.begin(), orbits_.end(),
            [&satellite](const OrbitData& o) { return o.satellite == satellite; });
        if (orbit_it != orbits_.end() && orbit_it->velocity_valid) {
            sat_vel = orbit_it->velocity;
        }
        computeSatelliteAttitude(system, sat_pos, sat_vel, d_s, y_s, time);

        // Receiver antenna orientation
        const auto& orientation = config_.getReceiverAntennaOrientation();
        double az = orientation.azimuth_deg * Constants::PI / 180.0;
        double el = orientation.elevation_deg * Constants::PI / 180.0;
        Eigen::Vector3d d_r(std::cos(el) * std::sin(az), std::cos(el) * std::cos(az), std::sin(el));
        // Override with RINEX antenna offsets if available
        Eigen::Vector3d hen = rinex_parser_.getAntennaDeltaHen();
        if (hen.norm() > 1e-6) {
            // Approximate orientation from H/E/N (assuming H is vertical)
            d_r = Eigen::Vector3d(hen.y(), hen.x(), hen.z()).normalized();
        }

        // Effective dipole vectors
        Eigen::Vector3d d_s_eff = d_s - k * (k.dot(d_s)) + k.cross(y_s);
        Eigen::Vector3d d_r_eff = d_r - k * (k.dot(d_r));
        if (d_s_eff.norm() < 1e-6 || d_r_eff.norm() < 1e-6) {
            log_manager_.getLogger()->trace("Phase wind-up for {}: 0 m (degenerate vectors)", satellite);
            return 0.0;
        }
        d_s_eff.normalize();
        d_r_eff.normalize();

        Eigen::Vector3d cross = d_s_eff.cross(d_r_eff);
        double dot = d_s_eff.dot(d_r_eff);
        if (dot > 1.0) dot = 1.0;
        if (dot < -1.0) dot = -1.0;
        double sign = k.dot(cross) >= 0.0 ? 1.0 : -1.0;
        double phi = sign * std::acos(dot) * wavelength / (2.0 * Constants::PI);

        log_manager_.getLogger()->trace("Phase wind-up for {}: {} m", satellite, phi);
        return phi;
    }

    double PppModel::getBiasCorrection(const std::string& satellite,
        const std::string& freq_type,
        const GnssTime& time) const {
        // Validate inputs
        if (satellite.empty() || freq_type.empty()) {
            log_manager_.getLogger()->warn("Invalid inputs for bias correction: satellite='{}', freq_type='{}' at TOW {:.3f}",
                satellite, freq_type, time.tow);
            return 0.0;
        }
        if (time.tow < 0.0 || time.week < 0) {
            log_manager_.getLogger()->warn("Invalid time for bias correction for satellite {}: week {}, TOW {:.3f}",
                satellite, time.week, time.tow);
            return 0.0;
        }

        // Find bias data
        auto it = std::find_if(biases_.begin(), biases_.end(),
            [&satellite, &freq_type, &time](const BiasData& b) {
                return b.satellite == satellite && b.signal == freq_type &&
                    b.start_time.tow <= time.tow && time.tow <= b.end_time.tow;
            });
        if (it != biases_.end()) {
            log_manager_.getLogger()->trace("Bias correction for {} ({}) {} at TOW {:.3f}: {:.3e} m",
                satellite, satellite.substr(0, 1) == "G" ? "GPS" : satellite.substr(0, 1) == "E" ? "Galileo" : "Unknown",
                freq_type, time.tow, it->bias);
            return it->bias;
        }

        log_manager_.getLogger()->trace("No bias correction for {} ({}) {} at TOW {:.3f}",
            satellite, satellite.substr(0, 1) == "G" ? "GPS" : satellite.substr(0, 1) == "E" ? "Galileo" : "Unknown",
            freq_type, time.tow);
        return 0.0;
    }

    Eigen::Vector3d PppModel::computePoleTideCorrection(const Eigen::Vector3d& rx_pos, const GnssTime& time) const {
        // Validate inputs
        if (!rx_pos.allFinite() || rx_pos.norm() < 1e3) {
            log_manager_.getLogger()->warn("Invalid receiver position for pole tide correction at TOW {:.3f}: rx_pos=({:.3f}, {:.3f}, {:.3f}), norm={:.3e}",
                time.tow, rx_pos.x(), rx_pos.y(), rx_pos.z(), rx_pos.norm());
            return Eigen::Vector3d::Zero();
        }
        if (time.tow < 0.0 || time.week < 0) {
            log_manager_.getLogger()->warn("Invalid time for pole tide correction: week {}, TOW {:.3f}",
                time.week, time.tow);
            return Eigen::Vector3d::Zero();
        }
        if (erps_.empty()) {
            log_manager_.getLogger()->warn("No ERP data available for pole tide correction at TOW {:.3f}", time.tow);
            return Eigen::Vector3d::Zero();
        }

        // Find closest ERP data
        auto it = std::min_element(erps_.begin(), erps_.end(),
            [&time](const ErpData& a, const ErpData& b) {
                return std::abs(a.time.tow - time.tow) < std::abs(b.time.tow - time.tow);
            });
        if (it == erps_.end() || !std::isfinite(it->xp) || !std::isfinite(it->yp)) {
            log_manager_.getLogger()->trace("No valid ERP data for pole tide correction at TOW {:.3f}", time.tow);
            return Eigen::Vector3d::Zero();
        }

        // Compute pole tide correction (simplified model, IERS 2010 conventions)
        double phi = std::asin(rx_pos.z() / rx_pos.norm()); // Geodetic latitude (radians)
        double lambda = std::atan2(rx_pos.y(), rx_pos.x()); // Geodetic longitude (radians)
        double xp = it->xp * 4.848136811e-6; // Convert arcsec to radians
        double yp = it->yp * 4.848136811e-6; // Convert arcsec to radians

        // Pole tide displacement in radial, north, east directions (meters)
        double R = rx_pos.norm();
        double radial = -0.033 * std::sin(2.0 * phi) * (xp * std::cos(lambda) + yp * std::sin(lambda));
        double north = -0.009 * std::cos(2.0 * phi) * (xp * std::sin(lambda) - yp * std::cos(lambda));
        double east = -0.009 * std::sin(phi) * (xp * std::cos(lambda) + yp * std::sin(lambda));

        // Convert to ECEF coordinates
        Eigen::Vector3d correction(
            (radial * std::cos(phi) * std::cos(lambda) - north * std::sin(phi) * std::cos(lambda) - east * std::sin(lambda)),
            (radial * std::cos(phi) * std::sin(lambda) - north * std::sin(phi) * std::sin(lambda) + east * std::cos(lambda)),
            (radial * std::sin(phi) + north * std::cos(phi))
        );

        if (!correction.allFinite() || correction.norm() > 0.1) { // Max 10 cm
            log_manager_.getLogger()->warn("Invalid pole tide correction at TOW {:.3f}: ({:.3e}, {:.3e}, {:.3e}) m, norm={:.3e}",
                time.tow, correction.x(), correction.y(), correction.z(), correction.norm());
            return Eigen::Vector3d::Zero();
        }

        log_manager_.getLogger()->trace("Pole tide correction at TOW {:.3f}: ({:.3e}, {:.3e}, {:.3e}) m, xp={:.3e} rad, yp={:.3e} rad",
            time.tow, correction.x(), correction.y(), correction.z(), xp, yp);
        return correction;
    }

    Eigen::Vector3d PppModel::computeOceanTideCorrection(const Eigen::Vector3d& rx_pos, const GnssTime& time) const {
        // Validate inputs
        if (!rx_pos.allFinite() || rx_pos.norm() < 1e3) {
            log_manager_.getLogger()->warn("Invalid receiver position for ocean tide correction at TOW {:.3f}: rx_pos=({:.3f}, {:.3f}, {:.3f}), norm={:.3e}",
                time.tow, rx_pos.x(), rx_pos.y(), rx_pos.z(), rx_pos.norm());
            return Eigen::Vector3d::Zero();
        }
        if (time.tow < 0.0 || time.week < 0) {
            log_manager_.getLogger()->warn("Invalid time for ocean tide correction: week {}, TOW {:.3f}",
                time.week, time.tow);
            return Eigen::Vector3d::Zero();
        }

        // Placeholder: Simplified ocean tide correction (requires external tidal model data)
        // Using a basic sinusoidal model based on position and time
        double phi = std::asin(rx_pos.z() / rx_pos.norm()); // Geodetic latitude (radians)
        double t = std::fmod(time.tow, 86400.0); // Seconds of day
        double amplitude = 0.01; // Typical ocean tide amplitude: 1 cm
        double period = 12.4206 * 3600.0; // M2 tidal constituent period (s)
        double radial = amplitude * std::cos(2.0 * Constants::PI * t / period) * std::cos(phi);
        double north = amplitude * std::sin(2.0 * Constants::PI * t / period) * std::sin(phi);
        double east = 0.0; // Simplified, no east component

        // Convert to ECEF coordinates
        double lambda = std::atan2(rx_pos.y(), rx_pos.x());
        Eigen::Vector3d correction(
            (radial * std::cos(phi) * std::cos(lambda) - north * std::sin(phi) * std::cos(lambda)),
            (radial * std::cos(phi) * std::sin(lambda) - north * std::sin(phi) * std::sin(lambda)),
            (radial * std::sin(phi) + north * std::cos(phi))
        );

        if (!correction.allFinite() || correction.norm() > 0.1) { // Max 10 cm
            log_manager_.getLogger()->warn("Invalid ocean tide correction at TOW {:.3f}: ({:.3e}, {:.3e}, {:.3e}) m, norm={:.3e}",
                time.tow, correction.x(), correction.y(), correction.z(), correction.norm());
            return Eigen::Vector3d::Zero();
        }

        log_manager_.getLogger()->trace("Ocean tide correction at TOW {:.3f}: ({:.3e}, {:.3e}, {:.3e}) m, amplitude={:.3e} m",
            time.tow, correction.x(), correction.y(), correction.z(), amplitude);
        return correction;
    }
    double PppModel::getFrequency(const std::string& freq_type, System system, const std::string& satellite) const {
        if (system == System::GPS) {
            if (freq_type.find("C1") != std::string::npos || freq_type.find("L1") != std::string::npos) {
                return Constants::GPS_L1_FREQ;
            }
            if (freq_type.find("C2") != std::string::npos || freq_type.find("L2") != std::string::npos) {
                return Constants::GPS_L2_FREQ;
            }
            if (freq_type.find("C5") != std::string::npos || freq_type.find("L5") != std::string::npos) {
                return Constants::GPS_L5_FREQ;
            }
        }
        else if (system == System::Galileo) {
            if (freq_type.find("C1") != std::string::npos || freq_type.find("L1") != std::string::npos) {
                return Constants::GAL_E1_FREQ;
            }
            if (freq_type.find("C5") != std::string::npos || freq_type.find("L5") != std::string::npos) {
                return Constants::GAL_E5A_FREQ;
            }
            if (freq_type.find("C7") != std::string::npos || freq_type.find("L7") != std::string::npos) {
                return Constants::GAL_E5B_FREQ;
            }
        }
        else if (system == System::GLONASS) {
            // Return reference frequency for rescaled phase observations
            if (freq_type.find("C1") != std::string::npos || freq_type.find("L1") != std::string::npos) {
                log_manager_.getLogger()->trace("Using GLONASS G1 reference frequency: {} MHz for type {}",
                    Constants::GLONASS_G1_BASE_FREQ / 1e6, freq_type);
                return Constants::GLONASS_G1_BASE_FREQ; // 1602 MHz
            }
            if (freq_type.find("C2") != std::string::npos || freq_type.find("L2") != std::string::npos) {
                log_manager_.getLogger()->trace("Using GLONASS G2 reference frequency: {} MHz for type {}",
                    Constants::GLONASS_G2_BASE_FREQ / 1e6, freq_type);
                return Constants::GLONASS_G2_BASE_FREQ; // 1246 MHz
            }
            log_manager_.getLogger()->warn("Invalid GLONASS frequency type: {}", freq_type);
            return 0.0;
        }
        else if (system == System::BeiDou) {
            if (freq_type.find("C2") != std::string::npos || freq_type.find("L2") != std::string::npos) {
                return Constants::BDS_B1_FREQ;
            }
            if (freq_type.find("C7") != std::string::npos || freq_type.find("L7") != std::string::npos) {
                return Constants::BDS_B2_FREQ;
            }
            if (freq_type.find("C6") != std::string::npos || freq_type.find("L6") != std::string::npos) {
                return Constants::BDS_B3_FREQ;
            }
        }
        else if (system == System::QZSS) {
            if (freq_type.find("C1") != std::string::npos || freq_type.find("L1") != std::string::npos) {
                return Constants::QZSS_L1_FREQ;
            }
            if (freq_type.find("C2") != std::string::npos || freq_type.find("L2") != std::string::npos) {
                return Constants::QZSS_L2_FREQ;
            }
            if (freq_type.find("C5") != std::string::npos || freq_type.find("L5") != std::string::npos) {
                return Constants::QZSS_L5_FREQ;
            }
        }
        else if (system == System::SBAS) {
            if (freq_type.find("C1") != std::string::npos || freq_type.find("L1") != std::string::npos) {
                return Constants::SBAS_L1_FREQ;
            }
            if (freq_type.find("C5") != std::string::npos || freq_type.find("L5") != std::string::npos) {
                return Constants::SBAS_L5_FREQ;
            }
        }

        log_manager_.getLogger()->warn("Unknown frequency for {} on system {}", freq_type, static_cast<int>(system));
        return 0.0;
    }

    double PppModel::getReferenceFrequency(const std::string& freq_type, System system) const {
        // Validate input
        if (freq_type.empty()) {
            log_manager_.getLogger()->warn("Empty frequency type for reference frequency, system={}",
                MeasurementModel::getSystemString(system));
            return 0.0;
        }

        if (system == System::GLONASS) {
            if (freq_type.find("C1") != std::string::npos || freq_type.find("L1") != std::string::npos) {
                return Constants::GLONASS_G1_BASE_FREQ; // Center frequency: 1602 MHz
            }
            if (freq_type.find("C2") != std::string::npos || freq_type.find("L2") != std::string::npos) {
                return Constants::GLONASS_G2_BASE_FREQ; // Center frequency: 1246 MHz
            }
            if (freq_type.find("C3") != std::string::npos || freq_type.find("L3") != std::string::npos) {
                return Constants::GLONASS_G3_BASE_FREQ; // Center frequency: 1202.025 MHz
            }
        }

        // For non-GLONASS systems, return actual frequency (no rescaling needed)
        double freq = getFrequency(freq_type, system, "");
        if (freq <= 0.0) {
            log_manager_.getLogger()->warn("Invalid reference frequency for type {} on system {}: {:.3f} MHz",
                freq_type, MeasurementModel::getSystemString(system), freq / 1e6);
        }
        return freq;
    }

    double PppModel::rescaleGlonassObservation(double observed_data, const std::string& freq_type,
        System system, const std::string& satellite) const {
        if (system != System::GLONASS) {
            return observed_data; // No rescaling for non-GLONASS systems
        }

        // Validate inputs
        if (freq_type.empty() || satellite.empty()) {
            log_manager_.getLogger()->warn("Invalid inputs for GLONASS rescaling: freq_type='{}', satellite='{}'",
                freq_type, satellite);
            return observed_data;
        }

        double actual_freq = getFrequency(freq_type, system, satellite);
        double ref_freq = getReferenceFrequency(freq_type, system);
        if (actual_freq <= 0.0 || ref_freq <= 0.0 || !std::isfinite(actual_freq) || !std::isfinite(ref_freq)) {
            log_manager_.getLogger()->warn("Cannot rescale GLONASS observation for type {}, satellite {}: actual_freq={:.3f} MHz, ref_freq={:.3f} MHz",
                freq_type, satellite, actual_freq / 1e6, ref_freq / 1e6);
            return observed_data;
        }

        // Rescale phase: L_new = L_old * (f_actual / f_ref)
        double scaled_data = observed_data * (actual_freq / ref_freq);
        log_manager_.getLogger()->trace("Rescaled GLONASS observation for type {}, satellite {}: from {:.3f} to {:.3f} cycles, actual_freq={:.3f} MHz, ref_freq={:.3f} MHz",
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
            // GLONASS: Handle channel priorities
            for (const auto& signal : it->second) {
                // Check for standard observation types (e.g., "C1C", "L1C")
                if (measurements.count(signal)) {
                    selected_types.push_back(signal);
                }
                // Check for channel-specific types (e.g., "C1C_k0", "C1C_m1")
                for (int channel = -7; channel <= 7; ++channel) { // Typical GLONASS channel range
                    std::string type = signal + "_k" + (channel >= 0 ? std::to_string(channel) : "m" + std::to_string(-channel));
                    if (measurements.count(type)) {
                        selected_types.push_back(type);
                    }
                }
            }
        }
        else {
            // Other systems: Use priority list directly
            for (const auto& type : it->second) {
                if (measurements.count(type)) {
                    selected_types.push_back(type);
                }
            }
        }
        return selected_types;
    }

    double PppModel::computeSagnacCorrection(const Eigen::Vector3d& rx_pos,
        const Eigen::Vector3d& sat_pos) const {
        // Sagnac effect: Correction for Earth's rotation during signal propagation
        // Formula: (ω_E / c) * (x_s * y_r - y_s * x_r), where ω_E is Earth's angular velocity, c is speed of light
        constexpr double EARTH_ANGULAR_VELOCITY = 7.2921151467e-5; // rad/s
        constexpr double C = Constants::SPEED_OF_LIGHT; // m/s
        constexpr double MAX_SAGNAC_CORRECTION = 50.0; // Max expected correction in meters
        constexpr double MIN_RX_NORM = 6e6; // Earth radius ~6.3e6 m
        constexpr double MAX_RX_NORM = 7e6;
        constexpr double MIN_SAT_NORM = 2e7; // Typical GNSS orbit ~2e7 m
        constexpr double MAX_SAT_NORM = 3e7;

        // Validate inputs
        if (!rx_pos.allFinite() || !sat_pos.allFinite()) {
            log_manager_.getLogger()->warn("Invalid Sagnac inputs: rx_pos=({}, {}, {}), sat_pos=({}, {}, {})",
                rx_pos.x(), rx_pos.y(), rx_pos.z(),
                sat_pos.x(), sat_pos.y(), sat_pos.z());
            return 0.0;
        }
        double rx_norm = rx_pos.norm();
        double sat_norm = sat_pos.norm();
        if (rx_norm < MIN_RX_NORM || rx_norm > MAX_RX_NORM || sat_norm < MIN_SAT_NORM || sat_norm > MAX_SAT_NORM) {
            log_manager_.getLogger()->warn("Unrealistic Sagnac input norms: rx_norm={:.3e} m, sat_norm={:.3e} m, "
                "rx_pos=({}, {}, {}), sat_pos=({}, {}, {})",
                rx_norm, sat_norm,
                rx_pos.x(), rx_pos.y(), rx_pos.z(),
                sat_pos.x(), sat_pos.y(), sat_pos.z());
            return 0.0;
        }

        // Compute correction: (ω_E / c) * (x_s * y_r - y_s * x_r)
        double cross_term = sat_pos.x() * rx_pos.y() - sat_pos.y() * rx_pos.x();
        double correction = (EARTH_ANGULAR_VELOCITY / C) * cross_term;
        if (!std::isfinite(correction) || std::abs(correction) > MAX_SAGNAC_CORRECTION) {
            log_manager_.getLogger()->warn("Excessive Sagnac correction: {:.3e} m, cross_term: {:.3e}, "
                "rx_pos=({}, {}, {}), sat_pos=({}, {}, {})",
                correction, cross_term,
                rx_pos.x(), rx_pos.y(), rx_pos.z(),
                sat_pos.x(), sat_pos.y(), sat_pos.z());
            return 0.0;
        }

        log_manager_.getLogger()->trace("Sagnac correction: {:.3f} m, cross_term: {:.3e}, "
            "rx_pos=({}, {}, {}), sat_pos=({}, {}, {})",
            correction, cross_term,
            rx_pos.x(), rx_pos.y(), rx_pos.z(),
            sat_pos.x(), sat_pos.y(), sat_pos.z());
        return correction;
    }

    double PppModel::computeAtmosphericLoadingCorrection(const Eigen::Vector3d& rx_pos,
        const GnssTime& time) const {
        // Placeholder: Simplified atmospheric loading correction
        // Model: Small sinusoidal variation based on time and position latitude
        // Note: Full implementation requires external loading models (e.g., IERS grids)
        double latitude = std::asin(rx_pos.z() / rx_pos.norm()) * 180.0 / Constants::PI; // Approx. latitude (degrees)
        double t = fmod(time.tow, 86400.0); // Seconds of day
        double amplitude = 0.001; // 1 mm amplitude (typical)
        double correction = amplitude * std::cos(2.0 * Constants::PI * t / 86400.0) * std::cos(latitude * Constants::PI / 180.0);
        log_manager_.getLogger()->trace("Atmospheric loading correction: {} m", correction);
        return correction;
    }

    const RinexParser& PppModel::getRinexParser() const {
        return rinex_parser_;
    }

    std::string PppModel::getReceiverAntennaType() const {
        std::string antenna_type = rinex_parser_.getReceiverAntennaType();
        if (antenna_type.empty()) {
            log_manager_.getLogger()->warn("Empty receiver antenna type returned from RINEX parser");
        }
        return antenna_type;
    }

    void PppModel::computeSatelliteAttitude(System system,
        const Eigen::Vector3d& sat_pos,
        const Eigen::Vector3d& sat_vel,
        Eigen::Vector3d& d_s,
        Eigen::Vector3d& y_s,
        const GnssTime& time) const {
        // Get Sun position
        Eigen::Vector3d sun_pos = interpolateSunPosition(time);
        if (sun_pos.norm() < 1e6) { // Fallback if no ephemeris
            sun_pos = Eigen::Vector3d(1.496e11, 0.0, 0.0); // 1 AU along x-axis
            log_manager_.getLogger()->warn("No Sun ephemeris for time TOW {0}", time.tow);
        }
        Eigen::Vector3d e = (sun_pos - sat_pos).normalized(); // Sun direction

        // Compute yaw-steering attitude
        Eigen::Vector3d y = sat_vel.normalized().cross(e);
        if (y.norm() < 1e-6) { // Handle collinearity
            y = Eigen::Vector3d(0.0, 1.0, 0.0); // Arbitrary fallback
        }
        y_s = y.normalized();
        Eigen::Vector3d z_s = -sat_pos.normalized();
        d_s = y_s.cross(z_s).normalized();
    }

    Eigen::Vector3d PppModel::getSatelliteVelocity(const std::string& satellite, const GnssTime& time) const {
        constexpr int velocity_order = 6; // Interpolation order for velocity
        constexpr double MIN_POSITION_NORM = 1.8e7; // ~20,000 km
        constexpr double MAX_POSITION_NORM = 3.2e7; // ~32,000 km
        constexpr double MAX_VELOCITY_NORM = 1e4; // ~10 km/s
        constexpr double MIN_TIME_DIFF = 1.0; // Minimum time difference (s)
        constexpr double MAX_TIME_WINDOW = 3600.0; // Maximum time window (s)

        // Validate inputs
        if (satellite.empty()) {
            log_manager_.getLogger()->warn("Empty satellite ID for velocity interpolation at TOW {:.3f}", time.tow);
            return Eigen::Vector3d::Zero();
        }
        if (orbits_.empty()) {
            log_manager_.getLogger()->warn("No orbit data available for {} at TOW {:.3f}", satellite, time.tow);
            return Eigen::Vector3d::Zero();
        }
        if (time.tow < 0.0 || time.week < 0) {
            log_manager_.getLogger()->warn("Invalid time for velocity interpolation for {}: week {}, TOW {:.3f}",
                satellite, time.week, time.tow);
            return Eigen::Vector3d::Zero();
        }

        // Check cached velocity
        auto vel_it = last_interpolated_velocity_.find(satellite);
        if (vel_it != last_interpolated_velocity_.end() && vel_it->second.allFinite() &&
            vel_it->second.norm() <= MAX_VELOCITY_NORM) {
            log_manager_.getLogger()->trace("Using cached velocity for {} at TOW {:.3f}: ({:.3f}, {:.3f}, {:.3f}) m/s, norm: {:.3e}",
                satellite, time.tow, vel_it->second.x(), vel_it->second.y(), vel_it->second.z(), vel_it->second.norm());
            return vel_it->second;
        }

        // Collect candidates
        std::vector<OrbitData> candidates;
        std::string system_str = satellite.substr(0, 1) == "G" ? "GPS" : satellite.substr(0, 1) == "E" ? "Galileo" : "Unknown";
        for (const auto& data : orbits_) {
            if (data.satellite == satellite && data.position_valid &&
                data.position.allFinite() && data.position.norm() >= MIN_POSITION_NORM &&
                data.position.norm() <= MAX_POSITION_NORM) {
                double time_diff = std::abs(data.time.tow - time.tow);
                if (time_diff < MAX_TIME_WINDOW) {
                    candidates.push_back(data);
                }
            }
        }

        // Check if sufficient data
        if (candidates.size() < velocity_order) {
            log_manager_.getLogger()->debug("Insufficient orbit data for velocity of {} ({}) at TOW {:.3f}: found {} points, need {}",
                satellite, system_str, time.tow, candidates.size(), velocity_order);
            if (!candidates.empty()) {
                auto it = std::min_element(candidates.begin(), candidates.end(),
                    [&time](const OrbitData& a, const OrbitData& b) {
                        return std::abs(a.time.tow - time.tow) < std::abs(b.time.tow - time.tow);
                    });
                if (it->velocity_valid && it->velocity.allFinite() && it->velocity.norm() <= MAX_VELOCITY_NORM) {
                    last_interpolated_velocity_[satellite] = it->velocity;
                    log_manager_.getLogger()->trace("Using nearest velocity for {} ({}) at TOW {:.3f}: ({:.3f}, {:.3f}, {:.3f}) m/s, norm: {:.3e}",
                        satellite, system_str, it->time.tow, it->velocity.x(), it->velocity.y(), it->velocity.z(), it->velocity.norm());
                    return it->velocity;
                }
            }
            log_manager_.getLogger()->warn("No valid velocity data for {} ({}) at TOW {:.3f}", satellite, system_str, time.tow);
            last_interpolated_velocity_.erase(satellite);
            return Eigen::Vector3d::Zero();
        }

        // Sort candidates by time proximity
        std::sort(candidates.begin(), candidates.end(),
            [&time](const OrbitData& a, const OrbitData& b) {
                return std::abs(a.time.tow - time.tow) < std::abs(b.time.tow - time.tow);
            });
        candidates.resize(velocity_order);

        // Check time separation
        for (size_t i = 0; i < candidates.size(); ++i) {
            for (size_t j = i + 1; j < candidates.size(); ++j) {
                if (std::abs(candidates[i].time.tow - candidates[j].time.tow) < MIN_TIME_DIFF) {
                    log_manager_.getLogger()->warn("Time difference too small for {} ({}) at TOW {:.3f}: {:.3f} s between epochs",
                        satellite, system_str, time.tow, std::abs(candidates[i].time.tow - candidates[j].time.tow));
                    last_interpolated_velocity_.erase(satellite);
                    return Eigen::Vector3d::Zero();
                }
            }
        }

        // Compute velocity using central difference
        Eigen::Vector3d interpolated_vel(0.0, 0.0, 0.0);
        bool velocity_computed = false;
        for (size_t i = 2; i < velocity_order - 2; ++i) {
            double t_prev = candidates[i - 1].time.tow;
            double t_next = candidates[i + 1].time.tow;
            double dt = t_next - t_prev;
            if (dt >= MIN_TIME_DIFF) {
                interpolated_vel += (candidates[i + 1].position - candidates[i - 1].position) / dt;
                velocity_computed = true;
            }
        }
        if (velocity_computed) {
            interpolated_vel /= (velocity_order - 4);
        }

        // Use SP3 velocity if available
        bool use_sp3_velocity = true;
        for (size_t i = 0; i < velocity_order; ++i) {
            if (!candidates[i].velocity_valid || !candidates[i].velocity.allFinite() ||
                candidates[i].velocity.norm() > MAX_VELOCITY_NORM) {
                use_sp3_velocity = false;
                break;
            }
        }
        if (use_sp3_velocity) {
            interpolated_vel = Eigen::Vector3d(0.0, 0.0, 0.0);
            double t = time.tow;
            for (size_t i = 0; i < velocity_order; ++i) {
                double li = 1.0;
                for (size_t j = 0; j < velocity_order; ++j) {
                    if (i != j) {
                        li *= (t - candidates[j].time.tow) / (candidates[i].time.tow - candidates[j].time.tow);
                    }
                }
                interpolated_vel += li * candidates[i].velocity;
            }
            velocity_computed = true;
        }

        // Validate interpolated velocity
        if (!velocity_computed || !interpolated_vel.allFinite() || interpolated_vel.norm() > MAX_VELOCITY_NORM) {
            log_manager_.getLogger()->warn("Invalid interpolated velocity for {} ({}) at TOW {:.3f}: ({:.3e}, {:.3e}, {:.3e}) m/s, norm: {:.3e}",
                satellite, system_str, time.tow, interpolated_vel.x(), interpolated_vel.y(), interpolated_vel.z(), interpolated_vel.norm());
            last_interpolated_velocity_.erase(satellite);
            return Eigen::Vector3d::Zero();
        }

        // Cache and return
        last_interpolated_velocity_[satellite] = interpolated_vel;
        log_manager_.getLogger()->trace("Interpolated velocity for {} ({}) at TOW {:.3f}: ({:.3f}, {:.3f}, {:.3f}) m/s, norm: {:.3e}",
            satellite, system_str, time.tow, interpolated_vel.x(), interpolated_vel.y(), interpolated_vel.z(), interpolated_vel.norm());
        return interpolated_vel;
    }

    Eigen::Vector3d PppModel::interpolateSunPosition(const GnssTime& time) const {
        if (sun_ephemeris_.empty()) {
            return Eigen::Vector3d(0.0, 0.0, 0.0);
        }

        // Find 2 closest epochs for linear interpolation
        std::vector<SunEphemeris> candidates;
        for (const auto& data : sun_ephemeris_) {
            double time_diff = std::abs(data.time.tow - time.tow);
            if (time_diff < 86400.0) { // Within 1 day
                candidates.push_back(data);
            }
        }

        if (candidates.size() < 2) {
            if (!candidates.empty()) {
                return candidates[0].position;
            }
            return Eigen::Vector3d(0.0, 0.0, 0.0);
        }

        std::sort(candidates.begin(), candidates.end(),
            [&time](const SunEphemeris& a, const SunEphemeris& b) {
                return std::abs(a.time.tow - time.tow) < std::abs(b.time.tow - time.tow);
            });
        candidates.resize(2);

        double t = time.tow;
        double t0 = candidates[0].time.tow;
        double t1 = candidates[1].time.tow;
        Eigen::Vector3d p0 = candidates[0].position;
        Eigen::Vector3d p1 = candidates[1].position;

        if (t0 == t1) {
            return p0;
        }

        double frac = (t - t0) / (t1 - t0);
        return p0 + frac * (p1 - p0);
    }

    double PppModel::computeInitialIonoDelay(const GnssTime& time, const Eigen::Vector3d& rx_pos) const {
        if (klobuchar_coeffs_.size() != 8) {
            return 5.0; // Default fallback
        }

        // Klobuchar model for zenith delay at receiver position
        double a0 = klobuchar_coeffs_[0], a1 = klobuchar_coeffs_[1], a2 = klobuchar_coeffs_[2], a3 = klobuchar_coeffs_[3];
        double b0 = klobuchar_coeffs_[4], b1 = klobuchar_coeffs_[5], b2 = klobuchar_coeffs_[6], b3 = klobuchar_coeffs_[7];

        double t = fmod(time.tow, 86400.0);
        double phi = std::asin(rx_pos.z() / rx_pos.norm()) * 180.0 / Constants::PI; // Geodetic latitude (degrees)
        double lambda = std::atan2(rx_pos.y(), rx_pos.x()) * 180.0 / Constants::PI; // Geodetic longitude (degrees)

        double phi_i = phi / 180.0; // Normalized latitude
        double lambda_i = lambda / 180.0; // Normalized longitude
        double phi_m = phi_i + 0.064 * std::cos((lambda_i - 1.617) * Constants::PI);

        double t_i = 43200.0 * lambda_i + t;
        t_i = fmod(t_i, 86400.0);
        if (t_i < 0) t_i += 86400.0;

        double AMP = a0 + a1 * phi_m + a2 * std::pow(phi_m, 2) + a3 * std::pow(phi_m, 3);
        if (AMP < 0) AMP = 0;
        double PER = b0 + b1 * phi_m + b2 * std::pow(phi_m, 2) + b3 * std::pow(phi_m, 3);
        if (PER < 72000) PER = 72000;
        double X = 2.0 * Constants::PI * (t_i - 50400.0) / PER;

        double delay;
        if (std::abs(X) < 1.57) {
            delay = 5e-9 + AMP * (1 - X * X / 2 + X * X * X * X / 24);
        }
        else {
            delay = 5e-9;
        }

        // Convert to meters (zenith delay at L1 frequency)
        delay *= Constants::SPEED_OF_LIGHT;
        return delay;
    }

    double PppModel::computeMultipathVarianceWeight(const PppObservation& obs, const std::string& type, double elevation) const {
        // Validate inputs
        if (type.empty() || obs.satellite.empty()) {
            log_manager_.getLogger()->warn("Invalid inputs for multipath weighting: type='{}', satellite='{}'", type, obs.satellite);
            return 1.0;
        }
        if (elevation < 0.0 || elevation > 90.0 || !std::isfinite(elevation)) {
            log_manager_.getLogger()->warn("Invalid elevation for multipath weighting for {} {}: {:.3f} deg",
                obs.satellite, type, elevation);
            return 1.0;
        }

        // Elevation-based weighting
        double elev_weight = std::sin(elevation * Constants::PI / 180.0);
        elev_weight = std::max(MultipathHistory::ELEVATION_MIN_WEIGHT, elev_weight); // Ensure minimum weight

        // Only apply code-minus-carrier analysis for code observations
        if (type[0] != 'C') {
            return 1.0; // No multipath adjustment for phase or other observations
        }

        // Map code to corresponding phase (e.g., C1C -> L1C)
        std::string phase_type = "L" + type.substr(1);
        if (!obs.observed.count(phase_type)) {
            log_manager_.getLogger()->debug("No phase data for {} {} (phase_type={}), using elevation weight: {:.3f}",
                obs.satellite, type, phase_type, elev_weight);
            return elev_weight;
        }

        // Compute CMC residual: P - λ * φ
        double code = obs.observed.at(type);
        double phase = obs.observed.at(phase_type);
        double wavelength = getWavelength(obs.system, phase_type);
        if (wavelength <= 0.0 || !std::isfinite(wavelength)) {
            log_manager_.getLogger()->warn("Invalid wavelength for {} {} (phase_type={}): {:.3f} m, using elevation weight: {:.3f}",
                obs.satellite, type, phase_type, wavelength, elev_weight);
            return elev_weight;
        }
        double cmc = code - wavelength * phase;
        if (!std::isfinite(cmc)) {
            log_manager_.getLogger()->warn("Non-finite CMC for {} {}: code={:.3f}, phase={:.3f}, wavelength={:.3f}, using elevation weight: {:.3f}",
                obs.satellite, type, code, phase, wavelength, elev_weight);
            return elev_weight;
        }

        // Update multipath history
        std::string key = obs.satellite + "_" + type;
        auto& history = const_cast<std::map<std::string, MultipathHistory>&>(multipath_histories_)[key];
        history.cmc_values.push_back(cmc);
        history.timestamps.push_back(obs.time);
        if (history.cmc_values.size() > MultipathHistory::WINDOW_SIZE) {
            history.cmc_values.pop_front();
            history.timestamps.pop_front();
        }

        // Check time continuity and compute statistics
        if (history.cmc_values.size() == MultipathHistory::WINDOW_SIZE) {
            for (size_t i = 1; i < history.timestamps.size(); ++i) {
                double dt = history.timestamps[i].tow - history.timestamps[i - 1].tow;
                if (dt <= 0.0 || dt > MultipathHistory::TIME_GAP_THRESHOLD) {
                    log_manager_.getLogger()->debug("Invalid time gap for {} {}: dt={:.3f} s, using elevation weight: {:.3f}",
                        obs.satellite, type, dt, elev_weight);
                    return elev_weight;
                }
            }

            // Compute mean and standard deviation of CMC values
            double sum = 0.0, sum_sq = 0.0;
            size_t n = history.cmc_values.size();
            for (double value : history.cmc_values) {
                sum += value;
                sum_sq += value * value;
            }
            double mean = sum / n;
            double variance = n > 1 ? (sum_sq / n - mean * mean) * n / (n - 1) : 0.0;
            double std_dev = variance > 0.0 ? std::sqrt(variance) : 1e-6;

            // Check CMC residual for multipath
            double cmc_residual = std::abs(cmc - mean);
            if (cmc_residual > MultipathHistory::MULTIPATH_CMC_THRESHOLD * std_dev) {
                elev_weight *= 0.5; // Down-weight by 50% if multipath detected
                log_manager_.getLogger()->trace("Multipath detected for {} {}: CMC residual={:.3f} m, threshold={:.3f} m, adjusted weight: {:.3f}",
                    obs.satellite, type, cmc_residual, MultipathHistory::MULTIPATH_CMC_THRESHOLD * std_dev, elev_weight);
            }
        }

        log_manager_.getLogger()->trace("Multipath variance weight for {} {}: {:.3f}, elevation={:.3f} deg, CMC={:.3f} m",
            obs.satellite, type, elev_weight, elevation, cmc);
        return elev_weight;
    }

} // namespace GnssProcessor
//#endif