#include "PppModel.hpp"
#include "PppTypes.hpp"
#include "RinexParser.hpp"
#include "StateManager.hpp"
#include "ProductManager.hpp"
#include "ProductParser.hpp"
#include "Constants.hpp"
#include <algorithm>
#include <fstream>
#include <sstream>

namespace GnssProcessor {

    PppModel::PppModel(const GnssConfig& config, ProductManager& product_manager, LogManager& log_manager,
        const RinexParser& rinex_parser, StateManager& state_manager)
        : config_(config), product_manager_(product_manager), log_manager_(log_manager),
        parser_(config, log_manager), rinex_parser_(rinex_parser), state_manager_(state_manager),
        signal_priorities_(config.getSignalPriorities()), elevation_threshold_(config.getElevationMaskDeg()),
        multipath_histories_(), klobuchar_coeffs_(), sun_ephemeris_() {
        loadProducts();
        log_manager_.getLogger()->info("PppModel initialized with elevation mask: {} degrees, {} signal priority sets",
            elevation_threshold_, signal_priorities_.size());
    }

    void PppModel::loadProducts() {
        for (const auto& sp3_path : product_manager_.getOrbitPaths()) {
            std::vector<OrbitData> temp_orbits;
            if (parser_.parseSp3(sp3_path, temp_orbits)) {
                orbits_.insert(orbits_.end(), temp_orbits.begin(), temp_orbits.end());
            }
        }
        std::filesystem::path clk_path = product_manager_.getProductPath("CLK");
        if (!clk_path.empty()) parser_.parseClk(clk_path, clocks_);
        std::filesystem::path atx_path = product_manager_.getProductPath("ATX");
        if (!atx_path.empty()) parser_.parseAtx(atx_path, antennas_);
        std::filesystem::path bia_path = product_manager_.getProductPath("BIA");
        if (!bia_path.empty()) parser_.parseBia(bia_path, biases_);
        std::filesystem::path erp_path = product_manager_.getProductPath("ERP");
        if (!erp_path.empty()) parser_.parseErp(erp_path, erps_);
        std::filesystem::path nc_path = product_manager_.getProductPath("OCEAN_TIDE");
        //if (!nc_path.empty()) parser_.parseOceanTide(nc_path, tides_);
        std::filesystem::path brdc_path = product_manager_.getProductPath("BRDC");
        if (!brdc_path.empty()) {
            parser_.parseKlobuchar(brdc_path, klobuchar_coeffs_);
            parser_.parseSunEphemeris(brdc_path, sun_ephemeris_);
        }
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

        if (state.position.norm() == 0.0) {
            Eigen::Vector3d init_pos = rinex_parser_.getApproxPositionXyz();
            if (init_pos.norm() > 1e3) {
                state.position = init_pos;
                log_manager_.getLogger()->info("Initialized position from RINEX: ({}, {}, {}) m",
                    init_pos.x(), init_pos.y(), init_pos.z());
            }
            else {
                state.position = config_.getInitialPosStd() > 0 ? Eigen::Vector3d::Zero() : Eigen::Vector3d::Zero();
                log_manager_.getLogger()->warn("Invalid or zero RINEX approximate position, using default: (0, 0, 0) m");
            }

            GnssTime init_time = rinex_parser_.getTimeOfFirstObs();
            if (init_time.tow > 0.0) {
                state.epoch_time = init_time;
                log_manager_.getLogger()->info("Initialized state epoch time from RINEX: TOW {}", init_time.tow);
            }
            else {
                state.epoch_time = GnssTime{ 0, 0.0 };
                log_manager_.getLogger()->warn("Invalid RINEX time of first observation, using default: TOW 0.0");
            }

            state.clock_bias = 0.0;
            state.troposphere_zenith_delay = 2.0;
            if (init_pos.norm() > 1e3 && init_time.tow > 0.0 && klobuchar_coeffs_.size() == 8) {
                state.ionosphere_zenith_delay = computeInitialIonoDelay(init_time, init_pos);
                log_manager_.getLogger()->info("Initialized ionospheric zenith delay from Klobuchar: {} m", state.ionosphere_zenith_delay);
            }
            else {
                state.ionosphere_zenith_delay = 5.0;
                log_manager_.getLogger()->warn("Using default ionospheric zenith delay: 5.0 m (insufficient Klobuchar data)");
            }

            state.covariance = Eigen::MatrixXd::Identity(6, 6);
            state.covariance.block(0, 0, 3, 3) *= config_.getInitialPosStd() * config_.getInitialPosStd();
            state.covariance(3, 3) = config_.getInitialClockStd() * config_.getInitialClockStd();
            state.covariance(4, 4) = config_.getInitialTropoStd() * config_.getInitialTropoStd();
            state.covariance(5, 5) = config_.getInitialIonoStd() * config_.getInitialIonoStd();
        }

        for (const auto& obs : observations) {
            PppObservation ppp_obs;
            ppp_obs.satellite = obs.satellite;
            ppp_obs.system = obs.system;
            ppp_obs.time = obs.time;
            ppp_obs.observed = obs.measurements;
            ppp_obs.variances = obs.variances;
            ppp_obs.lli_flags = obs.lli_flags;

            Eigen::Vector3d sat_pos = getSatellitePosition(obs.satellite, obs.time);
            if (sat_pos.norm() == 0.0) {
                log_manager_.getLogger()->warn("No satellite position for {} at time {}", obs.satellite, obs.time.tow);
                continue;
            }

            double elevation = computeElevation(state.position, sat_pos);
            if (elevation < elevation_threshold_) {
                log_manager_.getLogger()->debug("Satellite {} below elevation threshold: {} degrees", obs.satellite, elevation);
                continue;
            }

            Eigen::Vector3d los = (sat_pos - state.position);
            double azimuth = std::atan2(los.y(), los.x()) * 180.0 / Constants::PI;
            if (azimuth < 0.0) azimuth += 360.0;

            Eigen::Vector3d rx_pos = state.position + computePoleTideCorrection(state.position, obs.time) +
                computeOceanTideCorrection(state.position, obs.time);
            double range = computeGeometricRange(rx_pos, sat_pos);
            double tropo_delay = computeTroposphericDelay(elevation, state.troposphere_zenith_delay);
            double sat_clock = getSatelliteClockCorrection(obs.satellite, obs.time);
            double shapiro_delay = computeShapiroDelay(rx_pos, sat_pos);
            double sagnac_corr = computeSagnacCorrection(rx_pos, sat_pos);
            double atm_loading_corr = computeAtmosphericLoadingCorrection(rx_pos, obs.time);

            Eigen::Vector3d sat_vel(0.0, 0.0, 0.0);
            auto orbit_it = std::find_if(orbits_.begin(), orbits_.end(),
                [&obs](const OrbitData& o) { return o.satellite == obs.satellite && o.time.tow == obs.time.tow; });
            if (orbit_it != orbits_.end() && orbit_it->velocity_valid) {
                sat_vel = orbit_it->velocity;
            }
            double relativistic_corr = computeRelativisticCorrection(sat_pos, sat_vel);

            std::string system_str;
            switch (obs.system) {
            case System::GPS: system_str = "GPS"; break;
            case System::Galileo: system_str = "Galileo"; break;
            case System::GLONASS: system_str = "GLONASS"; break;
            case System::BeiDou: system_str = "BeiDou"; break;
            case System::QZSS: system_str = "QZSS"; break;
            case System::SBAS: system_str = "SBAS"; break;
            default: system_str = "Unknown";
            }

            auto obs_types = selectObservationTypes(obs.measurements, obs.system, system_str);
            if (obs_types.empty()) {
                log_manager_.getLogger()->warn("No valid observation types for system: {}", system_str);
                continue;
            }

            std::string rx_antenna_type = rinex_parser_.getReceiverAntennaType();
            for (const auto& type : obs_types) {
                if (obs.measurements.count(type)) {
                    double iono_delay = computeIonosphericDelay(obs.time, rx_pos, sat_pos, type, obs.system, obs.measurements);
                    double antenna_corr = getAntennaCorrection(obs.satellite, type, obs.system, rx_pos, sat_pos, elevation, azimuth, rx_antenna_type, obs.time);
                    double bias_corr = getBiasCorrection(obs.satellite, type, obs.time);
                    double modeled = range + state.clock_bias * Constants::SPEED_OF_LIGHT + tropo_delay -
                        (sat_clock + relativistic_corr) * Constants::SPEED_OF_LIGHT + iono_delay +
                        antenna_corr + bias_corr + shapiro_delay + sagnac_corr + atm_loading_corr;
                    if (type[0] == 'L') {
                        std::string amb_key = obs.satellite + "_" + type;
                        if (!state.ambiguities.count(amb_key)) {
                            double wavelength = getWavelength(type, obs.system);
                            state.ambiguities[amb_key] = obs.measurements.at(type) - modeled;
                            state.ambiguities[amb_key] = std::round(state.ambiguities[amb_key] / wavelength) * wavelength;
                            state_manager_.addAmbiguity(amb_key, state.ambiguities[amb_key]);
                        }
                        modeled += state.ambiguities[amb_key];
                    }

                    ppp_obs.modeled[type] = modeled;
                    ppp_obs.residuals[type] = obs.measurements.at(type) - modeled;

                    // Apply multipath mitigation for code observations
                    if (type[0] == 'C' && obs.variances.count(type)) {
                        double mp_weight = computeMultipathVarianceWeight(ppp_obs, type, elevation);
                        ppp_obs.variances[type] *= (1.0 / mp_weight); // Increase variance for multipath
                        log_manager_.getLogger()->trace("Multipath weight for {} {}: {}, adjusted variance: {}",
                            obs.satellite, type, mp_weight, ppp_obs.variances[type]);
                    }

                    log_manager_.getLogger()->trace("Modeled {} for {}: observed {:.3f}, modeled {:.3f}, residual {:.3f}",
                        type, obs.satellite, obs.measurements.at(type), modeled, ppp_obs.residuals[type]);
                }
            }
            modeled_obs.push_back(ppp_obs);
        }

        log_manager_.getLogger()->info("Modeled {} PPP observations", modeled_obs.size());
        return !modeled_obs.empty();
    }

