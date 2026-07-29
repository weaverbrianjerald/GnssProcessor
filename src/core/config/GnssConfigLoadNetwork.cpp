/*!
 * \file GnssConfigLoadNetwork.cpp
 * \brief JSON configuration loading for the PPP-RTK @c network section.
 *
 * Split out of @c GnssConfigLoad.cpp (decomposition INC-1, 2026-07-16;
 * see docs/network_decomposition_addendum_2026-07-16.md).  Contains, verbatim:
 *
 *   - the file-local @c parseCoordEpochYear helper (sole caller lives in
 *     @c loadNetworkSection_),
 *   - @c GnssConfig::loadNetworkSection_ — the network section loader,
 *     including its V1/W6 cross-section forcing writes (13 sites over 10
 *     non-network fields; they are private-member writes and travel with
 *     the member function),
 *   - the consumer-coupling helpers @c applyNetworkConsumerCouplings,
 *     @c deriveClosedLoopConsumerConfig and @c applyNetworkConsumerProfile.
 *
 * Behavior is unchanged; only the file layout has moved.  The estimation
 * section's raw-JSON peek at @c network.estimate_glonass deliberately stays
 * in @c GnssConfigLoad.cpp (it reads the JSON directly, not this code).
 */

#include "config/GnssConfig.hpp"
#include "config/GnssConfigJsonHelpers.hpp"
#include "common/Constants.hpp"                   // GPS_Lx_WAVELENGTH (H3 per-λ log line)
#include "products/NetworkProductEmitter.hpp"   // kDefaultDiscThresholdM (R9 single source of truth)
#include "products/StationInfoFile.hpp"         // resolveRosterCoordinate (RTA-12 SIF roster)
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <fmt/format.h>
#include <nlohmann/json.hpp>
#include <optional>
#include <spdlog/spdlog.h>

namespace GnssProcessor {

