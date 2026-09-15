/*!
 * \file GnssConfigLoad.cpp
 * \brief JSON configuration loading split into per-section helpers.
 *
 * @c GnssConfig::loadFromJson lives here as a thin orchestrator that
 * opens the file, parses the JSON once, and dispatches to one private
 * member function per top-level config section.  Each section helper
 * is independently readable and follows the same shape:
 *
 *   1. Pull a section reference via @c detail::jsection(...) (or read
 *      a few scalars directly via @c detail::jval<T>(...)).
 *   2. Populate the corresponding GnssConfig private members.
 *   3. Emit one or two summary log lines on the @c gnss_config logger.
 *
 * The hierarchical/legacy-flat fallback chain — try the new dotted
 * path, fall back to the legacy top-level key, otherwise use a default
 * — is implemented once in @c GnssConfigJsonHelpers.hpp and reused by
 * every section.
 *
 * Behavior is unchanged from the previous monolithic
 * @c GnssConfig::loadFromJson; only the file layout has moved.  The
 * commented-out "smart defaults" block from the old file (PPP-RTK
 * sat-bias / require-bia coupling) has been deleted; git history
 * preserves it should it ever need to come back.
 */

#include "config/GnssConfig.hpp"
#include "config/GnssConfigJsonHelpers.hpp"
#include "config/BaselineFixtures.hpp"
#include "common/Constants.hpp"                   // GPS_Lx_WAVELENGTH (H3 per-λ log line)
#include "math/TimeUtils.hpp"
#include "products/NetworkProductEmitter.hpp"   // kDefaultDiscThresholdM (R9 single source of truth)
#include "products/StationInfoFile.hpp"
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <fmt/format.h>
#include <fstream>
#include <nlohmann/json.hpp>
#include <optional>
#include <spdlog/spdlog.h>

// CMake-baked absolute path to the project root.  Used as the default
// for @c files.internal_data_root in @c DataSource::Internal mode so
// fresh clones run out of the box without any path configuration.
// Defined in CMakeLists.txt via target_compile_definitions.
#ifndef GNSS_PROJECT_ROOT
#define GNSS_PROJECT_ROOT ""
#endif

namespace GnssProcessor {

    using detail::toLower;
    using detail::joinStrings;
    using detail::jval;
    using detail::jsection;

    // =================================================================
    //  "extends" — config inheritance (base + delta)
    // =================================================================
    namespace {

        /// Max `extends` depth.  A guard against a hand-edited chain, not a design limit;
        /// the deepest sensible real chain is profile -> campaign -> leg.
        constexpr int kMaxExtendsDepth = 8;

        /// @brief Resolve one config file into a fully merged JSON document.
        ///
        /// A config may declare `"extends": "<path>"` or `"extends": ["<a>", "<b>"]` to
        /// inherit from one or more base configs.  Bases are merged left-to-right and the
        /// CHILD always wins (see detail::deepMergeJson for the object-vs-replace rule).
        ///
        /// Paths are resolved relative to the DECLARING file's directory first, then to the
        /// process working directory — so `"extends": "../profiles/network_base.json"` works
        /// from a config in `config/test_suite/`, and a repo-root-relative
        /// `"config/profiles/network_base.json"` works when invoked from the repo root.
        ///
        /// Byte-identity: a config with no `extends` key takes the `chain.empty()` path and
        /// this function returns exactly what `json::parse` produced, so every one of the
        /// project's existing configs parses bit-for-bit as before.
        ///
        /// @param p        file to load.
        /// @param log      logger for diagnostics.
        /// @param chain    OUT — every base actually merged, in application order.
        /// @param seen     cycle-detection set of canonical paths.
        /// @param depth    recursion depth.
        /// @param out      OUT — the merged document.
        /// @return false on a missing/unparseable base, a cycle, or depth overflow.
        bool loadConfigWithExtends(const std::filesystem::path& p,
                                   const std::shared_ptr<spdlog::logger>& log,
                                   std::vector<std::string>& chain,
                                   std::vector<std::string>& seen,
                                   int depth,
                                   nlohmann::json& out) {
            if (depth > kMaxExtendsDepth) {
                log->error("Config extends: chain deeper than {} levels at '{}' — refusing to "
                           "continue (a base almost certainly re-extends a descendant).",
                           kMaxExtendsDepth, p.string());
                return false;
            }
            std::error_code ec;
            auto canon = std::filesystem::weakly_canonical(p, ec);
            const std::string key = ec ? p.string() : canon.string();
            if (std::find(seen.begin(), seen.end(), key) != seen.end()) {
                log->error("Config extends: CYCLE — '{}' is already in the chain [{}].",
                           p.string(), joinStrings(seen, " -> "));
                return false;
            }
            seen.push_back(key);

            std::ifstream f(p);
            if (!f.is_open()) {
                log->error("Config extends: cannot open base config '{}'.", p.string());
                return false;
            }
            nlohmann::json self = nlohmann::json::parse(
                f, /*callback=*/nullptr, /*allow_exceptions=*/true, /*ignore_comments=*/true);

            if (!self.is_object() || !self.contains("extends")) {
                out = std::move(self);       // no inheritance — verbatim, byte-identical path
                return true;
            }

            std::vector<std::string> bases;
            if (self["extends"].is_string()) {
                bases.push_back(self["extends"].get<std::string>());
            } else if (self["extends"].is_array()) {
                for (const auto& b : self["extends"]) {
                    if (!b.is_string()) {
                        log->error("Config extends: '{}' has a non-string entry in its extends "
                                   "array.", p.string());
                        return false;
                    }
                    bases.push_back(b.get<std::string>());
                }
            } else {
                log->error("Config extends: '{}' has an \"extends\" that is neither a string nor "
                           "an array of strings.", p.string());
                return false;
            }
            // `extends` is a loader directive, not a settings key — drop it so no section
            // loader ever sees it and so the merged document looks hand-written.
            self.erase("extends");

            nlohmann::json merged = nlohmann::json::object();
            for (const std::string& b : bases) {
                std::filesystem::path bp = p.parent_path() / b;   // relative to the declarer
                if (!std::filesystem::exists(bp)) bp = std::filesystem::path(b);  // then cwd
                nlohmann::json base_json;
                if (!loadConfigWithExtends(bp, log, chain, seen, depth + 1, base_json))
                    return false;
                detail::deepMergeJson(merged, base_json);
                chain.push_back(bp.string());
            }
            detail::deepMergeJson(merged, self);                  // the child always wins
            out = std::move(merged);
            return true;
        }

    }  // namespace

    // =================================================================
    // loadFromJson — orchestrator
    // =================================================================

    bool GnssConfig::loadFromJson(const std::filesystem::path& path, LogManager& log_manager) {
        try {
            std::ifstream file(path);
            if (!file.is_open()) {
                log_manager.getLogger()->error("Failed to open config file: {}", path.string());
                return false;
            }
            file.close();

            // Resolve `extends` BEFORE any section loader runs, so every loader sees one
            // flat document and none of them needs to know inheritance exists.  With no
            // `extends` key this is a parse and nothing else — byte-identical to the
            // pre-inheritance loader for all ~493 existing configs.
            nlohmann::json json;
            {
                auto cfg_log = log_manager.getLogger("gnss_config");
                std::vector<std::string> seen;
                config_extends_chain_.clear();
                if (!loadConfigWithExtends(path, cfg_log, config_extends_chain_, seen,
                                           /*depth=*/0, json))
                    return false;
                if (!config_extends_chain_.empty())
                    cfg_log->info("Config INHERITANCE: '{}' extends [{}] — merged base-first, "
                                  "child wins; objects merge key-by-key, arrays/scalars replace. "
                                  "The run manifest records every link.",
                                  path.string(),
                                  joinStrings(config_extends_chain_, " -> "));
            }

            loadProcessingSection_(json, log_manager);
            loadEstimationSection_(json, log_manager);
            loadFilterSection_(json, log_manager);
            loadModelCorrectionsSection_(json, log_manager);
            loadReceiverSection_(json, log_manager);
            loadFilesSection_(json, log_manager);
            loadImuSection_(json, log_manager);
            loadDataDownloadSection_(json, log_manager);
            loadAppModeAndRealtimeSection_(json, log_manager);
            if (isBaselineMode()) {
                if (!loadBaselineSection_(json, log_manager)) return false;
            }
            if (isNetworkMode()) {
                if (!loadNetworkSection_(json, log_manager)) return false;
            }
            loadLoggingSection_(json, log_manager);

            // Cross-section validation.  Runs once after every section is
            // populated so the checks can mention parsed values from
            // multiple sections in their error messages.
            {
                auto cfg_log = log_manager.getLogger("gnss_config");
                // Doppler is needed for receiver-velocity estimation; if the
                // user opts in to estimate_velocity but trimmed 'D' out of
                // rinex_types, the velocity update would silently have no
                // observations to drive it.  Warn loudly so the
                // misconfiguration is obvious.
                if (estimate_velocity_ &&
                    rinex_types_.find('D') == std::string::npos) {
                    cfg_log->warn("estimation.estimate_velocity=true but 'D' is "
                        "not in processing.rinex_types ('{}'); Doppler "
                        "observations will be filtered out and velocity will "
                        "not converge.  Add 'D' to rinex_types.",
                        rinex_types_);
                }
                // SNR stochastic model needs S* observations; same idea.
                if (stochastic_model_ == StochasticModelType::SNR &&
                    rinex_types_.find('S') == std::string::npos) {
                    cfg_log->warn("filter.stochastic_model='snr' but 'S' is "
                        "not in processing.rinex_types ('{}'); SNR values "
                        "will be unavailable and the weighting will fall "
                        "back to default variances.  Add 'S' to rinex_types.",
                        rinex_types_);
                }
            }
            if (state_ambiguity_basis_ == StateAmbiguityBasis::NlWl) {
                auto err = log_manager.getLogger("gnss_config");
                if (!uncombined_enabled_) {
                    err->error("state_ambiguity_basis=nl_wl requires "
                        "processing.uncombined_enabled=true.  PPP IF + NlWl "
                        "is not yet implemented.");
                    return false;
                }
                if (isBaselineMode()) {
                    err->error("state_ambiguity_basis=nl_wl is not supported "
                        "in baseline modes (DD ambiguities have their own "
                        "schema).  Future work.");
                    return false;
                }
            }

            logConfigurationSummary_(log_manager);
            return true;
        }
        catch (const std::exception& e) {
            log_manager.getLogger()->error("Failed to parse config file {}: {}", path.string(), e.what());
            return false;
        }
    }

