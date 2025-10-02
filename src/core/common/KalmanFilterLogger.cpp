#include "KalmanFilterLogger.hpp"

KalmanFilterLogger::KalmanFilterLogger(const std::string& filename) {
    log_file_.open(filename, std::ios::out | std::ios::trunc);
    if (!log_file_.is_open()) {
        throw std::runtime_error("Unable to open log file: " + filename);
    }
    log_file_ << "=== Kalman Filter Log (" << getTimestamp() << ") ===\n";
}

KalmanFilterLogger::~KalmanFilterLogger() {
    if (log_file_.is_open()) {
        log_file_ << "=== End of Log (" << getTimestamp() << ") ===\n\n";
        log_file_.close();
    }
}

void KalmanFilterLogger::logStateVector(const Eigen::VectorXd& x, const std::string& label) {
    log_file_ << "[" << getTimestamp() << "] " << label << " (" << x.size() << "x1):\n";
    log_file_ << std::fixed << std::setprecision(6);
    for (int i = 0; i < x.size(); ++i) {
        log_file_ << x(i) << "\n";
    }
    log_file_ << "\n";
}

void KalmanFilterLogger::logCovarianceMatrix(const Eigen::MatrixXd& P, const std::string& label) {
    log_file_ << "[" << getTimestamp() << "] " << label << " (" << P.rows() << "x" << P.cols() << "):\n";
    log_file_ << std::fixed << std::setprecision(6);
    for (int i = 0; i < P.rows(); ++i) {
        for (int j = 0; j < P.cols(); ++j) {
            log_file_ << std::setw(12) << P(i, j) << " ";
        }
        log_file_ << "\n";
    }
    log_file_ << "\n";
}

void KalmanFilterLogger::logKalmanGain(const Eigen::MatrixXd& K, const std::string& label) {
    logMatrix(K, label); // Reuse generic matrix logging
}

void KalmanFilterLogger::logMatrix(const Eigen::MatrixXd& matrix, const std::string& label) {
    log_file_ << "[" << getTimestamp() << "] " << label << " (" << matrix.rows() << "x" << matrix.cols() << "):\n";
    log_file_ << std::fixed << std::setprecision(6);
    for (int i = 0; i < matrix.rows(); ++i) {
        for (int j = 0; j < matrix.cols(); ++j) {
            log_file_ << std::setw(12) << matrix(i, j) << " ";
        }
        log_file_ << "\n";
    }
    log_file_ << "\n";
}

void KalmanFilterLogger::logVector(const Eigen::VectorXd& vector, const std::string& label) {
    log_file_ << "[" << getTimestamp() << "] " << label << " (" << vector.size() << "x1):\n";
    log_file_ << std::fixed << std::setprecision(6);
    for (int i = 0; i < vector.size(); ++i) {
        log_file_ << vector(i) << "\n";
    }
    log_file_ << "\n";
}

std::string KalmanFilterLogger::getTimestamp() const {
    std::time_t now = std::time(nullptr);
    char buf[20];
    std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", std::localtime(&now));
    return std::string(buf);
}