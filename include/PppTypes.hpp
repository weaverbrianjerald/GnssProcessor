// PppTypes.hpp
#pragma once

#include <map>
#include <string>
#include <Eigen/Dense>
#include "TimeUtils.hpp"
#include "RinexParser.hpp"  // for System

namespace GnssProcessor {

    struct PppObservation {
        std::string satellite;
        System system;
        GnssTime time;
        std::map<std::string, double> observed;
        std::map<std::string, double> modeled;
        std::map<std::string, double> residuals;
        std::map<std::string, double> variances;
        std::map<std::string, int> lli_flags;
    };

    struct PppState {
        Eigen::Vector3d position;
        double clock_bias;
        double troposphere_zenith_delay;
        double ionosphere_zenith_delay;
        std::map<std::string, double> ionosphere_delays;
        std::map<std::string, double> ambiguities;
        Eigen::MatrixXd covariance;
        GnssTime epoch_time;
    };

}