    // =================================================================
    //  Section: processing
    // =================================================================
    void GnssConfig::loadProcessingSection_(const nlohmann::json& json, LogManager& log_manager) {
        auto log = log_manager.getLogger("gnss_config");
        {
            std::string mode = jval<std::string>(json, "processing.mode", "processing_mode", "PPP");
            // Accept hyphen / underscore variants and different casing.
            std::string norm = toLower(mode);
            for (auto& c : norm) if (c == '_') c = '-';
            if      (norm == "ppp")              processing_mode_ = ProcessingMode::PPP;
            else if (norm == "ppp-rtk")          processing_mode_ = ProcessingMode::PPP_RTK;
            else if (norm == "baseline-single")  processing_mode_ = ProcessingMode::BaselineSingle;
            else if (norm == "baseline-multi")   processing_mode_ = ProcessingMode::BaselineMulti;
            else if (norm == "ppp-rtk-network")  processing_mode_ = ProcessingMode::PPP_RTK_Network;
            else if (norm == "ppp-rtk-user")     processing_mode_ = ProcessingMode::PPP_RTK_User;
            else {
                log->warn("Unknown processing mode '{}', defaulting to PPP", mode);
                processing_mode_ = ProcessingMode::PPP;
            }
            const char* mode_label =
                processing_mode_ == ProcessingMode::PPP              ? "PPP"             :
                processing_mode_ == ProcessingMode::PPP_RTK          ? "PPP-RTK"         :
                processing_mode_ == ProcessingMode::BaselineSingle   ? "Baseline-Single" :
                processing_mode_ == ProcessingMode::BaselineMulti    ? "Baseline-Multi"  :
                processing_mode_ == ProcessingMode::PPP_RTK_Network  ? "PPP-RTK-Network" :
                processing_mode_ == ProcessingMode::PPP_RTK_User     ? "PPP-RTK-User"    :
                                                                       "Baseline-Multi";
            log->info("Processing mode: {}", mode_label);
        }
        // Legacy "signal_mode: all" used to mean both "all bands" AND
        // "every tracking-mode channel per band".  The new design splits
        // those into frequency_mode + channel_mode.  Track whether the
        // legacy path with "all" was taken so the channel_mode block
        // below can preserve back-compat by defaulting channel_mode to
        // "all" when the user didn't set it explicitly.
        bool legacy_signal_mode_all = false;
        // ----- KF frequency mode (prefer new name; accept legacy signal_mode) -----
        {
            std::string raw = jval<std::string>(json, "processing.frequency_mode", "frequency_mode", "");
            bool from_legacy = false;
            if (raw.empty()) {
                raw = jval<std::string>(json, "processing.signal_mode", "signal_mode", "");
                if (!raw.empty()) from_legacy = true;
            }
            if (raw.empty()) raw = "Dual";
            std::string low = toLower(raw);
            if      (low == "dual")        frequency_mode_ = FrequencyMode::Dual;
            else if (low == "triple")      frequency_mode_ = FrequencyMode::Triple;
            else if (low == "all")         frequency_mode_ = FrequencyMode::All;
            else if (low == "independent") frequency_mode_ = FrequencyMode::Independent;
            else { log->warn("Invalid frequency_mode '{}', defaulting to Dual", raw); frequency_mode_ = FrequencyMode::Dual; }
            if (from_legacy) {
                log->warn("Deprecation: 'signal_mode' renamed to 'frequency_mode'; please update config.");
                if (low == "all") legacy_signal_mode_all = true;
            }
            log->info("Frequency mode: {}", raw);
        }
        // ----- KF channel mode -----
        {
            // Detect absent-vs-explicit via empty sentinel so back-compat can
            // distinguish "user didn't set channel_mode" from "user set single".
            std::string raw = jval<std::string>(json, "processing.channel_mode", "channel_mode", "");
            const bool explicit_channel = !raw.empty();
            if (raw.empty()) {
                // Back-compat: legacy "signal_mode: all" implied multi-channel.
                // Preserve that when the new explicit knob isn't set.
                raw = legacy_signal_mode_all ? "all" : "single";
                if (legacy_signal_mode_all) {
                    log->warn("Back-compat: legacy 'signal_mode: all' implied multi-channel state; "
                              "defaulting channel_mode to 'all'. Set channel_mode explicitly to silence.");
                }
            }
            std::string low = toLower(raw);
            if      (low == "single") channel_mode_ = ChannelMode::Single;
            else if (low == "all")    channel_mode_ = ChannelMode::All;
            else { log->warn("Invalid channel_mode '{}', defaulting to single", raw); channel_mode_ = ChannelMode::Single; }
            log->info("Channel mode: {}{}", raw, explicit_channel ? "" : " (defaulted)");
        }
        // ----- MW frequency mode (prefer new name; accept legacy mw_signals as filter→mirror, all→all) -----
        {
            std::string raw = jval<std::string>(json, "processing.mw_frequency_mode", "mw_frequency_mode", "");
            bool from_legacy = false;
            if (raw.empty()) {
                std::string legacy = jval<std::string>(json, "processing.mw_signals", "mw_signals", "");
                if (!legacy.empty()) {
                    from_legacy = true;
                    std::string llow = toLower(legacy);
                    if      (llow == "filter") raw = "mirror";
                    else if (llow == "all")    raw = "all";
                    else { log->warn("Invalid legacy mw_signals '{}'", legacy); raw = "mirror"; }
                }
            }
            if (raw.empty()) raw = "mirror";
            std::string low = toLower(raw);
            if      (low == "mirror" || low == "kf") mw_frequency_mode_ = MwFrequencyMode::Mirror;
            else if (low == "dual")                  mw_frequency_mode_ = MwFrequencyMode::Dual;
            else if (low == "triple")                mw_frequency_mode_ = MwFrequencyMode::Triple;
            else if (low == "all")                   mw_frequency_mode_ = MwFrequencyMode::All;
            else { log->warn("Invalid mw_frequency_mode '{}', defaulting to mirror", raw); mw_frequency_mode_ = MwFrequencyMode::Mirror; }
            if (from_legacy)
                log->warn("Deprecation: 'mw_signals' split into 'mw_frequency_mode' + 'mw_channel_mode'; please update config.");
            log->info("MW frequency mode: {}", raw);
        }
        // ----- MW channel mode (prefer new name; accept legacy mw_signals: filter→mirror, all→all) -----
        {
            std::string raw = jval<std::string>(json, "processing.mw_channel_mode", "mw_channel_mode", "");
            if (raw.empty()) {
                // Inherit from legacy mw_signals; the deprecation warning was
                // already emitted by the mw_frequency_mode block above.
                std::string legacy = jval<std::string>(json, "processing.mw_signals", "mw_signals", "");
                if (!legacy.empty()) {
                    std::string llow = toLower(legacy);
                    if      (llow == "filter") raw = "mirror";
                    else if (llow == "all")    raw = "all";
                    else                       raw = "mirror";
                }
            }
            if (raw.empty()) raw = "mirror";
            std::string low = toLower(raw);
            if      (low == "mirror" || low == "kf") mw_channel_mode_ = MwChannelMode::Mirror;
            else if (low == "single")                mw_channel_mode_ = MwChannelMode::Single;
            else if (low == "all")                   mw_channel_mode_ = MwChannelMode::All;
            else { log->warn("Invalid mw_channel_mode '{}', defaulting to mirror", raw); mw_channel_mode_ = MwChannelMode::Mirror; }
            log->info("MW channel mode: {}", raw);
        }
        {
            enable_inhouse_bias_estimation_ = jval<bool>(json,
                "processing.inhouse_bias_estimation",
                "inhouse_bias_estimation", false);
            log->info("In-house MW bias estimation: {}",
                enable_inhouse_bias_estimation_ ? "enabled" : "disabled");
        }
        {
            mw_init_frac_threshold_ = jval<double>(json,
                "processing.mw_init_frac_threshold",
                "mw_init_frac_threshold", 0.4);
            if (!(mw_init_frac_threshold_ > 0.0)) {
                log->warn("Invalid mw_init_frac_threshold {} (must be > 0); "
                          "reverting to default 0.4",
                          mw_init_frac_threshold_);
                mw_init_frac_threshold_ = 0.4;
            }
        }
        {
            mw_cross_sat_ewma_alpha_ = jval<double>(json,
                "processing.mw_cross_sat_ewma_alpha",
                "mw_cross_sat_ewma_alpha", 0.1);
            if (!(mw_cross_sat_ewma_alpha_ > 0.0 && mw_cross_sat_ewma_alpha_ <= 1.0)) {
                log->warn("Invalid mw_cross_sat_ewma_alpha {} (must be in (0, 1]); "
                          "reverting to default 0.1",
                          mw_cross_sat_ewma_alpha_);
                mw_cross_sat_ewma_alpha_ = 0.1;
            }
        }
        {
            std::string raw = jval<std::string>(json, "processing.satellite_interpolation_mode",
                                                 "satellite_interpolation_mode", "Lagrange");
            std::string low = toLower(raw);
            if      (low == "lagrange") satellite_interpolation_mode_ = SatelliteInterpolationMode::Lagrange;
            else if (low == "finite")   satellite_interpolation_mode_ = SatelliteInterpolationMode::Finite;
            else if (low == "hermite")  satellite_interpolation_mode_ = SatelliteInterpolationMode::Hermite;
            else if (low == "spline")   satellite_interpolation_mode_ = SatelliteInterpolationMode::Spline;
            else { log->warn("Invalid satellite_interpolation_mode '{}', defaulting to Lagrange", raw);
                   satellite_interpolation_mode_ = SatelliteInterpolationMode::Lagrange; }
            log->info("Satellite Pos Vel interpolation mode: {}", raw);
        }
        {
            std::string raw = jval<std::string>(json, "processing.satellite_clock_interpolation_mode",
                                                 "satellite_clock_interpolation_mode", "Linear");
            std::string low = toLower(raw);
            if      (low == "linear")        satellite_clock_interpolation_mode_ = SatelliteClockInterpolationMode::Linear;
            else if (low == "lagrange")      satellite_clock_interpolation_mode_ = SatelliteClockInterpolationMode::Lagrange;
            else if (low == "lsq_sat_clock" || low == "lsqsatclock"
                  || low == "lsq")           satellite_clock_interpolation_mode_ = SatelliteClockInterpolationMode::LsqSatClock;
            else { log->warn("Invalid satellite_clock_interpolation_mode '{}', defaulting to Linear", raw);
                   satellite_clock_interpolation_mode_ = SatelliteClockInterpolationMode::Linear; }
            log->info("Satellite Clock interpolation mode: {}", raw);
        }

        uncombined_enabled_ = jval<bool>(json, "processing.uncombined_enabled", "uncombined_enabled", false);
        if (processing_mode_ == ProcessingMode::PPP_RTK
         || processing_mode_ == ProcessingMode::PPP_RTK_Network
         || processing_mode_ == ProcessingMode::PPP_RTK_User) {
            // The PPP-RTK family (single-receiver user, network producer, and
            // network-product consumer) all operate on the undifferenced +
            // uncombined L1/L2 substrate.  The network producer additionally
            // runs DifferencingMode::None (no receiver-side BSSD) and estimates
            // the satellite-side bias lumps explicitly (see runNetwork).
            uncombined_enabled_ = true;
            log->info("PPP-RTK family mode: forcing uncombined_enabled=true");
        }
        if (processing_mode_ == ProcessingMode::BaselineSingle
         || processing_mode_ == ProcessingMode::BaselineMulti) {
            // Baseline DD processes each signal independently: the
            // ionosphere-free combination used by combined PPP mode
            // mixes band codes into strings like "C1C_C2W" that don't
            // cross-match across receivers when they track different
            // L2 attributes (e.g. rover C2W, reference C2S).  DD
            // cancels ionosphere on its own for short baselines, so
            // IF combinations are neither needed nor wanted here.
            uncombined_enabled_ = true;
            log->info("Baseline mode: forcing uncombined_enabled=true (per-signal DD)");
        }
        elevation_mask_deg_ = jval<double>(json, "processing.elevation_mask_deg", "elevation_mask_deg", 10.0);
        exclude_eclipsing_satellites_ = jval<bool>(json,
            "processing.exclude_eclipsing_satellites",
            "exclude_eclipsing_satellites", false);
        eclipse_shadow_scale_ = jval<double>(json,
            "processing.eclipse_shadow_scale",
            "eclipse_shadow_scale", 1.0);
        if (eclipse_shadow_scale_ <= 0.0) {
            log->warn("Non-positive eclipse_shadow_scale ({:.3f}) ignored; reverting to 1.0",
                      eclipse_shadow_scale_);
            eclipse_shadow_scale_ = 1.0;
        }
        if (exclude_eclipsing_satellites_) {
            log->info("Eclipse-aware satellite rejection: enabled (shadow_scale={:.3f})",
                      eclipse_shadow_scale_);
        }
        data_gap_tolerance_s_ = jval<double>(json, "processing.data_gap_tolerance_s", "data_gap_tolerance_s", 0.0);
        long_gap_tolerance_s_ = jval<double>(json, "processing.long_gap_tolerance_s", "long_gap_tolerance_s", 300.0);
        if (long_gap_tolerance_s_ < 0.0) {
            log->warn("Negative long_gap_tolerance_s ({:.3f}) ignored; reverting to default 300 s",
                      long_gap_tolerance_s_);
            long_gap_tolerance_s_ = 300.0;
        }
        if (data_gap_tolerance_s_ > 0.0 && long_gap_tolerance_s_ < data_gap_tolerance_s_) {
            log->warn("long_gap_tolerance_s ({:.1f}) < data_gap_tolerance_s ({:.1f}); "
                      "clamping long_gap up to data_gap so short-gap band is non-empty",
                      long_gap_tolerance_s_, data_gap_tolerance_s_);
            long_gap_tolerance_s_ = data_gap_tolerance_s_;
        }
        gnss_rate_s_ = jval<double>(json, "processing.gnss_rate_s", "gnss_rate_s", 0.0);
        if (gnss_rate_s_ < 0.0) {
            log->warn("Negative gnss_rate_s ({:.3f}) ignored; decimation disabled", gnss_rate_s_);
            gnss_rate_s_ = 0.0;
        }
        if (gnss_rate_s_ > 0.0) {
            log->info("GNSS processing rate: {:.1f} s (input epochs will be decimated)", gnss_rate_s_);
        }
        max_duration_s_ = jval<double>(json, "processing.max_duration_s", "max_duration_s", 0.0);
        if (max_duration_s_ < 0.0) {
            log->warn("Negative max_duration_s ({:.3f}) ignored; processing not time-limited", max_duration_s_);
            max_duration_s_ = 0.0;
        }
        if (max_duration_s_ > 0.0) {
            log->info("Max processing duration: {:.1f} s of data (epoch time relative to first obs; "
                      "stops once this span is reached)", max_duration_s_);
        }
        // Real-time obs/correction synchronization (loop-level; inert in
        // post-processing).  skip-to-latest keeps the obs stream pinned to real
        // time; ssr_max_age rejects an SSR overlay whose record is desynced from
        // the obs epoch (the live-corrections-vs-lagging-obs failure mode).
        enable_obs_skip_to_latest_ = jval<bool>(json,
            "processing.enable_obs_skip_to_latest", "enable_obs_skip_to_latest", false);
        obs_skip_max_lag_s_ = jval<double>(json,
            "processing.obs_skip_max_lag_s", "obs_skip_max_lag_s", 5.0);
        if (obs_skip_max_lag_s_ <= 0.0) {
            log->warn("Non-positive obs_skip_max_lag_s ({:.1f}) → 5 s default", obs_skip_max_lag_s_);
            obs_skip_max_lag_s_ = 5.0;
        }
        rt_backlog_warn_s_ = jval<double>(json,
            "processing.rt_backlog_warn_s", "rt_backlog_warn_s", 5.0);
        ssr_max_age_s_ = jval<double>(json,
            "processing.ssr_max_age_s", "ssr_max_age_s", 30.0);
        if (ssr_max_age_s_ <= 0.0) {
            log->warn("Non-positive ssr_max_age_s ({:.1f}) → 30 s default", ssr_max_age_s_);
            ssr_max_age_s_ = 30.0;
        }
        // A5: SSR code/phase-bias staleness gate (default matches the clock max-age).
        enable_ssr_bias_max_age_ = jval<bool>(json,
            "processing.enable_ssr_bias_max_age", "enable_ssr_bias_max_age", false);
        // RTA-9(a): remember whether the USER wrote the key (either form) — the
        // network-consumer profile arms the gate under bias consumption only
        // when the user expressed no preference (explicit false is respected).
        enable_ssr_bias_max_age_explicit_ =
            detail::resolvePath(json, "processing.enable_ssr_bias_max_age") != nullptr
            || json.contains("enable_ssr_bias_max_age");
        ssr_bias_max_age_s_ = jval<double>(json,
            "processing.ssr_bias_max_age_s", "ssr_bias_max_age_s", ssr_max_age_s_);
        if (ssr_bias_max_age_s_ <= 0.0) {
            log->warn("Non-positive ssr_bias_max_age_s ({:.1f}) → {:.1f} s (ssr_max_age_s)",
                      ssr_bias_max_age_s_, ssr_max_age_s_);
            ssr_bias_max_age_s_ = ssr_max_age_s_;
        }
        // A5 stale-hold: the apply gate (and its freshness-flip ambiguity reset)
        // drops a bias only past factor × ssr_bias_max_age_s, HOLDING the last value
        // over shorter staleness gaps — a provider update interval hovering near the
        // max age then produces no model step and no reset per update cycle.
        ssr_bias_stale_hold_factor_ = jval<double>(json,
            "processing.ssr_bias_stale_hold_factor", "ssr_bias_stale_hold_factor", 2.0);
        if (ssr_bias_stale_hold_factor_ < 1.0) {
            if (enable_ssr_bias_max_age_)   // warn only when the knob has any effect
                log->warn("ssr_bias_stale_hold_factor {:.2f} < 1 → clamped to 1.0 (no hold)",
                          ssr_bias_stale_hold_factor_);
            ssr_bias_stale_hold_factor_ = 1.0;
        }
        if (enable_ssr_bias_max_age_)
            log->info("SSR bias staleness gate ON: reject code/phase bias older than {:.1f}s "
                      "(stale-hold to {:.1f}s = {:.2f}x before the applied bias drops)",
                      ssr_bias_max_age_s_,
                      ssr_bias_max_age_s_ * ssr_bias_stale_hold_factor_,
                      ssr_bias_stale_hold_factor_);
        // Card CC (A): the declared-step ambiguity carry for a network-product
        // consumer.  Default false = the existing reset reaction (byte-identical);
        // the closed-loop harness derives it from network.closed_loop.pb_disc_carry
        // (deriveClosedLoopConsumerConfig), a standalone consumer sets it here.
        ssr_phase_bias_disc_carry_ = jval<bool>(json,
            "processing.ssr_phase_bias_disc_carry", "ssr_phase_bias_disc_carry", false);
        if (ssr_phase_bias_disc_carry_)
            log->info("SSR phase-bias declared-step CARRY ON (card CC): a disc-counter bump / "
                      "freshness flip translates the float ambiguity by -delta (value-only, "
                      "covariance kept) instead of resetting it; first sightings, an NL/WL "
                      "basis, a BSSD consumer and unknown keys still take the reset.");
        // Card PBT (A): the EVERY-step ambiguity translation.  Default false =
        // the existing reset (or, under card CC, the declared-step carry) —
        // byte-identical; the closed-loop harness derives it from
        // network.closed_loop.pb_step_translate (deriveClosedLoopConsumerConfig),
        // a standalone consumer sets it here.
        ssr_phase_bias_step_translate_ = jval<bool>(json,
            "processing.ssr_phase_bias_step_translate", "ssr_phase_bias_step_translate", false);
        ssr_phase_bias_step_translate_eps_m_ = jval<double>(json,
            "processing.ssr_phase_bias_step_translate_eps_m",
            "ssr_phase_bias_step_translate_eps_m", 0.005);
        if (ssr_phase_bias_step_translate_eps_m_ < 0.0) {
            log->warn("Negative ssr_phase_bias_step_translate_eps_m ({:.4f}) ignored; using 0.005 m",
                      ssr_phase_bias_step_translate_eps_m_);
            ssr_phase_bias_step_translate_eps_m_ = 0.005;
        }
        if (ssr_phase_bias_step_translate_) {
            log->info("SSR phase-bias STEP TRANSLATION ON (card PBT): EVERY change in the "
                      "APPLIED satellite phase bias beyond {:.4f} m — declared by an R record "
                      "or not — translates that (sat, signal) float ambiguity by -delta "
                      "(value-only, covariance kept).  A change in the applied product carries "
                      "no position information, so the phase model stays consistent only if the "
                      "ambiguity absorbs it exactly; a satellite-common move is absorbed "
                      "uniformly and is exactly consistent.  This REPLACES the reset reaction "
                      "AND the card-CC declared-step carry, so a declared step is translated "
                      "exactly once.  First sightings / A5 freshness edges, an NL/WL basis, a "
                      "BSSD consumer and unknown keys still take the reset when DECLARED and "
                      "are a counted no-op when not.",
                      ssr_phase_bias_step_translate_eps_m_);
            if (ssr_phase_bias_disc_carry_)
                log->warn("processing.ssr_phase_bias_disc_carry is also armed: the card-CC carry "
                          "seam is NOT entered while the step translation is on (its counters "
                          "stay 0).  The declared population is handled by the translation and "
                          "is reported on its own summary line.");
        }
        // RTA-9(b): CLOCK stale-hold rung.  0 (default) = off — the historic
        // two-state gate (fresh/rejected) is byte-identical.  >0 opens the
        // middle rung: a clock aged in (ssr_max_age, factor x ssr_max_age] is
        // still applied (C0+C1*dt over the true dt) with URA-staleness R
        // inflation on the consuming rows; beyond the window the original
        // broadcast-grade cliff stands.  Values in (0,1] cannot shrink the base
        // gate — clamp to 1 (no hold).
        ssr_clock_stale_hold_factor_ = jval<double>(json,
            "processing.ssr_clock_stale_hold_factor", "ssr_clock_stale_hold_factor", 0.0);
        if (ssr_clock_stale_hold_factor_ < 0.0) {
            log->warn("Negative ssr_clock_stale_hold_factor ({:.2f}) ignored; rung off",
                      ssr_clock_stale_hold_factor_);
            ssr_clock_stale_hold_factor_ = 0.0;
        } else if (ssr_clock_stale_hold_factor_ > 0.0 && ssr_clock_stale_hold_factor_ <= 1.0) {
            log->warn("ssr_clock_stale_hold_factor {:.2f} <= 1 -> clamped to 1.0 (no hold; "
                      "the factor widens the clock apply window, it cannot shrink it)",
                      ssr_clock_stale_hold_factor_);
            ssr_clock_stale_hold_factor_ = 1.0;
        }
        if (ssr_clock_stale_hold_factor_ > 1.0)
            log->info("SSR clock stale-hold rung ON (RTA-9b): clocks aged ({:.1f}, {:.1f}]s are "
                      "still applied (C0+C1*dt) with ZOH q2*dt^3/3 R inflation on consuming rows; "
                      "beyond {:.1f}s the broadcast-grade cliff applies",
                      ssr_max_age_s_, ssr_max_age_s_ * ssr_clock_stale_hold_factor_,
                      ssr_max_age_s_ * ssr_clock_stale_hold_factor_);
        // RTA-13 (opt-in): CODE-domain band-wise bias fallback.  Armed on the
        // stores by RealTimeEngine::initialize (singleton) / runRealTime (owned).
        ssr_bias_band_fallback_ = jval<bool>(json,
            "processing.ssr_bias_band_fallback", "ssr_bias_band_fallback", false);
        if (ssr_bias_band_fallback_)
            log->info("SSR code-bias band fallback ON (RTA-13): an exact-attribute code-bias "
                      "miss consumes the same-band sibling attribute's bias (residual attribute "
                      "DCB absorbed by receiver code-bias states); phase NEVER falls back.");
        // obs↔SSR sync: hold obs epochs newer than (freshest SSR content epoch +
        // ssr_max_age) so PPP-RTK consumes obs time-aligned with the corrections
        // rather than pinned to wall clock; a stalled SSR feed is bounded by the
        // max-hold liveness budget before degraded fallback.
        enable_obs_ssr_sync_ = jval<bool>(json,
            "processing.enable_obs_ssr_sync", "enable_obs_ssr_sync", false);
        obs_ssr_sync_max_hold_s_ = jval<double>(json,
            "processing.obs_ssr_sync_max_hold_s", "obs_ssr_sync_max_hold_s", 30.0);
        if (obs_ssr_sync_max_hold_s_ <= 0.0) {
            log->warn("Non-positive obs_ssr_sync_max_hold_s ({:.1f}) → 30 s default", obs_ssr_sync_max_hold_s_);
            obs_ssr_sync_max_hold_s_ = 30.0;
        }
        if (enable_obs_skip_to_latest_) {
            log->info("Real-time obs skip-to-latest ON: drop epochs >{:.1f}s behind wall-clock GPS "
                      "(backlog warn at {:.1f}s); SSR overlay max age {:.1f}s",
                      obs_skip_max_lag_s_, rt_backlog_warn_s_, ssr_max_age_s_);
        }
        if (enable_obs_ssr_sync_) {
            log->info("Real-time obs↔SSR sync ON: hold obs newer than (freshest SSR content +{:.1f}s); "
                      "max hold {:.1f}s before degraded fallback",
                      ssr_max_age_s_, obs_ssr_sync_max_hold_s_);
        }
        // RINEX observation-type filter.  String of letters from the set
        // {C, L, D, S} selecting which measurement types the parser will
        // keep.  Default "CL" = code + phase, which is what every
        // estimator in the pipeline actually consumes.  Add 'D' for
        // Doppler (estimate_velocity) or 'S' for SNR (SNR stochastic
        // model).  Case-insensitive; duplicates are collapsed.  An
        // empty / all-invalid setting is rejected with a warning and
        // falls back to the default.
        {
            std::string raw = jval<std::string>(json,
                "processing.rinex_types", "rinex_types", "CL");
            std::string filtered;
            filtered.reserve(4);
            std::string invalid;
            for (char c : raw) {
                if (std::isspace(static_cast<unsigned char>(c))) continue;
                char u = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
                if (u == 'C' || u == 'L' || u == 'D' || u == 'S') {
                    if (filtered.find(u) == std::string::npos) filtered.push_back(u);
                } else {
                    invalid.push_back(c);
                }
            }
            if (!invalid.empty()) {
                log->warn("Ignoring unknown rinex_types character(s) '{}'; "
                          "legal letters are C, L, D, S.", invalid);
            }
            if (filtered.empty()) {
                log->warn("Empty rinex_types '{}'; reverting to default \"CL\".", raw);
                filtered = "CL";
            }
            rinex_types_ = std::move(filtered);
            log->info("RINEX measurement types retained: {} ({}{}{}{})",
                rinex_types_,
                rinex_types_.find('C') != std::string::npos ? "code "    : "",
                rinex_types_.find('L') != std::string::npos ? "phase "   : "",
                rinex_types_.find('D') != std::string::npos ? "doppler " : "",
                rinex_types_.find('S') != std::string::npos ? "snr "     : "");
        }
        // RINEX 3 zero-sentinel guard (docs/SPEC_rinex_zero_sentinel.md): a written
        // 0.0 is the standard's missing-observation sentinel, not a measurement.
        // Default false = legacy ingest (byte-identity); arm in new configs.
        rinex_skip_zero_values_ = jval<bool>(json, "processing.rinex_skip_zero_values",
                                             "rinex_skip_zero_values", false);
        if (rinex_skip_zero_values_) {
            log->info("RINEX 3 zero-sentinel guard ARMED: written 0.0 observation "
                      "values are treated as missing (RINEX 3 convention).");
        }

        leap_seconds_tai_   = jval<double>(json, "processing.leap_seconds_tai", "leap_seconds_tai", 37.0);

        // Relinearize geometry lives under processing (it affects the model loop).
        relinearize_geometry_ = jval<bool>(json, "processing.relinearize_geometry", "relinearize_geometry", false);
        relinearize_dpos_threshold_phase_m_ = jval<double>(json,
            "processing.relinearize_dpos_threshold_phase_m",
            "relinearize_dpos_threshold_phase_m", 1.0);
        relinearize_dpos_threshold_code_only_m_ = jval<double>(json,
            "processing.relinearize_dpos_threshold_code_only_m",
            "relinearize_dpos_threshold_code_only_m", 100.0);
        relinearize_max_count_        = jval<int>(json,
            "processing.relinearize_max_count",        "relinearize_max_count",        -1);

        // Eigen thread count.  Default 1 (sequential) — PPP-RTK matrix sizes
        // (n≈350, m≈250) sit below the threshold where OpenMP fork/join
        // overhead is absorbed by the parallel speedup.  0 = OpenMP default.
        eigen_threads_ = jval<int>(json, "processing.eigen_threads", "eigen_threads", 1);

        // Backward RTS smoother: post-processing only. Real-time guard
        // is applied by loadAppModeAndRealtimeSection_, once app_mode_
        // has been parsed.
        run_smoother_ = jval<bool>(json, "processing.run_smoother", "run_smoother", false);

        // Cross-session normal-equation export (item #13B): post-processing
        // only.  Real-time guard is applied by loadAppModeAndRealtimeSection_,
        // once app_mode_ has been parsed.
        enable_neq_export_ = jval<bool>(json, "processing.enable_neq_export", "enable_neq_export", false);

        // Orbit-layer enhancements (M-PAGES port): sidereal-trig SP3 position
        // interpolation and beta-driven orbit-normal satellite attitude.  Both
        // default off and are byte-identical to the legacy path when disabled.
        enable_trig_sp3_interp_ = jval<bool>(json, "processing.enable_trig_sp3_interp", "enable_trig_sp3_interp", false);
        enable_orbit_normal_attitude_ = jval<bool>(json, "processing.enable_orbit_normal_attitude", "enable_orbit_normal_attitude", false);

        // Non-causal cycle-slip bridge (runs before the smoother; requires
        // run_smoother_; forced off in real-time mode by
        // loadAppModeAndRealtimeSection_).
        enable_noncausal_slip_bridge_ = jval<bool>(json, "processing.enable_noncausal_slip_bridge", "enable_noncausal_slip_bridge", false);
        slip_bridge_half_window_epochs_ = jval<int>(json, "processing.slip_bridge_half_window_epochs", "slip_bridge_half_window_epochs", 30);
        pre_bridge_float_tol_cycles_ = jval<double>(json, "processing.pre_bridge_float_tol_cycles", "pre_bridge_float_tol_cycles", 0.5);
        pre_bridge_conv_var_m2_ = jval<double>(json, "processing.pre_bridge_conv_var_m2", "pre_bridge_conv_var_m2", 0.0025);

        // End-of-run auto report (pyviz/run_report.py -> gnss_report.html).
        enable_auto_report_ = jval<bool>(json, "processing.auto_report", "auto_report", false);
        report_backend_     = jval<std::string>(json, "processing.report_backend", "report_backend", "matplotlib");
        report_python_      = jval<std::string>(json, "processing.report_python", "report_python", "");
        report_async_       = jval<bool>(json, "processing.report_async", "report_async", false);
        report_viewers_.clear();
        {
            const auto& arr = jsection(json, "processing.report_viewers", "report_viewers");
            if (arr.is_array())
                report_viewers_ = arr.get<std::vector<std::string>>();
        }
        // Sanitize report_* values: they are interpolated into the std::system
        // command line, so reject anything that could break / inject the shell.
        {
            auto safe_token = [](const std::string& s) {
                if (s.empty()) return false;
                for (char c : s) {
                    const bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z')
                                  || (c >= '0' && c <= '9') || c == '_' || c == '-';
                    if (!ok) return false;
                }
                return true;
            };
            if (report_backend_ != "matplotlib" && report_backend_ != "plotly") {
                log->warn("Unknown report_backend '{}'; using 'matplotlib'", report_backend_);
                report_backend_ = "matplotlib";
            }
            std::vector<std::string> clean;
            for (const auto& v : report_viewers_) {
                if (safe_token(v)) clean.push_back(v);
                else log->warn("Ignoring invalid report_viewers entry '{}' "
                               "(allowed: [A-Za-z0-9_-])", v);
            }
            report_viewers_ = std::move(clean);
            if (report_python_.find('"') != std::string::npos) {
                log->warn("report_python contains a quote; ignoring it");
                report_python_.clear();
            }
        }

        // Active systems
        active_systems_.clear();
        {
            const auto& arr = jsection(json, "processing.active_systems", "active_systems");
            if (arr.is_array())
                active_systems_ = arr.get<std::vector<std::string>>();
        }
        if (active_systems_.empty()) {
            active_systems_ = { "GPS", "Galileo", "GLONASS", "BeiDou", "QZSS", "SBAS" };
            log->info("No active systems specified, using default: GPS, Galileo, GLONASS, BeiDou, QZSS, SBAS");
        } else {
            for (const auto& system : active_systems_)
                if (system != "GPS" && system != "Galileo" && system != "GLONASS" &&
                    system != "BeiDou" && system != "QZSS" && system != "SBAS")
                    log->warn("Unknown system in active_systems: {}", system);
            log->info("Active systems: {}", joinStrings(active_systems_));
        }

        // Excluded satellites
        excluded_satellites_.clear();
        {
            const auto& arr = jsection(json, "processing.excluded_satellites", "excluded_satellites");
            if (arr.is_array())
                excluded_satellites_ = arr.get<std::vector<std::string>>();
        }
        if (!excluded_satellites_.empty())
            log->info("Excluded satellites: {}", joinStrings(excluded_satellites_));

        // Signal priorities
        static const std::map<std::string, std::vector<char>> system_bands = {
            {"GPS",     {'1','2','5'}},
            {"Galileo", {'1','5','7','6','8'}},
            {"GLONASS", {'1','2','3','4','6'}},
            {"BeiDou",  {'1','2','5','6','7','8'}},
            {"QZSS",    {'1','2','5','6'}},
            {"SBAS",    {'1','5'}}
        };
        static const char obs_types[] = {'C','L','D','S'};

        signal_priorities_.clear();
        {
            const auto& sp = jsection(json, "processing.signal_priorities", "signal_priorities");
            if (sp.is_object()) {
                for (const auto& [system, value] : sp.items()) {
                    if (value.is_string()) {
                        std::string attrs = value.get<std::string>();
                        auto bands_it = system_bands.find(system);
                        if (bands_it == system_bands.end()) {
                            log->warn("No band definition for system '{}', skipping signal priorities", system);
                            continue;
                        }
                        std::vector<std::string> expanded;
                        for (char band : bands_it->second)
                            for (char ot : obs_types)
                                for (char attr : attrs)
                                    expanded.push_back(std::string{ot, band, attr});
                        signal_priorities_[system] = std::move(expanded);
                    } else if (value.is_array()) {
                        signal_priorities_[system] = value.get<std::vector<std::string>>();
                    }
                }
            }
        }

        // RTA-7: standalone AR MW-WL suppression + first-class network-consumer
        // profile.  Both default OFF/inert — byte-identical when absent.
        ar_ignore_mw_wl_ = jval<bool>(json, "processing.ar_ignore_mw_wl",
                                      "ar_ignore_mw_wl", false);
        if (ar_ignore_mw_wl_)
            log->info("AR: processing.ar_ignore_mw_wl ON — the MW wide-lane feed into "
                      "ambiguity resolution is dropped (pure MLAMBDA on the KF float).");
        {
            auto& nc = network_consumer_config_;
            nc.enable        = jval<bool>(json, "processing.network_consumer.enable",
                                          "network_consumer_enable", false);
            nc.clock         = jval<bool>(json, "processing.network_consumer.clock",
                                          "network_consumer_clock", true);
            nc.bias          = jval<bool>(json, "processing.network_consumer.bias",
                                          "network_consumer_bias", true);
            nc.undifferenced = jval<bool>(json, "processing.network_consumer.undifferenced",
                                          "network_consumer_undifferenced", true);
            // RTA-8(a): orbit-overlay source for the injected-store wiring.
            // "external" (default) keeps the orbit overlay on the singleton (the
            // external RTS corrections stream); "network" points it at the
            // injected product store (future orbit-bearing product).
            nc.orbit_source  = detail::toLower(jval<std::string>(
                json, "processing.network_consumer.orbit_source",
                "network_consumer_orbit_source", std::string{"external"}));
            if (nc.orbit_source != "external" && nc.orbit_source != "network") {
                log->warn("Unknown processing.network_consumer.orbit_source '{}' "
                          "(legal: \"external\", \"network\"); using \"external\"",
                          nc.orbit_source);
                nc.orbit_source = "external";
            }
            // Tri-state: only an EXPLICIT key becomes an override (-1 = derive
            // exactly as the closed-loop driver does; see networkConsumerArIgnoreMwWl).
            if (const auto* p = detail::resolvePath(json, "processing.network_consumer.ar_ignore_mw_wl"))
                nc.ar_ignore_mw_wl = p->get<bool>() ? 1 : 0;
            nc.expect_reference_receiver = jval<std::string>(
                json, "processing.network_consumer.expect_reference_receiver",
                "network_consumer_expect_reference_receiver", std::string{});
            nc.expect_apriori_product_id = jval<std::string>(
                json, "processing.network_consumer.expect_apriori_product_id",
                "network_consumer_expect_apriori_product_id", std::string{});
            // RTA-12: optional S-contract identity pins (frame=/atx=/oref= fields).
            // Empty (default) = don't pin — checkContract only compares a field
            // when the expectation declares it non-empty.
            nc.expect_frame_id = jval<std::string>(
                json, "processing.network_consumer.expect_frame_id",
                "network_consumer_expect_frame_id", std::string{});
            nc.expect_atx_id = jval<std::string>(
                json, "processing.network_consumer.expect_atx_id",
                "network_consumer_expect_atx_id", std::string{});
            nc.expect_orbit_ref = jval<std::string>(
                json, "processing.network_consumer.expect_orbit_ref",
                "network_consumer_expect_orbit_ref", std::string{});
            // Card 7 (RTF-2): orbit/clock coherence age-skew budget.  0 (default)
            // = the epoch-skew comparison + the clock withhold are OFF (IOD-SSR
            // identity mismatches still count).  Armed on the stores at the
            // setCodeBiasBandFallback sites (RealTimeEngine::initialize /
            // Application::runRealTime).
            nc.orbit_clock_max_skew_s = jval<double>(
                json, "processing.network_consumer.orbit_clock_max_skew_s",
                "network_consumer_orbit_clock_max_skew_s", 0.0);
            if (nc.orbit_clock_max_skew_s < 0.0) {
                log->warn("processing.network_consumer.orbit_clock_max_skew_s = {:.3f} is "
                          "negative; clamping to 0 (echo skew check + withhold OFF).",
                          nc.orbit_clock_max_skew_s);
                nc.orbit_clock_max_skew_s = 0.0;
            }
            if (nc.orbit_clock_max_skew_s > 0.0)
                log->info("Network-consumer orbit/clock coherence budget (Card 7): "
                          "orbit_clock_max_skew_s = {:.1f}s — a network clock whose "
                          "orbit echo disagrees with the applied SSR orbit (IOD-SSR, "
                          "presence, or content-epoch skew beyond the budget) is "
                          "WITHHELD for that satellite.",
                          nc.orbit_clock_max_skew_s);
            if (nc.enable)
                log->info("Network-consumer profile (RTA-7) CONFIGURED: clock {}, bias {}, "
                          "{} user model, orbit_source {}, ar_ignore_mw_wl {} — applied at "
                          "real-time startup when realtime.network_products_path provides a "
                          "product source.",
                          nc.clock ? "on" : "off", nc.bias ? "on" : "off",
                          nc.undifferenced ? "undifferenced" : "BSSD",
                          nc.orbit_source,
                          nc.ar_ignore_mw_wl < 0 ? "auto"
                              : (nc.ar_ignore_mw_wl != 0 ? "true (explicit)" : "false (explicit)"));
        }
    }

