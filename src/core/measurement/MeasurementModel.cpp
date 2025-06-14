#include "MeasurementModel.hpp"
#include "RinexParser.hpp"
#include <stdexcept>

namespace GnssProcessor {

    MeasurementModel::MeasurementModel(const StateManager& state_manager, const PppModel& ppp_model, const GnssConfig& config, LogManager& log_manager)
        : state_manager_(state_manager), ppp_model_(ppp_model), config_(config), log_manager_(log_manager) {
        log_manager_.getLogger()->info("MeasurementModel initialized with {} signal priority sets.", config_.getSignalPriorities().size());
    }

    EpochMeasurement MeasurementModel::computeMeasurementUpdate(const std::vector<PppObservation>& observations,
        const Eigen::VectorXd& current_state) {
        if (observations.empty()) {
            log_manager_.getLogger()->error("No observations provided for measurement update");
            return EpochMeasurement{};
        }

        int num_obs = 0;
        for (const auto& obs : observations) {
            num_obs += obs.observed.size();
        }

        int state_size = state_manager_.getStateSize();
        Eigen::VectorXd innovations(num_obs);
        Eigen::MatrixXd H(num_obs, state_size);
        H.setZero();
        Eigen::VectorXd R_v(num_obs);

        int row = 0;
        for (const auto& obs : observations) {
            Eigen::Vector3d sat_pos = ppp_model_.getSatellitePosition(obs.satellite, obs.time);
            if (sat_pos.norm() == 0.0) {
                log_manager_.getLogger()->warn("No satellite position for {} at time {}", obs.satellite, obs.time.tow);
                continue;
            }

            Eigen::Vector3d rx_pos = current_state.segment<3>(state_manager_.getPositionIndex());
            double elevation = ppp_model_.computeElevation(rx_pos, sat_pos);
            double range = ppp_model_.computeGeometricRange(rx_pos, sat_pos);
            Eigen::Vector3d los = (sat_pos - rx_pos).normalized();
            double mapping = 1.0 / (std::sin(elevation * Constants::PI / 180.0) + 0.00143 / (std::tan(elevation * Constants::PI / 180.0) + 0.0445));

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
            auto sig_it = config_.getSignalPriorities().find(system_str);
            if (sig_it == config_.getSignalPriorities().end()) {
                log_manager_.getLogger()->warn("No signal priorities for system: {}", system_str);
                continue;
            }
            const auto& signals = sig_it->second;

            bool is_triple = signals.size() >= 3 && obs.observed.count(signals[0]) && obs.observed.count(signals[1]) && obs.observed.count(signals[2]);
            bool is_dual = !is_triple && signals.size() >= 2 && obs.observed.count(signals[0]) && obs.observed.count(signals[1]);

            for (const auto& [type, value] : obs.observed) {
                double predicted = 0.0;
                Eigen::Vector3d d_pred_d_pos = Eigen::Vector3d::Zero();
                double d_pred_d_clock = 0.0;
                double d_pred_d_tropo = 0.0;
                double d_pred_d_iono = 0.0;
                double d_pred_d_amb = 0.0;
                std::string amb_key = obs.satellite + "_" + type;

                predicted = range + current_state(state_manager_.getClockIndex()) * Constants::SPEED_OF_LIGHT +
                    current_state(state_manager_.getTropoIndex()) * mapping;
                d_pred_d_pos = -los;
                d_pred_d_clock = Constants::SPEED_OF_LIGHT;
                d_pred_d_tropo = mapping;

                double iono_delay = ppp_model_.computeIonosphericDelay(obs.time, rx_pos, sat_pos, type, obs.system, obs.observed);
                predicted += iono_delay;

                if (!is_dual && !is_triple && type[0] == 'C') {
                    double freq = getFrequency(type, obs.system);
                    if (freq > 0.0) {
                        double f_L1 = Constants::GPS_L1_FREQ;
                        d_pred_d_iono = (f_L1 * f_L1) / (freq * freq);
                    }
                }

                if (type[0] == 'L') {
                    auto amb_idx = state_manager_.getAmbiguityIndex(amb_key);
                    if (amb_idx) {
                        predicted += current_state(*amb_idx);
                        d_pred_d_amb = 1.0;
                    }
                    else {
                        log_manager_.getLogger()->warn("Ambiguity key {} not found for phase observation", amb_key);
                        continue;
                    }
                }
                else if (type[0] != 'C') {
                    log_manager_.getLogger()->warn("Unsupported observation type: {}", type);
                    continue;
                }

                double antenna_corr = ppp_model_.getAntennaCorrection(obs.satellite, type, obs.system, rx_pos, sat_pos, elevation, 0.0, "", obs.time);
                double bias_corr = ppp_model_.getBiasCorrection(obs.satellite, type, obs.time);
                predicted += antenna_corr + bias_corr;

                innovations(row) = value - predicted;
                H(row, state_manager_.getPositionIndex() + 0) = d_pred_d_pos(0);
                H(row, state_manager_.getPositionIndex() + 1) = d_pred_d_pos(1);
                H(row, state_manager_.getPositionIndex() + 2) = d_pred_d_pos(2);
                H(row, state_manager_.getClockIndex()) = d_pred_d_clock;
                H(row, state_manager_.getTropoIndex()) = d_pred_d_tropo;
                if (d_pred_d_iono != 0.0) {
                    H(row, state_manager_.getIonoIndex()) = d_pred_d_iono;
                }
                if (!amb_key.empty()) {
                    auto amb_idx = state_manager_.getAmbiguityIndex(amb_key);
                    if (amb_idx) H(row, *amb_idx) = d_pred_d_amb;
                }

                R_v(row) = obs.variances.count(type) && obs.variances.at(type) > 0 ? obs.variances.at(type) :
                    (type[0] == 'C' ? 0.01 : 0.0001);
                if (!obs.variances.count(type)) {
                    log_manager_.getLogger()->warn("No variance for {} in observation for {}, using default: {}", type, obs.satellite, R_v(row));
                }

                row++;
            }
        }

        if (row != num_obs) {
            log_manager_.getLogger()->warn("Processed {} observations out of {} expected.", row, num_obs);
            innovations.conservativeResize(row);
            H.conservativeResize(row, state_size);
            R_v.conservativeResize(row);
        }

        EpochMeasurement measurement_result;
        measurement_result.innovations = innovations;
        measurement_result.H = H;
        measurement_result.R_v = R_v;
        measurement_result.time = observations.front().time;
        measurement_result.observations = observations;
        log_manager_.getLogger()->debug("Computed measurement update: H ({}x{}), innovations ({}), R_v ({}).",
            H.rows(), H.cols(), innovations.size(), R_v.size());
        return measurement_result;
    }

