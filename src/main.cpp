#include "app/Application.hpp"

int main() {
    GnssProcessor::Application app("config/gnss_config.json");
    return app.run();
}