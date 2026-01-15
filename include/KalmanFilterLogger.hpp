#ifndef KALMAN_FILTER_LOGGER_HPP
#define KALMAN_FILTER_LOGGER_HPP

#include <fstream>
#include <string>
#include <Eigen/Dense>
#include <iomanip>
#include <ctime>

class KalmanFilterLogger {
public:
    KalmanFilterLogger(const std::string& filename);
    ~KalmanFilterLogger();

    // Log state vector
    void logStateVector(const Eigen::VectorXd& x, const std::string& label = "State Vector",
        const std::string& epoch_info = {});

    // Log covariance matrix
    void logCovarianceMatrix(const Eigen::MatrixXd& P, const std::string& label = "Covariance Matrix",
        const std::string& epoch_info = {});

    // Log Kalman gain matrix
    void logKalmanGain(const Eigen::MatrixXd& K, const std::string& label = "Kalman Gain",
        const std::string& epoch_info = {});

    // Log generic matrix
    void logMatrix(const Eigen::MatrixXd& matrix, const std::string& label,
        const std::string& epoch_info = {});

    // Log generic vector
    void logVector(const Eigen::VectorXd& vector, const std::string& label,
        const std::string& epoch_info = {});

private:
    std::ofstream log_file_;
    std::string getTimestamp() const;
};

#endif // KALMAN_FILTER_LOGGER_HPP