    double MeasurementModel::getFrequency(const std::string& freq_type, System system) const {
        if (system == System::GPS) {
            if (freq_type.find("L1") != std::string::npos) return Constants::GPS_L1_FREQ;
            if (freq_type.find("L2") != std::string::npos) return Constants::GPS_L2_FREQ;
            if (freq_type.find("L5") != std::string::npos) return Constants::GPS_L5_FREQ;
        }
        else if (system == System::Galileo) {
            if (freq_type.find("E1") != std::string::npos) return Constants::GAL_E1_FREQ;
            if (freq_type.find("E5a") != std::string::npos) return Constants::GAL_E5A_FREQ;
            if (freq_type.find("E5b") != std::string::npos) return Constants::GAL_E5B_FREQ;
        }
        else if (system == System::GLONASS) {
            if (freq_type.find("G1") != std::string::npos) return Constants::GLONASS_G1_BASE_FREQ;
            if (freq_type.find("G2") != std::string::npos) return Constants::GLONASS_G2_BASE_FREQ;
        }
        else if (system == System::BeiDou) {
            if (freq_type.find("B1I") != std::string::npos) return Constants::BDS_B1I_FREQ;
            if (freq_type.find("B2I") != std::string::npos) return Constants::BDS_B2I_FREQ;
            if (freq_type.find("B3I") != std::string::npos) return Constants::BDS_B3I_FREQ;
        }
        else if (system == System::QZSS) {
            if (freq_type.find("L1") != std::string::npos) return Constants::QZSS_L1_FREQ;
            if (freq_type.find("L2") != std::string::npos) return Constants::QZSS_L2_FREQ;
            if (freq_type.find("L5") != std::string::npos) return Constants::QZSS_L5_FREQ;
        }
        else if (system == System::SBAS) {
            if (freq_type.find("L1") != std::string::npos) return Constants::SBAS_L1_FREQ;
            if (freq_type.find("L5") != std::string::npos) return Constants::SBAS_L5_FREQ;
        }
        log_manager_.getLogger()->warn("Unknown frequency for {} on system {}", freq_type, static_cast<int>(system));
        return 0.0;
    }

} // namespace GnssProcessor