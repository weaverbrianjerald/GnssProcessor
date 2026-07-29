/**
 * @file ImuIntegrator.cpp
 * @brief Implementation of IMU strapdown mechanization.
 */

#include "filter/ImuIntegrator.hpp"
#include "common/Constants.hpp"
#include "math/TimeUtils.hpp"
#include <fstream>
#include <sstream>
#include <algorithm>
#include <cmath>

namespace GnssProcessor {

    // ===================================================================
    // Construction
    // ===================================================================

    ImuIntegrator::ImuIntegrator(const GnssConfig& config, LogManager& log_mgr)
        : config_(config), log_manager_(log_mgr) {
        log_manager_.getLogger("imu")->info("ImuIntegrator created");
    }

    void ImuIntegrator::addSample(const ImuSample& sample) {
        // Streaming counterpart to loadData: append in arrival (time) order.
        samples_.push_back(sample);
    }

    // ===================================================================
    // Data loading
    // ===================================================================

    bool ImuIntegrator::loadData(const std::filesystem::path& path) {
        auto logger = log_manager_.getLogger("imu");

        std::ifstream file(path);
        if (!file.is_open()) {
            logger->error("Failed to open IMU file: {}", path.string());
            return false;
        }

        samples_.clear();
        std::string line;
        size_t line_num = 0;

        while (std::getline(file, line)) {
            ++line_num;
            // Skip empty lines and comments.
            if (line.empty() || line[0] == '#' || line[0] == '%') continue;

            std::istringstream iss(line);
            char comma;
            int week;
            double tow, ax, ay, az, wx, wy, wz;

            // Try CSV: week,tow,ax,ay,az,wx,wy,wz
            if (iss >> week >> comma >> tow >> comma
                    >> ax >> comma >> ay >> comma >> az >> comma
                    >> wx >> comma >> wy >> comma >> wz) {
                ImuSample s;
                s.time.week = week;
                s.time.tow = tow;
                s.accel = Eigen::Vector3d(ax, ay, az);
                s.gyro = Eigen::Vector3d(wx, wy, wz);
                samples_.push_back(s);
            } else {
                // Try whitespace-separated format.
                std::istringstream iss2(line);
                if (iss2 >> week >> tow >> ax >> ay >> az >> wx >> wy >> wz) {
                    ImuSample s;
                    s.time.week = week;
                    s.time.tow = tow;
                    s.accel = Eigen::Vector3d(ax, ay, az);
                    s.gyro = Eigen::Vector3d(wx, wy, wz);
                    samples_.push_back(s);
                } else {
                    logger->trace("Skipping unparseable IMU line {}: {}", line_num, line);
                }
            }
        }

        // Sort by time.
        std::sort(samples_.begin(), samples_.end(),
            [](const ImuSample& a, const ImuSample& b) {
                return TimeUtils::continuousGpsSeconds(a.time) <
                       TimeUtils::continuousGpsSeconds(b.time);
            });

        logger->info("Loaded {} IMU samples from {}", samples_.size(), path.string());
        if (!samples_.empty()) {
            logger->info("  Time span: [{}, {}] - [{}, {}]",
                samples_.front().time.week, samples_.front().time.tow,
                samples_.back().time.week, samples_.back().time.tow);
        }
        return !samples_.empty();
    }

    // ===================================================================
    // Strapdown integration
    // ===================================================================

