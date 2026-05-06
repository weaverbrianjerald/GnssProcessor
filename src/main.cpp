#include "app/Application.hpp"

#include <string>

// Pick a scenario config from argv[1]; fall back to ppp_rtk when run
// with no arguments (matches Visual Studio "Run" with no launch args).
// See config/README.md for the list of scenarios.
int main(int argc, char** argv) {
    const std::string cfg_path = (argc >= 2)
        ? std::string(argv[1])
        : std::string("config/ppp_rtk_internal.json");
    GnssProcessor::Application app(cfg_path);
    return app.run();
}