    double PppModel::computeGeometricRange(const Eigen::Vector3d& rx_pos, const Eigen::Vector3d& sat_pos) const {
        return (sat_pos - rx_pos).norm();
    }

    double PppModel::computeTroposphericDelay(double elevation, double zenith_delay) const {
        double sin_el = std::sin(elevation * Constants::PI / 180.0);
        double mapping = 1.0 / (sin_el + 0.00143 / (std::tan(elevation * Constants::PI / 180.0) + 0.0445));
        double delay = zenith_delay * mapping;
        log_manager_.getLogger()->trace("Computed tropospheric delay: {} m, elevation: {} deg", delay, elevation);
        return delay;
    }

    double PppModel::computeIonosphericDelay(const GnssTime& time,
        const Eigen::Vector3d& rx_pos,
        const Eigen::Vector3d& sat_pos,
        const std::string& freq_type,
        System system,
        const std::map<std::string, double>& measurements) const {
        double freq = getFrequency(freq_type, system);
        if (freq == 0.0) {
            log_manager_.getLogger()->warn("Unknown frequency for {} on system {}", freq_type, static_cast<int>(system));
            return 0.0;
        }

        std::string system_str;
        switch (system) {
        case System::GPS: system_str = "GPS"; break;
        case System::Galileo: system_str = "Galileo"; break;
        case System::GLONASS: system_str = "GLONASS"; break;
        case System::BeiDou: system_str = "BeiDou"; break;
        case System::QZSS: system_str = "QZSS"; break;
        case System::SBAS: system_str = "SBAS"; break;
        default: system_str = "Unknown";
        }
        auto it = signal_priorities_.find(system_str);
        if (it == signal_priorities_.end()) return 0.0;
        const auto& signals = it->second;

        if (signals.size() >= 3 && measurements.count(signals[0]) && measurements.count(signals[1]) && measurements.count(signals[2])) {
            double iono_free = computeTripleIonoFreeCombination(measurements, system, freq_type);
            log_manager_.getLogger()->trace("Computed triple-frequency iono-free delay for {}: {} m", freq_type, iono_free);
            return iono_free;
        }
        if (signals.size() >= 2 && measurements.count(signals[0]) && measurements.count(signals[1])) {
            double iono_free = computeIonoFreeCombination(measurements, system, freq_type);
            log_manager_.getLogger()->trace("Computed dual-frequency iono-free delay for {}: {} m", freq_type, iono_free);
            return iono_free;
        }
        if (klobuchar_coeffs_.size() != 8) {
            log_manager_.getLogger()->warn("No Klobuchar coefficients available for {}", freq_type);
            return 0.0;
        }
        double delay = computeKlobucharDelay(time, rx_pos, sat_pos, freq);
        log_manager_.getLogger()->trace("Computed Klobuchar delay for {}: {} m", freq_type, delay);
        return delay;
    }

