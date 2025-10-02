#ifndef WLS_LOGGER_HPP
#define WLS_LOGGER_HPP

#include <Eigen/Dense>
#include <fstream>
#include <string>
#include <vector>

namespace GnssProcessor {

    class WlsLogger {
    public:
        WlsLogger(const std::string& filename);
        ~WlsLogger();

        void logStateVector(const Eigen::VectorXd& state, const std::string& label, double tow, int iteration);
        void logCovarianceMatrix(const Eigen::MatrixXd& covariance, const std::string& label, double tow, int iteration);
        void logDesignMatrix(const Eigen::MatrixXd& H, const std::string& label, double tow, int iteration, const std::vector<std::string>& satellite_ids, const std::vector<std::string>& signal_types);
        void logInnovationsVector(const Eigen::VectorXd& z, const std::string& label, double tow, int iteration, const std::vector<std::string>& satellite_ids, const std::vector<std::string>& signal_types);
        void logWeightMatrix(const Eigen::MatrixXd& W, const std::string& label, double tow, int iteration);
        void logNormalMatrix(const Eigen::MatrixXd& HtWH, const std::string& label, double tow, int iteration, double condition_number);

    private:
        std::ofstream log_file_;
        std::string getTimestamp() const;
    };

} // namespace GnssProcessor

#endif // WLS_LOGGER_HPP