    // =================================================================
    //  Section: estimation
    // =================================================================
    void GnssConfig::loadEstimationSection_(const nlohmann::json& json, LogManager& log_manager) {
        auto log = log_manager.getLogger("gnss_config");
        estimate_tropo_gradients_ = jval<bool>(json, "estimation.estimate_tropo_gradients",
                                                "estimate_tropo_gradients", false);
        estimate_phase_clock_     = jval<bool>(json, "estimation.estimate_phase_clock",
                                                "estimate_phase_clock", false);
        estimate_velocity_        = jval<bool>(json, "estimation.estimate_velocity",
                                                "estimate_velocity", false);
        enable_iono_rate_         = jval<bool>(json, "estimation.enable_iono_rate",
                                                "enable_iono_rate", false);
        estimate_ifb_             = jval<bool>(json, "estimation.estimate_ifb",
                                                "estimate_ifb", false);
        code_bias_reinflate_std_  = jval<double>(json, "estimation.code_bias_reinflate_std",
                                                "code_bias_reinflate_std", 0.5);
        clock_disc_isb_reinflate_std_ = jval<double>(json,
                                                "estimation.clock_disc_isb_reinflate_std",
                                                "clock_disc_isb_reinflate_std", 0.0);
        lock_signals_to_clock_ref_ = jval<bool>(json, "estimation.lock_signals_to_clock_ref",
                                                 "lock_signals_to_clock_ref", false);
        require_bia_for_signals_   = jval<bool>(json, "estimation.require_bia_for_signals",
                                                 "require_bia_for_signals", false);
        estimate_sat_biases_       = jval<bool>(json, "estimation.estimate_sat_biases",
                                                 "estimate_sat_biases", false);
        // Network producer (PPP_RTK_Network) toggles.  Both default false so the
        // single-receiver user / baseline paths are byte-identical; they only
        // take effect once the network estimator (W4) creates the shared states.
        estimate_sat_phase_biases_ = jval<bool>(json, "estimation.estimate_sat_phase_biases",
                                                 "estimate_sat_phase_biases", false);
        estimate_sat_clocks_       = jval<bool>(json, "estimation.estimate_sat_clocks",
                                                 "estimate_sat_clocks", false);
        promote_sat_code_bias_     = jval<bool>(json, "estimation.promote_sat_code_bias",
                                                 "promote_sat_code_bias", false);
        // Sat-bias cache thresholds (only meaningful when estimate_sat_biases
        // is on).  warm_age_s controls full warm restore; max_age_s caps
        // entry lifetime.  Validate warm<=max; if not, clamp warm down so
        // both stay positive and the warm band is never empty.
        sat_bias_cache_warm_age_s_ = jval<double>(json,
            "estimation.sat_bias_cache.warm_age_s",
            "sat_bias_cache_warm_age_s", 300.0);
        sat_bias_cache_max_age_s_  = jval<double>(json,
            "estimation.sat_bias_cache.max_age_s",
            "sat_bias_cache_max_age_s",  3600.0);
        if (sat_bias_cache_warm_age_s_ < 0.0) sat_bias_cache_warm_age_s_ = 0.0;
        if (sat_bias_cache_max_age_s_  < 0.0) sat_bias_cache_max_age_s_  = 0.0;
        if (sat_bias_cache_warm_age_s_ > sat_bias_cache_max_age_s_) {
            log->warn("sat_bias_cache.warm_age_s ({:.1f}) > max_age_s ({:.1f}); "
                      "clamping warm = max.",
                      sat_bias_cache_warm_age_s_, sat_bias_cache_max_age_s_);
            sat_bias_cache_warm_age_s_ = sat_bias_cache_max_age_s_;
        }
        if (estimate_sat_biases_) {
            log->info("Sat-bias cache: warm_age={:.1f} s, max_age={:.1f} s",
                      sat_bias_cache_warm_age_s_, sat_bias_cache_max_age_s_);
        }
        no_bia_code_downweight_    = jval<double>(json, "estimation.no_bia_code_downweight",
                                                  "no_bia_code_downweight", 100.0);
        if (no_bia_code_downweight_ < 1.0) no_bia_code_downweight_ = 1.0;
        enable_if_slip_detector_  = jval<bool>(json, "estimation.enable_if_slip_detector",
                                                "enable_if_slip_detector", false);

        // ----- Cycle-slip detector selection (SlipDetectorRegistry) -----
        // estimation.cycle_slip.{mw,graphic,ewl,gf,if}.  The Melbourne-Wübbena
        // detector defaults on (it is the primary and the source of WL integers
        // for AR / BSSD pivots); the auxiliary detectors default off, preserving
        // the historical MW-only behavior.  The 'if' selector inherits the
        // legacy estimation.enable_if_slip_detector flag as its default for
        // backward compatibility.
        cycle_slip_.mw      = jval<bool>(json, "estimation.cycle_slip.mw",      "cycle_slip_mw",      true);
        cycle_slip_.graphic = jval<bool>(json, "estimation.cycle_slip.graphic", "cycle_slip_graphic", false);
        cycle_slip_.ewl     = jval<bool>(json, "estimation.cycle_slip.ewl",     "cycle_slip_ewl",     false);
        cycle_slip_.gf      = jval<bool>(json, "estimation.cycle_slip.gf",      "cycle_slip_gf",      false);
        cycle_slip_.ifp     = jval<bool>(json, "estimation.cycle_slip.if",      "cycle_slip_if",      enable_if_slip_detector_);
        // turbo_edit is a coordination mode (MW+GF joint per-signal repair), not
        // a detector kind — rate-adaptive via the split gate; see CycleSlipConfig doc.
        cycle_slip_.turbo_edit = jval<bool>(json, "estimation.cycle_slip.turbo_edit", "cycle_slip_turbo_edit", false);
        // MW-only code-domain veto: an MW flag with an affirmatively stable
        // GF is classified as a code artifact and the phase reset is skipped
        // (KF residual gate backstops).  Default on — it only fires inside
        // turbo_edit's own trust gates; see CycleSlipConfig doc.
        cycle_slip_.turbo_edit_mw_only_veto =
            jval<bool>(json, "estimation.cycle_slip.turbo_edit_mw_only_veto",
                       "cycle_slip_turbo_edit_mw_only_veto", true);
        // Gap-spanning GF bridge cap (s); 0 disables.  Negative values are
        // clamped to 0 with a warning (a negative coherence bound is
        // meaningless).  See CycleSlipConfig doc.
        {
            double cap = jval<double>(json, "estimation.cycle_slip.turbo_edit_max_gap_s",
                                      "cycle_slip_turbo_edit_max_gap_s",
                                      cycle_slip_.turbo_edit_max_gap_s);
            if (cap < 0.0) {
                log->warn("estimation.cycle_slip.turbo_edit_max_gap_s {} < 0; "
                          "clamping to 0 (bridge disabled)", cap);
                cap = 0.0;
            }
            cycle_slip_.turbo_edit_max_gap_s = cap;
        }
        // Soft-LLI bridge: hold AR-able dual-freq ambiguities across a short
        // LLI gap and let the bridge integer-repair them instead of an
        // immediate full reset.  Default off (baseline unchanged); see
        // CycleSlipConfig doc + docs/turbo_edit_lli_bridge_design.md.
        cycle_slip_.turbo_edit_bridge_lli =
            jval<bool>(json, "estimation.cycle_slip.turbo_edit_bridge_lli",
                       "cycle_slip_turbo_edit_bridge_lli", false);
        // WL-settle deferral cap (s) for ambiguous bridged solves; 0
        // disables (an ambiguous solve resets immediately).  Negative
        // clamps to 0 with a warning.  See CycleSlipConfig doc.
        {
            double cap = jval<double>(json, "estimation.cycle_slip.turbo_edit_wl_settle_max_s",
                                      "cycle_slip_turbo_edit_wl_settle_max_s",
                                      cycle_slip_.turbo_edit_wl_settle_max_s);
            if (cap < 0.0) {
                log->warn("estimation.cycle_slip.turbo_edit_wl_settle_max_s {} < 0; "
                          "clamping to 0 (WL-settle deferral disabled)", cap);
                cap = 0.0;
            }
            cycle_slip_.turbo_edit_wl_settle_max_s = cap;
        }
        // Split-capability gate (cycles): the rate-adaptive lever — the joint
        // solve emits per-signal integers only when the arc's measured GF
        // noise satisfies σ_TD2 ≤ this × |λ1−λ2|.  Must be > 0; an invalid
        // override warns and keeps the default.  See CycleSlipConfig doc.
        {
            const double v = jval<double>(
                json, "estimation.cycle_slip.turbo_edit_split_max_sigma",
                "cycle_slip_turbo_edit_split_max_sigma",
                cycle_slip_.turbo_edit_split_max_sigma);
            if (v > 0.0) {
                cycle_slip_.turbo_edit_split_max_sigma = v;
            } else {
                log->warn("Invalid estimation.cycle_slip.turbo_edit_split_max_sigma "
                          "{} (must be > 0); keeping default {:.2f}",
                          v, cycle_slip_.turbo_edit_split_max_sigma);
            }
        }
        log->info("Cycle-slip detectors: mw={}, graphic={}, ewl={}, gf={}, if={}, "
                  "turbo_edit={} (mw_only_veto={}, gap_bridge={:.1f} s, bridge_lli={}, "
                  "wl_settle={:.1f} s, split_max_sigma={:.2f} cyc)",
                  cycle_slip_.mw ? "on" : "off", cycle_slip_.graphic ? "on" : "off",
                  cycle_slip_.ewl ? "on" : "off", cycle_slip_.gf ? "on" : "off",
                  cycle_slip_.ifp ? "on" : "off", cycle_slip_.turbo_edit ? "on" : "off",
                  cycle_slip_.turbo_edit_mw_only_veto ? "on" : "off",
                  cycle_slip_.turbo_edit_max_gap_s,
                  cycle_slip_.turbo_edit_bridge_lli ? "on" : "off",
                  cycle_slip_.turbo_edit_wl_settle_max_s,
                  cycle_slip_.turbo_edit_split_max_sigma);

        // Per-detector MAD threshold scale factors (kσ gate multipliers).  Each
        // defaults to the detector's historical value (already in cycle_slip_);
        // a non-positive override warns and keeps the default.
        auto load_sigma_mult = [&](const char* path, const char* alias, double& dst) {
            const double v = jval<double>(json, path, alias, dst);
            if (v > 0.0) {
                dst = v;
            } else {
                log->warn("Invalid {} {} (must be > 0); keeping default {:.2f}", path, v, dst);
            }
        };
        load_sigma_mult("estimation.cycle_slip.mw_sigma_multiplier",
                        "cycle_slip_mw_sigma_multiplier",         cycle_slip_.mw_sigma_multiplier);
        load_sigma_mult("estimation.cycle_slip.turbo_edit_sigma_multiplier",
                        "cycle_slip_turbo_edit_sigma_multiplier", cycle_slip_.turbo_edit_sigma_multiplier);
        load_sigma_mult("estimation.cycle_slip.graphic_sigma_multiplier",
                        "cycle_slip_graphic_sigma_multiplier",    cycle_slip_.graphic_sigma_multiplier);
        load_sigma_mult("estimation.cycle_slip.ewl_sigma_multiplier",
                        "cycle_slip_ewl_sigma_multiplier",        cycle_slip_.ewl_sigma_multiplier);
        log->info("Cycle-slip σ multipliers (kσ gate): mw={:.1f}, turbo_edit={:.1f}, graphic={:.1f}, ewl={:.1f}",
                  cycle_slip_.mw_sigma_multiplier, cycle_slip_.turbo_edit_sigma_multiplier,
                  cycle_slip_.graphic_sigma_multiplier, cycle_slip_.ewl_sigma_multiplier);

        // MW detection reference: which statistic the slip gate tests.
        // "hatch_mean" (legacy slow recursive mean, TD1-MAD spread),
        // "local_median" (short causal window median with a matching
        // window-MAD spread — trend-immune), or "time_difference" (the
        // deviation IS the MW time-difference, order per mw_td_order —
        // trend- AND curvature-immune; see MwDetectReference doc).
        {
            const std::string raw = jval<std::string>(json, "estimation.cycle_slip.mw_detect_reference",
                                                      "cycle_slip_mw_detect_reference", "hatch_mean");
            std::string low = toLower(raw);
            for (auto& c : low) if (c == '-') c = '_';
            if      (low == "hatch_mean")      cycle_slip_.mw_detect_reference = MwDetectReference::HatchMean;
            else if (low == "local_median")    cycle_slip_.mw_detect_reference = MwDetectReference::LocalMedian;
            else if (low == "time_difference") cycle_slip_.mw_detect_reference = MwDetectReference::TimeDifference;
            else {
                log->warn("Invalid estimation.cycle_slip.mw_detect_reference '{}' "
                          "(expected 'hatch_mean', 'local_median', or 'time_difference'); "
                          "defaulting to hatch_mean", raw);
                cycle_slip_.mw_detect_reference = MwDetectReference::HatchMean;
            }

            // Differencing order for the time_difference reference.
            const std::string raw_ord = jval<std::string>(json, "estimation.cycle_slip.mw_td_order",
                                                          "cycle_slip_mw_td_order", "first");
            std::string low_ord = toLower(raw_ord);
            if      (low_ord == "first")  cycle_slip_.mw_td_order = MwTdOrder::First;
            else if (low_ord == "second") cycle_slip_.mw_td_order = MwTdOrder::Second;
            else {
                log->warn("Invalid estimation.cycle_slip.mw_td_order '{}' "
                          "(expected 'first' or 'second'); defaulting to first", raw_ord);
                cycle_slip_.mw_td_order = MwTdOrder::First;
            }

            // Window length for the local-median reference.  Bounds match the
            // detector: ≥ 15 (MwSlipDetector::kMinEpochsForAdaptive — shorter
            // windows give a statistically marginal MAD and would retire the
            // warmup floors early) and ≤ 64 (fixed per-arc ring capacity).
            int win = jval<int>(json, "estimation.cycle_slip.mw_local_window",
                                "cycle_slip_mw_local_window", cycle_slip_.mw_local_window);
            constexpr int kMinLocalWindow = 15;
            constexpr int kMaxLocalWindow = 64;
            if (win < kMinLocalWindow || win > kMaxLocalWindow) {
                log->warn("estimation.cycle_slip.mw_local_window {} out of range [{}, {}]; clamping",
                          win, kMinLocalWindow, kMaxLocalWindow);
                win = std::clamp(win, kMinLocalWindow, kMaxLocalWindow);
            }
            cycle_slip_.mw_local_window = win;

            const char* ref_name =
                cycle_slip_.mw_detect_reference == MwDetectReference::LocalMedian    ? "local_median"
              : cycle_slip_.mw_detect_reference == MwDetectReference::TimeDifference ? "time_difference"
                                                                                     : "hatch_mean";
            log->info("MW detection reference: {} (td_order={}, local window {} epochs)",
                      ref_name,
                      cycle_slip_.mw_td_order == MwTdOrder::Second ? "second" : "first",
                      cycle_slip_.mw_local_window);
        }

        accumulate_phase_windup_  = jval<bool>(json, "estimation.accumulate_phase_windup",
                                                "accumulate_phase_windup", false);
        phase_windup_max_gap_s_   = jval<double>(json, "estimation.phase_windup_max_gap_s",
                                                 "phase_windup_max_gap_s", 300.0);
        if (!(phase_windup_max_gap_s_ > 0.0)) {
            log->warn("estimation.phase_windup_max_gap_s must be > 0 (a non-positive "
                      "horizon restarts the accumulator every epoch, silently "
                      "collapsing accumulated mode to fractional) — reset to 300.0 s.");
            phase_windup_max_gap_s_ = 300.0;
        }
        trace_windup_census_      = jval<bool>(json, "estimation.trace_windup_census",
                                               "trace_windup_census", false);

        // Between-satellite single differencing
        enable_between_satellite_sd_ = jval<bool>(json, "estimation.between_satellite_sd.enabled",
                                                   "enable_between_satellite_sd", false);
        enable_glonass_bssd_         = jval<bool>(json, "estimation.between_satellite_sd.enable_glonass",
                                                   "enable_glonass_bssd", false);
        // Per-FCN-slot GLONASS receiver inter-frequency code bias (M-PAGES item
        // #4).  FDMA: each slot is a distinct carrier frequency → a distinct
        // receiver group delay, so GLONASS code biases are modelled per
        // (band, slot) rather than per band.  Requires estimate_ifb + uncombined;
        // the central channel k=0 is the held datum.  Default false keeps the
        // GLONASS path byte-identical.
        enable_glonass_ifb_          = jval<bool>(json, "estimation.glonass_ifb.enable",
                                                   "enable_glonass_ifb", false);
        if (enable_glonass_ifb_ && !estimate_ifb_) {
            // The PPP-RTK network producer force-enables BOTH estimate_ifb and
            // glonass_ifb under network.estimate_glonass (loadNetworkSection_, which
            // dispatches AFTER this estimation section), so the prerequisite is
            // satisfied there — don't warn on that config (the warning, evaluated on
            // the pre-force values, would be misleading).
            const bool glonass_network =
                jval<bool>(json, "network.estimate_glonass", "estimate_glonass", false);
            if (!glonass_network) {
                log->warn("enable_glonass_ifb is set but estimate_ifb is off — per-slot "
                          "GLONASS IFB needs estimate_ifb=true in uncombined PPP-RTK; it will be inactive.");
            }
        }
        // CIO-based IAU 2000A/2006 ECI↔ECEF transform (SOFA) for Sun/Moon; off
        // by default → legacy IAU 1976/1980 equinox chain stays byte-identical.
        enable_cio_frame_            = jval<bool>(json, "estimation.enable_cio_frame",
                                                   "enable_cio_frame", false);

        // Cancel the inherited RTKLIB precession double-count in the Sun/Moon ECI
        // vectors (sunMoonPosEci returns mean-of-date; both ECI→ECEF chains then
        // apply precession again).  Off by default → every existing lineage,
        // product and sim fixture stays byte-identical; arming it is a migration
        // because it moves solid tides, satellite attitude/PCO, phase wind-up and
        // the eclipse-gate edges together.
        enable_sun_frame_correction_ = jval<bool>(json, "estimation.enable_sun_frame_correction",
                                                   "enable_sun_frame_correction", false);

        // Statistical QC layer (item #9 / M-PAGES NgsLsq variance factor +
        // Baarda data-snooping).  All three default false → the post-fit
        // residual / outlier path is byte-identical.
        //  * variance_factor : a-posteriori σ̂₀² = (Σ vᵢ²/Rᵢᵢ)/dof, reported
        //    only (rescales a copy of σ_pos, never the propagated P).
        //  * data_snooping   : per-row redundancy/w-test/MDB + a w-test
        //    outlier gate (|w| > data_snooping_critical) over checkable rows.
        //  * full_r_kernel   : form the post-fit residual covariance with the
        //    off-diagonals of a dense R (BSSD/DD) instead of the diagonal,
        //    so the redundancy/w-test honour correlated observations.
        enable_variance_factor_ = jval<bool>(json, "estimation.enable_variance_factor",
                                             "enable_variance_factor", false);
        enable_data_snooping_   = jval<bool>(json, "estimation.enable_data_snooping",
                                             "enable_data_snooping", false);
        data_snooping_critical_ = jval<double>(json, "estimation.data_snooping_critical",
                                               "data_snooping_critical", 3.29);
        if (data_snooping_critical_ <= 0.0) {
            log->warn("Non-positive data_snooping_critical ({:.3f}) ignored; reverting to 3.29",
                      data_snooping_critical_);
            data_snooping_critical_ = 3.29;
        }
        enable_full_r_kernel_   = jval<bool>(json, "estimation.enable_full_r_kernel",
                                             "enable_full_r_kernel", false);
        bssd_pivot_min_elevation_deg_ = jval<double>(json, "estimation.between_satellite_sd.pivot_min_elevation",
                                                      "bssd_pivot_min_elevation", 45.0);

        // Pivot-slip detector wavelength floor.  Suppresses false-positive
        // handovers from sub-cycle coherent SD residuals (multipath, PCV
        // miscal, kinematic dynamics) that cannot be real cycle slips.
        // Default 0.25 (λ/4); 0 disables the floor.
        pivot_slip_min_cycles_ = jval<double>(json,
            "estimation.pivot_slip_min_cycles", "pivot_slip_min_cycles", 0.25);
        if (pivot_slip_min_cycles_ < 0.0) pivot_slip_min_cycles_ = 0.0;

        // Persistent-bias R-inflation threshold (cycles of band λ on the
        // EWMA mean).  When a (sat, signal)'s residual history shows a
        // bias > this fraction of λ, the row's R is inflated so the KF
        // stops absorbing the unmodeled error into position/clock/tropo.
        // 0 disables.
        persistent_bias_min_cycles_ = jval<double>(json,
            "estimation.persistent_bias_min_cycles", "persistent_bias_min_cycles", 0.10);
        if (persistent_bias_min_cycles_ < 0.0) persistent_bias_min_cycles_ = 0.0;

        // In-update Huber R-inflation threshold (standardized residual).
        // Acts on a standardized phase residual, no history needed —
        // catches first-epoch biases that the EWMA persistence detector
        // is too slow to identify.  Default 6.0 — the same threshold as
        // the outlier reset (sub-cycle phase rows whose standardized
        // residual exceeds it get their R inflated toward the
        // threshold).  0 disables.
        phase_huber_sres_threshold_ = jval<double>(json,
            "estimation.phase_huber_sres_threshold", "phase_huber_sres_threshold", 6.0);
        if (phase_huber_sres_threshold_ < 0.0) phase_huber_sres_threshold_ = 0.0;

        // Huber R-inflation scheme.  Default (false) is the post-fit
        // IRLS: inflate R by (|sres|/k)^2 per rebuild pass until the
        // post-fit standardized residual converges to k.  When true,
        // standardize the pre-fit innovation by its innovation
        // covariance and set R in closed form so the standardized
        // residual lands on k in a single rebuild pass.
        enable_huber_innovation_ = jval<bool>(json,
            "estimation.enable_huber_innovation", "enable_huber_innovation", false);
        if (enable_huber_innovation_ && phase_huber_sres_threshold_ > 0.0) {
            log->info("Huber R-inflation: innovation-based one-shot scheme "
                      "(estimation.enable_huber_innovation=true)");
        }

        // Master on/off for the Huber R-inflation pass.  Independent of
        // the threshold knob above so users can deactivate the entire
        // pass without setting a sentinel value.  Defaults to true so
        // existing configs (which only set the threshold) keep working.
        enable_huber_r_inflation_ = jval<bool>(json,
            "estimation.enable_huber_r_inflation",
            "enable_huber_r_inflation", true);
        if (!enable_huber_r_inflation_) {
            log->info("Huber R-inflation pass: DISABLED via "
                      "estimation.enable_huber_r_inflation=false "
                      "(phase_huber_sres_threshold={} ignored)",
                      phase_huber_sres_threshold_);
        }

        // Master on/off for the per-row sub-cycle slip suppression
        // (λ-fraction floor in the phase outlier dispatch).  Independent
        // of @c pivot_slip_min_cycles, which sets the floor's value.
        // Defaults to true to preserve existing behavior.
        enable_subcycle_slip_suppression_ = jval<bool>(json,
            "estimation.enable_subcycle_slip_suppression",
            "enable_subcycle_slip_suppression", true);
        if (!enable_subcycle_slip_suppression_) {
            log->info("Sub-cycle phase slip suppression: DISABLED via "
                      "estimation.enable_subcycle_slip_suppression=false "
                      "(pivot_slip_min_cycles={} ignored in per-row dispatch)",
                      pivot_slip_min_cycles_);
        }

        // Residual history tracker (post-fit temporal corroboration of
        // spatial pivot-slip detection).  Defaults are off — opt in
        // via JSON.  See ResidualHistoryTracker.hpp for the EWMA
        // recurrence and reset semantics.
        residual_history_.enabled = jval<bool>(json,
            "estimation.residual_history.enabled",   "residual_history_enabled",   false);
        residual_history_.alpha   = jval<double>(json,
            "estimation.residual_history.alpha",     "residual_history_alpha",     0.05);
        residual_history_.warmup_n = static_cast<std::size_t>(std::max<int>(1, jval<int>(json,
            "estimation.residual_history.warmup_n",  "residual_history_warmup_n",  20)));
        residual_history_.z_threshold = jval<double>(json,
            "estimation.residual_history.z_threshold", "residual_history_z_threshold", 4.0);
        residual_history_.fraction_threshold = jval<double>(json,
            "estimation.residual_history.fraction_threshold", "residual_history_fraction_threshold", 0.5);
        if (residual_history_.enabled) {
            log->info("Residual history tracker: alpha={:.3f}, warmup_n={}, "
                "z_threshold={:.2f}, fraction_threshold={:.2f}",
                residual_history_.alpha, residual_history_.warmup_n,
                residual_history_.z_threshold, residual_history_.fraction_threshold);
        }

        // Stuck-code-bias detector — breaks the closed loop where an
        // over-converged sat-bias state keeps causing its own code
        // rows to be rejected as outliers (innovation → reject →
        // bias never updates → same innovation next epoch).  Driven
        // by the residual-history tracker's per-row EWMA mean and
        // consecutive-rejection streak.  Default-on; cheap when no
        // row is stuck.  See StuckCodeBiasConfig docs for trigger
        // semantics.
        stuck_code_bias_.enabled = jval<bool>(json,
            "estimation.stuck_code_bias.enabled",     "stuck_code_bias_enabled",     true);
        stuck_code_bias_.min_streak = static_cast<std::size_t>(std::max<int>(0, jval<int>(json,
            "estimation.stuck_code_bias.min_streak",  "stuck_code_bias_min_streak",  5)));
        stuck_code_bias_.bias_floor_m = jval<double>(json,
            "estimation.stuck_code_bias.bias_floor_m","stuck_code_bias_bias_floor_m", 3.0);
        if (stuck_code_bias_.bias_floor_m < 0.0) stuck_code_bias_.bias_floor_m = 0.0;
        if (stuck_code_bias_.enabled && estimate_sat_biases_) {
            log->info("Stuck-code-bias detector: min_streak={} epochs, bias_floor={:.2f} m "
                "(streak_branch={}, drift_branch={})",
                stuck_code_bias_.min_streak, stuck_code_bias_.bias_floor_m,
                stuck_code_bias_.min_streak > 0 ? "on" : "off",
                stuck_code_bias_.bias_floor_m > 0.0 ? "on" : "off");
        }

        // Sat-wide state re-inflation on phase ambiguity reset (LLI
        // lock loss or sat-wide MW slip).  Re-inflates iono and
        // sat-bias diagonals so the post-reseed filter can re-find
        // the (iono, sat-bias) partition rather than baking in the
        // pre-reset one for the rest of the arc.  See
        // SlipStateReinflationConfig docs for the partition-locking
        // failure mode.
        slip_state_reinflation_.enabled = jval<bool>(json,
            "estimation.slip_state_reinflation.enabled",         "slip_state_reinflation_enabled",      true);
        slip_state_reinflation_.iono_std_m = jval<double>(json,
            "estimation.slip_state_reinflation.iono_std_m",      "slip_state_reinflation_iono_std_m",   5.0);
        slip_state_reinflation_.sat_bias_std_m = jval<double>(json,
            "estimation.slip_state_reinflation.sat_bias_std_m",  "slip_state_reinflation_sat_bias_std_m", 5.0);
        if (slip_state_reinflation_.enabled
            && (uncombined_enabled_ || estimate_sat_biases_)) {
            log->info("Slip-state re-inflation: iono_std={:.2f} m, sat_bias_std={:.2f} m "
                "(iono_branch={}, sat_bias_branch={})",
                slip_state_reinflation_.iono_std_m,
                slip_state_reinflation_.sat_bias_std_m,
                slip_state_reinflation_.iono_std_m > 0.0 ? "on" : "fallback-cold-start",
                slip_state_reinflation_.sat_bias_std_m > 0.0 ? "on" : "fallback-cold-start");
        }

        // Real-time startup gate: defer ALL estimation until the SSR
        // correction stream is ready — every modeled above-mask satellite is
        // broadcast+SSR (cm-grade clock, phase retained) and a solvable floor
        // of them is present — so the filter seeds code+phase together instead
        // of cold-starting code-only on metre-grade broadcast clocks (which
        // bakes a contaminated iono/bias/ambiguity partition as each sat's SSR
        // flips it cm-grade mid-arc).  Real-time only; no-op in post-processing
        // (precise CLK present -> never broadcast_only).  See
        // CorrectionsReadyGateConfig docs.
        corrections_ready_gate_.enabled = jval<bool>(json,
            "estimation.defer_until_corrections_ready.enabled",        "defer_until_corrections_ready_enabled",        false);
        corrections_ready_gate_.min_satellites = std::max<int>(1, jval<int>(json,
            "estimation.defer_until_corrections_ready.min_satellites", "defer_until_corrections_ready_min_satellites",  5));
        corrections_ready_gate_.min_satellites_per_system = jval<int>(json,
            "estimation.defer_until_corrections_ready.min_satellites_per_system", "defer_until_corrections_ready_min_satellites_per_system", 4);
        corrections_ready_gate_.timeout_s = std::max<double>(0.0, jval<double>(json,
            "estimation.defer_until_corrections_ready.timeout_s",      "defer_until_corrections_ready_timeout_s",       0.0));
        corrections_ready_gate_.require_all_above_mask = jval<bool>(json,
            "estimation.defer_until_corrections_ready.require_all_above_mask", "defer_until_corrections_ready_require_all_above_mask", false);
        corrections_ready_gate_.drop_broadcast_grade = jval<bool>(json,
            "estimation.defer_until_corrections_ready.drop_broadcast_grade", "defer_until_corrections_ready_drop_broadcast_grade", false);
        if (corrections_ready_gate_.enabled) {
            const char* mode = corrections_ready_gate_.require_all_above_mask
                ? "all above-mask broadcast+SSR"
                : (corrections_ready_gate_.min_satellites_per_system > 0
                    ? "per-constellation quorum" : "global quorum");
            log->info("Corrections-ready startup gate ON: {}, >= {} cm-grade total, "
                ">= {} per constellation, drop_broadcast_grade={}, {}",
                mode, corrections_ready_gate_.min_satellites,
                corrections_ready_gate_.min_satellites_per_system,
                corrections_ready_gate_.drop_broadcast_grade ? "on" : "off",
                corrections_ready_gate_.timeout_s > 0.0 ? "timeout -> degraded" : "wait indefinitely");
        }

        // Soft a-priori ZWD constraint — anchors the BSSD [up, ZWD] null space
        // so a common-mode start error cannot run ZWD/height to ~1000 m.  See
        // ZwdConstraintConfig docs.  Default off; opt in for real-time BSSD.
        zwd_constraint_.enabled = jval<bool>(json,
            "estimation.zwd_constraint.enabled",   "zwd_constraint_enabled",   false);
        zwd_constraint_.apriori_m = jval<double>(json,
            "estimation.zwd_constraint.apriori_m", "zwd_constraint_apriori_m", 0.10);
        zwd_constraint_.sigma_m = jval<double>(json,
            "estimation.zwd_constraint.sigma_m",   "zwd_constraint_sigma_m",   0.30);
        if (zwd_constraint_.enabled) {
            log->info("ZWD soft constraint ON: prior {:.2f} m +/- {:.2f} m",
                zwd_constraint_.apriori_m, zwd_constraint_.sigma_m);
        }

        // Adaptive ionospheric process noise (GF second-difference driven).
        // Defaults off — opt in via JSON.  See AdaptiveIonoQConfig and
        // IonoVariabilityMonitor for the recent/baseline MAD-sigma ratio scheme.
        adaptive_iono_q_.enabled = jval<bool>(json,
            "estimation.adaptive_iono_process_noise.enable", "adaptive_iono_q_enable", false);
        adaptive_iono_q_.recent_window = std::max<int>(5, jval<int>(json,
            "estimation.adaptive_iono_process_noise.recent_window", "adaptive_iono_q_recent_window", 20));
        adaptive_iono_q_.baseline_window = std::max<int>(adaptive_iono_q_.recent_window, jval<int>(json,
            "estimation.adaptive_iono_process_noise.baseline_window", "adaptive_iono_q_baseline_window", 240));
        adaptive_iono_q_.max_multiplier = jval<double>(json,
            "estimation.adaptive_iono_process_noise.max_multiplier", "adaptive_iono_q_max_multiplier", 100.0);
        if (adaptive_iono_q_.max_multiplier < 1.0) adaptive_iono_q_.max_multiplier = 1.0;
        adaptive_iono_q_.min_epochs = std::max<int>(5, jval<int>(json,
            "estimation.adaptive_iono_process_noise.min_epochs", "adaptive_iono_q_min_epochs", 30));
        adaptive_iono_q_.trace = jval<bool>(json,
            "estimation.adaptive_iono_process_noise.trace", "adaptive_iono_q_trace", false);
        if (adaptive_iono_q_.enabled) {
            log->info("Adaptive iono process noise: recent_window={}, baseline_window={}, "
                      "max_multiplier={:.1f}, min_epochs={}, trace={}",
                      adaptive_iono_q_.recent_window, adaptive_iono_q_.baseline_window,
                      adaptive_iono_q_.max_multiplier, adaptive_iono_q_.min_epochs,
                      adaptive_iono_q_.trace ? "on" : "off");
        }

        // GIM ionosphere weighting (PPP-RTK soft iono constraint).
        // Defaults off — opt in via JSON.  See IonosphereWeightingConfig.
        iono_weighting_.enabled = jval<bool>(json,
            "estimation.ionosphere_weighting.enable", "ionosphere_weighting_enable", false);
        iono_weighting_.common_mode_sigma_m = jval<double>(json,
            "estimation.ionosphere_weighting.common_mode_sigma_m",
            "ionosphere_weighting_common_mode_sigma_m", 50.0);
        iono_weighting_.std_scale = jval<double>(json,
            "estimation.ionosphere_weighting.std_scale", "ionosphere_weighting_std_scale", 1.0);
        iono_weighting_.floor_m = jval<double>(json,
            "estimation.ionosphere_weighting.floor_m", "ionosphere_weighting_floor_m", 0.05);
        iono_weighting_.elevation_cutoff_deg = jval<double>(json,
            "estimation.ionosphere_weighting.elevation_cutoff_deg",
            "ionosphere_weighting_elevation_cutoff_deg", 10.0);
        if (iono_weighting_.common_mode_sigma_m < 1.0) iono_weighting_.common_mode_sigma_m = 1.0;
        if (iono_weighting_.std_scale < 0.0) iono_weighting_.std_scale = 1.0;
        if (iono_weighting_.floor_m < 0.0) iono_weighting_.floor_m = 0.0;
        if (iono_weighting_.enabled) {
            log->info("GIM ionosphere weighting: common_mode_sigma={:.1f} m, std_scale={:.2f}, "
                      "floor={:.3f} m, elevation_cutoff={:.1f} deg",
                      iono_weighting_.common_mode_sigma_m, iono_weighting_.std_scale,
                      iono_weighting_.floor_m, iono_weighting_.elevation_cutoff_deg);
        }

        // Co-estimated global SH-VTEC ionosphere (network producer).  The master
        // toggle (global_vtec_sh_.enabled) is driven by network.estimate_iono_sh in
        // loadNetworkSection_; the tuning knobs are parsed here so they are
        // available regardless of section order.  Defaults are the Stage-1 design
        // values (degree 4, 450 km shell, loose Kaula/a₀₀ priors).
        global_vtec_sh_.max_degree = jval<int>(json,
            "estimation.global_vtec_sh.max_degree", "global_vtec_sh_max_degree", 4);
        global_vtec_sh_.layer_height_km = jval<double>(json,
            "estimation.global_vtec_sh.layer_height_km", "global_vtec_sh_layer_height_km", 450.0);
        global_vtec_sh_.earth_radius_km = jval<double>(json,
            "estimation.global_vtec_sh.earth_radius_km", "global_vtec_sh_earth_radius_km", 6371.0);
        global_vtec_sh_.kaula_scale_tecu = jval<double>(json,
            "estimation.global_vtec_sh.kaula_scale_tecu", "global_vtec_sh_kaula_scale_tecu", 10.0);
        global_vtec_sh_.q_per_degree = jval<double>(json,
            "estimation.global_vtec_sh.q_per_degree", "global_vtec_sh_q_per_degree", 1.0e-2);
        global_vtec_sh_.mean_vtec_prior_tecu = jval<double>(json,
            "estimation.global_vtec_sh.mean_vtec_prior_tecu", "global_vtec_sh_mean_vtec_prior_tecu", 15.0);
        global_vtec_sh_.mean_vtec_prior_sigma_tecu = jval<double>(json,
            "estimation.global_vtec_sh.mean_vtec_prior_sigma_tecu",
            "global_vtec_sh_mean_vtec_prior_sigma_tecu", 30.0);
        global_vtec_sh_.representation_sigma_tecu = jval<double>(json,
            "estimation.global_vtec_sh.representation_sigma_tecu",
            "global_vtec_sh_representation_sigma_tecu", 5.0);  // matches GlobalVtecShConfig struct default
        global_vtec_sh_.sun_fixed = jval<bool>(json,
            "estimation.global_vtec_sh.sun_fixed", "global_vtec_sh_sun_fixed", true);
        global_vtec_sh_.trace = jval<bool>(json,
            "estimation.global_vtec_sh.trace", "global_vtec_sh_trace", false);
        // T1a physical-band probe on the estimated mean VTEC (a₀₀): a WARN fires when
        // a₀₀ leaves [low, high] TECU so a diverging global field is visible + attributable.
        // Pure diagnostic — never touches state.  Set low ≥ high to silence.
        global_vtec_sh_.a00_warn_low_tecu = jval<double>(json,
            "estimation.global_vtec_sh.a00_warn_low_tecu", "global_vtec_sh_a00_warn_low_tecu", 2.0);
        global_vtec_sh_.a00_warn_high_tecu = jval<double>(json,
            "estimation.global_vtec_sh.a00_warn_high_tecu", "global_vtec_sh_a00_warn_high_tecu", 150.0);
        // T1b: SH pseudo-obs row cadence (0/1 = every epoch = current behaviour).
        global_vtec_sh_.row_decimation_epochs = jval<int>(json,
            "estimation.global_vtec_sh.row_decimation_epochs", "global_vtec_sh_row_decimation_epochs", 0);
        if (global_vtec_sh_.row_decimation_epochs < 0) global_vtec_sh_.row_decimation_epochs = 0;
        if (global_vtec_sh_.max_degree < 0)         global_vtec_sh_.max_degree = 0;
        if (global_vtec_sh_.layer_height_km <= 0.0)  global_vtec_sh_.layer_height_km = 450.0;
        if (global_vtec_sh_.earth_radius_km  <= 0.0) global_vtec_sh_.earth_radius_km = 6371.0;
        if (global_vtec_sh_.kaula_scale_tecu <= 0.0) global_vtec_sh_.kaula_scale_tecu = 10.0;
        if (global_vtec_sh_.mean_vtec_prior_sigma_tecu <= 0.0)
            global_vtec_sh_.mean_vtec_prior_sigma_tecu = 30.0;
        if (global_vtec_sh_.representation_sigma_tecu <= 0.0)
            global_vtec_sh_.representation_sigma_tecu = 5.0;
        // Iono-2 mechanism A: optional second thin shell (dual-layer SH field).
        global_vtec_sh_.layer2_max_degree = jval<int>(json,
            "estimation.global_vtec_sh.layer2_max_degree", "global_vtec_sh_layer2_max_degree", 0);
        global_vtec_sh_.layer2_height_km = jval<double>(json,
            "estimation.global_vtec_sh.layer2_height_km", "global_vtec_sh_layer2_height_km", 1300.0);
        global_vtec_sh_.layer2_kaula_scale_tecu = jval<double>(json,
            "estimation.global_vtec_sh.layer2_kaula_scale_tecu",
            "global_vtec_sh_layer2_kaula_scale_tecu", 3.0);
        global_vtec_sh_.layer2_q_per_degree = jval<double>(json,
            "estimation.global_vtec_sh.layer2_q_per_degree",
            "global_vtec_sh_layer2_q_per_degree", 1.0e-2);
        global_vtec_sh_.layer2_mean_vtec_prior_tecu = jval<double>(json,
            "estimation.global_vtec_sh.layer2_mean_vtec_prior_tecu",
            "global_vtec_sh_layer2_mean_vtec_prior_tecu", 3.0);
        global_vtec_sh_.layer2_mean_vtec_prior_sigma_tecu = jval<double>(json,
            "estimation.global_vtec_sh.layer2_mean_vtec_prior_sigma_tecu",
            "global_vtec_sh_layer2_mean_vtec_prior_sigma_tecu", 5.0);
        if (global_vtec_sh_.layer2_max_degree < 0) global_vtec_sh_.layer2_max_degree = 0;
        if (global_vtec_sh_.layer2_height_km <= 0.0) global_vtec_sh_.layer2_height_km = 1300.0;
        if (global_vtec_sh_.layer2_kaula_scale_tecu <= 0.0)
            global_vtec_sh_.layer2_kaula_scale_tecu = 3.0;
        if (global_vtec_sh_.layer2_q_per_degree <= 0.0)
            global_vtec_sh_.layer2_q_per_degree = 1.0e-2;
        if (global_vtec_sh_.layer2_mean_vtec_prior_sigma_tecu <= 0.0)
            global_vtec_sh_.layer2_mean_vtec_prior_sigma_tecu = 5.0;
        if (global_vtec_sh_.layer2_max_degree > 0 && log)
            log->info("Network (Iono-2): dual-layer SH-VTEC ON — layer 2 degree {} at "
                      "{} km (Kaula {} TECU, a00 anchor {}±{} TECU); pin rows sum both "
                      "shells (keys SH2|n|m|t on the shared border).",
                      global_vtec_sh_.layer2_max_degree, global_vtec_sh_.layer2_height_km,
                      global_vtec_sh_.layer2_kaula_scale_tecu,
                      global_vtec_sh_.layer2_mean_vtec_prior_tecu,
                      global_vtec_sh_.layer2_mean_vtec_prior_sigma_tecu);
        // T5: correlated per-LOS SH representativeness-error deviation (GM carry).
        global_vtec_sh_.rep_error_tau_s = jval<double>(json,
            "estimation.global_vtec_sh.rep_error_tau_s", "global_vtec_sh_rep_error_tau_s", 0.0);
        if (global_vtec_sh_.rep_error_tau_s < 0.0) global_vtec_sh_.rep_error_tau_s = 0.0;
        // Iono-2 v3: carried-prior common-mode de-mean (the anti-ratchet;
        // docs/SPEC_iono2_mechB_v3.md).  Inert without the GM deviation itself.
        global_vtec_sh_.rep_error_demean = jval<bool>(json,
            "estimation.global_vtec_sh.rep_error_demean", "global_vtec_sh_rep_error_demean", false);
        // Iono-2 v3k: erase-policy split (raw-confirmed slip ⇒ erase; D5-only
        // re-tap ⇒ keep level, re-tap variance).  Inert without the GM deviation.
        global_vtec_sh_.rep_error_d5_keep = jval<bool>(json,
            "estimation.global_vtec_sh.rep_error_d5_keep", "global_vtec_sh_rep_error_d5_keep", false);
        if (global_vtec_sh_.rep_error_tau_s > 0.0) {
            if (log) {
                log->info("Network (Iono-2/T5v2): SH representativeness-error GM deviation ON — the "
                          "SH↔slant pin's per-LOS misfit is tracked as a Gauss–Markov deviation "
                          "(τ {} s, steady-state σ = representation_sigma_tecu {} TECU, carried in "
                          "vertical TECU) with the EXACT joint-posterior harvest (v2; the v1 scalar "
                          "filter was circular).  This is the per-LOS iono temporal-accumulation "
                          "channel targeting the NL-AR representation blocker (T4/Jul-17 A/B).",
                          global_vtec_sh_.rep_error_tau_s,
                          global_vtec_sh_.representation_sigma_tecu);
                if (global_vtec_sh_.rep_error_demean)
                    log->info("Network (Iono-2/v3): carried-prior common-mode de-mean ON — the "
                              "information-weighted global and per-station means are removed from "
                              "the deviation priors each epoch (the a00-twin / receiver-lump "
                              "quasi-null directions; anti-ratchet, SPEC_iono2_mechB_v3).");
                if (global_vtec_sh_.rep_error_d5_keep) {
                    log->info("Network (Iono-2/v3k): D5-keep erase-policy split ON — only "
                              "RAW-detector-confirmed slips erase the deviation; D5-only re-taps "
                              "keep the level and re-tap the variance to steady state (97 %% of "
                              "D5 re-taps have no raw step — iono tension the deviation absorbs).");
                    if (!global_vtec_sh_.rep_error_demean)
                        log->warn("Network (Iono-2/v3k): rep_error_d5_keep WITHOUT rep_error_demean "
                                  "— kept levels have no common-mode drain; the measured a00 "
                                  "ratchet can re-open through them.  Arm rep_error_demean.");
                }
                if (global_vtec_sh_.row_decimation_epochs >= 2)
                    log->warn("Network (T5): rep_error_tau_s supersedes row_decimation_epochs "
                              "({}) — the T1b σ-inflation is ignored while the GM deviation is on.",
                              global_vtec_sh_.row_decimation_epochs);
            }
            global_vtec_sh_.row_decimation_epochs = 0;
        } else if ((global_vtec_sh_.rep_error_demean || global_vtec_sh_.rep_error_d5_keep) && log) {
            log->warn("Network (Iono-2/v3): rep_error_demean/rep_error_d5_keep set but "
                      "rep_error_tau_s is 0 — both act on the GM deviation carry, so they are "
                      "INERT without it.");
        }

        // Ambiguity resolution
        ar_mode_            = jval<int>(json, "estimation.ambiguity_resolution.mode",
                                        "ar_mode", 0);
        ar_ratio_threshold_ = jval<double>(json, "estimation.ambiguity_resolution.ratio_threshold",
                                            "ar_ratio_threshold", 3.0);
        ar_elevation_mask_  = jval<double>(json, "estimation.ambiguity_resolution.elevation_mask",
                                            "ar_elevation_mask", 20.0);
        ar_hold_variance_   = jval<double>(json, "estimation.ambiguity_resolution.hold_variance",
                                            "ar_hold_variance", 0.001);
        ar_min_epochs_           = jval<int>(json, "estimation.ambiguity_resolution.min_epochs",
                                              "ar_min_epochs", 10);
        ar_max_position_sigma_m_ = jval<double>(json, "estimation.ambiguity_resolution.max_position_sigma_m",
                                                 "ar_max_position_sigma_m", 0.05);
        ar_ratio_upper_bound_    = jval<double>(json, "estimation.ambiguity_resolution.ratio_upper_bound",
                                                 "ar_ratio_upper_bound", 1e4);
        ar_wl_only_              = jval<bool>(json, "estimation.ambiguity_resolution.wl_only",
                                               "ar_wl_only", false);
        ar_wl_only_pseudo_var_   = jval<double>(json, "estimation.ambiguity_resolution.wl_only_pseudo_var",
                                                 "ar_wl_only_pseudo_var", 1e-4);
        ar_wl_max_frac_          = jval<double>(json, "estimation.ambiguity_resolution.wl_max_frac",
                                                 "ar_wl_max_frac", 0.40);
        ar_min_success_rate_     = jval<double>(json, "estimation.ambiguity_resolution.min_success_rate",
                                                 "ar_min_success_rate", 0.999);
        // The WL-only / MW-WL paths apply MW integers directly and had NO
        // success-rate gate at all — only the wl_max_frac point test.  Default
        // ON: this closes a fail-unsafe path (an ungated 5-constraint fix put a
        // held-out consumer 35 m off, SPEC_clock_temporal_anchor §8.11).  Set
        // false to reproduce the pre-fix behaviour exactly.
        ar_wl_only_ffr_          = jval<bool>(json, "estimation.ambiguity_resolution.wl_only_ffr",
                                              "ar_wl_only_ffr", true);
        ar_wl_only_min_arc_count_ = jval<int>(json, "estimation.ambiguity_resolution.wl_only_min_arc_count",
                                              "ar_wl_only_min_arc_count", 10);
        ar_wl_only_max_innov_cyc_ = jval<double>(json, "estimation.ambiguity_resolution.wl_only_max_innov_cyc",
                                                 "ar_wl_only_max_innov_cyc", 0.5);
        if (!ar_wl_only_ffr_)
            log->warn("AR: estimation.ambiguity_resolution.wl_only_ffr is OFF — MW wide-lane "
                      "integers will be applied with no bootstrap success-rate gate, which is "
                      "the pre-2026-08-11 fail-unsafe behaviour.  Intended only for reproducing "
                      "a stored lineage.");
        ar_min_arc_info_         = jval<double>(json, "estimation.ambiguity_resolution.min_arc_info",
                                                 "ar_min_arc_info", 0.0);
        ar_nl_max_per_pair_frac_ = jval<double>(json, "estimation.ambiguity_resolution.nl_max_per_pair_frac",
                                                 "ar_nl_max_per_pair_frac", 0.15);
        ar_enable_partial_ar_    = jval<bool>(json, "estimation.ambiguity_resolution.enable_partial_ar",
                                               "ar_enable_partial_ar", false);
        ar_nl_iono_free_row_     = jval<bool>(json, "estimation.ambiguity_resolution.nl_iono_free_row",
                                               "ar_nl_iono_free_row", false);
        if (ar_nl_iono_free_row_) {
            // Invariant #4: an armed functional switch must announce itself, and
            // this one is only safe with a hardened widelane (8.06× WL
            // sensitivity vs the narrowlane row).
            log->warn("AR: WL-constrained N_1 search is on the exact IONO-FREE row "
                      "(c_IF·alpha = 0).  This is 8.06x more sensitive to a widelane "
                      "error than the narrowlane row — a 1-cycle WL error moves the "
                      "N_1 target 3.53 cyc.  Verify the widelane before trusting fixes.");
        }

        // estimation.ambiguity_resolution.active_systems = ["GPS","Galileo",...]
        // Empty / missing → all active systems are eligible for AR.
        ar_active_systems_.clear();
        {
            const auto& arr = jsection(json, "estimation.ambiguity_resolution.active_systems",
                                        "ar_active_systems");
            if (arr.is_array())
                ar_active_systems_ = arr.get<std::vector<std::string>>();
        }
        if (!ar_active_systems_.empty()) {
            log->info("AR restricted to constellations: {}", joinStrings(ar_active_systems_));
        }

        // State-vector ambiguity parameterization.  PerBand is the
        // historical layout (one state per (sat, signal/IF combo));
        // NlWl decomposes each dual-frequency ambiguity into NL + WL
        // states per band-pair.  Default PerBand to preserve behavior.
        {
            std::string raw = jval<std::string>(json, "estimation.state_ambiguity_basis",
                                                 "state_ambiguity_basis", "per_band");
            std::string low = toLower(raw);
            for (auto& c : low) if (c == '-') c = '_';
            if      (low == "per_band")              state_ambiguity_basis_ = StateAmbiguityBasis::PerBand;
            else if (low == "nl_wl" || low == "nlwl") state_ambiguity_basis_ = StateAmbiguityBasis::NlWl;
            else {
                log->warn("Unknown state_ambiguity_basis '{}', defaulting to per_band", raw);
                state_ambiguity_basis_ = StateAmbiguityBasis::PerBand;
            }
        }
        // NOTE: the consolidated "Toggles: …" echo used to print HERE, and printed
        // PRE-RESOLUTION values.  It moved to logConfigurationSummary_ (same reason
        // the aiding block below is loaded at the tail of the IMU section): this
        // helper is call #2 of 12 in loadFromJson, and NINE of the fields that line
        // reports are still mutated by three LATER sections — the IMU section, the
        // app-mode/real-time section, and the whole network section.  `ifb=` was the
        // reported case (the W6 and EXT-CB slices and the GLONASS slice all force
        // estimate_ifb_ on afterwards); `imu=` was strictly worse, since
        // imu_config_.enabled is still its NSDMI `false` here and is not parsed until
        // ~800 lines later, so that field could only ever print "off".  Because the
        // mutations span three different sections, an in-place annotation could not
        // have named the affected fields — relocation is the fix.
    }

