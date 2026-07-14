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
    // loadFromJson — orchestrator
    // =================================================================

    bool GnssConfig::loadFromJson(const std::filesystem::path& path, LogManager& log_manager) {
        try {
            std::ifstream file(path);
            if (!file.is_open()) {
                log_manager.getLogger()->error("Failed to open config file: {}", path.string());
                return false;
            }

            nlohmann::json json = nlohmann::json::parse(file,
                /*callback=*/ nullptr, /*allow_exceptions=*/ true, /*ignore_comments=*/ true);

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
        // T5: correlated per-LOS SH representativeness-error deviation (GM carry).
        global_vtec_sh_.rep_error_tau_s = jval<double>(json,
            "estimation.global_vtec_sh.rep_error_tau_s", "global_vtec_sh_rep_error_tau_s", 0.0);
        if (global_vtec_sh_.rep_error_tau_s < 0.0) global_vtec_sh_.rep_error_tau_s = 0.0;
        if (global_vtec_sh_.rep_error_tau_s > 0.0) {
            if (log) {
                log->info("Network (T5): SH representativeness-error GM deviation ON — the "
                          "SH↔slant pin's per-LOS misfit is tracked as a Gauss–Markov deviation "
                          "(τ {} s, steady-state σ = representation_sigma_tecu {} TECU mapped to "
                          "slant) instead of re-applied white; targets the dispersive misfit→"
                          "ambiguity leakage behind the uniform DD fractions (T4 2026-07-10).",
                          global_vtec_sh_.rep_error_tau_s,
                          global_vtec_sh_.representation_sigma_tecu);
                if (global_vtec_sh_.row_decimation_epochs >= 2)
                    log->warn("Network (T5): rep_error_tau_s supersedes row_decimation_epochs "
                              "({}) — the T1b σ-inflation is ignored while the GM deviation is on.",
                              global_vtec_sh_.row_decimation_epochs);
            }
            global_vtec_sh_.row_decimation_epochs = 0;
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
        ar_nl_max_per_pair_frac_ = jval<double>(json, "estimation.ambiguity_resolution.nl_max_per_pair_frac",
                                                 "ar_nl_max_per_pair_frac", 0.15);
        ar_enable_partial_ar_    = jval<bool>(json, "estimation.ambiguity_resolution.enable_partial_ar",
                                               "ar_enable_partial_ar", false);

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
        const char* basis_label =
            state_ambiguity_basis_ == StateAmbiguityBasis::NlWl ? "nl_wl" : "per_band";

        log->info("Toggles: tropo_grad={}, relin_geom={}, smoother={}, phase_clk={}, if_slip={}, accum_windup={}, bssd={}, velocity={}, iono_rate={}, ifb={}, glonass_ifb={}, lock_clkref={}, req_bia={}, sat_bias={}, no_bia_dw={:.0f}, imu={}, amb_basis={}, adaptive_iono_q={}, eclipse_excl={}, cio_frame={}, auto_report={}, nc_bridge={}, var_factor={}, data_snoop={}, full_r={}, neq_export={}, trig_sp3={}, orbit_normal_att={}",
            estimate_tropo_gradients_ ? "on" : "off",
            relinearize_geometry_     ? "on" : "off",
            run_smoother_             ? "on" : "off",
            estimate_phase_clock_     ? "on" : "off",
            enable_if_slip_detector_  ? "on" : "off",
            accumulate_phase_windup_  ? "on" : "off",
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
            enable_auto_report_ ? "on" : "off",
            enable_noncausal_slip_bridge_ ? "on" : "off",
            enable_variance_factor_ ? "on" : "off",
            enable_data_snooping_ ? "on" : "off",
            enable_full_r_kernel_ ? "on" : "off",
            enable_neq_export_ ? "on" : "off",
            enable_trig_sp3_interp_ ? "on" : "off",
            enable_orbit_normal_attitude_ ? "on" : "off");
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
            "filter.phase_elevation_power", "phase_elevation_power", 0.5);
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
        // estimation.aiding.imu always wins.  Behaviorally inert until
        // AidingSourceManager is wired into the epoch loop (see
        // docs/aiding_source_abstraction_design.md).
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

    namespace {

        /**
         * @brief RTA-12: parse a coordinate-epoch declaration into a decimal year.
         *
         * Accepts the two documented @c network.coord_epoch shapes: a decimal year
         * ("2024.5") or an ISO calendar date ("2024-07-01", mapped to the day
         * centre).  Anything else — including trailing junk — is rejected so the
         * coordinate-age advisory can distinguish "unset" from "unparseable".
         *
         * @param s Epoch string as typed in the config.
         * @return Decimal year, or @c std::nullopt when unparseable.
         */
        std::optional<double> parseCoordEpochYear(const std::string& s) {
            if (s.empty()) return std::nullopt;
            // ISO date YYYY-MM-DD -> decimal year at the day centre.
            if (s.size() == 10 && s[4] == '-' && s[7] == '-') {
                int y = 0, m = 0, d = 0;
                if (std::sscanf(s.c_str(), "%4d-%2d-%2d", &y, &m, &d) == 3
                    && m >= 1 && m <= 12 && d >= 1 && d <= 31) {
                    static const int cum[12] = { 0, 31, 59, 90, 120, 151, 181,
                                                 212, 243, 273, 304, 334 };
                    const bool leap = (y % 4 == 0 && y % 100 != 0) || (y % 400 == 0);
                    const double doy = cum[m - 1] + d + ((leap && m > 2) ? 1 : 0);
                    return static_cast<double>(y) + (doy - 0.5) / (leap ? 366.0 : 365.0);
                }
                return std::nullopt;
            }
            // Decimal year (whole string must consume; sane geodetic range).
            try {
                std::size_t pos = 0;
                const double v = std::stod(s, &pos);
                if (pos == s.size() && v > 1900.0 && v < 2200.0) return v;
            } catch (const std::exception&) {}
            return std::nullopt;
        }

    } // namespace

    // =================================================================
    //  Section: network  (PPP_RTK_Network producer)
    // =================================================================
    bool GnssConfig::loadNetworkSection_(const nlohmann::json& json, LogManager& log_manager) {
        auto log = log_manager.getLogger("gnss_config");
        const auto& nw = jsection(json, "network", "network");
        if (nw.is_null() || !nw.is_object()) {
            log->error("PPP-RTK-Network mode selected but no 'network' section found in config");
            return false;
        }

        // RTB-8: read the service-profile flag FIRST.  Several coherence warnings
        // in this section (per_const no-op, contract-rebroadcast inert, warmup-URA
        // no-op, emit_ura-without-raim, retro-bump-no-tier, RT0-5 inert rate) would
        // be FALSE under service_mode — the bundle arms their prerequisites in the
        // arming block near the end of this section — so they are gated on
        // !service_mode.  The arming ITSELF still runs LAST, after every armed
        // toggle is parsed.  Kept as a member (networkServiceMode) purely for
        // provenance: the L15 run manifest records that the run's toggle set came
        // from the bundle rather than hand-tuning.
        network_service_mode_ =
            jval<bool>(json, "network.service_mode", "network_service_mode", false);
        const bool service_mode = network_service_mode_;

        // --- Peer stations (>=2; each holds coordinates fixed) ---
        network_config_.stations.clear();
        auto addStation = [&](const nlohmann::json& sj) {
            StationConfig sc;
            sc.name       = sj.value("name", std::string{});
            sc.rinex_path = sj.value("rinex_path", std::string{});
            sc.fixed      = sj.value("fixed", true);  // network holds station coordinates fixed
            if (sj.contains("position_ecef") && sj["position_ecef"].is_array()
                && sj["position_ecef"].size() == 3) {
                sc.position_ecef = {
                    sj["position_ecef"][0].get<double>(),
                    sj["position_ecef"][1].get<double>(),
                    sj["position_ecef"][2].get<double>()
                };
                sc.has_position = true;
            }
            network_config_.stations.push_back(std::move(sc));
        };
        if (nw.contains("stations") && nw["stations"].is_array()) {
            for (const auto& sj : nw["stations"])
                if (sj.is_object()) addStation(sj);
        }

        if (network_config_.stations.size() < 2) {
            log->error("Network: need >=2 stations in network.stations (got {})",
                       network_config_.stations.size());
            return false;
        }

        // --- RTA-12: contract identity declarations + SIF roster coordinates ---
        // frame_id / coord_epoch are pure declarations stamped into the product
        // S-contract (frame=/cepoch= fields; empty = omitted, byte-identical).
        network_config_.frame_id =
            jval<std::string>(json, "network.frame_id", "frame_id", std::string{});
        network_config_.coord_epoch =
            jval<std::string>(json, "network.coord_epoch", "coord_epoch", std::string{});
        network_config_.use_sif_coordinates =
            jval<bool>(json, "network.use_sif_coordinates", "use_sif_coordinates", false);
        network_config_.coord_age_warn_years =
            jval<double>(json, "network.coord_age_warn_years", "coord_age_warn_years", 0.0);

        // Run-date hint for SIF velocity propagation and the coordinate-age
        // advisory: the data_download {year, doy} pair (loadDataDownloadSection_
        // has already run), re-used as the generic date hint exactly as
        // PathResolver does under data_source=internal.  0/0 = no hint.
        std::optional<double> run_year;
        if (download_config_.year > 0 && download_config_.doy > 0) {
            const int  y    = download_config_.year;
            const bool leap = (y % 4 == 0 && y % 100 != 0) || (y % 400 == 0);
            run_year = static_cast<double>(y)
                     + (static_cast<double>(download_config_.doy) - 0.5) / (leap ? 366.0 : 365.0);
        }

        std::size_t n_sif_resolved = 0;
        for (auto& s : network_config_.stations) {
            if (s.name.empty()) {
                log->error("Network: every station needs a non-empty 'name'");
                return false;
            }
            // An empty rinex_path is tolerated (PathResolver fills it from the
            // internal tree by station name); a known position is required (the
            // network holds coordinates fixed, so it cannot bootstrap them).
            // RTA-12: under network.use_sif_coordinates a missing position_ecef
            // may be resolved from the loaded SIF registry — the ONLY relaxation
            // of the hard error; explicit JSON ECEF always wins (this branch is
            // never entered for it), and with the toggle off the pre-RTA-12 hard
            // error below is unchanged.
            if (!s.has_position && network_config_.use_sif_coordinates) {
                if (!station_info_) {
                    log->error("Network: station '{}' has no position_ecef and "
                               "network.use_sif_coordinates is on, but no SIF registry is "
                               "loaded — arm processing.enable_station_info (+ "
                               "files.station_info_path/s) or supply position_ecef.",
                               s.name);
                    return false;
                }
                if (!run_year) {
                    log->error("Network: station '{}' has no position_ecef and SIF "
                               "velocity propagation needs a run date — set "
                               "data_download.year/doy or supply position_ecef.",
                               s.name);
                    return false;
                }
                const auto rc = resolveRosterCoordinate(*station_info_, s.name, *run_year);
                if (!rc) {
                    log->error("Network: station '{}' has no position_ecef and could not "
                               "be resolved from the SIF registry (unknown station or no "
                               "usable occupation at {:.4f}) — supply position_ecef.",
                               s.name, *run_year);
                    return false;
                }
                s.position_ecef     = { rc->ecef.x(), rc->ecef.y(), rc->ecef.z() };
                s.has_position      = true;
                s.position_from_sif = true;
                ++n_sif_resolved;
            }
            if (!s.has_position) {
                log->error("Network: station '{}' needs a known 'position_ecef' (held fixed)", s.name);
                return false;
            }
        }
        if (network_config_.use_sif_coordinates) {
            // Default the declared coordinate epoch to the propagation date when the
            // user left it empty and any coordinate was actually SIF-propagated —
            // the S-contract cepoch= field then states the epoch the roster is
            // really valid at.  An explicit network.coord_epoch always wins.
            if (n_sif_resolved > 0 && network_config_.coord_epoch.empty()) {
                network_config_.coord_epoch = fmt::format("{:.4f}", *run_year);
                log->info("Network (RTA-12): network.coord_epoch defaulted to the SIF "
                          "propagation date {} (decimal year).", network_config_.coord_epoch);
            }
            // Per-station provenance, once at roster build (toggle-gated so a
            // default run's log is unchanged).
            for (const auto& s : network_config_.stations)
                log->info("Network roster: station '{}' coordinates {} "
                          "({:.4f}, {:.4f}, {:.4f}) m",
                          s.name,
                          s.position_from_sif
                              ? fmt::format("SIF-propagated to {:.4f}", *run_year)
                              : "json-explicit",
                          s.position_ecef[0], s.position_ecef[1], s.position_ecef[2]);
        }
        // RTA-12(4): cheap coordinate-age advisory.  Plate motion is ~2-7 cm/yr, so
        // fixed roster coordinates more than coord_age_warn_years beyond their
        // declared epoch should be re-propagated before they poison a cm-level
        // product datum.  Advisory ONLY (single WARN) — datum-event machinery
        // (automatic re-propagation / product re-basing on a coordinate update) is
        // an RTF-scale follow-on, deliberately out of this card's scope.
        if (network_config_.coord_age_warn_years > 0.0
            && !network_config_.coord_epoch.empty() && run_year) {
            const auto epoch_year = parseCoordEpochYear(network_config_.coord_epoch);
            if (!epoch_year) {
                log->warn("Network: network.coord_epoch '{}' is not parseable (decimal "
                          "year or YYYY-MM-DD) — coordinate-age advisory skipped.",
                          network_config_.coord_epoch);
            } else if (*run_year - *epoch_year > network_config_.coord_age_warn_years) {
                log->warn("Network: roster coordinate epoch {} is {:.2f} years before the "
                          "run date ({:.4f}), beyond network.coord_age_warn_years = {:.2f} "
                          "— plate motion is ~2-7 cm/yr; re-propagate the station "
                          "coordinates (or arm network.use_sif_coordinates).",
                          network_config_.coord_epoch, *run_year - *epoch_year,
                          *run_year, network_config_.coord_age_warn_years);
            }
        }

        // --- Datum fields ---
        network_config_.clock_datum_station =
            jval<std::string>(json, "network.clock_datum_station", "clock_datum_station", std::string{});
        network_config_.clock_datum_satellite =
            jval<std::string>(json, "network.clock_datum_satellite", "clock_datum_satellite", std::string{});
        network_config_.epoch_sync_tolerance_s =
            jval<double>(json, "network.epoch_sync_tolerance_s", "epoch_sync_tolerance_s", 0.01);

        // --- Producer-side network AR (network.ambiguity_resolution.*) ---
        // Integer-leveled satellite phase biases so a held-out user sees an
        // integer-consistent bias surface (docs/network_ambiguity_resolution_design.md).
        // Default enable=false => float product bit-identical until armed.
        {
            auto& ar = network_config_.ambiguity_resolution;
            ar.enable = jval<bool>(json, "network.ambiguity_resolution.enable",
                                   "network_ar_enable", false);
            ar.min_success_rate = jval<double>(json, "network.ambiguity_resolution.min_success_rate",
                                               "network_ar_min_success_rate", 0.9999);
            ar.ratio_threshold = jval<double>(json, "network.ambiguity_resolution.ratio_threshold",
                                              "network_ar_ratio_threshold", 3.0);
            ar.ratio_upper_bound = jval<double>(json, "network.ambiguity_resolution.ratio_upper_bound",
                                                "network_ar_ratio_upper_bound", 1e4);
            ar.nl_max_per_pair_frac = jval<double>(json, "network.ambiguity_resolution.nl_max_per_pair_frac",
                                                   "network_ar_nl_max_per_pair_frac", 0.10);
            ar.enable_partial_ar = jval<bool>(json, "network.ambiguity_resolution.enable_partial_ar",
                                              "network_ar_enable_partial_ar", true);
            ar.wl_cascade = jval<bool>(json, "network.ambiguity_resolution.wl_cascade",
                                       "network_ar_wl_cascade", false);
            ar.min_epochs = jval<int>(json, "network.ambiguity_resolution.min_epochs",
                                      "network_ar_min_epochs", 10);
            if (ar.enable) {
                log->info("Network producer AR ON — integer-leveled satellite phase biases "
                          "(FFR min_success_rate={}, ratio>={}, nl_frac<={} cyc, partial_ar={}, "
                          "wl_cascade={}, min_epochs={}).  GPS+Galileo only; GLONASS stays float.",
                          ar.min_success_rate, ar.ratio_threshold, ar.nl_max_per_pair_frac,
                          ar.enable_partial_ar ? "on" : "off", ar.wl_cascade ? "on" : "off",
                          ar.min_epochs);
            }
        }

        network_config_.product_output_path =
            jval<std::string>(json, "network.product_output_path", "product_output_path", std::string{});
        // RTA-6: per-epoch product-file flush for a live tail consumer.  Flushing
        // changes no bytes (OFF run byte-identical); default false per the
        // default-off toggle discipline.
        network_config_.product_flush_per_epoch =
            jval<bool>(json, "network.product_flush_per_epoch", "product_flush_per_epoch", false);
        if (network_config_.product_flush_per_epoch) {
            if (network_config_.product_output_path.empty())
                log->warn("Network: network.product_flush_per_epoch is set but "
                          "network.product_output_path is empty — no product file is "
                          "written, so the knob is inert.");
            else
                log->info("Network producer (RTA-6): per-epoch product-file flush ON — a "
                          "live tail consumer sees each epoch block as soon as it is "
                          "emitted (no byte change; ordering/latency only).");
        }

        // --- Per-station compute parallelism (geometry + measurement assembly) ---
        network_config_.parallel_stations =
            jval<bool>(json, "network.parallel_stations", "parallel_stations", true);
        network_config_.parallel_min_stations =
            jval<int>(json, "network.parallel_min_stations", "parallel_min_stations", 10);
        network_config_.parallel_max_threads =
            jval<int>(json, "network.parallel_max_threads", "parallel_max_threads", 0);
        network_config_.parallel_reduce =
            jval<bool>(json, "network.parallel_reduce", "parallel_reduce", false);
        network_config_.parallel_seam =
            jval<bool>(json, "network.parallel_seam", "parallel_seam", false);

        // --- Visualization traces (pyviz network viewers) ---
        network_config_.trace_stations =
            jval<bool>(json, "network.trace_stations", "trace_stations", true);
        network_config_.trace_ionosphere =
            jval<bool>(json, "network.trace_ionosphere", "trace_ionosphere", false);
        network_config_.trace_estimability =
            jval<bool>(json, "network.trace_estimability", "trace_estimability", false);
        network_config_.trace_slip_events =
            jval<bool>(json, "network.trace_slip_events", "trace_slip_events", false);
        network_config_.trace_slip_raw =
            jval<bool>(json, "network.trace_slip_raw", "trace_slip_raw", false);
        network_config_.trace_clock_full =
            jval<bool>(json, "network.trace_clock_full", "trace_clock_full", false);
        network_config_.trace_anchor_info =
            jval<bool>(json, "network.trace_anchor_info", "trace_anchor_info", false);
        network_config_.trace_dd_fractional =
            jval<bool>(json, "network.trace_dd_fractional", "trace_dd_fractional", false);
        // T2b: cap DD pairs emitted/covariance-propagated per epoch (0 = unlimited).
        network_config_.trace_dd_fractional_max_pairs =
            jval<int>(json, "network.trace_dd_fractional_max_pairs",
                      "trace_dd_fractional_max_pairs", 0);
        if (network_config_.trace_dd_fractional_max_pairs < 0)
            network_config_.trace_dd_fractional_max_pairs = 0;
        // T2e: DD-trace stride (trace-only) + epoch decimation (estimation-relevant).
        network_config_.trace_dd_fractional_epoch_stride =
            jval<int>(json, "network.trace_dd_fractional_epoch_stride",
                      "trace_dd_fractional_epoch_stride", 1);
        if (network_config_.trace_dd_fractional_epoch_stride < 1)
            network_config_.trace_dd_fractional_epoch_stride = 1;
        network_config_.epoch_decimation_n =
            jval<int>(json, "network.epoch_decimation_n", "epoch_decimation_n", 1);
        if (network_config_.epoch_decimation_n < 1)
            network_config_.epoch_decimation_n = 1;
        // T3f: RSS the SH formal σ + floor + stale-OSB skip on the closed-loop iono supply.
        network_config_.iono_supply_formal_sigma =
            jval<bool>(json, "network.iono_supply_formal_sigma",
                       "iono_supply_formal_sigma", false);
        network_config_.trace_flag_rates =
            jval<bool>(json, "network.trace_flag_rates", "trace_flag_rates", false);
        network_config_.trace_epoch_status =
            jval<bool>(json, "network.trace_epoch_status", "trace_epoch_status", false);
        // D0: per-epoch section-timer breakdown of the network solve (diagnostic; byte-identical OFF).
        network_config_.trace_section_timers =
            jval<bool>(json, "network.trace_section_timers", "trace_section_timers", false);
        network_config_.health_interval_s =
            jval<double>(json, "network.health_interval_s", "health_interval_s", 0.0);
        if (network_config_.health_interval_s < 0.0) {
            log->warn("Network: network.health_interval_s {} < 0 clamped to 0 (no operator "
                      "health heartbeat).", network_config_.health_interval_s);
            network_config_.health_interval_s = 0.0;
        }
        // P1: size-based rotation of the unbounded service CSVs (network_trace /
        // network_solution / network_service_health).  Default 0 = off ⇒ no
        // close/rename ever happens (byte-identical).
        network_config_.trace_rotate_mb =
            jval<double>(json, "network.trace_rotate_mb", "trace_rotate_mb", 0.0);
        network_config_.trace_rotate_keep =
            jval<int>(json, "network.trace_rotate_keep", "trace_rotate_keep", 3);
        if (network_config_.trace_rotate_mb < 0.0) {
            log->warn("Network: network.trace_rotate_mb {} < 0 clamped to 0 (trace rotation "
                      "off).", network_config_.trace_rotate_mb);
            network_config_.trace_rotate_mb = 0.0;
        }
        if (network_config_.trace_rotate_keep < 1) {
            log->warn("Network: network.trace_rotate_keep {} < 1 clamped to 1 (at least one "
                      "rolled generation is kept when rotation is on).",
                      network_config_.trace_rotate_keep);
            network_config_.trace_rotate_keep = 1;
        }
        if (network_config_.trace_rotate_mb > 0.0)
            log->info("Network (P1): trace-CSV rotation ON — network_trace.csv / "
                      "network_solution.csv / network_service_health.csv roll at {:.1f} MiB, "
                      "keeping {} rolled generation(s) (.1 oldest-shifted, header re-written; "
                      "rotated suffixes are NOT auto-discovered by the pyviz viewers).",
                      network_config_.trace_rotate_mb, network_config_.trace_rotate_keep);
        network_config_.warmup_ura_floor_m =
            jval<double>(json, "network.warmup_ura_floor_m", "warmup_ura_floor_m", 0.0);
        network_config_.warmup_epochs =
            jval<int>(json, "network.warmup_epochs", "warmup_epochs", 0);
        // RTB-7(a): producer state snapshot + warm resume.  Path empty (default)
        // = OFF, byte-identical; the interval is inert without the path.
        network_config_.state_snapshot_path =
            jval<std::string>(json, "network.state_snapshot_path", "state_snapshot_path",
                              std::string{});
        network_config_.state_snapshot_interval_s =
            jval<double>(json, "network.state_snapshot_interval_s",
                         "state_snapshot_interval_s", 10.0);
        if (network_config_.state_snapshot_interval_s <= 0.0) {
            log->warn("Network: network.state_snapshot_interval_s {} must be > 0; clamped to "
                      "the default 10 s.", network_config_.state_snapshot_interval_s);
            network_config_.state_snapshot_interval_s = 10.0;
        }
        if (!network_config_.state_snapshot_path.empty())
            log->info("Network (RTB-7): producer state snapshot ON — atomic rewrite of '{}' "
                      "every {:.0f} s of product time (driver carries/oscillators/pivots/"
                      "pending bumps + emitter discontinuity state).  At startup a fresh, "
                      "config-matching snapshot WARM-resumes (generation id preserved, no "
                      "consumer reset); a stale/mismatched/corrupt one cold-starts with a "
                      "new generation id.",
                      network_config_.state_snapshot_path,
                      network_config_.state_snapshot_interval_s);
        network_config_.trace_events =
            jval<bool>(json, "network.trace_events", "trace_events", false);
        if (network_config_.trace_events)
            log->info("Network: per-event QC trace ENABLED — network_events.csv logs every "
                      "tiered QC action (T1-T6, GATE/SIGMA0/PIVOT/RETRO) in long format.");
        network_config_.trace_zwd =
            jval<bool>(json, "network.trace_zwd", "trace_zwd", false);
        if (network_config_.trace_zwd)
            log->info("Network: per-station ZWD trace ENABLED — network_zwd.csv logs each "
                      "station's estimated ZWD + datum-conditioned formal sigma each emitted epoch.");
        network_config_.trace_station_residuals =
            jval<bool>(json, "network.trace_station_residuals", "trace_station_residuals", false);
        network_config_.write_run_manifest =
            jval<bool>(json, "network.write_run_manifest", "write_run_manifest", false);
        if (network_config_.write_run_manifest)
            log->info("Network: run-provenance manifest ENABLED — network_run_manifest.json "
                      "records the config hash, build stamp, and full toggle set (JSONL).");
        network_config_.trace_ssr_consumer =
            jval<bool>(json, "network.trace_ssr_consumer", "trace_ssr_consumer", false);
        if (network_config_.trace_ssr_consumer)
            log->info("Network: consumer-side SSR trace ENABLED — the SSR consumer writes "
                      "ssr_consumer_trace.csv (age, clk-applied, URA R-inflation, disc counters).");
        network_config_.serialize_sbasis_contract =
            jval<bool>(json, "network.serialize_sbasis_contract", "serialize_sbasis_contract", false);
        if (network_config_.serialize_sbasis_contract)
            log->info("Network: S-basis contract serialization ENABLED — product header carries the "
                      "S-basis declaration, a-priori orbit/clock product identity, and phase-bias "
                      "discontinuity threshold (v2 'S' line) for consumer cross-check.");

        if (network_config_.clock_datum_station.empty()) {
            network_config_.clock_datum_station = network_config_.stations.front().name;
            log->info("Network: clock_datum_station unset; defaulting to first station '{}'",
                      network_config_.clock_datum_station);
        } else {
            bool found = false;
            for (const auto& s : network_config_.stations)
                if (s.name == network_config_.clock_datum_station) { found = true; break; }
            if (!found) {
                log->error("Network: clock_datum_station '{}' not found among stations",
                           network_config_.clock_datum_station);
                return false;
            }
        }

        // --- Force the shared satellite-state estimation for V1 (network producer) ---
        // V1 scope = estimable satellite CLOCK + PHASE BIAS only (the integer-
        // recovery essentials).  Satellite CODE bias is DEFERRED to W6: estimating
        // it would add the IF-code-bias lump as a SECOND estimable place (it is
        // already absorbed into the estimable clock), needing an extra code-bias
        // datum pin.  So force clock + phase-bias ON and leave code-bias OFF —
        // the satellite code delay then re-partitions into the per-station rx code
        // bias + local slant iono (the existing L1-referenced iono+DCB convention;
        // the emitted clock is code-blind / phase-consistent).  loadEstimation-
        // Section_ runs earlier, so this overrides the JSON estimation block.
        estimate_sat_clocks_       = true;
        estimate_sat_phase_biases_ = true;
        estimate_sat_biases_       = false;  // V1 default: defer satellite code bias (W6 below re-enables)
        promote_sat_code_bias_     = false;  // V1 default: defer satellite code bias (W6 below re-enables)
        log->info("Network producer: forcing estimate_sat_clocks + estimate_sat_phase_biases = true");

        // --- W6: satellite CODE-bias (OSB) estimation -----------------------------
        // When network.estimate_sat_code_bias is on, the network estimates its OWN
        // per-(satellite,signal) code biases (GPS + Galileo) and emits them, so the
        // user can SKIP the external .BIA.  This is a single coupled slice — the
        // satellite code-bias column, the receiver DCB (estimate_ifb), and the GIM
        // ionosphere weighting (the load-bearing constraint that makes the satellite
        // DCB observable against the per-(rx,sat) slant iono) must ALL be present or
        // the shared null-space rank gate withholds every epoch.  Forcing them here
        // keeps them in lockstep behind the one flag.
        estimate_network_sat_code_bias_ = jval<bool>(
            json, "network.estimate_sat_code_bias", "estimate_sat_code_bias", false);

        // Co-estimated global SH-VTEC ionosphere.  This REPLACES the external GIM as
        // the iono constraint that makes the satellite DCB observable, so it implies
        // — and forces on — the whole W6 satellite code-bias slice.  Setting it here
        // (before the W6 chain below) lets that chain fire unchanged; the driver then
        // uses the SH row builder instead of the external-GIM one and does NOT load a
        // GIM (the two iono sources are never double-applied).
        estimate_network_iono_sh_ = jval<bool>(
            json, "network.estimate_iono_sh", "estimate_iono_sh", false);
        if (estimate_network_iono_sh_) {
            estimate_network_sat_code_bias_ = true;   // SH co-estimation is the W6 iono constraint
            global_vtec_sh_.enabled         = true;
        }

        if (estimate_network_sat_code_bias_) {
            estimate_sat_biases_   = true;   // promote the satellite code bias to a state
            promote_sat_code_bias_ = true;   // estimate it on every raw code row (not fallback-only)
            estimate_ifb_          = true;   // receiver DCB/IFB — an explicit rx code-bias state on the
                                             // undifferenced rows, or the satellite OSB absorbs it
            // GIM-weighted iono constraint: the satellite DCB is co-linear with the
            // free per-(rx,sat) slant iono until the GIM imposes between-satellite
            // structure (common-mode-free R leaves the per-constellation level free,
            // pinned by the explicit sum-to-zero DCB datum).  runNetwork hard-errors
            // if the GIM is absent at startup (the slice would be inert without it).
            iono_weighting_.enabled = true;
            // Estimate the FULL satellite code bias from scratch in the network's own
            // datum — NOT a residual to .BIA.  Skip the external .BIA on both producer
            // and (via the derived consumer config) the held-out consumer, so the
            // network OSB is the single code-bias source and nothing double-corrects.
            model_corrections_.enable_code_phase_bias = false;
            // The producer never needs the phase-clock split for V1; keep it off so it
            // cannot add a spurious gauge to the shared system.
            estimate_phase_clock_ = false;
            log->info("Network producer (W6): satellite code-bias estimation ENABLED — forcing "
                      "estimate_sat_biases + promote_sat_code_bias + estimate_ifb (rx DCB) + "
                      "ionosphere_weighting = on, enable_code_phase_bias (.BIA) = OFF; the network "
                      "emits its own satellite code biases (GPS+Galileo) under a sum-to-zero "
                      "per-constellation DCB datum.");
        } else {
            log->info("Network producer (V1): satellite code bias deferred "
                      "(estimate_sat_biases / promote_sat_code_bias = false)");
        }

        if (estimate_network_iono_sh_) {
            log->info("Network producer (SH-iono): co-estimating the global single-layer "
                      "SH-VTEC ionosphere (degree {}, shell {:.0f} km, {}) jointly with the "
                      "satellite code biases — the network's OWN iono constraint REPLACES the "
                      "external GIM (.INX is NOT loaded; no double-apply). Every coefficient "
                      "carries a Kaula prior (a₀₀ loose: {:.0f}±{:.0f} TECU) so the null-space "
                      "rank gate stays at #constellations regardless of coverage.",
                      global_vtec_sh_.max_degree, global_vtec_sh_.layer_height_km,
                      global_vtec_sh_.sun_fixed ? "sun-fixed" : "geographic",
                      global_vtec_sh_.mean_vtec_prior_tecu,
                      global_vtec_sh_.mean_vtec_prior_sigma_tecu);
        }

        // --- RTF-1/RTF-2: producer a-priori orbit/clock source -------------------
        // network.apriori_source selects what the producer's per-satellite
        // a-priori clock (and the orbit its geometry uses) is referenced to.  The
        // default "sp3" is today's behaviour (precise SP3/CLK identity) and is
        // byte-identical everywhere.  "brdc_rts" references the a-priori to the
        // broadcast ephemeris (+ external IGS-SSR 4076 orbits) — the
        // self-sufficient real-time service mode (docs/rt_service_conventions.md
        // §1).  Parse-only in this card; the producer core path (nav load,
        // phase-gate suppression, anchor re-base, C0 delta-to-broadcast) lands in
        // later RTF-1 cards.  Mirrors the receiver.troposphere_apriori_source
        // string-match pattern (own local to_lower — the receiver-section lambda
        // is out of scope here).  Placed AFTER the estimate_iono_sh coupling above
        // so the brdc_rts validation sees final flag values.
        {
            auto to_lower = [](std::string s) {
                std::transform(s.begin(), s.end(), s.begin(),
                               [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
                return s;
            };
            const std::string src = to_lower(jval<std::string>(
                json, "network.apriori_source", "apriori_source", std::string{"sp3"}));
            if (src == "brdc_rts") {
                network_apriori_source_ = NetworkAprioriSource::BrdcRts;
            } else {
                if (src != "sp3")
                    log->warn("Unknown network.apriori_source='{}', falling back to 'sp3' "
                              "(precise SP3/CLK a-priori).", src);
                network_apriori_source_ = NetworkAprioriSource::Sp3;
            }
        }
        // D6: a-priori clock anchor σ (m); 0 = auto (mode-resolved at the
        // producer), an explicit positive value wins.  Parse+store only here.
        network_apriori_clock_anchor_sigma_m_ = jval<double>(
            json, "network.apriori_clock_anchor_sigma_m", "apriori_clock_anchor_sigma_m", 0.0);
        if (network_apriori_clock_anchor_sigma_m_ < 0.0) {
            log->warn("Network: network.apriori_clock_anchor_sigma_m {} < 0 is meaningless; "
                      "clamped to 0 (auto).", network_apriori_clock_anchor_sigma_m_);
            network_apriori_clock_anchor_sigma_m_ = 0.0;
        }

        if (network_apriori_source_ == NetworkAprioriSource::BrdcRts) {
            // Validation 1 (hard error): the satellite code-bias slice (W6) needs
            // an iono constraint that makes the satellite DCB observable.  In
            // brdc_rts that constraint MUST be the co-estimated SH-VTEC field —
            // the external GIM path is post-processing-only (a daily product; see
            // Phase 3b GIM/SH exclusivity in ApplicationNetwork + conventions §1).
            // estimate_iono_sh forces estimate_sat_code_bias on above, so the only
            // illegal combination is sat-code-bias ON with iono_sh OFF (the GIM
            // path).
            if (estimate_network_sat_code_bias_ && !estimate_network_iono_sh_) {
                log->error("Network: network.apriori_source = brdc_rts with "
                           "network.estimate_sat_code_bias but NOT network.estimate_iono_sh — "
                           "the external GIM iono constraint that makes the satellite code bias "
                           "observable is post-processing-only, so the real-time producer has no "
                           "iono constraint for the code-bias slice.  Set network.estimate_iono_sh "
                           "= true (co-estimate the SH-VTEC field) or drop "
                           "network.estimate_sat_code_bias.");
                return false;
            }
            // Validation 2 (warn — no per-mount dialect declaration exists yet,
            // RTF-3): the producer's SSR ORBIT corrections must be IGS-SSR 4076
            // (CoM); legacy 1240-1270 (APC) mountpoints are user-engine-only
            // (RTB-5, honor_ssr_reference_point).  Dialect is decoded from the
            // message type at runtime, not declared in config, so this cannot be a
            // load-time hard error yet — downgrade to a warn per the plan.
            if (realtime_config_.corrections.enabled) {
                log->warn("Network: network.apriori_source = brdc_rts with a realtime "
                          "corrections stream configured — the producer's SSR ORBIT corrections "
                          "must be IGS-SSR 4076 (CoM); legacy 1240-1270 (APC) dialect is NOT "
                          "supported for the producer and is not validated at load (no per-mount "
                          "dialect declaration — RTF-3).  Ensure the corrections mountpoint "
                          "streams 4076.");
            }
            log->info("Network producer (RTF-1): a-priori source = broadcast + external RTS "
                      "orbits (brdc_rts) — the emitted clock C0 becomes a delta against "
                      "broadcast(IODE) + the applied RTS orbit; a-priori clock anchor σ = {} m "
                      "(0 = auto → broadcast grade).  NOTE: the producer core path (RINEX-nav "
                      "load, broadcast-phase-gate suppression, anchor re-base) lands in later "
                      "RTF-1 cards — this card is config plumbing only.",
                      network_apriori_clock_anchor_sigma_m_);
        }

        // --- GLONASS in the network (code-only: sat clock + per-FCN-slot rx IFB) ---
        // FDMA makes a single shared per-(sat,signal) satellite OSB unphysical, so
        // GLONASS contributes a per-satellite CLOCK only.  The receiver group delay
        // is carried by the per-FCN-slot receiver IFB as a LOCAL nuisance (Schur-
        // eliminated, never emitted), and NO GLONASS satellite phase bias is
        // produced.  Admitting GLONASS therefore REQUIRES the per-slot IFB
        // machinery: force its prerequisites on, and require GLONASS in
        // active_systems (otherwise GLONASS rows would build with no shared clock
        // column and pin the receiver clock, corrupting the GPS/Galileo products).
        enable_glonass_network_ = jval<bool>(json, "network.estimate_glonass",
                                             "estimate_glonass", false);
        if (enable_glonass_network_) {
            const bool has_glonass =
                std::find(active_systems_.begin(), active_systems_.end(), "GLONASS")
                != active_systems_.end();
            if (!has_glonass) {
                log->error("Network: network.estimate_glonass=true but 'GLONASS' is not in "
                           "processing.active_systems.  Add GLONASS to active_systems AND give it "
                           "a processing.signal_priorities entry, or set estimate_glonass=false.");
                return false;
            }
            // A GLONASS signal_priorities entry is the OTHER half of admitting it:
            // without one, PppModel::decideConstellationCanonicals finds no priority
            // string and skips every GLONASS satellite at signal selection, so the
            // network would silently emit ZERO GLONASS products (no clock, no per-slot
            // IFB ever allocated) while the GPS/Galileo solution looks nominal — the
            // documented multi-GNSS signal_priorities silent-drop failure mode.  Fail
            // loud here so the misconfig is caught at load, matching the error string
            // above that already instructs the user to add the entry.
            if (signal_priorities_.find("GLONASS") == signal_priorities_.end()) {
                log->error("Network: network.estimate_glonass=true but 'GLONASS' has no "
                           "processing.signal_priorities entry.  Without it every GLONASS "
                           "observation is dropped at signal selection (a silent zero-emit of "
                           "GLONASS products).  Add a GLONASS signal_priorities entry (e.g. "
                           "\"GLONASS\": \"CP\" for the FDMA G1/G2 bands), or set "
                           "estimate_glonass=false.");
                return false;
            }
            if (!uncombined_enabled_) {
                log->error("Network: network.estimate_glonass=true requires uncombined PPP-RTK "
                           "(the per-FCN-slot IFB is an uncombined receiver-bias state).");
                return false;
            }
            estimate_ifb_       = true;   // enables the receiver-bias allocation loop
            enable_glonass_ifb_ = true;   // enables the GLONASS per-FCN-slot resolver branch
            log->info("Network producer: GLONASS ENABLED (code-only) — forcing estimate_ifb + "
                      "enable_glonass_ifb = true; emitting a GLONASS satellite clock with the "
                      "per-FCN-slot receiver IFB as a local nuisance (no GLONASS satellite phase bias).");
        }

        // GLONASS + satellite code-bias are not jointly supported in V1: admitting
        // GLONASS forces the receiver-bias allocation to skip GPS/Galileo IFB (only
        // the GLONASS per-slot IFB is created — see MeasurementModel's network alloc
        // gate), but the satellite code-bias separation REQUIRES an explicit GPS/
        // Galileo receiver DCB on the undifferenced rows.  Fail loud rather than
        // silently produce an unidentifiable system.
        if (estimate_network_sat_code_bias_ && enable_glonass_network_) {
            log->error("Network: network.estimate_sat_code_bias and network.estimate_glonass cannot "
                       "both be on in V1 — GLONASS admission skips the GPS/Galileo receiver DCB that "
                       "the satellite code-bias estimation needs.  Disable one of them.");
            return false;
        }

        // --- Phase-leveling: carry per-station ambiguities across epochs ---------
        // Move the producer from a per-epoch ABSOLUTE snapshot solve to one that
        // carries each non-pivot station's float ambiguities as a diagonal prior on
        // its own local block, so carrier-phase precision (not just code) sharpens
        // the satellite clock/phase-bias/code-bias products.  Additive and purely
        // local: OFF is byte-identical to today (the bisection anchor for the whole
        // feature).  q_amb / warmup / max_gap are heuristics flagged for tuning.
        network_phase_leveling_ =
            jval<bool>(json, "network.phase_leveling", "phase_leveling", false);
        network_phase_leveling_q_amb_ =
            jval<double>(json, "network.phase_leveling_q_amb", "phase_leveling_q_amb", 0.0);
        network_phase_leveling_warmup_epochs_ =
            jval<int>(json, "network.phase_leveling_warmup_epochs", "phase_leveling_warmup_epochs", 6);
        network_phase_leveling_max_gap_s_ =
            jval<double>(json, "network.phase_leveling_max_gap_s", "phase_leveling_max_gap_s", 120.0);
        network_phase_leveling_var_floor_m2_ =
            jval<double>(json, "network.phase_leveling_var_floor_m2", "phase_leveling_var_floor_m2", 0.0);
        network_shared_carry_var_floor_pb_m2_ =
            jval<double>(json, "network.shared_carry_var_floor_pb_m2", "shared_carry_var_floor_pb_m2", 0.0);
        // T1d: convergence-gated leveling-carry warmup (default OFF = fixed epoch count).
        network_phase_leveling_warmup_on_convergence_ =
            jval<bool>(json, "network.phase_leveling_warmup_on_convergence",
                       "phase_leveling_warmup_on_convergence", false);
        network_phase_leveling_refloat_at_convergence_ =
            jval<bool>(json, "network.phase_leveling_refloat_at_convergence",
                       "phase_leveling_refloat_at_convergence", false);
        network_phase_leveling_conv_rms_thresh_tecu_ =
            jval<double>(json, "network.phase_leveling_conv_rms_thresh_tecu",
                         "phase_leveling_conv_rms_thresh_tecu", 0.5);
        network_shared_carry_var_floor_cb_m2_ =
            jval<double>(json, "network.shared_carry_var_floor_cb_m2", "shared_carry_var_floor_cb_m2", 0.0);
        // C1: full dense cross-covariance carry of the shared border (information-form
        // KF on the border; recovers the clock↔phase-bias off-diagonals the diagonal
        // D2 stash discards).  Independent of phase_leveling — it acts on the SHARED
        // D2 carry, not the per-station ambiguity carry — so parsed and announced here.
        network_shared_carry_full_cov_ =
            jval<bool>(json, "network.shared_carry_full_cov", "shared_carry_full_cov", false);
        if (network_shared_carry_full_cov_)
            log->info("Network producer (C1): FULL-COVARIANCE shared carry enabled — the D2 "
                      "cross-epoch prior folds the dense (P_prev + Q)⁻¹ block over the carried "
                      "clock/phase-bias/code-bias columns (information-form KF on the shared "
                      "border) instead of independent per-key diagonals.  Departed/pinned/"
                      "retired keys are marginalized; a non-PD carried block degrades to the "
                      "diagonal fold for that epoch (warned).");
        // Card 1: rigorous conditional local carry.  The persistent per-station locals
        // (phase-leveling ambiguities + ZWD) are carried as ONE pseudo-observation row
        // coupling each to the FREE shared border (regression M_r, pure conditional
        // variance (N_rr⁻¹)_{rr}) instead of an independent diagonal prior — this removes
        // the shared-information double count the diagonal carry incurs against the dense
        // border carry, so the sequential recursion equals the batch solve.  It is only
        // EXACT against a dense border carry, so hard-require shared_carry_full_cov.
        network_carry_conditional_coupling_ =
            jval<bool>(json, "network.carry_conditional_coupling", "carry_conditional_coupling", false);
        if (network_carry_conditional_coupling_ && !network_shared_carry_full_cov_)
            throw std::runtime_error(
                "network.carry_conditional_coupling requires network.shared_carry_full_cov = true "
                "(the conditional local carry is exact only against a dense shared-border carry).");
        if (network_carry_conditional_coupling_)
            log->info("Network producer (Card 1): CONDITIONAL local carry enabled — each carried "
                      "phase-leveling ambiguity / ZWD is injected as a pseudo-observation row "
                      "coupling it to the free shared border (coeff +1 on the local, regression "
                      "M_r on the border, variance = pure conditional (N_rr⁻¹)_rr), not an "
                      "independent diagonal prior.  Departed shared columns are marginalized into "
                      "the row variance.  Removes the shared-info double count → the recursion "
                      "equals the batch solve.");
        // Card 1b: JOINTLY whiten a station's coupling rows instead of marginalizing each
        // departed shared column per-row into a scalar variance — restores the between-row
        // (DD) correlation the diagonal marginalization discards (the T7 flat-σ bug).
        network_carry_coupling_joint_whiten_ =
            jval<bool>(json, "network.carry_coupling_joint_whiten",
                       "carry_coupling_joint_whiten", false);
        if (network_carry_coupling_joint_whiten_ && !network_carry_conditional_coupling_)
            throw std::runtime_error(
                "network.carry_coupling_joint_whiten requires network.carry_conditional_coupling = true "
                "(it only changes how that card's coupling-row noise is applied).");
        if (network_carry_coupling_joint_whiten_)
            log->info("Network producer (Card 1b): JOINT coupling-row whitening enabled — a "
                      "station's conditional coupling rows are built as a group with the full "
                      "joint covariance R = diag(var_eff) + M_w·diag(P_w)·M_wᵀ over the distinct "
                      "DEPARTED shared columns and LLᵀ-whitened, so their shared common-mode "
                      "(which cancels in any DD) is common-mode-free and the between-row DD "
                      "information is preserved instead of individually deweighted to nothing.");
        // Card 1d: border TIME UPDATE of the coupling rows at injection.  A harvested row
        // is exact at the harvest epoch only; the border then moves s⁺ = s + w, w~N(0,Q),
        // so re-independencing the row against s⁺ needs the per-column κ shrink
        // M' = (P+Q)⁻¹P·M and the MᵀQM-style variance inflation.  Card 1 applies neither
        // to layout-present columns — the un-handled middle case between "static border"
        // and its departed-column (q→∞) marginalization.
        network_carry_coupling_time_update_ =
            jval<bool>(json, "network.carry_coupling_time_update",
                       "carry_coupling_time_update", false);
        if (network_carry_coupling_time_update_ && !network_carry_conditional_coupling_)
            throw std::runtime_error(
                "network.carry_coupling_time_update requires network.carry_conditional_coupling = true "
                "(it only corrects how that card's coupling rows re-map to the moved border).");
        // Card 1d-SH: fully marginalize the coupling rows' SH-VTEC terms.  The SH border
        // is a mean-reverting GM (climatology-anchored), not the pure RW the κ update
        // models — shrink-carrying its terms compounds row weight against the anchor
        // (the ~4 h a₀₀ runaway in the 12 h low-floor runs).  q=∞/departed treatment
        // severs that feedback; conservative and always consistent.
        network_carry_coupling_sh_marginalize_ =
            jval<bool>(json, "network.carry_coupling_sh_marginalize",
                       "carry_coupling_sh_marginalize", false);
        if (network_carry_coupling_sh_marginalize_ && !network_carry_conditional_coupling_)
            throw std::runtime_error(
                "network.carry_coupling_sh_marginalize requires network.carry_conditional_coupling "
                "= true (it only changes how that card's SH coupling terms are treated).");
        if (network_carry_coupling_sh_marginalize_)
            log->info("Network producer (Card 1d-SH): SH-VTEC coupling terms fully MARGINALIZED "
                      "(q=∞ / departed treatment: coefficient dropped, M²·P_prev into the row "
                      "variance) — severs the ambiguity↔SH carry feedback (the GM-carried border "
                      "violates the κ update's random-walk model; shrink-carrying its terms "
                      "compounds row weight against the climatology anchor).");
        if (network_carry_coupling_time_update_)
            log->info("Network producer (Card 1d): coupling-row border TIME UPDATE enabled — "
                      "each PRESENT coupled column of every injected coupling row is shrunk by "
                      "κ_j = P_prev/(P_prev + q_j·Δt) (coefficient + RHS term) and the row "
                      "variance inflated by M²·κ_j·q_j·Δt, with the per-kind q ladder exactly "
                      "the dense border fold's (clock-monitor-reinit sats treated as q=∞ ⇒ full "
                      "marginalization).  Removes the stale-row over-assertion (the carried-DD "
                      "random-walk / flat-σ defect, Card 1c).");
        // ZWD random-walk carry (B9): carry per-station tropospheric ZWD across epochs as a
        // diagonal RW prior via the same addStationPrior path as the ambiguity carry.
        network_carry_zwd_ =
            jval<bool>(json, "network.carry_zwd", "carry_zwd", false);
        network_zwd_carry_q_ =
            jval<double>(json, "network.zwd_carry_q", "zwd_carry_q", 1e-9);
        // M2 (LS-VCE carry-q): the carry-row variance-component MONITOR.  A
        // calibration/honesty instrument — accumulates carry innovations and logs
        // σ̂ against the hand floors; NO feedback into the solve (v1).  Default OFF
        // keeps the accumulator + logging inert → byte-identical.
        network_carry_vce_ =
            jval<bool>(json, "network.carry_vce", "carry_vce", false);
        network_carry_vce_apply_ =
            jval<bool>(json, "network.carry_vce_apply", "carry_vce_apply", false);
        if (network_carry_vce_apply_ && !network_carry_vce_)
            throw std::runtime_error(
                "network.carry_vce_apply requires network.carry_vce = true "
                "(the apply path feeds back the estimator's σ̂, which only runs when the monitor does).");
        if (network_carry_vce_apply_)
            throw std::runtime_error(
                "network.carry_vce_apply (M2 v2 — feed σ̂ back into the carry stochastic model) is "
                "NOT IMPLEMENTED in this build; v1 is monitor-only.  Set network.carry_vce = true for "
                "the monitor and leave carry_vce_apply = false.");
        network_carry_vce_components_ =
            jval<std::string>(json, "network.carry_vce_components", "carry_vce_components", "pl");
        network_carry_vce_window_epochs_ =
            jval<int>(json, "network.carry_vce_window_epochs", "carry_vce_window_epochs", 60);
        if (network_carry_vce_ && network_carry_vce_window_epochs_ < 1) {
            log->warn("Network (M2/LS-VCE): network.carry_vce_window_epochs {} < 1 — treating as "
                      "whole-run accumulation (no forgetting).",
                      network_carry_vce_window_epochs_);
        }
        if (network_carry_vce_)
            log->info("Network producer (M2/LS-VCE): carry-row variance-component MONITOR enabled "
                      "(components='{}', window={} epochs) — per family per constellation the "
                      "post-fit carry innovations are folded into a residual-form LS-VCE estimator "
                      "and logged as σ̂ (Floor m² + Rate m²/s) against the hand floors.  No feedback "
                      "into the solve (calibration/honesty instrument; solution byte-identical).",
                      network_carry_vce_components_, network_carry_vce_window_epochs_);

        // Station-adaptive SH-VTEC-constraint deweight (invariant #6: default OFF /
        // byte-identical).  Relaxes the per-LOS-iono ↔ global SH field coupling per
        // station by a factor w_s ∈ (0,1] (σ² ×= 1/w_s) — the surgical alternative to
        // dropping a hostile (scintillation-belt) station: it keeps serving the shared
        // clocks / biases / datum / position while its per-LOS iono stops poisoning the
        // field and its own ambiguities.  v1 static list + v2 adaptive IGG gate.
        {
            IonoConstraintDeweightConfig& idw = iono_constraint_deweight_;
            idw.stations.clear();
            const auto& dw_arr = jsection(json, "network.iono_constraint_deweight_stations",
                                          "iono_constraint_deweight_stations");
            if (dw_arr.is_array())
                idw.stations = dw_arr.get<std::vector<std::string>>();
            idw.factor = jval<double>(json, "network.iono_constraint_deweight_factor",
                                      "iono_constraint_deweight_factor", 0.0);
            idw.adaptive = jval<bool>(json, "network.iono_constraint_deweight_adaptive",
                                      "iono_constraint_deweight_adaptive", false);
            idw.window_epochs = jval<double>(json, "network.iono_constraint_deweight_window_epochs",
                                             "iono_constraint_deweight_window_epochs", 60.0);
            idw.sigma_hi = jval<double>(json, "network.iono_constraint_deweight_sigma_hi",
                                        "iono_constraint_deweight_sigma_hi", 3.0);
            idw.sigma_lo = jval<double>(json, "network.iono_constraint_deweight_sigma_lo",
                                        "iono_constraint_deweight_sigma_lo", 2.0);
            idw.floor = jval<double>(json, "network.iono_constraint_deweight_floor",
                                     "iono_constraint_deweight_floor", 0.0);
            idw.rate_limit = jval<double>(json, "network.iono_constraint_deweight_rate_limit",
                                          "iono_constraint_deweight_rate_limit", 0.25);
            idw.min_epochs = jval<int>(json, "network.iono_constraint_deweight_min_epochs",
                                       "iono_constraint_deweight_min_epochs", 30);
            // Clamps: keep the mechanism well-posed (never NaN / never information-inflating).
            if (idw.factor < 0.0) idw.factor = 0.0;
            if (idw.factor > 1.0) idw.factor = 1.0;
            if (idw.floor < 0.0)  idw.floor = 0.0;
            if (idw.floor >= 1.0) idw.floor = 0.999;
            if (idw.window_epochs < 1.0) idw.window_epochs = 1.0;
            if (idw.rate_limit <= 0.0) idw.rate_limit = 1.0;   // 0 ⇒ no slew limit (snap)
            if (idw.min_epochs < 1) idw.min_epochs = 1;
            const bool dw_static_on = !idw.stations.empty() && idw.factor < 1.0;
            if (idw.adaptive && idw.sigma_lo >= idw.sigma_hi) {
                log->warn("Network (iono deweight): sigma_lo ({:.2f}) >= sigma_hi ({:.2f}) — "
                          "hysteresis band ill-formed; forcing sigma_lo = 0.5*sigma_hi.",
                          idw.sigma_lo, idw.sigma_hi);
                idw.sigma_lo = 0.5 * idw.sigma_hi;
            }
            if (dw_static_on)
                log->info("Network (iono deweight v1): {} station(s) statically deweighted at "
                          "w_s={:.3f} ({} — {} the SH field) — they still serve the shared "
                          "clocks/biases/datum/position.",
                          idw.stations.size(), idw.factor,
                          (idw.factor <= 0.0) ? "w_s=0, free-floating iono" : "partial info",
                          (idw.factor <= 0.0) ? "fully decoupled from"
                                              : "loosely coupled to");
            if (idw.adaptive)
                log->info("Network (iono deweight v2): adaptive IGG gate ON — engage a station when "
                          "its EWMA SH-misfit z-score (median/MAD across the network, half-life "
                          "{:.0f} ep, ≥{} clean samples) exceeds {:.2f}σ, release below {:.2f}σ; "
                          "ramp to w_s floor {:.3f}, slew ≤{:.3f}/epoch.",
                          idw.window_epochs, idw.min_epochs, idw.sigma_hi, idw.sigma_lo,
                          idw.floor, idw.rate_limit);
        }

        // T0 cycle-slip repair (Increment 2): only meaningful with the carry on.
        network_cycle_slip_turbo_edit_ =
            jval<bool>(json, "network.cycle_slip.turbo_edit", "network_cycle_slip_turbo_edit", false);
        network_cycle_slip_turbo_edit_frac_max_ =
            jval<double>(json, "network.cycle_slip.turbo_edit_frac_max",
                         "network_cycle_slip_turbo_edit_frac_max", 0.15);
        network_cycle_slip_turbo_edit_dn_max_ =
            jval<int>(json, "network.cycle_slip.turbo_edit_dn_max",
                      "network_cycle_slip_turbo_edit_dn_max", 50);
        network_cycle_slip_mw_detect_ =
            jval<bool>(json, "network.cycle_slip.mw_detect",
                       "network_cycle_slip_mw_detect", false);
        // IF-phase triple-difference slip DETECTION (this increment): between-
        // satellite, between-epoch ionosphere-free phase OMC.  Detection-only — a
        // flagged IF pair refloats BOTH band ambiguities (pending-reseed) so the
        // carry re-estimates them from data; no MW/GF confirm, no integer repair.
        // OFF is byte-identical; meaningful for products only with the carry on.
        network_cycle_slip_if_phase_td_ =
            jval<bool>(json, "network.cycle_slip.if_phase_td",
                       "network_cycle_slip_if_phase_td", false);
        network_cycle_slip_if_phase_td_k_mad_ =
            jval<double>(json, "network.cycle_slip.if_phase_td_k_mad",
                         "network_cycle_slip_if_phase_td_k_mad", 8.0);
        network_cycle_slip_if_phase_td_floor_frac_ =
            jval<double>(json, "network.cycle_slip.if_phase_td_floor_frac",
                         "network_cycle_slip_if_phase_td_floor_frac", 1.0);
        network_cycle_slip_if_phase_td_min_pool_ =
            jval<int>(json, "network.cycle_slip.if_phase_td_min_pool",
                      "network_cycle_slip_if_phase_td_min_pool", 30);
        network_cycle_slip_if_phase_td_ewma_alpha_ =
            jval<double>(json, "network.cycle_slip.if_phase_td_ewma_alpha",
                         "network_cycle_slip_if_phase_td_ewma_alpha", 0.15);
        network_cycle_slip_if_phase_td_shrink_kappa_ =
            jval<double>(json, "network.cycle_slip.if_phase_td_shrink_kappa",
                         "network_cycle_slip_if_phase_td_shrink_kappa", 20.0);
        // Carrying carrier-phase ambiguities as zero-process-noise random constants is only
        // sound if an un-screened cycle slip cannot freeze a wrong value into the shared clock:
        // a slip makes the post-slip phase self-consistent with the SLIPPED integer, so the
        // stiff carry does not fight it — it locks it in.  Carrying phase therefore REQUIRES at
        // least one pre-fit slip detector (mw_detect, if_phase_td, or the turbo_edit repair,
        // which also runs the MW detector).  If a config enables phase-leveling with none, arm
        // network.cycle_slip.mw_detect — the ROBUST default: the wide-lane + LLI detector needs
        // NO a-priori position (works even when a station sits far from its reference), whereas
        // IF-phase TD relies on accurate coordinates (fine in-network, where they are fixed at
        // near-truth, but not in general).  Both are ionosphere-free and valid at the 30 s
        // product rate (unlike the MW+GF integer REPAIR).  Mirrors the raim_within_epoch ⟹
        // raim+attribution auto-arm below.  IF-phase TD stays an explicit opt-in for the
        // complementary geometric-jump coverage where coordinates are trusted.
        if (network_phase_leveling_ && !network_cycle_slip_mw_detect_
            && !network_cycle_slip_if_phase_td_ && !network_cycle_slip_turbo_edit_) {
            log->warn("Network: network.phase_leveling carries carrier-phase ambiguities but no "
                      "pre-fit slip detector is enabled — an un-screened slip would freeze into "
                      "the shared clock.  Enabling network.cycle_slip.mw_detect (wide-lane + LLI, "
                      "needs no a-priori position).  Add network.cycle_slip.if_phase_td for "
                      "complementary geometric-jump coverage where coordinates are trusted.");
            network_cycle_slip_mw_detect_ = true;
        }
        // Post-fit network RAIM (Increment 3, T2): the safety net for the zero-q carry.
        network_raim_ =
            jval<bool>(json, "network.raim", "network_raim", false);
        network_raim_k_sigma_ =
            jval<double>(json, "network.raim_k_sigma", "network_raim_k_sigma", 6.0);
        network_raim_sigma0_floor_ =
            jval<double>(json, "network.raim_sigma0_floor", "network_raim_sigma0_floor", 1.0);
        network_raim_min_stations_ =
            jval<int>(json, "network.raim_min_stations", "network_raim_min_stations", 3);
        network_raim_arc_persist_epochs_ =
            jval<int>(json, "network.raim_arc_persist_epochs", "network_raim_arc_persist_epochs", 1);
        // T2d: isolated-arc refloat maturity gate (default OFF = byte-identical).
        network_raim_refloat_maturity_gate_ =
            jval<bool>(json, "network.raim_refloat_maturity_gate",
                       "network_raim_refloat_maturity_gate", false);
        network_raim_refloat_maturity_epochs_ =
            jval<int>(json, "network.raim_refloat_maturity_epochs",
                      "network_raim_refloat_maturity_epochs", 0);
        if (network_raim_refloat_maturity_epochs_ < 0)
            network_raim_refloat_maturity_epochs_ = 0;
        network_signal_min_station_fraction_ =
            jval<double>(json, "network.signal_min_station_fraction", "network_signal_min_station_fraction", 0.0);
        network_phase_bias_min_station_fraction_ =
            jval<double>(json, "network.phase_bias_min_station_fraction", "network_phase_bias_min_station_fraction", 0.0);
        // Per-station analogue of the phase-bias coverage float-gate above: withhold a
        // station whose receiver antenna has no ATX model match (zero receiver PCO/PCV
        // → uncalibrated carrier phase) from every satellite phase-bias state.  Default
        // OFF = every station contributes (byte-identical).
        network_phase_bias_exclude_unmatched_antenna_ =
            jval<bool>(json, "network.phase_bias_exclude_unmatched_antenna",
                       "network_phase_bias_exclude_unmatched_antenna", false);
        // Policy backstop (invariant #6: default OFF): hard-drop any station whose
        // receiver antenna still has no ATX match AFTER the always-on radome→NONE
        // fallback (a genuinely unknown antenna TYPE = unmodelled receiver PCO/PCV,
        // a systematic in every obs feeding the shared clocks/biases).
        network_exclude_unmodeled_antenna_stations_ =
            jval<bool>(json, "network.exclude_unmodeled_antenna_stations",
                       "network_exclude_unmodeled_antenna_stations", false);
        network_raim_attribution_ =
            jval<bool>(json, "network.raim_attribution", "network_raim_attribution", false);
        network_raim_within_epoch_ =
            jval<bool>(json, "network.raim_within_epoch", "network_raim_within_epoch", false);
        network_raim_sigma0_trigger_ =
            jval<double>(json, "network.raim_sigma0_trigger", "network_raim_sigma0_trigger", 3.0);
        network_raim_withhold_sigma0_ =
            jval<double>(json, "network.raim_withhold_sigma0", "network_raim_withhold_sigma0", 0.0);
        // R11/R1: a σ̂0 withhold ceiling in (0,1] is pathological — a healthy epoch's reduced-
        // χ² √ (the σ̂0 the gate compares against) sits at ~1, so a ceiling at or below 1 would
        // WITHHOLD essentially every warm armed epoch (total availability loss), the opposite
        // of the intended rare escape-hatch.  A sensible value is ~2× network.raim_sigma0_trigger.
        // Treat it as a config error: warn and disable (0 = emit-anyway, the documented default)
        // rather than silently withholding the whole run.  Done before the within-epoch coupling
        // below so a clamped-to-0 ceiling does not spuriously arm within-epoch adaptation.
        if (network_raim_withhold_sigma0_ != 0.0 && network_raim_withhold_sigma0_ <= 1.0) {
            log->warn("Network: network.raim_withhold_sigma0 {} is <= 1 — σ̂0 is floored at 1, so "
                      "a ceiling at or below 1 would withhold most warm epochs (and a negative "
                      "value is meaningless).  Disabling it (set ~2x network.raim_sigma0_trigger, "
                      "e.g. 6, to use it).",
                      network_raim_withhold_sigma0_);
            network_raim_withhold_sigma0_ = 0.0;
        }
        // RTA-10: per-constellation carve-out of the A4 σ̂0 withhold.  Activates ONLY at a
        // tripped withhold (raim_withhold_sigma0 > 0), so without that ceiling it is a silent
        // no-op — warn rather than auto-arm (choosing a σ̂0 ceiling is a numeric decision the
        // user must make; there is no safe default to inject).
        network_per_const_withhold_ =
            jval<bool>(json, "network.per_const_withhold", "network_per_const_withhold", false);
        network_per_const_withhold_min_rows_ =
            jval<int>(json, "network.per_const_withhold_min_rows",
                      "network_per_const_withhold_min_rows", 20);
        if (network_per_const_withhold_min_rows_ < 1) {
            log->warn("Network (RTA-10): network.per_const_withhold_min_rows {} < 1 clamped to 1 "
                      "(a constellation needs at least one standardized-residual row to be judged).",
                      network_per_const_withhold_min_rows_);
            network_per_const_withhold_min_rows_ = 1;
        }
        // P4: suppressed under service_mode — the bundle arms raim_withhold_sigma0
        // (2x trigger) in the arming block below, so the "silent no-op" claim would
        // be false by the end of the load.
        if (network_per_const_withhold_ && network_raim_withhold_sigma0_ <= 0.0 && !service_mode)
            log->warn("Network (RTA-10): network.per_const_withhold is on but "
                      "network.raim_withhold_sigma0 is 0 (off) — the carve-out only runs when the "
                      "A4 σ̂0 withhold trips, so it is a silent no-op.  Set a withhold ceiling "
                      "(~2x network.raim_sigma0_trigger) to activate it.");
        if (network_per_const_withhold_ && network_raim_withhold_sigma0_ > 0.0)
            log->info("Network (RTA-10): per-constellation σ̂0 carve-out ON — a tripped withhold "
                      "(ceiling {:.2f}) publishes each constellation whose own σ̂0_c passes the "
                      "same ceiling with >= {} rows, and prunes the rest (whole-epoch withhold "
                      "only when NO constellation passes).",
                      network_raim_withhold_sigma0_, network_per_const_withhold_min_rows_);
        network_raim_surgical_station_ =
            jval<bool>(json, "network.raim_surgical_station", "network_raim_surgical_station", true);
        network_raim_station_drop_bias_ =
            jval<double>(json, "network.raim_station_drop_bias", "network_raim_station_drop_bias", 1.5);
        network_raim_redundancy_normalize_ =
            jval<bool>(json, "network.raim_redundancy_normalize", "network_raim_redundancy_normalize", false);
        if (network_raim_station_drop_bias_ < 1.0) {
            log->warn("Network: network.raim_station_drop_bias {} < 1 clamped to 1 (a bias below 1 "
                      "would PREFER whole-station exclusion, the opposite of its purpose).",
                      network_raim_station_drop_bias_);
            network_raim_station_drop_bias_ = 1.0;
        }
        // A4/P0-4: the withhold ceiling reuses the WITHIN-EPOCH σ̂0 (published only when the
        // within-epoch adaptation runs — otherwise sigma0_adapt stays 0 and the gate never
        // fires).  So raim_withhold_sigma0 is a silent no-op without network.raim_within_epoch.
        // Enable that tier (which then pulls in raim + attribution just below) so the
        // diffuse-contamination escape-hatch is actually active, and say so.
        if (network_raim_withhold_sigma0_ > 0.0 && !network_raim_within_epoch_) {
            log->warn("Network: network.raim_withhold_sigma0 > 0 needs the within-epoch σ̂0 — "
                      "enabling network.raim_within_epoch so the withhold escape-hatch is active "
                      "(it is the tier that computes the σ̂0 the ceiling compares against).");
            network_raim_within_epoch_ = true;
        }
        // The within-epoch adaptation REFLOATS (reinitializes) a contaminated sat/arc's carried
        // ambiguity in-place — the re-solve lets it find its new value and the harvest re-stashes
        // it, so within-epoch is self-contained (no lock-in even at zero process noise).  The
        // post-fit RAIM prominence-attribution refloat is the COMPLEMENTARY tier: it catches
        // slips too small to trip the within-epoch σ̂0 trigger yet large enough to standardized-
        // residual-flag (e.g. a single-satellite spike that does not move the robust network σ̂0).
        // They are the two halves of one backstop, so enable both together for complete coverage.
        if (network_raim_within_epoch_ && (!network_raim_ || !network_raim_attribution_)) {
            log->warn("Network: enabling network.raim + network.raim_attribution alongside "
                      "network.raim_within_epoch — the post-fit attribution refloat is the "
                      "complementary tier that catches slips below the within-epoch σ̂0 trigger.");
            network_raim_ = true;
            network_raim_attribution_ = true;
        }

        // Per-satellite carried-clock sanity monitor (the shared-parameter analogue of the
        // carried-ambiguity refloat): detects a lone satellite whose clock has locked into a
        // physically impossible value — invisible to the σ̂0 RAIM, which needs the shared
        // residual level to move — and refloats its carried clock (+ coupled phase/code bias)
        // so the stiff sat_clock_q carry can no longer freeze the wrong value.
        network_clock_monitor_ =
            jval<bool>(json, "network.clock_monitor", "network_clock_monitor", false);
        network_clock_monitor_abs_max_m_ =
            jval<double>(json, "network.clock_monitor_abs_max_m", "network_clock_monitor_abs_max_m", 20.0);
        network_clock_monitor_jump_max_m_ =
            jval<double>(json, "network.clock_monitor_jump_max_m", "network_clock_monitor_jump_max_m", 10.0);
        network_clock_monitor_couple_raim_ =
            jval<bool>(json, "network.clock_monitor_couple_raim", "network_clock_monitor_couple_raim", false);
        // A flagged satellite's carried state is loosened to this σ (a soft large-q reinit that
        // keeps a weak anchor at the previous value) rather than being withheld entirely; ≤0
        // restores the hard skip-stash refloat.
        network_clock_monitor_reinit_sigma_m_ =
            jval<double>(json, "network.clock_monitor_reinit_sigma_m", "network_clock_monitor_reinit_sigma_m", 30.0);

        // Two-state (offset+drift) oscillator model for the carried satellite clock, with
        // Allan/Hadamard-derived process noise, replacing the scalar random-walk carry: the tight
        // drift-following prediction turns a self-consistent lock-in into a large normalized
        // innovation (a detectable dynamics fault) and sizes the detection band physically.
        network_clock_model_ =
            jval<bool>(json, "network.clock_model", "network_clock_model", false);
        network_clock_model_q1_ =
            jval<double>(json, "network.clock_model_q1", "network_clock_model_q1", 0.0);
        network_clock_model_q2_ =
            jval<double>(json, "network.clock_model_q2", "network_clock_model_q2", 1.5e-7);
        network_clock_model_k_sigma_ =
            jval<double>(json, "network.clock_model_k_sigma", "network_clock_model_k_sigma", 6.0);
        network_clock_model_meas_std_m_ =
            jval<double>(json, "network.clock_model_meas_std_m", "network_clock_model_meas_std_m", 0.05);
        // R11: dedicated max-gap for the two-state clock-model carry (reseed / drift-reset /
        // C1-ZOH bound), distinct from the ambiguity/ZWD phase-leveling max-gap.  Defaults to
        // network_phase_leveling_max_gap_s_ (loaded above) so a config that does not set it is
        // byte-identical to the previous behaviour, which borrowed that same value.
        network_clock_model_max_gap_s_ =
            jval<double>(json, "network.clock_model_max_gap_s", "network_clock_model_max_gap_s",
                         network_phase_leveling_max_gap_s_);
        if (!(network_clock_model_max_gap_s_ > 0.0)) {
            // The phase-leveling gap is itself user-settable and unvalidated, so it
            // cannot be the unconditional fallback (a pl gap <= 0 here would leave the
            // clock model permanently reseeding: no prior, drift 0, C1 0).
            const double gap_fallback = (network_phase_leveling_max_gap_s_ > 0.0)
                                            ? network_phase_leveling_max_gap_s_ : 120.0;
            log->warn("Network: network.clock_model_max_gap_s {} must be > 0; restoring "
                      "{:.0f} s (phase-leveling max-gap, or 120 s when that is also invalid).",
                      network_clock_model_max_gap_s_, gap_fallback);
            network_clock_model_max_gap_s_ = gap_fallback;
        }
        if (network_clock_model_ &&
            network_clock_model_max_gap_s_ != network_phase_leveling_max_gap_s_)
            log->info("Network (R11): network.clock_model_max_gap_s = {:.0f} s (clock-model carry "
                      "reseed / drift-reset / C1-ZOH bound; independent of the {:.0f} s "
                      "ambiguity/ZWD phase-leveling max-gap).",
                      network_clock_model_max_gap_s_, network_phase_leveling_max_gap_s_);
        // --- RT0-4: product generation-time metadata ---
        network_emit_generation_time_ =
            jval<bool>(json, "network.emit_generation_time", "network_emit_generation_time", false);
        if (network_emit_generation_time_)
            log->info("Network: network.emit_generation_time on — each emitted epoch carries a "
                      "wall-clock time-of-generation (G record) for consumer latency measurement. "
                      "Note: the product is NO LONGER byte-reproducible (wall-clock stamps differ "
                      "per run) — leave off for regression/byte-identity runs.");
        // --- RT0-5: per-constellation clock-runaway quarantine ---
        network_clock_runaway_check_ =
            jval<bool>(json, "network.clock_runaway_check", "network_clock_runaway_check", false);
        network_clock_runaway_mag_m_ =
            jval<double>(json, "network.clock_runaway_mag_m", "network_clock_runaway_mag_m", 1000.0);
        network_clock_runaway_rate_m_s_ =
            jval<double>(json, "network.clock_runaway_rate_m_s", "network_clock_runaway_rate_m_s", 10.0);
        if (network_clock_runaway_check_) {
            log->info("Network (RT0-5): clock-runaway quarantine on — a constellation whose median "
                      "|clock−global median| exceeds {:.0f} m or median |drift| exceeds {:.2f} m/s is "
                      "dropped from the epoch's emission and flagged (scope-limits a runaway).",
                      network_clock_runaway_mag_m_, network_clock_runaway_rate_m_s_);
            // P4: gated on !service_mode — the bundle arms network.clock_model below,
            // so under service_mode the rate ceiling is NOT inert.
            if (!network_clock_model_ && !service_mode)
                log->info("Network (RT0-5): the rate ceiling ({:.2f} m/s) is inert without "
                          "network.clock_model (no drift is estimated); the magnitude ceiling "
                          "({:.0f} m) still applies.", network_clock_runaway_rate_m_s_,
                          network_clock_runaway_mag_m_);
            // Degenerate single-constellation run: the global clock median IS that
            // constellation's own median, so the magnitude test measures only the
            // constellation's INTERNAL scatter about its own center — a common-mode
            // runaway (every satellite walking off together, the very BeiDou-style
            // fault the check targets) is invisible to it.  Only the rate ceiling
            // (which needs network.clock_model for a non-zero drift) can catch that.
            if (active_systems_.size() == 1)
                log->warn("Network (RT0-5): clock_runaway_check with a single active system ('{}') — "
                          "the magnitude test degenerates to the constellation's own-median scatter, "
                          "so a common-mode runaway of the whole constellation is invisible; only the "
                          "rate ceiling ({:.2f} m/s, requires network.clock_model) can catch it.",
                          active_systems_.front(), network_clock_runaway_rate_m_s_);
        }
        // --- RTA-10: product-level epoch-status heartbeat ---
        network_emit_epoch_status_record_ =
            jval<bool>(json, "network.emit_epoch_status_record",
                       "network_emit_epoch_status_record", false);
        if (network_emit_epoch_status_record_) {
            log->info("Network (RTA-10): network.emit_epoch_status_record on — one positional H "
                      "heartbeat line per epoch in the product file (including gate / σ̂0 "
                      "withholds, which otherwise emit nothing), so a tail consumer can tell "
                      "\"withheld\" from \"producer died\".  Coverage starts at the first emit "
                      "(the deferred product writer opens then).");
            // P4: gated on !service_mode — the bundle arms product_flush_per_epoch below.
            if (!network_config_.product_flush_per_epoch && !service_mode)
                log->info("Network (RTA-10): consider network.product_flush_per_epoch alongside "
                          "the heartbeat — a live tail only sees the H line when the stream "
                          "flushes.");
        }
        // --- B1: URA quality channel ---
        network_emit_ura_ =
            jval<bool>(json, "network.emit_ura", "network_emit_ura", false);
        network_ura_ewma_alpha_ =
            jval<double>(json, "network.ura_ewma_alpha", "network_ura_ewma_alpha", 0.1);
        network_consume_ura_ =
            jval<bool>(json, "network.consume_ura", "network_consume_ura", false);
        // --- B1b: phase-bias σ channel (Tienstra product-covariance, phase-bias block) ---
        network_emit_phase_bias_sigma_ =
            jval<bool>(json, "network.emit_phase_bias_sigma", "network_emit_phase_bias_sigma", false);
        network_consume_phase_bias_sigma_ =
            jval<bool>(json, "network.consume_phase_bias_sigma", "network_consume_phase_bias_sigma", false);
        // --- M4 (contract v2): combined-correction σ channel (Zhang 2021 / Psychas 2024b) ---
        network_emit_combined_sigma_ =
            jval<bool>(json, "network.emit_combined_sigma", "network_emit_combined_sigma", false);
        network_combined_sigma_agek_ =
            jval<double>(json, "network.combined_sigma_agek", "network_combined_sigma_agek", 0.0);
        // T3a: consumer AR-eligibility veto on the product signal_integer flag (default OFF).
        network_require_signal_integer_for_ar_ =
            jval<bool>(json, "network.require_signal_integer_for_ar",
                       "network_require_signal_integer_for_ar", false);
        if (network_emit_phase_bias_sigma_)
            log->info("Network: network.emit_phase_bias_sigma on — per-(sat,signal) phase-bias "
                      "formal σ (EWMA(σ̂0)×√shared-cov, gauge-exact) written to SsrPhaseBiasEntry."
                      "sigma_m; consumer R-inflation {}.  (B1b: Tienstra phase-bias block of Qŷ_B1.)",
                      network_consume_phase_bias_sigma_ ? "ON" : "off");
        if (network_emit_combined_sigma_)
            log->info("Network: network.emit_combined_sigma on — per-key COMBINED-correction σ "
                      "(sigc=clock+phasebias, sigcc=clock+codebias, cross-cov included) + code/phase "
                      "correlation rho written to the sigc=/sigcc=/rho= wire tokens{}.  Consumer folds "
                      "them (age-inflated) instead of the URA+phase-bias-σ RSS when "
                      "closed_loop.consume_combined_sigma is on.  (M4: Zhang 2021 / Psychas 2024b — "
                      "combined σ is mm-level where the individual σ are cm-level during "
                      "rise-reconvergence.)",
                      network_combined_sigma_agek_ != 0.0
                          ? fmt::format("; age scaling agek={} on the S-line", network_combined_sigma_agek_)
                          : std::string(" (no age scaling: combined_sigma_agek=0)"));
        // R5: network_ura_staleness_q2_ (which defaults to network_clock_model_q2_) is captured
        // AFTER the clock_model validity-restore below, so it inherits the RESTORED q2 (not a
        // rejected-then-reset value).  See the load just past that block.
        network_ura_retro_bump_ =
            jval<bool>(json, "network.ura_retro_bump", "network_ura_retro_bump", false);
        network_ura_refloat_inflate_ =
            jval<double>(json, "network.ura_refloat_inflate", "network_ura_refloat_inflate", 1.0);
        // (RTB-8: service_mode was read at the very top of this section so ALL
        // coherence warnings — including those above this point — can gate on it.)
        if (network_emit_ura_)
            log->info("Network: network.emit_ura on — per-satellite URA quality channel "
                      "(EWMA α {:.2f}) written to the product; consumer R-inflation {}.",
                      network_ura_ewma_alpha_, network_consume_ura_ ? "ON" : "off");
        if (network_emit_ura_ && !network_raim_ && !service_mode)
            log->warn("Network: network.emit_ura is on but network.raim is off — the σ̂0 EWMA that "
                      "scales the URA is only updated by the post-fit RAIM pass, so the emitted URA "
                      "is the FORMAL clock σ only (no robust variance-factor scaling).  Enable "
                      "network.raim for σ̂0-scaled URA.");
        // RTB-7(b): the warmup URA floor rides the URA channel — without emit_ura
        // the URA pass never runs and the floor is a SILENT NO-OP.  Say so.
        // P4: under service_mode the bundle arms emit_ura below, so the floor is
        // NOT a no-op — fall through to the ramp-ON info instead of the warn.
        if (network_config_.warmup_ura_floor_m > 0.0 && !network_emit_ura_ && !service_mode)
            log->warn("Network (RTB-7): network.warmup_ura_floor_m {:.1f} is set but "
                      "network.emit_ura is off — the startup quality ramp floors the EMITTED "
                      "URA, so without the URA channel it is a silent no-op.  Enable "
                      "network.emit_ura.", network_config_.warmup_ura_floor_m);
        else if (network_config_.warmup_ura_floor_m > 0.0)
            log->info("Network (RTB-7): startup quality ramp ON — emitted URA floored at "
                      "{:.1f} m until n_emitted >= max(warmup_epochs {}, "
                      "phase_leveling_warmup_epochs) AND the σ̂0 EWMA has received >= {} "
                      "updates (count waived when the EWMA is structurally unavailable); "
                      "cold-start-to-product-grade is logged at the lift.",
                      network_config_.warmup_ura_floor_m, network_config_.warmup_epochs,
                      network_config_.warmup_epochs);
        if (network_ura_ewma_alpha_ <= 0.0 || network_ura_ewma_alpha_ > 1.0) {
            log->warn("Network: network.ura_ewma_alpha {} out of (0,1]; clamped to 0.1.",
                      network_ura_ewma_alpha_);
            network_ura_ewma_alpha_ = 0.1;
        }
        if (network_clock_model_ &&
            (!(network_clock_model_q2_ > 0.0) || network_clock_model_q1_ < 0.0 ||
             !(network_clock_model_k_sigma_ > 0.0) || !(network_clock_model_meas_std_m_ > 0.0))) {
            log->warn("Network: invalid network.clock_model_* parameters (q1={}, q2={}, k_sigma={}, "
                      "meas_std={}); restoring defaults (q1=0, q2=1.5e-7, k_sigma=6, meas_std=0.05).",
                      network_clock_model_q1_, network_clock_model_q2_,
                      network_clock_model_k_sigma_, network_clock_model_meas_std_m_);
            network_clock_model_q1_ = 0.0; network_clock_model_q2_ = 1.5e-7;
            network_clock_model_k_sigma_ = 6.0; network_clock_model_meas_std_m_ = 0.05;
        }
        // R5: capture the ZOH clock-staleness q² AFTER the validity-restore above so its default
        // inherits the RESTORED network_clock_model_q2_ (a rejected clock_model_q2 was reset to
        // 1.5e-7 there; capturing before would have propagated the bad value into the default).
        // P1-3: defaulting to clock_model_q2 (not 0) keeps the age inflation active whenever
        // network.consume_ura is on; an explicit 0 disables it.  Consumer-side only.
        network_ura_staleness_q2_ =
            jval<double>(json, "network.ura_staleness_q2", "network_ura_staleness_q2",
                         network_clock_model_q2_);
        // A negative value (typo, or inherited from a user-set bad clock_model_q2 when
        // clock_model is OFF and the validity-restore above never ran) is inert at the
        // consumer (q2 > 0 guard) — surface it instead of silently disabling inflation.
        if (network_ura_staleness_q2_ < 0.0) {
            log->warn("Network: network.ura_staleness_q2 {} < 0 → 0 (ZOH staleness inflation "
                      "off; check network.clock_model_q2, which supplies the default).",
                      network_ura_staleness_q2_);
            network_ura_staleness_q2_ = 0.0;
        }
        // R3: ON-only summary lines for the B1 producer/consumer knobs that previously logged
        // nothing (emit_ura already logs above).  Purely informational; no behaviour change.
        if (network_ura_retro_bump_)
            log->info("Network (B1): network.ura_retro_bump on — a satellite flagged by any "
                      "post-emit QC tier gets its discontinuity counter bumped on the NEXT emit "
                      "(edge-triggered, per-satellite) so the consumer resets the ambiguity that "
                      "integrated the contaminated epoch.");
        if (network_ura_refloat_inflate_ > 1.0)
            log->info("Network (B1): network.ura_refloat_inflate {:.2f}x — a recently-refloated "
                      "satellite's URA is inflated for the phase-leveling warmup horizon (honest "
                      "surrogate for the post-refloat DIA conditioning penalty).",
                      network_ura_refloat_inflate_);
        if (network_consume_ura_ && network_ura_staleness_q2_ > 0.0)
            log->info("Network (B1): consumer ZOH clock-staleness q2 = {:.3e} m^2/s^3 (folded as "
                      "q2*dt^3/3 into the applied clock variance as SSR corrections age).",
                      network_ura_staleness_q2_);
        if (network_clock_model_ && network_clock_monitor_) {
            log->info("Network: network.clock_model on — the two-state oscillator normalized-"
                      "innovation test supersedes the clock_monitor absolute/jump bounds for "
                      "flagging; clock_monitor reinit action (refloat/inflate-q) still applies.");
        }
        if (network_clock_monitor_ &&
            (!(network_clock_monitor_abs_max_m_ > 0.0) || !(network_clock_monitor_jump_max_m_ > 0.0))) {
            log->warn("Network: network.clock_monitor_{{abs_max_m,jump_max_m}} must be > 0 "
                      "(got {}, {}); a non-positive bound would flag every satellite — "
                      "restoring defaults 20 / 10 m.",
                      network_clock_monitor_abs_max_m_, network_clock_monitor_jump_max_m_);
            network_clock_monitor_abs_max_m_ = 20.0;
            network_clock_monitor_jump_max_m_ = 10.0;
        }

        // Datum-change signalling (A1/A2): vanish-return pivot handover detection +
        // per-satellite clock discontinuity channel with per-constellation bump scoping.
        network_pivot_vanish_return_signal_ =
            jval<bool>(json, "network.pivot_vanish_return_signal",
                       "network_pivot_vanish_return_signal", false);
        network_emit_clock_disc_ =
            jval<bool>(json, "network.emit_clock_disc", "network_emit_clock_disc", false);
        // Optional emitter discontinuity channels (both default OFF, both travel
        // in the S-contract so file replay reproduces the counters).
        network_code_bias_disc_jump_test_ =
            jval<bool>(json, "network.code_bias_disc_jump_test",
                       "network_code_bias_disc_jump_test", false);
        network_clock_disc_on_set_change_ =
            jval<bool>(json, "network.clock_disc_on_set_change",
                       "network_clock_disc_on_set_change", false);
        // H3: per-λ discontinuity threshold as a fraction of a carrier cycle
        // (frac·λ jump gate).  0 = legacy flat 0.05 m (byte-identical).
        network_disc_threshold_frac_cycles_ =
            jval<double>(json, "network.disc_threshold_frac_cycles",
                         "network_disc_threshold_frac_cycles", 0.0);
        // RTA-1/RTA-2: sticky clock-pivot selection + per-constellation scoping of the
        // producer's INTERNAL handover response (carry skip / oscillator clear).
        network_sticky_pivot_ =
            jval<bool>(json, "network.sticky_pivot", "network_sticky_pivot", false);
        // Anchor-free clock gauge: fix each constellation's clock common-mode with a
        // sum-to-zero over its own satellite clocks (folded like the code-bias gauge)
        // instead of a hard pivot pin.  Removes the pivot/handover machinery entirely
        // for the clock; ignores network.clock_datum_satellite (no pivot to place).
        network_clock_datum_sum_to_zero_ =
            jval<bool>(json, "network.clock_datum_sum_to_zero",
                       "network_clock_datum_sum_to_zero", false);
        // Broadcast-anchored clock gauge (brdc_rts only): per-satellite soft
        // anchors to each satellite's OWN broadcast clock ARE the datum (folded
        // pre-gate).  No pivot, no handover seam, and — unlike sum-to-zero, whose
        // raw-set reference redistributes onto the survivors as satellites
        // rise/set — no common-mode wander: the absolute clock-product frame
        // holds still against the broadcast reference (the RT datum-stability
        // contract).
        network_clock_datum_broadcast_anchored_ =
            jval<bool>(json, "network.clock_datum_broadcast_anchored",
                       "network_clock_datum_broadcast_anchored", false);
        // Time-consistent (δtˢ, dˢ) emission: HOLD the per-satellite clock↔code-bias
        // IF-pin reference signal pair fixed across epochs (latch on first sight,
        // re-latch only on genuine loss) instead of re-selecting the two lowest
        // distinct-frequency signals from each epoch's coverage.  Fixes the per-sat
        // clock re-anchoring (by the satellite's inter-channel code bias) that a
        // sparse channel entering/leaving ≥2-station coverage otherwise triggers —
        // the differential-clock wander a receiver clock cannot absorb.  Only the
        // W6 code-bias IF pin realizes this split, so the toggle is inert without
        // network.estimate_sat_code_bias.
        network_clock_signal_basis_hold_ =
            jval<bool>(json, "network.clock_signal_basis_hold",
                       "network_clock_signal_basis_hold", false);
        // Card 2: impose the W6 code-bias datum (per-sat IF pin + per-(const,signal)
        // sum-to-zero) as a TIME datum — pin the CHANGE of the previous epoch's solved
        // values instead of the level to zero.  Same row count; cures the set-mean
        // wander/relaxation after every rise/set (defect 6-7).  Validated below (both-
        // armed error + inert warn) once estimate_sat_code_bias coupling has resolved.
        network_code_datum_time_linked_ =
            jval<bool>(json, "network.code_datum_time_linked",
                       "network_code_datum_time_linked", false);
        network_scoped_handover_response_ =
            jval<bool>(json, "network.scoped_handover_response",
                       "network_scoped_handover_response", false);
        // Phase-bias gauge: single clock-pivot station (legacy) vs per-(sat,signal)
        // prioritized anchor receiver.  The two sub-knobs (coverage-preference
        // elevation, per-(sat,signal) stickiness) are INERT unless the master toggle
        // is on.
        network_phase_datum_prioritized_ =
            jval<bool>(json, "network.phase_datum_prioritized",
                       "network_phase_datum_prioritized", false);
        // Fix A: anchor-free ensemble (sum-to-zero over receivers) phase-bias gauge.
        // Reuses the prioritized SPOF-cure machinery (no pivot-first, every station
        // carries its arcs) but elects no anchor and folds Σ_r zˢ_{r,σ}=0 through the
        // kernel instead of a single-anchor pin.
        network_phase_datum_ensemble_ =
            jval<bool>(json, "network.phase_datum_ensemble",
                       "network_phase_datum_ensemble", false);
        // Card 3: Eq-34 / de Jonge single-receiver-chain phase-bias datum.
        // Reuses the prioritized SPOF-cure machinery (no pivot-first, every
        // station carries its arcs) but replaces the anchor election with a
        // spanning-tree chain election over per-(const,signal) components +
        // per-(receiver,system,signal) RPB split states.  Two sub-knobs
        // (preferred datum receiver, re-pin rounding) are INERT unless the
        // master toggle is on.
        network_phase_datum_eq34_ =
            jval<bool>(json, "network.phase_datum_eq34",
                       "network_phase_datum_eq34", false);
        network_phase_datum_eq34_receiver_ =
            jval<std::string>(json, "network.phase_datum_eq34_receiver",
                              "network_phase_datum_eq34_receiver", "");
        network_phase_datum_eq34_pin_round_ =
            jval<bool>(json, "network.phase_datum_eq34_pin_round",
                       "network_phase_datum_eq34_pin_round", true);
        network_phase_datum_eq34_rpb_prior_sigma_m_ =
            jval<double>(json, "network.phase_datum_eq34_rpb_prior_sigma_m",
                         "network_phase_datum_eq34_rpb_prior_sigma_m", 1.0);
        network_phase_datum_eq34_rpb_q_m2s_ =
            jval<double>(json, "network.phase_datum_eq34_rpb_q_m2s",
                         "network_phase_datum_eq34_rpb_q_m2s", 3e-6);
        network_phase_datum_eq34_carry_shift_ =
            jval<bool>(json, "network.phase_datum_eq34_carry_shift",
                       "network_phase_datum_eq34_carry_shift", false);
        network_phase_datum_eq34_longevity_election_ =
            jval<bool>(json, "network.phase_datum_eq34_longevity_election",
                       "network_phase_datum_eq34_longevity_election", false);
        network_phase_datum_eq34_extend_only_ =
            jval<bool>(json, "network.phase_datum_eq34_extend_only",
                       "network_phase_datum_eq34_extend_only", false);
        // Card 4 (M1): the DD cycle-basis invariant carry (network.carry_invariant*).
        network_carry_invariant_ =
            jval<bool>(json, "network.carry_invariant",
                       "network_carry_invariant", false);
        network_carry_invariant_qz_ =
            jval<double>(json, "network.carry_invariant_qz",
                         "network_carry_invariant_qz", 0.0);
        network_carry_invariant_emit_rebase_ =
            jval<bool>(json, "network.carry_invariant_emit_rebase",
                       "network_carry_invariant_emit_rebase", true);
        network_carry_invariant_honesty_ =
            jval<bool>(json, "network.carry_invariant_honesty",
                       "network_carry_invariant_honesty", false);
        network_carry_invariant_innov_gate_ =
            jval<bool>(json, "network.carry_invariant_innov_gate",
                       "network_carry_invariant_innov_gate", false);
        network_carry_invariant_innov_k_ =
            jval<double>(json, "network.carry_invariant_innov_k",
                         "network_carry_invariant_innov_k", 4.0);
        network_carry_invariant_innov_refold_ =
            jval<bool>(json, "network.carry_invariant_innov_refold",
                       "network_carry_invariant_innov_refold", false);
        network_carry_invariant_innov_calibrate_ =
            jval<bool>(json, "network.carry_invariant_innov_calibrate",
                       "network_carry_invariant_innov_calibrate", false);
        network_carry_invariant_innov_calibrate_window_ =
            jval<double>(json, "network.carry_invariant_innov_calibrate_window",
                         "network_carry_invariant_innov_calibrate_window", 60.0);
        // PB emission-continuity filter (M5 prerequisite) — outputs-only smoother
        // on the re-based emitted phase biases + a self-tuning k·σ̂_step jump gate.
        network_emit_pb_filter_ =
            jval<bool>(json, "network.emit_pb_filter", "network_emit_pb_filter", false);
        network_emit_pb_filter_q_ =
            jval<double>(json, "network.emit_pb_filter_q", "network_emit_pb_filter_q", 3E-07);
        network_emit_pb_filter_jump_k_ =
            jval<double>(json, "network.emit_pb_filter_jump_k", "network_emit_pb_filter_jump_k", 4.0);
        network_emit_pb_filter_meas_sigma_m_ =
            jval<double>(json, "network.emit_pb_filter_meas_sigma_m",
                         "network_emit_pb_filter_meas_sigma_m", 0.05);
        network_emit_pb_filter_max_gap_s_ =
            jval<double>(json, "network.emit_pb_filter_max_gap_s",
                         "network_emit_pb_filter_max_gap_s", 600.0);
        network_emit_pb_filter_window_epochs_ =
            jval<double>(json, "network.emit_pb_filter_window_epochs",
                         "network_emit_pb_filter_window_epochs", 60.0);
        network_emit_pb_filter_emit_sigma_ =
            jval<bool>(json, "network.emit_pb_filter_emit_sigma",
                       "network_emit_pb_filter_emit_sigma", false);
        network_phase_datum_hold_sigma_m_ =
            jval<double>(json, "network.phase_datum_hold_sigma_m",
                         "network_phase_datum_hold_sigma_m", 0.0);
        network_phase_datum_pref_elev_deg_ =
            jval<double>(json, "network.phase_datum_pref_elev_deg",
                         "network_phase_datum_pref_elev_deg", 15.0);
        network_phase_datum_sticky_ =
            jval<bool>(json, "network.phase_datum_sticky",
                       "network_phase_datum_sticky", true);
        // Coverage-threshold gating of the anchored union: a shared phase-bias
        // column enters the union only when >= this many present stations track it
        // with a usable ambiguity.  1 (default) = every tracked key, the original
        // prioritized-datum behavior (byte-identical); 2-3 excludes the thinly-
        // tracked columns that eject marginal stations as non-PD, re-level on a
        // one-station anchor pool, and leak their anchor lump into the clock↔PB
        // split.  INERT unless network.phase_datum_prioritized.
        network_phase_datum_min_stations_ =
            jval<int>(json, "network.phase_datum_min_stations",
                      "network_phase_datum_min_stations", 1);
        if (network_phase_datum_min_stations_ < 1) {
            log->error("Network: network.phase_datum_min_stations must be >= 1 (got {}); "
                       "1 = no gating (every tracked key enters the anchored union).",
                       network_phase_datum_min_stations_);
            return false;
        }
        // Pin-aware station PD guard: reference the addStation local-block rank test
        // to the MEDIAN |pivot| instead of the max, so a deliberate in-block 1e10
        // anchor pin (prioritized phase datum) no longer inflates the rejection bar
        // ~10 orders and ejects a healthy marginal station every epoch (the
        // TWTF/VOIM class — proven pin-driven by the 2026-07-04 A/B: identical
        // failing pivots with the coverage gate on/off).
        network_station_pd_pin_aware_ =
            jval<bool>(json, "network.station_pd_pin_aware",
                       "network_station_pd_pin_aware", false);
        if (network_station_pd_pin_aware_)
            log->info("Network producer: PIN-AWARE station PD guard ON — the local-block "
                      "rank test references the median |LDLᵀ pivot| (robust to gauge-pin "
                      "rows) instead of the max; marginal stations carrying anchor pins "
                      "are no longer ejected for a healthy ~1e-3 weakest parameter.");
        // R4: the B1 retro-bump steps a flagged satellite's phase, code AND clock discontinuity
        // counters, but the emitter only maintains/stamps the CLOCK counter when emit_clock_disc
        // is on (otherwise disc_counter stays 0 for byte-identity).  So with ura_retro_bump on
        // but emit_clock_disc off, the clock channel would silently never bump — the consumer
        // would reset the phase/code ambiguities of a contaminated satellite but keep integrating
        // its bad clock.  Couple them: arm the clock-disc channel so the retro-bump reaches it.
        if (network_ura_retro_bump_ && !network_emit_clock_disc_) {
            log->warn("Network (R4): enabling network.emit_clock_disc alongside "
                      "network.ura_retro_bump — the per-satellite retro-bump only reaches the "
                      "CLOCK discontinuity counter when the clock-disc channel is armed.  "
                      "(This also scopes the handover datum bump per constellation, A2.)");
            network_emit_clock_disc_ = true;
        }
        // The retro-bump's flag sources are the QC tiers: within-epoch/post-fit/attribution
        // feed it only under the raim toggles, the clock tiers under clock_monitor/clock_model.
        // With every source off the retro-bump can never fire, yet the R4 coupling above has
        // already switched the product to the clock-disc format — a dead knob with a format
        // side-effect deserves a loud note.
        if (network_ura_retro_bump_ && !network_raim_ && !network_raim_within_epoch_ &&
            !network_clock_monitor_ && !network_clock_model_ && !service_mode)
            log->warn("Network (B1): network.ura_retro_bump is on but NO flag-producing QC tier "
                      "is armed (raim / raim_within_epoch / clock_monitor / clock_model all "
                      "off) — the retro-bump can never fire.  Enable at least network.raim.");
        if (network_pivot_vanish_return_signal_)
            log->info("Network producer (A1): vanish-return pivot handover signalling ON — a "
                      "constellation absent for an epoch that returns with a different clock pivot "
                      "now forces a discontinuity bump (persistent last-ever-pivot map).");
        if (network_emit_clock_disc_)
            log->info("Network producer (A2): clock discontinuity channel ON + per-constellation "
                      "datum-bump scoping — the forced handover bump is scoped to the affected "
                      "constellation(s) and satellite clocks carry a disc counter.");
        if (network_sticky_pivot_)
            log->info("Network producer (RTA-1): sticky clock-pivot selection ON — the incumbent "
                      "pivot satellite (per constellation, from the last emitted product) is kept "
                      "while it remains registered; a handover occurs only when it sets.  A "
                      "config-pinned network.clock_datum_satellite still has priority.");
        if (network_clock_datum_sum_to_zero_) {
            log->info("Network producer: SUM-TO-ZERO clock datum ON — each constellation's clock "
                      "common-mode is fixed by Σ δtˢ = 0 over its satellite clocks (folded like the "
                      "code-bias gauge), NOT a pivot pin.  Anchor-free: no clock-pivot satellite, no "
                      "handover seam, no pivot-absent epoch skip; the S-contract isb_datum records it.");
            // A pivot pin and a sum-to-zero are mutually exclusive gauge realizations
            // — sum-to-zero has no pivot to place, so an explicit pin is a no-op here.
            if (!network_config_.clock_datum_satellite.empty())
                log->warn("Network: network.clock_datum_satellite '{}' is IGNORED under "
                          "network.clock_datum_sum_to_zero (the sum-to-zero clock datum has no pivot "
                          "satellite).  Clear one of the two to silence this warning.",
                          network_config_.clock_datum_satellite);
            // Sticky-pivot / vanish-return signalling are pivot-handover machinery that
            // the anchor-free datum removes; note the redundancy rather than silently
            // ignoring a user knob.
            if (network_sticky_pivot_ || network_pivot_vanish_return_signal_)
                log->warn("Network: network.sticky_pivot / network.pivot_vanish_return_signal are "
                          "INERT under network.clock_datum_sum_to_zero — the sum-to-zero clock datum "
                          "never re-references (there is no pivot to keep or hand over).");
        }
        if (network_clock_datum_broadcast_anchored_) {
            // Exactly one clock-gauge realization may be selected — two datums on
            // the same gauge would over-constrain (and the rank gate would not
            // catch it: both close the deficiency).  Fail loud.
            if (network_clock_datum_sum_to_zero_) {
                log->error("Network: network.clock_datum_broadcast_anchored and "
                           "network.clock_datum_sum_to_zero are mutually exclusive clock-gauge "
                           "realizations.  Pick one.");
                return false;
            }
            // The anchor target is each satellite's OWN a-priori clock; only under
            // brdc_rts is that the stable broadcast reference this datum exists to
            // hold the frame against.  Under sp3 the a-priori is already the
            // precise product — anchoring to it re-derives the product, and the
            // sp3 sigma auto-resolution (initial_std.sat_clock, default 1000 m)
            // would leave the gauge unrealized anyway.  Fail loud.
            if (network_apriori_source_ != NetworkAprioriSource::BrdcRts) {
                log->error("Network: network.clock_datum_broadcast_anchored requires "
                           "network.apriori_source = 'brdc_rts' (the datum anchors each satellite "
                           "clock to its own broadcast a-priori).  Set apriori_source or disable "
                           "the toggle.");
                return false;
            }
            // The anchors ARE the gauge: each satellite's single-entry anchor row
            // folds (1/σ²)·I onto that constellation's clock columns, so the clock
            // common-mode direction u = (1/√N)·1 realizes eigenvalue uᵀ(σ⁻²I)u =
            // 1/σ² — independent of N (NOT ≈ N/σ²).  That eigenvalue must clear the
            // per-epoch rank-gate cutoff (≈ 1e-9·smax, and smax grows with the
            // station count), or the datum reads as under-specified and every epoch
            // is withheld.  A fixed load-time bound can only be a necessary floor:
            // reject the clearly non-binding here, and warn in the range that a large
            // network would reject at run time.  The brdc_rts auto default is 3.0 m
            // (1/σ² ≈ 0.11, safe at any realistic scale); only an explicit override
            // can be this weak.
            const double eff_sigma = resolveNetworkAprioriClockAnchorSigmaM();
            if (!(std::isfinite(eff_sigma) && eff_sigma > 0.0 && eff_sigma < 100.0)) {
                log->error("Network: network.clock_datum_broadcast_anchored needs a BINDING "
                           "per-satellite anchor sigma; the resolved "
                           "network.apriori_clock_anchor_sigma_m is {} m.  The realized clock "
                           "common-mode eigenvalue is 1/σ², which must exceed the per-epoch "
                           "rank-gate cutoff (~1e-9·smax); remove the explicit override "
                           "(brdc_rts auto-resolves 3.0 m) or set a binding value.",
                           eff_sigma);
                return false;
            }
            if (eff_sigma > 30.0) {
                log->warn("Network: network.apriori_clock_anchor_sigma_m = {:.1f} m is weak for "
                          "the broadcast-anchored clock datum — the realized common-mode "
                          "eigenvalue 1/σ² = {:.2e} may fall below the per-epoch rank cutoff "
                          "(~1e-9·smax) as the network grows (a ~305-station network rejects "
                          "σ ≳ 60 m as under-specified and withholds every epoch).  The brdc_rts "
                          "default (3.0 m) is safe at any scale.",
                          eff_sigma, 1.0 / (eff_sigma * eff_sigma));
            }
            log->info("Network producer: BROADCAST-ANCHORED clock datum ON — each satellite's "
                      "clock is gauged by a soft anchor to its OWN broadcast clock (sigma "
                      "{:.2f} m, folded pre-gate as the datum), NOT a pivot pin or sum-to-zero. "
                      "No handover seam, and the absolute clock frame does NOT wander on "
                      "rise/set; the S-contract isb_datum records it.", eff_sigma);
            if (!network_config_.clock_datum_satellite.empty())
                log->warn("Network: network.clock_datum_satellite '{}' is IGNORED under "
                          "network.clock_datum_broadcast_anchored (the broadcast-anchored clock "
                          "datum has no pivot satellite).  Clear one of the two to silence this "
                          "warning.", network_config_.clock_datum_satellite);
            if (network_sticky_pivot_ || network_pivot_vanish_return_signal_)
                log->warn("Network: network.sticky_pivot / network.pivot_vanish_return_signal are "
                          "INERT under network.clock_datum_broadcast_anchored — the broadcast-"
                          "anchored clock datum never re-references (there is no pivot to keep "
                          "or hand over).");
        }
        if (network_clock_signal_basis_hold_) {
            // The per-satellite clock↔code-bias IF pin is a W6 construct; without
            // estimate_sat_code_bias there is no satellite code-bias state, so no
            // split to hold — arming the toggle alone changes nothing.  Warn rather
            // than silently ignore (this is a common mis-arming: the toggle targets
            // the SAME datum whose instability motivates it, which only exists in W6).
            if (!estimate_network_sat_code_bias_)
                log->warn("Network: network.clock_signal_basis_hold is INERT without "
                          "network.estimate_sat_code_bias — the per-satellite clock↔code-bias "
                          "IF pin it holds fixed only exists when the network estimates its own "
                          "satellite code biases (W6).");
            else
                log->info("Network producer: HOLD clock signal basis ON — each satellite's "
                          "ionosphere-free clock reference pair is latched on first registration "
                          "and held while both signals stay in the ≥2-station set, so the emitted "
                          "(clock, code-bias) split is time-consistent (the IGS fixed-clock-"
                          "convention analogue) rather than re-anchored by the satellite's "
                          "inter-channel bias whenever a sparse channel enters/leaves coverage.");
        }
        // Card 2: the time-linked code-bias datum and clock_signal_basis_hold BOTH
        // re-target the SAME per-satellite IF-pin row (the clock↔code split).  Running
        // both would double-realize that split with conflicting references — time-
        // linking already subsumes the hold for the pin VALUE (its RHS references the
        // previous solved IF combination directly).  Fail loud rather than silently
        // let one win.
        if (network_code_datum_time_linked_ && network_clock_signal_basis_hold_)
            throw std::runtime_error(
                "network.code_datum_time_linked and network.clock_signal_basis_hold are "
                "mutually exclusive (both re-target the per-satellite IF-pin row; the time-"
                "linked datum subsumes the hold for the pin value).  Enable at most one.");
        if (network_code_datum_time_linked_) {
            // The datum it re-times (IF pin + per-(const,signal) sum-to-zero) only exists
            // in W6; without estimate_sat_code_bias there is nothing to time-link.
            if (!estimate_network_sat_code_bias_)
                log->warn("Network: network.code_datum_time_linked is INERT without "
                          "network.estimate_sat_code_bias — the code-bias IF-pin + sum-to-zero "
                          "datum it re-times only exists when the network estimates its own "
                          "satellite code biases (W6).");
            else
                log->info("Network producer (Card 2): TIME-LINKED code-bias datum ON — the per-"
                          "satellite IF pin and per-(constellation,signal) sum-to-zero rows pin "
                          "the CHANGE of the previous epoch's solved values (veteran-referenced "
                          "sum-to-zero RHS, previous-IF-combination pin RHS) instead of the level "
                          "to zero.  Same row count (rank unchanged); a no-veteran channel or a "
                          "first split re-seeds at 0 with a per-satellite disc bump so the value "
                          "step coincides with a signalled discontinuity.  Eliminates the set-mean "
                          "wander/relaxation after every satellite rise/set.");
        }
        // T3c: the sum-to-zero clock gauge re-references on a clock-satellite SET change;
        // with the clock-disc channel live that re-reference MUST bump the clock disc counters
        // or the consumer integrates across an unsignalled datum step.  Auto-arm the set-change
        // trigger when both its preconditions (sum_to_zero + emit_clock_disc) are satisfied but
        // it was left off — the "auto-arm (or fail loud)" the contract-honesty card requires.
        // Fires ONLY under this specific advanced combination; every other config is untouched.
        if (network_clock_datum_sum_to_zero_ && network_emit_clock_disc_
            && !network_clock_disc_on_set_change_) {
            log->warn("Network (T3c): auto-arming network.clock_disc_on_set_change — under the "
                      "sum-to-zero clock datum with the clock-disc channel on, a clock-satellite "
                      "set change re-references the gauge and MUST bump the clock disc counters "
                      "(otherwise the consumer integrates across an unsignalled datum step).");
            network_clock_disc_on_set_change_ = true;
        }
        if (network_clock_disc_on_set_change_) {
            // The trigger models "membership change = clock-datum re-reference",
            // which is true ONLY for the sum-to-zero gauge (the pivot pin holds
            // while the pivot is up; the broadcast anchors never move).  Armed
            // under any other datum it would mint spurious consumer resets — the
            // exact churn class this work removes.  Fail loud.
            if (!network_clock_datum_sum_to_zero_) {
                log->error("Network: network.clock_disc_on_set_change requires "
                           "network.clock_datum_sum_to_zero — only the sum-to-zero clock gauge "
                           "re-references on a membership change (pivot and broadcast-anchored "
                           "datums hold still).  Disable one of the two.");
                return false;
            }
            if (!network_emit_clock_disc_) {
                log->warn("Network: enabling network.emit_clock_disc alongside "
                          "network.clock_disc_on_set_change — the set-change trigger only "
                          "reaches the wire through the clock discontinuity channel.");
                network_emit_clock_disc_ = true;
            }
            log->info("Network producer: clock set-change discontinuity ON — a constellation "
                      "whose clock-satellite set changes between emits steps its clock disc "
                      "counters (the sum-to-zero datum re-reference, previously unsignalled); "
                      "replay-reproducible via the S-contract clkset= field.");
        }
        if (network_code_bias_disc_jump_test_) {
            if (!estimate_network_sat_code_bias_)
                log->warn("Network: network.code_bias_disc_jump_test is INERT without "
                          "network.estimate_sat_code_bias (no code biases are emitted).");
            log->info("Network producer: code-bias VALUE jump test ON — disc_per_signal also "
                      "bumps on a fresh-baseline jump > {:.2f} m (on top of the structural "
                      "set/band-change triggers), so weak-separation code-bias wander is "
                      "signalled to the consumer; replay-reproducible via the S-contract "
                      "cbjump= field.", NetworkProductEmitter::kDefaultDiscThresholdM);
        }
        if (network_disc_threshold_frac_cycles_ > 0.0) {
            log->info("Network producer: per-λ discontinuity threshold ON — the phase (and, "
                      "under code_bias_disc_jump_test, code) jump test uses {:.3f}·λ(sat,signal) "
                      "instead of the flat {:.2f} m (uniform in CYCLES, still a fixed contract; "
                      "e.g. L1/E1 {:.4f} m, L2 {:.4f} m, L5/E5a {:.4f} m); replay-reproducible "
                      "via the S-contract discfrac= field.",
                      network_disc_threshold_frac_cycles_,
                      NetworkProductEmitter::kDefaultDiscThresholdM,
                      network_disc_threshold_frac_cycles_ * Constants::GPS_L1_WAVELENGTH,
                      network_disc_threshold_frac_cycles_ * Constants::GPS_L2_WAVELENGTH,
                      network_disc_threshold_frac_cycles_ * Constants::GPS_L5_WAVELENGTH);
        }
        if (network_scoped_handover_response_)
            log->info("Network producer (RTA-2): scoped handover response ON — the internal D2 "
                      "carry skip / clock-oscillator clear / jump-test suppression on a pivot "
                      "handover applies only to the constellation(s) whose datum changed, not "
                      "network-wide (the SH-VTEC carry skip stays global).");
        // Card 4 (M1) dependency: the DD cycle-basis carry lives on the eq34 chain
        // datum (it needs the per-receiver RPB split + the spanning tree that supply
        // the fundamental-cycle DDs).  Reject the incoherent combination fail-loud.
        if (network_carry_invariant_ && !network_phase_datum_eq34_) {
            log->error("Network: network.carry_invariant requires network.phase_datum_eq34 — the DD "
                       "cycle-basis carry is defined only on the eq34 chain datum (RPB split + "
                       "spanning tree supply the invariant DDs).  Enable phase_datum_eq34 or disable "
                       "carry_invariant.");
            return false;
        }
        // Card 4 (M1) D5: the fine-slip innovation gate is defined only on the carried DDs.
        if (network_carry_invariant_innov_gate_ && !network_carry_invariant_) {
            log->error("Network: network.carry_invariant_innov_gate requires network.carry_invariant — "
                       "the innovation test standardizes the CARRIED double-difference residual against "
                       "the fold's H0 covariance; with no z-store there is nothing to gate.  Enable "
                       "carry_invariant or disable the innovation gate.");
            return false;
        }
        // Card 4 (M1) D5 v2: the LS-VCE calibration only re-scales the gate's threshold.
        if (network_carry_invariant_innov_calibrate_ && !network_carry_invariant_innov_gate_) {
            log->error("Network: network.carry_invariant_innov_calibrate requires "
                       "network.carry_invariant_innov_gate — it standardizes the gate's innovation by "
                       "the empirical σ̂₀²; with the gate off there is nothing to calibrate.  Enable "
                       "the innovation gate or disable the calibration.");
            return false;
        }
        if (network_phase_datum_eq34_) {
            // Card 3 mutual exclusion (mirror the clock-gauge fail-loud at
            // network_clock_datum_broadcast_anchored above): eq34 and the
            // ensemble are two DIFFERENT realizations of the same phase-bias
            // gauge; neither subsumes the other, so refuse to load both.
            if (network_phase_datum_ensemble_) {
                log->error("Network: network.phase_datum_eq34 and network.phase_datum_ensemble "
                           "are mutually exclusive realizations of the SAME phase-bias gauge "
                           "(single-receiver chain vs sum-to-zero over receivers).  Pick one.");
                return false;
            }
            log->info("Network producer: Eq-34 SINGLE-RECEIVER-CHAIN phase-bias datum ON (Card 3) — "
                      "each (constellation,signal) connected component gets a per-(receiver,system,"
                      "signal) RPB split state δ_{{r,σ}}, ONE datum-receiver RPB pin, and a "
                      "spanning-tree of ambiguity pins over the bipartite (receiver × satellite) "
                      "observation graph.  The emitted between-satellite phase-bias reference then "
                      "stays integer-consistent across ALL satellites (each chain hop contributes "
                      "only an integer) while the frame stays full-rank at global scale.  Reuses the "
                      "prioritized SPOF-cure machinery but replaces the anchor election with a chain "
                      "election; SUPERSEDES network.phase_datum_prioritized.  pin-round={}, "
                      "rpb-prior sigma={} m (0=off).",
                      network_phase_datum_eq34_pin_round_ ? "true" : "false",
                      network_phase_datum_eq34_rpb_prior_sigma_m_);
            if (network_phase_datum_prioritized_)
                log->info("Network producer: network.phase_datum_eq34 SUPERSEDES "
                          "network.phase_datum_prioritized (the ensemble precedent) — the anchor "
                          "election is replaced by the chain election.");
            if (network_phase_datum_hold_sigma_m_ > 0.0)
                log->warn("Network: network.phase_datum_hold_sigma_m = {} is INERT under "
                          "network.phase_datum_eq34 — the hold is an ensemble-only stopgap; the "
                          "chain datum's frame is pinned hard (one RPB pin + tree ambiguity pins), "
                          "so it needs no temporal hold.", network_phase_datum_hold_sigma_m_);
            if (!network_station_pd_pin_aware_)
                log->warn("Network: RECOMMEND network.station_pd_pin_aware = true with "
                          "network.phase_datum_eq34 — hub stations (high spanning-tree degree) "
                          "carry many 1e10 ambiguity pins, and the median-|pivot| PD guard keeps a "
                          "healthy hub from being ejected as non-PD (the TWTF/VOIM relative-rank "
                          "lesson).");
            if (network_phase_datum_min_stations_ > 1)
                log->info("Network producer: eq34 per-(sat,signal) coverage gate ON — a shared "
                          "phase-bias column enters the graph only when >= {} present stations "
                          "track it (gated keys keep legacy per-station lumping).",
                          network_phase_datum_min_stations_);
            if (network_phase_datum_eq34_longevity_election_)
                log->info("Network producer: eq34 LONGEVITY-AWARE tree growth ON (I4) — new "
                          "spanning-tree edges are grown from r1 by arc longevity (rising arcs "
                          "first, then high elevation; high-coverage receivers first) instead of "
                          "lexical order, so tree edges outlive the epoch and b_s re-references "
                          "fewer times (spec D2 step 3).  Incumbent-edge seed unchanged.");
            if (network_phase_datum_eq34_extend_only_)
                log->info("Network producer: eq34 EXTEND-ONLY membership ON (I2, de Jonge §3.5) — "
                          "a tree edge's sticky identity is retained across a momentary row gap "
                          "while its phase-leveling carry is alive (age-stamped to "
                          "phase_leveling_max_gap_s = {} s), so a returning arc re-pins to its "
                          "pre-gap integer (step ~0) instead of re-leveling from a first-sighting "
                          "0.  Tree growth unchanged (live arcs only); watch the gap-repin counter "
                          "in the run-end producer summary.",
                          network_phase_leveling_max_gap_s_);
            if (network_carry_invariant_)
                log->info("Network producer: Card 4 (M1) INVARIANT CARRY ON — the DD cycle-basis "
                          "z-store time-links the S-system-invariant double differences (chords of "
                          "the eq34 spanning tree, q_z={} m²/s) through the producer solve, "
                          "SUPERSEDING the PhaseBias shared carry + phase-leveling local carry + RPB "
                          "carry + eq34 pin-round/shift/reref hygiene for phase keys (GLONASS "
                          "excluded, FDMA).  The per-epoch datum is re-realized freely; basis churn "
                          "is a non-event by construction.  emit_rebase={}, honesty={}.",
                          network_carry_invariant_qz_,
                          network_carry_invariant_emit_rebase_ ? "true" : "false",
                          network_carry_invariant_honesty_ ? "true" : "false");
            if (network_carry_invariant_innov_gate_) {
                log->info("Network producer: Card 4 (M1) D5 FINE-SLIP INNOVATION GATE ON — after each "
                          "accepted solve, every carried DD's standardized innovation "
                          "|z_free−Gᵀx_s−ẑ|/√(M⁻¹−GᵀΣG) is tested at k={}; a chord above threshold is "
                          "re-tapped FRESH (reset, not repair) and its arc marked slipped for the next "
                          "epoch (the C3c membership-exclusion path).  The Card-4-native replacement "
                          "for the RAIM fine-slip layer the invariant bypass empties; watch "
                          "innov_flags/retaps in the run-end summary.", network_carry_invariant_innov_k_);
                if (network_carry_invariant_innov_calibrate_)
                    log->info("Network producer: Card 4 (M1) D5 v2 LS-VCE CALIBRATION ON — the gate "
                              "standardizes by the empirical innovation inflation σ̂₀² (robust geo-mean "
                              "of the standardized innovations, the M2 NetworkCarryVce instrument; "
                              "forgetting half-life {} epochs) instead of the raw formal Cov(v) that the "
                              "M2 monitor measured 10-30× optimistic — so k={} is now a TRUE Nσ gate "
                              "that auto-adapts per regime (one k for both em10/em25).  σ̂₀ is reported "
                              "in the run-end summary.", network_carry_invariant_innov_calibrate_window_,
                              network_carry_invariant_innov_k_);
                if (network_carry_invariant_innov_k_ < 3.0)
                    log->warn("Network: network.carry_invariant_innov_k = {} is BELOW 3 — a low "
                              "threshold storms the carry with false-alarm refloats (each shreds the "
                              "accumulated DD information).  Recommend k >= 4.",
                              network_carry_invariant_innov_k_);
                if (network_carry_invariant_innov_refold_)
                    log->warn("Network: network.carry_invariant_innov_refold (D5 v2 within-epoch refold) "
                              "is parsed for the config contract but NOT YET IMPLEMENTED — the v1 "
                              "reset-only path is active (the one already-solved epoch retains bounded "
                              "contamination; q_z is the backstop).");
            } else if (network_carry_invariant_innov_refold_) {
                log->warn("Network: network.carry_invariant_innov_refold is INERT without "
                          "network.carry_invariant_innov_gate (it routes the gate's flagged set).");
            }
            if (network_emit_pb_filter_) {
                if (network_carry_invariant_emit_rebase_)
                    log->info("Network producer: PB EMISSION-CONTINUITY FILTER ON (M5 prerequisite) — "
                              "a slip-gated per-key scalar KF on the POST-REBASE emitted phase biases "
                              "(q={} m²/s) smooths the per-epoch estimation noise the re-base leaves, and "
                              "a self-tuning k·σ̂_step jump gate (k={}, seed σ̂_step {:.3f} m, window {} ep) "
                              "resets ONLY on a reference-path slip, a genuine event, or a >{:.0f} s gap — "
                              "clearing the ~200/epoch threshold-in-noise discontinuity floor.  "
                              "Outputs-only + S-transparent (the solve, user DDs and dd-frac grading are "
                              "byte-unchanged); the emitter jump test stays the SOLE disc authority.  "
                              "emit_posterior_sigma={}.",
                              network_emit_pb_filter_q_, network_emit_pb_filter_jump_k_,
                              network_emit_pb_filter_meas_sigma_m_,
                              network_emit_pb_filter_window_epochs_,
                              network_emit_pb_filter_max_gap_s_,
                              network_emit_pb_filter_emit_sigma_ ? "true" : "false");
                else
                    log->warn("Network: network.emit_pb_filter is INERT without "
                              "network.carry_invariant_emit_rebase — it filters the RE-BASED emission "
                              "frame, so the re-base must run for the filter to fire.");
                if (network_emit_pb_filter_jump_k_ < 2.0)
                    log->warn("Network: network.emit_pb_filter_jump_k = {} is BELOW 2 — the gate sits "
                              "inside the step noise and leaves the discontinuity floor uncleared.",
                              network_emit_pb_filter_jump_k_);
            }
            // Warn-INERT (spec D7): the eq34 frame-continuity hygiene is subsumed by the
            // invariant carry — the datum targets are plain 0 and continuity lives in the
            // carried DDs, so pin_round / carry_shift / rpb_q are forced off when armed.
            if (network_phase_datum_eq34_pin_round_)
                log->warn("Network: network.phase_datum_eq34_pin_round is INERT under "
                          "carry_invariant (per-epoch datum targets are 0; DD continuity is the carry).");
            if (network_phase_datum_eq34_carry_shift_)
                log->warn("Network: network.phase_datum_eq34_carry_shift is INERT under "
                          "carry_invariant (no carry S-transform — the datum is re-realized freely).");
            if (network_phase_datum_eq34_rpb_q_m2s_ > 0.0)
                log->warn("Network: network.phase_datum_eq34_rpb_q_m2s is INERT under carry_invariant "
                          "(the RPB re-estimates fresh; the DD carry is the phase time-link).");
        } else if (network_phase_datum_ensemble_) {
            log->info("Network producer: ANCHOR-FREE ENSEMBLE phase-bias datum ON (Fix A) — the "
                      "per-(sat,signal) phase-bias gauge is fixed by a sum-to-zero over the "
                      "tracking receivers' ambiguities (Σ_r zˢ_{{r,σ}}=0, kernel Schur-projected "
                      "fold), NOT a single-anchor pin.  No pivot, no handover (the emitted bias "
                      "stops re-referencing → phase accumulates), no single-anchor SPOF, and every "
                      "satellite referenced to the SAME receiver ensemble (the user's "
                      "between-satellite fractional bias cancels → DD ambiguity stays integer).  "
                      "Reuses the prioritized SPOF-cure machinery but elects no anchor; supersedes "
                      "network.phase_datum_prioritized.");
            if (network_phase_datum_min_stations_ > 1)
                log->info("Network producer: ensemble coverage gate ON — a shared phase-bias "
                          "column (and its gauge) is emitted only when >= {} present stations "
                          "track it.", network_phase_datum_min_stations_);
        } else if (network_phase_datum_prioritized_) {
            log->info("Network producer: prioritized phase-bias anchor ON — the per-(sat,signal) "
                      "phase-bias gauge is pinned at the highest-coverage receiver tracking each "
                      "satellite (pref elevation {:.1f} deg, sticky={}), decoupled from the single "
                      "clock-pivot station.  Cures the low-elevation gauge jump + the pivot-absent "
                      "skip; per-(sat,signal) anchor handovers are signalled to the consumer as "
                      "discontinuities.  Clock datum unchanged.",
                      network_phase_datum_pref_elev_deg_,
                      network_phase_datum_sticky_ ? "true" : "false");
            if (network_phase_datum_min_stations_ > 1)
                log->info("Network producer: anchored-union coverage gate ON — a shared "
                          "phase-bias column is registered/emitted only when >= {} present "
                          "stations track it with a usable ambiguity (thin columns are the "
                          "non-PD-ejection / one-station-re-level / split-leak class; their "
                          "phase data still serves the clock through the local blocks).",
                          network_phase_datum_min_stations_);
        } else if (network_phase_datum_min_stations_ > 1) {
            log->warn("Network: network.phase_datum_min_stations = {} is INERT — the anchored-"
                      "union coverage gate only applies under network.phase_datum_prioritized "
                      "(the legacy pivot datum has no union to gate).",
                      network_phase_datum_min_stations_);
        }
        // PB emission-continuity filter INERT unless the invariant carry is armed
        // (its re-based emission frame is the filter's input); the carry-ON case is
        // reported inside the carry_invariant block above.
        if (network_emit_pb_filter_ && !network_carry_invariant_)
            log->warn("Network: network.emit_pb_filter is INERT without network.carry_invariant "
                      "(+ network.carry_invariant_emit_rebase) — it filters the re-based emission "
                      "frame, which only exists under the invariant carry.");

        // RTA-3/RTA-4/RTA-5/RTA-11: product wire/contract hardening for late joiners
        // and producer restarts.  All default OFF ⇒ store and file byte-identical.
        network_emit_generation_id_ =
            jval<bool>(json, "network.emit_generation_id", "network_emit_generation_id", false);
        network_serialize_disc_counters_ =
            jval<bool>(json, "network.serialize_disc_counters",
                       "network_serialize_disc_counters", false);
        network_contract_rebroadcast_s_ =
            jval<double>(json, "network.contract_rebroadcast_s",
                         "network_contract_rebroadcast_s", 0.0);
        network_emitter_staleness_horizon_s_ =
            jval<double>(json, "network.emitter_staleness_horizon_s",
                         "network_emitter_staleness_horizon_s", 0.0);
        if (network_contract_rebroadcast_s_ < 0.0) {
            log->warn("Network: network.contract_rebroadcast_s {} < 0 clamped to 0 (no "
                      "S-contract re-broadcast).", network_contract_rebroadcast_s_);
            network_contract_rebroadcast_s_ = 0.0;
        }
        if (network_emitter_staleness_horizon_s_ < 0.0) {
            log->warn("Network: network.emitter_staleness_horizon_s {} < 0 clamped to 0 "
                      "(keep-forever emitter baselines).", network_emitter_staleness_horizon_s_);
            network_emitter_staleness_horizon_s_ = 0.0;
        }
        if (network_emit_generation_id_)
            log->info("Network producer (RTA-3): solution-generation ID ON — every emitted epoch "
                      "carries a uint32 generation id derived from the first emitted epoch's GPS "
                      "time (optional I record + S-line gid=); a consumer treats a CHANGE of a "
                      "present id as a global datum break (producer restart).");
        if (network_serialize_disc_counters_)
            log->info("Network producer (RTA-4): discontinuity-counter VALUES serialized ON — "
                      "C/B/P records gain optional trailing post-emit counter fields so a "
                      "late-joining/tail reader reconstructs counters without a full replay "
                      "(reader adopts present values, replay recompute kept as a self-check).");
        if (network_contract_rebroadcast_s_ > 0.0) {
            if (network_config_.serialize_sbasis_contract)
                log->info("Network producer (RTA-5): S-contract re-broadcast ON — a CURRENT S line "
                          "(refreshed realized pivots) every {:.0f} s of product time and on any "
                          "datum-change emit.", network_contract_rebroadcast_s_);
            // P4: warn suppressed under service_mode — the bundle arms
            // serialize_sbasis_contract in the arming block below, so the
            // "inert" claim would be false by the end of the load.
            else if (!service_mode)
                log->warn("Network: network.contract_rebroadcast_s {} is set but "
                          "network.serialize_sbasis_contract is OFF — there is no S contract to "
                          "re-broadcast, so the knob is inert.  Enable serialize_sbasis_contract.",
                          network_contract_rebroadcast_s_);
        }

        // --- RTB-8: curated service profile -------------------------------------
        // One switch (network.service_mode) arms the coherent set of producer
        // toggles a live PPP-RTK service needs, instead of hand-assembling ~20
        // interlocking default-off flags (each with implicit couplings that were
        // only ever wired up in scattered test configs).  BUNDLE-WINS semantics:
        // a bool the profile arms is forced true even over an explicit user
        // `false`, and a numeric at 0 (default, an explicit 0, or a negative
        // clamped to 0 above) is armed to its service value — to keep a specific
        // knob off, run without service_mode.  P1: every override of a value the
        // USER actually wrote (presence-checked on both the dotted and the flat
        // legacy key form, the enable_ssr_bias_max_age_explicit_ pattern) is
        // individually WARNED; a user value that already equals the bundle's
        // choice is not an override and stays silent.  Placed after every armed
        // toggle is parsed so it sees final values; the two parse-site validations
        // it can bypass (withhold clamp, clock_model restore) are RE-RUN just
        // below (P3).  Default off ⇒ nothing armed (byte-identical).
        // Deliberately NOT armed (see the do-not-arm notes below the arms):
        //   - network.state_snapshot_path — needs a user-chosen path; a missing
        //     one gets a recommendation info line instead.
        //   - network.warmup_ura_floor_m — policy numeric with no universal
        //     default; the RTB-7 no-op warn above covers a misconfig.
        //   - network.clock_runaway_* ceilings — numeric policy, default-off
        //     quarantine; scope-limiting a runaway is an operator decision.
        //   - network.apriori_source (RTF-1) — an input-SOURCE selection, not a
        //     toggle, and orthogonal to the bundle: a service can run on sp3
        //     ultra-rapids (RTB-4) or brdc_rts equally.  Left exactly as set.
        // Cross-validation of the realized product against the consumer is the
        // S contract itself (RTA-5 re-broadcast + RTA-12 identity fields +
        // checkContract), which this profile turns on.  service_mode was read at
        // the TOP of this section so the coherence warnings could suppress under it.
        if (service_mode) {
            // P1: presence check — did the user write this key (either form)?
            auto userWrote = [&](const char* dotted, const char* flat) {
                return detail::resolvePath(json, dotted) != nullptr || json.contains(flat);
            };
            auto armBool = [&](bool& f, const char* dotted, const char* flat) {
                if (!f && userWrote(dotted, flat))
                    log->warn("Network (RTB-8): service_mode overrides explicit {}=false -> true "
                              "(opinionated bundle; run without service_mode to hand-tune).",
                              dotted);
                if (!f) f = true;
            };
            auto armDouble = [&](double& f, double v, const char* dotted, const char* flat) {
                if (f != 0.0) return;                       // explicit non-zero value wins
                if (userWrote(dotted, flat))
                    log->warn("Network (RTB-8): service_mode overrides explicit {} "
                              "(disabled: 0, or clamped to 0 above) -> {:.1f} (opinionated "
                              "bundle; run without service_mode to hand-tune).", dotted, v);
                f = v;
            };
            armBool(network_emit_clock_disc_,               // consumer resets on clock-pivot handover
                    "network.emit_clock_disc", "network_emit_clock_disc");
            armBool(network_emit_ura_,                      // per-sat quality → consumer weighting
                    "network.emit_ura", "network_emit_ura");
            armBool(network_ura_retro_bump_,                // per-sat post-fit disc
                    "network.ura_retro_bump", "network_ura_retro_bump");
            armBool(network_clock_model_,                   // two-state clock → C1 drift extrapolation
                    "network.clock_model", "network_clock_model");
            armBool(network_phase_leveling_,                // cross-epoch carry — carry_warm prerequisite of BOTH RAIM tiers
                    "network.phase_leveling", "phase_leveling");
            armBool(network_raim_,                          // post-fit RAIM: the σ̂0 EWMA that scales the emitted URA is fed by THIS base flag (NOT within_epoch), and its auto-coupling at the raim_within_epoch parse ran BEFORE this block, so arm it explicitly
                    "network.raim", "network_raim");
            armBool(network_raim_attribution_,              // complementary post-fit attribution refloat tier (the two halves of one backstop)
                    "network.raim_attribution", "network_raim_attribution");
            armBool(network_raim_within_epoch_,             // fault detect → drop-culprit → re-solve
                    "network.raim_within_epoch", "network_raim_within_epoch");
            armDouble(network_raim_withhold_sigma0_,        // withhold a still-contaminated epoch at ~2× the trigger (design guidance); re-validated below (P3)
                      2.0 * network_raim_sigma0_trigger_,
                      "network.raim_withhold_sigma0", "network_raim_withhold_sigma0");
            armBool(network_per_const_withhold_,            // P2/RTA-10: the bundle injects the withhold ceiling above, so the parse-site "no safe ceiling" rationale for not arming the carve-out is void here — degrade SCOPE, not availability
                    "network.per_const_withhold", "network_per_const_withhold");
            armBool(network_emit_epoch_status_record_,      // P2/RTA-10: H heartbeat — a service's live tail must distinguish "withheld" from "producer died"
                    "network.emit_epoch_status_record", "network_emit_epoch_status_record");
            // Pivot-selection / handover-signalling machinery: PROVABLY INERT under the
            // anchor-free sum-to-zero and broadcast-anchored clock datums (no pivot
            // exists, pivotHandoverSignalled always returns false), so don't force-arm
            // dead knobs there — leaving them false keeps the operator from being misled
            // that pivot machinery is active (and keeps the "INERT under ..." warns above
            // consistent with the realized flag state).  An EXPLICIT user set still
            // stands and is warned above.
            if (!network_clock_datum_sum_to_zero_
                && !network_clock_datum_broadcast_anchored_) {
                armBool(network_pivot_vanish_return_signal_,    // P2/A1: without it a vanish-return pivot change is an UNSIGNALLED datum break (emit_clock_disc cannot cover it)
                        "network.pivot_vanish_return_signal", "network_pivot_vanish_return_signal");
                armBool(network_sticky_pivot_,                  // P2/RTA-1: minimize handover rate (datum re-references the consumer pays for)
                        "network.sticky_pivot", "network_sticky_pivot");
            }
            armBool(network_scoped_handover_response_,      // P2/RTA-2: limit a handover's internal blast radius to the affected constellation(s)
                    "network.scoped_handover_response", "network_scoped_handover_response");
            armBool(network_emit_generation_id_,            // RTA-3 restart / datum-break detection
                    "network.emit_generation_id", "network_emit_generation_id");
            armBool(network_serialize_disc_counters_,       // RTA-4 late-joiner counter recovery
                    "network.serialize_disc_counters", "network_serialize_disc_counters");
            armBool(network_config_.serialize_sbasis_contract,    // v2 S contract header
                    "network.serialize_sbasis_contract", "serialize_sbasis_contract");
            armDouble(network_contract_rebroadcast_s_, 30.0,      // RTA-5 periodic re-broadcast
                      "network.contract_rebroadcast_s", "network_contract_rebroadcast_s");
            armBool(network_config_.product_flush_per_epoch,      // RTA-6 live-tail visibility
                    "network.product_flush_per_epoch", "product_flush_per_epoch");
            armDouble(network_config_.health_interval_s, 30.0,    // RTB-6 operator heartbeat
                      "network.health_interval_s", "health_interval_s");
            armDouble(network_config_.trace_rotate_mb, 256.0,     // P2/RTB-6: a 24/7 service must not grow its CSVs unbounded (keep stays at its parsed default 3)
                      "network.trace_rotate_mb", "trace_rotate_mb");
            // P4(b): EVERY file-side knob the bundle just armed (per-epoch flush,
            // disc-counter serialization, v2 S contract + re-broadcast, generation
            // id, H heartbeat) writes into the product FILE — with no
            // product_output_path there is no file and all of them are inert.  The
            // parse-site RTA-6 flush warn cannot catch this (it ran before the
            // arming), so say it here, once, listing the inert knobs.
            if (network_config_.product_output_path.empty())
                log->warn("Network (RTB-8): service_mode is ON but network.product_output_path "
                          "is empty — no product file is written, so the bundle's file-side "
                          "knobs (per-epoch flush, disc-counter serialization, v2 S contract + "
                          "{:.0f} s re-broadcast, generation id, epoch-status heartbeat) are "
                          "ALL inert.  Set network.product_output_path.",
                          network_contract_rebroadcast_s_);
            // Not armed (needs a user-chosen path) — but a 24/7 service without a
            // snapshot cold-starts a new generation on every restart, so recommend it.
            if (network_config_.state_snapshot_path.empty())
                log->info("Network (RTB-8): service_mode is on with no "
                          "network.state_snapshot_path — consider setting one so a producer "
                          "restart WARM-resumes from the RTB-7 snapshot (generation id "
                          "preserved, no consumer-wide ambiguity reset) instead of "
                          "cold-starting a new generation.");
        }

        // --- P3: post-arming re-validation ---------------------------------------
        // Two parse-site checks earlier in this section run BEFORE the service
        // arming block and would be bypassed by service-armed values:
        //   (a) the R11/R1 pathological σ̂0-withhold clamp — the bundle arms the
        //       withhold at 2× network.raim_sigma0_trigger, which for a small
        //       explicit trigger (<= 0.5) lands in the pathological (0,1] band the
        //       parse-site clamp exists to reject (σ̂0 is floored at 1 ⇒ near-total
        //       availability loss);
        //   (b) the clock_model_* validity-restore, gated on the PRE-arming
        //       network_clock_model_ — service_mode arms the oscillator, so a stray
        //       invalid clock_model_q2 would otherwise run it unvalidated.
        // The parse-site originals are KEPT (moving them here would change
        // non-service semantics: the within-epoch auto-coupling reads the CLAMPED
        // withhold, and the R5 ura_staleness_q2 default capture reads the RESTORED
        // q2 — both sit between the two sites).  These re-runs are IDEMPOTENT for
        // non-service runs: they re-apply the exact parse-site conditions, which
        // the parse-site pass already discharged (only the arming block writes
        // these values in between), so they fire — with their one warn — only when
        // the ARMING itself introduced the invalid value.
        if (network_raim_withhold_sigma0_ != 0.0 && network_raim_withhold_sigma0_ <= 1.0) {
            log->warn("Network: network.raim_withhold_sigma0 {} (service-armed at 2x "
                      "network.raim_sigma0_trigger {}) is <= 1 — σ̂0 is floored at 1, so a "
                      "ceiling at or below 1 would withhold most warm epochs (near-total "
                      "availability loss).  Disabling it; raise network.raim_sigma0_trigger "
                      "or set an explicit network.raim_withhold_sigma0 > 1 to restore the "
                      "withhold (the per-constellation carve-out is inert until then).",
                      network_raim_withhold_sigma0_, network_raim_sigma0_trigger_);
            network_raim_withhold_sigma0_ = 0.0;
        }
        if (network_clock_model_ &&
            (!(network_clock_model_q2_ > 0.0) || network_clock_model_q1_ < 0.0 ||
             !(network_clock_model_k_sigma_ > 0.0) || !(network_clock_model_meas_std_m_ > 0.0))) {
            log->warn("Network: invalid network.clock_model_* parameters (q1={}, q2={}, k_sigma={}, "
                      "meas_std={}) under the service-armed clock model; restoring defaults "
                      "(q1=0, q2=1.5e-7, k_sigma=6, meas_std=0.05).",
                      network_clock_model_q1_, network_clock_model_q2_,
                      network_clock_model_k_sigma_, network_clock_model_meas_std_m_);
            network_clock_model_q1_ = 0.0; network_clock_model_q2_ = 1.5e-7;
            network_clock_model_k_sigma_ = 6.0; network_clock_model_meas_std_m_ = 0.05;
        }
        // m8: re-capture the R5 ZOH clock-staleness q² AFTER the service-armed
        // clock_model_q2 restore above.  The parse-site default (line ~3600) read the
        // PRE-arming clock_model_q2; under service_mode a stray-invalid clock_model_q2
        // was left unvalidated there (the parse-site validity-restore is gated on the
        // PRE-arming clock_model flag) and the negative guard then zeroed the derived
        // ura_staleness_q2 — so a service run that armed the oscillator carried
        // staleness inflation OFF despite the model being on.  Re-running the exact
        // capture picks up the NOW-restored q2.  Gated on service_mode so the
        // non-service path is byte-identical (an explicit user value still wins via
        // jval, exactly as at the parse site).
        if (service_mode) {
            network_ura_staleness_q2_ =
                jval<double>(json, "network.ura_staleness_q2", "network_ura_staleness_q2",
                             network_clock_model_q2_);
            if (network_ura_staleness_q2_ < 0.0) {
                log->warn("Network: network.ura_staleness_q2 {} < 0 → 0 (ZOH staleness inflation "
                          "off; check network.clock_model_q2, which supplies the default).",
                          network_ura_staleness_q2_);
                network_ura_staleness_q2_ = 0.0;
            }
        }

        // m9: announce the armed service profile AFTER the P3 re-validation above, so
        // the logged withhold reflects its POST-clamp value.  Armed at 2× a small
        // explicit trigger the withhold can land in the pathological (0,1] band and be
        // disabled by the clamp just above; logging inside the arming block printed the
        // pre-clamp ceiling one line before the clamp retracted it.
        if (service_mode)
            log->info("Network (RTB-8): service_mode ON — armed the RT-service producer set "
                      "(clock/URA disc + retro-bump, clock-model, phase-leveling, post-fit + "
                      "within-epoch RAIM, withhold @ {:.1f} with per-constellation carve-out, "
                      "epoch-status heartbeat, pivot vanish-return signalling, sticky pivot, "
                      "scoped handover response, generation-id, disc-counter serialization, "
                      "v2 contract + {:.0f} s re-broadcast, per-epoch flush, {:.0f} s health "
                      "heartbeat, {:.0f} MiB trace rotation).  service_mode is an OPINIONATED "
                      "bundle: it overrides explicit disables of the armed keys (each override "
                      "is warned individually) — run without service_mode to hand-tune.  Left "
                      "user-set: state_snapshot_path (needs a path), warmup_ura_floor_m and "
                      "the clock_runaway_* ceilings (policy numerics with no universal default).",
                      network_raim_withhold_sigma0_, network_contract_rebroadcast_s_,
                      network_config_.health_interval_s, network_config_.trace_rotate_mb);

        if (network_emitter_staleness_horizon_s_ > 0.0)
            log->info("Network producer (RTA-11): emitter staleness horizon {:.0f} s ON — a "
                      "(sat[,signal]) key returning after a longer product-time gap reseeds its "
                      "phase/code/clock baselines as a first sighting (no discontinuity bump); "
                      "the horizon travels in the S contract (stale=) for replay.",
                      network_emitter_staleness_horizon_s_);

        // Testing fast-forward: skip ahead to a late-hour event (e.g. the ~4.3 h lock-in) in a
        // few warm-up epochs instead of hours of processing.  Cold-starts the carry at the
        // offset, so it reproduces an externally-driven event, not the exact prior history.
        network_start_offset_hours_ =
            jval<double>(json, "network.start_offset_hours", "network_start_offset_hours", 0.0);
        if (network_start_offset_hours_ < 0.0) {
            log->warn("Network: network.start_offset_hours {} < 0 clamped to 0 (process from "
                      "the start).", network_start_offset_hours_);
            network_start_offset_hours_ = 0.0;
        }
        if (network_phase_leveling_) {
            if (network_phase_leveling_q_amb_ < 0.0) {
                log->error("Network: network.phase_leveling_q_amb must be >= 0 (got {}); a "
                           "negative carry process noise is meaningless.",
                           network_phase_leveling_q_amb_);
                return false;
            }
            if (network_phase_leveling_q_amb_ == 0.0)
                log->info("Network producer (phase-leveling): carrying per-station carrier-phase "
                          "ambiguities with ZERO process noise — the correct random-constant model, "
                          "so each ambiguity ACCUMULATES carrier-phase precision indefinitely "
                          "between resets.  Continuity is enforced by the IF-phase / MW-GF slip "
                          "detectors (refloat), gaps by the {:.0f} s max-gap drop — NOT by "
                          "continuous dilation.  warmup {} epochs.  Non-pivot ambiguity columns "
                          "only; the pivot's 1e10 pins still define the phase-bias gauge.",
                          network_phase_leveling_max_gap_s_,
                          network_phase_leveling_warmup_epochs_);
            else
                log->info("Network producer (phase-leveling): carrying per-station carrier-phase "
                          "ambiguities across epochs (soft gap-reacquisition q_amb {:.1e} m²/s, "
                          "warmup {} epochs, max gap {:.0f} s).  Non-pivot ambiguity columns only; "
                          "the pivot's 1e10 pins still define the phase-bias gauge.",
                          network_phase_leveling_q_amb_, network_phase_leveling_warmup_epochs_,
                          network_phase_leveling_max_gap_s_);
            if (network_cycle_slip_turbo_edit_) {
                log->info("Network producer (phase-leveling T0): per-station MW+GF TurboEdit "
                          "cycle-slip REPAIR ENABLED — only confident integer slips (frac ≤ {:.2f}, "
                          "|dN| ≤ {} cyc) shift the carried ambiguity continuously (value += dN·λ); "
                          "less-confident detections are LEFT untouched (BIA-less float-MW false "
                          "positives dominate).  Non-pivot, raw-per-signal ambiguities only.",
                          network_cycle_slip_turbo_edit_frac_max_,
                          network_cycle_slip_turbo_edit_dn_max_);
            }
            if (network_cycle_slip_mw_detect_) {
                if (network_cycle_slip_turbo_edit_)
                    log->warn("Network: network.cycle_slip.mw_detect and .turbo_edit are BOTH on — "
                              "TurboEdit already runs the MW detector for integer REPAIR; the "
                              "mw_detect REFLOAT path is redundant with it and stays inert (repair "
                              "wins).  Use mw_detect INSTEAD of turbo_edit at the 30 s product rate.");
                else
                    log->info("Network producer (phase-leveling): MW slip DETECTOR enabled (mw_detect) — "
                              "a confident wide-lane integer jump (frac ≤ {:.2f}) OR a receiver LLI "
                              "loss-of-lock REFLOATS the carried ambiguity (pending-reseed), never "
                              "applying an integer.  Iono-safe at 30 s (no GF repair).  Complements "
                              "the IF-phase TD detector; non-pivot only.",
                              network_cycle_slip_turbo_edit_frac_max_);
            }
            if (network_shared_carry_var_floor_pb_m2_ > 0.0 || network_shared_carry_var_floor_cb_m2_ > 0.0)
                log->info("Network producer (phase-leveling, B4): D2 shared-carry variance floors — "
                          "sat phase-bias {:.2e} m², sat code-bias {:.2e} m² (caps Λ so a stiff zero-q "
                          "carry cannot lock a biased product in; 0 = off).",
                          network_shared_carry_var_floor_pb_m2_, network_shared_carry_var_floor_cb_m2_);
        } else if (network_cycle_slip_turbo_edit_) {
            log->warn("Network: network.cycle_slip.turbo_edit is on but network.phase_leveling is "
                      "off — T0 slip repair acts on the carried ambiguities, so it is INERT "
                      "without phase-leveling.  Enable network.phase_leveling too.");
        }
        if (network_carry_zwd_) {
            if (network_zwd_carry_q_ < 0.0) {
                log->error("Network: network.zwd_carry_q must be >= 0 (got {}); a negative carry "
                           "process noise is meaningless.", network_zwd_carry_q_);
                return false;
            }
            if (network_phase_leveling_)
                log->info("Network producer (ZWD carry, B9): carrying per-station tropospheric ZWD "
                          "across epochs with random-walk q {:.3e} m²/s (var_eff = var + q·Δt); slant "
                          "iono left white.  Reuses the phase-leveling warmup ({} epochs) / max-gap "
                          "({:.0f} s) housekeeping; non-pivot stations only.",
                          network_zwd_carry_q_, network_phase_leveling_warmup_epochs_,
                          network_phase_leveling_max_gap_s_);
            else
                log->warn("Network: network.carry_zwd is on but network.phase_leveling is off — the "
                          "ZWD carry reuses the phase-leveling warmup gate, so it is INERT without "
                          "phase-leveling.  Enable network.phase_leveling too.");
        }
        if (network_cycle_slip_if_phase_td_) {
            if (network_phase_leveling_)
                log->info("Network producer (IF-phase slip): between-satellite, between-epoch "
                          "ionosphere-free phase triple-difference slip DETECTION enabled — a "
                          "flagged IF pair refloats BOTH of its band ambiguities (pending-reseed) "
                          "so the carry re-estimates them from this epoch's data.  Threshold is "
                          "elevation-normalized (1/sin el) and per (station, signal-pair): the "
                          "network pool seeds each station-pair, then an EWMA of its own noise "
                          "scale takes over as it accumulates clean samples (shrink κ={:.0f}, "
                          "α={:.2f}).  Non-pivot only; detection-only (no MW/GF confirm, no "
                          "integer repair).",
                          network_cycle_slip_if_phase_td_shrink_kappa_,
                          network_cycle_slip_if_phase_td_ewma_alpha_);
            else if (network_carry_invariant_)
                log->info("Network producer (IF-phase slip): between-satellite, between-epoch "
                          "ionosphere-free phase triple-difference slip DETECTION enabled — a "
                          "flagged IF pair feeds the Card-4 DD invariant-carry C3c slip funnel "
                          "(carry_slipped_ambs), so BOTH of its band ambiguities are "
                          "membership-excluded and re-tapped fresh next epoch (the same path the "
                          "MW/LLI coarse detectors take).  Threshold elevation-normalized "
                          "(1/sin el), per (station, signal-pair); non-pivot only; detection-only "
                          "(no MW/GF confirm, no integer repair).");
            else
                log->info("Network producer (IF-phase slip): detection enabled in DIAGNOSTIC-ONLY "
                          "mode — neither network.phase_leveling nor network.carry_invariant is "
                          "on, so there is no carry to refloat; slips are logged but have no "
                          "product effect (every non-pivot ambiguity is re-estimated free each "
                          "epoch in the absolute solve).");
        }
        if (network_raim_) {
            if (network_phase_leveling_)
                log->info("Network producer (RAIM, Increment 3 / T2): post-fit cross-station "
                          "residual consensus ENABLED — a carried ambiguity whose slip the "
                          "pre-processors missed is caught when the network's OTHER stations "
                          "(pinned to their own carries) cannot absorb its leaked shared-clock "
                          "error and their post-fit residuals spike (>{:.1f}σ); a satellite so "
                          "flagged at >= {} stations is refloated network-wide (pending-reseed). "
                          "The redundancy-based safety net that makes zero-process-noise carry sound.",
                          network_raim_k_sigma_, network_raim_min_stations_);
            else
                log->warn("Network: network.raim is on but network.phase_leveling is off — the "
                          "post-fit RAIM protects the carried ambiguities, so it is INERT without "
                          "phase-leveling.  Enable network.phase_leveling too.");
            if (network_raim_redundancy_normalize_)
                log->info("Network producer (RAIM, B2): Baarda redundancy normalization ENABLED — "
                          "residuals standardized by v/(σ·√r_i) using per-row redundancy from the "
                          "cached Schur factors; low-redundancy setting arcs are no longer under-flagged.");
        }
        // L13: the per-station residual field is emitted only inside the post-fit RAIM cell loop
        // (gated raim_on && carry_warm; carry_warm needs phase_leveling) — warn if armed without
        // its runtime prerequisites so an empty file is not a surprise.
        if (network_config_.trace_station_residuals && !network_raim_)
            log->warn("Network: network.trace_station_residuals is on but network.raim is off — "
                      "the per-station residual field is emitted from the post-fit RAIM cell loop, "
                      "so it will be EMPTY.  Enable network.raim AND network.phase_leveling.");

        // --- Optional closed-loop integer-recovery harness (held-out consumer) ---
        // A station deliberately NOT in network.stations that consumes the
        // network's emitted SSR products through the user-side PPP-RTK + AR
        // pipeline (see ClosedLoopConfig).  Inert unless closed_loop.enable.
        network_config_.closed_loop = ClosedLoopConfig{};
        if (nw.contains("closed_loop") && nw["closed_loop"].is_object()) {
            const auto& cl = nw["closed_loop"];
            auto& clc = network_config_.closed_loop;
            clc.enable = cl.value("enable", false);
            if (cl.contains("station") && cl["station"].is_object()) {
                const auto& sj = cl["station"];
                clc.station.name       = sj.value("name", std::string{});
                clc.station.rinex_path = sj.value("rinex_path", std::string{});
                clc.station.fixed      = true;   // truth coordinates, held for scoring
                if (sj.contains("position_ecef") && sj["position_ecef"].is_array()
                    && sj["position_ecef"].size() == 3) {
                    clc.station.position_ecef = {
                        sj["position_ecef"][0].get<double>(),
                        sj["position_ecef"][1].get<double>(),
                        sj["position_ecef"][2].get<double>()
                    };
                    clc.station.has_position = true;
                }
            }
            clc.ar_mode    = cl.value("ar_mode", 1);
            clc.ar_wl_only = cl.value("ar_wl_only", true);
            clc.consume_network_products = cl.value("consume_network_products", true);
            clc.consume_network_clock    = cl.value("consume_network_clock", true);
            clc.consume_network_iono     = cl.value("consume_network_iono", false);
            clc.consume_combined_sigma   = cl.value("consume_combined_sigma", false);  // M4 (contract v2)
            clc.consumer_undifferenced   = cl.value("consumer_undifferenced", false);
            if (cl.contains("ar_active_systems") && cl["ar_active_systems"].is_array()) {
                for (const auto& s : cl["ar_active_systems"])
                    if (s.is_string()) clc.ar_active_systems.push_back(s.get<std::string>());
            }
            if (clc.enable) {
                if (clc.station.name.empty() || clc.station.rinex_path.empty()
                    || !clc.station.has_position) {
                    log->error("Network closed_loop.enable=true but the consumer station is "
                               "incomplete (needs name + rinex_path + position_ecef); disabling.");
                    clc.enable = false;
                } else {
                    log->info("Network closed-loop harness ENABLED: held-out consumer '{}' "
                              "(AR mode {}, wl_only {})", clc.station.name, clc.ar_mode,
                              clc.ar_wl_only ? "on" : "off");
                }
            }
        }

        // --- Producer-side network ambiguity resolution (network AR) ---
        // Integer-leveled satellite phase biases (doc §5).  Default OFF so the
        // float product is bit-identical until explicitly enabled.  Gates are
        // stricter than the user path (FFR 0.9999, per-pair frac 0.10, partial
        // AR on) because a wrong network fix poisons every downstream user.
        {
            auto& nar = network_config_.ambiguity_resolution;
            nar.enable               = jval<bool>  (json, "network.ambiguity_resolution.enable",               "network_ar_enable",               false);
            nar.min_success_rate     = jval<double>(json, "network.ambiguity_resolution.min_success_rate",     "network_ar_min_success_rate",     0.9999);
            nar.ratio_threshold      = jval<double>(json, "network.ambiguity_resolution.ratio_threshold",      "network_ar_ratio_threshold",      3.0);
            nar.ratio_upper_bound    = jval<double>(json, "network.ambiguity_resolution.ratio_upper_bound",    "network_ar_ratio_upper_bound",    1e4);
            nar.nl_max_per_pair_frac = jval<double>(json, "network.ambiguity_resolution.nl_max_per_pair_frac", "network_ar_nl_max_per_pair_frac", 0.10);
            nar.enable_partial_ar    = jval<bool>  (json, "network.ambiguity_resolution.enable_partial_ar",    "network_ar_enable_partial_ar",    true);
            nar.wl_cascade           = jval<bool>  (json, "network.ambiguity_resolution.wl_cascade",           "network_ar_wl_cascade",           false);
            nar.min_epochs           = jval<int>   (json, "network.ambiguity_resolution.min_epochs",           "network_ar_min_epochs",           10);
            if (nar.enable) {
                // RT0-2: the network.ambiguity_resolution section is fully parsed but has
                // ZERO consumers — producer AR is designed (docs/network_ambiguity_resolution_design.md)
                // but NOT built.  Warn loudly so a user who arms it is not misled into thinking a
                // fixed product is emitted; the gates below are inert until the producer AR stage
                // lands.  (Do NOT phrase this as "ENABLED" — that implies integer fixing runs.)
                log->warn("Network AR CONFIGURED but NOT IMPLEMENTED — a FLOAT product is emitted "
                          "(no integer fixing runs; the section parameters are parsed but unused). "
                          "Parsed (inert) gates: FFR>={:.4f}, ratio>={:.1f} (<{:.0e}), "
                          "per-pair frac<={:.2f} cyc, partial_ar {}, wl_cascade {}, min_epochs {}.",
                          nar.min_success_rate, nar.ratio_threshold, nar.ratio_upper_bound,
                          nar.nl_max_per_pair_frac, nar.enable_partial_ar ? "on" : "off",
                          nar.wl_cascade ? "on" : "off", nar.min_epochs);
            }
        }

        log->info("Network mode: {} stations, clock datum station '{}'{}, sync tol {:.3f} s",
                  network_config_.stations.size(),
                  network_config_.clock_datum_station,
                  network_config_.clock_datum_satellite.empty()
                      ? std::string{}
                      : (" + datum sat '" + network_config_.clock_datum_satellite + "'"),
                  network_config_.epoch_sync_tolerance_s);
        if (network_config_.parallel_stations
            && static_cast<int>(network_config_.stations.size()) > network_config_.parallel_min_stations) {
            log->info("Network: per-station compute parallelism ENABLED ({} stations > threshold {}; "
                      "thread cap {})", network_config_.stations.size(),
                      network_config_.parallel_min_stations,
                      network_config_.parallel_max_threads > 0
                          ? std::to_string(network_config_.parallel_max_threads)
                          : std::string("auto"));
        } else {
            log->info("Network: per-station compute runs SERIALLY ({} stations <= threshold {} or "
                      "parallel_stations=false)", network_config_.stations.size(),
                      network_config_.parallel_min_stations);
        }
        if (network_config_.parallel_seam) {
            log->info("Network: deterministic-parallel post-solve seam ENABLED "
                      "(carry-fold + D5 readouts computed per-block in parallel, merged in "
                      "fixed block order — byte-identical to the serial fold)");
        }
        return true;
    }

    void GnssConfig::applyNetworkConsumerCouplings(GnssConfig& c, bool consume_clock,
                                                   bool consume_bias, bool undifferenced) {
        // RTA-7(a): the ONE place the network-consumer couplings live.  Pure
        // extraction of the closed-loop derivation body — deriveClosedLoopConsumerConfig
        // and applyNetworkConsumerProfile both call this, so the in-process
        // closed-loop harness and the standalone real-time consumer can never
        // diverge.  No coupling keys off consume_bias today; it is in the
        // signature so a future bias-side coupling lands here (see header).
        (void)consume_bias;
        // The consumer is an uncombined PPP-RTK USER (single receiver), not the
        // joint network producer.
        c.processing_mode_ = ProcessingMode::PPP_RTK;
        // The network producer ESTIMATES the shared satellite states; the
        // consumer CONSUMES them from the injected SSR store, so turn the
        // network-forced estimation flags back off to their PPP-RTK defaults.
        c.estimate_sat_clocks_       = false;
        c.estimate_sat_phase_biases_ = false;
        c.promote_sat_code_bias_     = false;
        c.estimate_network_sat_code_bias_ = false;  // producer-only switch; consumer CONSUMES the OSB
        c.estimate_network_iono_sh_ = false;        // producer-only; the consumer estimates its own local iono
        c.global_vtec_sh_.enabled   = false;
        // The consumer applies the network's satellite code biases via ssr_bias_term
        // (not gated by enable_code_phase_bias), and inherits enable_code_phase_bias
        // = false from the producer when the network estimates its own OSB — so the
        // network OSB is the SOLE code-bias source and the .BIA never double-corrects.
        // Satellite CODE bias handling depends on WHICH clock the consumer uses.
        // The network folds the satellite code delay into its estimable CLOCK, so
        // a consumer using the NETWORK clock (FULL mode) must NOT re-estimate a
        // separate sat code bias.  But on the PRECISE clock (BIAS-ONLY / BASELINE)
        // there is no folded delay — the consumer is an ordinary PPP-RTK user and
        // SHOULD estimate residual per-(sat,signal) code biases for signals its
        // .BIA does not cover, or the uncorrected code pulls the position estimate.
        c.estimate_sat_biases_ = !consume_clock;
        // The network SOLVE keeps code fully weighted (no_bia_code_downweight ≈ 1)
        // to separate the satellite clock from the phase bias.  A single-receiver
        // USER that inherits that fully weights code carrying uncorrected biases →
        // a systematic position pull.  Restore the user default so code on signals
        // without .BIA coverage is downweighted.
        c.no_bia_code_downweight_ = 100.0;

        if (undifferenced) {
            // UNDIFFERENCED uncombined PPP-RTK USER (the end-state). No
            // observation-space differencing: the network satellite phase biases
            // are applied directly to the raw float (ssr_bias_term, undifferenced
            // — there is no applyDifferencing to re-difference them away), and the
            // AmbiguityResolver forms the between-satellite difference INTERNALLY
            // at fix time (y_dd = x[i] - x[ref]), which cancels the receiver phase
            // bias and yields integer SD ambiguities. So integer recovery needs no
            // BSSD filter machinery / pivot handover at all.
            c.enable_between_satellite_sd_ = false;
            // Direct per-band LAMBDA, NOT the MW/.BIA-frame WL: the state is
            // already PerBand, and forcing wl_only off makes the driver drop the
            // MW WL (setArIgnoreMwWl(true)) so AR runs pure MLAMBDA on the network-
            // bias-corrected float — dissolving the network-bias↔.BIA frame
            // conflict. No WL or WL/NL cascade.
            c.ar_wl_only_ = false;
            // Direct NL LAMBDA needs a converged float to keep the integer search
            // bounded — a loose σ-gate (safe for the robust long-λ WL) lets the
            // per-band search explode. Cap the gate so AR only fires once the
            // float has settled (a BIAS-ONLY float reaches cm well within this).
            if (!(c.ar_max_position_sigma_m_ > 0.0) || c.ar_max_position_sigma_m_ > 0.20)
                c.ar_max_position_sigma_m_ = 0.20;
        } else {
            // Legacy BSSD consumer: the user-side S-transform removes the receiver
            // clock/bias datum so the SD ambiguity is integer once the network
            // satellite phase bias is applied. The producer runs undifferenced
            // (no receiver datum to remove); a BSSD single receiver recovers
            // integers through the pivot S-transform.  (The closed-loop wrapper
            // overlays its configured ar_wl_only for this branch; the standalone
            // profile keeps the user's own setting.)
            c.enable_between_satellite_sd_ = true;
        }
    }

    GnssConfig GnssConfig::deriveClosedLoopConsumerConfig() const {
        GnssConfig c = *this;
        const ClosedLoopConfig& cl = network_config_.closed_loop;
        // Shared couplings (RTA-7(a) — one source of truth with the standalone
        // profile).  consume_clock mirrors the pre-extraction local: the clock
        // coupling applies only when products are actually consumed.
        const bool consuming_network_clock =
            cl.consume_network_products && cl.consume_network_clock;
        applyNetworkConsumerCouplings(c, consuming_network_clock,
                                      cl.consume_network_products,
                                      cl.consumer_undifferenced);
        // RTA-9(a) closed-loop counterpart: the derived consumer consumes live
        // network biases exactly like the standalone profile, but this path
        // never ran the profile's staleness-gate arming — so it silently applied
        // unboundedly stale biases across producer withhold gaps AND never armed
        // the A5 freshness/presence flip detectors (the sp3_6h withhold-aligned
        // excursions of the Jul-2026 closed-loop audit).  Arm the gate under
        // consumption with the same explicit-false respect; the arming happens
        // on the DERIVED config only, so the producer run is untouched.  (No
        // LogManager here — the effective value surfaces via the consumer
        // engine's config echo.)
        if (cl.consume_network_products && !c.enable_ssr_bias_max_age_
            && !c.enable_ssr_bias_max_age_explicit_)
            c.enable_ssr_bias_max_age_ = true;
        // Closed-loop-specific overlay: enable ambiguity resolution for the
        // consumer (the producer itself is float) — the closed loop validates
        // integer RECOVERY on the held-out station from the producer's products.
        c.ar_mode_    = cl.ar_mode;
        if (!cl.ar_active_systems.empty())
            c.ar_active_systems_ = cl.ar_active_systems;
        if (!cl.consumer_undifferenced)
            c.ar_wl_only_ = cl.ar_wl_only;   // BSSD consumer takes the harness's WL-only choice
        return c;
    }

    void GnssConfig::applyNetworkConsumerProfile(LogManager& log_manager) {
        auto log = log_manager.getLogger("gnss_config");
        const NetworkConsumerConfig& nc = network_consumer_config_;
        applyNetworkConsumerCouplings(*this, nc.clock, nc.bias, nc.undifferenced);
        // A standalone consumer of network CODE biases needs receiver code-bias
        // states (estimate_ifb) to absorb a code-bias datum re-reference — the
        // closed-loop consumer inherits estimate_ifb from the producer config,
        // but a standalone user config has no producer to inherit from.  Arm it
        // up front so the engine's one-shot A5 warning (kept for non-profile
        // paths) never fires on the profile path.
        if (nc.bias && !estimate_ifb_) {
            estimate_ifb_ = true;
            log->info("Network-consumer profile: arming estimation.estimate_ifb — receiver "
                      "code-bias states are required to absorb network code-bias datum "
                      "re-references (disc-counter driven reinflation).");
        }
        // RTA-9(a): bias consumption without the bias staleness gate silently
        // applies unboundedly stale biases across producer withhold gaps — arm
        // the gate by default UNDER CONSUMPTION.  Presence-checked at load
        // (enable_ssr_bias_max_age_explicit_): an explicit user 'false' is
        // respected; the closed-loop harness path never runs this (the coupling
        // lives in the profile, not applyNetworkConsumerCouplings).
        if (nc.bias && !enable_ssr_bias_max_age_ && !enable_ssr_bias_max_age_explicit_) {
            enable_ssr_bias_max_age_ = true;
            log->info("Network-consumer profile (RTA-9a): arming processing.enable_ssr_bias_max_age "
                      "— consumed code/phase biases are age-gated at {:.1f}s (stale-hold to "
                      "{:.1f}s = {:.2f}x).  Set the key explicitly false to opt out.",
                      ssr_bias_max_age_s_,
                      ssr_bias_max_age_s_ * ssr_bias_stale_hold_factor_,
                      ssr_bias_stale_hold_factor_);
        }
        log->info("Network-consumer profile APPLIED: PPP-RTK user, consume clock {} / bias {}, "
                  "{} model, orbit_source {}, estimate_sat_biases {}, ar_wl_only {}, "
                  "ar_ignore_mw_wl {}, bias_max_age {} (effective).",
                  nc.clock ? "on" : "off", nc.bias ? "on" : "off",
                  nc.undifferenced ? "undifferenced" : "BSSD",
                  nc.orbit_source,
                  estimate_sat_biases_ ? "on" : "off",
                  ar_wl_only_ ? "on" : "off",
                  networkConsumerArIgnoreMwWl() ? "true" : "false",
                  enable_ssr_bias_max_age_ ? "on" : "off");
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
    }

} // namespace GnssProcessor
