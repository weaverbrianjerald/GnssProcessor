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
#include <algorithm>
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

    // Reads an int knob as either 120 or 120.0 (nlohmann's get<int> throws on a float
    // literal) and rounds to nearest, matching the double semantics of the other knobs.
    auto jint = [&](const char* path, int dflt) -> int {
        return static_cast<int>(std::llround(jval<double>(s, path, "", static_cast<double>(dflt))));
    };
    // Reads a non-negative uint64 seed.  A negative JSON literal is an error rather
    // than a value wrapped to about 1.8e19.
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
    cfg.multipath.code_amp_gps_l5_m  = jval(s, "multipath.code_amp_gps_l5_m",  "", cfg.multipath.code_amp_gps_l5_m);
    cfg.multipath.code_amp_gal_e5b_m = jval(s, "multipath.code_amp_gal_e5b_m", "", cfg.multipath.code_amp_gal_e5b_m);
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
    // Time-varying truth ZWD (all knobs default 0 => constant, byte-identical).
    cfg.atmosphere.zwd_diurnal_amp_m    = jval(s, "atmosphere.zwd_diurnal_amp_m",    "", cfg.atmosphere.zwd_diurnal_amp_m);
    cfg.atmosphere.zwd_diurnal_period_s = jval(s, "atmosphere.zwd_diurnal_period_s", "", cfg.atmosphere.zwd_diurnal_period_s);
    cfg.atmosphere.zwd_diurnal_phase_s  = jval(s, "atmosphere.zwd_diurnal_phase_s",  "", cfg.atmosphere.zwd_diurnal_phase_s);
    cfg.atmosphere.zwd_rw_sigma_m       = jval(s, "atmosphere.zwd_rw_sigma_m",       "", cfg.atmosphere.zwd_rw_sigma_m);
    cfg.atmosphere.zwd_rw_tau_s         = jval(s, "atmosphere.zwd_rw_tau_s",         "", cfg.atmosphere.zwd_rw_tau_s);
    if (!(cfg.atmosphere.zwd_rw_tau_s > 0.0))
        return fail("sim config 'atmosphere.zwd_rw_tau_s' must be > 0");
    cfg.atmosphere.zwd_min_m            = jval(s, "atmosphere.zwd_min_m",            "", cfg.atmosphere.zwd_min_m);
    if (!(cfg.atmosphere.zwd_diurnal_period_s > 0.0))
        return fail("sim config 'atmosphere.zwd_diurnal_period_s' must be > 0");
    if (!(cfg.atmosphere.zwd_diurnal_amp_m >= 0.0))
        return fail("sim config 'atmosphere.zwd_diurnal_amp_m' must be >= 0");
    if (!(cfg.atmosphere.zwd_rw_sigma_m >= 0.0))
        return fail("sim config 'atmosphere.zwd_rw_sigma_m' must be >= 0");
    if (!(cfg.atmosphere.zwd_min_m >= 0.0))
        return fail("sim config 'atmosphere.zwd_min_m' must be >= 0");
    if (const nlohmann::json* sd = resolvePath(s, "atmosphere.seed")) {
        if (!sd->is_number() || sd->get<double>() < 0.0)
            return fail("sim config 'atmosphere.seed' must be a non-negative integer");
        cfg.atmosphere.seed = static_cast<std::uint64_t>(sd->get<double>());
    }

    cfg.biases.isb_gps_gal_m   = jval(s, "biases.isb_gps_gal_m",   "", cfg.biases.isb_gps_gal_m);
    cfg.geometry.elevation_mask_deg = jval(s, "geometry.elevation_mask_deg", "", cfg.geometry.elevation_mask_deg);

    // --- Per-observable receiver biases (SPEC_synthetic_minimum_network.md §3). ---
    // Shape: "biases.rx_code_bias": [ {"code":"C1C","value_m":0.42}, ... ].  Absent or
    // empty leaves the lists empty, so the synthesis is byte-identical.  Validation is
    // done here (the header stays a plain resolved record): an unparseable entry is an
    // error, because a dropped bias would make the Table-8 identity read 0 == 0.
    {
        auto loadRxBias = [&](const char* key, std::vector<RxBias>& out) -> const char* {
            const nlohmann::json* arr = resolvePath(s, key);
            if (!arr) return nullptr;
            if (!arr->is_array()) return "must be an array";
            if (arr->size() > 64) return "has more than 64 entries (paste-accident guard)";
            for (const auto& el : *arr) {
                if (!el.is_object()) return "entries must be JSON objects";
                if (!el.contains("code") || !el["code"].is_string())
                    return "each entry needs a string 'code' (a RINEX-3 observation code)";
                RxBias b;
                b.code = el["code"].get<std::string>();
                if (b.code.size() != 3)
                    return "each 'code' must be a 3-character RINEX-3 observation code";
                if (!el.contains("value_m") || !el["value_m"].is_number())
                    return "each entry needs a numeric 'value_m'";
                b.value_m = el["value_m"].get<double>();
                if (!std::isfinite(b.value_m)) return "'value_m' must be finite";
                for (const auto& prev : out)
                    if (prev.code == b.code) return "lists a duplicate observation code";
                out.push_back(b);
            }
            return nullptr;
        };
        if (const char* why = loadRxBias("biases.rx_code_bias", cfg.biases.rx_code_bias))
            return fail(std::string("sim config 'biases.rx_code_bias' ") + why);
        if (const char* why = loadRxBias("biases.rx_phase_bias", cfg.biases.rx_phase_bias))
            return fail(std::string("sim config 'biases.rx_phase_bias' ") + why);
    }

    // Full-correction stack (M7).  All default false, giving a byte-identical bare dataset.
    cfg.corrections.relativity       = jval(s, "corrections.relativity",       "", cfg.corrections.relativity);
    cfg.corrections.shapiro          = jval(s, "corrections.shapiro",          "", cfg.corrections.shapiro);
    cfg.corrections.solid_earth_tide = jval(s, "corrections.solid_earth_tide", "", cfg.corrections.solid_earth_tide);
    cfg.corrections.pole_tide        = jval(s, "corrections.pole_tide",        "", cfg.corrections.pole_tide);
    cfg.corrections.sat_antenna      = jval(s, "corrections.sat_antenna",      "", cfg.corrections.sat_antenna);
    cfg.corrections.signal_bias      = jval(s, "corrections.signal_bias",      "", cfg.corrections.signal_bias);
    cfg.corrections.phase_windup     = jval(s, "corrections.phase_windup",     "", cfg.corrections.phase_windup);
    cfg.corrections.ocean_tide       = jval(s, "corrections.ocean_tide",       "", cfg.corrections.ocean_tide);
    if (const nlohmann::json* nc = resolvePath(s, "corrections.eot20_nc_path")) {
        if (!nc->is_string())
            return fail("sim config 'corrections.eot20_nc_path' must be a string (path to EOT20_load.nc)");
        cfg.corrections.eot20_nc_path = nc->get<std::string>();
    }
    if (cfg.corrections.ocean_tide && cfg.corrections.eot20_nc_path.empty())
        return fail("sim config 'corrections.ocean_tide' is set but 'corrections.eot20_nc_path' is empty "
                    "(the EOT20 global load grid supplies the station coefficients)");

    // Third-band opt-in (Odijk 2016 §5.1: the slant-ionosphere posture has no estimable
    // code bias below three frequencies).  Default false on both => the band plan, and
    // therefore every RNG draw, is byte-identical to the two-band fixture.
    cfg.signals.gps_l5      = jval(s, "signals.gps_l5",      "", cfg.signals.gps_l5);
    cfg.signals.galileo_e5b = jval(s, "signals.galileo_e5b", "", cfg.signals.galileo_e5b);
    if (const nlohmann::json* so = resolvePath(s, "signals.synth_osb_amp_m")) {
        if (!so->is_object())
            return fail("sim config 'signals.synth_osb_amp_m' must be an object mapping "
                        "RINEX code -> zenith amplitude in metres, e.g. "
                        "{\"C5Q\": 0.60, \"L5Q\": 0.02}");
        for (auto it = so->begin(); it != so->end(); ++it) {
            if (!it.value().is_number())
                return fail("sim config 'signals.synth_osb_amp_m." + it.key() +
                            "' must be a number (metres)");
            const std::string code = it.key();
            // A 3-character RINEX observation code; anything else is a typo that would
            // otherwise sit in the map matching nothing and inject silently zero.
            if (code.size() != 3 || (code[0] != 'C' && code[0] != 'L'))
                return fail("sim config 'signals.synth_osb_amp_m' key '" + code +
                            "' is not a 3-character RINEX code/phase observable "
                            "(e.g. C5Q or L5Q)");
            cfg.signals.synth_osb_amp_m[code] = it.value().get<double>();
        }
    }
    if (!cfg.signals.synth_osb_amp_m.empty() && !cfg.corrections.signal_bias) {
        // The synthetic OSB uses the injection site of the product OSB, which is gated
        // on corrections.signal_bias.  Accepting it here would produce a fixture that
        // cannot be told apart from one whose third band has no bias.
        return fail("sim config 'signals.synth_osb_amp_m' is set but "
                    "'corrections.signal_bias' is false — the synthetic OSB is injected "
                    "at the same site as the product OSB and would be silently inert. "
                    "Set corrections.signal_bias = true (the W6 bias posture) or drop "
                    "the synthetic amplitudes.");
    }

    // ---- Integer-ambiguity regime -------------------------------------------------
    // Default "legacy" (N in [1e5,1e6)) so every existing fixture regenerates
    // byte-identically.  "field" (N in [-W,+W]) is required for any experiment against a
    // zero-target datum.  include/sim/AmbiguityDraw.hpp gives the measured IGS spread
    // this is calibrated against and why the four-decade gap matters.
    if (const nlohmann::json* ar = resolvePath(s, "ambiguity.regime")) {
        if (!ar->is_string())
            return fail("sim config 'ambiguity.regime' must be a string: "
                        "\"legacy\" (N in [1e5,1e6), the default) or \"field\" "
                        "(N in [-half_width_cycles, +half_width_cycles])");
        const std::string regime = ar->get<std::string>();
        if (regime == "legacy")      cfg.ambiguity.regime = AmbiguityRegime::Legacy;
        else if (regime == "field")  cfg.ambiguity.regime = AmbiguityRegime::Field;
        else
            return fail("sim config 'ambiguity.regime' is '" + regime +
                        "'; expected \"legacy\" or \"field\"");
    }
    if (const nlohmann::json* aw = resolvePath(s, "ambiguity.half_width_cycles")) {
        if (!aw->is_number())
            return fail("sim config 'ambiguity.half_width_cycles' must be a number (cycles)");
        // Under the legacy regime this knob has no effect, so a fixture generated with
        // it set would look field-realistic in its config and be 1e5 cycles wide on
        // disk.  Setting it outside the field regime is therefore an error.
        if (cfg.ambiguity.regime != AmbiguityRegime::Field)
            return fail("sim config 'ambiguity.half_width_cycles' is set but "
                        "'ambiguity.regime' is not \"field\" — under the legacy regime the "
                        "half-width is inert and the fixture would still draw N in "
                        "[1e5,1e6).  Set ambiguity.regime = \"field\" or drop the half-width.");
        const long long w = std::llround(aw->get<double>());
        // The upper bound matches the ScriptedSlip |delta_cycles| guard (100000).  Both
        // quantities are in cycles in the same truth files, and a larger half-width
        // would make an injected slip look small next to the ambiguity draw itself.
        if (w < 1 || w > 100000)
            return fail("sim config 'ambiguity.half_width_cycles' " + std::to_string(w) +
                        " out of range [1, 100000]");
        cfg.ambiguity.half_width_cycles = static_cast<int>(w);
    }

    cfg.corrections.ocean_tide_along_track =
        jval(s, "corrections.ocean_tide_along_track", "", cfg.corrections.ocean_tide_along_track);
    cfg.corrections.otl_anchor_spacing_km =
        jval(s, "corrections.otl_anchor_spacing_km", "", cfg.corrections.otl_anchor_spacing_km);
    // An error rather than a silent fallback: in the along-track mode the term does
    // not cancel, and a config that fell back to the cancelling path would give no
    // sign of it in the output.
    if (cfg.corrections.ocean_tide_along_track && !cfg.corrections.ocean_tide)
        return fail("sim config 'corrections.ocean_tide_along_track' is set but "
                    "'corrections.ocean_tide' is false (the along-track field extends the "
                    "ocean-loading term; it cannot be injected on its own)");
    if (!(cfg.corrections.otl_anchor_spacing_km > 0.0))
        return fail("sim config 'corrections.otl_anchor_spacing_km' must be > 0 km");
    // Receiver antenna type: the RINEX "ANT # / TYPE" field (16-char model + 4-char
    // radome).  Empty means off.  The same string is written to the RINEX header, so
    // the declared antenna and the injected one always agree.
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
    // Absolute storm anchor.  The NaN default derives the centre from this run's own
    // trajectory origin (legacy).  A network run has to set both, otherwise every
    // station sits at the blob centre; see SimConfig.hpp Iono::center_lat_deg.
    cfg.iono.center_lat_deg = jval(s, "iono.center_lat_deg", "", cfg.iono.center_lat_deg);
    cfg.iono.center_lon_deg = jval(s, "iono.center_lon_deg", "", cfg.iono.center_lon_deg);
    if (std::isfinite(cfg.iono.center_lat_deg) !=
        std::isfinite(cfg.iono.center_lon_deg))
        return fail("sim config 'iono.center_lat_deg' and 'iono.center_lon_deg' must be "
                    "set together (an absolute storm anchor needs both components)");
    if (std::isfinite(cfg.iono.center_lat_deg) &&
        (cfg.iono.center_lat_deg < -90.0 || cfg.iono.center_lat_deg > 90.0))
        return fail("sim config 'iono.center_lat_deg' must lie in [-90, 90]");
    cfg.iono.tid_wavelength_km      = jval(s, "iono.tid_wavelength_km",      "", cfg.iono.tid_wavelength_km);
    cfg.iono.tid_period_s           = jval(s, "iono.tid_period_s",           "", cfg.iono.tid_period_s);
    cfg.iono.tid_period_start_s     = jval(s, "iono.tid_period_start_s",     "", cfg.iono.tid_period_start_s);
    cfg.iono.tid_period_end_s       = jval(s, "iono.tid_period_end_s",       "", cfg.iono.tid_period_end_s);
    cfg.iono.tid_pulse_count        = jint("iono.tid_pulse_count",        cfg.iono.tid_pulse_count);
    cfg.iono.tid_pulse_duration_s   = jval(s, "iono.tid_pulse_duration_s",   "", cfg.iono.tid_pulse_duration_s);
    cfg.iono.tid_fast_period_s      = jval(s, "iono.tid_fast_period_s",      "", cfg.iono.tid_fast_period_s);
    if (const nlohmann::json* ts = resolvePath(s, "iono.tid_sats")) {
        if (!ts->is_array())
            return fail("sim config 'iono.tid_sats' must be an array of PRN strings (e.g. [\"E13\",\"G18\"])");
        cfg.iono.tid_sats.clear();
        for (const auto& e : *ts) {
            if (!e.is_string())
                return fail("sim config 'iono.tid_sats' entries must be PRN strings (e.g. \"G18\")");
            cfg.iono.tid_sats.push_back(e.get<std::string>());
        }
    }
    if (const nlohmann::json* us = resolvePath(s, "iono.turb_sats")) {
        if (!us->is_array())
            return fail("sim config 'iono.turb_sats' must be an array of PRN strings (e.g. [\"E13\",\"G18\"])");
        cfg.iono.turb_sats.clear();
        for (const auto& e : *us) {
            if (!e.is_string())
                return fail("sim config 'iono.turb_sats' entries must be PRN strings (e.g. \"G18\")");
            cfg.iono.turb_sats.push_back(e.get<std::string>());
        }
    }
    cfg.iono.sed_bulk = jval(s, "iono.sed_bulk", "", cfg.iono.sed_bulk);
    cfg.iono.turb_sigma_tecu = jval(s, "iono.turb_sigma_tecu", "", cfg.iono.turb_sigma_tecu);
    if (cfg.iono.turb_sigma_tecu < 0.0)
        return fail("sim config 'iono.turb_sigma_tecu' must be >= 0");
    cfg.iono.turb_step_s = jval(s, "iono.turb_step_s", "", cfg.iono.turb_step_s);
    cfg.iono.turb_window_start_frac =
        jval(s, "iono.turb_window_start_frac", "", cfg.iono.turb_window_start_frac);
    cfg.iono.turb_window_duration_frac =
        jval(s, "iono.turb_window_duration_frac", "", cfg.iono.turb_window_duration_frac);
    if (const nlohmann::json* fs = resolvePath(s, "iono.tid_fast_sat")) {
        if (!fs->is_string())
            return fail("sim config 'iono.tid_fast_sat' must be a PRN string (e.g. \"G23\")");
        cfg.iono.tid_fast_sat = fs->get<std::string>();
    }
    // The fast satellite must be one of the TID satellites, or its flicker is never applied.
    if (!cfg.iono.tid_fast_sat.empty() && !cfg.iono.tid_sats.empty() &&
        std::find(cfg.iono.tid_sats.begin(), cfg.iono.tid_sats.end(), cfg.iono.tid_fast_sat) == cfg.iono.tid_sats.end())
        return fail("sim config 'iono.tid_fast_sat' (" + cfg.iono.tid_fast_sat +
                    ") is not in 'iono.tid_sats' — it would carry no TID to flicker");
    // Explicit per-satellite TID pulses (the richest overlay control; supersedes the above).
    if (const nlohmann::json* tp = resolvePath(s, "iono.tid_pulses")) {
        if (!tp->is_array())
            return fail("sim config 'iono.tid_pulses' must be an array of pulse objects");
        cfg.iono.tid_pulses.clear();
        for (const auto& e : *tp) {
            if (!e.is_object())
                return fail("sim config 'iono.tid_pulses' entries must be objects "
                            "{sat, center_tow_s, amp_tecu, period_s, duration_s}");
            SimConfig::Iono::TidPulse p;
            if (!e.contains("sat") || !e["sat"].is_string())
                return fail("sim config 'iono.tid_pulses[].sat' (PRN string) is required");
            p.sat = e["sat"].get<std::string>();
            auto num = [&](const char* key, double dflt, bool required) -> double {
                if (!e.contains(key)) {
                    if (required) { seed_ok = false; seed_err = std::string("sim config 'iono.tid_pulses[].") + key + "' is required"; }
                    return dflt;
                }
                return e[key].is_number() ? e[key].get<double>() : dflt;
            };
            p.center_tow_s = num("center_tow_s", 0.0, true);
            p.amp_tecu     = num("amp_tecu", 0.0, true);
            p.period_s     = num("period_s", 0.0, false);      // <=0 => smooth bump
            p.duration_s   = num("duration_s", 600.0, false);
            if (!seed_ok) return fail(seed_err);
            if (!(p.duration_s > 0.0))
                return fail("sim config 'iono.tid_pulses[].duration_s' must be > 0 (sat " + p.sat + ")");
            if (!(p.amp_tecu >= 0.0))
                return fail("sim config 'iono.tid_pulses[].amp_tecu' must be >= 0 (sat " + p.sat + ")");
            cfg.iono.tid_pulses.push_back(std::move(p));
        }
    }
    cfg.iono.sed_elev_exp           = jval(s, "iono.sed_elev_exp",           "", cfg.iono.sed_elev_exp);
    cfg.iono.tid_positive           = jval(s, "iono.tid_positive",           "", cfg.iono.tid_positive);
    cfg.iono.storm_active_fraction  = jval(s, "iono.storm_active_fraction",  "", cfg.iono.storm_active_fraction);
    cfg.iono.tid_azimuth_deg        = jval(s, "iono.tid_azimuth_deg",        "", cfg.iono.tid_azimuth_deg);
    cfg.iono.shell_height_km        = jval(s, "iono.shell_height_km",        "", cfg.iono.shell_height_km);
    cfg.iono.scint_elev_clamp_deg   = jval(s, "iono.scint_elev_clamp_deg",   "", cfg.iono.scint_elev_clamp_deg);

    // Quiet-background source.  An unknown value is an error, so a typo cannot fall
    // back to Klobuchar and produce a dataset that only appears to be a GIM run.
    if (const nlohmann::json* bs = resolvePath(s, "iono.background_source")) {
        if (!bs->is_string())
            return fail("sim config 'iono.background_source' must be a string "
                        "(\"klobuchar\" | \"gim\" | \"analytic\")");
        cfg.iono.background_source = bs->get<std::string>();
        if (cfg.iono.background_source != "klobuchar" &&
            cfg.iono.background_source != "gim" &&
            cfg.iono.background_source != "analytic")
            return fail("sim config 'iono.background_source' is '" + cfg.iono.background_source +
                        "'; must be one of \"klobuchar\", \"gim\", \"analytic\"");
    }
    if (const nlohmann::json* gp = resolvePath(s, "iono.gim_path")) {
        if (!gp->is_string())
            return fail("sim config 'iono.gim_path' must be a string (path to a decompressed IONEX .INX)");
        cfg.iono.gim_path = gp->get<std::string>();
    }

    cfg.date.year = jint("date.year", cfg.date.year);
    cfg.date.doy  = jint("date.doy",  cfg.date.doy);
    if (cfg.date.year < 1980 || cfg.date.year > 2200)
        return fail("sim config 'date.year' = " + std::to_string(cfg.date.year) + " is out of range [1980, 2200]");
    if (cfg.date.doy < 1 || cfg.date.doy > 366)
        return fail("sim config 'date.doy' = " + std::to_string(cfg.date.doy) + " is out of range [1, 366]");

    cfg.trajectory.interval_s      = jval(s, "trajectory.interval_s",      "", cfg.trajectory.interval_s);
    cfg.trajectory.n_epochs        = jint("trajectory.n_epochs",        cfg.trajectory.n_epochs);
    cfg.trajectory.cruise_speed_mps= jval(s, "trajectory.cruise_speed_mps","", cfg.trajectory.cruise_speed_mps);
    cfg.trajectory.lat0_deg        = jval(s, "trajectory.lat0_deg",        "", cfg.trajectory.lat0_deg);
    cfg.trajectory.lon0_deg        = jval(s, "trajectory.lon0_deg",        "", cfg.trajectory.lon0_deg);
    cfg.trajectory.h0_m            = jval(s, "trajectory.h0_m",            "", cfg.trajectory.h0_m);
    cfg.trajectory.heading0_deg    = jval(s, "trajectory.heading0_deg",    "", cfg.trajectory.heading0_deg);
    cfg.trajectory.heading_amp_deg = jval(s, "trajectory.heading_amp_deg", "", cfg.trajectory.heading_amp_deg);
    cfg.trajectory.heading_period_s= jval(s, "trajectory.heading_period_s","", cfg.trajectory.heading_period_s);
    cfg.trajectory.static_prefix_s = jval(s, "trajectory.static_prefix_s", "", cfg.trajectory.static_prefix_s);
    cfg.trajectory.heave_amp_m     = jval(s, "trajectory.heave_amp_m",     "", cfg.trajectory.heave_amp_m);
    cfg.trajectory.heave_period_s  = jval(s, "trajectory.heave_period_s",  "", cfg.trajectory.heave_period_s);
    cfg.trajectory.clock_bias0_s   = jval(s, "trajectory.clock_bias0_s",   "", cfg.trajectory.clock_bias0_s);
    cfg.trajectory.clock_drift_sps = jval(s, "trajectory.clock_drift_sps", "", cfg.trajectory.clock_drift_sps);
    // Receiver clock random walk (m^2/s, the PPP-filter unit).  A negative spectral
    // density is an error: its sqrt() would be NaN and would corrupt every
    // observation in the file without failing the run.
    cfg.trajectory.clock_rw_q_m2ps = jval(s, "trajectory.clock_rw_q_m2ps", "", cfg.trajectory.clock_rw_q_m2ps);
    if (!(cfg.trajectory.clock_rw_q_m2ps >= 0.0))
        return fail("sim config 'trajectory.clock_rw_q_m2ps' must be >= 0 (m^2/s)");
    cfg.trajectory.clock_seed      = jseed("trajectory.clock_seed", cfg.trajectory.clock_seed);
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

    // --- RINEX header realism / leak knobs (SPEC R2). --------------------------
    // Every field is optional and every default reproduces the current header byte-for-
    // byte, so an absent block (or one of all-defaults) is byte-identical.
    if (const nlohmann::json* pm = resolvePath(s, "rinex.approx_position_mode")) {
        if (!pm->is_string())
            return fail("sim config 'rinex.approx_position_mode' must be a string (\"truth\" | \"quantized\")");
        cfg.rinex.approx_position_mode = pm->get<std::string>();
        if (cfg.rinex.approx_position_mode != "truth" &&
            cfg.rinex.approx_position_mode != "quantized")
            return fail("sim config 'rinex.approx_position_mode' is '" + cfg.rinex.approx_position_mode +
                        "'; must be \"truth\" or \"quantized\"");
    }
    if (const nlohmann::json* off = resolvePath(s, "rinex.approx_offset_enu_m")) {
        if (!off->is_array() || off->size() != 3)
            return fail("sim config 'rinex.approx_offset_enu_m' must be [east_m, north_m, up_m]");
        for (int i = 0; i < 3; ++i) {
            if (!(*off)[i].is_number())
                return fail("sim config 'rinex.approx_offset_enu_m' entries must be numbers");
            cfg.rinex.approx_offset_enu_m[i] = (*off)[i].get<double>();
        }
    }
    cfg.rinex.approx_quantize_deg = jval(s, "rinex.approx_quantize_deg", "", cfg.rinex.approx_quantize_deg);
    if (!(cfg.rinex.approx_quantize_deg > 0.0))
        return fail("sim config 'rinex.approx_quantize_deg' must be > 0 degrees");
    // Optional header strings.  Absent => keep the writer default (byte-identical); each
    // guarded to its RINEX field width so a paste cannot overrun the fixed columns.
    auto jrstr = [&](const char* pth, std::string& field, std::size_t maxw) {
        const nlohmann::json* p = resolvePath(s, pth);
        if (!p) return;                              // absent: keep default
        if (!p->is_string()) { seed_ok = false; seed_err = std::string("sim config '") + pth + "' must be a string"; return; }
        std::string v = p->get<std::string>();
        if (v.size() > maxw) {
            seed_ok = false;
            seed_err = std::string("sim config '") + pth + "' is " + std::to_string(v.size()) +
                       " chars (> " + std::to_string(maxw) + "; the RINEX field width)";
            return;
        }
        field = v;
    };
    jrstr("rinex.pgm",      cfg.rinex.pgm,      20);
    jrstr("rinex.run_by",   cfg.rinex.run_by,   20);
    jrstr("rinex.observer", cfg.rinex.observer, 20);
    jrstr("rinex.agency",   cfg.rinex.agency,   40);   // OBSERVER/AGENCY: agency spans cols 20-59
    if (const nlohmann::json* fm = resolvePath(s, "rinex.file_date_mode")) {
        if (!fm->is_string())
            return fail("sim config 'rinex.file_date_mode' must be a string (\"build\" | \"collection_end\")");
        cfg.rinex.file_date_mode = fm->get<std::string>();
        if (cfg.rinex.file_date_mode != "build" && cfg.rinex.file_date_mode != "collection_end")
            return fail("sim config 'rinex.file_date_mode' is '" + cfg.rinex.file_date_mode +
                        "'; must be \"build\" or \"collection_end\"");
    }
    cfg.rinex.gps_utc_leap_s = jint("rinex.gps_utc_leap_s", cfg.rinex.gps_utc_leap_s);

    if (!seed_ok) return fail(seed_err);

    // --- Scripted cycle slips (optional; SPEC_scripted_slips.md §3). ---
    // Absent or an empty array leaves cfg.slips.scripted empty, so the synthesis is
    // byte-identical to the random-only dataset.  Every event is validated here
    // (SimConfig.hpp stays a plain resolved record); a rejected event is an error,
    // not a dropped test event.
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
        // Phase codes only (a slip on a code observable has no meaning here).  A
        // third-band code is accepted only when its signals.* switch is on: a slip on a
        // band the fixture does not emit would never fire, and an event that never
        // fires cannot be told apart from one the detector missed.
        auto validPhase = [&cfg](char sys, const std::string& code) -> bool {
            if (sys == 'G')
                return code == "L1C" || code == "L2W" ||
                       (cfg.signals.gps_l5 && code == "L5Q");
            if (sys == 'E')
                return code == "L1C" || code == "L5Q" ||
                       (cfg.signals.galileo_e5b && code == "L7Q");
            return false;
        };
        // Reads an int as either 20 or 20.0 (nlohmann get<int> throws on a float
        // literal, as handled by jint above).  Magnitudes outside the int range
        // saturate to INT_MAX/MIN rather than wrapping mod 2^32 (e.g. 2^32+50 would
        // wrap to 50), so the |delta|/epoch/repeat range checks below reject them.
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
                            std::string(1, ev.sat[0]) +
                            " (GPS: L1C|L2W" + (cfg.signals.gps_l5 ? "|L5Q" : "") +
                            ", Galileo: L1C|L5Q" + (cfg.signals.galileo_e5b ? "|L7Q" : "") +
                            "; a third-band code needs its signals.* switch armed)");

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
            // delta == 0 is an LLI-only false alarm, which requires the flag.
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

    // --- Station identity of this invocation (SPEC_synthetic_minimum_network.md §1). ---
    // Absent gives an empty name and zero salt, i.e. the legacy "SARV" single-station
    // dataset, byte-for-byte.  The `synth-network` driver writes one child config per
    // station with this block filled in.
    {
        if (const nlohmann::json* nm = resolvePath(s, "station.name")) {
            if (!nm->is_string())
                return fail("sim config 'station.name' must be a string");
            cfg.station.name = nm->get<std::string>();
            if (cfg.station.name.size() > 9)
                return fail("sim config 'station.name' '" + cfg.station.name +
                            "' exceeds 9 characters (RINEX marker field)");
            for (char ch : cfg.station.name)
                if (!std::isalnum(static_cast<unsigned char>(ch)))
                    return fail("sim config 'station.name' '" + cfg.station.name +
                                "' must be alphanumeric (it becomes a filename prefix)");
        }
        if (const nlohmann::json* sd = resolvePath(s, "station.seed_salt")) {
            if (!sd->is_number() || sd->get<double>() < 0.0)
                return fail("sim config 'station.seed_salt' must be a non-negative integer");
            // Reads the integer exactly when the literal is one.  Going through a double
            // (as atmosphere.seed and sat_clock.seed do; exact only below 2^53) would
            // round a large salt and could give two stations the same salt.
            cfg.station.seed_salt = sd->is_number_unsigned()
                                        ? sd->get<std::uint64_t>()
                                        : static_cast<std::uint64_t>(sd->get<double>());
        }
    }

    // --- Network peer roster for `synth-network` (optional). --------------------
    // Shape: "network_stations": [ {"name":"SNA000SIM","lat_deg":..,"lon_deg":..,
    //                               "h_m":..,"consumer":false}, ... ]
    // Empty/absent = single-station synthesis; the other subcommands ignore it.
    if (const nlohmann::json* arr = resolvePath(s, "network_stations")) {
        if (!arr->is_array())
            return fail("sim config 'network_stations' must be an array");
        // Paste-accident guard only.  The cap spans the field's 63- and 305-station
        // lineages; larger networks (e.g. the 50-station global variant) reach QC tiers
        // that are inert on a 3- or 5-station network (raim_min_stations needs a real
        // consensus, cb_admission_min_stations = 4, rx_cb_carry_demean_min_stations = 8).
        // Each station is a separate child process, so the practical limit is
        // wall-clock time, not memory.
        if (arr->size() > 400)
            return fail("sim config 'network_stations' has " + std::to_string(arr->size()) +
                        " entries (> 400; that is beyond the largest field lineage and is "
                        "almost certainly a paste accident)");
        int idx = 0, n_consumer = 0;
        for (const auto& el : *arr) {
            const std::string where = "network_stations[" + std::to_string(idx++) + "]";
            if (!el.is_object()) return fail(where + " must be a JSON object");

            NetworkStation st;
            if (!el.contains("name") || !el["name"].is_string())
                return fail(where + ".name is required (string, 9-char marker)");
            st.name = el["name"].get<std::string>();
            if (st.name.empty() || st.name.size() > 9)
                return fail(where + ".name '" + st.name + "' must be 1-9 characters");
            for (char ch : st.name)
                if (!std::isalnum(static_cast<unsigned char>(ch)))
                    return fail(where + ".name '" + st.name + "' must be alphanumeric");
            for (const auto& prev : cfg.network_stations)
                if (prev.name == st.name)
                    return fail(where + ".name '" + st.name + "' is a duplicate — every station "
                                "needs a distinct name (it derives the RNG salt, so a duplicate "
                                "silently clones another station's whole noise realization)");

            for (const char* k : { "lat_deg", "lon_deg" })
                if (!el.contains(k) || !el[k].is_number())
                    return fail(where + "." + k + " is required (number)");
            st.lat_deg = el["lat_deg"].get<double>();
            st.lon_deg = el["lon_deg"].get<double>();
            st.h_m = el.contains("h_m") && el["h_m"].is_number() ? el["h_m"].get<double>() : 0.0;
            if (st.lat_deg < -90.0 || st.lat_deg > 90.0)
                return fail(where + ".lat_deg must lie in [-90, 90]");
            if (!std::isfinite(st.lon_deg) || !std::isfinite(st.h_m))
                return fail(where + ".lon_deg / .h_m must be finite");

            if (el.contains("consumer")) {
                if (!el["consumer"].is_boolean())
                    return fail(where + ".consumer must be a boolean");
                st.consumer = el["consumer"].get<bool>();
                if (st.consumer) ++n_consumer;
            }

            // Per-station receiver biases (they must differ across the roster, or the
            // fleet mean the Table-8 identity resolves is indistinguishable from its
            // members).  Same shape and validation as the top-level biases.rx_*_bias.
            auto loadStationRxBias = [&](const char* key,
                                         std::vector<RxBias>& out) -> const char* {
                if (!el.contains(key)) return nullptr;
                const nlohmann::json& arr = el[key];
                if (!arr.is_array()) return "must be an array";
                for (const auto& b : arr) {
                    if (!b.is_object()) return "entries must be JSON objects";
                    if (!b.contains("code") || !b["code"].is_string())
                        return "each entry needs a string 'code'";
                    RxBias rb;
                    rb.code = b["code"].get<std::string>();
                    if (rb.code.size() != 3)
                        return "each 'code' must be a 3-character RINEX-3 observation code";
                    if (!b.contains("value_m") || !b["value_m"].is_number())
                        return "each entry needs a numeric 'value_m'";
                    rb.value_m = b["value_m"].get<double>();
                    if (!std::isfinite(rb.value_m)) return "'value_m' must be finite";
                    for (const auto& prev : out)
                        if (prev.code == rb.code) return "lists a duplicate observation code";
                    out.push_back(rb);
                }
                return nullptr;
            };
            if (const char* why = loadStationRxBias("rx_code_bias", st.rx_code_bias))
                return fail(where + ".rx_code_bias " + why);
            if (const char* why = loadStationRxBias("rx_phase_bias", st.rx_phase_bias))
                return fail(where + ".rx_phase_bias " + why);

            cfg.network_stations.push_back(std::move(st));
        }
        if (n_consumer > 1)
            return fail("sim config 'network_stations' marks " + std::to_string(n_consumer) +
                        " stations as 'consumer'; at most one may be held out");
        // A 2-station "network" has no redundancy anywhere and a 1-station one is not a
        // network at all.  Catch it here rather than in the producer's rank gate, where
        // it surfaces as an under-specified datum and reads as a producer defect.
        const int n_solve = static_cast<int>(cfg.network_stations.size()) - n_consumer;
        if (!cfg.network_stations.empty() && n_solve < 3)
            return fail("sim config 'network_stations' leaves only " + std::to_string(n_solve) +
                        " station(s) in the solve (excluding the consumer); the minimum "
                        "hand-auditable network is 3");
    }

    // --- Synthetic satellite clock error (optional; the all-zero default is legacy). ---
    cfg.sat_clock.bias0_sigma_m = jval(s, "sat_clock.bias0_sigma_m", "", cfg.sat_clock.bias0_sigma_m);
    cfg.sat_clock.rate_sigma_mps = jval(s, "sat_clock.rate_sigma_mps", "", cfg.sat_clock.rate_sigma_mps);
    cfg.sat_clock.rw_q_m2ps = jval(s, "sat_clock.rw_q_m2ps", "", cfg.sat_clock.rw_q_m2ps);
    for (const auto& [nm, v] : { std::pair<const char*, double>{ "sat_clock.bias0_sigma_m", cfg.sat_clock.bias0_sigma_m },
                                 { "sat_clock.rate_sigma_mps", cfg.sat_clock.rate_sigma_mps },
                                 { "sat_clock.rw_q_m2ps", cfg.sat_clock.rw_q_m2ps } })
        if (!std::isfinite(v) || v < 0.0)
            return fail(std::string("sim config '") + nm + "' must be finite and >= 0 "
                        "(it is a 1-sigma / spectral density, not a signed value)");
    if (const nlohmann::json* sd = resolvePath(s, "sat_clock.seed")) {
        if (!sd->is_number() || sd->get<double>() < 0.0)
            return fail("sim config 'sat_clock.seed' must be a non-negative integer");
        cfg.sat_clock.seed = static_cast<std::uint64_t>(sd->get<double>());
    }
    if (const nlohmann::json* arr = resolvePath(s, "sat_clock.steps")) {
        if (!arr->is_array())
            return fail("sim config 'sat_clock.steps' must be an array");
        if (arr->size() > 64)
            return fail("sim config 'sat_clock.steps' has " + std::to_string(arr->size()) +
                        " events (> 64; paste-accident guard)");
        const int n_ep = cfg.trajectory.n_epochs;
        int idx = 0;
        for (const auto& el : *arr) {
            const std::string where = "sat_clock.steps[" + std::to_string(idx++) + "]";
            if (!el.is_object()) return fail(where + " must be a JSON object");

            SimConfig::ScriptedSatClockStep ev;
            if (!el.contains("sat") || !el["sat"].is_string())
                return fail(where + ".sat is required (string, e.g. \"G05\")");
            ev.sat = el["sat"].get<std::string>();
            if (ev.sat.size() != 3 || (ev.sat[0] != 'G' && ev.sat[0] != 'E') ||
                !std::isdigit(static_cast<unsigned char>(ev.sat[1])) ||
                !std::isdigit(static_cast<unsigned char>(ev.sat[2])))
                return fail(where + ".sat '" + ev.sat + "' must match ^[GE][0-9][0-9]$");

            if (!el.contains("epoch") || !el["epoch"].is_number())
                return fail(where + ".epoch is required (1-based epoch index)");
            ev.epoch = static_cast<int>(std::llround(el["epoch"].get<double>()));
            if (ev.epoch < 1 || ev.epoch > n_ep)
                return fail(where + ".epoch " + std::to_string(ev.epoch) + " out of range [1, " +
                            std::to_string(n_ep) + "]");

            if (el.contains("step_m")) {
                if (!el["step_m"].is_number()) return fail(where + ".step_m must be a number");
                ev.step_m = el["step_m"].get<double>();
            }
            if (el.contains("ramp_mps")) {
                if (!el["ramp_mps"].is_number()) return fail(where + ".ramp_mps must be a number");
                ev.ramp_mps = el["ramp_mps"].get<double>();
            }
            if (!std::isfinite(ev.step_m) || !std::isfinite(ev.ramp_mps))
                return fail(where + ".step_m / .ramp_mps must be finite");
            if (ev.step_m == 0.0 && ev.ramp_mps == 0.0)
                return fail(where + " has neither a step nor a ramp — an event that injects "
                            "nothing would be graded as an undetected event");

            cfg.sat_clock.steps.push_back(std::move(ev));
        }
    }

    return true;
}