    using detail::jval;
    using detail::jsection;

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
            // Cap on the MLAMBDA search dimension (0 = uncapped legacy).  Required at
            // scale: an uncapped ~2500-dim basis at 305 stations does not return.
            ar.ils_max_dim = jval<int>(json, "network.ambiguity_resolution.ils_max_dim",
                                       "network_ar_ils_max_dim", 0);
            if (ar.enable) {
                log->info("Network producer AR ON — integer-leveled satellite phase biases "
                          "(FFR min_success_rate={}, ratio>={}, nl_frac<={} cyc, partial_ar={}, "
                          "wl_cascade={}, min_epochs={}).  GPS+Galileo only; GLONASS stays float.",
                          ar.min_success_rate, ar.ratio_threshold, ar.nl_max_per_pair_frac,
                          ar.enable_partial_ar ? "on" : "off", ar.wl_cascade ? "on" : "off",
                          ar.min_epochs);
            }
        }

        // --- MW-seeded wide-lane pseudo-obs layer (M5 card WL-1; network.mw_wl.*) ---
        // Feeds the per-arc Melbourne-Wubbena Welford means, DD'd over the Card-4 chord
        // basis, into the solve as rank-K WL pseudo-obs blocks — the code-coherence
        // accumulator the per-epoch network lacks (docs/SPEC_mw_wl_card.md).  Default
        // enable=false => byte-identical.  The carry_invariant requirement + Phase-B
        // warnings are enforced in the carry_invariant validation block below (after
        // network.carry_invariant is parsed).
        {
            auto& mw = network_config_.mw_wl;
            mw.enable = jval<bool>(json, "network.mw_wl.enable", "network_mw_wl_enable", false);
            mw.min_arc_epochs = jval<int>(json, "network.mw_wl.min_arc_epochs",
                                          "network_mw_wl_min_arc_epochs", 10);
            mw.sigma_floor_cyc = jval<double>(json, "network.mw_wl.sigma_floor_cyc",
                                              "network_mw_wl_sigma_floor_cyc", 0.05);
            mw.max_rows_per_epoch = jval<int>(json, "network.mw_wl.max_rows_per_epoch",
                                              "network_mw_wl_max_rows_per_epoch", 0);
            mw.fix_enable = jval<bool>(json, "network.mw_wl.fix_enable",
                                       "network_mw_wl_fix_enable", false);
            mw.fix_sigma_cyc = jval<double>(json, "network.mw_wl.fix_sigma_cyc",
                                            "network_mw_wl_fix_sigma_cyc", 0.10);
            mw.fix_frac_cyc = jval<double>(json, "network.mw_wl.fix_frac_cyc",
                                           "network_mw_wl_fix_frac_cyc", 0.15);
            mw.fix_weight_sigma_cyc = jval<double>(json, "network.mw_wl.fix_weight_sigma_cyc",
                                                   "network_mw_wl_fix_weight_sigma_cyc", 0.01);
            mw.f6_dump = jval<bool>(json, "network.mw_wl.f6_dump",
                                    "network_mw_wl_f6_dump", false);
            // Row budget + block span for the F6 dump.  The legacy dump sampled 8 rows of ONE
            // block; that fixed size is a SAMPLE and says nothing about how many cycles the
            // layer built (a lesson paid for by misreading it as a ~99.7% quarantine).  Both
            // default to the legacy shape ⇒ an unset config dumps exactly as before.
            mw.f6_max_rows = jval<int>(json, "network.mw_wl.f6_max_rows",
                                       "network_mw_wl_f6_max_rows", 8);
            mw.f6_all_blocks = jval<bool>(json, "network.mw_wl.f6_all_blocks",
                                          "network_mw_wl_f6_all_blocks", false);
            mw.debug_r_scale = jval<double>(json, "network.mw_wl.debug_r_scale",
                                            "network_mw_wl_debug_r_scale", 1.0);
            mw.ewl_fold_rows = jval<bool>(json, "network.mw_wl.ewl_fold_rows",
                                          "network_mw_wl_ewl_fold_rows", false);
            if (mw.ewl_fold_rows)
                log->warn("network.mw_wl.ewl_fold_rows=true — F-A STUDY SHAPE: folding the EWL "
                          "integer rows into the solve re-creates the ~2x D5 elevation the "
                          "Jul-16 A/B attributed (border-mediated tension against the stiff "
                          "carries).  The WL cascade does not need the fold (it reads the "
                          "detector statistic).  Diagnostic only, NOT a production shape.");
            mw.d5_dump = jval<bool>(json, "network.mw_wl.d5_dump",
                                    "network_mw_wl_d5_dump", false);
            mw.lane_trace = jval<bool>(json, "network.mw_wl.lane_trace",
                                       "network_mw_wl_lane_trace", false);
            // --- M5 card WL-3: lane-coordinate carry (docs/SPEC_lane_carry_card.md).
            // Parsed + clamped here; the carry_invariant / ewl_enable mutex + the ON
            // summary live in the validation block below (after network.carry_invariant
            // is parsed), mirroring the mw_wl mutexes.
            {
                auto& lc = network_config_.lane_carry;
                lc.enable = jval<bool>(json, "network.lane_carry.enable",
                                       "network_lane_carry_enable", false);
                // DEFAULT 0.01 (measured Jul-16, 15-ep A/B).  The SPEC §3.1 "10³ × q_nl"
                // numerology assumed q_nl ≈ 0; at the lineage qz = 1e-4 m²/s it adds
                // (√3 m ≈ 11.8 WL cyc)² of lane variance PER 30 s EPOCH — swamping every
                // accumulated lane measurement and destroying each FFR fix within one
                // epoch.  0.01×qz ≈ (0.04 cyc)²/epoch keeps the √N accumulation alive
                // (constrained circR 0.471 / med|f| 0.061 vs 0.251/0.247 at scale 1) while
                // still relaxing a stale lane in tens of epochs.  The lane starts DIFFUSE
                // and earns tightness only from measurements, so a large q_wl is not
                // needed to prevent unearned stiffness.
                lc.q_wl_scale = std::max(0.0,
                    jval<double>(json, "network.lane_carry.q_wl_scale",
                                 "network_lane_carry_q_wl_scale", 0.01));
                // DEFAULT 0.0 = keep the ISOTROPIC diffuse prior at birth.  The SPEC §3.1
                // mean-zero "code-seed scale" fresh lane prior is FALSIFIED on real data
                // (measured Jul-16: D5 691 vs 125 without it): the correction-frame origin
                // is NOT the code-seed lane — a cycle's ambiguities seed at different
                // epochs, so per-station clock error at seed time does NOT cancel in the
                // lane (× (f1−f2) ⇒ hundreds of cycles of mis-centering at σ=3 cyc).  The
                // honest code-seed-scale information enters instead through the FIRST
                // per-epoch MW lane increment (correctly centered on the detector value).
                // Positive values (study shape) clamp to ≥0.1 cyc.
                lc.sigma_wl_fresh_cyc =
                    jval<double>(json, "network.lane_carry.sigma_wl_fresh_cyc",
                                 "network_lane_carry_sigma_wl_fresh_cyc", 0.0);
                lc.sigma_wl_fresh_cyc = (lc.sigma_wl_fresh_cyc <= 0.0)
                    ? 0.0 : std::max(0.1, lc.sigma_wl_fresh_cyc);
                if (lc.sigma_wl_fresh_cyc > 0.0)
                    log->warn("Network: network.lane_carry.sigma_wl_fresh_cyc={} — the mean-zero "
                              "fresh lane prior is a FALSIFIED mechanism on real data (seed-time "
                              "clock diversity mis-centers it by O(10²) cycles; measured D5 "
                              "691-vs-125, Jul-16).  Study shape only; the default (0 = diffuse "
                              "birth + first MW increment) is the honest form.",
                              lc.sigma_wl_fresh_cyc);
                lc.lane_meas = jval<bool>(json, "network.lane_carry.lane_meas",
                                          "network_lane_carry_lane_meas", true);
                lc.bridge_meas = jval<bool>(json, "network.lane_carry.bridge_meas",
                                            "network_lane_carry_bridge_meas", true);
                // LF-1 INC-1 (docs/SPEC_lane_fold_perf_card.md §3.2): the optimized blocked/sparse
                // lane fold.  A PERF knob — it changes only HOW the linear algebra is applied
                // (O(K) sparse SET + hoisted symmetrize), never what folds — so it stays OUT of
                // the equivalence_shape discriminator below.  Equivalence class (NOT bit-identical).
                lc.fold_blocked = jval<bool>(json, "network.lane_carry.fold_blocked",
                                             "network_lane_carry_fold_blocked", false);
                // LF-1 INC-2 (§3.3): pinned-cycle increment skip.  Also a PERF knob (stays out
                // of the equivalence_shape discriminator).  within_n clamps to ≥1 (a 0 window
                // would resume increments the epoch after every decision — the skip never fires).
                lc.skip_pinned_increments =
                    jval<bool>(json, "network.lane_carry.skip_pinned_increments",
                               "network_lane_carry_skip_pinned_increments", false);
                lc.skip_pinned_within_n = std::max<long long>(1,
                    jval<long long>(json, "network.lane_carry.skip_pinned_within_n",
                                    "network_lane_carry_skip_pinned_within_n", 3));
                // LF-1 INC-4 (§3.5): component-parallel MW fold.  PERF knob (out of the
                // equivalence_shape discriminator); byte-identical to serial.
                lc.parallel_components =
                    jval<bool>(json, "network.lane_carry.parallel_components",
                               "network_lane_carry_parallel_components", false);
            }
            // --- M5 card WL-2: EWL-first tri-frequency cascade (SPEC_ewl_wl_cascade_card.md §4). ---
            mw.ewl_enable = jval<bool>(json, "network.mw_wl.ewl_enable",
                                       "network_mw_wl_ewl_enable", false);
            mw.ewl_min_arc_epochs = jval<int>(json, "network.mw_wl.ewl_min_arc_epochs",
                                              "network_mw_wl_ewl_min_arc_epochs", 2);
            mw.ewl_fix_sigma_cyc = jval<double>(json, "network.mw_wl.ewl_fix_sigma_cyc",
                                                "network_mw_wl_ewl_fix_sigma_cyc", 0.10);
            mw.ewl_fix_frac_cyc = jval<double>(json, "network.mw_wl.ewl_fix_frac_cyc",
                                               "network_mw_wl_ewl_fix_frac_cyc", 0.20);
            mw.wl_inject_enable = jval<bool>(json, "network.mw_wl.wl_inject_enable",
                                             "network_mw_wl_wl_inject_enable", false);
            mw.wl_fix_sigma_cyc = jval<double>(json, "network.mw_wl.wl_fix_sigma_cyc",
                                               "network_mw_wl_wl_fix_sigma_cyc", 0.10);
            mw.wl_fix_frac_cyc = jval<double>(json, "network.mw_wl.wl_fix_frac_cyc",
                                              "network_mw_wl_wl_fix_frac_cyc", 0.15);
            mw.bridge_max_sigma_cyc = jval<double>(json, "network.mw_wl.bridge_max_sigma_cyc",
                                                   "network_mw_wl_bridge_max_sigma_cyc", 0.15);
            mw.inject_disagree_min_cyc = jval<double>(json, "network.mw_wl.inject_disagree_min_cyc",
                                                      "network_mw_wl_inject_disagree_min_cyc", 0.75);
            mw.lane_max_abs_xlin_m = jval<double>(json, "network.mw_wl.lane_max_abs_xlin_m",
                                                  "network_mw_wl_lane_max_abs_xlin_m", 1000.0);
            // --- M5 card UWL-1: per-constellation ultra-wide-lane top tier (SPEC_uwl_card.md §5). ---
            mw.uwl_enable = jval<bool>(json, "network.mw_wl.uwl_enable",
                                       "network_mw_wl_uwl_enable", false);
            {
                const int gtb = jval<int>(json, "network.mw_wl.uwl_gal_top_band",
                                          "network_mw_wl_uwl_gal_top_band", 7);
                if (gtb == 7 || gtb == 8) {
                    mw.uwl_gal_top_band = static_cast<char>('0' + gtb);
                } else {
                    log->warn("Network: network.mw_wl.uwl_gal_top_band = {} is not 7 (E5b, "
                              "λ 9.768 m) or 8 (E5 AltBOC, λ 19.537 m) — keeping the default 7.",
                              gtb);
                    mw.uwl_gal_top_band = '7';
                }
            }
            mw.uwl_mid_enable = jval<bool>(json, "network.mw_wl.uwl_mid_enable",
                                           "network_mw_wl_uwl_mid_enable", false);
            // Increment 5: dual-top tier (band-8 preferred / (5,7) fallback + sibling closure).
            mw.uwl_dual_top = jval<bool>(json, "network.mw_wl.uwl_dual_top",
                                         "network_mw_wl_uwl_dual_top", false);
            mw.uwl_sib_gate = jval<bool>(json, "network.mw_wl.uwl_sib_gate",
                                         "network_mw_wl_uwl_sib_gate", false);
            // v2 census recovery: per-family lane pair groups.
            mw.uwl_family_pairs = jval<bool>(json, "network.mw_wl.uwl_family_pairs",
                                             "network_mw_wl_uwl_family_pairs", false);
            mw.uwl_family_min_arcs = jval<int>(json, "network.mw_wl.uwl_family_min_arcs",
                                               "network_mw_wl_uwl_family_min_arcs", 8);
            if (mw.uwl_family_min_arcs < 1) {
                log->warn("Network: network.mw_wl.uwl_family_min_arcs = {} is below 1 — "
                          "clamping to 1 (a pair group needs at least one arc).",
                          mw.uwl_family_min_arcs);
                mw.uwl_family_min_arcs = 1;
            }
            // 60-ep age-bias fix: per-sample estimable-iono de-bias of the bridge.
            mw.bridge_sample_debias = jval<bool>(json, "network.mw_wl.bridge_sample_debias",
                                                 "network_mw_wl_bridge_sample_debias", false);
            mw.uwl_cold_fix = jval<bool>(json, "network.mw_wl.uwl_cold_fix",
                                         "network_mw_wl_uwl_cold_fix", false);
            mw.uwl_cold_sigma_m = jval<double>(json, "network.mw_wl.uwl_cold_sigma_m",
                                               "network_mw_wl_uwl_cold_sigma_m", 0.7);
            mw.uwl_cold_min_lambda_m = jval<double>(json, "network.mw_wl.uwl_cold_min_lambda_m",
                                                    "network_mw_wl_uwl_cold_min_lambda_m", 8.0);
            if (!(mw.uwl_cold_sigma_m > 0.0)) {
                log->warn("Network: network.mw_wl.uwl_cold_sigma_m = {} is non-positive — "
                          "clamping to 0.7 m (the cold decision needs an honest a-priori R).",
                          mw.uwl_cold_sigma_m);
                mw.uwl_cold_sigma_m = 0.7;
            }
            if (mw.uwl_cold_min_lambda_m < 6.0) {
                log->warn("Network: network.mw_wl.uwl_cold_min_lambda_m = {} m would admit the "
                          "GPS 5.861 m EWL to the n=1 cold fix — the cold fix is a UWL privilege "
                          "earned by wavelength (SPEC_uwl_card.md §3.3); clamping to 6.0 m.",
                          mw.uwl_cold_min_lambda_m);
                mw.uwl_cold_min_lambda_m = 6.0;
            }
            if (mw.ewl_min_arc_epochs < 2) {
                log->warn("Network: network.mw_wl.ewl_min_arc_epochs = {} is below 2 (a "
                          "variance-of-mean needs >=2 samples) — clamping to 2.",
                          mw.ewl_min_arc_epochs);
                mw.ewl_min_arc_epochs = 2;
            }
            if (mw.min_arc_epochs < 2) {
                log->warn("Network: network.mw_wl.min_arc_epochs = {} is below 2 (a "
                          "variance-of-mean needs >=2 samples) — clamping to 2.",
                          mw.min_arc_epochs);
                mw.min_arc_epochs = 2;
            }
            if (!(mw.sigma_floor_cyc > 0.0)) {
                log->warn("Network: network.mw_wl.sigma_floor_cyc = {} is non-positive — "
                          "clamping to 0.05 cyc (an un-floored R lets a lucky-tight arc fold "
                          "a near-exact WL constraint).", mw.sigma_floor_cyc);
                mw.sigma_floor_cyc = 0.05;
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
        network_config_.parallel_carry =
            jval<bool>(json, "network.parallel_carry", "parallel_carry", false);
        // Guard (Fable lean review, Jul-14 eve): parallel_reduce and parallel_carry are
        // MUTUALLY EXCLUSIVE.  Both OpenMP-fan the per-station passes, and est.solve()'s
        // dense algebra (the fold's GEMMs + the K×K middle inverse) is itself Eigen-
        // threaded — so armed together they nest OpenMP × Eigen inside reduce() and
        // oversubscribe.  Resolve it HERE, at load time (one early, config-visible point,
        // not buried in the epoch driver): force parallel_carry OFF when both are set.  No
        // shipped config arms parallel_reduce, so this only closes the trap.
        if (network_config_.parallel_reduce && network_config_.parallel_carry) {
            log->warn("Network: parallel_reduce AND parallel_carry both set — forcing "
                      "parallel_carry OFF (both OpenMP-fan the per-station passes and "
                      "est.solve() is Eigen-threaded, so together they nest OpenMP × Eigen "
                      "inside reduce()).  Set one false in config to silence this.");
            network_config_.parallel_carry = false;
        }

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
        // One-shot full-arrowhead NEQ export at the Nth processed epoch (<=0 = off).
        network_config_.neq_export_epoch =
            jval<int>(json, "network.neq_export_epoch", "neq_export_epoch", -1);
        network_config_.trace_epoch_status =
            jval<bool>(json, "network.trace_epoch_status", "trace_epoch_status", false);
        // SPEC_day129_frame_guards P1-3/P1-4: trigger instrumentation.  Both are
        // TRACE-ONLY and absent from the resume fingerprint (NetworkRunContext.cpp
        // :172-307) — they may be armed on a warm resume against an untraced snapshot.
        network_config_.trace_sh_spectrum =
            jval<bool>(json, "network.trace_sh_spectrum", "trace_sh_spectrum", false);
        network_config_.trace_sh_spectrum_stride =
            jval<int>(json, "network.trace_sh_spectrum_stride", "trace_sh_spectrum_stride", 60);
        network_config_.trace_postfit_sigma0 =
            jval<bool>(json, "network.trace_postfit_sigma0", "trace_postfit_sigma0", false);
        // §4.3 RHS localization rider: keyed (station / satellite / signal)
        // post-fit pools on the same P1-4 residual pass.  Trace-only,
        // fingerprint-SAFE; implies the pass runs even without the sigma0 trace.
        network_config_.trace_postfit_keyed =
            jval<bool>(json, "network.trace_postfit_keyed", "trace_postfit_keyed", false);
        if (network_config_.trace_postfit_keyed)
            log->info("Network (P2-RHS): network.trace_postfit_keyed ON — per-station / "
                      "per-satellite / per-signal robust post-fit pools -> "
                      "network_pf_keyed.csv (trace-only; the P1-4 residual pass runs "
                      "whenever this or trace_postfit_sigma0 is armed).");
        if (network_config_.trace_sh_spectrum)
            log->info("Network (P1-3): network.trace_sh_spectrum ON — per-epoch reduced-shared-"
                      "normal spectrum + per-family eigenvector mass partition into "
                      "network_sh_spectrum.csv (mode detail every {} traced epoch(s)).  One "
                      "SelfAdjointEigenSolver of N_ss per emitted epoch; measure-only.",
                      network_config_.trace_sh_spectrum_stride);
        if (network_config_.trace_postfit_sigma0)
            log->info("Network (P1-4): network.trace_postfit_sigma0 ON — robust post-fit "
                      "variance factor over EVERY station's residuals (code/phase pools "
                      "separated) appended to network_epoch_status.csv.  MEASURE-ONLY: the A4 "
                      "withhold ceiling still reads the within-epoch RAIM's sigma0_adapt, so "
                      "this changes no decision.");
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
        // RTB-7(c): branch-faithful resume — also serialize the Card-4 invariant DD carry.
        // Raw parse here; the carry_invariant + path requirement is enforced in the
        // carry_invariant validation block below (network_carry_invariant_ is parsed later).
        network_config_.state_snapshot_carry =
            jval<bool>(json, "network.state_snapshot_carry", "state_snapshot_carry", false);
        // RTB-7(e): diagonal-only Pᶻ in the carry blob (write side; the reader is
        // content-driven).  Validated with the rest of the carry block below.
        network_config_.state_snapshot_carry_diagonal =
            jval<bool>(json, "network.state_snapshot_carry_diagonal",
                       "state_snapshot_carry_diagonal", false);
        // RTB-7(d): resume-event response (RE-2), A/B safety (HB-1), blob history (BR-1).
        network_config_.state_snapshot_resume_shock_m =
            jval<double>(json, "network.state_snapshot_resume_shock_m",
                         "state_snapshot_resume_shock_m", 0.0);
        if (network_config_.state_snapshot_resume_shock_m < 0.0) {
            log->warn("Network (RTB-7d): network.state_snapshot_resume_shock_m {} < 0 — "
                      "clamped to 0 (OFF).", network_config_.state_snapshot_resume_shock_m);
            network_config_.state_snapshot_resume_shock_m = 0.0;
        }
        network_config_.state_snapshot_require =
            jval<bool>(json, "network.state_snapshot_require", "state_snapshot_require", false);
        network_config_.state_snapshot_keep_history =
            jval<bool>(json, "network.state_snapshot_keep_history",
                       "state_snapshot_keep_history", false);
        if (network_config_.state_snapshot_require)
            log->info("Network (RTB-7d/HB-1): warm resume REQUIRED — any resume failure "
                      "(missing/parse/fingerprint/age/emitter) is a hard run error instead "
                      "of a silent cold start.  Arm this in every same-snapshot A/B.");
        if (network_config_.state_snapshot_keep_history)
            log->info("Network (RTB-7d/BR-1): per-epoch snapshot history ON — each snapshot "
                      "is also written to '<path>.ep<NNNN>' for branch grading "
                      "(scripts/diff_carry_blobs.py).  Write-side only; heavy on disk.");
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
        // per-(satellite,signal) code biases (all non-GLONASS constellations — GPS,
        // Galileo, BeiDou) and emits them, so the
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

        // Cold-start receiver-clock pre-seed.  On the network path nothing
        // pre-solves the receiver clock before the first ambiguity seed
        // (syncPppStateToStateVector, the PPP-path writer of x(clock), never
        // runs from ApplicationNetwork), so a free-running receiver's whole
        // clock offset leaks UNIFORMLY into every cold ambiguity seed via
        // computeAmbiguitySeed's clk_term=x(clock)=0.  When on, the state
        // pre-pass writes the median GPS code residual into x(clock) once per
        // station.  Default OFF => the seeder is never called (byte-identical).
        network_cold_clock_seed_ = jval<bool>(
            json, "network.cold_clock_seed", "cold_clock_seed", false);
        if (network_cold_clock_seed_) {
            log->info("Network: cold_clock_seed ON — pre-seeding each station's cold "
                      "receiver clock from the median GPS code residual before the first "
                      "ambiguity seed (removes the cold-start clock leak into ambiguity seeds).");
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
                      "emits its own satellite code biases (GPS + Galileo + BeiDou) under a "
                      "sum-to-zero per-constellation DCB datum.");
        } else {
            log->info("Network producer (V1): satellite code bias deferred "
                      "(estimate_sat_biases / promote_sat_code_bias = false)");
        }

        // CB-ADM: run-latched W6 code-bias admission census + station-local
        // quarantine.  Replaces the per-epoch >= 2-station gate (whose count
        // FLAPPING at BeiDou scale churns prune / re-register / re-seed and
        // spawns datum-orphan near-null directions that withhold whole epochs)
        // with a one-shot startup census.  Only effective when the W6 slice is
        // armed; armed-without-W6 is INERT (the code-bias registration block
        // never runs) — warn, per the established armed-but-ineffective pattern.
        network_cb_admission_ =
            jval<bool>(json, "network.cb_admission", "cb_admission", false);
        network_cb_admission_min_stations_ = jval<int>(
            json, "network.cb_admission_min_stations", "cb_admission_min_stations", 4);
        network_cb_admission_census_epochs_ = jval<int>(
            json, "network.cb_admission_census_epochs", "cb_admission_census_epochs", 15);
        if (network_cb_admission_min_stations_ < 1) {
            log->error("Network: network.cb_admission_min_stations must be >= 1 "
                       "(got {}); effective_min = max(2, min(this, n_census_stations)).",
                       network_cb_admission_min_stations_);
            return false;
        }
        if (network_cb_admission_census_epochs_ < 1) {
            log->error("Network: network.cb_admission_census_epochs must be >= 1 (got {}).",
                       network_cb_admission_census_epochs_);
            return false;
        }
        if (network_cb_admission_) {
            if (estimate_network_sat_code_bias_) {
                log->info("Network producer (CB-ADM): W6 code-bias admission census ON — "
                          "latching each (sat,signal) code pair over the first {} loop "
                          "epochs (>= {} supporting stations, each sustained over "
                          ">= ceil(K_eff/2) epochs, and >= 2 distinct bands per satellite "
                          "for the IF pin).  Admitted pairs become shared border columns; "
                          "sub-threshold pairs keep their measurements and their per-station "
                          "sat-bias state as station-local sponges (kept, never pruned, "
                          "never datumed, never emitted).",
                          network_cb_admission_census_epochs_,
                          network_cb_admission_min_stations_);
            } else {
                log->warn("Network: network.cb_admission = true is INERT — the W6 code-bias "
                          "slice is off (network.estimate_sat_code_bias / estimate_iono_sh), "
                          "so no code-bias columns are registered for the census to gate.");
            }
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

        // --- Slip-QC hardening Cards R / L / G (docs/SPEC_slip_qc_hardening.md) ---
        // Three independent, default-OFF qualifications of decisions the detector
        // arms above already make.  None of them drops a station, satellite or
        // observation (project rule 5) and none derives a threshold from the
        // stochastic model (rule 4).  All-OFF ⇒ byte-identical.
        {
            auto& sq = network_config_.slip_qc;
            // Card R — hardened IF-TD reference-satellite gate (fixes the fixed
            // 0.5·λ_IF, n≥2 gate that re-fired every epoch on a sub-slip drift).
            sq.ref_gate = jval<bool>(json, "network.cycle_slip.if_phase_td_ref_gate",
                                     "network_cycle_slip_if_phase_td_ref_gate", false);
            sq.ref_k = jval<double>(json, "network.cycle_slip.if_phase_td_ref_k",
                                    "network_cycle_slip_if_phase_td_ref_k", 6.0);
            sq.ref_min_n = jval<int>(json, "network.cycle_slip.if_phase_td_ref_min_n",
                                     "network_cycle_slip_if_phase_td_ref_min_n", 4);
            sq.ref_thin_widen =
                jval<double>(json, "network.cycle_slip.if_phase_td_ref_thin_widen",
                             "network_cycle_slip_if_phase_td_ref_thin_widen", 3.0);
            sq.ref_coherence =
                jval<double>(json, "network.cycle_slip.if_phase_td_ref_coherence",
                             "network_cycle_slip_if_phase_td_ref_coherence", 0.5);
            sq.ref_max_consecutive =
                jval<int>(json, "network.cycle_slip.if_phase_td_ref_max_consecutive",
                          "network_cycle_slip_if_phase_td_ref_max_consecutive", 1);
            sq.ref_ewma_alpha =
                jval<double>(json, "network.cycle_slip.if_phase_td_ref_ewma_alpha",
                             "network_cycle_slip_if_phase_td_ref_ewma_alpha", 0.15);
            sq.ref_shrink_kappa =
                jval<double>(json, "network.cycle_slip.if_phase_td_ref_shrink_kappa",
                             "network_cycle_slip_if_phase_td_ref_shrink_kappa", 20.0);
            sq.ref_floor_frac =
                jval<double>(json, "network.cycle_slip.if_phase_td_ref_floor_frac",
                             "network_cycle_slip_if_phase_td_ref_floor_frac", 0.5);
            // Card L — LLI corroboration in the MW drain.
            sq.lli_corroborate = jval<bool>(json, "network.cycle_slip.mw_lli_corroborate",
                                            "network_cycle_slip_mw_lli_corroborate", false);
            sq.lli_chatter_frac_hi =
                jval<double>(json, "network.cycle_slip.mw_lli_chatter_frac_hi",
                             "network_cycle_slip_mw_lli_chatter_frac_hi", 0.6);
            sq.lli_chatter_frac_lo =
                jval<double>(json, "network.cycle_slip.mw_lli_chatter_frac_lo",
                             "network_cycle_slip_mw_lli_chatter_frac_lo", 0.3);
            sq.lli_min_epochs = jval<int>(json, "network.cycle_slip.mw_lli_min_epochs",
                                          "network_cycle_slip_mw_lli_min_epochs", 1);
            sq.lli_ewma_alpha = jval<double>(json, "network.cycle_slip.mw_lli_ewma_alpha",
                                             "network_cycle_slip_mw_lli_ewma_alpha", 0.4);
            sq.lli_k_mw = jval<double>(json, "network.cycle_slip.mw_lli_k_mw",
                                       "network_cycle_slip_mw_lli_k_mw", 5.0);
            sq.lli_k_gf = jval<double>(json, "network.cycle_slip.mw_lli_k_gf",
                                       "network_cycle_slip_mw_lli_k_gf", 5.0);
            sq.lli_mw_floor_cyc =
                jval<double>(json, "network.cycle_slip.mw_lli_mw_floor_cyc",
                             "network_cycle_slip_mw_lli_mw_floor_cyc", 0.5);
            sq.lli_gf_floor_m = jval<double>(json, "network.cycle_slip.mw_lli_gf_floor_m",
                                             "network_cycle_slip_mw_lli_gf_floor_m", 0.04);
            sq.lli_step_alpha = jval<double>(json, "network.cycle_slip.mw_lli_step_alpha",
                                             "network_cycle_slip_mw_lli_step_alpha", 0.15);
            // Card G — gap-coincident instrumentation (counters + trace rows only).
            sq.gap_instrument = jval<bool>(json, "network.cycle_slip.gap_instrument",
                                           "network_cycle_slip_gap_instrument", false);

            // Arming diagnostics: a card whose host arm is off is INERT, and a
            // silent inert card is exactly the "clean log, dead wiring" failure
            // standing invariant #4 exists to prevent.
            if (sq.ref_gate && !network_cycle_slip_if_phase_td_) {
                log->warn("Network (slip-QC card R): network.cycle_slip.if_phase_td_ref_gate "
                          "is set but .if_phase_td is OFF — no IF-phase detector is built, so "
                          "the hardened reference gate is INERT this run.");
                sq.ref_gate = false;
            }
            if (sq.lli_corroborate && !network_cycle_slip_mw_detect_) {
                log->warn("Network (slip-QC card L): network.cycle_slip.mw_lli_corroborate is "
                          "set but .mw_detect is OFF — the LLI arm it qualifies never runs, so "
                          "the corroboration gate is INERT this run.");
                sq.lli_corroborate = false;
            }
            if (sq.lli_chatter_frac_lo > sq.lli_chatter_frac_hi) {
                log->warn("Network (slip-QC card L): mw_lli_chatter_frac_lo ({:.2f}) exceeds "
                          "_hi ({:.2f}) — the hysteresis band is inverted and the credibility "
                          "state would chatter.  Clamping _lo to _hi.",
                          sq.lli_chatter_frac_lo, sq.lli_chatter_frac_hi);
                sq.lli_chatter_frac_lo = sq.lli_chatter_frac_hi;
            }
            if (sq.ref_gate)
                log->info("Network (slip-QC card R): hardened IF-TD reference gate ON — fire "
                          "only when |median(non-ref ΔΔ)| > max({:.1f}·s_eff, {:.2f}·λ_IF) AND "
                          "the family agrees (MAD ≤ {:.2f}·|median|); thin families (< {} DDs) "
                          "widen the physical floor {:.1f}× and are counted; at most {} "
                          "consecutive fires per reference before a forced re-selection.",
                          sq.ref_k, sq.ref_floor_frac,
                          sq.ref_coherence, sq.ref_min_n, sq.ref_thin_widen,
                          sq.ref_max_consecutive);
            if (sq.lli_corroborate)
                log->info("Network (slip-QC card L): LLI corroboration ON — an assertion acts "
                          "when an empirical MW step (> max({:.1f}·σ̂, {:.2f} WL cyc)) or GF step "
                          "(> max({:.1f}·σ̂, {:.3f} m)) confirms it, or while the station's "
                          "LLI-set-fraction EWMA (α={:.2f}) stays credible (demote > {:.2f}, "
                          "promote < {:.2f}, arm after {} epoch(s)).  Suppressed assertions are "
                          "emitted with action=suppressed and counted — never dropped silently.",
                          sq.lli_k_mw, sq.lli_mw_floor_cyc, sq.lli_k_gf, sq.lli_gf_floor_m,
                          sq.lli_ewma_alpha, sq.lli_chatter_frac_hi, sq.lli_chatter_frac_lo,
                          sq.lli_min_epochs);
            if (sq.gap_instrument)
                log->info("Network (slip-QC card G): gap-coincident instrumentation ON — "
                          "counting arc resumptions across a tracking break, whether the raw "
                          "MW/GF level shifted, and whether the arc re-entered with BRIDGED "
                          "carry state.  Instrumentation only: zero mechanism change.");
        }
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
        // WE-CI: extend the within-epoch screening to the CODE + IONO-CONSTRAINT
        // row pools.  Those rows historically had NO screening statistic in any
        // tier (the P1-4 pass excludes SH-touching rows; the within-epoch pool is
        // carried-ambiguity phase rows only) — the DOY-129 19:26:00 SH detonation
        // entered through exactly them.  Follows the A4 precedent: arming the
        // extension pulls in the tier it extends.
        network_raim_we_code_iono_ =
            jval<bool>(json, "network.raim_we_code_iono", "network_raim_we_code_iono", false);
        network_raim_we_code_trigger_ =
            jval<double>(json, "network.raim_we_code_trigger", "network_raim_we_code_trigger", 10.0);
        network_raim_we_iono_trigger_ =
            jval<double>(json, "network.raim_we_iono_trigger", "network_raim_we_iono_trigger", 12.0);
        if (network_raim_we_code_iono_ && !network_raim_within_epoch_) {
            log->warn("Network: network.raim_we_code_iono extends the within-epoch adaptation — "
                      "enabling network.raim_within_epoch (the loop the code/iono pools feed).");
            network_raim_within_epoch_ = true;
            if (!network_raim_ || !network_raim_attribution_) {
                network_raim_ = true;
                network_raim_attribution_ = true;
            }
        }
        if (network_raim_we_code_iono_)
            log->info("Network producer (WE-CI): within-epoch CODE+IONO row screening ON — "
                      "per-pool robust σ̂0 with triggers code {:g} / iono {:g}; responses: "
                      "epoch-scoped station exclusion (epicenter class) or network-wide "
                      "satellite code-row de-weight (σ×10, fallback-not-drop).  The phase "
                      "pool, σ̂0_adapt and the A4 ceiling are unchanged.",
                      network_raim_we_code_trigger_, network_raim_we_iono_trigger_);

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
        // SPEC_day129_frame_guards Phase 2.1: CB-carry event response (invariant #1).
        // Solve-determining when armed ⇒ conditional-appended to the resume
        // fingerprint (NetworkRunContext.cpp), so an unarmed run's fingerprint is
        // byte-identical and a resume cannot cross a flip of the toggle.
        network_config_.cb_excursion_reinit =
            jval<bool>(json, "network.cb_excursion_reinit", "cb_excursion_reinit", false);
        network_config_.cb_excursion_median_m =
            jval<double>(json, "network.cb_excursion_median_m", "cb_excursion_median_m", 50.0);
        network_config_.cb_excursion_reinit_all_sats =
            jval<bool>(json, "network.cb_excursion_reinit_all_sats",
                       "cb_excursion_reinit_all_sats", false);
        if (network_config_.cb_excursion_reinit) {
            if (!(network_config_.cb_excursion_median_m > 0.0)) {
                log->error("Network: network.cb_excursion_reinit is on but "
                           "cb_excursion_median_m = {} is not > 0 — the detector would "
                           "trip on every epoch.  Disabling the guard.",
                           network_config_.cb_excursion_median_m);
                network_config_.cb_excursion_reinit = false;
            } else if (!estimate_network_sat_code_bias_) {
                log->warn("Network: network.cb_excursion_reinit is on but "
                          "estimate_sat_code_bias is off — there is no satellite "
                          "code-bias carry to reinitialize, so the guard is INERT.");
            } else {
                log->info("Network (Phase 2.1): CB excursion reinit ARMED — a per-epoch "
                          "network-wide common-mode trip (median |Δclock| over satellites "
                          "> {:.1f} m) marks the {} code-bias carry for REINIT "
                          "(inflate_q, σ={:.0f} m next epoch) and queues a code-bias "
                          "discontinuity bump at the following emit.  Reset-not-repair; "
                          "no measurement is dropped.  Datum-change epochs are exempt "
                          "(a handover moves every clock — a datum artifact, not a fault).",
                          network_config_.cb_excursion_median_m,
                          network_config_.cb_excursion_reinit_all_sats
                              ? "ALL satellites'" : "clock-PIVOT satellite's",
                          network_clock_monitor_reinit_sigma_m_);
            }
        }

        // SPEC_day129_frame_guards Phase 2.2: SH-field health GATE (gate-not-warn
        // rework of the T1a a00 band probe; handoff §7).  EMISSION-ONLY — the solve
        // and carry are untouched — so it is NOT in the resume fingerprint (same
        // class as raim / clock_runaway_check).
        network_config_.sh_health_gate =
            jval<bool>(json, "network.sh_health_gate", "sh_health_gate", false);
        network_config_.sh_gate_min_epochs =
            jval<int>(json, "network.sh_gate_min_epochs", "sh_gate_min_epochs", 1);
        network_config_.sh_gate_rms_max_tecu =
            jval<double>(json, "network.sh_gate_rms_max_tecu", "sh_gate_rms_max_tecu", 0.0);
        if (network_config_.sh_health_gate) {
            const GlobalVtecShConfig& shc_gate = global_vtec_sh_;
            const bool band_active = shc_gate.a00_warn_low_tecu < shc_gate.a00_warn_high_tecu;
            const bool rms_active  = network_config_.sh_gate_rms_max_tecu > 0.0;
            if (network_config_.sh_gate_min_epochs < 1) {
                log->warn("Network: network.sh_gate_min_epochs = {} clamped to 1.",
                          network_config_.sh_gate_min_epochs);
                network_config_.sh_gate_min_epochs = 1;
            }
            if (!estimate_network_iono_sh_) {
                log->warn("Network: network.sh_health_gate is on but estimate_iono_sh is "
                          "off — there is no SH field to band-check, so the gate is INERT "
                          "(disabled).");
                network_config_.sh_health_gate = false;
            } else if (!band_active && !rms_active) {
                log->warn("Network: network.sh_health_gate is on but the a00 band is "
                          "silenced (low {} >= high {}) and sh_gate_rms_max_tecu is 0 — "
                          "no active condition, so the gate is INERT (disabled).",
                          shc_gate.a00_warn_low_tecu, shc_gate.a00_warn_high_tecu);
                network_config_.sh_health_gate = false;
            } else {
                log->info("Network (Phase 2.2): SH health GATE armed — an epoch with total "
                          "a00 outside [{:.1f}, {:.1f}] TECU{} for >= {} consecutive "
                          "epoch(s) is WITHHELD from emission (solve/carry/QC continue; "
                          "counted per EPOCH, transitions logged un-throttled).",
                          shc_gate.a00_warn_low_tecu, shc_gate.a00_warn_high_tecu,
                          rms_active ? fmt::format(" (or rms_all > {:.1f} TECU)",
                                                   network_config_.sh_gate_rms_max_tecu)
                                     : std::string{},
                          network_config_.sh_gate_min_epochs);
            }
        }


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
        // Emit C1 (clock rate) from the solved clock WITHOUT the two-state carry prior — the
        // recalibration-supported alternative to network.clock_model (which is a no-go: the
        // estimable is white-FM, not RWFM, so the carry buys ~1× yet risks a NIS storm; the
        // exploitable stability is the deterministic RATE, emitted here without carrying).
        network_emit_clock_rate_ =
            jval<bool>(json, "network.emit_clock_rate", "network_emit_clock_rate", false);
        if (network_emit_clock_rate_ && network_clock_model_)
            log->info("Network: network.emit_clock_rate is on but network.clock_model already "
                      "supplies C1 from its two-state drift — emit_clock_rate is inert this run "
                      "(clock_model wins).");
        else if (network_emit_clock_rate_) {
            log->info("Network: network.emit_clock_rate on — C1 = finite-differenced solved clock "
                      "(reseed / drift-reset / C1-ZOH bound {:.0f} s); solution byte-identical, only "
                      "the emitted C1 changes. Consumer extrapolates c0 + c1·dt (SSR saw-tooth fix).",
                      network_clock_model_max_gap_s_);
            // Inert-arming guard: every FD pair spans one emitted-epoch gap, so if the
            // configured epoch spacing already exceeds the C1-ZOH bound, EVERY pair is
            // stale — the toggle silently ships C1=0 all run.  Only the config-determinable
            // spacing (gnss_rate_s × epoch_decimation_n) is checked here; with
            // gnss_rate_s = 0 the native RINEX rate is unknown until data is read, so a
            // decimated native-rate run (the 300 s dev tier) is not catchable at load.
            const double known_spacing_s =
                gnss_rate_s_ * static_cast<double>(network_config_.epoch_decimation_n);
            if (known_spacing_s > network_clock_model_max_gap_s_)
                log->info("Network: emit_clock_rate is INERT at this cadence — the epoch spacing "
                          "({:.0f} s = gnss_rate_s {:.0f} × epoch_decimation_n {}) exceeds the "
                          "{:.0f} s C1-ZOH bound (clock_model_max_gap_s), so every finite-difference "
                          "pair is stale and C1 stays 0 (ZOH). Raise clock_model_max_gap_s or run at "
                          "a higher rate (the rate asset is high-rate anyway — see "
                          "docs/CLOCK_HADAMARD_CALIB).",
                          known_spacing_s, gnss_rate_s_, network_config_.epoch_decimation_n,
                          network_clock_model_max_gap_s_);
        }
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
        // RTB-7(c): the branch-faithful carry blob requires the invariant carry to be
        // armed and a snapshot path to write to (parsed above); enforce + log here now
        // that network_carry_invariant_ is known.
        if (network_config_.state_snapshot_carry) {
            if (network_config_.state_snapshot_path.empty() || !network_carry_invariant_) {
                log->warn("Network (RTB-7c): network.state_snapshot_carry requires "
                          "state_snapshot_path + carry_invariant; disabling (the carry blob "
                          "would be inert).");
                network_config_.state_snapshot_carry = false;
                network_config_.state_snapshot_carry_diagonal = false;  // RTB-7(e): no blob to shape
            } else {
                log->info("Network (RTB-7c): BRANCH-FAITHFUL resume ON — the invariant DD "
                          "carry (z-store means + {} Pᶻ + vertex/group numbering) is "
                          "serialized into the snapshot, so a warm resume reproduces the "
                          "phase-ambiguity branch exactly instead of re-converging onto a "
                          "different one.  Snapshot grows with the carried DD {}.",
                          network_config_.state_snapshot_carry_diagonal ? "DIAGONAL"
                                                                        : "dense",
                          network_config_.state_snapshot_carry_diagonal
                              ? std::string("count (LINEARLY — blob v3)")
                              : std::string("count"));
                if (network_config_.state_snapshot_carry_diagonal)
                    log->info("Network (RTB-7e): carry blob DIAGONAL-ONLY (v3) — only each "
                              "component's Pᶻ diagonal is written (Σ K_c numbers instead of "
                              "Σ K_c², ~1663x smaller at 305 stations).  The restored means "
                              "are exact; chord CROSS-covariances start at zero and are "
                              "re-earned from data.  This is a measured trade (a diagonal "
                              "prior tightens correlated chords' common mode) — grade it on "
                              "datum-invariant quantities before trusting it.  Reading is "
                              "unaffected: v1/v2/v3 blobs all restore under either setting.");
                if (network_config_.state_snapshot_resume_shock_m > 0.0)
                    log->info("Network (RTB-7d/RE-2): resume EVENT RESPONSE armed — σ = "
                              "{:.3f} m added to every restored DD's Pᶻ diagonal at resume "
                              "(variance {:.4g} m²).  The branch (means + cross-covariance) "
                              "is kept; the widened own-variance lets the fold absorb the "
                              "un-restored station layer's re-convergence instead of the D5 "
                              "gate discarding the branch — gate stays armed (invariant #1).",
                              network_config_.state_snapshot_resume_shock_m,
                              network_config_.state_snapshot_resume_shock_m
                                  * network_config_.state_snapshot_resume_shock_m);
            }
        } else {
            if (network_config_.state_snapshot_resume_shock_m > 0.0)
                log->warn("Network (RTB-7d/RE-2): state_snapshot_resume_shock_m {:.3f} is "
                          "INERT without state_snapshot_carry (nothing is restored to "
                          "shock).", network_config_.state_snapshot_resume_shock_m);
            if (network_config_.state_snapshot_carry_diagonal) {
                log->warn("Network (RTB-7e): state_snapshot_carry_diagonal is INERT without "
                          "state_snapshot_carry (no carry blob is written to shape).");
                network_config_.state_snapshot_carry_diagonal = false;
            }
        }
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
        network_emit_pb_filter_gate_v2_ =
            jval<bool>(json, "network.emit_pb_filter_gate_v2",
                       "network_emit_pb_filter_gate_v2", false);
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
        // Card KP (docs/SPEC_beidou_w6_datum.md §4) — the CONDITIONAL second lever,
        // behind the DG rank-gate calibration and the PA drop-and-heal.  Default
        // OFF; INERT without estimate_sat_code_bias (no OSB columns exist).
        network_cb_kaula_prior_ =
            jval<bool>(json, "network.cb_kaula_prior", "network_cb_kaula_prior", false);
        network_cb_kaula_sigma_m_ =
            jval<double>(json, "network.cb_kaula_sigma_m",
                         "network_cb_kaula_sigma_m", 10.0);
        if (network_cb_kaula_prior_ && !(network_cb_kaula_sigma_m_ > 0.0)) {
            // info = 1/sigma^2 with sigma <= 0 is a HARD pin on every admitted OSB,
            // i.e. a silent datum takeover wearing a "loose prior" label.  Refuse.
            log->error("Network: network.cb_kaula_sigma_m must be > 0 (got {:g}); it is the "
                       "standard deviation of the admitted-OSB physical-bound prior "
                       "(info = 1/sigma^2).  Default 10.0 m (~33 ns).",
                       network_cb_kaula_sigma_m_);
            return false;
        }
        if (network_cb_kaula_prior_) {
            if (!estimate_network_sat_code_bias_)
                log->warn("Network (Card KP): network.cb_kaula_prior is ON but "
                          "network.estimate_sat_code_bias is OFF — there are no satellite "
                          "code-bias columns to put a prior on, so the toggle is INERT.  "
                          "[Standing invariant #4: an armed card with nothing to act on is "
                          "a miswire, not a quiet run.]");
            else
                log->info("Network producer (Card KP): admitted-OSB physical-bound prior ON — "
                          "value 0, sigma {:g} m (info {:g}) on every REGISTERED satellite "
                          "code-bias column, folded through the diagonal prior channel (NOT "
                          "the tight code-bias gauge rows), so the declared S-basis dimension "
                          "is unchanged.  Targets a CODE-dominated near-null direction the "
                          "phase drop-and-heal cannot reach (it anchors PB columns only).  "
                          "The K2 anti-fragility margin vs the armed rank-gate cutoff is "
                          "computed and logged at the first fold.",
                          network_cb_kaula_sigma_m_,
                          1.0 / (network_cb_kaula_sigma_m_ * network_cb_kaula_sigma_m_));
        }
        // Card DG (docs/SPEC_beidou_w6_datum.md §2) — the shared-border null-space
        // rank gate's RELATIVE eigenvalue tolerance becomes a per-config
        // calibration parameter.  Default 1e-9 == NetworkEstimator::kNullSpaceTol
        // (which stays the single source of that default), so an unset key sends
        // the identical double through the identical arithmetic: byte-identical.
        // The resolved value is handed to NetworkEstimator::setRankGateTol once,
        // pre-loop, and read from there by ALL THREE consumers (gate, estimability
        // dump, prioritized drop-and-heal) — DG2's "no private tolerances" rule.
        network_rank_gate_tol_ =
            jval<double>(json, "network.rank_gate_tol",
                         "network_rank_gate_tol", 1e-9);
        if (!(network_rank_gate_tol_ > 0.0)) {
            // NaN also lands here (every comparison false) — deliberately, since a
            // NaN cutoff makes every `ev < cutoff` test false and would report a
            // rank-0 null space on a perfectly healthy system.
            log->error("Network: network.rank_gate_tol must be > 0 (got {:g}); it is the "
                       "RELATIVE eigenvalue tolerance of the shared null-space rank gate "
                       "(cutoff = tol x max(lambda_max, 1)).  Default 1e-9.",
                       network_rank_gate_tol_);
            return false;
        }
        if (network_rank_gate_tol_ < 1e-16) {
            // Clamp rather than fail: a tolerance below double's relative
            // resolution against lambda_max cannot count ANY direction as null,
            // which silently converts the gate into "declared_dim must be 0" — an
            // OFF switch disguised as a tuning value.  Standing invariant #4: an
            // inert safety gate must never look clean.
            log->warn("Network: network.rank_gate_tol {:g} is below the 1e-16 floor and was "
                      "clamped to 1e-16 — a cutoff that deep counts NO direction as null, "
                      "which turns the rank gate off rather than tuning it.  Place the "
                      "value from the config's own estimability dump (>= 2 decades above "
                      "the gauge depth, >= 2 decades below the weakest-real layer).",
                      network_rank_gate_tol_);
            network_rank_gate_tol_ = 1e-16;
        }
        if (network_rank_gate_tol_ != 1e-9)
            log->info("Network producer: rank-gate tolerance CALIBRATED to {:g} (default 1e-9) "
                      "— cutoff = {:g} x max(lambda_max, 1), shared by the gate, the "
                      "estimability dump and the prioritized drop-and-heal.  This is an "
                      "exact-equality gate that throws in BOTH directions: a value below "
                      "this config's own gauge depth stops counting the declared gauges and "
                      "withholds a HEALTHY config.  Justify it from this config's "
                      "network_estimability.csv (network.trace_estimability).",
                      network_rank_gate_tol_, network_rank_gate_tol_);
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
        // M5 card WL-1: the WL layer rides the Card-4 chord basis, so v1 REQUIRES the
        // carry (mutex-check, SPEC §11 Q1).  Fail-loud rather than silently no-op.
        if (network_config_.mw_wl.enable && !network_carry_invariant_) {
            log->error("Network: network.mw_wl.enable requires network.carry_invariant — the "
                       "wide-lane pseudo-observations are double-differenced over the Card-4 "
                       "invariant carry's fundamental-cycle chord basis; with no carry there is "
                       "no cycle basis to seed.  Enable carry_invariant or disable mw_wl.");
            return false;
        }
        if (network_config_.mw_wl.enable) {
            log->info("Network MW-WL seed ON (M5 card WL-1) — MW Welford means DD'd over the "
                      "Card-4 chord basis fold as WL pseudo-obs (min_arc_epochs={}, "
                      "sigma_floor={} cyc, max_rows_per_epoch={}).  GLONASS excluded (with the "
                      "carry).  Per-station MW detector is built even with phase_leveling off.",
                      network_config_.mw_wl.min_arc_epochs, network_config_.mw_wl.sigma_floor_cyc,
                      network_config_.mw_wl.max_rows_per_epoch);
            // Phase B (WL integer fixing) — config contract only in v1.  The un-fix
            // wiring (standing invariant #1: no stiff integer assertion without a
            // wired+force-fire-tested slip response) is a follow-up increment; warn
            // rather than silently fold nothing.
            if (network_config_.mw_wl.fix_enable)
                log->warn("Network: network.mw_wl.fix_enable is set but Phase-B WL integer "
                          "fixing is NOT yet implemented (v1 is Phase-A float only) — the fix "
                          "gate is INERT.  Grade Phase A first (SPEC §7.7), then arm Phase B "
                          "once its un-fix wiring is force-fire tested (SPEC §7.4).");
            if (network_config_.mw_wl.f6_dump)
                log->info("Network: network.mw_wl.f6_dump ON — one-shot F6 attribution dump "
                          "to logs/network_mw_wl_f6.csv on the first arming epoch (per-arc "
                          "MW̄/x_lin + per-row z_MW/z_lin/z_corr/reconstructed_abs/innovation). "
                          "Log-only; does not change the solve.");
        } else if (network_config_.mw_wl.fix_enable) {
            log->warn("Network: network.mw_wl.fix_enable is INERT without network.mw_wl.enable "
                      "(Phase B requires Phase A).");
        }
        // ---- M5 card WL-2: EWL-first tri-frequency cascade (SPEC_ewl_wl_cascade_card.md). ----
        {
            auto& mw = network_config_.mw_wl;
            // Both lanes ride the Card-4 chord basis (EWL rows on it; inject edits the
            // carried z's), so both REQUIRE the carry — fail-loud like WL-1.
            if ((mw.ewl_enable || mw.wl_inject_enable) && !network_carry_invariant_) {
                log->error("Network: network.mw_wl.{{ewl_enable,wl_inject_enable}} require "
                           "network.carry_invariant — the EWL/WL lanes ride the invariant carry's "
                           "fundamental-cycle chord basis (and the WL inject edits its carried DDs). "
                           "Enable carry_invariant or disable the WL-2 lanes.");
                return false;
            }
            // The WL bridge path (§3.4) needs the EWL integers; without ewl_enable the WL
            // lane runs MW(1,2)-fallback only (§3.5) — allowed, but warn so it's intentional.
            if (mw.wl_inject_enable && !mw.ewl_enable)
                log->warn("Network: network.mw_wl.wl_inject_enable is ON without ewl_enable — the "
                          "phase-bridge WL path (SPEC §3.4) is disabled; the WL lane runs the plain "
                          "MW(1,2) fallback only (SPEC §3.5, FFR at n≈25–50 ep).  Set ewl_enable for "
                          "the fast tri-frequency bridge.");
            // WL-1's float layer (mw.enable) is superseded by WL-2 (SPEC §3.0.2): it stays as a
            // default-OFF diagnostic; running it WITH the inject double-drives the WL columns.
            if (mw.enable && mw.wl_inject_enable)
                log->warn("Network: network.mw_wl.enable (WL-1 float rows) is a DIAGNOSTIC under "
                          "WL-2 and is armed together with wl_inject_enable — the float pseudo-obs "
                          "and the inject both drive the WL columns (F6: the float layer loses and "
                          "adds churn).  Disable mw_wl.enable for a clean WL-2 A/B.");
            // Deprecate the WL-1 Phase-B posterior-gated surface — WL-2 fixes on DETECTOR
            // statistics (SPEC §3.0.1), never the carry-contaminated posterior.
            if ((mw.ewl_enable || mw.wl_inject_enable) && mw.fix_enable)
                log->warn("Network: network.mw_wl.fix_enable (WL-1 Phase-B, never implemented) is "
                          "REPLACED under WL-2 by statistic-gated fixing "
                          "(ewl_fix_*/wl_fix_*/inject_disagree_min_cyc).  The fix_enable flag is "
                          "ignored.");
            if (mw.ewl_enable)
                log->info("Network WL-2 EWL lane ON (M5 card WL-2) — extra-wide-lane statistic rows "
                          "on the Card-4 chord basis, third band UNCARRIED so they win cleanly; FFR "
                          "fix at σ<{} / |frac|<{} cyc, eligible at n>={} (ep ~{}).  Requires "
                          "frequency_mode with the 3rd band (GPS L5 / Galileo E5b).",
                          mw.ewl_fix_sigma_cyc, mw.ewl_fix_frac_cyc, mw.ewl_min_arc_epochs,
                          mw.ewl_min_arc_epochs + 1);
            if (mw.wl_inject_enable)
                log->info("Network WL-2 WL inject ON (M5 card WL-2) — WL integers (EWL-bridge {} / "
                          "MW(1,2) fallback) injected into the Card-4 carry via injectLaneTarget "
                          "(set ∇ΔN₁−∇ΔN₂, preserve NL) + hold row + 1-epoch D5 exemption; trigger "
                          "|stat−carried|>{} cyc, FFR σ<{}/|frac|<{}, bridge σ guard {} cyc.",
                          mw.ewl_enable ? "armed" : "OFF", mw.inject_disagree_min_cyc,
                          mw.wl_fix_sigma_cyc, mw.wl_fix_frac_cyc, mw.bridge_max_sigma_cyc);
            // ---- M5 card UWL-1: per-constellation ultra-wide-lane top tier (SPEC_uwl_card.md). ----
            if (mw.uwl_enable && !mw.ewl_enable) {
                log->error("Network: network.mw_wl.uwl_enable requires network.mw_wl.ewl_enable — "
                           "the UWL top tier IS the EWL machinery run on the per-constellation "
                           "lane-tier pair (statistic layer, FFR gates, bridge, telemetry).  "
                           "Enable ewl_enable or disable uwl_enable.");
                return false;
            }
            if (mw.uwl_cold_fix && !mw.uwl_enable)
                log->warn("Network: network.mw_wl.uwl_cold_fix is INERT without uwl_enable — the "
                          "n=1 cold fix applies only to lane tiers with λ ≥ {} m, which only the "
                          "UWL table provides (GPS's 5.861 m EWL is excluded by design).",
                          mw.uwl_cold_min_lambda_m);
            if (mw.uwl_mid_enable && !mw.uwl_enable)
                log->warn("Network: network.mw_wl.uwl_mid_enable is INERT without uwl_enable — "
                          "the E6 mid-ladder hop rides the armed lane-tier table (Galileo "
                          "(E6,E5a) λ 2.931 m rung).");
            // ---- Increment 5: dual-top tier contract. ----
            if (mw.uwl_dual_top && !mw.uwl_enable)
                log->warn("Network: network.mw_wl.uwl_dual_top is INERT without uwl_enable — "
                          "the preferred/fallback top rung is a property of the armed "
                          "lane-tier table.");
            if (mw.uwl_dual_top && mw.uwl_enable && mw.ewl_fold_rows) {
                log->warn("Network: network.mw_wl.ewl_fold_rows is FORCED OFF under "
                          "uwl_dual_top — a cycle decided by both the preferred and the "
                          "fallback rung must never fold two integer rows (the F-A refold "
                          "class), and fold-off is the validated production shape anyway.");
                mw.ewl_fold_rows = false;
            }
            if (mw.uwl_dual_top && mw.uwl_enable
                && jval<int>(json, "network.mw_wl.uwl_gal_top_band",
                             "network_mw_wl_uwl_gal_top_band", 7) != 7)
                log->info("Network: network.mw_wl.uwl_gal_top_band is IGNORED under "
                          "uwl_dual_top — the dual tier always prefers '8' (E5 AltBOC, "
                          "λ 19.537 m) per cycle and falls back to '7' (E5b, λ 9.768 m).");
            if (mw.uwl_sib_gate && !(mw.uwl_dual_top && mw.uwl_enable))
                log->warn("Network: network.mw_wl.uwl_sib_gate is INERT without uwl_dual_top — "
                          "the sibling closure gate compares the untaken {7,8} rung against "
                          "the decided one, which only exists under the dual tier.");
            // ---- v2: per-family lane pair groups contract. ----
            if (mw.uwl_family_pairs
                && !(mw.uwl_enable && network_config_.lane_carry.enable))
                log->warn("Network: network.mw_wl.uwl_family_pairs is INERT without uwl_enable "
                          "AND network.lane_carry.enable — the admissible-pair census extends "
                          "the JOINT pair census (uwl) and the family groups are lane-carry "
                          "pair groups.");
            // ---- Per-sample bridge de-bias (60-ep age-bias fix) contract. ----
            if (mw.bridge_sample_debias && !mw.ewl_enable)
                log->warn("Network: network.mw_wl.bridge_sample_debias is INERT without "
                          "ewl_enable — it changes how the WL-2 GF bridge accumulates.");
            if (mw.bridge_sample_debias && mw.ewl_enable)
                log->info("Network bridge PER-SAMPLE iono de-bias ON (60-ep age-bias fix) — "
                          "bridge samples fold as GF_k − C_iono·ı̃(t_k) (estimable-iono, "
                          "one-epoch-lagged push; Σc·ı̃ over a closed cycle = ∇ΔI exactly, "
                          "Odijk 2016 Table 8); the read-time de-bias is disabled.  The "
                          "decision statistic is stationary at arbitrary arc age.");
            if (mw.uwl_enable && mw.uwl_mid_enable)
                log->info("Network UWL-1 E6 MID HOP ON — the Galileo WL determination chains "
                          "top({}) → (E6,E5a) λ 2.931 m (rounded mid integer) → WL λ 0.751 m, "
                          "cutting each bridge hop's λ amplification ~3.3×; the direct hop "
                          "remains the per-cycle fallback + cross-check, MW(6,5) is the mid "
                          "closure.  Requires frequency_mode covering Galileo E6.",
                          mw.uwl_gal_top_band == '8' ? "E5-AltBOC, 19.537 m" : "E5b, 9.768 m");
            if (mw.uwl_enable)
                log->info("Network UWL-1 top tier ON (M5 card UWL-1) — per-constellation lane-tier "
                          "table armed: GPS EWL (L2,L5) λ 5.861 m; Galileo UWL ({},E5a) λ {:.3f} m; "
                          "BDS-3 deferred (clock divergence), GLONASS excluded (FDMA).  Cold fix "
                          "{} (σ_apriori={} m, λ floor={} m).  Requires frequency_mode covering "
                          "the tier bands (Galileo E5a+{}).",
                          mw.uwl_dual_top ? "E5-AltBOC pref | E5b fallback"
                              : mw.uwl_gal_top_band == '8' ? "E5-AltBOC" : "E5b",
                          mw.uwl_dual_top ? 19.537
                              : mw.uwl_gal_top_band == '8' ? 19.537 : 9.768,
                          mw.uwl_cold_fix ? "ON" : "off",
                          mw.uwl_cold_sigma_m, mw.uwl_cold_min_lambda_m,
                          mw.uwl_dual_top ? "E5b+E5(AltBOC)"
                              : mw.uwl_gal_top_band == '8' ? "E5(AltBOC)" : "E5b");
            if (mw.uwl_enable && mw.uwl_dual_top)
                log->info("Network UWL-1 DUAL-TOP tier ON (increment 5) — Galileo prefers the "
                          "(E5-AltBOC,E5a) λ 19.537 m rung per CYCLE where band 8 is tracked "
                          "across every arc, falls back to (E5b,E5a) λ 9.768 m otherwise; both "
                          "{{7,8}} uncarried; the untaken sibling is a closure statistic "
                          "(2·N(8,5) = N(7,5)) — hard gate {}.",
                          mw.uwl_sib_gate ? "ON (0.5+3σ_sib)" : "OFF (measure-only)");
            if (mw.uwl_family_pairs && mw.uwl_enable && network_config_.lane_carry.enable)
                log->info("Network UWL-1 FAMILY PAIR GROUPS ON (v2 census recovery) — minority "
                          "receiver-family primary pairs with joint-census count ≥ {} join "
                          "their OWN lane pair groups (latched for the run); majority-pair "
                          "steering unchanged.  Recovers the skip_basis population the single-"
                          "pair census left per-band.",
                          mw.uwl_family_min_arcs);
            // ---- M5 card WL-3: lane-coordinate carry (SPEC_lane_carry_card.md). ----
            auto& lc = network_config_.lane_carry;
            if (lc.enable && (!network_carry_invariant_ || !mw.ewl_enable)) {
                log->error("Network: network.lane_carry.enable requires network.carry_invariant "
                           "AND network.mw_wl.ewl_enable — the lane-coordinate carry restructures "
                           "the invariant carry's pair groups and consumes the WL-2 MW/bridge "
                           "statistics as its lane measurements.  Enable both or disable "
                           "lane_carry.");
                return false;
            }
            if (lc.enable && mw.wl_inject_enable)
                log->warn("Network: network.lane_carry SUPERSEDES network.mw_wl.wl_inject_enable "
                          "at runtime (SPEC_lane_carry_card.md §3.3) — the WL-2 hold/inject walk "
                          "is replaced by continuous lane increments + fold-on-change fixing; "
                          "injectCarriedChord remains only as the pair-block fallback.  Disable "
                          "wl_inject_enable for a clean lane-carry shape.");
            if (lc.enable) {
                const bool equivalence_shape = lc.q_wl_scale == 1.0
                                            && lc.sigma_wl_fresh_cyc <= 0.0
                                            && !lc.lane_meas && !lc.bridge_meas;
                log->info("Network WL-3 lane-coordinate carry ON (M5 card WL-3) — primary-pair "
                          "chords carried as jointly-covarianced (band-1, band-2) blocks; shape = "
                          "{}.  q_wl_scale={}, sigma_wl_fresh={} cyc, lane_meas={}, bridge_meas={}, "
                          "fold_blocked={}.",
                          equivalence_shape
                              ? "EQUIVALENCE (SPEC §6.1: split per-band emission, no lane "
                                "features — the structural-parity gate; ≤1e-9 equivalence holds "
                                "in the unit harness, field runs diverge benignly via cycle-path-"
                                "dependent membership, see HANDOFF_lane_carry_card.md)"
                              : "LANE (joint 2K blocks, diffuse lane birth, per-epoch MW/bridge "
                                "increments, per-decided-epoch idempotent FFR lane re-pin)",
                          lc.q_wl_scale, lc.sigma_wl_fresh_cyc,
                          lc.lane_meas ? "true" : "false",
                          lc.bridge_meas ? "true" : "false",
                          lc.fold_blocked ? "true (LF-1 INC-1: O(K) sparse SET + hoisted symmetrize; "
                                            "EQUIVALENCE class, not bit-identical)" : "false");
                if (lc.skip_pinned_increments)
                    log->info("Network WL-3 lane-fold LF-1 INC-2: pinned-cycle increment skip ON "
                              "(within_n={} epochs) — committed-decided chords skip MW/bridge "
                              "increments; any slip/D5/re-tap resumes them.  EQUIVALENCE class.",
                              lc.skip_pinned_within_n);
                if (lc.parallel_components)
                    log->info("Network WL-3 lane-fold LF-1 INC-4: component-parallel MW fold ON "
                              "(one joint block per thread; needs parallel_stations).  "
                              "BYTE-IDENTICAL to serial.");
            }
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
                if (network_emit_pb_filter_gate_v2_ && network_carry_invariant_emit_rebase_)
                    log->info("Network producer: PB filter GATE v2 ON — data/model-driven reset "
                              "replacing the v1 per-cadence hand k/seed: (a) CENSORING-FREE σ̂_step feed "
                              "(innovation-gated rows feed too, killing the v1 low-σ̂ death-spiral), "
                              "(b) MODEL-SCALED gate |innov| > k·√(Ppred+R) so dt enters via the prior's "
                              "q·gap term and ONE k={:.1f} transfers 30 s ↔ 300 s, (c) seed σ̂_step "
                              "demoted to a numerical guard (floor ~1 mm).  OFF = v1 (k·σ̂_step, "
                              "smoothed-rows-only feed).  Retire any per-rate emit_pb_filter_jump_k / "
                              "_meas_sigma_m override once the 30 s+300 s A/B transfer (G5) passes.",
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
            // Q5 replay: consume an already-written product file instead of the
            // live producer.  Presence alone arms the replay driver (the enable
            // flag still gates the station validation below), so a config that
            // sets it is unambiguously a consumer-only run.
            clc.replay_products_path     = cl.value("replay_products_path", std::string{});
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
                } else if (!clc.replay_products_path.empty()) {
                    log->info("Network closed-loop REPLAY (Q5) armed: consumer '{}' vs product "
                              "file '{}' (AR mode {}, wl_only {}) — the joint producer does NOT "
                              "run; runNetwork short-circuits to the consumer-only driver.",
                              clc.station.name, clc.replay_products_path, clc.ar_mode,
                              clc.ar_wl_only ? "on" : "off");
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
            nar.ils_max_dim          = jval<int>   (json, "network.ambiguity_resolution.ils_max_dim",          "network_ar_ils_max_dim",          0);
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
        logNetworkPosture_(log);
        return true;
    }

    // =================================================================
    //  POSTURE readout — what the field record recommends vs what is armed
    // =================================================================
    /**
     * @brief Log, once per run, every knob the field record recommends for THIS run's regime
     *        and whether it is actually ON — plus the integrity of each COUPLED bundle.
     *
     * Motivation (user, 2026-07-29): the project ships new work default-OFF and
     * byte-identity-gated, which is right for new work but leaves no PROMOTION step, so
     * validated-good options accumulate in the OFF column and are re-discovered by accident
     * one wasted run at a time.  The loader already logs everything it ARMS; this logs the
     * inverse, so a run cannot start without the gap being visible.
     *
     * Two design rules, both paid for with data:
     *  - **Regime, not blanket.** A knob validated at one scale can be harmful at another
     *    (`mw_wl.bridge_sample_debias`: validated at 50 stations, net-harmful at 305 —
     *    short-baseline wrong-integer rate 0.0 % → 12.5 %).  Every entry carries the regime
     *    it was validated in and is only reported when that regime matches this run.
     *  - **Bundles, not knobs.** Several options are only correct TOGETHER; arming a strict
     *    subset is its own failure mode (the MW/WL lane stack without `cold_clock_seed`
     *    quarantines its lane cycles — measured `x_lin` 269 km at 305 stations).  A partially
     *    armed bundle WARNs.
     *
     * Log-only: reads already-parsed state and changes nothing.  Full evidence and the
     * SCOPED/UNVALIDATED tiers live in `docs/CONFIG_promotion_ledger.md`; check any config
     * offline with `scripts/config_posture_check.py`.
     *
     * @param log  the "gnss_config" logger.
     */
    void GnssConfig::logNetworkPosture_(const std::shared_ptr<spdlog::logger>& log) const {
        const auto&       nc    = network_config_;
        const std::size_t n_sta = nc.stations.size();
        const std::size_t n_sys = active_systems_.size();
        const bool tri   = frequency_mode_ == FrequencyMode::Triple
                        || frequency_mode_ == FrequencyMode::All;
        const bool warm  = !nc.state_snapshot_path.empty();
        const bool lane  = nc.mw_wl.ewl_enable || nc.mw_wl.enable || nc.lane_carry.enable;

        struct Item { bool applies; bool on; const char* knob; const char* why; };
        const std::vector<Item> items = {
            // --- Tier 1: recommended wherever they apply -------------------------------
            { true, network_carry_invariant_, "network.carry_invariant",
              "the ONLY cross-epoch channel this per-epoch estimator has; consumer float 7-10x" },
            { network_carry_invariant_, network_carry_invariant_innov_gate_,
              "network.carry_invariant_innov_gate",
              "a zero-q carry with no jump response warps the solution (standing invariant #1)" },
            { network_carry_invariant_innov_gate_, network_carry_invariant_innov_calibrate_,
              "network.carry_invariant_innov_calibrate",
              "the default innov_k=4.0 is only correct WITH calibrate; raw-Cov(v) wanted k=10" },
            { true, network_phase_leveling_, "network.phase_leveling",
              "without it phase post-fit residuals are identically zero — no QC statistic exists" },
            { true, network_raim_, "network.raim",
              "USER DIRECTIVE Jul-28: armed residual QC on every network run" },
            { network_raim_, network_raim_within_epoch_, "network.raim_within_epoch",
              "adapt-and-EMIT inside the epoch; also the only source of sigma0_adapt" },
            { true, network_station_pd_pin_aware_, "network.station_pd_pin_aware",
              "305-sta 3h: 884 spurious pin-driven station rejections -> 0, clock better on every metric" },
            { n_sta >= 3 && tri, estimate_network_iono_sh_, "network.estimate_iono_sh",
              "co-estimated SH-VTEC replaces the external GIM; needs >=3 frequencies in the DATA" },
            { true, nc.trace_section_timers, "network.trace_section_timers",
              "the only per-epoch cost attribution; every perf claim is unreproducible without it" },
            { true, nc.trace_epoch_status, "network.trace_epoch_status",
              "cost is O(epochs), not O(stations); the primary run-forensics record" },
            { true, nc.write_run_manifest, "network.write_run_manifest",
              "provenance; no estimation effect and no product bytes" },
            { warm, nc.state_snapshot_require, "network.state_snapshot_require",
              "without it a fingerprint mismatch SILENTLY cold-starts — every same-snapshot A/B needs it" },
            // --- Regime-scoped ---------------------------------------------------------
            { lane, network_cold_clock_seed_, "network.cold_clock_seed",
              "lane/WL layers difference ~3e5-magnitude x_lin; measured 269 km at 305 sta cold" },
            { network_emit_pb_filter_, network_emit_pb_filter_gate_v2_,
              "network.emit_pb_filter_gate_v2",
              "supersedes the retired per-cadence jump_k/meas_sigma_m hand tuning" },
            { n_sta >= 50, iono_constraint_deweight_.adaptive,
              "network.iono_constraint_deweight_adaptive",
              "robust-z relative deweight of hostile minority iono rows (>=50 sta for a stable median)" },
        };

        struct Member { const char* knob; bool on; };
        struct Bundle { bool engaged; const char* name; const char* rule;
                        std::vector<Member> members; };
        const std::vector<Bundle> bundles = {
            { lane, "MW/WL lane stack",
              "the lane rows are built in the CORRECTION frame, so an unseeded cold receiver "
              "clock lands in the ambiguity seeds and lane_max_abs_xlin_m quarantines the "
              "cycles (x_lin 269 km measured at 305 sta); the lane also rides the Card-4 "
              "chord basis, and lane_carry additionally REQUIRES ewl_enable",
              { { "network.cold_clock_seed", network_cold_clock_seed_ },
                { "network.carry_invariant", network_carry_invariant_ },
                { "network.phase_datum_eq34", network_phase_datum_eq34_ },
                { "network.mw_wl.ewl_enable", nc.mw_wl.ewl_enable } } },
            { network_phase_leveling_ || network_carry_invariant_,
              "zero-q phase memory + slip response",
              "STANDING INVARIANT #1: any stiff time-linked quantity MUST have a wired slip "
              "response before an ON field run — a stale value carried past a jump warps the "
              "whole solution (iono/clock absorb the tension)",
              { { "network.cycle_slip.mw_detect", network_cycle_slip_mw_detect_ },
                { "network.cycle_slip.if_phase_td", network_cycle_slip_if_phase_td_ } } },
            { network_raim_, "armed QC set",
              "a 96 km epoch entering the network is never acceptable; withhold is the "
              "backstop, adapt-and-emit is the goal",
              { { "network.raim_attribution", network_raim_attribution_ },
                { "network.raim_within_epoch", network_raim_within_epoch_ },
                { "network.clock_runaway_check", network_clock_runaway_check_ } } },
            { estimate_network_iono_sh_, "SH-VTEC / W6 slice",
              "the SH field is fed a per-epoch CODE-derived slant iono, so low-elevation code "
              "contamination dominates a00 — the 25 deg iono cutoff is load-bearing and is a "
              "SEPARATE gate from processing.elevation_mask_deg",
              { { "estimation.ionosphere_weighting.enable", iono_weighting_.enabled },
                { "estimation.ionosphere_weighting.elevation_cutoff_deg >= 25",
                  iono_weighting_.elevation_cutoff_deg >= 24.9 },
                { "network.estimate_sat_code_bias", estimate_network_sat_code_bias_ } } },
        };

        log->info("=== Network POSTURE (regime: {} station(s), {} constellation(s), {} "
                  "frequency mode, {} start) — recommended-vs-armed; evidence in "
                  "docs/CONFIG_promotion_ledger.md ===",
                  n_sta, n_sys,
                  frequency_mode_ == FrequencyMode::All ? "all"
                      : frequency_mode_ == FrequencyMode::Triple ? "triple" : "dual",
                  warm ? "WARM-RESUME" : "cold");
        int n_on = 0, n_off = 0;
        for (const auto& it : items) {
            if (!it.applies) continue;
            if (it.on) { ++n_on; continue; }
            ++n_off;
            log->warn("  POSTURE OFF  {} — {}", it.knob, it.why);
        }
        for (const auto& b : bundles) {
            if (!b.engaged) continue;
            std::string missing;
            int armed = 0;
            for (const auto& m : b.members) {
                if (m.on) { ++armed; continue; }
                if (!missing.empty()) missing += ", ";
                missing += m.knob;
            }
            if (armed == static_cast<int>(b.members.size())) {
                log->info("  POSTURE BUNDLE '{}': {}/{} armed — complete.",
                          b.name, armed, b.members.size());
            } else if (armed == 0) {
                log->info("  POSTURE BUNDLE '{}': engaged but 0/{} companion(s) armed — {}.  "
                          "Missing: {}", b.name, b.members.size(), b.rule, missing);
            } else {
                log->warn("  POSTURE BUNDLE '{}': PARTIAL {}/{} — a strict subset of a coupled "
                          "set is its own failure mode.  {}.  MISSING: {}",
                          b.name, armed, b.members.size(), b.rule, missing);
            }
        }
        log->info("=== Network POSTURE: {} recommended knob(s) armed, {} NOT armed (see the "
                  "POSTURE OFF lines above; `python scripts/config_posture_check.py <config> "
                  "--tier all` lists the scoped tier too) ===", n_on, n_off);
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

} // namespace GnssProcessor