    // =================================================================
    //  Section: filter
    // =================================================================
    void GnssConfig::loadFilterSection_(const nlohmann::json& json, LogManager& log_manager) {
        auto log = log_manager.getLogger("gnss_config");
        {
            const auto& pn = jsection(json, "filter.process_noise", "process_noise");
            process_noise_.position            = pn.value("position_q", 0.0);
            process_noise_.clock               = pn.value("clock_bias_q", 1e4);
            process_noise_.troposphere         = pn.value("troposphere_q", 1e-8);
            process_noise_.ionosphere          = pn.value("ionosphere_q", 0.10);
            process_noise_.iono_rate_psd       = pn.value("iono_rate_psd", 1e-10);
            process_noise_.system_time_offset  = pn.value("system_time_offset_q", 1e-7);
            process_noise_.code_bias           = pn.value("code_bias_q", 1e-4);
            process_noise_.tropo_gradient      = pn.value("tropo_gradient_q", 5e-12);
            process_noise_.phase_clock         = pn.value("phase_clock_q", 1e-9);
            // CWNA acceleration power spectral density (m*m/s^3).  Drives the
            // coupled position+velocity process-noise block.  Legacy alias:
            // velocity_q (identical units and value).
            if (pn.contains("acceleration_psd_q")) {
                process_noise_.acceleration_psd = pn.value("acceleration_psd_q", 1e2);
            } else if (pn.contains("velocity_q")) {
                process_noise_.acceleration_psd = pn.value("velocity_q", 1e2);
                log->warn("Deprecation: 'velocity_q' renamed to 'acceleration_psd_q' "
                          "(white-noise acceleration PSD); please update config.");
            } else {
                process_noise_.acceleration_psd = 1e2;
            }
            process_noise_.sat_bias            = pn.value("sat_bias_q", 1e-10);
            process_noise_.sat_code_bias       = pn.value("sat_code_bias_q", 1e-10);
            process_noise_.sat_phase_bias      = pn.value("sat_phase_bias_q", 1e-12);
            process_noise_.sat_clock           = pn.value("sat_clock_q", 1e-2);
            process_noise_.clock_rate          = pn.value("clock_rate_q", 1e-4);
            process_noise_.system_time_rate_offset =
                pn.value("system_time_rate_offset_q", 1e-10);
            // Ambiguity process noise.  Default 1e-12 is a numerical
            // safety floor — sub-mm accumulation per day, physically
            // indistinguishable from the constant-ambiguity model,
            // keeps per-(sat, signal) ambiguity covariance from
            // underflowing on long phase arcs (which on its own can
            // trigger non-positive-definite Cholesky downstream).
            // Set 0.0 in the JSON to enforce strict constant
            // ambiguity.
            process_noise_.ambiguity           = pn.value("ambiguity_q", 1e-12);
        }
        {
            const auto& is = jsection(json, "filter.initial_std", "initial_std");
            initial_std_.position            = is.value("position", 1e2);
            initial_std_.velocity            = is.value("velocity", 1e2);
            initial_std_.clock               = is.value("clock", 1e3);
            initial_std_.troposphere         = is.value("troposphere", 0.5);
            initial_std_.ionosphere          = is.value("ionosphere", 5.0);
            initial_std_.ionosphere_rate     = is.value("ionosphere_rate", 0.1);
            initial_std_.ambiguity           = is.value("ambiguity", 1e3);
            initial_std_.system_time_offset  = is.value("system_time_offset", 1e3);
            initial_std_.code_bias           = is.value("code_bias", 10.0);
            initial_std_.tropo_gradient      = is.value("tropo_gradient", 1e-3);
            initial_std_.phase_clock         = is.value("phase_clock", 1e3);
            initial_std_.sat_bias            = is.value("sat_bias", 10.0);
            initial_std_.sat_phase_bias      = is.value("sat_phase_bias", 10.0);
            initial_std_.sat_clock           = is.value("sat_clock", 1000.0);
            initial_std_.clock_rate          = is.value("clock_rate", 1.0);
            initial_std_.system_time_rate_offset =
                is.value("system_time_rate_offset", 1e-2);
        }
        {
            const auto& mn = jsection(json, "filter.measurement_noise", "default_measurement_noise");
            default_meas_noise_.code_std_m    = mn.value("code_std_m", 0.600);
            default_meas_noise_.phase_std_m   = mn.value("phase_std_m", 0.002);
            default_meas_noise_.doppler_std_m = mn.value("doppler_std_m", 0.5);
        }
        log->info("Default measurement noise: code_std={:.3f} m, phase_std={:.3f} m, doppler_std={:.3f} Hz",
            default_meas_noise_.code_std_m, default_meas_noise_.phase_std_m, default_meas_noise_.doppler_std_m);

        // -----------------------------------------------------------
        // Derive per-block code/phase σ tables from the unitless
        // ratio table × the measurement_noise baselines.  The user
        // tunes one σ per observable type (code, phase) via
        // filter.measurement_noise; the per-block proportions follow
        // automatically.  This keeps the relative hardware-quality
        // hierarchy (Block III at 1/3 of legacy Block II, etc.)
        // anchored to a single user knob and prevents drift between
        // the code and phase block tables that previously had to be
        // hand-edited in tandem.
        // -----------------------------------------------------------
        // -----------------------------------------------------------
        // JSON overrides for sat-block scale tables.  Optional —
        // each subsection independently merges into / replaces the
        // hard-coded defaults.  Parsed before the absolute-σ
        // derivation below so any user override is reflected.
        // -----------------------------------------------------------
        {
            const auto& sb = jsection(json, "filter.satellite_block",
                                       "satellite_block");
            if (sb.is_object()) {
                // block_scale_factors: ordered array of [substring, scale]
                // pairs (or [{block, scale}] objects).  Order matters
                // because lookup is first-substring-match-wins.
                if (sb.contains("block_scale_factors")
                    && sb["block_scale_factors"].is_array()) {
                    std::vector<std::pair<std::string, double>> override_factors;
                    override_factors.reserve(sb["block_scale_factors"].size());
                    for (const auto& entry : sb["block_scale_factors"]) {
                        std::string block;
                        double scale = std::numeric_limits<double>::quiet_NaN();
                        if (entry.is_array() && entry.size() == 2
                            && entry[0].is_string() && entry[1].is_number()) {
                            block = entry[0].get<std::string>();
                            scale = entry[1].get<double>();
                        } else if (entry.is_object()
                                   && entry.contains("block") && entry["block"].is_string()
                                   && entry.contains("scale") && entry["scale"].is_number()) {
                            block = entry["block"].get<std::string>();
                            scale = entry["scale"].get<double>();
                        }
                        if (!block.empty() && std::isfinite(scale) && scale > 0.0)
                            override_factors.emplace_back(std::move(block), scale);
                        else
                            log->warn("Invalid block_scale_factors entry; "
                                "expected [\"BLOCKIIF\", 0.5] or "
                                "{{\"block\":\"BLOCKIIF\",\"scale\":0.5}}");
                    }
                    if (!override_factors.empty()) {
                        sat_block_noise_params_.block_scale_factors =
                            std::move(override_factors);
                        log->info("Sat-block: {} block_scale_factors overrides loaded",
                                  sat_block_noise_params_.block_scale_factors.size());
                    }
                }
                // freq_band_scale + tracking_attr_scale: now split into
                // per-observable tables.  Accepted JSON forms (mixable
                // in the same block — keys are classified by position):
                //
                //   1. Nested per-observable (preferred):
                //        "code":  { "freq_band_scale": {...}, "tracking_attr_scale": {...} },
                //        "phase": { "freq_band_scale": {...}, "tracking_attr_scale": {...} }
                //      Each observable maintains its own scales, allowing
                //      e.g. GPS [code][C] = 1.8 (chip-rate C/A penalty)
                //      while GPS [phase][C] = 1.0 (no chip-rate effect
                //      on carrier).
                //
                //   2. Legacy top-level (deprecated): old configs may
                //      have set @c freq_band_scale or
                //      @c tracking_attr_scale at the top level of
                //      @c satellite_block.  Those are routed to the
                //      @c code side with a deprecation warning, because
                //      the original tables were calibrated for code
                //      noise.  Phase-side overrides require the
                //      nested form.
                //
                // A shared lambda merges one observable's JSON block
                // into an ObservableScale.

                using ObservableScale = SatBlockNoiseParams::ObservableScale;
                auto mergeObservable =
                    [&](const nlohmann::json& obs_json,
                        ObservableScale& target,
                        const std::string& label) -> void {
                    // freq_band_scale
                    if (obs_json.contains("freq_band_scale")
                        && obs_json["freq_band_scale"].is_object()) {
                        size_t merged = 0;
                        for (const auto& [k, v] : obs_json["freq_band_scale"].items()) {
                            if (k.size() == 1 && v.is_number()) {
                                target.freq_band_scale[k[0]] = v.get<double>();
                                ++merged;
                            } else {
                                log->warn("Sat-block [{}].freq_band_scale: invalid "
                                          "key/value '{}' (expected single-char band "
                                          "digit, numeric value)", label, k);
                            }
                        }
                        if (merged > 0)
                            log->info("Sat-block [{}]: {} freq_band_scale "
                                      "overrides merged", label, merged);
                    }
                    // tracking_attr_scale (per-system nested, with
                    // intra-block legacy-flat fallback).
                    if (obs_json.contains("tracking_attr_scale")
                        && obs_json["tracking_attr_scale"].is_object()) {
                        size_t merged_nested = 0;
                        size_t merged_legacy = 0;
                        for (const auto& [k, v] : obs_json["tracking_attr_scale"].items()) {
                            if (k.size() == 1 && v.is_number()) {
                                // Legacy flat key inside an observable block —
                                // route to the observable's "default" table.
                                target.tracking_attr_scale["default"][k[0]] =
                                    v.get<double>();
                                ++merged_legacy;
                            } else if (v.is_object()) {
                                auto& sys_tbl = target.tracking_attr_scale[k];
                                for (const auto& [attr, scale] : v.items()) {
                                    if (attr.size() == 1 && scale.is_number()) {
                                        sys_tbl[attr[0]] = scale.get<double>();
                                        ++merged_nested;
                                    } else {
                                        log->warn("Sat-block [{}].tracking_attr_scale "
                                                  "under '{}': invalid entry '{}' "
                                                  "(expected single-char attr key, "
                                                  "numeric value)", label, k, attr);
                                    }
                                }
                            } else {
                                log->warn("Sat-block [{}].tracking_attr_scale: invalid "
                                          "key/value '{}' (expected either single-char "
                                          "attr with number, or system name with object)",
                                          label, k);
                            }
                        }
                        if (merged_legacy > 0)
                            log->warn("Sat-block [{}]: {} tracking_attr_scale entries "
                                      "from flat form merged into 'default' table — "
                                      "use nested {{\"GPS\": {{\"C\": 1.8}}}} for "
                                      "per-system overrides", label, merged_legacy);
                        if (merged_nested > 0)
                            log->info("Sat-block [{}]: {} tracking_attr_scale "
                                      "overrides merged (nested)", label, merged_nested);
                    }
                };

                // ---- Preferred form: nested per-observable blocks ----
                if (sb.contains("code") && sb["code"].is_object())
                    mergeObservable(sb["code"], sat_block_noise_params_.code, "code");
                if (sb.contains("phase") && sb["phase"].is_object())
                    mergeObservable(sb["phase"], sat_block_noise_params_.phase, "phase");

                // ---- Legacy top-level: route to code side ----
                // Older configs set freq_band_scale / tracking_attr_scale
                // directly under satellite_block.  Those were calibrated
                // for code; route them there and warn that phase-side
                // overrides now require the nested form.
                const bool has_legacy_band =
                    sb.contains("freq_band_scale")
                    && sb["freq_band_scale"].is_object();
                const bool has_legacy_attr =
                    sb.contains("tracking_attr_scale")
                    && sb["tracking_attr_scale"].is_object();
                if (has_legacy_band || has_legacy_attr) {
                    // Synthesise an observable-shaped sub-object so the
                    // shared merge helper can process it uniformly.
                    nlohmann::json legacy_code;
                    if (has_legacy_band)
                        legacy_code["freq_band_scale"] = sb["freq_band_scale"];
                    if (has_legacy_attr)
                        legacy_code["tracking_attr_scale"] = sb["tracking_attr_scale"];
                    mergeObservable(legacy_code, sat_block_noise_params_.code,
                                     "code(legacy)");
                    log->warn("Sat-block: top-level freq_band_scale / "
                              "tracking_attr_scale found — routed to 'code' "
                              "(legacy form).  To tune phase, use nested "
                              "'phase': {{ 'freq_band_scale': {{...}}, "
                              "'tracking_attr_scale': {{...}} }}");
                }
                // apply_to_phase: route phase observations through the
                // full block × freq_band × tracking_attr × elevation
                // pipeline (true, default) or the simpler
                // default_phase_std_m × elevation path (false).  See
                // SatBlockNoiseParams::apply_to_phase doxygen for the
                // physics-vs-empirical trade-off.
                if (sb.contains("apply_to_phase")
                    && sb["apply_to_phase"].is_boolean()) {
                    sat_block_noise_params_.apply_to_phase =
                        sb["apply_to_phase"].get<bool>();
                    log->info("Sat-block: apply_to_phase set to {}",
                              sat_block_noise_params_.apply_to_phase);
                }
            }
        }

        {
            auto& bp = sat_block_noise_params_;
            bp.block_code_std_m.clear();
            bp.block_phase_std_m.clear();
            bp.block_code_std_m.reserve(bp.block_scale_factors.size());
            bp.block_phase_std_m.reserve(bp.block_scale_factors.size());
            for (const auto& [block, scale] : bp.block_scale_factors) {
                bp.block_code_std_m.emplace_back(block,
                    scale * default_meas_noise_.code_std_m);
                bp.block_phase_std_m.emplace_back(block,
                    scale * default_meas_noise_.phase_std_m);
            }
            // Fallback when no block match: scale factor 1.0 →
            // baseline σ unchanged (conservative — matches the
            // legacy / worst-case zenith noise).
            bp.default_code_std_m  = default_meas_noise_.code_std_m;
            bp.default_phase_std_m = default_meas_noise_.phase_std_m;

            // Compute the actual min/max scale from the (possibly
            // overridden) factor table for a meaningful range log.
            double scale_min = 1.0, scale_max = 1.0;
            if (!bp.block_scale_factors.empty()) {
                scale_min = bp.block_scale_factors.front().second;
                scale_max = scale_min;
                for (const auto& [_, s] : bp.block_scale_factors) {
                    if (s < scale_min) scale_min = s;
                    if (s > scale_max) scale_max = s;
                }
            }
            log->info("Sat-block noise: derived from measurement_noise "
                "(code_zen ∈ [{:.3f}, {:.3f}] m, phase_zen ∈ [{:.4f}, {:.4f}] m) "
                "× per-block scale factors",
                default_meas_noise_.code_std_m  * scale_min,
                default_meas_noise_.code_std_m  * scale_max,
                default_meas_noise_.phase_std_m * scale_min,
                default_meas_noise_.phase_std_m * scale_max);
        }

        // Stochastic model.  Drives both code and phase weighting,
        // but each model handles phase per its own per-model rule
        // (see GnssConfig::getStochasticModel() docstring).  In
        // particular, SatelliteBlock uses the block + freq + elevation
        // pipeline on code while phase reduces to just the configured
        // phase_std_m amplified by elevation.
        {
            std::string raw = jval<std::string>(json, "filter.stochastic_model", "stochastic_model", "elevation");
            const std::string low = toLower(raw);
            if      (low == "constant")        stochastic_model_ = StochasticModelType::Constant;
            else if (low == "elevation")       stochastic_model_ = StochasticModelType::Elevation;
            else if (low == "snr")             stochastic_model_ = StochasticModelType::SNR;
            else if (low == "satellite_block") stochastic_model_ = StochasticModelType::SatelliteBlock;
            else {
                log->warn("Invalid stochastic_model '{}', defaulting to Elevation", raw);
                stochastic_model_ = StochasticModelType::Elevation;
            }
            log->info("Stochastic model: {}", raw);
        }
        // Elevation-amplification σ-power, separately configurable for
        // code and phase.  Variance scales as 1/sin^(2·power)(elev),
        // i.e. σ = σ_zen / sin^power(elev).  Defaults: code=1.0
        // (legacy csc²(θ) on variance, fits code multipath and tropo
        // path-length physics), phase=0.5 (csc(θ) on variance, fits
        // phase carrier-loop noise + PCV miscalibration physics and
        // keeps low-elevation residual checks usable).
        code_elevation_power_  = jval<double>(json,
            "filter.code_elevation_power",  "code_elevation_power",  1.0);
        phase_elevation_power_ = jval<double>(json,
            "filter.phase_elevation_power", "phase_elevation_power", 1.0);
        log->info("Elevation power: code={:.2f}, phase={:.2f}  "
            "(var_actual = var_zen / sin^(2·power)(elev))",
            code_elevation_power_, phase_elevation_power_);
        {
            // SNR model defaults are tuned for the variance-additive
            // form  σ² = a² + b²·10^(-CN0/10).  These differ from any
            // values left over from the σ-additive era; configs from
            // before the formula switch should be re-tuned (the old
            // code_b ≈ 1.0 etc. produces a near-flat σ vs CN0 under
            // the new form, defeating the SNR axis).
            const auto& sp = jsection(json, "filter.snr_model_params", "snr_model_params");
            snr_model_params_.code_a         = sp.value("code_a", 0.30);
            snr_model_params_.code_b         = sp.value("code_b", 50.0);
            snr_model_params_.phase_a        = sp.value("phase_a", 0.001);
            snr_model_params_.phase_b        = sp.value("phase_b", 0.5);
            snr_model_params_.snr_floor_dbhz = sp.value("snr_floor_dbhz", 20.0);
            snr_model_params_.snr_ceil_dbhz  = sp.value("snr_ceil_dbhz", 55.0);
            log->info("SNR model params (variance-additive): code_a={:.3f}, code_b={:.2f}, "
                "phase_a={:.4f}, phase_b={:.2f}, floor={:.0f} dBHz, ceil={:.0f} dBHz",
                snr_model_params_.code_a, snr_model_params_.code_b,
                snr_model_params_.phase_a, snr_model_params_.phase_b,
                snr_model_params_.snr_floor_dbhz, snr_model_params_.snr_ceil_dbhz);
        }
    }