    double PppModel::computeKlobucharDelay(const GnssTime& time,
        const Eigen::Vector3d& rx_pos,
        const Eigen::Vector3d& sat_pos,
        double frequency) const {
        double a0 = klobuchar_coeffs_[0], a1 = klobuchar_coeffs_[1], a2 = klobuchar_coeffs_[2], a3 = klobuchar_coeffs_[3];
        double b0 = klobuchar_coeffs_[4], b1 = klobuchar_coeffs_[5], b2 = klobuchar_coeffs_[6], b3 = klobuchar_coeffs_[7];

        double t = fmod(time.tow, 86400.0);
        double phi = 0.0, lambda = 0.0;
        double el = computeElevation(rx_pos, sat_pos) * Constants::PI / 180.0;
        double az = 0.0;

        double psi = 0.0137 / (el + 0.11) - 0.022;
        double phi_i = phi + psi * std::cos(az);
        if (phi_i > 0.416) phi_i = 0.416;
        if (phi_i < -0.416) phi_i = -0.416;
        double lambda_i = lambda + psi * std::sin(az) / std::cos(phi_i);
        double phi_m = phi_i + 0.064 * std::cos(lambda_i - 1.617);

        double t_i = 43200.0 * lambda_i / Constants::PI + t;
        t_i = fmod(t_i, 86400.0);
        if (t_i < 0) t_i += 86400.0;

        double F = 1.0 + 16.0 * std::pow((0.53 - el / Constants::PI), 3);
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
        const std::string& freq_type) const {
        std::string system_str;
        switch (system) {
        case System::GPS: system_str = "GPS"; break;
        case System::Galileo: system_str = "Galileo"; break;
        case System::GLONASS: system_str = "GLONASS"; break;
        case System::BeiDou: system_str = "BeiDou"; break;
        case System::QZSS: system_str = "QZSS"; break;
        case System::SBAS: system_str = "SBAS"; break;
        default: return 0.0;
        }
        auto it = signal_priorities_.find(system_str);
        if (it == signal_priorities_.end()) return 0.0;
        const auto& signals = it->second;

        if (signals.size() < 2) return 0.0;
        std::string f1 = signals[0], f2 = signals[1];
        if (!measurements.count(f1) || !measurements.count(f2)) return 0.0;

        double freq1 = getFrequency(f1, system);
        double freq2 = getFrequency(f2, system);
        if (freq1 == 0.0 || freq2 == 0.0) return 0.0;

        double c1 = (freq1 * freq1) / (freq1 * freq1 - freq2 * freq2);
        double c2 = -(freq2 * freq2) / (freq1 * freq1 - freq2 * freq2);

        if (freq_type == f1) return c1 * measurements.at(f1) + c2 * measurements.at(f2);
        if (freq_type == f2) return c1 * measurements.at(f1) + c2 * measurements.at(f2);
        return 0.0;
    }

