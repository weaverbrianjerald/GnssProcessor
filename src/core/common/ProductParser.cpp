#include "ProductParser.hpp"
#include "Constants.hpp"
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <netcdf.h>
#include <algorithm>
#include <iomanip>

namespace GnssProcessor {

    ProductParser::ProductParser(const GnssConfig& config, LogManager& log_manager)
        : config_(config), log_manager_(log_manager) {
        log_manager_.getLogger()->info("ProductParser initialized.");
    }

    bool ProductParser::parseSp3(const std::filesystem::path& sp3_path, std::vector<OrbitData>& orbits) {
        try {
            std::ifstream file(sp3_path);
            if (!file.is_open()) {
                log_manager_.getLogger()->error("Failed to open SP3 file: {}", sp3_path.string());
                return false;
            }

            orbits.clear();
            std::string line;
            bool in_header = true;
            int epoch_interval = 0; // Seconds, from header
            std::vector<std::string> satellite_ids;

            // Parse header
            while (std::getline(file, line) && in_header) {
                if (line.empty()) continue;
                if (line[0] == '#') {
                    // Version and type
                    if (line[1] == 'c' || line[1] == 'd') {
                        std::string file_type = line.substr(9, 3);
                        if (file_type != "ORB") {
                            log_manager_.getLogger()->warn("SP3 file type is {}, expected ORB", file_type);
                        }
                    }
                }
                else if (line.substr(0, 2) == "+ ") {
                    // Number of satellites
                    int num_sats = std::stoi(line.substr(3, 3));
                    satellite_ids.clear();
                    // Read satellite IDs (continuation lines start with "++")
                    while (std::getline(file, line) && (line.substr(0, 2) == "+ " || line.substr(0, 2) == "++")) {
                        for (size_t i = 9; i < line.length() && i < 60; i += 3) {
                            std::string sat_id = line.substr(i, 3);
                            if (!sat_id.empty() && sat_id != "   ") {
                                satellite_ids.push_back(sat_id);
                            }
                        }
                    }
                    if (static_cast<int>(satellite_ids.size()) != num_sats) {
                        log_manager_.getLogger()->warn("Mismatch in satellite count: expected {}, found {}", num_sats, satellite_ids.size());
                    }
                    continue; // Skip back to while after reading continuation lines
                }
                else if (line.substr(0, 2) == "%f") {
                    // Read epoch interval
                    std::istringstream iss(line);
                    std::string dummy;
                    double interval;
                    iss >> dummy >> dummy >> interval;
                    epoch_interval = static_cast<int>(interval);
                    log_manager_.getLogger()->debug("SP3 epoch interval: {} seconds", epoch_interval);
                }
                else if (line.substr(0, 2) == "/*") {
                    // Comment lines
                    continue;
                }
                else if (line[0] == '*') {
                    in_header = false;
                    // Process first epoch line below
                }
            }

            // Parse body
            while (line[0] == '*' || std::getline(file, line)) {
                if (line[0] == '*') { // Epoch line
                    std::istringstream iss(line);
                    std::string star;
                    int year, month, day, hour, min;
                    double sec;
                    iss >> star >> year >> month >> day >> hour >> min >> sec;
                    if (!iss) {
                        log_manager_.getLogger()->warn("Invalid epoch line: {}", line);
                        continue;
                    }
                    GnssTime epoch_time = TimeUtils::gregorianToGpsTime(year, month, day, hour, min, sec);

                    // Read position/velocity records for this epoch
                    while (std::getline(file, line) && line[0] != '*' && !line.empty() && line[0] != 'E') {
                        if (line[0] == 'P') { // Position record
                            OrbitData data;
                            data.satellite = line.substr(1, 3);
                            data.time = epoch_time;
                            data.position_valid = true;
                            data.velocity_valid = false;
                            data.position = Eigen::Vector3d::Zero();
                            data.velocity = Eigen::Vector3d::Zero();

                            std::istringstream iss(line.substr(4));
                            double x, y, z;
                            iss >> x >> y >> z;
                            if (iss.fail() || x > 1e6 || y > 1e6 || z > 1e6) { // Check for invalid or missing data
                                log_manager_.getLogger()->warn("Invalid position data for {} at epoch {}", data.satellite, epoch_time.tow);
                                data.position_valid = false;
                            }
                            else {
                                data.position = Eigen::Vector3d(x * 1000.0, y * 1000.0, z * 1000.0); // km to m
                            }

                            orbits.push_back(data);
                        }
                        else if (line[0] == 'V') { // Velocity record
                            std::string sat_id = line.substr(1, 3);
                            auto it = std::find_if(orbits.rbegin(), orbits.rend(),
                                [&sat_id, &epoch_time](const OrbitData& o) {
                                    return o.satellite == sat_id && o.time.tow == epoch_time.tow;
                                });
                            if (it != orbits.rend()) {
                                std::istringstream iss(line.substr(4));
                                double vx, vy, vz;
                                iss >> vx >> vy >> vz;
                                if (iss.fail() || vx > 1e6 || vy > 1e6 || vz > 1e6) {
                                    log_manager_.getLogger()->warn("Invalid velocity data for {} at epoch {}", sat_id, epoch_time.tow);
                                }
                                else {
                                    it->velocity = Eigen::Vector3d(vx * 0.1, vy * 0.1, vz * 0.1); // dm/s to m/s
                                    it->velocity_valid = true;
                                }
                            }
                        }
                    }
                }
                else if (line.substr(0, 3) == "EOF") {
                    break;
                }
            }

            file.close();
            if (orbits.empty()) {
                log_manager_.getLogger()->error("No valid orbit data parsed from {}", sp3_path.string());
                return false;
            }
            log_manager_.getLogger()->info("Parsed {} orbit entries from {}", orbits.size(), sp3_path.string());
            return true;
        }
        catch (const std::exception& e) {
            log_manager_.getLogger()->error("Error parsing SP3 file {}: {}", sp3_path.string(), e.what());
            return false;
        }
    }