    ImuPrediction ImuIntegrator::integrate(
        const GnssTime& t_start,
        const GnssTime& t_end,
        const Eigen::Vector3d& pos_ecef,
        const Eigen::Vector3d& vel_ecef,
        const Eigen::Matrix3d& C_b_e,
        const Eigen::Vector3d& accel_bias,
        const Eigen::Vector3d& gyro_bias) const {

        auto logger = log_manager_.getLogger("imu");
        ImuPrediction result;
        result.position = pos_ecef;
        result.velocity = vel_ecef;
        result.attitude = C_b_e;

        const double t0 = TimeUtils::continuousGpsSeconds(t_start);
        const double t1 = TimeUtils::continuousGpsSeconds(t_end);

        if (t1 <= t0) {
            logger->warn("integrate: t_end <= t_start ({:.3f} <= {:.3f})", t1, t0);
            return result;
        }

        // Find samples in (t_start, t_end] using the cached hint.
        // Reset hint if it's out of range.
        if (search_hint_ >= samples_.size()) search_hint_ = 0;

        // Binary search for the first sample with time > t0.
        auto time_of = [](const ImuSample& s) {
            return TimeUtils::continuousGpsSeconds(s.time);
        };

        size_t idx_begin = search_hint_;
        // Walk backward if hint is too far.
        while (idx_begin > 0 && time_of(samples_[idx_begin]) > t0 + 1e-6)
            --idx_begin;
        // Walk forward to find first sample > t0.
        while (idx_begin < samples_.size() && time_of(samples_[idx_begin]) <= t0 + 1e-6)
            ++idx_begin;

        if (idx_begin >= samples_.size()) {
            logger->debug("No IMU samples after t_start={:.3f}", t0);
            return result;
        }

        // Collect samples up to t_end.
        size_t idx_end = idx_begin;
        while (idx_end < samples_.size() && time_of(samples_[idx_end]) <= t1 + 1e-6)
            ++idx_end;

        if (idx_end == idx_begin) {
            logger->debug("No IMU samples in ({:.3f}, {:.3f}]", t0, t1);
            return result;
        }

        // Cache for next call.
        search_hint_ = (idx_end > 0) ? idx_end - 1 : 0;

        // --- Mechanization loop ---
        const auto& imu_cfg = config_.getImuConfig();
        Eigen::Vector3d pos = pos_ecef;
        Eigen::Vector3d vel = vel_ecef;
        Eigen::Matrix3d C = C_b_e;

        // Earth rate vector in ECEF (only z-component for ECEF frame).
        const Eigen::Vector3d omega_ie_e(0.0, 0.0, Constants::EARTH_ANGULAR_VELOCITY);
        const Eigen::Matrix3d Omega_ie = skew(omega_ie_e);

        // Error-state transition accumulation (15-state: dpos, dvel, datt, dba, dbg).
        Eigen::MatrixXd Phi_total = Eigen::MatrixXd::Identity(15, 15);
        Eigen::MatrixXd Q_total = Eigen::MatrixXd::Zero(15, 15);

        double prev_time = t0;
        size_t n_integrated = 0;

        for (size_t i = idx_begin; i < idx_end; ++i) {
            const double t_sample = time_of(samples_[i]);
            const double dt = t_sample - prev_time;
            if (dt <= 0.0 || dt > 10.0) {
                // Skip unreasonable time steps.
                prev_time = t_sample;
                continue;
            }

            // Correct for biases.
            const Eigen::Vector3d f_b = samples_[i].accel - accel_bias;
            const Eigen::Vector3d w_b = samples_[i].gyro - gyro_bias;

            // 1. Attitude update: C(k+1) = C(k) * (I + [w_b]*dt) - [omega_ie]*C(k)*dt
            //    Simplified first-order update.
            const Eigen::Matrix3d dR = Eigen::Matrix3d::Identity() + skew(w_b) * dt;
            C = C * dR - Omega_ie * C * dt;

            // Re-orthonormalize via SVD to prevent drift.
            if (n_integrated % 100 == 0) {
                Eigen::JacobiSVD<Eigen::Matrix3d> svd(C, Eigen::ComputeFullU | Eigen::ComputeFullV);
                C = svd.matrixU() * svd.matrixV().transpose();
                if (C.determinant() < 0.0) C.col(2) *= -1.0;
            }

            // 2. Specific force in ECEF.
            const Eigen::Vector3d f_e = C * f_b;

            // 3. Gravity.
            const Eigen::Vector3d g_e = computeGravityEcef(pos);

            // 4. Velocity update: v(k+1) = v(k) + (f_e + g_e - 2*Omega_ie*v)*dt
            const Eigen::Vector3d vel_new = vel + (f_e + g_e - 2.0 * Omega_ie * vel) * dt;

            // 5. Position update (trapezoidal).
            pos = pos + 0.5 * (vel + vel_new) * dt;
            vel = vel_new;

            // --- Error-state matrices for this step ---
            // 15-state: [dp(3), dv(3), dpsi(3), dba(3), dbg(3)]
            //
            // Linearized error dynamics:
            //   F = | 0   I   0      0     0   |
            //       | 0  -2Oe  -[f_e]x  C   0   |
            //       | 0   0   -Oe    0    -C  |
            //       | 0   0    0     0     0   |
            //       | 0   0    0     0     0   |
            Eigen::MatrixXd F_k = Eigen::MatrixXd::Zero(15, 15);
            F_k.block<3,3>(0, 3) = Eigen::Matrix3d::Identity();               // dp/dv
            F_k.block<3,3>(3, 3) = -2.0 * Omega_ie;                           // dv/dv
            F_k.block<3,3>(3, 6) = -skew(f_e);                                // dv/dpsi
            F_k.block<3,3>(3, 9) = C;                                          // dv/dba
            F_k.block<3,3>(6, 6) = -Omega_ie;                                  // dpsi/dpsi
            F_k.block<3,3>(6, 12) = -C;                                        // dpsi/dbg

            // Discrete transition: Phi_k = I + F*dt (first order).
            const Eigen::MatrixXd Phi_k = Eigen::MatrixXd::Identity(15, 15) + F_k * dt;

            // Noise spectral densities.
            Eigen::MatrixXd G_k = Eigen::MatrixXd::Zero(15, 12);
            G_k.block<3,3>(3, 0) = C;                   // accel noise -> velocity
            G_k.block<3,3>(6, 3) = -C;                  // gyro noise -> attitude
            G_k.block<3,3>(9, 6) = Eigen::Matrix3d::Identity();  // accel bias random walk
            G_k.block<3,3>(12, 9) = Eigen::Matrix3d::Identity(); // gyro bias random walk

            Eigen::MatrixXd W = Eigen::MatrixXd::Zero(12, 12);
            const double sa = imu_cfg.accel_noise_density;
            const double sg = imu_cfg.gyro_noise_density;
            const double sba = imu_cfg.accel_bias_instability;
            const double sbg = imu_cfg.gyro_bias_instability;
            W.block<3,3>(0, 0) = Eigen::Matrix3d::Identity() * (sa * sa);
            W.block<3,3>(3, 3) = Eigen::Matrix3d::Identity() * (sg * sg);
            W.block<3,3>(6, 6) = Eigen::Matrix3d::Identity() * (sba * sba);
            W.block<3,3>(9, 9) = Eigen::Matrix3d::Identity() * (sbg * sbg);

            const Eigen::MatrixXd Q_k = G_k * W * G_k.transpose() * dt;

            // Accumulate: Phi_total = Phi_k * Phi_total, Q_total = Phi_k * Q_total * Phi_k^T + Q_k
            Q_total = Phi_k * Q_total * Phi_k.transpose() + Q_k;
            Phi_total = Phi_k * Phi_total;

            prev_time = t_sample;
            ++n_integrated;
        }

        result.position = pos;
        result.velocity = vel;
        result.attitude = C;
        result.Phi = Phi_total;
        result.Q_ins = Q_total;
        result.dt_total = t1 - t0;
        result.num_samples = n_integrated;
        result.valid = (n_integrated > 0);

        logger->debug("IMU integration: {} samples over {:.3f}s, pos_delta={:.3f}m, vel_delta={:.3f}m/s",
            n_integrated, result.dt_total,
            (pos - pos_ecef).norm(), (vel - vel_ecef).norm());

        return result;
    }