    double PppModel::computeTripleIonoFreeCombination(const std::map<std::string, double>& measurements,
        System system,
        const std::string& freq_type) const {
        std::string system_str;
        switch (system) {
        case System::GPS: system_str = "GPS"; break;
        case System::Galileo: system_str = "Galileo"; break;
        case System::GLONASS: system_str = "GLONASS"; break;
        case System::BeiDou: system_str = "BeiDou"; break;
        case System::QZSS: system_str = "QZSS"; break;
        case System::SBAS: system_str = "SBAS"; break;
        default: return 0.0;
        }
        auto it = signal_priorities_.find(system_str);
        if (it == signal_priorities_.end()) return 0.0;
        const auto& signals = it->second;

        if (signals.size() < 3) return 0.0;
        std::string f1 = signals[0], f2 = signals[1], f3 = signals[2];
        if (!measurements.count(f1) || !measurements.count(f2) || !measurements.count(f3)) return 0.0;

        double freq1 = getFrequency(f1, system);
        double freq2 = getFrequency(f2, system);
        double freq3 = getFrequency(f3, system);
        if (freq1 == 0.0 || freq2 == 0.0 || freq3 == 0.0) return 0.0;

        double f1_sq = freq1 * freq1, f2_sq = freq2 * freq2, f3_sq = freq3 * freq3;
        double denom = f1_sq * f2_sq * (f1_sq - f2_sq) + f2_sq * f3_sq * (f2_sq - f3_sq) + f3_sq * f1_sq * (f3_sq - f1_sq);
        double c1 = (f2_sq * f3_sq * (f2_sq - f3_sq)) / denom;
        double c2 = (f3_sq * f1_sq * (f3_sq - f1_sq)) / denom;
        double c3 = (f1_sq * f2_sq * (f1_sq - f2_sq)) / denom;

        double combined = c1 * measurements.at(f1) + c2 * measurements.at(f2) + c3 * measurements.at(f3);
        if (freq_type == f1 || freq_type == f2 || freq_type == f3) {
            return combined;
        }
        log_manager_.getLogger()->warn("Frequency {} not in triple-frequency set for system {}", freq_type, system_str);
        return 0.0;
    }

    double PppModel::getSatelliteClockCorrection(const std::string& satellite, const GnssTime& time) const {
        if (clocks_.empty()) {
            log_manager_.getLogger()->warn("No clock data available for satellite {}", satellite);
            return 0.0;
        }

        double clock_corr = interpolateClock(satellite, time, clocks_);
        if (clock_corr != 0.0 || std::abs(clock_corr) < 1e-3) { // Allow small non-zero corrections
            log_manager_.getLogger()->trace("Interpolated clock correction for {} at time {}: {} s", satellite, time.tow, clock_corr);
            return clock_corr;
        }
        log_manager_.getLogger()->warn("No valid clock correction interpolated for {} at time {}", satellite, time.tow);
        return 0.0;
    }

    double PppModel::interpolateClock(const std::string& satellite,
        const GnssTime& time,
        const std::vector<ClockData>& clock_data) const {
        // Find 2 closest epochs for linear interpolation
        std::vector<ClockData> candidates;
        for (const auto& data : clock_data) {
            if (data.satellite == satellite) {
                double time_diff = std::abs(data.time.tow - time.tow);
                if (time_diff < 60.0) { // Within 1 minute
                    candidates.push_back(data);
                }
            }
        }

        if (candidates.size() < 2) {
            log_manager_.getLogger()->debug("Insufficient clock data for {}: found {} points, need 2", satellite, candidates.size());
            // Fallback to nearest epoch if available
            if (!candidates.empty()) {
                auto it = std::min_element(candidates.begin(), candidates.end(),
                    [&time](const ClockData& a, const ClockData& b) {
                        return std::abs(a.time.tow - time.tow) < std::abs(b.time.tow - time.tow);
                    });
                return it->correction;
            }
            return 0.0;
        }

        // Select 2 closest points
        std::sort(candidates.begin(), candidates.end(),
            [&time](const ClockData& a, const ClockData& b) {
                return std::abs(a.time.tow - time.tow) < std::abs(b.time.tow - time.tow);
            });
        candidates.resize(2);

        // Linear interpolation
        double t = time.tow;
        double t0 = candidates[0].time.tow;
        double t1 = candidates[1].time.tow;
        double c0 = candidates[0].correction;
        double c1 = candidates[1].correction;

        if (t0 == t1) {
            log_manager_.getLogger()->warn("Duplicate clock epochs for {} at time {}", satellite, t0);
            return c0;
        }

        double interpolated_clock = c0 + (c1 - c0) * (t - t0) / (t1 - t0);
        return interpolated_clock;
    }

