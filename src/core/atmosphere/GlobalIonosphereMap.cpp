#include "atmosphere/GlobalIonosphereMap.hpp"

#include "atmosphere/SphericalHarmonicIonosphere.hpp"
#include "common/Constants.hpp"
#include "math/Coordinates.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

namespace GnssProcessor {

    namespace {
        // Ionospheric delay: delay[m] = 40.3 * STEC[el/m^2] / f^2[Hz^2].
        constexpr double kIonoConstant      = 40.3;
        constexpr double kTecuToElectrons   = 1.0e16;       // 1 TECU = 1e16 el/m^2
        constexpr double kEarthRotDegPerSec = 360.0 / 86400.0;
        constexpr double kFallbackRmsTecu   = 3.0;          // when the IONEX file has no RMS maps
    } // namespace

    GlobalIonosphereMap::GlobalIonosphereMap(const std::string& ionex_path, LogManager& log_manager)
        : log_manager_(log_manager) {
        auto log = log_manager_.getLogger("ionosphere");
        if (ionex_path.empty()) {
            log->warn("GIM: no IONEX path supplied — ionosphere weighting will be inactive");
            return;
        }
        data_ = IonexParser::parse(ionex_path, log_manager_);
        if (data_.valid) {
            log->info("GIM: ready ({} maps, {}x{} grid, shell {:.0f} km, base radius {:.0f} km)",
                      data_.maps.size(), data_.n_lat, data_.n_lon,
                      data_.height_km, data_.base_radius_km);
        }
    }

    double GlobalIonosphereMap::sampleMap(const std::vector<double>& grid,
                                          double lat_deg, double lon_deg) const {
        if (grid.empty()) return std::numeric_limits<double>::quiet_NaN();

        const double lon_min = std::min(data_.lon1_deg, data_.lon2_deg);
        const double lon_max = std::max(data_.lon1_deg, data_.lon2_deg);
        double lon = lon_deg;
        while (lon < lon_min) lon += 360.0;
        while (lon > lon_max) lon -= 360.0;

        double fr = (lat_deg - data_.lat1_deg) / data_.dlat_deg;
        double fc = (lon - data_.lon1_deg) / data_.dlon_deg;
        fr = std::clamp(fr, 0.0, static_cast<double>(data_.n_lat - 1));
        fc = std::clamp(fc, 0.0, static_cast<double>(data_.n_lon - 1));

        const int r0 = static_cast<int>(std::floor(fr));
        const int c0 = static_cast<int>(std::floor(fc));
        const int r1 = std::min(r0 + 1, data_.n_lat - 1);
        const int c1 = std::min(c0 + 1, data_.n_lon - 1);
        const double wr = fr - r0;
        const double wc = fc - c0;

        auto node = [&](int r, int c) {
            return grid[static_cast<size_t>(r) * data_.n_lon + c];
        };

        double sum = 0.0, wsum = 0.0;
        auto accumulate = [&](double v, double w) {
            if (std::isfinite(v)) { sum += w * v; wsum += w; }
        };
        accumulate(node(r0, c0), (1.0 - wr) * (1.0 - wc));
        accumulate(node(r0, c1), (1.0 - wr) * wc);
        accumulate(node(r1, c0), wr * (1.0 - wc));
        accumulate(node(r1, c1), wr * wc);

        if (wsum <= 0.0) return std::numeric_limits<double>::quiet_NaN();
        return sum / wsum;
    }

