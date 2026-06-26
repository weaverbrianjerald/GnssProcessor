/**
 * @file SimConfig.cpp
 * @brief JSON loader for the synthetic-observation generator knobs.
 *
 * Uses the same nlohmann/json + `ignore_comments` convention as the processor's
 * GnssConfig (so `//` comments are allowed) and the same @c detail::jval
 * "path-or-default" helper, keeping the simulator and processor config dialects
 * consistent.
 */

#include "sim/SimConfig.hpp"

#include <cmath>
#include <fstream>

#include <nlohmann/json.hpp>

#include "config/GnssConfigJsonHelpers.hpp" // detail::jval, detail::resolvePath

namespace GnssProcessor {
namespace sim {

bool loadSimConfig(const std::filesystem::path& path, SimConfig& cfg,
                   std::string* err) {
    auto fail = [&](const std::string& msg) {
        if (err) *err = msg;
        return false;
    };

    std::ifstream file(path);
    if (!file.is_open())
        return fail("cannot open sim config: " + path.string());

    nlohmann::json doc;
    try {
        doc = nlohmann::json::parse(file, /*callback=*/nullptr,
                                    /*allow_exceptions=*/true,
                                    /*ignore_comments=*/true);
    } catch (const std::exception& e) {
        return fail("JSON parse error in " + path.string() + ": " + e.what());
    }

    // Fields may live at the root or under a "simulator" object, so a sim config
    // can also be embedded in a processor config later without ambiguity.
    const nlohmann::json& s =
        (doc.is_object() && doc.contains("simulator")) ? doc["simulator"] : doc;
    if (!s.is_object())
        return fail("sim config root is not a JSON object: " + path.string());

    using detail::jval;
    using detail::resolvePath;

    // Read an int knob tolerantly: accept 120 OR 120.0 (nlohmann's get<int> throws
    // on a float literal), rounding to nearest — matching the loose double semantics
    // of every other knob.
    auto jint = [&](const char* path, int dflt) -> int {
        return static_cast<int>(std::llround(jval<double>(s, path, "", static_cast<double>(dflt))));
    };
    // Read a non-negative uint64 seed; reject a negative JSON literal loudly rather
    // than letting it wrap to ~1.8e19.
    bool seed_ok = true;
    std::string seed_err;
    auto jseed = [&](const char* path, std::uint64_t dflt) -> std::uint64_t {
        const nlohmann::json* p = resolvePath(s, path);
        if (!p) return dflt;
        if (p->is_number_integer() && p->get<long long>() < 0) {
            seed_ok = false;
            seed_err = std::string("sim config '") + path + "' must be a non-negative integer";
            return dflt;
        }
        return p->get<std::uint64_t>();
    };

    cfg.noise.code_sigma_zen_m     = jval(s, "noise.code_sigma_zen_m",     "", cfg.noise.code_sigma_zen_m);
    cfg.noise.phase_sigma_zen_m    = jval(s, "noise.phase_sigma_zen_m",    "", cfg.noise.phase_sigma_zen_m);
    cfg.noise.elev_factor_floor_deg= jval(s, "noise.elev_factor_floor_deg","", cfg.noise.elev_factor_floor_deg);
    cfg.noise.seed                 = jseed("noise.seed", cfg.noise.seed);

    cfg.multipath.code_amp_gps_l1_m  = jval(s, "multipath.code_amp_gps_l1_m",  "", cfg.multipath.code_amp_gps_l1_m);
    cfg.multipath.code_amp_gps_l2_m  = jval(s, "multipath.code_amp_gps_l2_m",  "", cfg.multipath.code_amp_gps_l2_m);
    cfg.multipath.code_amp_gal_e1_m  = jval(s, "multipath.code_amp_gal_e1_m",  "", cfg.multipath.code_amp_gal_e1_m);
    cfg.multipath.code_amp_gal_e5a_m = jval(s, "multipath.code_amp_gal_e5a_m", "", cfg.multipath.code_amp_gal_e5a_m);
    cfg.multipath.phase_amp_zen_m    = jval(s, "multipath.phase_amp_zen_m",    "", cfg.multipath.phase_amp_zen_m);
    cfg.multipath.tau_s              = jval(s, "multipath.tau_s",              "", cfg.multipath.tau_s);
    cfg.multipath.elev_factor_cap_deg= jval(s, "multipath.elev_factor_cap_deg","", cfg.multipath.elev_factor_cap_deg);
    cfg.multipath.seed               = jseed("multipath.seed", cfg.multipath.seed);

    cfg.slips.rate_base        = jval(s, "cycle_slips.rate_base",        "", cfg.slips.rate_base);
    cfg.slips.min_gap_epochs   = jint("cycle_slips.min_gap_epochs",   cfg.slips.min_gap_epochs);
    cfg.slips.max_elev_deg     = jval(s, "cycle_slips.max_elev_deg",     "", cfg.slips.max_elev_deg);
    cfg.slips.min_cycles_zenith= jval(s, "cycle_slips.min_cycles_zenith","", cfg.slips.min_cycles_zenith);
    cfg.slips.max_cycles_zenith= jval(s, "cycle_slips.max_cycles_zenith","", cfg.slips.max_cycles_zenith);
    cfg.slips.min_magnitude    = jint("cycle_slips.min_magnitude",    cfg.slips.min_magnitude);

    cfg.atmosphere.zwd_m       = jval(s, "atmosphere.zwd_m",       "", cfg.atmosphere.zwd_m);
    cfg.biases.isb_gps_gal_m   = jval(s, "biases.isb_gps_gal_m",   "", cfg.biases.isb_gps_gal_m);
    cfg.geometry.elevation_mask_deg = jval(s, "geometry.elevation_mask_deg", "", cfg.geometry.elevation_mask_deg);

    cfg.iono.storm_scale            = jval(s, "iono.storm_scale",            "", cfg.iono.storm_scale);
    cfg.iono.quiet_vtec_tecu        = jval(s, "iono.quiet_vtec_tecu",        "", cfg.iono.quiet_vtec_tecu);
    cfg.iono.sed_amp_tecu           = jval(s, "iono.sed_amp_tecu",           "", cfg.iono.sed_amp_tecu);
    cfg.iono.tid_amp_tecu           = jval(s, "iono.tid_amp_tecu",           "", cfg.iono.tid_amp_tecu);
    cfg.iono.scint_sigma_phi_max_rad= jval(s, "iono.scint_sigma_phi_max_rad","", cfg.iono.scint_sigma_phi_max_rad);
    cfg.iono.window_start_frac      = jval(s, "iono.window_start_frac",      "", cfg.iono.window_start_frac);
    cfg.iono.window_duration_frac   = jval(s, "iono.window_duration_frac",   "", cfg.iono.window_duration_frac);
    cfg.iono.sed_sigma_lat_deg      = jval(s, "iono.sed_sigma_lat_deg",      "", cfg.iono.sed_sigma_lat_deg);
    cfg.iono.sed_sigma_lon_deg      = jval(s, "iono.sed_sigma_lon_deg",      "", cfg.iono.sed_sigma_lon_deg);
    cfg.iono.drift_lat_deg_per_min  = jval(s, "iono.drift_lat_deg_per_min",  "", cfg.iono.drift_lat_deg_per_min);
    cfg.iono.drift_lon_deg_per_min  = jval(s, "iono.drift_lon_deg_per_min",  "", cfg.iono.drift_lon_deg_per_min);
    cfg.iono.center_lat_offset_deg  = jval(s, "iono.center_lat_offset_deg",  "", cfg.iono.center_lat_offset_deg);
    cfg.iono.center_lon_offset_deg  = jval(s, "iono.center_lon_offset_deg",  "", cfg.iono.center_lon_offset_deg);
    cfg.iono.tid_wavelength_km      = jval(s, "iono.tid_wavelength_km",      "", cfg.iono.tid_wavelength_km);
    cfg.iono.tid_period_s           = jval(s, "iono.tid_period_s",           "", cfg.iono.tid_period_s);
    cfg.iono.tid_azimuth_deg        = jval(s, "iono.tid_azimuth_deg",        "", cfg.iono.tid_azimuth_deg);
    cfg.iono.shell_height_km        = jval(s, "iono.shell_height_km",        "", cfg.iono.shell_height_km);
    cfg.iono.scint_elev_clamp_deg   = jval(s, "iono.scint_elev_clamp_deg",   "", cfg.iono.scint_elev_clamp_deg);

    cfg.trajectory.interval_s      = jval(s, "trajectory.interval_s",      "", cfg.trajectory.interval_s);
    cfg.trajectory.n_epochs        = jint("trajectory.n_epochs",        cfg.trajectory.n_epochs);
    cfg.trajectory.cruise_speed_mps= jval(s, "trajectory.cruise_speed_mps","", cfg.trajectory.cruise_speed_mps);
    cfg.trajectory.lat0_deg        = jval(s, "trajectory.lat0_deg",        "", cfg.trajectory.lat0_deg);
    cfg.trajectory.lon0_deg        = jval(s, "trajectory.lon0_deg",        "", cfg.trajectory.lon0_deg);
    cfg.trajectory.h0_m            = jval(s, "trajectory.h0_m",            "", cfg.trajectory.h0_m);
    cfg.trajectory.heading0_deg    = jval(s, "trajectory.heading0_deg",    "", cfg.trajectory.heading0_deg);
    cfg.trajectory.heading_amp_deg = jval(s, "trajectory.heading_amp_deg", "", cfg.trajectory.heading_amp_deg);
    cfg.trajectory.heading_period_s= jval(s, "trajectory.heading_period_s","", cfg.trajectory.heading_period_s);
    cfg.trajectory.heave_amp_m     = jval(s, "trajectory.heave_amp_m",     "", cfg.trajectory.heave_amp_m);
    cfg.trajectory.heave_period_s  = jval(s, "trajectory.heave_period_s",  "", cfg.trajectory.heave_period_s);
    cfg.trajectory.clock_bias0_s   = jval(s, "trajectory.clock_bias0_s",   "", cfg.trajectory.clock_bias0_s);
    cfg.trajectory.clock_drift_sps = jval(s, "trajectory.clock_drift_sps", "", cfg.trajectory.clock_drift_sps);

    if (!seed_ok) return fail(seed_err);
    return true;
}

} // namespace sim
} // namespace GnssProcessor