    double PppModel::computeRelativisticCorrection(const Eigen::Vector3d& sat_pos,
        const Eigen::Vector3d& sat_vel) const {
        // Relativistic clock correction due to orbit eccentricity
        // Formula: -2 * (r · v) / c², where r = position, v = velocity, c = speed of light
        double r_dot_v = sat_pos.dot(sat_vel);
        double correction = -2.0 * r_dot_v / (Constants::SPEED_OF_LIGHT * Constants::SPEED_OF_LIGHT);
        log_manager_.getLogger()->trace("Relativistic correction: {} s", correction);
        return correction;
    }

    double PppModel::computeShapiroDelay(const Eigen::Vector3d& rx_pos,
        const Eigen::Vector3d& sat_pos) const {
        // Shapiro delay (gravitational time delay)
        // Formula: (2 * GM / c³) * ln(1 + cosθ), where θ is angle between rx and sat
        double GM = Constants::GRAVITATIONAL_CONSTANT; // Gravitational constant * Earth mass (m³/s²)
        double r = (sat_pos - rx_pos).norm();
        double cos_theta = (sat_pos - rx_pos).dot(sat_pos) / (r * sat_pos.norm());
        if (cos_theta > 1.0) cos_theta = 1.0;
        if (cos_theta < -1.0) cos_theta = -1.0;
        double delay = (2.0 * GM / (Constants::SPEED_OF_LIGHT * Constants::SPEED_OF_LIGHT * Constants::SPEED_OF_LIGHT)) *
            std::log(1.0 + cos_theta);
        delay *= Constants::SPEED_OF_LIGHT; // Convert seconds to meters
        log_manager_.getLogger()->trace("Shapiro delay: {} m", delay);
        return delay;
    }

    Eigen::Vector3d PppModel::getSatellitePosition(const std::string& satellite, const GnssTime& time) const {
        if (orbits_.empty()) {
            log_manager_.getLogger()->warn("No orbit data available for satellite {}", satellite);
            return Eigen::Vector3d::Zero();
        }

        Eigen::Vector3d pos = interpolateOrbit(satellite, time, orbits_);
        if (pos.norm() > 0.0) {
            log_manager_.getLogger()->trace("Interpolated position for {} at time {}: ({}, {}, {})",
                satellite, time.tow, pos(0), pos(1), pos(2));
            return pos;
        }
        log_manager_.getLogger()->warn("No valid position interpolated for {} at time {}", satellite, time.tow);
        return Eigen::Vector3d::Zero();
    }

    Eigen::Vector3d PppModel::interpolateOrbit(const std::string& satellite,
        const GnssTime& time,
        const std::vector<OrbitData>& orbit_data) const {
        // Find 4 closest epochs for Lagrange interpolation (2 before, 2 after)
        const int order = 4; // 4-point Lagrange interpolation
        std::vector<OrbitData> candidates;
        for (const auto& data : orbit_data) {
            if (data.satellite == satellite && data.position_valid) {
                double time_diff = std::abs(data.time.tow - time.tow);
                if (time_diff < 3600.0) { // Within 1 hour
                    candidates.push_back(data);
                }
            }
        }

        if (candidates.size() < order) {
            log_manager_.getLogger()->debug("Insufficient orbit data for {}: found {} points, need {}", satellite, candidates.size(), order);
            // Fallback to nearest epoch if available
            if (!candidates.empty()) {
                auto it = std::min_element(candidates.begin(), candidates.end(),
                    [&time](const OrbitData& a, const OrbitData& b) {
                        return std::abs(a.time.tow - time.tow) < std::abs(b.time.tow - time.tow);
                    });
                return it->position;
            }
            return Eigen::Vector3d::Zero();
        }

        // Sort by time difference and select closest 4 points
        std::sort(candidates.begin(), candidates.end(),
            [&time](const OrbitData& a, const OrbitData& b) {
                return std::abs(a.time.tow - time.tow) < std::abs(b.time.tow - time.tow);
            });
        candidates.resize(order);

        // Lagrange interpolation
        Eigen::Vector3d interpolated_pos(0.0, 0.0, 0.0);
        double t = time.tow;
        bool use_velocity = true;
        for (const auto& data : candidates) {
            if (!data.velocity_valid) {
                use_velocity = false;
                break;
            }
        }

        if (use_velocity) {
            // Use position and velocity with Hermite-like interpolation
            for (int i = 0; i < order; ++i) {
                double ti = candidates[i].time.tow;
                double li = 1.0;
                for (int j = 0; j < order; ++j) {
                    if (i != j) {
                        li *= (t - candidates[j].time.tow) / (ti - candidates[j].time.tow);
                    }
                }
                // Position contribution
                interpolated_pos += li * candidates[i].position;
                // Velocity contribution (approximate derivative term)
                double vi = 1.0;
                double sum = 0.0;
                for (int j = 0; j < order; ++j) {
                    if (i != j) {
                        vi *= (t - candidates[j].time.tow) / (ti - candidates[j].time.tow);
                        sum += 1.0 / (ti - candidates[j].time.tow);
                    }
                }
                vi *= sum;
                interpolated_pos += vi * candidates[i].velocity * (t - ti);
            }
        }
        else {
            // Standard Lagrange interpolation with positions only
            for (int i = 0; i < order; ++i) {
                double li = 1.0;
                for (int j = 0; j < order; ++j) {
                    if (i != j) {
                        li *= (t - candidates[j].time.tow) / (candidates[i].time.tow - candidates[j].time.tow);
                    }
                }
                interpolated_pos += li * candidates[i].position;
            }
        }

        return interpolated_pos;
    }