std::uint64_t stationSeedSalt(const std::string& name) {
    // splitmix64 over the marker, matching the simulator's other self-contained hashes.
    auto mix = [](std::uint64_t x) {
        x += 0x9E3779B97F4A7C15ull;
        x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ull;
        x = (x ^ (x >> 27)) * 0x94D049BB133111EBull;
        return x ^ (x >> 31);
    };
    std::uint64_t h = mix(0x57A710CAFEEDull ^ name.size());
    for (unsigned char ch : name) h = mix(h ^ static_cast<std::uint64_t>(ch));
    // Truncate to 53 bits: the salt is written into a JSON child config, and a JSON
    // number is conventionally a double, whose mantissa is 53 bits.  A full 64-bit
    // value comes back rounded from that round trip, and two stations whose salts
    // differ only in the low bits would then share one noise realization.  53 bits is
    // ample for a minimum network and keeps the value exact for any reader, whether
    // it parses the number as an integer or as a double.
    h &= (1ull << 53) - 1ull;
    // 0 is the "legacy, do not salt" sentinel, so a station hash of 0 is remapped.
    return h == 0 ? 0x1F3779B97F4A7Cull : h;
}

bool writeStationConfig(const std::filesystem::path& base_config,
                        const NetworkStation& st,
                        double anchor_lat_deg, double anchor_lon_deg,
                        const std::filesystem::path& out_path,
                        std::string* err) {
    auto fail = [&](const std::string& msg) {
        if (err) *err = msg;
        return false;
    };

    nlohmann::json doc = nlohmann::json::object();
    if (!base_config.empty()) {
        std::ifstream file(base_config);
        if (!file.is_open())
            return fail("cannot open base sim config: " + base_config.string());
        try {
            doc = nlohmann::json::parse(file, nullptr, true, /*ignore_comments=*/true);
        } catch (const std::exception& e) {
            return fail("JSON parse error in " + base_config.string() + ": " + e.what());
        }
    }
    if (!doc.is_object()) doc = nlohmann::json::object();

    // Write into the same object the loader reads from ("simulator" wrapper or root),
    // otherwise the overrides would land beside the knobs instead of among them.
    nlohmann::json& s = doc.contains("simulator") && doc["simulator"].is_object()
                            ? doc["simulator"] : doc;

    s["station"] = { { "name", st.name },
                     { "seed_salt", stationSeedSalt(st.name) } };

    if (!s.contains("trajectory") || !s["trajectory"].is_object())
        s["trajectory"] = nlohmann::json::object();
    s["trajectory"]["lat0_deg"] = st.lat_deg;
    s["trajectory"]["lon0_deg"] = st.lon_deg;
    s["trajectory"]["h0_m"] = st.h_m;

    if (!s.contains("iono") || !s["iono"].is_object())
        s["iono"] = nlohmann::json::object();
    s["iono"]["center_lat_deg"] = anchor_lat_deg;
    s["iono"]["center_lon_deg"] = anchor_lon_deg;

    // Per-station receiver biases replace the base lists wholesale when the roster
    // entry declares any.  Config arrays replace rather than merge, the same rule the
    // processor's deepMergeJson follows, so the two dialects behave alike.
    if (!st.rx_code_bias.empty() || !st.rx_phase_bias.empty()) {
        if (!s.contains("biases") || !s["biases"].is_object())
            s["biases"] = nlohmann::json::object();
        auto emit = [](const std::vector<RxBias>& v) {
            nlohmann::json a = nlohmann::json::array();
            for (const auto& b : v) a.push_back({ { "code", b.code }, { "value_m", b.value_m } });
            return a;
        };
        if (!st.rx_code_bias.empty())  s["biases"]["rx_code_bias"]  = emit(st.rx_code_bias);
        if (!st.rx_phase_bias.empty()) s["biases"]["rx_phase_bias"] = emit(st.rx_phase_bias);
    }

    // Per-station scripted cycle slips.  A slip is a receiver event, and on a globally
    // distributed roster no satellite is visible at every station at a given epoch, so
    // a slip written once at the top level cannot fire everywhere: the synthesis fails
    // the stations where its satellite is below the horizon.  A roster entry may
    // therefore carry its own "scripted_slips" array, which is copied verbatim into
    // the child's cycle_slips.scripted and validated there by the top-level parser;
    // the parent does not repeat that validation.
    //
    // Per-station slips, when present, replace the base list for that station (config
    // arrays replace, they do not merge), so a roster entry opts a station out of the
    // global list by declaring its own.
    for (const nlohmann::json* rosters : { &s, &doc }) {
        if (!rosters->contains("network_stations") || !(*rosters)["network_stations"].is_array())
            continue;
        for (const auto& el : (*rosters)["network_stations"]) {
            if (!el.is_object() || !el.contains("name") || el["name"] != st.name) continue;
            if (!el.contains("scripted_slips")) continue;
            if (!s.contains("cycle_slips") || !s["cycle_slips"].is_object())
                s["cycle_slips"] = nlohmann::json::object();
            s["cycle_slips"]["scripted"] = el["scripted_slips"];
        }
    }

    // The child must not recurse, and must not think it owns a roster.
    s.erase("network_stations");
    doc.erase("network_stations");

    std::ofstream out(out_path);
    if (!out) return fail("cannot write child sim config: " + out_path.string());
    out << doc.dump(2) << "\n";
    if (!out) return fail("write failed for child sim config: " + out_path.string());
    return true;
}

} // namespace sim
} // namespace GnssProcessor