    bool GlobalIonosphereMap::interpolateVtec(const GnssTime& time,
                                              double ipp_lat_deg, double ipp_lon_deg,
                                              double& vtec_tecu, double& vtec_rms_tecu) const {
        if (data_.maps.empty()) return false;
        const double t = TimeUtils::continuousGpsSeconds(time);

        size_t i1 = 0;
        while (i1 + 1 < data_.maps.size() &&
               TimeUtils::continuousGpsSeconds(data_.maps[i1 + 1].epoch) <= t) {
            ++i1;
        }
        const size_t i2 = std::min(i1 + 1, data_.maps.size() - 1);
        const double t1 = TimeUtils::continuousGpsSeconds(data_.maps[i1].epoch);
        const double t2 = TimeUtils::continuousGpsSeconds(data_.maps[i2].epoch);

        // Sun-fixed interpolation: evaluate each map at the IPP longitude rotated
        // by the Earth's rotation between the map epoch and the query epoch.
        const double lon1 = ipp_lon_deg + (t - t1) * kEarthRotDegPerSec;
        const double lon2 = ipp_lon_deg + (t - t2) * kEarthRotDegPerSec;
        const double w = (t2 > t1) ? std::clamp((t - t1) / (t2 - t1), 0.0, 1.0) : 0.0;

        const double v1 = sampleMap(data_.maps[i1].vtec, ipp_lat_deg, lon1);
        const double v2 = sampleMap(data_.maps[i2].vtec, ipp_lat_deg, lon2);
        if (std::isfinite(v1) && std::isfinite(v2))      vtec_tecu = (1.0 - w) * v1 + w * v2;
        else if (std::isfinite(v1))                      vtec_tecu = v1;
        else if (std::isfinite(v2))                      vtec_tecu = v2;
        else return false;

        double r1 = std::numeric_limits<double>::quiet_NaN();
        double r2 = std::numeric_limits<double>::quiet_NaN();
        if (!data_.maps[i1].rms.empty()) r1 = sampleMap(data_.maps[i1].rms, ipp_lat_deg, lon1);
        if (!data_.maps[i2].rms.empty()) r2 = sampleMap(data_.maps[i2].rms, ipp_lat_deg, lon2);
        if (std::isfinite(r1) && std::isfinite(r2))      vtec_rms_tecu = (1.0 - w) * r1 + w * r2;
        else if (std::isfinite(r1))                      vtec_rms_tecu = r1;
        else if (std::isfinite(r2))                      vtec_rms_tecu = r2;
        else                                             vtec_rms_tecu = kFallbackRmsTecu;

        return true;
    }

    GimQuery GlobalIonosphereMap::slantIonoDelay(const GnssTime& time,
                                                 const Eigen::Vector3d& rx_pos_ecef,
                                                 const Eigen::Vector3d& sat_pos_ecef,
                                                 double frequency_hz) const {
        GimQuery q;
        if (!isLoaded() || frequency_hz <= 0.0) return q;

        // Single-layer pierce-point geometry — shared with the SH ionosphere model
        // so the two never drift (the math here is unchanged: the helper reproduces
        // this block verbatim, so the GIM result stays byte-identical).
        const double Re = data_.base_radius_km * 1000.0;
        const double H  = data_.height_km * 1000.0;
        const IppGeometry g = computeIppGeometry(rx_pos_ecef, sat_pos_ecef, Re, H);
        if (!g.valid) return q;
        const double mapping  = g.mapping;
        const double lat_ipp  = g.ipp_lat_rad;
        const double lon_ipp  = g.ipp_lon_rad;

        const double rad2deg = 180.0 / Constants::PI;
        double vtec = 0.0, vtec_rms = 0.0;
        if (!interpolateVtec(time, lat_ipp * rad2deg, lon_ipp * rad2deg, vtec, vtec_rms)) {
            return q;
        }

        const double stec     = vtec * mapping;                    // TECU
        const double stec_rms = std::abs(vtec_rms) * mapping;       // TECU
        const double tecu_to_m = kIonoConstant * kTecuToElectrons / (frequency_hz * frequency_hz);

        q.delay_m      = stec * tecu_to_m;
        q.std_m        = stec_rms * tecu_to_m;
        q.stec_tecu    = stec;
        q.ipp_lat_rad  = lat_ipp;
        q.ipp_lon_rad  = lon_ipp;
        q.ipp_radius_m = Re + H;
        q.valid        = std::isfinite(q.delay_m) && std::isfinite(q.std_m);
        return q;
    }

} // namespace GnssProcessor
