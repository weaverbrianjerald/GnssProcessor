#pragma once

#include "GnssConfig.hpp"
#include "LogManager.hpp"
#include "ProductParser.hpp"
#include "RinexParser.hpp"
#include "StateManager.hpp"
#include "PppModel.hpp"
#include "MeasurementModel.hpp"
#include "KalmanFilter.hpp"
#include "PppTypes.hpp"
#include <filesystem>
#include <string>

namespace GnssProcessor {

class Application {
public:
    explicit Application(const std::filesystem::path& config_path = "config/gnss_config.json");
    int run();

private:
    std::filesystem::path config_path_;
};

} // namespace GnssProcessor