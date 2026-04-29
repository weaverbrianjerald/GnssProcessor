/**
 * @file WlsLogger.cpp
 * @brief Implementation of WlsLogger: writes WLS-related vectors and matrices to a text log.
 *
 * The implementation provides human-readable output with timestamps and optional epoch metadata.
 * It complements the declaration in WlsLogger.hpp.
 */

#include "logging/WlsLogger.hpp"
#include <iomanip>
#include <ctime>
#include <fmt/format.h>

namespace {
    /**
     * @brief Write the common log entry prefix containing wall clock time and optional epoch info.
     * @param os Output stream to write the prefix to.
     * @param wall_time Wall-clock timestamp string (typically produced by `WlsLogger::getTimestamp()`).
     * @param epoch_info Optional epoch metadata string that, when non-empty, is appended in brackets.
     *
     * This helper is internal to the translation unit and is declared in an anonymous namespace.
     */
    inline void writePrefix(std::ofstream& os, const std::string& wall_time, const std::string& epoch_info) {
        os << "[" << wall_time << "]";
        if (!epoch_info.empty()) {
            os << " [" << epoch_info << "]";
        }
        os << " ";
    }
}

namespace GnssProcessor {

    /**
     * @brief Construct a new WlsLogger object and open the log file.
     * @param filename Path to the log file to open. The file is opened for writing and truncated.
     * @throws std::runtime_error if the file cannot be opened.
     *
     * On successful open, a header line with the creation timestamp is written to the file.
     */
    WlsLogger::WlsLogger(const std::string& filename) {
        log_file_.open(filename, std::ios::out | std::ios::trunc);
        if (!log_file_.is_open()) {
            throw std::runtime_error("Unable to open WLS log file: " + filename);
        }
        log_file_ << "=== WLS Log (" << getTimestamp() << ") ===\n";
    }

    /**
     * @brief Destroy the WlsLogger object and close the log file.
     *
     * Writes a footer line with the closing timestamp and closes the underlying file stream.
     * Safe to call even if the stream is already closed.
     */
    WlsLogger::~WlsLogger() {
        if (log_file_.is_open()) {
            log_file_ << "=== End of WLS Log (" << getTimestamp() << ") ===\n\n";
            log_file_.close();
        }
    }

    /**
     * @brief Log a state vector to the file.
     * @param state Vector of state values to log.
     * @param label Short label identifying the logged item (e.g., "state_pre" / "state_post").
     * @param tow Time-of-week (seconds) associated with the epoch.
     * @param iteration Iteration index of the solver for which this state was produced.
     * @param epoch_info Optional free-form epoch metadata to include in the prefix.
     *
     * The vector is logged one element per line with fixed formatting (6 decimal places).
     */
    void WlsLogger::logStateVector(const Eigen::VectorXd& state, const std::string& label, double tow, int iteration,
        const std::string& epoch_info) {
        writePrefix(log_file_, getTimestamp(), epoch_info);
        log_file_ << label << " at TOW " << std::fixed << std::setprecision(3) << tow
            << ", Iteration " << iteration << " (" << state.size() << "x1):\n";
        for (Eigen::Index i = 0; i < state.size(); ++i) {
            log_file_ << std::fixed << std::setprecision(6) << state(i) << "\n";
        }
        log_file_ << "\n";
    }

    /**
     * @brief Log a covariance matrix to the file.
     * @param covariance Covariance matrix to log (rows x cols).
     * @param label Short label identifying the logged item.
     * @param tow Time-of-week (seconds) associated with the epoch.
     * @param iteration Iteration index of the solver.
     * @param epoch_info Optional free-form epoch metadata to include in the prefix.
     *
     * Each matrix row is logged on a separate line; columns are separated by spaces.
     */
    void WlsLogger::logCovarianceMatrix(const Eigen::MatrixXd& covariance, const std::string& label, double tow, int iteration,
        const std::string& epoch_info) {
        writePrefix(log_file_, getTimestamp(), epoch_info);
        log_file_ << label << " at TOW " << std::fixed << std::setprecision(3) << tow
            << ", Iteration " << iteration << " (" << covariance.rows() << "x" << covariance.cols() << "):\n";
        for (Eigen::Index i = 0; i < covariance.rows(); ++i) {
            for (Eigen::Index j = 0; j < covariance.cols(); ++j) {
                log_file_ << std::fixed << std::setprecision(6) << covariance(i, j);
                if (j < covariance.cols() - 1) log_file_ << " ";
            }
            log_file_ << "\n";
        }
        log_file_ << "\n";
    }