    bool ProductParser::parseClk(const std::filesystem::path& clk_path, std::vector<ClockData>& clocks) {
        try {
            std::ifstream file(clk_path);
            if (!file.is_open()) {
                log_manager_.getLogger()->error("Failed to open CLK file: {}", clk_path.string());
                return false;
            }

            clocks.clear();
            std::string line;
            while (std::getline(file, line)) {
                if (line.find("AS ") == 0) {
                    std::istringstream iss(line);
                    std::string type, sat, epoch_str;
                    double mjd, sod, corr;
                    iss >> type >> sat >> epoch_str >> mjd >> sod >> corr;
                    ClockData data;
                    data.satellite = sat;
                    data.time = GnssTime{ 0, (mjd - 51544.5) * 86400.0 + sod };
                    data.correction = corr;
                    clocks.push_back(data);
                }
            }
            file.close();
            log_manager_.getLogger()->info("Parsed {} clock entries from {}", clocks.size(), clk_path.string());
            return !clocks.empty();
        }
        catch (const std::exception& e) {
            log_manager_.getLogger()->error("Error parsing CLK file {}: {}", clk_path.string(), e.what());
            return false;
        }
    }

    bool ProductParser::parseAtx(const std::filesystem::path& atx_path, std::vector<AntennaData>& antennas) {
        try {
            std::ifstream file(atx_path);
            if (!file.is_open()) {
                log_manager_.getLogger()->error("Failed to open ATX file: {}", atx_path.string());
                return false;
            }

            antennas.clear();
            std::string line;
            AntennaData current_antenna;
            bool in_antenna_block = false;
            bool in_pcv_block = false;
            std::string current_freq;
            std::vector<double> current_pcv_row;

            while (std::getline(file, line)) {
                if (line.length() < 60) continue; // Skip short or comment lines

                if (line.find("START OF ANTENNA") != std::string::npos) {
                    in_antenna_block = true;
                    current_antenna = AntennaData();
                    in_pcv_block = false;
                    current_freq.clear();
                    current_pcv_row.clear();
                }
                else if (line.find("END OF ANTENNA") != std::string::npos && in_antenna_block) {
                    antennas.push_back(current_antenna);
                    in_antenna_block = false;
                    in_pcv_block = false;
                }
                else if (in_antenna_block) {
                    if (line.find("TYPE / SERIAL NO") != std::string::npos) {
                        current_antenna.type = line.substr(0, 20);
                        std::string serial_no = line.substr(20, 20);
                        if (!serial_no.empty() && serial_no != std::string(20, ' ')) {
                            current_antenna.type += "_" + serial_no;
                        }
                        log_manager_.getLogger()->debug("Parsing antenna: {}", current_antenna.type);
                    }
                    else if (line.find("START OF FREQ") != std::string::npos) {
                        current_freq = line.substr(3, 5);
                        in_pcv_block = true;
                        current_antenna.pcv[current_freq] = std::vector<std::vector<double>>();
                        std::getline(file, line); // Read PCO line
                        if (line.length() >= 30) {
                            std::istringstream iss(line);
                            double x, y, z;
                            iss >> x >> y >> z;
                            if (!iss.fail()) {
                                current_antenna.pco[current_freq] = Eigen::Vector3d(x, y, z); // mm
                                log_manager_.getLogger()->trace("PCO for {} {}: ({}, {}, {}) mm",
                                    current_antenna.type, current_freq, x, y, z);
                            }
                        }
                    }
                    else if (line.find("END OF FREQ") != std::string::npos) {
                        in_pcv_block = false;
                        current_freq.clear();
                    }
                    else if (in_pcv_block && line.find("DAZI") != std::string::npos) {
                        // Skip DAZI line
                    }
                    else if (in_pcv_block && line.find("ZEN1 / ZEN2 / DZEN") != std::string::npos) {
                        // Skip ZEN1/ZEN2/DZEN line
                    }
                    else if (in_pcv_block && !line.empty() && line[0] != ' ') {
                        // PCV table row (zenith angle)
                        std::istringstream iss(line.substr(8));
                        current_pcv_row.clear();
                        double value;
                        while (iss >> value) {
                            current_pcv_row.push_back(value); // mm
                        }
                        if (!current_pcv_row.empty()) {
                            current_antenna.pcv[current_freq].push_back(current_pcv_row);
                        }
                    }
                }
            }

            file.close();
            if (antennas.empty()) {
                log_manager_.getLogger()->error("No valid antenna data parsed from {}", atx_path.string());
                return false;
            }
            log_manager_.getLogger()->info("Parsed {} antenna entries from {}", antennas.size(), atx_path.string());
            return true;
        }
        catch (const std::exception& e) {
            log_manager_.getLogger()->error("Error parsing ATX file {}: {}", atx_path.string(), e.what());
            return false;
        }
    }

