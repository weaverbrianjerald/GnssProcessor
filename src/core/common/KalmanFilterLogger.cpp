/**
 * @file KalmanFilterLogger.cpp
 * @brief Implementation of KalmanFilterLogger for logging Kalman filter internals.
 *
 * This file contains helpers to write human-readable matrices and vectors to a
 * plain text log file. Each logged block is prefixed with a wall-clock timestamp
 * and optional epoch/context information.
 */

#include "KalmanFilterLogger.hpp"

namespace {
    /**
     * @brief Write the log entry prefix containing wall-clock time and optional epoch info.
     *
     * The prefix format is:
     *   [YYYY-MM-DD HH:MM:SS] [epoch_info] 
     *
     * @param os Output stream to write the prefix to.
     * @param wall_time Preformatted wall-clock time string.
     * @param epoch_info Optional epoch/context string; omitted when empty.
     */
    inline void writePrefix(std::ofstream& os, const std::string& wall_time, const std::string& epoch_info) {
        os << "[" << wall_time << "]";
        if (!epoch_info.empty()) {
            os << " [" << epoch_info << "]";
        }
        os << " ";
    }
} // anonymous namespace

namespace GnssProcessor {

KalmanFilterLogger::KalmanFilterLogger(const std::string& filename) {
    log_file_.open(filename, std::ios::out | std::ios::trunc);
    if (!log_file_.is_open()) {
        throw std::runtime_error("Unable to open log file: " + filename);
    }
    log_file_ << "=== Kalman Filter Log (" << getTimestamp() << ") ===\n";
}

/**
 * @brief Destructor: writes a closing banner and closes the log file if open.
 *
 * Ensures the file is properly closed and a final timestamped footer is written.
 */
KalmanFilterLogger::~KalmanFilterLogger() {
    if (log_file_.is_open()) {
        log_file_ << "=== End of Log (" << getTimestamp() << ") ===\n\n";
        log_file_.close();
    }
}

/**
 * @brief Log a column state vector to the log file.
 *
 * Writes a prefixed labeled block containing each element of the vector on its
 * own line with fixed precision.
 *
 * @param x Vector to log (Mx1).
 * @param label Human-readable label for this vector (defaults to "State Vector").
 * @param epoch_info Optional epoch or context string to include in the prefix.
 */
void KalmanFilterLogger::logStateVector(const Eigen::VectorXd& x, const std::string& label,
    const std::string& epoch_info) {
    writePrefix(log_file_, getTimestamp(), epoch_info);
    log_file_ << label << " (" << x.size() << "x1):\n";
    log_file_ << std::fixed << std::setprecision(6);
    for (int i = 0; i < x.size(); ++i) {
        log_file_ << x(i) << "\n";
    }
    log_file_ << "\n";
}

/**
 * @brief Log a matrix (covariance or similar) to the log file.
 *
 * The matrix is written row-by-row with each element formatted in a fixed-width
 * column for easier human inspection.
 *
 * @param P Matrix to log.
 * @param label Human-readable label for this matrix (defaults to "Covariance Matrix").
 * @param epoch_info Optional epoch or context string to include in the prefix.
 */
void KalmanFilterLogger::logCovarianceMatrix(const Eigen::MatrixXd& P, const std::string& label,
    const std::string& epoch_info) {
    writePrefix(log_file_, getTimestamp(), epoch_info);
    log_file_ << label << " (" << P.rows() << "x" << P.cols() << "):\n";
    log_file_ << std::fixed << std::setprecision(6);
    for (int i = 0; i < P.rows(); ++i) {
        for (int j = 0; j < P.cols(); ++j) {
            log_file_ << std::setw(12) << P(i, j) << " ";
        }
        log_file_ << "\n";
    }
    log_file_ << "\n";
}

/**
 * @brief Log the Kalman gain matrix.
 *
 * This implementation forwards to the generic matrix logger.
 *
 * @param K Kalman gain matrix to log.
 * @param label Human-readable label for this matrix (defaults to "Kalman Gain").
 * @param epoch_info Optional epoch or context string to include in the prefix.
 */
void KalmanFilterLogger::logKalmanGain(const Eigen::MatrixXd& K, const std::string& label,
    const std::string& epoch_info) {
    logMatrix(K, label, epoch_info); // Reuse generic matrix logging
}

/**
 * @brief Generic matrix logger used by other helpers.
 *
 * Writes the matrix with a prefixed header and fixed precision formatting.
 *
 * @param matrix Matrix to log.
 * @param label Human-readable label for the matrix.
 * @param epoch_info Optional epoch or context string to include in the prefix.
 */
void KalmanFilterLogger::logMatrix(const Eigen::MatrixXd& matrix, const std::string& label,
    const std::string& epoch_info) {
    writePrefix(log_file_, getTimestamp(), epoch_info);
    log_file_ << label << " (" << matrix.rows() << "x" << matrix.cols() << "):\n";
    log_file_ << std::fixed << std::setprecision(6);
    for (int i = 0; i < matrix.rows(); ++i) {
        for (int j = 0; j < matrix.cols(); ++j) {
            log_file_ << std::setw(12) << matrix(i, j) << " ";
        }
        log_file_ << "\n";
    }
    log_file_ << "\n";
}

/**
 * @brief Generic vector logger used by other helpers.
 *
 * Writes each element of the vector on its own line with a labeled header.
 *
 * @param vector Vector to log (Mx1).
 * @param label Human-readable label for the vector.
 * @param epoch_info Optional epoch or context string to include in the prefix.
 */
void KalmanFilterLogger::logVector(const Eigen::VectorXd& vector, const std::string& label,
    const std::string& epoch_info) {
    writePrefix(log_file_, getTimestamp(), epoch_info);
    log_file_ << label << " (" << vector.size() << "x1):\n";
    log_file_ << std::fixed << std::setprecision(6);
    for (int i = 0; i < vector.size(); ++i) {
        log_file_ << vector(i) << "\n";
    }
    log_file_ << "\n";
}

/**
 * @brief Produce a human-readable timestamp string for log prefixes.
 *
 * The format is "YYYY-MM-DD HH:MM:SS" in local time.
 *
 * @return std::string Formatted timestamp.
 */
std::string KalmanFilterLogger::getTimestamp() const {
    std::time_t now = std::time(nullptr);
    char buf[20];
    std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", std::localtime(&now));
    return std::string(buf);
}

} // namespace GnssProcessor