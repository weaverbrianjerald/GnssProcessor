#pragma once

/**
 * @file ImuIntegrator.hpp
 * @brief IMU strapdown mechanization and data management for GNSS/INS integration.
 *
 * The ImuIntegrator reads a CSV file of time-stamped IMU measurements
 * (specific force and angular rate in the body frame) and provides a
 * strapdown integration routine that propagates position, velocity and
 * attitude (as a rotation matrix) between GNSS epochs.
 *
 * The integration is a first-order strapdown mechanization in ECEF:
 *   1. Attitude update:  C_b^e(k+1) = C_b^e(k) * (I + [omega_ib_b] * dt)
 *                         with Earth-rate compensation.
 *   2. Velocity update:  v(k+1) = v(k) + C_b^e * f_b * dt + g * dt
 *                         - 2 * Omega_ie * v * dt   (Coriolis)
 *   3. Position update:  p(k+1) = p(k) + 0.5*(v(k)+v(k+1)) * dt
 *
 * Accelerometer and gyroscope biases are external states in the Kalman
 * filter (held by StateManager); the caller passes them in and they are
 * subtracted from the raw measurements before integration.
 *
 * ## CSV format
 * Each row: `gps_week, gps_tow, ax, ay, az, wx, wy, wz`
 * where a{x,y,z} are specific force in m/s² and w{x,y,z} are angular
 * rate in rad/s, both in the IMU body frame.  Whitespace-separated rows with
 * the same columns are also accepted; empty lines and lines starting with
 * '#' or '%' are skipped.
 */

#include "config/GnssConfig.hpp"
#include "logging/LogManager.hpp"
#include "common/PppTypes.hpp"
#include <Eigen/Dense>
#include <string>
#include <vector>
#include <filesystem>

namespace GnssProcessor {

    /**
     * @brief A single IMU measurement sample.
     */
    struct ImuSample {
        GnssTime time;                   ///< GPS time of the sample.
        Eigen::Vector3d accel;           ///< Specific force in body frame (m/s²).
        Eigen::Vector3d gyro;            ///< Angular rate in body frame (rad/s).
    };

    /**
     * @brief Result of strapdown integration over one GNSS epoch interval.
     *
     * Contains the propagated navigation state and the state transition
     * matrix (Phi) and process noise (Q_ins) needed by the Kalman filter.
     */
    struct ImuPrediction {
        Eigen::Vector3d position;        ///< Propagated ECEF position (m).
        Eigen::Vector3d velocity;        ///< Propagated ECEF velocity (m/s).
        Eigen::Matrix3d attitude;        ///< Body-to-ECEF rotation matrix C_b^e.
        Eigen::MatrixXd Phi;             ///< State transition matrix (15×15 INS error states).
        Eigen::MatrixXd Q_ins;           ///< INS process noise matrix (15×15).
        double dt_total{0.0};            ///< Total integrated time (s).
        size_t num_samples{0};           ///< Number of IMU samples used.
        bool valid{false};               ///< True if integration succeeded.
    };

    /**
     * @class ImuIntegrator
     * @brief Reads IMU data and performs strapdown mechanization.
     *
     * Usage:
     * @code
     *   ImuIntegrator imu(config, log_manager);
     *   imu.loadData(config.getImuConfig().imu_file_path);
     *   // At each GNSS epoch:
     *   auto pred = imu.integrate(t_prev, t_curr, pos, vel, att, accel_bias, gyro_bias);
     * @endcode
     */
    class ImuIntegrator {
    public:
        /**
         * @brief Construct from configuration.
         * @param config Global configuration (reads IMU noise parameters).
         * @param log_mgr Logging manager.
         */
        ImuIntegrator(const GnssConfig& config, LogManager& log_mgr);

        /**
         * @brief Load IMU data from a CSV file.
         *
         * Replaces any samples already held, then sorts the loaded samples by
         * time.
         *
         * @param path Path to the IMU CSV file.
         * @return true if at least one sample was loaded.
         */
        bool loadData(const std::filesystem::path& path);

        /**
         * @brief Check if IMU data is available.
         * @return true if samples have been loaded.
         */
        bool hasData() const { return !samples_.empty(); }

        /**
         * @brief Number of loaded IMU samples.
         */
        size_t sampleCount() const { return samples_.size(); }

        /**
         * @brief Append one streaming IMU sample (real-time ingest).
         *
         * The incremental counterpart to @c loadData: pushes @p sample onto the
         * sample buffer, so a later @c integrate over an interval that contains
         * its time consumes it.  Samples must arrive in non-decreasing time
         * order, because the sample search in @c integrate assumes a sorted
         * buffer and this call does not sort; a live receiver delivers samples
         * in order.  For in-order samples the buffer, and therefore the
         * @c integrate result, is the same as the one @c loadData builds.
         *
         * @param sample IMU sample (body-frame specific force + angular rate).
         */
        void addSample(const ImuSample& sample);

        /**
         * @brief Perform strapdown integration between two GNSS epochs.
         *
         * Integrates all IMU samples in the interval (t_start, t_end] (with a
         * 1e-6 s tolerance) using the provided initial navigation state and
         * sensor biases.  Propagation ends at the last sample in the interval;
         * the state is not extrapolated to @p t_end.  A step whose sample
         * spacing is not positive or exceeds 10 s is skipped.  The result is
         * invalid (unchanged input state) when @p t_end <= @p t_start or no
         * sample is integrated.
         *
         * @param t_start    Start time (exclusive, typically the previous GNSS epoch).
         * @param t_end      End time (inclusive, the current GNSS epoch).
         * @param pos_ecef   Initial ECEF position (m).
         * @param vel_ecef   Initial ECEF velocity (m/s).
         * @param C_b_e      Initial body-to-ECEF rotation matrix.
         * @param accel_bias Accelerometer bias estimate in body frame (m/s²).
         * @param gyro_bias  Gyroscope bias estimate in body frame (rad/s).
         * @return ImuPrediction with propagated state and error-state matrices.
         */
        ImuPrediction integrate(const GnssTime& t_start,
                                const GnssTime& t_end,
                                const Eigen::Vector3d& pos_ecef,
                                const Eigen::Vector3d& vel_ecef,
                                const Eigen::Matrix3d& C_b_e,
                                const Eigen::Vector3d& accel_bias,
                                const Eigen::Vector3d& gyro_bias) const;

        /**
         * @brief Compute the ECEF gravity vector at a position.
         *
         * Point-mass radial gravity plus the ECEF centrifugal term
         * (g = -mu/r^3 * r_ecef + omega^2 * [x, y, 0]).  The Somigliana normal-
         * gravity flattening and the J2 zonal term are not modelled; the residual
         * deterministic acceleration is absorbed by the estimated accel bias.
         * Returns zero for |r| < 1 m.
         *
         * @param pos_ecef ECEF position (m).
         * @return Gravity vector in ECEF (m/s²), pointing roughly toward Earth center.
         */
        static Eigen::Vector3d computeGravityEcef(const Eigen::Vector3d& pos_ecef);

    private:
        const GnssConfig& config_;
        LogManager& log_manager_;
        std::vector<ImuSample> samples_;
        mutable size_t search_hint_{0};    ///< Index where the next sample search starts (last sample used by the previous call).

        /// Skew-symmetric (cross-product) matrix of a 3-vector.
        static Eigen::Matrix3d skew(const Eigen::Vector3d& v);
    };

} // namespace GnssProcessor
