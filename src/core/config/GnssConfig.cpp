/*!
 * \file GnssConfig.cpp
 * \brief Non-loader bits of GnssConfig and LoggingConfig.
 *
 * Organization:
 *  - LoggingConfig::isLoggerEnabled
 *  - GnssConfig::getProductPath
 *
 * The bulk of this file used to be a 900-line @c loadFromJson — that
 * function has been split per top-level JSON section into private
 * helpers defined in @c GnssConfigLoad.cpp.  The shared JSON-reading
 * primitives (resolvePath / jval / jsection) live in
 * @c include/common/GnssConfigJsonHelpers.hpp.
 */

#include "config/GnssConfig.hpp"

namespace GnssProcessor {

    // =================================================================
    // LoggingConfig helpers
    // =================================================================

    bool LoggingConfig::isLoggerEnabled(const std::string& name) const {
        if (name == "main_log")              return enable_main_log;
        if (name == "gnss_config")           return enable_gnss_config;
        if (name == "kalman_filter")         return enable_kalman_filter;
        if (name == "smoother")              return enable_smoother;
        if (name == "state_manager")         return enable_state_manager;
        if (name == "ppp_model")             return enable_ppp_model;
        if (name == "measurement_model")     return enable_measurement_model;
        if (name == "mw_slip_detector")      return enable_mw_slip_detector;
        if (name == "if_slip_detector")      return enable_if_slip_detector;
        if (name == "gf_slip_detector")      return enable_gf_slip_detector;
        if (name == "cycle_slip_manager")    return enable_cycle_slip_manager;
        if (name == "ambiguity_resolver")    return enable_ambiguity_resolver;
        if (name == "corrections")           return enable_corrections;
        if (name == "geometry")              return enable_geometry;
        if (name == "troposphere")           return enable_troposphere;
        if (name == "ionosphere")            return enable_ionosphere;
        if (name == "antenna")               return enable_antenna;
        if (name == "antenna_model")         return enable_antenna_model;
        if (name == "ocean_tide")            return enable_ocean_tide;
        if (name == "planetary_model")       return enable_planetary_model;
        if (name == "stochastic_model")      return enable_stochastic_model;
        if (name == "rinex_parser")          return enable_rinex_parser;
        if (name == "product_parser")        return enable_product_parser;
        if (name == "product_cache")         return enable_product_cache;
        if (name == "resolver")              return enable_resolver;
        if (name == "state_initializer")     return enable_state_initializer;
        if (name == "bssd_pivot_manager")    return enable_bssd_pivot_manager;
        if (name == "bssd_transform")        return enable_bssd_transform;
        if (name == "ppp_utils")             return enable_ppp_utils;
        if (name == "downloader")            return enable_downloader;
        if (name == "ntrip")                 return enable_ntrip;
        if (name == "rt_engine")             return enable_rt_engine;
        if (name == "imu")                   return enable_imu;
        return true; // unknown loggers default to enabled
    }

    // =================================================================
    // Product path lookup
    // =================================================================

    std::filesystem::path GnssConfig::getProductPath(const std::string& type) const {
        for (const auto& path : product_paths_) {
            std::string ext = path.extension().string();
            if ((type == "SP3" && ext == ".SP3") ||
                (type == "CLK" && ext == ".CLK") ||
                (type == "ATX" && ext == ".atx") ||
                (type == "BIA" && ext == ".BIA") ||
                (type == "ERP" && ext == ".ERP") ||
                (type == "NC"  && ext == ".nc"))
                return path;
        }
        if (type == "RNX") return navigation_path_;
        return {};
    }

} // namespace GnssProcessor