    /**
     * @brief Log the design matrix (H) with associated satellite and signal identifiers.
     * @param H Design matrix (observations x parameters).
     * @param label Short label identifying the logged item.
     * @param tow Time-of-week (seconds) associated with the epoch.
     * @param iteration Iteration index of the solver.
     * @param satellite_ids Vector of satellite identifiers; expected length == H.rows().
     * @param signal_types Vector of signal type strings; expected length == H.rows().
     * @param epoch_info Optional free-form epoch metadata to include in the prefix.
     *
     * Each observation row is prefixed with its satellite id and signal type.
     */
    void WlsLogger::logDesignMatrix(const Eigen::MatrixXd& H, const std::string& label, double tow, int iteration,
        const std::vector<std::string>& satellite_ids, const std::vector<std::string>& signal_types,
        const std::string& epoch_info) {
        writePrefix(log_file_, getTimestamp(), epoch_info);
        log_file_ << label << " at TOW " << std::fixed << std::setprecision(3) << tow
            << ", Iteration " << iteration << " (" << H.rows() << "x" << H.cols() << "):\n";
        for (Eigen::Index i = 0; i < H.rows(); ++i) {
            log_file_ << "Sat " << satellite_ids[i] << " (" << signal_types[i] << "): ";
            for (Eigen::Index j = 0; j < H.cols(); ++j) {
                log_file_ << std::fixed << std::setprecision(6) << H(i, j);
                if (j < H.cols() - 1) log_file_ << " ";
            }
            log_file_ << "\n";
        }
        log_file_ << "\n";
    }

    /**
     * @brief Log the innovations (residuals) vector with satellite and signal identifiers.
     * @param z Innovations vector to log.
     * @param label Short label identifying the logged item.
     * @param tow Time-of-week (seconds) associated with the epoch.
     * @param iteration Iteration index of the solver.
     * @param satellite_ids Vector of satellite identifiers; expected length == z.size().
     * @param signal_types Vector of signal type strings; expected length == z.size().
     * @param epoch_info Optional free-form epoch metadata to include in the prefix.
     *
     * Each innovation is logged on its own line with the corresponding satellite and signal.
     */
    void WlsLogger::logInnovationsVector(const Eigen::VectorXd& z, const std::string& label, double tow, int iteration,
        const std::vector<std::string>& satellite_ids, const std::vector<std::string>& signal_types,
        const std::string& epoch_info) {
        writePrefix(log_file_, getTimestamp(), epoch_info);
        log_file_ << label << " at TOW " << std::fixed << std::setprecision(3) << tow
            << ", Iteration " << iteration << " (" << z.size() << "x1):\n";
        for (Eigen::Index i = 0; i < z.size(); ++i) {
            log_file_ << "Sat " << satellite_ids[i] << " (" << signal_types[i] << "): "
                << std::fixed << std::setprecision(6) << z(i) << "\n";
        }
        log_file_ << "\n";
    }

    /**
     * @brief Log the weight matrix (W) used by the WLS formulation.
     * @param W Weight matrix to log.
     * @param label Short label identifying the logged item.
     * @param tow Time-of-week (seconds) associated with the epoch.
     * @param iteration Iteration index of the solver.
     * @param epoch_info Optional free-form epoch metadata to include in the prefix.
     *
     * Matrix rows are written one per line with columns separated by spaces.
     */
    void WlsLogger::logWeightMatrix(const Eigen::MatrixXd& W, const std::string& label, double tow, int iteration,
        const std::string& epoch_info) {
        writePrefix(log_file_, getTimestamp(), epoch_info);
        log_file_ << label << " at TOW " << std::fixed << std::setprecision(3) << tow
            << ", Iteration " << iteration << " (" << W.rows() << "x" << W.cols() << "):\n";
        for (Eigen::Index i = 0; i < W.rows(); ++i) {
            for (Eigen::Index j = 0; j < W.cols(); ++j) {
                log_file_ << std::fixed << std::setprecision(6) << W(i, j);
                if (j < W.cols() - 1) log_file_ << " ";
            }
            log_file_ << "\n";
        }
        log_file_ << "\n";
    }

    /**
     * @brief Log the normal matrix (H^T * W * H) along with its condition number.
     * @param HtWH Normal matrix to log.
     * @param label Short label identifying the logged item.
     * @param tow Time-of-week (seconds) associated with the epoch.
     * @param iteration Iteration index of the solver.
     * @param condition_number Condition number of the normal matrix for diagnostics.
     * @param epoch_info Optional free-form epoch metadata to include in the prefix.
     *
     * The condition number is written in scientific notation; the matrix is logged row-by-row.
     */
    void WlsLogger::logNormalMatrix(const Eigen::MatrixXd& HtWH, const std::string& label, double tow, int iteration,
        double condition_number, const std::string& epoch_info) {
        writePrefix(log_file_, getTimestamp(), epoch_info);
        log_file_ << label << " at TOW " << std::fixed << std::setprecision(3) << tow
            << ", Iteration " << iteration << " (" << HtWH.rows() << "x" << HtWH.cols() << "), Condition Number: "
            << std::scientific << std::setprecision(3) << condition_number << ":\n";
        for (Eigen::Index i = 0; i < HtWH.rows(); ++i) {
            for (Eigen::Index j = 0; j < HtWH.cols(); ++j) {
                log_file_ << std::fixed << std::setprecision(6) << HtWH(i, j);
                if (j < HtWH.cols() - 1) log_file_ << " ";
            }
            log_file_ << "\n";
        }
        log_file_ << "\n";
    }

    /**
     * @brief Produce a wall-clock timestamp string in "YYYY-MM-DD HH:MM:SS" format.
     * @return Formatted timestamp string suitable for log prefixes.
     *
     * Uses std::localtime and std::strftime; format size is fixed to 19 printable chars.
     */
    std::string WlsLogger::getTimestamp() const {
        std::time_t now = std::time(nullptr);
        char buf[20];
        std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", std::localtime(&now));
        return std::string(buf);
    }

} // namespace GnssProcessor