#include "app/Application.hpp"

#include <string>

// The scenario config path is argv[1]. With no arguments (for example, a Visual Studio
// launch without arguments), config/ppp_rtk_internal.json is used.
// config/README.md lists the scenarios.
int main(int argc, char** argv) {
    const std::string cfg_path = (argc >= 2)
        ? std::string(argv[1])
        : std::string("config/ppp_rtk_internal.json");
    GnssProcessor::Application app(cfg_path);
    return app.run();
}