    // =================================================================
    //  Section: model_corrections  (unchanged leaf keys)
    // =================================================================
    void GnssConfig::loadModelCorrectionsSection_(const nlohmann::json& json, LogManager& log_manager) {
        auto log = log_manager.getLogger("gnss_config");
        {
            const auto& mc = jsection(json, "model_corrections", "model_corrections");
            model_corrections_.enable_troposphere               = mc.value("enable_troposphere", true);
            model_corrections_.enable_relativistic_correction   = mc.value("enable_relativistic_correction", true);
            model_corrections_.enable_shapiro_delay             = mc.value("enable_shapiro_delay", true);
            model_corrections_.enable_solid_earth_tide          = mc.value("enable_solid_earth_tide", true);
            model_corrections_.enable_ocean_tide_loading        = mc.value("enable_ocean_tide_loading", true);
            model_corrections_.enable_pole_tide                 = mc.value("enable_pole_tide", true);
            model_corrections_.enable_ocean_pole_tide           = mc.value("enable_ocean_pole_tide", false);
            model_corrections_.enable_atmospheric_loading       = mc.value("enable_atmospheric_loading", false);
            model_corrections_.enable_satellite_pco             = mc.value("enable_satellite_pco", true);
            model_corrections_.enable_satellite_pcv             = mc.value("enable_satellite_pcv", true);
            model_corrections_.enable_receiver_pco              = mc.value("enable_receiver_pco", true);
            model_corrections_.enable_receiver_pcv              = mc.value("enable_receiver_pcv", true);
            model_corrections_.enable_phase_windup              = mc.value("enable_phase_windup", true);
            model_corrections_.enable_code_phase_bias           = mc.value("enable_code_phase_bias", true);
            model_corrections_.bias_coverage_requires_apply     = mc.value("bias_coverage_requires_apply", false);
            model_corrections_.enable_atx_frequency_fallback    = mc.value("enable_atx_frequency_fallback", true);
            model_corrections_.require_atx_coverage             = mc.value("require_atx_coverage",          false);
            model_corrections_.require_atx_coverage_for_ar      = mc.value("require_atx_coverage_for_ar",  false);
            model_corrections_.restrict_ar_for_substituted_atx  = mc.value("restrict_ar_for_substituted_atx", false);
            model_corrections_.loosen_amb_q_for_substituted_atx = mc.value("loosen_amb_q_for_substituted_atx", false);
            model_corrections_.ambiguity_q_substituted_atx      = mc.value("ambiguity_q_substituted_atx",  1e-12);
            model_corrections_.ambiguity_std_substituted_atx    = mc.value("ambiguity_std_substituted_atx", 1e3);
            model_corrections_.atx_substitution_phase_std_m     = mc.value("atx_substitution_phase_std_m",  0.0);
            model_corrections_.apply_phase_shifts               = mc.value("apply_phase_shifts",            false);
            model_corrections_.receiver_antenna_type_override   = mc.value("receiver_antenna_type_override", std::string{});
            model_corrections_.enable_receiver_cross_system_atx_fallback =
                mc.value("enable_receiver_cross_system_atx_fallback", false);
            model_corrections_.enable_klobuchar_ionosphere      = mc.value("enable_klobuchar_ionosphere", false);
            model_corrections_.enable_higher_order_ionosphere   = mc.value("enable_higher_order_ionosphere", false);
            model_corrections_.refine_emission_time             = mc.value("refine_emission_time",         true);
        }
        log->info("Model corrections: tropo={}, rel={}, shapiro={}, set={}, otl={}, pole={}, opt={}, atm={}, "
            "sat_pco={}, sat_pcv={}, rx_pco={}, rx_pcv={}, windup={}, cpb={}, atx_fallback={}",
            model_corrections_.enable_troposphere              ? "on" : "off",
            model_corrections_.enable_relativistic_correction ? "on" : "off",
            model_corrections_.enable_shapiro_delay           ? "on" : "off",
            model_corrections_.enable_solid_earth_tide        ? "on" : "off",
            model_corrections_.enable_ocean_tide_loading      ? "on" : "off",
            model_corrections_.enable_pole_tide               ? "on" : "off",
            model_corrections_.enable_ocean_pole_tide         ? "on" : "off",
            model_corrections_.enable_atmospheric_loading     ? "on" : "off",
            model_corrections_.enable_satellite_pco           ? "on" : "off",
            model_corrections_.enable_satellite_pcv           ? "on" : "off",
            model_corrections_.enable_receiver_pco            ? "on" : "off",
            model_corrections_.enable_receiver_pcv            ? "on" : "off",
            model_corrections_.enable_phase_windup            ? "on" : "off",
            model_corrections_.enable_code_phase_bias        ? "on" : "off",
            model_corrections_.enable_atx_frequency_fallback  ? "on" : "off");
        log->info("Emission-time refinement: {} (second-pass slant-ZHD + IF-ATX subtraction in geometry pipeline)",
            model_corrections_.refine_emission_time ? "ENABLED" : "disabled");
        // SPEC_phase_windup_sign.md §7: with the correction modeled but NOT
        // accumulated, the raw atan2 angle is applied per epoch, so every
        // half-cycle crossing steps the modeled phase by EXACTLY one cycle
        // mid-pass — a band-common, equal-in-cycles staircase that is invisible
        // to MW (ΔWL = 0) and lands as a phantom slip/level event (measured 43
        // one-cycle jumps/day/sat ⇒ ~753/day at 63 stations; a standing
        // invariant #1/#7 instance).  Every field lineage inherits this silently
        // because the estimation key parses absent ⇒ false.  Behavior-neutral
        // WARN only: flipping the loader default is a migration card
        // (fingerprint-invalidating), not a quick edit.
        if (model_corrections_.enable_phase_windup && !accumulate_phase_windup_) {
            log->warn("Phase wind-up runs in FRACTIONAL (wrapping) mode — "
                      "estimation.accumulate_phase_windup is absent/false.  Each "
                      "half-cycle crossing steps the correction by exactly one "
                      "cycle mid-pass (MW-blind staircase; see "
                      "docs/SPEC_phase_windup_sign.md §7).  Set "
                      "estimation.accumulate_phase_windup: true on any "
                      "fresh-convergence lineage.");
        }
        if (model_corrections_.require_atx_coverage) {
            log->info("ATX coverage: STRICT — observations without complete sat+rx PCO+PCV "
                "for the exact signal frequency will be dropped.");
        }
        if (model_corrections_.bias_coverage_requires_apply) {
            log->info("BIA coverage: HONEST — .BIA entries count as code-bias coverage only "
                "while enable_code_phase_bias is applying them (absorber states + no-BIA "
                "downweight re-engage otherwise; network producer mode exempt).");
        }
        if (model_corrections_.require_atx_coverage_for_ar) {
            log->info("AR ATX gate (strict): WL constraints skipped for (sat, signal) pairs "
                "lacking strict ATX coverage; observations stay in float KF.");
        }
        if (model_corrections_.restrict_ar_for_substituted_atx) {
            log->info("AR ATX gate (substituted): WL constraints skipped for (sat, signal) "
                "pairs whose ATX correction was frequency-fallback-substituted.");
        }
        log->info("ATX-substituted ambiguity overrides: {}{}",
            model_corrections_.loosen_amb_q_for_substituted_atx ? "ENABLED" : "disabled",
            model_corrections_.loosen_amb_q_for_substituted_atx
              ? fmt::format(" — q={:.3e} m²/s, init_std={:.3f} m",
                            model_corrections_.ambiguity_q_substituted_atx,
                            model_corrections_.ambiguity_std_substituted_atx)
              : std::string{" (all ambiguities use process_noise.ambiguity and getAmbiguityStd default)"});
        if (model_corrections_.atx_substitution_phase_std_m > 0.0) {
            log->info("ATX-substituted phase σ inflation: ENABLED — +({:.1f} mm / sin el)² added "
                "to substituted-signal phase variance (down-weights common-mode PCV residual that "
                "aliases into ZWD/height; complements the ambiguity-Q override above)",
                model_corrections_.atx_substitution_phase_std_m * 1e3);
        }
        if (model_corrections_.apply_phase_shifts) {
            log->info("SYS / PHASE SHIFT subtraction: ENABLED — recorded phase shifts "
                "will be subtracted from RINEX phase observations at parse time.");
        }
        if (model_corrections_.enable_higher_order_ionosphere) {
            log->info("Higher-order ionosphere (I₂+I₃): ENABLED — GIM-derived STEC + "
                "dipole B-field, applied per-signal at observation level.  No-op "
                "when no IONEX GIM resolves.");
        }
    }