    bool ProductParser::parseBia(const std::filesystem::path& bia_path, std::vector<BiasData>& biases) {
        try {
            std::ifstream file(bia_path);
            if (!file.is_open()) {
                log_manager_.getLogger()->error("Failed to open BIA file: {}", bia_path.string());
                return false;
            }

            biases.clear();
            std::string line;
            while (std::getline(file, line)) {
                if (line.find("+BIAS/SOLUTION") != std::string::npos) {
                    while (std::getline(file, line) && line.find("-BIAS/SOLUTION") == std::string::npos) {
                        if (line.find("OSB") == 0) {
                            BiasData data;
                            std::istringstream iss(line);
                            std::string type, sat, signal, start, end;
                            double bias, std;
                            iss >> type >> sat >> signal >> start >> end >> bias >> std;
                            data.satellite = sat;
                            data.signal = signal;
                            //data.type = signal;
                            data.bias = bias / Constants::SPEED_OF_LIGHT;
                            data.start_time = GnssTime{ 2316, 0.0 };
                            data.end_time = GnssTime{ 2316, 86400.0 };
                            biases.push_back(data);
                        }
                    }
                }
            }
            file.close();
            log_manager_.getLogger()->info("Parsed {} bias entries from {}", biases.size(), bia_path.string());
            return !biases.empty();
        }
        catch (const std::exception& e) {
            log_manager_.getLogger()->error("Error parsing BIA file {}: {}", bia_path.string(), e.what());
            return false;
        }
    }