    // ===================================================================
    // Gravity model (point-mass radial + ECEF centrifugal; no Somigliana/J2)
    // ===================================================================

    Eigen::Vector3d ImuIntegrator::computeGravityEcef(const Eigen::Vector3d& pos_ecef) {
        const double r = pos_ecef.norm();
        if (r < 1.0) return Eigen::Vector3d::Zero();

        // Simple radial gravity: g = -mu/r^3 * r_ecef + omega^2 * [x, y, 0] (centrifugal)
        const double mu = Constants::GRAVITATIONAL_CONSTANT;
        const double omega = Constants::EARTH_ANGULAR_VELOCITY;

        Eigen::Vector3d g = -(mu / (r * r * r)) * pos_ecef;
        // Add centrifugal term.
        g(0) += omega * omega * pos_ecef(0);
        g(1) += omega * omega * pos_ecef(1);

        return g;
    }

    // ===================================================================
    // Skew-symmetric matrix
    // ===================================================================

    Eigen::Matrix3d ImuIntegrator::skew(const Eigen::Vector3d& v) {
        Eigen::Matrix3d S;
        S <<     0.0, -v(2),  v(1),
              v(2),    0.0, -v(0),
             -v(1),  v(0),    0.0;
        return S;
    }

} // namespace GnssProcessor