    double PppModel::computeElevation(const Eigen::Vector3d& rx_pos, const Eigen::Vector3d& sat_pos) const {
        Eigen::Vector3d los = (sat_pos - rx_pos).normalized();
        double elevation = std::asin(los(2)) * 180.0 / Constants::PI;
        log_manager_.getLogger()->trace("Computed elevation: {} degrees", elevation);
        return elevation;
    }

    double PppModel::getWavelength(const std::string& freq_type, System system) const {
        double freq = getFrequency(freq_type, system);
        if (freq > 0.0) {
            return Constants::SPEED_OF_LIGHT / freq;
        }
        log_manager_.getLogger()->warn("Unknown wavelength for {} on system {}", freq_type, static_cast<int>(system));
        return 0.19; // Default fallback
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
        double correction = 0.0;
        bool is_phase = freq_type[0] == 'L';
        double wavelength = is_phase ? getWavelength(freq_type, system) : 0.0;

        // Map frequency type to ATX frequency code
        std::string atx_freq;
        if (system == System::GPS) {
            if (freq_type.find("C1C") == 0 || freq_type.find("L1C") == 0 || freq_type.find("C1W") == 0 || freq_type.find("L1W") == 0) atx_freq = "G01";
            else if (freq_type.find("C2W") == 0 || freq_type.find("L2W") == 0 || freq_type.find("C2C") == 0 || freq_type.find("L2C") == 0) atx_freq = "G02";
            else if (freq_type.find("C5Q") == 0 || freq_type.find("L5Q") == 0) atx_freq = "G05";
        }
        else if (system == System::Galileo) {
            if (freq_type.find("C1C") == 0 || freq_type.find("L1C") == 0 || freq_type.find("C1B") == 0 || freq_type.find("L1B") == 0) atx_freq = "E01";
            else if (freq_type.find("C5Q") == 0 || freq_type.find("L5Q") == 0) atx_freq = "E05";
            else if (freq_type.find("C7Q") == 0 || freq_type.find("L7Q") == 0) atx_freq = "E07";
        }
        else if (system == System::GLONASS) {
            if (freq_type.find("C1C") == 0 || freq_type.find("L1C") == 0) atx_freq = "R01";
            else if (freq_type.find("C2C") == 0 || freq_type.find("L2C") == 0) atx_freq = "R02";
        }
        else if (system == System::BeiDou) {
            if (freq_type.find("C2I") == 0 || freq_type.find("L2I") == 0) atx_freq = "C02";
            else if (freq_type.find("C7I") == 0 || freq_type.find("L7I") == 0) atx_freq = "C07";
            else if (freq_type.find("C6I") == 0 || freq_type.find("L6I") == 0) atx_freq = "C06";
        }
        else if (system == System::QZSS) {
            if (freq_type.find("C1C") == 0 || freq_type.find("L1C") == 0) atx_freq = "J01";
            else if (freq_type.find("C2L") == 0 || freq_type.find("L2L") == 0) atx_freq = "J02";
            else if (freq_type.find("C5Q") == 0 || freq_type.find("L5Q") == 0) atx_freq = "J05";
        }
        else if (system == System::SBAS) {
            if (freq_type.find("C1C") == 0 || freq_type.find("L1C") == 0) atx_freq = "S01";
            else if (freq_type.find("C5I") == 0 || freq_type.find("L5I") == 0) atx_freq = "S05";
        }

        if (atx_freq.empty()) {
            log_manager_.getLogger()->warn("No ATX frequency mapping for {} on system {}", freq_type, static_cast<int>(system));
            return 0.0;
        }

        // Satellite antenna correction
        std::string sat_antenna_type = satellite;
        auto sat_it = std::find_if(antennas_.begin(), antennas_.end(),
            [&sat_antenna_type](const AntennaData& a) { return a.type.find(sat_antenna_type) != std::string::npos; });
        if (sat_it != antennas_.end() && sat_it->pco.count(atx_freq)) {
            // PCO correction: Project onto line-of-sight (mm to m)
            Eigen::Vector3d pco = sat_it->pco.at(atx_freq) * 1e-3;
            Eigen::Vector3d los = (sat_pos - rx_pos).normalized();
            correction += pco.dot(los);

            // PCV correction: Interpolate based on zenith and azimuth
            if (sat_it->pcv.count(atx_freq) && !sat_it->pcv.at(atx_freq).empty()) {
                const auto& pcv_table = sat_it->pcv.at(atx_freq);
                double zenith = 90.0 - elevation;
                double dazi = 5.0; // Assume 5° azimuth increment
                double dzen = 5.0; // Assume 5° zenith increment
                double pcv = interpolatePcv(pcv_table, zenith, azimuth, dazi, dzen) * 1e-3; // mm to m
                correction += pcv;
            }
        }

        // Receiver antenna correction
        auto rx_it = std::find_if(antennas_.begin(), antennas_.end(),
            [&rx_antenna_type](const AntennaData& a) { return a.type.find(rx_antenna_type) != std::string::npos; });
        if (rx_it != antennas_.end() && rx_it->pco.count(atx_freq)) {
            // PCO correction: Project onto line-of-sight (mm to m)
            Eigen::Vector3d pco = rx_it->pco.at(atx_freq) * 1e-3;
            Eigen::Vector3d los = (sat_pos - rx_pos).normalized();
            correction -= pco.dot(los);

            // PCV correction: Interpolate based on zenith and azimuth
            if (rx_it->pcv.count(atx_freq) && !rx_it->pcv.at(atx_freq).empty()) {
                const auto& pcv_table = rx_it->pcv.at(atx_freq);
                double zenith = 90.0 - elevation;
                double dazi = 5.0;
                double dzen = 5.0;
                double pcv = interpolatePcv(pcv_table, zenith, azimuth, dazi, dzen) * 1e-3; // mm to m
                correction -= pcv;
            }
        }

        // Phase wind-up correction (for carrier phase only)
        if (is_phase) {
            double wind_up = computePhaseWindUp(satellite, system, rx_pos, sat_pos, wavelength, time);
            correction += wind_up;
        }

        if (correction != 0.0) {
            log_manager_.getLogger()->trace("Antenna correction for {} {}: {} m", satellite, freq_type, correction);
        }
        else {
            log_manager_.getLogger()->trace("No antenna correction for {} {}", satellite, freq_type);
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
        computeSatelliteAttitude(system, sat_pos, Eigen::Vector3d::Zero(), d_s, y_s, time);

        // Receiver antenna orientation
        double az = config_.getReceiverAntennaAzimuthDeg() * Constants::PI / 180.0;
        double el = config_.getReceiverAntennaElevationDeg() * Constants::PI / 180.0;
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
        auto it = std::find_if(biases_.begin(), biases_.end(),
            [&satellite, &freq_type, &time](const BiasData& b) {
                return b.satellite == satellite && b.signal == freq_type &&
                    b.start_time.tow <= time.tow && time.tow <= b.end_time.tow;
            });
        if (it != biases_.end()) {
            log_manager_.getLogger()->trace("Bias correction for {} {}: {} m", satellite, freq_type, it->bias);
            return it->bias;
        }
        log_manager_.getLogger()->trace("No bias correction for {} {}", satellite, freq_type);
        return 0.0;
    }

    Eigen::Vector3d PppModel::computePoleTideCorrection(const Eigen::Vector3d& rx_pos,
        const GnssTime& time) const {
        auto it = std::min_element(erps_.begin(), erps_.end(),
            [&time](const ErpData& a, const ErpData& b) {
                return std::abs(a.time.tow - time.tow) < std::abs(b.time.tow - time.tow);
            });
        if (it != erps_.end()) {
            Eigen::Vector3d correction(0.0, 0.0, 0.0);
            log_manager_.getLogger()->trace("Pole tide correction: ({}, {}, {})", correction(0), correction(1), correction(2));
            return correction;
        }
        log_manager_.getLogger()->trace("No pole tide correction for time {}", time.tow);
        return Eigen::Vector3d::Zero();
    }

    Eigen::Vector3d PppModel::computeOceanTideCorrection(const Eigen::Vector3d& rx_pos,
        const GnssTime& time) const {
        Eigen::Vector3d correction(0.0, 0.0, 0.0);
        log_manager_.getLogger()->trace("Ocean tide correction: ({}, {}, {})", correction(0), correction(1), correction(2));
        return correction;
    }

    double PppModel::getFrequency(const std::string& freq_type, System system) const {
        if (system == System::GPS) {
            if (freq_type == "C1C" || freq_type == "L1C" || freq_type == "C1W" || freq_type == "L1W") return Constants::GPS_L1_FREQ;
            if (freq_type == "C2W" || freq_type == "L2W" || freq_type == "C2C" || freq_type == "L2C") return Constants::GPS_L2_FREQ;
            if (freq_type == "C5Q" || freq_type == "L5Q") return Constants::GPS_L5_FREQ;
        }
        else if (system == System::Galileo) {
            if (freq_type == "C1C" || freq_type == "L1C" || freq_type == "C1B" || freq_type == "L1B") return Constants::GAL_E1_FREQ;
            if (freq_type == "C5Q" || freq_type == "L5Q") return Constants::GAL_E5A_FREQ;
            if (freq_type == "C7Q" || freq_type == "L7Q") return Constants::GAL_E5B_FREQ;
        }
        else if (system == System::GLONASS) {
            // Extract channel number from type (e.g., "C1C_k0", "C1C_km7")
            size_t k_pos = freq_type.find("_k");
            if (k_pos != std::string::npos) {
                std::string channel_str = freq_type.substr(k_pos + 2);
                int channel;
                try {
                    if (channel_str[0] == 'm') {
                        channel = -std::stoi(channel_str.substr(1));
                    }
                    else {
                        channel = std::stoi(channel_str);
                    }
                    if (freq_type.find("C1C") == 0 || freq_type.find("L1C") == 0) {
                        return Constants::GLONASS_G1_BASE_FREQ + channel * Constants::GLONASS_G1_CHANNEL_OFFSET;
                    }
                    if (freq_type.find("C2C") == 0 || freq_type.find("L2C") == 0) {
                        return Constants::GLONASS_G2_BASE_FREQ + channel * Constants::GLONASS_G2_CHANNEL_OFFSET;
                    }
                }
                catch (const std::exception& e) {
                    log_manager_.getLogger()->warn("Invalid GLONASS channel format: {}", freq_type);
                }
            }
        }
        else if (system == System::BeiDou) {
            if (freq_type == "C2I" || freq_type == "L2I") return Constants::BDS_B1I_FREQ;
            if (freq_type == "C7I" || freq_type == "L7I") return Constants::BDS_B2I_FREQ;
            if (freq_type == "C6I" || freq_type == "L6I") return Constants::BDS_B3I_FREQ;
        }
        else if (system == System::QZSS) {
            if (freq_type == "C1C" || freq_type == "L1C") return Constants::QZSS_L1_FREQ;
            if (freq_type == "C2L" || freq_type == "L2L") return Constants::QZSS_L2_FREQ;
            if (freq_type == "C5Q" || freq_type == "L5Q") return Constants::QZSS_L5_FREQ;
        }
        else if (system == System::SBAS) {
            if (freq_type == "C1C" || freq_type == "L1C") return Constants::SBAS_L1_FREQ;
            if (freq_type == "C5I" || freq_type == "L5I") return Constants::SBAS_L5_FREQ;
        }
        log_manager_.getLogger()->warn("Unknown frequency for {} on system {}", freq_type, static_cast<int>(system));
        return 0.0;
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
        // Formula: (ω_E / c) * (x_s * y_r - y_s * x_r), where ω_E is Earth's angular velocity
        constexpr double EARTH_ANGULAR_VELOCITY = 7.2921151467e-5; // rad/s
        double correction = (EARTH_ANGULAR_VELOCITY / Constants::SPEED_OF_LIGHT) *
            (sat_pos.x() * rx_pos.y() - sat_pos.y() * rx_pos.x());
        correction *= Constants::SPEED_OF_LIGHT; // Convert seconds to meters
        log_manager_.getLogger()->trace("Sagnac correction: {} m", correction);
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
        // Elevation-based weighting
        double elev_weight = std::sin(elevation * Constants::PI / 180.0);
        elev_weight = std::max(ELEVATION_MIN_WEIGHT, elev_weight); // Ensure minimum weight

        // Code-minus-carrier analysis for code observations
        std::string phase_type;
        if (type[0] == 'C') {
            // Map code to corresponding phase (e.g., C1C -> L1C)
            phase_type = "L" + type.substr(1);
            if (!obs.observed.count(phase_type)) {
                return elev_weight; // No phase data, use elevation weight only
            }
        }
        else {
            return 1.0; // Only apply to code observations
        }

        // Compute CMC residual: P - λ * φ
        double code = obs.observed.at(type);
        double phase = obs.observed.at(phase_type);
        double wavelength = getWavelength(phase_type, obs.system);
        if (wavelength <= 0.0) {
            return elev_weight;
        }
        double cmc = code - wavelength * phase;

        // Update multipath history
        std::string key = obs.satellite + "_" + type;
        auto& history = multipath_histories_[key];
        history.cmc_values.push_back(cmc);
        history.timestamps.push_back(obs.time);
        if (history.cmc_values.size() > MultipathHistory::WINDOW_SIZE) {
            history.cmc_values.pop_front();
            history.timestamps.pop_front();
        }

        // Check time continuity
        if (history.cmc_values.size() == MultipathHistory::WINDOW_SIZE) {
            for (size_t i = 1; i < history.timestamps.size(); ++i) {
                double dt = history.timestamps[i].tow - history.timestamps[i - 1].tow;
                if (dt > MultipathHistory::TIME_GAP_THRESHOLD) {
                    log_manager_.getLogger()->debug("Time gap too large for {} {}: dt = {} s", obs.satellite, type, dt);
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
            double variance = (sum_sq / n - mean * mean) * n / (n - 1);
            double std_dev = variance > 0.0 ? std::sqrt(variance) : 1e-6;

            // Check CMC residual for multipath
            double cmc_residual = std::abs(cmc - mean);
            if (cmc_residual > MULTIPATH_CMC_THRESHOLD * std_dev) {
                elev_weight *= 0.5; // Down-weight if multipath detected
                log_manager_.getLogger()->debug("Multipath detected for {} {}: CMC residual = {}, threshold = {}",
                    obs.satellite, type, cmc_residual, MULTIPATH_CMC_THRESHOLD * std_dev);
            }
        }

        return elev_weight;
    }

} // namespace GnssProcessor
//#endif