    // =================================================================
    //  Section: receiver
    // =================================================================
    void GnssConfig::loadReceiverSection_(const nlohmann::json& json, LogManager& log_manager) {
        auto log = log_manager.getLogger("gnss_config");
        {
            const auto& ao = jsection(json, "receiver.antenna_orientation", "receiver_antenna_orientation");
            receiver_antenna_orientation_.azimuth_deg   = ao.value("azimuth_deg", 0.0);
            receiver_antenna_orientation_.elevation_deg = ao.value("elevation_deg", 90.0);
        }
        enable_rx_antenna_orientation_  = jval<bool>(json, "receiver.enable_antenna_orientation",  "enable_antenna_orientation",  false);
        enable_rx_antenna_substitution_ = jval<bool>(json, "receiver.enable_antenna_substitution", "enable_antenna_substitution", false);
        log->info("Receiver antenna: orientation={} (az={:.1f} deg), substitution={}",
                  enable_rx_antenna_orientation_  ? "on" : "off",
                  receiver_antenna_orientation_.azimuth_deg,
                  enable_rx_antenna_substitution_ ? "on" : "off");
        zhd_pressure_mbar = jval<double>(json, "receiver.zhd_pressure_mbar", "zhd_pressure_mbar", 1013.25);
        zhd_temp_kelvin   = jval<double>(json, "receiver.zhd_temp_kelvin", "zhd_temp_kelvin", 288.15);
        log->info("ZHD defaults: P={:.1f} mbar, T={:.1f} K", zhd_pressure_mbar, zhd_temp_kelvin);

        // Troposphere a priori source: "standard_atmosphere" (default) or "gpt3".
        // Troposphere mapping function: "gmf" (default) or "vmf3".
        {
            auto to_lower = [](std::string s) {
                std::transform(s.begin(), s.end(), s.begin(),
                               [](unsigned char c){ return static_cast<char>(std::tolower(c)); });
                return s;
            };

            const std::string src = to_lower(
                jval<std::string>(json, "receiver.troposphere_apriori_source",
                                  "troposphere_apriori_source", std::string{"standard_atmosphere"}));
            if (src == "gpt3") {
                tropo_apriori_source_ = TroposphereAprioriSource::Gpt3;
            } else {
                if (src != "standard_atmosphere") {
                    log->warn("Unknown receiver.troposphere_apriori_source='{}', falling back to standard_atmosphere", src);
                }
                tropo_apriori_source_ = TroposphereAprioriSource::StandardAtmosphere;
            }

            const std::string mf = to_lower(
                jval<std::string>(json, "receiver.troposphere_mapping_function",
                                  "troposphere_mapping_function", std::string{"gmf"}));
            if (mf == "vmf3") {
                tropo_mapping_function_ = TroposphereMappingFunction::Vmf3;
            } else {
                if (mf != "gmf") {
                    log->warn("Unknown receiver.troposphere_mapping_function='{}', falling back to gmf", mf);
                }
                tropo_mapping_function_ = TroposphereMappingFunction::Gmf;
            }

            const bool need_gpt3 =
                tropo_apriori_source_   == TroposphereAprioriSource::Gpt3 ||
                tropo_mapping_function_ == TroposphereMappingFunction::Vmf3;

            // Explicit receiver.gpt3_grid_path wins.  Otherwise leave the path
            // EMPTY here and let the consumer (Troposphere) default to
            // <internal_data_root>/static/gpt3/gpt3_5.grd at construction time.
            // The internal data root is not resolved until loadFilesSection_,
            // which runs *after* this section, so baking the default here would
            // build it from an empty root -> the bogus relative path
            // "static/gpt3/gpt3_5.grd", which fails to open and silently drops
            // VMF3/GPT3 back to GMF.  Deferring to the consumer mirrors how the
            // ocean-pole-tide (opt_grid_path) and atmospheric-loading
            // (atm_tide_grid_path) static grids default in ModelGeometry, where
            // the internal data root is always known.
            const std::string gpt3_str = jval<std::string>(json, "receiver.gpt3_grid_path",
                                                            "gpt3_grid_path", std::string{});
            if (!gpt3_str.empty()) {
                gpt3_grid_path_ = gpt3_str;
            }

            const std::string gpt3_display =
                !gpt3_grid_path_.empty() ? gpt3_grid_path_.string()
                : need_gpt3              ? std::string("<default: static/gpt3/gpt3_5.grd under internal data root>")
                                        : std::string("<unset>");
            log->info("Troposphere a priori: source={}, mapping={}, gpt3_grid_path={}",
                      tropo_apriori_source_ == TroposphereAprioriSource::Gpt3 ? "gpt3" : "standard_atmosphere",
                      tropo_mapping_function_ == TroposphereMappingFunction::Vmf3 ? "vmf3" : "gmf",
                      gpt3_display);
        }

        // Reference position (optional)
        {
            const auto& rp = jsection(json, "receiver.reference_position_ecef", "reference_position_ecef");
            if (rp.is_object() && rp.contains("x") && rp.contains("y") && rp.contains("z")) {
                reference_position_ecef_ = { rp.value("x", 0.0), rp.value("y", 0.0), rp.value("z", 0.0) };
                has_reference_position_ = true;
                log->info("Reference position ECEF: ({:.4f}, {:.4f}, {:.4f}) m",
                    reference_position_ecef_[0], reference_position_ecef_[1], reference_position_ecef_[2]);
            }
        }

        // ITRF station-velocity propagation of the reference position (optional).
        // Propagates the fixed a-priori to the observation epoch via linear plate
        // motion r(t)=r0+v·(t−t0); see GnssConfig::getReferencePositionEcefAtEpoch.
        {
            enable_itrf_velocity_ = jval<bool>(json, "receiver.enable_itrf_velocity",
                                               "enable_itrf_velocity", false);
            reference_position_epoch_year_ = jval<double>(json, "receiver.reference_position_epoch_year",
                                                          "reference_position_epoch_year", 0.0);
            const auto& rv = jsection(json, "receiver.reference_velocity_ecef", "reference_velocity_ecef");
            if (rv.is_object() && rv.contains("x") && rv.contains("y") && rv.contains("z")) {
                reference_velocity_ecef_ = { rv.value("x", 0.0), rv.value("y", 0.0), rv.value("z", 0.0) };
                has_reference_velocity_ = true;
            }
            if (enable_itrf_velocity_) {
                if (has_reference_position_ && has_reference_velocity_ && reference_position_epoch_year_ != 0.0) {
                    log->info("ITRF velocity propagation: ON, v=({:.5f}, {:.5f}, {:.5f}) m/yr, ref epoch={:.3f}",
                        reference_velocity_ecef_[0], reference_velocity_ecef_[1], reference_velocity_ecef_[2],
                        reference_position_epoch_year_);
                }
                else {
                    log->warn("enable_itrf_velocity=true but propagation is INACTIVE — needs "
                              "reference_position_ecef + reference_velocity_ecef + reference_position_epoch_year; "
                              "a-priori will NOT be propagated");
                }
            }
        }
    }

    // =================================================================
    //  Section: files
    //
    //  Parses the top-level @c data_source selector and the @c files
    //  block.  Path validation and discovery is deferred to
    //  @c PathResolver, which is invoked by the application driver
    //  after the config has finished loading.  This keeps the loader
    //  responsibility narrow: stash whatever the user wrote, plus
    //  defaults for anything they omitted.
    // =================================================================
    void GnssConfig::loadFilesSection_(const nlohmann::json& json, LogManager& log_manager) {
        auto log = log_manager.getLogger("gnss_config");

        // --- Top-level data_source selector ---------------------------
        // Accepts "internal" / "production" (any case).  Defaults to
        // Internal so a fresh checkout with no extra config still runs
        // against the data/ tree shipped with the repo.
        {
            std::string raw = jval<std::string>(json, "data_source", "data_source", "internal");
            std::string low = toLower(raw);
            if      (low == "internal")   data_source_ = DataSource::Internal;
            else if (low == "production") data_source_ = DataSource::Production;
            else {
                log->warn("Unknown data_source '{}', defaulting to internal", raw);
                data_source_ = DataSource::Internal;
            }
            log->info("Data source: {}",
                data_source_ == DataSource::Internal ? "internal (data/)" : "production (download)");
        }

        // --- Internal data root --------------------------------------
        // Relative paths are resolved against the project root baked
        // in at CMake configure time.  An empty / missing field falls
        // back to <project_root>/data, which is the canonical fixture
        // location for unit tests and demos.
        {
            std::string raw = jval<std::string>(json,
                "files.internal_data_root", "internal_data_root", "");
            const std::filesystem::path project_root = GNSS_PROJECT_ROOT;
            if (raw.empty()) {
                internal_data_root_ = project_root / "data";
            } else {
                std::filesystem::path p(raw);
                if (p.is_relative()) {
                    // "./data" and friends resolve under the project
                    // root so the same JSON works regardless of CWD.
                    internal_data_root_ = std::filesystem::weakly_canonical(project_root / p);
                } else {
                    internal_data_root_ = std::filesystem::weakly_canonical(p);
                }
            }
            if (data_source_ == DataSource::Internal) {
                if (std::filesystem::exists(internal_data_root_)) {
                    log->info("Internal data root: {}", internal_data_root_.string());
                } else {
                    log->warn("Internal data root does not exist: {} "
                              "(PathResolver will fail if no files are found)",
                              internal_data_root_.string());
                }
            } else {
                log->debug("Internal data root: {} (unused in production mode)",
                    internal_data_root_.string());
            }
        }

        // --- Direct file overrides (optional) -------------------------
        // The user may still pin specific paths in the config.  When
        // set, these win over PathResolver auto-discovery.  Empty
        // values let PathResolver fill them in.  No hard-coded fallback
        // strings — PathResolver is the single source of truth for
        // "what to do when the user didn't specify".
        observation_path_ = jval<std::string>(json, "files.observation_path", "observation_path", "");
        navigation_path_  = jval<std::string>(json, "files.navigation_path",  "navigation_path",  "");

        // --- Legacy product_list_path (optional override) -------------
        // When non-empty, the file's contents seed @c product_paths_
        // directly and PathResolver leaves the list alone.  This is the
        // escape hatch for runs that need a hand-curated product set
        // (e.g. comparing analysis centres on the same epoch).  Empty
        // means "let PathResolver do it" — the common case.
        product_list_path_ = jval<std::string>(json, "files.product_list_path", "product_list_path", "");
        if (!product_list_path_.empty()) {
            if (std::filesystem::exists(product_list_path_)) {
                std::ifstream product_file(product_list_path_);
                std::string line;
                while (std::getline(product_file, line)) {
                    line.erase(line.find_last_not_of(" \t\r\n") + 1);
                    if (!line.empty()) product_paths_.push_back(line);
                }
                log->info("Loaded {} product paths from list file: {}",
                    product_paths_.size(), product_list_path_.string());
            } else {
                log->warn("product_list_path set but file does not exist: {} "
                          "(PathResolver will populate the list instead)",
                          product_list_path_.string());
            }
        }

        // Ocean tidal loading BLQ
        {
            std::string otl_path_str = jval<std::string>(json, "files.otl_blq_path", "otl_blq_path", "");
            if (!otl_path_str.empty()) {
                otl_blq_path_ = otl_path_str;
                if (std::filesystem::exists(otl_blq_path_))
                    log->info("OTL BLQ file: {}", otl_blq_path_.string());
                else {
                    log->warn("OTL BLQ file not found: {}", otl_blq_path_.string());
                    otl_blq_path_.clear();
                }
            }
        }

        // Ocean pole tide load grid (IERS opoleloadcoefcmcor.txt).  Optional
        // explicit override; when empty ModelGeometry falls back to
        // <internal_data_root>/static/opole/opoleloadcoefcmcor.txt.  Only used
        // when model_corrections.enable_ocean_pole_tide is on.
        {
            std::string opt_path_str = jval<std::string>(json, "files.opt_grid_path", "opt_grid_path", "");
            if (!opt_path_str.empty()) {
                opt_grid_path_ = opt_path_str;
                if (std::filesystem::exists(opt_grid_path_))
                    log->info("Ocean pole tide grid: {}", opt_grid_path_.string());
                else {
                    log->warn("Ocean pole tide grid not found: {}", opt_grid_path_.string());
                    opt_grid_path_.clear();
                }
            }
        }

        // Atmospheric S1/S2 tide load grid.  Optional explicit override; when
        // empty ModelGeometry falls back to <internal_data_root>/static/apl/.
        // Only used when model_corrections.enable_atmospheric_loading is on.
        {
            std::string apl_path_str = jval<std::string>(json, "files.atm_tide_grid_path", "atm_tide_grid_path", "");
            if (!apl_path_str.empty()) {
                atm_tide_grid_path_ = apl_path_str;
                if (std::filesystem::exists(atm_tide_grid_path_))
                    log->info("Atmospheric S1/S2 tide grid: {}", atm_tide_grid_path_.string());
                else {
                    log->warn("Atmospheric S1/S2 tide grid not found: {}", atm_tide_grid_path_.string());
                    atm_tide_grid_path_.clear();
                }
            }
        }

        // Station Information Files (SIF).  A bundle of per-station metadata
        // (ITRF coordinates/velocity/covariance, FES2014b ocean tide loading,
        // Desai ocean pole tide, antenna/receiver history, nonlinear motion).
        // The master toggle lives in the processing section (loop-level data
        // source); paths in the files section.  Default off keeps runs
        // byte-identical to the pre-SIF behaviour.
        {
            enable_station_info_ = jval<bool>(json, "processing.enable_station_info",
                                               "enable_station_info", false);
            station_info_paths_.clear();

            // Explicit paths win: files.station_info_paths (array) and/or
            // files.station_info_path (single string).
            const nlohmann::json& files = jsection(json, "files");
            if (auto it = files.find("station_info_paths");
                it != files.end() && it->is_array()) {
                for (const auto& p : *it)
                    if (p.is_string()) station_info_paths_.emplace_back(p.get<std::string>());
            }
            const std::string single =
                jval<std::string>(json, "files.station_info_path", "station_info_path", "");
            if (!single.empty()) station_info_paths_.emplace_back(single);

            if (enable_station_info_) {
                // Auto-discovery: glob <internal_data_root>/static/sif/*.json.
                // SIFs are date-independent static assets (like the ATX and
                // opole grid), so they don't go through PathResolver's
                // date-aware product logic.
                if (station_info_paths_.empty()) {
                    const std::filesystem::path sif_dir =
                        internal_data_root_ / "static" / "sif";
                    std::error_code ec;
                    if (std::filesystem::is_directory(sif_dir, ec)) {
                        for (const auto& e : std::filesystem::directory_iterator(sif_dir, ec)) {
                            if (e.is_regular_file() && e.path().extension() == ".json")
                                station_info_paths_.push_back(e.path());
                        }
                        std::sort(station_info_paths_.begin(), station_info_paths_.end());
                    }
                }

                auto registry = std::make_shared<StationInfoFile>(log_manager);
                std::size_t total = 0;
                for (const auto& p : station_info_paths_) {
                    if (std::filesystem::exists(p)) total += registry->load(p);
                    else log->warn("SIF path not found: {}", p.string());
                }
                if (total > 0) {
                    station_info_ = std::move(registry);
                    log->info("Station info: loaded {} stations from {} SIF file(s)",
                              total, station_info_paths_.size());
                } else {
                    log->warn("enable_station_info=true but no SIF stations were loaded "
                              "(checked {} path(s) under {})",
                              station_info_paths_.size(),
                              (internal_data_root_ / "static" / "sif").string());
                }
            }
        }
    }

