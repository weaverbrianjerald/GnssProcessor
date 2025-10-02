#include "WlsLogger.hpp"
#include <iomanip>
#include <ctime>
#include <fmt/format.h>

namespace GnssProcessor {

    WlsLogger::WlsLogger(const std::string& filename) {
        log_file_.open(filename, std::ios::out | std::ios::trunc);
        if (!log_file_.is_open()) {
            throw std::runtime_error("Unable to open WLS log file: " + filename);
        }
        log_file_ << "=== WLS Log (" << getTimestamp() << ") ===\n";
    }

    WlsLogger::~WlsLogger() {
        if (log_file_.is_open()) {
            log_file_ << "=== End of WLS Log (" << getTimestamp() << ") ===\n\n";
            log_file_.close();
        }
    }

    void WlsLogger::logStateVector(const Eigen::VectorXd& state, const std::string& label, double tow, int iteration) {
        log_file_ << "[" << getTimestamp() << "] " << label << " at TOW " << std::fixed << std::setprecision(3) << tow
            << ", Iteration " << iteration << " (" << state.size() << "x1):\n";
        for (Eigen::Index i = 0; i < state.size(); ++i) {
            log_file_ << std::fixed << std::setprecision(6) << state(i) << "\n";
        }
        log_file_ << "\n";
    }

    void WlsLogger::logCovarianceMatrix(const Eigen::MatrixXd& covariance, const std::string& label, double tow, int iteration) {
        log_file_ << "[" << getTimestamp() << "] " << label << " at TOW " << std::fixed << std::setprecision(3) << tow
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

    void WlsLogger::logDesignMatrix(const Eigen::MatrixXd& H, const std::string& label, double tow, int iteration,
        const std::vector<std::string>& satellite_ids, const std::vector<std::string>& signal_types) {
        log_file_ << "[" << getTimestamp() << "] " << label << " at TOW " << std::fixed << std::setprecision(3) << tow
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

    void WlsLogger::logInnovationsVector(const Eigen::VectorXd& z, const std::string& label, double tow, int iteration,
        const std::vector<std::string>& satellite_ids, const std::vector<std::string>& signal_types) {
        log_file_ << "[" << getTimestamp() << "] " << label << " at TOW " << std::fixed << std::setprecision(3) << tow
            << ", Iteration " << iteration << " (" << z.size() << "x1):\n";
        for (Eigen::Index i = 0; i < z.size(); ++i) {
            log_file_ << "Sat " << satellite_ids[i] << " (" << signal_types[i] << "): "
                << std::fixed << std::setprecision(6) << z(i) << "\n";
        }
        log_file_ << "\n";
    }

    void WlsLogger::logWeightMatrix(const Eigen::MatrixXd& W, const std::string& label, double tow, int iteration) {
        log_file_ << "[" << getTimestamp() << "] " << label << " at TOW " << std::fixed << std::setprecision(3) << tow
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

    void WlsLogger::logNormalMatrix(const Eigen::MatrixXd& HtWH, const std::string& label, double tow, int iteration, double condition_number) {
        log_file_ << "[" << getTimestamp() << "] " << label << " at TOW " << std::fixed << std::setprecision(3) << tow
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

    std::string WlsLogger::getTimestamp() const {
        std::time_t now = std::time(nullptr);
        char buf[20];
        std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", std::localtime(&now));
        return std::string(buf);
    }

} // namespace GnssProcessor