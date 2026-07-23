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

#include <cctype>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <limits>
#include <string>

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
    cfg.slips.taper_start_frac = jval(s, "cycle_slips.taper_start_frac","", cfg.slips.taper_start_frac);

    cfg.atmosphere.zwd_m       = jval(s, "atmosphere.zwd_m",       "", cfg.atmosphere.zwd_m);
    cfg.biases.isb_gps_gal_m   = jval(s, "biases.isb_gps_gal_m",   "", cfg.biases.isb_gps_gal_m);
    cfg.geometry.elevation_mask_deg = jval(s, "geometry.elevation_mask_deg", "", cfg.geometry.elevation_mask_deg);

    // M7 full-correction stack (all default false -> byte-identical bare dataset).
    cfg.corrections.relativity       = jval(s, "corrections.relativity",       "", cfg.corrections.relativity);
    cfg.corrections.shapiro          = jval(s, "corrections.shapiro",          "", cfg.corrections.shapiro);
    cfg.corrections.solid_earth_tide = jval(s, "corrections.solid_earth_tide", "", cfg.corrections.solid_earth_tide);
    cfg.corrections.pole_tide        = jval(s, "corrections.pole_tide",        "", cfg.corrections.pole_tide);
    cfg.corrections.sat_antenna      = jval(s, "corrections.sat_antenna",      "", cfg.corrections.sat_antenna);
    // Receiver antenna type: the RINEX "ANT # / TYPE" field (16-char model + 4-char
    // radome).  Empty = OFF.  Doubles as the RINEX header string, so a single source
    // of truth guarantees the declared antenna IS the injected one.
    if (const nlohmann::json* rx = resolvePath(s, "corrections.rx_antenna_type")) {
        if (!rx->is_string())
            return fail("sim config 'corrections.rx_antenna_type' must be a string "
                        "(RINEX ANT # / TYPE field, e.g. \"TRM59800.00     NONE\")");
        cfg.corrections.rx_antenna_type = rx->get<std::string>();
        if (cfg.corrections.rx_antenna_type.size() > 20)
            return fail("sim config 'corrections.rx_antenna_type' is " +
                        std::to_string(cfg.corrections.rx_antenna_type.size()) +
                        " chars (> 20; the RINEX field is a 16-char model + 4-char radome)");
    }

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
    cfg.iono.tid_period_start_s     = jval(s, "iono.tid_period_start_s",     "", cfg.iono.tid_period_start_s);
    cfg.iono.tid_period_end_s       = jval(s, "iono.tid_period_end_s",       "", cfg.iono.tid_period_end_s);
    cfg.iono.sed_elev_exp           = jval(s, "iono.sed_elev_exp",           "", cfg.iono.sed_elev_exp);
    cfg.iono.tid_positive           = jval(s, "iono.tid_positive",           "", cfg.iono.tid_positive);
    cfg.iono.storm_active_fraction  = jval(s, "iono.storm_active_fraction",  "", cfg.iono.storm_active_fraction);
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
    // Optional landfall mode (both dest_* finite => enabled; NaN default keeps the
    // analytic model byte-identical).  See TrajectoryGenerator.hpp.
    cfg.trajectory.dest_lat_deg    = jval(s, "trajectory.dest_lat_deg",    "", cfg.trajectory.dest_lat_deg);
    cfg.trajectory.dest_lon_deg    = jval(s, "trajectory.dest_lon_deg",    "", cfg.trajectory.dest_lon_deg);
    cfg.trajectory.start_tow_s = jval(s, "trajectory.start_tow_s", "", cfg.trajectory.start_tow_s);
    // NaN-safe range check (the !(x >= 0) form rejects NaN as well as negatives).
    if (!(cfg.trajectory.start_tow_s >= 0.0) || cfg.trajectory.start_tow_s >= 604800.0)
        return fail("sim config 'trajectory.start_tow_s' must be within [0, 604800) seconds of the GPS week");
    cfg.trajectory.landfall_decel_frac    = jval(s, "trajectory.landfall_decel_frac",    "", cfg.trajectory.landfall_decel_frac);
    cfg.trajectory.landfall_terminal_frac = jval(s, "trajectory.landfall_terminal_frac", "", cfg.trajectory.landfall_terminal_frac);
    cfg.trajectory.approach_weave_amp_m   = jval(s, "trajectory.approach_weave_amp_m",   "", cfg.trajectory.approach_weave_amp_m);
    cfg.trajectory.approach_weave_period_s= jval(s, "trajectory.approach_weave_period_s","", cfg.trajectory.approach_weave_period_s);
    // Optional meander waypoints: an array of [lat_deg, lon_deg] pairs.
    if (const nlohmann::json* wps = resolvePath(s, "trajectory.waypoints")) {
        if (!wps->is_array())
            return fail("sim config 'trajectory.waypoints' must be an array of [lat,lon] pairs");
        if (wps->size() > 64)
            return fail("sim config 'trajectory.waypoints' has " + std::to_string(wps->size()) +
                        " entries (> 64; paste-accident guard)");
        for (const auto& w : *wps) {
            if (!w.is_array() || w.size() != 2 || !w[0].is_number() || !w[1].is_number())
                return fail("each 'trajectory.waypoints' entry must be [lat_deg, lon_deg] numbers");
            cfg.trajectory.waypoints.push_back({ w[0].get<double>(), w[1].get<double>() });
        }
    }

    if (!seed_ok) return fail(seed_err);

    // --- Scripted "gotcha" cycle slips (optional; SPEC_scripted_slips.md §3). ---
    // Absent OR an empty array leaves cfg.slips.scripted empty, so the synthesis is
    // byte-identical to the random-only dataset.  Every event is fully validated
    // here (SimConfig.hpp stays a plain resolved record); a rejected event is a
    // loud error, never a silently-dropped exam fixture.
    if (const nlohmann::json* arr = resolvePath(s, "cycle_slips.scripted")) {
        if (!arr->is_array())
            return fail("sim config 'cycle_slips.scripted' must be an array");
        if (arr->size() > 64)
            return fail("sim config 'cycle_slips.scripted' has " + std::to_string(arr->size()) +
                        " events (> 64; paste-accident guard)");

        // ^[GE]\d\d$ (system letter G or E + two digits).
        auto validSat = [](const std::string& id) -> bool {
            return id.size() == 3 && (id[0] == 'G' || id[0] == 'E') &&
                   std::isdigit(static_cast<unsigned char>(id[1])) &&
                   std::isdigit(static_cast<unsigned char>(id[2]));
        };
        // PHASE codes only (scripting a code observable is meaningless here).
        auto validPhase = [](char sys, const std::string& code) -> bool {
            if (sys == 'G') return code == "L1C" || code == "L2W";
            if (sys == 'E') return code == "L1C" || code == "L5Q";
            return false;
        };
        // Tolerant int read: accept 20 OR 20.0 (nlohmann get<int> throws on a float
        // literal — the crash class already fixed once for the other int knobs).
        // Saturate out-of-int-range magnitudes to INT_MAX/MIN so a pathological
        // literal (e.g. 2^32+50) can NOT wrap (mod 2^32) into an in-range value and
        // silently bypass the |delta|/epoch/repeat guards below — instead the
        // saturated value trips those range checks and is rejected loudly.
        auto toInt = [](const nlohmann::json& v) -> int {
            const long long r = std::llround(v.get<double>());
            if (r > std::numeric_limits<int>::max()) return std::numeric_limits<int>::max();
            if (r < std::numeric_limits<int>::min()) return std::numeric_limits<int>::min();
            return static_cast<int>(r);
        };

        const int n_ep = cfg.trajectory.n_epochs;
        int idx = 0;
        for (const auto& el : *arr) {
            const std::string where = "cycle_slips.scripted[" + std::to_string(idx++) + "]";
            if (!el.is_object())
                return fail(where + " must be a JSON object");

            ScriptedSlip ev;

            // sat (required).
            if (!el.contains("sat") || !el["sat"].is_string())
                return fail(where + ".sat is required (string, e.g. \"G05\")");
            ev.sat = el["sat"].get<std::string>();
            if (!validSat(ev.sat))
                return fail(where + ".sat '" + ev.sat + "' must match ^[GE][0-9][0-9]$");

            // signal (required; a phase code of that system).
            if (!el.contains("signal") || !el["signal"].is_string())
                return fail(where + ".signal is required (a phase code)");
            ev.signal = el["signal"].get<std::string>();
            if (!validPhase(ev.sat[0], ev.signal))
                return fail(where + ".signal '" + ev.signal + "' is not a phase code for system " +
                            std::string(1, ev.sat[0]) + " (GPS: L1C|L2W, Galileo: L1C|L5Q)");

            // Trigger: exactly one of epoch (form A) / elev_deg (form B).
            const bool has_epoch = el.contains("epoch");
            const bool has_elev  = el.contains("elev_deg");
            const bool has_nbe   = el.contains("not_before_epoch");
            if (has_epoch == has_elev)
                return fail(where + ": exactly one of 'epoch' or 'elev_deg' must be given");
            if (has_epoch && has_nbe)
                return fail(where + ": 'not_before_epoch' only modifies the 'elev_deg' trigger and "
                                    "must not appear with 'epoch'");

            if (has_epoch) {
                if (!el["epoch"].is_number())
                    return fail(where + ".epoch must be a number");
                ev.epoch = toInt(el["epoch"]);
                if (ev.epoch < 1 || ev.epoch > n_ep)
                    return fail(where + ".epoch " + std::to_string(ev.epoch) +
                                " out of range [1, " + std::to_string(n_ep) + "]");
            } else {
                const auto& e = el["elev_deg"];
                if (!e.is_array() || e.size() != 2 || !e[0].is_number() || !e[1].is_number())
                    return fail(where + ".elev_deg must be a 2-element numeric array [lo, hi)");
                ev.elev_lo_deg = e[0].get<double>();
                ev.elev_hi_deg = e[1].get<double>();
                if (!(ev.elev_lo_deg >= 0.0 && ev.elev_lo_deg < ev.elev_hi_deg && ev.elev_hi_deg <= 90.0))
                    return fail(where + ".elev_deg must satisfy 0 <= lo < hi <= 90");
                if (has_nbe) {
                    if (!el["not_before_epoch"].is_number())
                        return fail(where + ".not_before_epoch must be a number");
                    ev.not_before_epoch = toInt(el["not_before_epoch"]);
                }
                if (ev.not_before_epoch < 1 || ev.not_before_epoch > n_ep)
                    return fail(where + ".not_before_epoch " + std::to_string(ev.not_before_epoch) +
                                " out of range [1, " + std::to_string(n_ep) + "]");
            }

            // delta_cycles (required signed int, |delta| <= 100000).
            if (!el.contains("delta_cycles"))
                return fail(where + ".delta_cycles is required");
            if (!el["delta_cycles"].is_number())
                return fail(where + ".delta_cycles must be a number");
            ev.delta_cycles = toInt(el["delta_cycles"]);
            if (std::abs(ev.delta_cycles) > 100000)
                return fail(where + ".delta_cycles " + std::to_string(ev.delta_cycles) +
                            " exceeds |delta| <= 100000");

            // flag_lli (optional, default false).
            if (el.contains("flag_lli")) {
                if (!el["flag_lli"].is_boolean())
                    return fail(where + ".flag_lli must be a boolean");
                ev.flag_lli = el["flag_lli"].get<bool>();
            }
            // delta == 0 is an LLI-only false alarm — only meaningful WITH the flag.
            if (ev.delta_cycles == 0 && !ev.flag_lli)
                return fail(where + ": delta_cycles == 0 is only allowed with flag_lli=true "
                                    "(LLI-only false alarm)");

            // repeat (optional, default 1, [1, n_epochs]).
            if (el.contains("repeat")) {
                if (!el["repeat"].is_number())
                    return fail(where + ".repeat must be a number");
                ev.repeat = toInt(el["repeat"]);
            }
            if (ev.repeat < 1 || ev.repeat > n_ep)
                return fail(where + ".repeat " + std::to_string(ev.repeat) +
                            " out of range [1, " + std::to_string(n_ep) + "]");

            cfg.slips.scripted.push_back(std::move(ev));
        }
    }

    return true;
}

} // namespace sim
} // namespace GnssProcessor