    // =================================================================
    //  Section: imu
    // =================================================================
    void GnssConfig::loadImuSection_(const nlohmann::json& json, LogManager& log_manager) {
        auto log = log_manager.getLogger("gnss_config");
        {
            const auto& ic = jsection(json, "imu", "imu_config");
            imu_config_.enabled = jval<bool>(json, "imu.enabled", "enable_imu", false);
            std::string imu_path_str = ic.value("imu_file_path", "");
            if (!imu_path_str.empty()) {
                imu_config_.imu_file_path = imu_path_str;
                if (!std::filesystem::exists(imu_config_.imu_file_path))
                    log->warn("IMU file not found: {}", imu_config_.imu_file_path.string());
            }
            {
                std::string raw = ic.value("coupling_mode",
                                    ic.value("imu_coupling_mode", "loosely_coupled"));
                std::string low = toLower(raw);
                if (low == "tightly_coupled" || low == "tight")
                    imu_config_.coupling_mode = ImuCouplingMode::TightCoupled;
                else
                    imu_config_.coupling_mode = ImuCouplingMode::LooseCoupled;
            }
            imu_config_.accel_noise_density    = ic.value("accel_noise_density", 0.05);
            imu_config_.gyro_noise_density     = ic.value("gyro_noise_density", 5e-4);
            imu_config_.accel_bias_instability = ic.value("accel_bias_instability", 1e-4);
            imu_config_.gyro_bias_instability  = ic.value("gyro_bias_instability", 1e-6);
            imu_config_.initial_accel_bias_std = ic.value("initial_accel_bias_std", 0.1);
            imu_config_.initial_gyro_bias_std  = ic.value("initial_gyro_bias_std", 1e-3);
            imu_config_.initial_attitude_std   = ic.value("initial_attitude_std", 0.1);
        }
        if (imu_config_.enabled) {
            const char* coupling_str = (imu_config_.coupling_mode == ImuCouplingMode::TightCoupled)
                ? "tightly_coupled" : "loosely_coupled";
            log->info("IMU integration enabled: coupling={}, file={}, accel_noise={:.4f}, gyro_noise={:.6f}, "
                "accel_bias_inst={:.6f}, gyro_bias_inst={:.8f}",
                coupling_str,
                imu_config_.imu_file_path.string(),
                imu_config_.accel_noise_density, imu_config_.gyro_noise_density,
                imu_config_.accel_bias_instability, imu_config_.gyro_bias_instability);

            if (imu_config_.coupling_mode == ImuCouplingMode::TightCoupled && !estimate_velocity_) {
                estimate_velocity_ = true;
                log->info("Tightly-coupled IMU: forcing estimate_velocity=true");
            }
        }

        // --- estimation.aiding.{imu,zupt,nhc,wheel,baro,vision} ---------------
        // Aiding-source selection (AidingSourceRegistry / AidingSourceManager,
        // the navigation-aiding analog of estimation.cycle_slip).  Loaded HERE,
        // at the tail of the IMU section, rather than in loadEstimationSection_,
        // because `imu` back-compat-defaults to the already-parsed imu.enabled
        // (enableImu()) and the estimation section runs first.  An explicit
        // estimation.aiding.imu always wins.  The AidingSourceManager is live
        // (predictAided / collectPredict in Application::run and
        // RealTimeEngine::run); see docs/aiding_source_abstraction_design.md.
        {
            aiding_.imu    = jval<bool>(json, "estimation.aiding.imu",    "aiding_imu",    imu_config_.enabled);
            aiding_.zupt   = jval<bool>(json, "estimation.aiding.zupt",   "aiding_zupt",   false);
            aiding_.nhc    = jval<bool>(json, "estimation.aiding.nhc",    "aiding_nhc",    false);
            aiding_.wheel  = jval<bool>(json, "estimation.aiding.wheel",  "aiding_wheel",  false);
            aiding_.baro   = jval<bool>(json, "estimation.aiding.baro",   "aiding_baro",   false);
            aiding_.vision = jval<bool>(json, "estimation.aiding.vision", "aiding_vision", false);
            aiding_.zupt_sigma_mps = jval<double>(json, "estimation.aiding.zupt_sigma_mps",
                                                  "aiding_zupt_sigma_mps", aiding_.zupt_sigma_mps);
            log->info("Aiding sources: imu={}, zupt={}, nhc={}, wheel={}, baro={}, vision={} (zupt_sigma={:.3f} m/s)",
                      aiding_.imu ? "on" : "off", aiding_.zupt ? "on" : "off",
                      aiding_.nhc ? "on" : "off", aiding_.wheel ? "on" : "off",
                      aiding_.baro ? "on" : "off", aiding_.vision ? "on" : "off",
                      aiding_.zupt_sigma_mps);
        }
    }

    // =================================================================
    //  Section: data_download
    // =================================================================
    void GnssConfig::loadDataDownloadSection_(const nlohmann::json& json, LogManager& log_manager) {
        auto log = log_manager.getLogger("gnss_config");
        if (json.contains("data_download")) {
            const auto& dl = json["data_download"];
            download_config_.enabled           = dl.value("enabled", false);
            download_config_.data_root         = dl.value("data_root", "");
            download_config_.product_source    = dl.value("product_source", "COD");
            download_config_.product_type      = dl.value("product_type", "FIN");
            download_config_.netrc_path        = dl.value("netrc_path", "");
            download_config_.station           = dl.value("station", "");
            download_config_.station_country   = dl.value("station_country", "USA");
            download_config_.year              = dl.value("year", 0);
            download_config_.doy               = dl.value("doy", 0);
            download_config_.download_rinex    = dl.value("download_rinex", false);
            download_config_.download_products = dl.value("download_products", true);
            download_config_.download_gim      = dl.value("download_gim", false);
            std::string atx_str = dl.value("atx_path", "");
            if (!atx_str.empty()) download_config_.atx_path = atx_str;
            std::string nc_str = dl.value("nc_path", "");
            if (!nc_str.empty()) download_config_.nc_path = nc_str;
            std::string crx_str = dl.value("crx2rnx_path", "");
            if (!crx_str.empty()) download_config_.crx2rnx_path = crx_str;
            download_config_.keep_zip_after_unzip = dl.value("keep_zip_after_unzip", false);
            download_config_.atx_auto_refresh = dl.value("atx_auto_refresh", false);
            download_config_.atx_refresh_max_age_days = dl.value("atx_refresh_max_age_days", 7.0);
            download_config_.atx_refresh_url = dl.value("atx_refresh_url", std::string{});

            if (download_config_.enabled) {
                log->info("Data download enabled: root={}, source={}, type={}, station={}, year={}, doy={}",
                    download_config_.data_root.string(), download_config_.product_source,
                    download_config_.product_type, download_config_.station,
                    download_config_.year, download_config_.doy);
            }
        }

        // Internal mode is mutually exclusive with auto-download.
        // loadFilesSection_ runs before this, so data_source_ is set.
        // Forcing the flag here means downstream code (Application,
        // ApplicationBaseline, PathResolver) only has to check one
        // condition — DownloadConfig::enabled — to know whether it
        // should hit the network.
        if (data_source_ == DataSource::Internal && download_config_.enabled) {
            log->info("data_source=internal overrides data_download.enabled "
                      "(no network access in internal mode)");
            download_config_.enabled = false;
        }
    }

    // =================================================================
    //  Section: application mode & realtime
    // =================================================================
    void GnssConfig::loadAppModeAndRealtimeSection_(const nlohmann::json& json, LogManager& log_manager) {
        auto log = log_manager.getLogger("gnss_config");
        {
            std::string mode_str = json.value("app_mode", "post");
            if (mode_str == "realtime" || mode_str == "real_time" || mode_str == "rt") {
                app_mode_ = AppMode::RealTime;
            } else {
                app_mode_ = AppMode::PostProcessing;
            }
            log->info("Application mode: {}", app_mode_ == AppMode::RealTime ? "realtime" : "post");
            // Wiring reality (invariant #4), sited HERE because app_mode_ is
            // settled only at this point: the wind-up census is armed on the
            // network producer and on the single-receiver Application::run path
            // only.  In baseline and real-time modes it collects nothing and
            // prints nothing, which is indistinguishable from a clean result —
            // so say so at load time rather than let a reader infer a null from
            // silence.
            if (trace_windup_census_
                && (isBaselineMode() || app_mode_ == AppMode::RealTime)) {
                log->warn("estimation.trace_windup_census has no wiring in this "
                          "mode (baseline / real-time): its counters are never "
                          "armed and nothing is reported, so a silent run here is "
                          "NOT a null result.  It is INERT — arm it on a network "
                          "or PPP run to measure the wind-up step/gap population.");
            }

            // Smoother is post-processing only.
            if (run_smoother_ && app_mode_ == AppMode::RealTime) {
                run_smoother_ = false;
                log->warn("run_smoother=true ignored in real-time mode (smoothing is post-processing only)");
            }
            // Non-causal slip bridge rides on the smoother → post-processing only.
            if (enable_noncausal_slip_bridge_ && app_mode_ == AppMode::RealTime) {
                enable_noncausal_slip_bridge_ = false;
                log->warn("enable_noncausal_slip_bridge=true ignored in real-time mode (post-processing only)");
            }
            // Normal-equation export is a batch product → post-processing only.
            if (enable_neq_export_ && app_mode_ == AppMode::RealTime) {
                enable_neq_export_ = false;
                log->warn("enable_neq_export=true ignored in real-time mode (post-processing only)");
            }
        }
        {
            const auto& rt = jsection(json, "realtime", "realtime_config");
            realtime_config_.ntrip_host        = rt.value("ntrip_host", "");
            realtime_config_.ntrip_port         = rt.value("ntrip_port", 2101);
            realtime_config_.ntrip_mountpoint   = rt.value("ntrip_mountpoint", "");
            realtime_config_.ntrip_user         = rt.value("ntrip_user", "");
            realtime_config_.ntrip_password     = rt.value("ntrip_password", "");
            realtime_config_.nmea_gga           = rt.value("nmea_gga", "");
            realtime_config_.reconnect_delay_s  = rt.value("reconnect_delay_s", 5);
            realtime_config_.ntrip_use_tls      = rt.value("ntrip_use_tls", false);
            realtime_config_.ntrip_tls_insecure = rt.value("ntrip_tls_insecure", false);
            realtime_config_.input_type         = rt.value("input_type", "ntrip");
            realtime_config_.input_format       = rt.value("input_format", "rtcm");
            realtime_config_.input_path         = rt.value("input_path", "");
            realtime_config_.tcp_host           = rt.value("tcp_host", "");
            realtime_config_.tcp_port           = rt.value("tcp_port", 28784);
            realtime_config_.output_type        = rt.value("output_type", "console");
            realtime_config_.output_path        = rt.value("output_path", "");
            realtime_config_.max_epoch_buffer   = rt.value("max_epoch_buffer", 100U);
            // RTB-4: pin precise (ultra-rapid) orbits while clocks stay live/network.
            realtime_config_.use_precise_orbits = rt.value("use_precise_orbits", false);
            if (realtime_config_.use_precise_orbits) {
                log->info("realtime.use_precise_orbits enabled — precise (ultra-rapid) "
                          "SP3 orbits pinned; satellite clocks stay on the live/network "
                          "source (RTB-4)");
            }
            // RTA-6: live network product file to tail (empty = off).  Consumed
            // by runRealTime via NetworkProductTail into an OWNED SsrCorrectionStore.
            realtime_config_.network_products_path = rt.value("network_products_path", "");
            // RT filter-state snapshot (consumer warm start — the RTB-7 complement).
            // Non-empty path arms the periodic write AND the staged warm resume on
            // any engine built from this config; the closed-loop consumer inherits
            // it through deriveClosedLoopConsumerConfig's full copy.  Empty =
            // byte-identical off.
            realtime_config_.state_snapshot_path = rt.value("state_snapshot_path", "");
            realtime_config_.state_snapshot_interval_s =
                rt.value("state_snapshot_interval_s", 10.0);
            realtime_config_.state_snapshot_horizon_s =
                rt.value("state_snapshot_horizon_s", 300.0);
            realtime_config_.state_snapshot_seam_refloat =
                rt.value("state_snapshot_seam_refloat", true);
            realtime_config_.state_snapshot_restore_scope =
                rt.value("state_snapshot_restore_scope", "core");
            if (realtime_config_.state_snapshot_restore_scope != "core"
                && realtime_config_.state_snapshot_restore_scope != "full") {
                log->warn("realtime.state_snapshot_restore_scope '{}' is not one of "
                          "core|full; using the default 'core'.",
                          realtime_config_.state_snapshot_restore_scope);
                realtime_config_.state_snapshot_restore_scope = "core";
            }
            if (!realtime_config_.state_snapshot_path.empty()) {
                if (realtime_config_.state_snapshot_interval_s <= 0.0) {
                    log->warn("realtime.state_snapshot_interval_s {} must be > 0; clamped to "
                              "the default 10 s.", realtime_config_.state_snapshot_interval_s);
                    realtime_config_.state_snapshot_interval_s = 10.0;
                }
                if (realtime_config_.state_snapshot_horizon_s <= 0.0) {
                    log->warn("realtime.state_snapshot_horizon_s {} must be > 0; clamped to "
                              "the default 300 s.", realtime_config_.state_snapshot_horizon_s);
                    realtime_config_.state_snapshot_horizon_s = 300.0;
                }
                log->info("realtime.state_snapshot_path set — RT filter-state snapshot armed: "
                          "atomic write to '{}' every {:.0f} s of data time, and a staged warm "
                          "RESUME at startup (fingerprint-gated, horizon {:.0f} s, applied at "
                          "the first processed epoch).  The engine's slip QC / LLI / SSR "
                          "datum-break reactions are the restored state's event responses.",
                          realtime_config_.state_snapshot_path,
                          realtime_config_.state_snapshot_interval_s,
                          realtime_config_.state_snapshot_horizon_s);
            }
            // RTA-12(3): blunder gate on the RTCM 1005/1006 a-priori seed (m).
            // 0 (default) = off, legacy seed-always behaviour; suggested 10.0.
            realtime_config_.arp_blunder_gate_m = rt.value("arp_blunder_gate_m", 0.0);
            // RTB-1(c): GPS-UTC leap seconds for the RT wall-clock -> GPS
            // conversion.  Default = the compiled TimeUtils::LEAP_SECONDS, so
            // an absent key is byte-identical; a non-default value is applied
            // via TimeUtils::setLeapSecondsOverride in RealTimeEngine::initialize.
            realtime_config_.leap_seconds = rt.value(
                "leap_seconds", static_cast<int>(TimeUtils::LEAP_SECONDS));
            if (realtime_config_.leap_seconds != static_cast<int>(TimeUtils::LEAP_SECONDS)) {
                log->info("realtime.leap_seconds override: {} s (compiled default {}) — "
                          "applied to the RT wall-clock->GPS conversion; nav-derived "
                          "leap seconds are the RTF-scale follow-on",
                          realtime_config_.leap_seconds,
                          static_cast<int>(TimeUtils::LEAP_SECONDS));
            }
            // RTB-3: MSM lock-time -> LLI loss-of-lock derivation (default off —
            // arming changes slip-detection behaviour on RT replays).
            realtime_config_.msm_lli_from_locktime =
                rt.value("msm_lli_from_locktime", false);
            if (realtime_config_.msm_lli_from_locktime) {
                log->info("realtime.msm_lli_from_locktime enabled — LLI 0x1 derived "
                          "from MSM lock-time decreases at epoch assembly (LockTimeTracker)");
            }
            // RTB-5(c): skip the satellite-PCO range term for satellites whose
            // applied SSR orbit is APC-referenced (legacy-SSR dialect).  Default
            // off = byte-identical (historical double count preserved).
            realtime_config_.honor_ssr_reference_point =
                rt.value("honor_ssr_reference_point", false);
            if (realtime_config_.honor_ssr_reference_point) {
                log->info("realtime.honor_ssr_reference_point enabled — satellite PCO "
                          "skipped for APC-referenced (legacy-SSR) orbit corrections");
            }

            // Optional SSR corrections stream (separate NTRIP mountpoint
            // or file-replay).  Default: disabled — single-stream behaviour.
            if (rt.contains("corrections") && rt.at("corrections").is_object()) {
                const auto& cc = rt.at("corrections");
                auto& c = realtime_config_.corrections;
                c.enabled            = cc.value("enabled", false);
                c.input_type         = cc.value("input_type", std::string{"ntrip"});
                c.ntrip_host         = cc.value("ntrip_host", std::string{});
                c.ntrip_port         = cc.value("ntrip_port", 2101);
                c.ntrip_mountpoint   = cc.value("ntrip_mountpoint", std::string{});
                c.ntrip_user         = cc.value("ntrip_user", std::string{});
                c.ntrip_password     = cc.value("ntrip_password", std::string{});
                c.nmea_gga           = cc.value("nmea_gga", std::string{});
                c.reconnect_delay_s  = cc.value("reconnect_delay_s", 5);
                c.ntrip_use_tls      = cc.value("ntrip_use_tls", false);
                c.ntrip_tls_insecure = cc.value("ntrip_tls_insecure", false);
                c.input_path         = cc.value("input_path", std::string{});
                c.tcp_host           = cc.value("tcp_host", std::string{});
                c.tcp_port           = cc.value("tcp_port", 28784);
                // RTA-8(b): per-stream SSR ingest accept policy.  "all" (default)
                // = byte-identical unfiltered push; "orbit_only" drops this
                // stream's clock/bias content and decomposes combined records so
                // an RTS clock can never shadow an injected network clock.
                c.accept             = detail::toLower(
                    cc.value("corrections_accept", std::string{"all"}));
                if (c.accept != "all" && c.accept != "orbit_only") {
                    log->warn("Unknown realtime.corrections.corrections_accept '{}' "
                              "(legal: \"all\", \"orbit_only\"); using \"all\"", c.accept);
                    c.accept = "all";
                }
            }

            // Optional third RTCM stream: dedicated broadcast ephemeris (nav),
            // for an obs-only station mountpoint that carries no 1019/1020/etc.
            // Default disabled — a self-contained obs+nav station needs only
            // primary + corrections.  Same transport descriptor as corrections.
            if (rt.contains("ephemeris") && rt.at("ephemeris").is_object()) {
                const auto& ec = rt.at("ephemeris");
                auto& e = realtime_config_.ephemeris;
                e.enabled            = ec.value("enabled", false);
                e.input_type         = ec.value("input_type", std::string{"ntrip"});
                e.ntrip_host         = ec.value("ntrip_host", std::string{});
                e.ntrip_port         = ec.value("ntrip_port", 2101);
                e.ntrip_mountpoint   = ec.value("ntrip_mountpoint", std::string{});
                e.ntrip_user         = ec.value("ntrip_user", std::string{});
                e.ntrip_password     = ec.value("ntrip_password", std::string{});
                e.nmea_gga           = ec.value("nmea_gga", std::string{});
                e.reconnect_delay_s  = ec.value("reconnect_delay_s", 5);
                e.ntrip_use_tls      = ec.value("ntrip_use_tls", false);
                e.ntrip_tls_insecure = ec.value("ntrip_tls_insecure", false);
                e.input_path         = ec.value("input_path", std::string{});
                e.tcp_host           = ec.value("tcp_host", std::string{});
                e.tcp_port           = ec.value("tcp_port", 28784);
                // RTA-8(b): same per-stream ingest policy as the corrections
                // block (the two streams share the transport descriptor).  A nav
                // stream normally carries no SSR, so this is a guard against a
                // mixed mountpoint leaking clock content.
                e.accept             = detail::toLower(
                    ec.value("corrections_accept", std::string{"all"}));
                if (e.accept != "all" && e.accept != "orbit_only") {
                    log->warn("Unknown realtime.ephemeris.corrections_accept '{}' "
                              "(legal: \"all\", \"orbit_only\"); using \"all\"", e.accept);
                    e.accept = "all";
                }
            }

            if (app_mode_ == AppMode::RealTime) {
                log->info("Real-time config: input={}, ntrip={}://{}:{}/{}, output={}",
                    realtime_config_.input_type,
                    realtime_config_.ntrip_use_tls ? "https" : "http",
                    realtime_config_.ntrip_host,
                    realtime_config_.ntrip_port,
                    realtime_config_.ntrip_mountpoint,
                    realtime_config_.output_type);
                if (realtime_config_.corrections.enabled) {
                    const auto& c = realtime_config_.corrections;
                    if (c.input_type == "ntrip") {
                        log->info("Real-time corrections: ntrip={}://{}:{}/{}",
                                  c.ntrip_use_tls ? "https" : "http",
                                  c.ntrip_host, c.ntrip_port, c.ntrip_mountpoint);
                    } else {
                        log->info("Real-time corrections: file={}", c.input_path);
                    }
                    // RTA-8(b): surface a non-default ingest policy once at startup.
                    if (c.accept != "all")
                        log->info("Real-time corrections ingest policy: {} — this stream's "
                                  "standalone clock / code-bias / phase-bias / high-rate-clock "
                                  "records are dropped; combined orbit+clock records are "
                                  "decomposed to orbit-only.", c.accept);
                }
                if (realtime_config_.ephemeris.enabled) {
                    const auto& e = realtime_config_.ephemeris;
                    if (e.input_type == "ntrip") {
                        log->info("Real-time ephemeris (nav): ntrip={}://{}:{}/{}",
                                  e.ntrip_use_tls ? "https" : "http",
                                  e.ntrip_host, e.ntrip_port, e.ntrip_mountpoint);
                    } else {
                        log->info("Real-time ephemeris (nav): file={}", e.input_path);
                    }
                }
                if (!realtime_config_.network_products_path.empty()) {
                    log->info("Real-time network products (RTA-6): tailing file={}",
                              realtime_config_.network_products_path);
                }
            }
        }
    }

