#pragma once

/**
 * @file KalmanFilterLogger.hpp
 * @brief Lightweight logger for Kalman filter matrices and vectors used for debugging.
 *
 * The KalmanFilterLogger writes human-readable records to a text file. It is
 * intended for development and debugging to inspect state vectors, covariance
 * matrices, Kalman gain matrices and other matrices/vectors produced by the
 * filter. Each entry is annotated with an optional label and epoch information.
 *
 * The logger is simple and not thread-safe; it is designed for controlled
 * diagnostic use rather than production telemetry.
 */

#include <fstream>
#include <string>
#include <Eigen/Dense>
#include <iomanip>
#include <ctime>

namespace GnssProcessor {
    /**
     * @class KalmanFilterLogger
     * @brief Provides file-based logging utilities for Kalman filter internals.
     *
     * The class opens a file specified at construction and provides methods to log
     * vectors and matrices with textual labels and optional epoch/context strings.
     * Logged entries are prefixed with a timestamp produced by @c getTimestamp().
     */
    class KalmanFilterLogger {
    public:
        /**
         * @brief Construct a KalmanFilterLogger that writes to @p filename.
         * @param filename Path to the text file where logs will be written.
         *
         * The constructor opens the file in append mode. If opening fails the
         * ensuing log calls will be no-ops (the stream should be checked by the caller
         * if needed).
         */
        KalmanFilterLogger(const std::string& filename);

        /**
         * @brief Close the log file and release resources.
         */
        ~KalmanFilterLogger();

        /**
         * @brief Log a state vector with an optional label and epoch info.
         *
         * The vector is formatted with fixed precision and written as a single
         * labeled block to the log file.
         *
         * @param x State vector to log.
         * @param label Human-readable label describing the vector (default: "State Vector").
         * @param epoch_info Optional textual epoch or context information appended to the entry.
         */
        void logStateVector(const Eigen::VectorXd& x, const std::string& label = "State Vector",
            const std::string& epoch_info = {});

        /**
         * @brief Log a covariance matrix with an optional label and epoch info.
         *
         * The matrix is written row-by-row with fixed precision. The label and
         * epoch_info are written as a header for the matrix block.
         *
         * @param P Covariance matrix to log.
         * @param label Human-readable label describing the matrix (default: "Covariance Matrix").
         * @param epoch_info Optional textual epoch or context information appended to the entry.
         */
        void logCovarianceMatrix(const Eigen::MatrixXd& P, const std::string& label = "Covariance Matrix",
            const std::string& epoch_info = {});

        /**
         * @brief Log the Kalman gain matrix with an optional label and epoch info.
         *
         * @param K Kalman gain matrix to log.
         * @param label Human-readable label describing the matrix (default: "Kalman Gain").
         * @param epoch_info Optional textual epoch or context information appended to the entry.
         */
        void logKalmanGain(const Eigen::MatrixXd& K, const std::string& label = "Kalman Gain",
            const std::string& epoch_info = {});

        /**
         * @brief Log a generic matrix with a label and optional epoch info.
         *
         * Use this method to log any matrix not covered by the dedicated helpers.
         *
         * @param matrix Matrix to log.
         * @param label Human-readable label describing the matrix.
         * @param epoch_info Optional textual epoch or context information appended to the entry.
         */
        void logMatrix(const Eigen::MatrixXd& matrix, const std::string& label,
            const std::string& epoch_info = {});

        /**
         * @brief Log a generic vector with a label and optional epoch info.
         *
         * Use this method to log any vector not covered by the dedicated helpers.
         *
         * @param vector Vector to log.
         * @param label Human-readable label describing the vector.
         * @param epoch_info Optional textual epoch or context information appended to the entry.
         */
        void logVector(const Eigen::VectorXd& vector, const std::string& label,
            const std::string& epoch_info = {});

    private:
        std::ofstream log_file_; /**< Output file stream used for logging. */

        /**
         * @brief Produce a human-readable timestamp string for log entries.
         * @return std::string Timestamp in local time, e.g. "2026-01-21 15:04:05".
         *
         * This helper is used internally to prefix log blocks with a consistent time.
         */
        std::string getTimestamp() const;
    };
} // namespace GnssProcessor
