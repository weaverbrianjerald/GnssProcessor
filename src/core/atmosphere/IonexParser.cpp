#include "atmosphere/IonexParser.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <fstream>
#include <limits>
#include <sstream>

namespace GnssProcessor {

    namespace {

        /// IONEX records carry a label in file columns 61-80.
        std::string ionexLabel(const std::string& line) {
            if (line.size() <= 60) return {};
            const std::string s = line.substr(60);
            const size_t b = s.find_first_not_of(" \t\r\n");
            const size_t e = s.find_last_not_of(" \t\r\n");
            if (b == std::string::npos) return {};
            return s.substr(b, e - b + 1);
        }

        /// The data portion of an IONEX record is file columns 1-60.
        std::string ionexData(const std::string& line) {
            return line.size() > 60 ? line.substr(0, 60) : line;
        }

        /// Parse one I5 TEC/RMS field.  Returns false for an all-blank field so
        /// trailing padding on a short value line is skipped rather than counted.
        bool parseField(const std::string& field, double scale, double& value) {
            bool blank = true;
            for (unsigned char ch : field) {
                if (!std::isspace(ch)) { blank = false; break; }
            }
            if (blank) return false;
            try {
                const long raw = std::stol(field);
                value = (raw >= 9999 || raw <= -9999)
                            ? std::numeric_limits<double>::quiet_NaN()
                            : static_cast<double>(raw) * scale;
            } catch (...) {
                value = std::numeric_limits<double>::quiet_NaN();
            }
            return true;
        }

    } // namespace

    IonexData IonexParser::parse(const std::string& path, LogManager& log_manager) {
        auto log = log_manager.getLogger("ionosphere");
        IonexData out;

        std::ifstream file(path);
        if (!file.is_open()) {
            log->warn("IONEX: cannot open file: {}", path);
            return out;
        }

        std::vector<std::string> lines;
        {
            std::string l;
            while (std::getline(file, l)) lines.push_back(l);
        }
        file.close();

        // ---- header ----
        size_t i = 0;
        for (; i < lines.size(); ++i) {
            const std::string label = ionexLabel(lines[i]);
            std::istringstream iss(ionexData(lines[i]));
            if (label == "END OF HEADER") { ++i; break; }
            if (label == "LAT1 / LAT2 / DLAT") {
                iss >> out.lat1_deg >> out.lat2_deg >> out.dlat_deg;
            } else if (label == "LON1 / LON2 / DLON") {
                iss >> out.lon1_deg >> out.lon2_deg >> out.dlon_deg;
            } else if (label == "HGT1 / HGT2 / DHGT") {
                double h1 = 0.0, h2 = 0.0, dh = 0.0;
                iss >> h1 >> h2 >> dh;
                out.height_km = h1;  // single-layer 2D map: HGT1 == HGT2
            } else if (label == "BASE RADIUS") {
                iss >> out.base_radius_km;
            } else if (label == "EXPONENT") {
                iss >> out.exponent;
            }
        }

        if (out.dlat_deg == 0.0 || out.dlon_deg == 0.0) {
            log->warn("IONEX: missing/zero grid step in {}", path);
            return out;
        }
        out.n_lat = static_cast<int>(std::lround((out.lat2_deg - out.lat1_deg) / out.dlat_deg)) + 1;
        out.n_lon = static_cast<int>(std::lround((out.lon2_deg - out.lon1_deg) / out.dlon_deg)) + 1;
        if (out.n_lat < 2 || out.n_lon < 2) {
            log->warn("IONEX: degenerate {}x{} grid in {}", out.n_lat, out.n_lon, path);
            return out;
        }

        const double scale = std::pow(10.0, static_cast<double>(out.exponent));

        // ---- body: TEC and RMS map blocks ----
        std::vector<IonexMap> tec_maps, rms_maps;
        while (i < lines.size()) {
            const std::string label = ionexLabel(lines[i]);
            if (label == "START OF AUX DATA") {
                ++i;
                while (i < lines.size() && ionexLabel(lines[i]) != "END OF AUX DATA") ++i;
                if (i < lines.size()) ++i;
                continue;
            }
            if (label != "START OF TEC MAP" && label != "START OF RMS MAP") {
                ++i;
                continue;
            }

            const bool is_rms = (label == "START OF RMS MAP");
            const std::string end_label = is_rms ? "END OF RMS MAP" : "END OF TEC MAP";
            IonexMap map;
            map.vtec.assign(static_cast<size_t>(out.n_lat) * out.n_lon,
                            std::numeric_limits<double>::quiet_NaN());
            ++i;

            while (i < lines.size()) {
                const std::string lab = ionexLabel(lines[i]);
                if (lab == end_label) { ++i; break; }

                if (lab == "EPOCH OF CURRENT MAP") {
                    int y = 0, mo = 0, d = 0, h = 0, mi = 0;
                    double s = 0.0;
                    std::istringstream iss(ionexData(lines[i]));
                    iss >> y >> mo >> d >> h >> mi >> s;
                    // IONEX epochs are UT; convert to the GPS time scale.
                    map.epoch = TimeUtils::gregorianToGpsTime(y, mo, d, h, mi, s);
                    ++i;
                } else if (lab == "LAT/LON1/LON2/DLON/H") {
                    double lat = 0.0, lo1 = 0.0, lo2 = 0.0, dlo = 0.0, hh = 0.0;
                    std::istringstream iss(ionexData(lines[i]));
                    iss >> lat >> lo1 >> lo2 >> dlo >> hh;
                    const int row = static_cast<int>(
                        std::lround((lat - out.lat1_deg) / out.dlat_deg));
                    ++i;
                    int got = 0;
                    while (got < out.n_lon && i < lines.size()) {
                        const std::string vlab = ionexLabel(lines[i]);
                        if (vlab == end_label || vlab == "LAT/LON1/LON2/DLON/H" ||
                            vlab == "EPOCH OF CURRENT MAP") {
                            break;  // malformed/short band — stop before the next record
                        }
                        const std::string& vl = lines[i];
                        for (size_t c = 0; c < vl.size() && got < out.n_lon; c += 5) {
                            double val = 0.0;
                            if (!parseField(vl.substr(c, 5), scale, val)) continue;
                            if (row >= 0 && row < out.n_lat) {
                                map.vtec[static_cast<size_t>(row) * out.n_lon + got] = val;
                            }
                            ++got;
                        }
                        ++i;
                    }
                } else {
                    ++i;
                }
            }
            (is_rms ? rms_maps : tec_maps).push_back(std::move(map));
        }

        if (tec_maps.empty()) {
            log->warn("IONEX: no TEC maps parsed from {}", path);
            return out;
        }

        // Attach each RMS grid to the TEC map sharing its epoch.
        for (auto& tm : tec_maps) {
            const double te = TimeUtils::continuousGpsSeconds(tm.epoch);
            for (const auto& rm : rms_maps) {
                if (std::abs(TimeUtils::continuousGpsSeconds(rm.epoch) - te) < 2.0) {
                    tm.rms = rm.vtec;  // RMS values were filled into the working 'vtec' grid
                    break;
                }
            }
        }
        std::sort(tec_maps.begin(), tec_maps.end(),
                  [](const IonexMap& a, const IonexMap& b) {
                      return TimeUtils::continuousGpsSeconds(a.epoch) <
                             TimeUtils::continuousGpsSeconds(b.epoch);
                  });

        out.maps = std::move(tec_maps);
        out.valid = true;
        log->info("IONEX: parsed {} TEC maps ({} RMS maps) from {}",
                  out.maps.size(), rms_maps.size(), path);
        return out;
    }

} // namespace GnssProcessor