    // =================================================================
    //  Section: baseline  (BaselineSingle / BaselineMulti)
    // =================================================================
    bool GnssConfig::loadBaselineSection_(const nlohmann::json& json, LogManager& log_manager) {
        auto log = log_manager.getLogger("gnss_config");
        const auto& bl = jsection(json, "baseline", "baseline");
        if (bl.is_null() || !bl.is_object()) {
            log->error("Baseline mode selected but no 'baseline' section found in config");
            return false;
        }

        // --- Reference station (required) ---
        const auto ref_it = bl.find("reference");
        if (ref_it == bl.end() || !ref_it->is_object()) {
            log->error("Baseline: missing 'reference' station block");
            return false;
        }
        baseline_config_.reference.name       = ref_it->value("name", std::string{"REF"});
        baseline_config_.reference.rinex_path = ref_it->value("rinex_path", std::string{});
        baseline_config_.reference.fixed      = ref_it->value("fixed", true);
        if (ref_it->contains("position_ecef") && (*ref_it)["position_ecef"].is_array()
            && (*ref_it)["position_ecef"].size() == 3) {
            const auto& p = (*ref_it)["position_ecef"];
            baseline_config_.reference.position_ecef = {
                p[0].get<double>(), p[1].get<double>(), p[2].get<double>()
            };
            baseline_config_.reference.has_position = true;
        }
        // NOTE: an empty rinex_path is NOT an error here — PathResolver
        // runs after config-load (in runBaseline → resolveData) and
        // fills it in by station-name match against the internal data
        // tree.  Empty position_ecef is similarly tolerated here: the
        // baseline-fixture preset block below may fill it in from the
        // catalog.  Final validation happens AFTER the preset has had
        // its chance — see the @c has_position check just past the
        // preset block.

        // --- Rovers (required: one for Single, one-or-more for Multi) ---
        baseline_config_.rovers.clear();
        auto addRover = [&](const nlohmann::json& rj) {
            StationConfig sc;
            sc.name       = rj.value("name", std::string{"ROV"});
            sc.rinex_path = rj.value("rinex_path", std::string{});
            sc.fixed      = false;
            if (rj.contains("position_ecef") && rj["position_ecef"].is_array()
                && rj["position_ecef"].size() == 3) {
                sc.position_ecef = {
                    rj["position_ecef"][0].get<double>(),
                    rj["position_ecef"][1].get<double>(),
                    rj["position_ecef"][2].get<double>()
                };
                sc.has_position = true;
            }
            baseline_config_.rovers.push_back(std::move(sc));
        };
        if (bl.contains("rover") && bl["rover"].is_object()) {
            addRover(bl["rover"]);
        }
        if (bl.contains("rovers") && bl["rovers"].is_array()) {
            for (const auto& rj : bl["rovers"]) {
                if (rj.is_object()) addRover(rj);
            }
        }

        // ---- Apply named-fixture preset for any blank slots --------
        // Top-level @c baseline_fixture chooses one of the bundled
        // presets (see include/config/BaselineFixtures.hpp).  The
        // preset only fills fields the user left empty: explicit
        // entries in baseline.reference / baseline.rover always win.
        // Unknown values fall back to "Custom" (no defaults applied).
        // Empty / missing field also defaults to Custom so legacy
        // configs keep working.
        {
            std::string preset_raw = jval<std::string>(json,
                "baseline_fixture", "baseline_fixture", std::string{"Custom"});
            std::string preset_low = toLower(preset_raw);
            // Compact whitespace and dashes/underscores to a single
            // representation so "IGS_Pasadena" / "igs-pasadena" /
            // "igs pasadena" all hit the same key.
            for (auto& c : preset_low) if (c == '-' || c == ' ') c = '_';
            if (preset_low.empty() || preset_low == "custom" || preset_low == "none") {
                log->info("Baseline fixture: custom (no preset applied)");
            } else {
                const auto& presets = baselineFixturePresets();
                auto it = presets.find(preset_low);
                if (it == presets.end()) {
                    log->warn("Unknown baseline_fixture '{}' — known presets: ", preset_raw);
                    for (const auto& kv : presets) log->warn("    {}", kv.first);
                    log->warn("Continuing as if baseline_fixture=Custom.");
                } else {
                    const auto& p = it->second;
                    bool applied_anything = false;

                    // Reference station: name + ECEF.
                    auto& ref = baseline_config_.reference;
                    const bool ref_name_empty =
                        ref.name.empty() || ref.name == "REF";
                    if (ref_name_empty) {
                        ref.name = p.ref_name;
                        applied_anything = true;
                    }
                    if (!ref.has_position) {
                        ref.position_ecef = p.ref_ecef;
                        ref.has_position  = true;
                        applied_anything  = true;
                    }

                    // Rovers: when none configured, seed one from the
                    // preset.  When one slot already exists with a
                    // placeholder name ("ROV") that we want to fill,
                    // fill it; otherwise leave the user's roster
                    // untouched (e.g. multi-rover networks).
                    if (baseline_config_.rovers.empty()) {
                        StationConfig rv;
                        rv.name  = p.rover_name;
                        rv.fixed = false;
                        baseline_config_.rovers.push_back(std::move(rv));
                        applied_anything = true;
                    } else if (baseline_config_.rovers.size() == 1) {
                        auto& rv = baseline_config_.rovers.front();
                        if (rv.name.empty() || rv.name == "ROV") {
                            rv.name = p.rover_name;
                            applied_anything = true;
                        }
                    }

                    // Production-mode date fields: only push the
                    // preset's year/doy into data_download when the
                    // user hasn't typed their own.  This lets a
                    // baseline run flip between fixtures by changing
                    // a single field without nuking a year/doy that
                    // was set up for an unrelated PPP run.
                    if (download_config_.year == 0) download_config_.year = p.year;
                    if (download_config_.doy == 0) download_config_.doy = p.doy;

                    if (applied_anything) {
                        log->info("Baseline fixture preset '{}' applied: {}",
                            preset_low, p.description);
                    } else {
                        log->info("Baseline fixture preset '{}' selected but every "
                                  "field was already user-supplied: {}",
                                  preset_low, p.description);
                    }
                }
            }
        }

        // ---- Post-preset validation --------------------------------
        // Reference station must have a position by now: either the
        // user supplied position_ecef explicitly, or a recognised
        // baseline_fixture preset filled it in.  Fail loudly when
        // neither happened — without coordinates the DD solution has
        // no anchor and the rover absolute position is meaningless.
        if (!baseline_config_.reference.has_position) {
            log->error("Baseline: reference station '{}' requires position_ecef [X,Y,Z] "
                       "(neither user-supplied nor filled by a baseline_fixture preset).",
                       baseline_config_.reference.name.empty()
                           ? std::string{"<unnamed>"} : baseline_config_.reference.name);
            return false;
        }
        if (baseline_config_.reference.name.empty()) {
            log->error("Baseline: reference station has no name. "
                       "Set baseline.reference.name explicitly or pick a "
                       "baseline_fixture preset that supplies it.");
            return false;
        }

        baseline_config_.max_baseline_km        = bl.value("max_baseline_km", 30.0);
        baseline_config_.epoch_sync_tolerance_s = bl.value("epoch_sync_tolerance_s", 0.01);
        baseline_config_.code_only              = bl.value("code_only", false);
        baseline_config_.use_doppler            = bl.value("use_doppler", false);
        baseline_config_.signal_aliasing        = bl.value("signal_aliasing", true);
        baseline_config_.use_iono_free          = bl.value("use_iono_free", false);
        baseline_config_.enable_pivot_handover  = bl.value("enable_pivot_handover", true);
        baseline_config_.estimate_differential_tropo =
            bl.value("estimate_differential_tropo", false);
        baseline_config_.differential_tropo_init_std_m =
            bl.value("differential_tropo_init_std_m", 0.1);
        baseline_config_.differential_tropo_process_noise_m_per_sqrt_s =
            bl.value("differential_tropo_process_noise_m_per_sqrt_s", 1e-4);
        if (baseline_config_.code_only) {
            log->info("Baseline: code_only=true — phase DDs will be skipped by the filter");
        }
        if (baseline_config_.use_doppler) {
            log->info("Baseline: use_doppler=true — Doppler DDs will be formed (filter update ignores them until a velocity state is added)");
        }
        if (baseline_config_.signal_aliasing) {
            log->info("Baseline: signal_aliasing=true — DD rows pair across tracking attributes "
                      "within the same band (e.g. rover C2W vs reference C2C). "
                      "Set to false for strict RINEX-code matching.");
        } else {
            log->info("Baseline: signal_aliasing=false — strict RINEX-code matching only");
        }
        if (baseline_config_.use_iono_free) {
            log->info("Baseline: use_iono_free=true — per-band rows collapsed into the dual-frequency "
                      "ionosphere-free combination.  Phase IF ambiguities are float-only (no AR). "
                      "Sat-pairs with <2 shared bands are dropped in this mode.");
        }
        if (!baseline_config_.enable_pivot_handover) {
            log->info("Baseline: enable_pivot_handover=false — DD ambiguities will be erased and "
                      "re-seeded on every pivot change (legacy behavior).");
        }
        if (baseline_config_.estimate_differential_tropo) {
            log->info("Baseline: estimate_differential_tropo=true — adding 1 differential ZWD "
                      "state (init_std={:.3f} m, process_noise={:.2e} m/sqrt(s)).  Recommended "
                      "for >10-20 km baselines where the wet delay no longer cancels in BRSD.",
                      baseline_config_.differential_tropo_init_std_m,
                      baseline_config_.differential_tropo_process_noise_m_per_sqrt_s);
        }

        // --- Validate against mode ---
        if (baseline_config_.rovers.empty()) {
            log->error("Baseline: at least one rover station required "
                "(provide 'rover' object or 'rovers' array)");
            return false;
        }
        if (processing_mode_ == ProcessingMode::BaselineSingle
            && baseline_config_.rovers.size() > 1) {
            log->warn("Baseline-Single configured with {} rovers; "
                "only the first will be processed. Use Baseline-Multi for a network.",
                baseline_config_.rovers.size());
            baseline_config_.rovers.resize(1);
        }
        // Rover rinex_path may be empty here too — PathResolver fills
        // it in on the same pass that resolves the reference station.
        // Final rinex_path check happens in runBaseline() after
        // resolveData().  Names, however, must be set by now: either
        // the user typed them or a baseline_fixture preset filled
        // them in.  An empty rover name would defeat PathResolver's
        // station-name match (findObservationByStation returns empty
        // on empty input), and the downstream "no rinex_path" error
        // would be confusing.
        for (size_t i = 0; i < baseline_config_.rovers.size(); ++i) {
            if (baseline_config_.rovers[i].name.empty()) {
                log->error("Baseline: rover #{} has no name. "
                           "Set baseline.rover[s].name explicitly or pick a "
                           "baseline_fixture preset that supplies it.", i);
                return false;
            }
        }

        // --- Summary log ---
        log->info("Baseline config: reference='{}' ({}), rovers={}, max_baseline={:.1f} km, sync_tol={:.3f} s",
            baseline_config_.reference.name,
            baseline_config_.reference.fixed ? "fixed" : "weakly constrained",
            baseline_config_.rovers.size(),
            baseline_config_.max_baseline_km,
            baseline_config_.epoch_sync_tolerance_s);
        for (const auto& rv : baseline_config_.rovers) {
            log->info("  rover '{}': rinex={}{}",
                rv.name, rv.rinex_path.string(),
                rv.has_position ? ", a-priori position supplied" : "");
        }
        return true;
    }

    // =================================================================
    //  Section: logging
    // =================================================================
    void GnssConfig::loadLoggingSection_(const nlohmann::json& json, LogManager& log_manager) {
        const auto& lg = jsection(json, "logging", "logging");
        // All logger toggles default to *false*: a missing key means
        // "off."  Configs explicitly opt loggers in by setting their
        // enable_* key to true.  This keeps batch-mode runs (and any
        // other quiet-by-design caller) from inheriting verbose
        // per-epoch output through omission.  Interactive configs
        // that want rich diagnostics list each logger they want on.
        // spdlog file loggers
        logging_config_.enable_main_log           = lg.value("enable_main_log", false);
        logging_config_.enable_gnss_config        = lg.value("enable_gnss_config", false);
        logging_config_.enable_kalman_filter      = lg.value("enable_kalman_filter", false);
        logging_config_.enable_smoother           = lg.value("enable_smoother", false);
        logging_config_.enable_state_manager      = lg.value("enable_state_manager", false);
        logging_config_.enable_ppp_model          = lg.value("enable_ppp_model", false);
        logging_config_.enable_measurement_model  = lg.value("enable_measurement_model", false);
        logging_config_.enable_mw_slip_detector   = lg.value("enable_mw_slip_detector", false);
        logging_config_.enable_if_slip_detector   = lg.value("enable_if_slip_detector", false);
        logging_config_.enable_gf_slip_detector   = lg.value("enable_gf_slip_detector", false);
        logging_config_.enable_turbo_edit_resolver = lg.value("enable_turbo_edit_resolver", false);
        logging_config_.enable_cycle_slip_manager = lg.value("enable_cycle_slip_manager", false);
        logging_config_.enable_ambiguity_resolver = lg.value("enable_ambiguity_resolver", false);
        logging_config_.enable_corrections        = lg.value("enable_corrections", false);
        logging_config_.enable_geometry           = lg.value("enable_geometry", false);
        logging_config_.enable_troposphere        = lg.value("enable_troposphere", false);
        logging_config_.enable_ionosphere         = lg.value("enable_ionosphere", false);
        logging_config_.enable_antenna            = lg.value("enable_antenna", false);
        logging_config_.enable_antenna_model      = lg.value("enable_antenna_model", false);
        logging_config_.enable_ocean_tide         = lg.value("enable_ocean_tide", false);
        logging_config_.enable_planetary_model    = lg.value("enable_planetary_model", false);
        logging_config_.enable_stochastic_model   = lg.value("enable_stochastic_model", false);
        logging_config_.enable_rinex_parser       = lg.value("enable_rinex_parser", false);
        logging_config_.enable_product_parser     = lg.value("enable_product_parser", false);
        logging_config_.enable_product_cache      = lg.value("enable_product_cache", false);
        logging_config_.enable_resolver           = lg.value("enable_resolver", false);
        logging_config_.enable_state_initializer  = lg.value("enable_state_initializer", false);
        logging_config_.enable_bssd_pivot_manager = lg.value("enable_bssd_pivot_manager", false);
        logging_config_.enable_bssd_transform     = lg.value("enable_bssd_transform", false);
        logging_config_.enable_ppp_utils          = lg.value("enable_ppp_utils", false);
        logging_config_.enable_downloader         = lg.value("enable_downloader", false);
        logging_config_.enable_ntrip              = lg.value("enable_ntrip", false);
        logging_config_.enable_rt_engine          = lg.value("enable_rt_engine", false);
        logging_config_.enable_imu                = lg.value("enable_imu", false);
        // CSV trace files
        logging_config_.enable_state_trace        = lg.value("enable_state_trace", false);
        logging_config_.enable_ambiguity_trace    = lg.value("enable_ambiguity_trace", false);
        logging_config_.enable_ionosphere_trace   = lg.value("enable_ionosphere_trace", false);
        logging_config_.enable_sat_bias_trace     = lg.value("enable_sat_bias_trace", false);
        logging_config_.enable_code_bias_trace    = lg.value("enable_code_bias_trace", false);
        logging_config_.enable_sat_status_trace   = lg.value("enable_sat_status_trace", false);
        logging_config_.enable_dop_trace          = lg.value("enable_dop_trace", false);
        logging_config_.enable_residual_trace     = lg.value("enable_residual_trace", false);
        logging_config_.enable_wls_trace          = lg.value("enable_wls_trace", false);
        logging_config_.enable_smoother_trace     = lg.value("enable_smoother_trace", false);
        logging_config_.enable_mw_slip_trace      = lg.value("enable_mw_slip_trace", false);
        logging_config_.enable_if_slip_trace      = lg.value("enable_if_slip_trace", false);
        logging_config_.enable_gf_slip_trace      = lg.value("enable_gf_slip_trace", false);
        logging_config_.enable_turbo_edit_trace   = lg.value("enable_turbo_edit_trace", false);
        logging_config_.enable_windup_trace       = lg.value("enable_windup_trace", false);

        // Concise KF log mode — suppresses intermediate rebuild passes,
        // keeps pass-1 + final-pass output verbatim. Default off (full).
        logging_config_.kalman_filter_concise_log = lg.value("kalman_filter_concise_log", false);

        // Per-logger level overrides: logging.levels = { "name": "level", ... }
        // Each entry sets one logger's emission threshold; loggers absent from
        // the map fall back to the global level set in LogManager::initialize().
        // Disabled loggers (enable_X = false) take precedence over any entry here.
        logging_config_.per_logger_levels.clear();
        if (lg.contains("levels") && lg["levels"].is_object()) {
            for (const auto& [name, val] : lg["levels"].items()) {
                if (!val.is_string()) continue;
                try {
                    logging_config_.per_logger_levels[name] = logLevelFromString(val.get<std::string>());
                } catch (const std::invalid_argument& e) {
                    log_manager.getLogger("gnss_config")->warn(
                        "Ignoring unknown log level for '{}': {}", name, e.what());
                }
            }
        }
    }

    // =================================================================
    //  Summary
    // =================================================================
    void GnssConfig::logConfigurationSummary_(LogManager& log_manager) const {
        const char* mode_str =
            processing_mode_ == ProcessingMode::PPP              ? "PPP"             :
            processing_mode_ == ProcessingMode::PPP_RTK          ? "PPP-RTK"         :
            processing_mode_ == ProcessingMode::BaselineSingle   ? "Baseline-Single" :
                                                                   "Baseline-Multi";
        log_manager.getLogger()->info("Loaded configuration: mode={}, uncombined_enabled={}, elevation_mask={} deg, "
            "observation_path={}, navigation_path={}, {} products, leap_seconds={}",
            mode_str,
            uncombined_enabled_, elevation_mask_deg_, observation_path_.string(),
            navigation_path_.string(), product_paths_.size(), leap_seconds_tai_);

        // Consolidated toggle echo.  RELOCATED here from the tail of
        // loadEstimationSection_ so every field reports its FINAL, post-resolution
        // value.  loadFromJson calls this last, after all twelve section helpers and
        // every cross-section forcing (the W6 / EXT-CB / GLONASS estimate_ifb_
        // promotions, the IMU parse, the real-time overrides), so what this prints is
        // what the run actually uses.  Keep it last: a toggle added to this line that
        // a later section can still mutate re-introduces exactly the staleness this
        // move removed.
        const char* basis_label =
            state_ambiguity_basis_ == StateAmbiguityBasis::NlWl ? "nl_wl" : "per_band";

        log_manager.getLogger("gnss_config")->info("Toggles: tropo_grad={}, relin_geom={}, smoother={}, phase_clk={}, if_slip={}, accum_windup={}, wu_gap={:.0f}s, wu_census={}, bssd={}, velocity={}, iono_rate={}, ifb={}, glonass_ifb={}, lock_clkref={}, req_bia={}, sat_bias={}, no_bia_dw={:.0f}, imu={}, amb_basis={}, adaptive_iono_q={}, eclipse_excl={}, cio_frame={}, sun_frame_fix={}, auto_report={}, nc_bridge={}, var_factor={}, data_snoop={}, full_r={}, neq_export={}, trig_sp3={}, orbit_normal_att={}",
            estimate_tropo_gradients_ ? "on" : "off",
            relinearize_geometry_     ? "on" : "off",
            run_smoother_             ? "on" : "off",
            estimate_phase_clock_     ? "on" : "off",
            enable_if_slip_detector_  ? "on" : "off",
            accumulate_phase_windup_  ? "on" : "off",
            phase_windup_max_gap_s_, trace_windup_census_ ? "on" : "off",
            enable_between_satellite_sd_ ? "on" : "off",
            estimate_velocity_        ? "on" : "off",
            enable_iono_rate_         ? "on" : "off",
            estimate_ifb_             ? "on" : "off",
            enable_glonass_ifb_       ? "on" : "off",
            lock_signals_to_clock_ref_ ? "on" : "off",
            require_bia_for_signals_  ? "on" : "off",
            estimate_sat_biases_      ? "on" : "off",
            no_bia_code_downweight_,
            imu_config_.enabled
                ? (imu_config_.coupling_mode == ImuCouplingMode::TightCoupled ? "tight" : "loose")
                : "off",
            basis_label,
            adaptive_iono_q_.enabled ? "on" : "off",
            exclude_eclipsing_satellites_ ? "on" : "off",
            enable_cio_frame_ ? "on" : "off",
            enable_sun_frame_correction_ ? "on" : "off",
            enable_auto_report_ ? "on" : "off",
            enable_noncausal_slip_bridge_ ? "on" : "off",
            enable_variance_factor_ ? "on" : "off",
            enable_data_snooping_ ? "on" : "off",
            enable_full_r_kernel_ ? "on" : "off",
            enable_neq_export_ ? "on" : "off",
            enable_trig_sp3_interp_ ? "on" : "off",
            enable_orbit_normal_attitude_ ? "on" : "off");
    }

} // namespace GnssProcessor