    bool ProductParser::parseErp(const std::filesystem::path& erp_path, std::vector<ErpData>& erps) {
        try {
            std::ifstream file(erp_path);
            if (!file.is_open()) {
                log_manager_.getLogger()->error("Failed to open ERP file: {}", erp_path.string());
                return false;
            }

            erps.clear();
            std::string line;
            while (std::getline(file, line)) {
                if (line.find("MJD") != std::string::npos) continue;
                std::istringstream iss(line);
                double mjd, xp, yp, ut1_utc;
                iss >> mjd >> xp >> yp >> ut1_utc;
                ErpData data;
                data.time = GnssTime{ 0, (mjd - 51544.5) * 86400.0 };
                data.xpole = xp;
                data.ypole = yp;
                data.ut1_utc = ut1_utc;
                erps.push_back(data);
            }
            file.close();
            log_manager_.getLogger()->info("Parsed {} ERP entries from {}", erps.size(), erp_path.string());
            return !erps.empty();
        }
        catch (const std::exception& e) {
            log_manager_.getLogger()->error("Error parsing ERP file {}: {}", erp_path.string(), e.what());
            return false;
        }
    }

    /*
    bool ProductParser::parseOceanTide(const std::filesystem::path& nc_path, std::vector<OceanTideData>& tides) {
        try {
            netCDF::NcFile nc_file(nc_path.string(), netCDF::NcFile::read);
            tides.clear();

            // Get dimensions
            auto time_dim = nc_file.getDim("time");
            if (time_dim.isNull()) {
                log_manager_.getLogger()->error("No 'time' dimension in NetCDF file: {}", nc_path.string());
                return false;
            }
            size_t num_times = time_dim.getSize();

            // Get variables
            auto time_var = nc_file.getVar("time");
            auto disp_x_var = nc_file.getVar("displacement_x");
            auto disp_y_var = nc_file.getVar("displacement_y");
            auto disp_z_var = nc_file.getVar("displacement_z");
            if (time_var.isNull() || disp_x_var.isNull() || disp_y_var.isNull() || disp_z_var.isNull()) {
                log_manager_.getLogger()->error("Missing required variables in NetCDF file: {}", nc_path.string());
                return false;
            }

            // Read data
            std::vector<double> times(num_times);
            std::vector<double> disp_x(num_times), disp_y(num_times), disp_z(num_times);
            time_var.getVar(times.data());
            disp_x_var.getVar(disp_x.data());
            disp_y_var.getVar(disp_y.data());
            disp_z_var.getVar(disp_z.data());

            // Populate tides
            for (size_t i = 0; i < num_times; ++i) {
                OceanTideData data;
                // Assume times are in seconds since GPS epoch; convert to GnssTime
                int week = static_cast<int>(times[i] / (7 * 86400));
                double tow = times[i] - week * (7 * 86400);
                data.time = GnssTime{ week, tow };
                data.displacement = Eigen::Vector3d(disp_x[i], disp_y[i], disp_z[i]);
                tides.push_back(data);
            }

            log_manager_.getLogger()->info("Parsed {} ocean tide entries from {}", tides.size(), nc_path.string());
            return true;
        }
        catch (const netCDF::NcException& e) {
            log_manager_.getLogger()->error("Error parsing ocean tide file {}: {}", nc_path.string(), e.what());
            return false;
        }
        catch (const std::exception& e) {
            log_manager_.getLogger()->error("Unexpected error parsing ocean tide file {}: {}", nc_path.string(), e.what());
            return false;
        }
    }
    */

