/*
#pragma once

#include "RinexParser.hpp"
#include "GnssConfig.hpp"
#include "LogManager.hpp"
#include <Eigen/Dense>
#include <map>
#include <string>
#include <vector>

namespace GnssProcessor {

    class NetworkProcessor {
    public:
        NetworkProcessor(const std::vector<RinexObservation>& observations, const GnssConfig& config, LogManager& log_manager);
        void process();
        std::map<std::string, double> getCorrections() const;

    private:
        void initialize();
        void setupModel();
        void applySBasis();
        void estimateParameters();
        void generateCorrections();
        Eigen::MatrixXd computeNullSpace(const Eigen::MatrixXd& A);
        Eigen::MatrixXd computeSMatrix(const Eigen::MatrixXd& V);

        std::vector<RinexObservation> observations_;
        const GnssConfig& config_;
        LogManager& log_manager_;
        int n_, m_, f_; // Receivers, satellites, frequencies
        Eigen::MatrixXd A_net_, V_net_, S_net_, A_full_;
        Eigen::VectorXd y_net_, estimableParams_;
        std::map<std::string, double> corrections_;
    };

    class UserProcessor {
    public:
        UserProcessor(const std::vector<RinexObservation>& observations, const std::map<std::string, double>& corrections,
            const Eigen::Vector3d& initial_pos, const GnssConfig& config, LogManager& log_manager);
        void process();
        Eigen::Vector3d getPosition() const;

    private:
        void setupModel();
        void applyCorrections();
        void resolveAmbiguities();
        void estimatePosition();

        std::vector<RinexObservation> observations_;
        std::map<std::string, double> corrections_;
        Eigen::Vector3d position_;
        const GnssConfig& config_;
        LogManager& log_manager_;
        Eigen::MatrixXd A_user_, V_user_, S_user_, A_full_;
        Eigen::VectorXd y_user_;
    };

} // namespace GnssProcessor

//#endif // PPPRTKMODEL_HPP
*/