    bool ProductParser::parseKlobuchar(const std::filesystem::path& brdc_path, std::vector<double>& coeffs) {
        try {
            std::ifstream file(brdc_path);
            if (!file.is_open()) {
                log_manager_.getLogger()->error("Failed to open BRDC file: {}", brdc_path.string());
                return false;
            }

            coeffs.clear();
            std::string line;
            bool in_iono = false;
            while (std::getline(file, line)) {
                if (line.find("ION ALPHA") != std::string::npos) {
                    in_iono = true;
                    std::istringstream iss(line);
                    std::string dummy;
                    double a0, a1, a2, a3;
                    iss >> dummy >> a0 >> a1 >> a2 >> a3;
                    coeffs.push_back(a0);
                    coeffs.push_back(a1);
                    coeffs.push_back(a2);
                    coeffs.push_back(a3);
                }
                else if (line.find("ION BETA") != std::string::npos && in_iono) {
                    std::istringstream iss(line);
                    std::string dummy;
                    double b0, b1, b2, b3;
                    iss >> dummy >> b0 >> b1 >> b2 >> b3;
                    coeffs.push_back(b0);
                    coeffs.push_back(b1);
                    coeffs.push_back(b2);
                    coeffs.push_back(b3);
                    break;
                }
                if (in_iono && line.find("END OF HEADER") != std::string::npos) {
                    break;
                }
            }
            file.close();
            if (coeffs.size() == 8) {
                log_manager_.getLogger()->info("Parsed Klobuchar coefficients from {}", brdc_path.string());
                return true;
            }
            log_manager_.getLogger()->error("Incomplete Klobuchar coefficients parsed from {}", brdc_path.string());
            return false;
        }
        catch (const std::exception& e) {
            log_manager_.getLogger()->error("Error parsing BRDC file {}: {}", brdc_path.string(), e.what());
            return false;
        }
    }

    void ProductParser::parseSunEphemeris(const std::filesystem::path& brdc_path, std::vector<SunEphemeris>& sun_ephemeris) {
        try {
            std::ifstream file(brdc_path);
            if (!file.is_open()) {
                log_manager_.getLogger()->error("Failed to open BRDC file for sun ephemeris: {}", brdc_path.string());
                return;
            }

            sun_ephemeris.clear();
            bool in_sun_data = false;
            std::string line;
            size_t line_count = 0;

            while (std::getline(file, line)) {
                ++line_count;
                if (line.find("SUN EPHEMERIS") != std::string::npos) {
                    in_sun_data = true;
                    continue;
                }
                if (in_sun_data && line.empty()) {
                    break;
                }
                if (in_sun_data) {
                    SunEphemeris data;
                    std::istringstream iss(line);
                    int week;
                    double tow, x, y, z;
                    if (iss >> week >> tow >> x >> y >> z) {
                        // Validate time
                        if (week < 0 || tow < 0 || tow >= 604800) {
                            log_manager_.getLogger()->warn("Invalid GPS time (week: {}, tow: {}) at line {}", week, tow, line_count);
                            continue;
                        }
                        data.time = GnssTime{ week, tow };
                        data.position = Eigen::Vector3d(x * 1e3, y * 1e3, z * 1e3); // Convert km to m
                        sun_ephemeris.push_back(data);
                    }
                    else {
                        log_manager_.getLogger()->warn("Invalid sun ephemeris line {}: {}", line_count, line);
                    }
                }
            }

            log_manager_.getLogger()->info("Parsed {} sun ephemeris entries from {}", sun_ephemeris.size(), brdc_path.string());
        }
        catch (const std::exception& e) {
            log_manager_.getLogger()->error("Error parsing sun ephemeris from {}: {}", brdc_path.string(), e.what());
        }
    }

} // namespace GnssProcessor
//#endif