/**
 * @file MwSlipDetectorEpoch.cpp
 * @brief The MW detector's per-epoch steps: detectLargeMwResets' passes.
 *
 * detectLargeMwResets (MwSlipDetector.cpp) runs these over one
 * MwSlipDetector::MwEpochWork (slip/MwDetectorWork.hpp, which lists them): the epoch's
 * resets, pass 1 per satellite (its path, its LLI policy, its lanes), pass 2 per SD
 * group (the pivot election, the re-reference, the pivot's ZD sample, the SD samples),
 * and the epoch's end.  emitLaneSlip_ forms every flagged lane's results.  Card UOM step
 * U5m-d cut the bodies out unchanged apart from the declared edits
 * (scripts/uom_u5m_verify.py proves them against f823a36).
 */

#include "slip/MwSlipDetector.hpp"
#include "slip/MwDetectorWork.hpp"
#include "math/TimeUtils.hpp"
#include <cmath>
#include <iterator>
#include <limits>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace GnssProcessor {

    using namespace mw;  // the constants table and the per-arc state (slip/MwArcState.hpp)

    // ===================================================================================
    // E0   beginEpoch_: the per-epoch resets
    // ===================================================================================

    void MwSlipDetector::beginEpoch_(MwEpochWork& w,
                                     const std::vector<PppObservation>& observations) {
        std::vector<MwSlipResult>& results = w.results;

        results.reserve(observations.size());
        // Drop any pre-epoch snapshots from the previous epoch.  Each
        // processMwSample call below will save a fresh snapshot for
        // its arc; this ensures the map only ever holds current-epoch
        // data for the resetSatellite / resetSatelliteSignal rollback
        // path (called by the KF post-fit residual handler, which
        // runs after this function returns).
        arc_snapshots_.clear();

        // Per-epoch affirmative-cleanliness set (getEpochCleanPairs):
        // rebuilt from scratch each pass so stale affirmations can never
        // leak into a later epoch's TurboEdit GF-only gating.
        epoch_clean_pairs_.clear();
        // The detection-grade per-epoch record (SPEC_event_harness §14): same
        // lifetime as the cleanliness set above, different question.
        epoch_pair_status_.clear();
        // LLI shadow (§14.11): rotate the one-epoch loss-of-lock memory before any
        // sample is processed, so every stamp this pass reads the previous epoch's
        // flags.  The LLI epoch itself never reaches processMwSample (the branch
        // below `continue`s after scheduling the resets), so its shadow falls on
        // the next epoch's first sample, the one compared against a mean that
        // never saw the epoch the receiver declared broken.
        prev_lli_bands_.swap(cur_lli_bands_);
        cur_lli_bands_.clear();
        // Capture epoch time from first valid observation for tracing
        for (const auto& obs : observations)
            if (!obs.satellite.empty()) { current_epoch_ = obs.time; break; }

        // Age out demoted-level stashes (across-gap WL settle evidence):
        // past kDemotedLevelMaxAgeS the consumer's own deferral cap has
        // long since expired and a stale level must not feed a future
        // unrelated demote's estimate.
        if (!demoted_levels_.empty()) {
            const double now_s = TimeUtils::continuousGpsSeconds(current_epoch_);
            for (auto it = demoted_levels_.begin(); it != demoted_levels_.end(); ) {
                if (now_s > 0.0 && it->second.epoch_s > 0.0 &&
                    now_s - it->second.epoch_s > kDemotedLevelMaxAgeS)
                    it = demoted_levels_.erase(it);
                else
                    ++it;
            }
        }
    }

    // ===================================================================================
    // E1   collectLaneSamples_: one satellite of pass 1
    // ===================================================================================

    void MwSlipDetector::collectLaneSamples_(MwEpochWork& w, const PppObservation& obs) {
        const bool has_lli = hasAnyPhaseLockLoss(obs);

        // Try combined (PPP) path first
        const auto if_pairs = collectIonoFreePairs(obs);
        const bool has_if = !if_pairs.empty();
        if (has_if) {
            // ---- Combined (PPP) path ----
            if (has_lli) {
                eraseArcsOnLli_(w, obs, if_pairs);       // the combined path's LLI policy
                return;
            }
            const std::set<std::string> if_band_pairs =
                collectCombinedLanes_(w, obs, if_pairs);
            collectSupplementaryLanes_(w, obs, if_band_pairs);
        } else {
            // ---- Uncombined (PPP-RTK) path ----
            const auto uc_pairs = collectUncombinedPairs(obs);
            if (uc_pairs.empty()) return;

            if (has_lli) {
                preserveArcsOnLli_(w, obs);              // the uncombined path's LLI policy
                return;
            }
            collectUncombinedLanes_(w, obs, uc_pairs);
        }
    }

    // ===================================================================================
    // E1   the combined path's LLI policy: erase the arcs
    // ===================================================================================

    void MwSlipDetector::eraseArcsOnLli_(MwEpochWork& w, const PppObservation& obs,
                                         const std::vector<IfPair>& if_pairs) {
        std::vector<MwSlipResult>& results = w.results;
        const auto& logger = w.logger;

        for (const auto& ifp : if_pairs) {
            const std::string amb_key = obs.satellite + "_" + ifp.phase_key;
            results.push_back(MwSlipResult{ amb_key });
            logger->warn("LLI lock loss detected: scheduling ambiguity reset for {}", amb_key);
            std::string s1, s2;
            if (mwwl_trace_.is_open() && splitIfLabel(ifp.phase_key, s1, s2)) {
                std::string c1, c2;
                splitIfLabel(ifp.code_key, c1, c2);
                constexpr double NaN = std::numeric_limits<double>::quiet_NaN();
                writeTraceRecord(obs, s1, s2, computeMwMeters(obs, s1, s2, c1, c2),
                                 NaN, NaN, NaN, 0,
                                 computeWideLaneWavelengthMeters(obs, s1, s2),
                                 NaN, /*cross_sat_floor_m=*/NaN, false, true);
            }
        }
        // Same LLI band memory (§14.11) on the combined path.  Here
        // the arcs are erased below, so the next sample starts a
        // fresh, immature arc that the maturity gate already refuses;
        // the stamp is recorded for symmetry and for the basis
        // string, not because this path needs it.
        {
            auto& bands = cur_lli_bands_[obs.satellite];
            for (const auto& [code, lli] : obs.lli_flags)
                if (!code.empty() && code[0] == 'L' && lli != 0)
                    bands.insert(code);
        }
        for (auto it = mw_stats_.begin(); it != mw_stats_.end(); )
            it = (it->first.rfind(obs.satellite, 0) == 0) ? mw_stats_.erase(it) : std::next(it);
    }

    // ===================================================================================
    // E1   the uncombined path's LLI policy: keep the arcs
    // ===================================================================================

    void MwSlipDetector::preserveArcsOnLli_(MwEpochWork& w, const PppObservation& obs) {
        std::vector<MwSlipResult>& results = w.results;
        const auto& logger = w.logger;

        for (const auto& [key, val] : obs.raw_observed)
            if (!key.empty() && key[0] == 'L' && key.find('_') == std::string::npos)
                results.push_back(MwSlipResult{ obs.satellite + "_" + key });
        logger->warn("LLI lock loss (uncombined): scheduling all phase ambiguity resets for {}", obs.satellite);
        // Remember which bands the receiver flagged (§14.11), so the
        // next epoch's stamp can mark the pairs that contain one of
        // them as LLI-shadowed.  Band-scoped on purpose: a lane made
        // only of unflagged bands is untouched by the receiver's
        // report and its silence still carries information, which
        // keeps the correct late narrowings available.
        {
            auto& bands = cur_lli_bands_[obs.satellite];
            for (const auto& [code, lli] : obs.lli_flags)
                if (!code.empty() && code[0] == 'L'
                    && code.find('_') == std::string::npos && lli != 0)
                    bands.insert(code);
            // Readout (§14.10): the LLI epoch has no MWDEC line of its
            // own, so log it explicitly; otherwise the gap reads as
            // missing data.
            std::string blist;
            for (const auto& b : bands) {
                if (!blist.empty()) blist += ";";
                blist += b;
            }
            logger->debug("MWSKIP sta={} {} t={:.1f} lli_bands=[{}] — EVERY pair's "
                          "sample WITHHELD this epoch (MW arc preserved, "
                          "count not reset); next epoch's first sample is "
                          "LLI-shadowed on every pair containing one of them.",
                          station_label_, obs.satellite, obs.time.tow, blist);
        }
        // MW stats are preserved across LLI on purpose.  The
        // running MW = (φ_1 − φ_2)/λ_WL − code-NL/λ_WL is a
        // function of raw observations only, independent of
        // the filter ambiguity state, so a short LLI gap does
        // not invalidate the running statistics.  When
        // observations resume:
        //  - a real cycle slip jumps MW by ΔN_1 − ΔN_2 cycles
        //    and trips the threshold check in processMwSample
        //    on the very next sample (which then resets its
        //    own per-arc stats with full slip-integer info);
        //    a band-common slip (ΔN_1 = ΔN_2) leaves MW
        //    unchanged, which is why the next sample is
        //    marked LLI-shadowed (above);
        //  - a spurious / no-slip LLI leaves MW unchanged, so
        //    the running mean keeps its converged precision.
        // Wiping the per-arc statistics here would discard that
        // precision in the no-slip case and be redundant in the
        // real-slip case.  The LLI ambiguity reset above remains
        // a conservative second safeguard on the *filter* side;
        // MW keeps its observation-domain history.
    }

    // ===================================================================================
    // E1   the combined path's IF lanes
    // ===================================================================================

    std::set<std::string> MwSlipDetector::collectCombinedLanes_(
        MwEpochWork& w, const PppObservation& obs, const std::vector<IfPair>& if_pairs) {
        std::vector<MwSlipResult>& results = w.results;
        const auto& logger = w.logger;
        auto& sd_groups = w.sd_groups;

        // Track which band-pairs already have IF labels (e.g. "1_2" from "L1W_L2W")
        // so that supplementary uncombined MW pairs can be added for bands not
        // covered by the combined signal_mode (e.g. L1-L5, L2-L5 when signal_mode=dual).
        std::set<std::string> if_band_pairs;

        for (const auto& ifp : if_pairs) {
            std::string s1, s2, c1, c2;
            if (!splitIfLabel(ifp.phase_key, s1, s2)) {
                logger->debug("[DIAG MW skip] sat={} if_phase={}: splitIfLabel(phase) failed",
                              obs.satellite, ifp.phase_key);
                continue;
            }
            if (!splitIfLabel(ifp.code_key, c1, c2)) {
                logger->debug("[DIAG MW skip] sat={} if_code={}: splitIfLabel(code) failed",
                              obs.satellite, ifp.code_key);
                continue;
            }

            const double lambda_wl = computeWideLaneWavelengthMeters(obs, s1, s2);
            if (!(std::isfinite(lambda_wl) && lambda_wl > 0.0)) {
                logger->debug("[DIAG MW skip] sat={} {}/{}: invalid lambda_wl={} (combined IF path)",
                              obs.satellite, s1, s2, lambda_wl);
                continue;
            }
            const double mw_raw = computeMwMeters(obs, s1, s2, c1, c2);
            if (!std::isfinite(mw_raw)) {
                logger->debug("[DIAG MW skip] sat={} {}/{}: NaN mw_raw (combined IF path) "
                              "— check raw_observed has all of L:{},{} C:{},{}",
                              obs.satellite, s1, s2, s1, s2, c1, c2);
                continue;
            }

            // Record the band pair as covered only after confirming
            // a valid MW could be formed.  Otherwise, a failed IF
            // pair would block the supplementary uncombined path
            // from producing the MW for that band combination.
            if (s1.size() >= 2 && s2.size() >= 2)
                if_band_pairs.insert(std::string(1, s1[1]) + "_" + std::string(1, s2[1]));

            // Compute satellite BIA MW bias
            double sat_bias = 0.0;
            const bool has_bia = computeSatMwBias(obs, s1, s2, c1, c2, sat_bias);
            const double mw_bia = has_bia ? (mw_raw - sat_bias) : mw_raw;

            if (has_bia) {
                // Collect for two-pass BSSD integer processing
                const char cons = obs.satellite.empty() ? '?' : obs.satellite[0];
                const std::string group_key = std::string(1, cons) + "_" + s1 + "_" + s2;
                sd_groups[group_key][obs.satellite] = {
                    &obs, { s1, s2, c1, c2 }, mw_raw, mw_bia,
                    sat_bias, has_bia, lambda_wl,
                    /*is_combined=*/true, ifp.phase_key
                };
            } else {
                // No BIA: undifferenced processing only, no integer locking
                const std::string stats_key = obs.satellite + "_" + ifp.phase_key;
                const MwLane lane{ obs.satellite, s1, s2, c1, c2, ifp.phase_key,
                                   stats_key };
                const MwSample sample{ mw_raw, lambda_wl, /*bia_corrected=*/false,
                                       /*sat_mw_bias=*/0.0 };
                if (const auto v = processMwSample(obs, lane, sample, MwSdFrame{})) {
                    emitLaneSlip_(results, lane, *v, lambda_wl,
                                  /*bia_corrected=*/false);
                    logger->warn("Scheduling reset for {} (wl_known={}, slip_wl={})",
                                 obs.satellite + "_" + ifp.phase_key, v->wl_known, v->slip_cycles);
                }
            }
        }
        return if_band_pairs;
    }

    // ===================================================================================
    // E1   the combined path's supplementary lanes
    // ===================================================================================

    void MwSlipDetector::collectSupplementaryLanes_(
        MwEpochWork& w, const PppObservation& obs,
        const std::set<std::string>& if_band_pairs) {
        std::vector<MwSlipResult>& results = w.results;
        const auto& logger = w.logger;
        auto& sd_groups = w.sd_groups;

        // Supplementary uncombined MW pairs from raw_observed for band
        // pairs not already covered by the combined IF labels.  This
        // provides extra multi-frequency constraints for cycle slip
        // detection (e.g. L1-L5, L2-L5) even when signal_mode limits
        // the KF to dual-frequency IF combinations.
        const auto uc_pairs = collectUncombinedPairs(obs);
        for (const auto& pair : uc_pairs) {
            // Skip pairs already handled via the combined path
            if (pair.phase1.size() >= 2 && pair.phase2.size() >= 2) {
                const std::string bkey = std::string(1, pair.phase1[1]) + "_"
                                       + std::string(1, pair.phase2[1]);
                if (if_band_pairs.count(bkey)) {
                    logger->debug("[DIAG MW skip] sat={} {}/{}: bands {} already covered by combined path",
                                  obs.satellite, pair.phase1, pair.phase2, bkey);
                    continue;
                }
            }

            const double lambda_wl = computeWideLaneWavelengthMeters(obs, pair.phase1, pair.phase2);
            if (!(std::isfinite(lambda_wl) && lambda_wl > 0.0)) {
                logger->debug("[DIAG MW skip] sat={} {}/{}: invalid lambda_wl={} (supplementary uc path)",
                              obs.satellite, pair.phase1, pair.phase2, lambda_wl);
                continue;
            }
            const double mw_raw = computeMwMeters(obs, pair.phase1, pair.phase2, pair.code1, pair.code2);
            if (!std::isfinite(mw_raw)) {
                logger->debug("[DIAG MW skip] sat={} {}/{}: NaN mw_raw (supplementary uc path) "
                              "— check raw_observed has all of L:{},{} C:{},{}",
                              obs.satellite, pair.phase1, pair.phase2,
                              pair.phase1, pair.phase2, pair.code1, pair.code2);
                continue;
            }

            double sat_bias = 0.0;
            const bool has_bia = computeSatMwBias(obs, pair.phase1, pair.phase2,
                                                  pair.code1, pair.code2, sat_bias);
            const double mw_bia = has_bia ? (mw_raw - sat_bias) : mw_raw;

            if (has_bia) {
                const char cons = obs.satellite.empty() ? '?' : obs.satellite[0];
                const std::string group_key = std::string(1, cons) + "_" + pair.phase1 + "_" + pair.phase2;
                sd_groups[group_key][obs.satellite] = {
                    &obs, pair, mw_raw, mw_bia,
                    sat_bias, has_bia, lambda_wl,
                    /*is_combined=*/false, {}
                };
            } else {
                const std::string stats_key = obs.satellite + "_" + pair.phase1 + "_" + pair.phase2;
                const MwLane lane{ obs.satellite, pair.phase1, pair.phase2,
                                   pair.code1, pair.code2, {}, stats_key };
                const MwSample sample{ mw_raw, lambda_wl, /*bia_corrected=*/false,
                                       /*sat_mw_bias=*/0.0 };
                if (const auto v = processMwSample(obs, lane, sample, MwSdFrame{})) {
                    emitLaneSlip_(results, lane, *v, lambda_wl,
                                  /*bia_corrected=*/false);
                    logger->warn("Scheduling supplementary MW reset for {}_{}, {}_{} (wl_known={}, slip_wl={})",
                                 obs.satellite, pair.phase1, obs.satellite, pair.phase2,
                                 v->wl_known, v->slip_cycles);
                }
            }
        }
    }

    // ===================================================================================
    // E1   the uncombined path's lanes
    // ===================================================================================

    void MwSlipDetector::collectUncombinedLanes_(
        MwEpochWork& w, const PppObservation& obs,
        const std::vector<UncombinedPair>& uc_pairs) {
        std::vector<MwSlipResult>& results = w.results;
        const auto& logger = w.logger;
        auto& sd_groups = w.sd_groups;

        for (const auto& pair : uc_pairs) {
            const double lambda_wl = computeWideLaneWavelengthMeters(obs, pair.phase1, pair.phase2);
            if (!(std::isfinite(lambda_wl) && lambda_wl > 0.0)) {
                logger->debug("[DIAG MW skip] sat={} {}/{}: invalid lambda_wl={} (uncombined path)",
                              obs.satellite, pair.phase1, pair.phase2, lambda_wl);
                continue;
            }
            const double mw_raw = computeMwMeters(obs, pair.phase1, pair.phase2, pair.code1, pair.code2);
            if (!std::isfinite(mw_raw)) {
                logger->debug("[DIAG MW skip] sat={} {}/{}: NaN mw_raw (uncombined path) "
                              "— check raw_observed has all of L:{},{} C:{},{}",
                              obs.satellite, pair.phase1, pair.phase2,
                              pair.phase1, pair.phase2, pair.code1, pair.code2);
                continue;
            }

            // Compute satellite BIA MW bias
            double sat_bias = 0.0;
            const bool has_bia = computeSatMwBias(obs, pair.phase1, pair.phase2,
                                                  pair.code1, pair.code2, sat_bias);
            const double mw_bia = has_bia ? (mw_raw - sat_bias) : mw_raw;

            logger->debug("[DIAG MW pair] sat={} {}/{}: lambda_wl={:.3f} m, mw_raw={:.3f} m, "
                          "has_bia={}, path={} -> sd_groups[{}]",
                          obs.satellite, pair.phase1, pair.phase2, lambda_wl, mw_raw,
                          has_bia ? 1 : 0,
                          has_bia ? "BSSD-pass2" : "direct-no-BIA",
                          has_bia ? (std::string{ obs.satellite[0] } + "_" + pair.phase1 + "_" + pair.phase2) : std::string{ "<none>" });

            if (has_bia) {
                // Collect for two-pass BSSD integer processing
                const char cons = obs.satellite.empty() ? '?' : obs.satellite[0];
                const std::string group_key = std::string(1, cons) + "_" + pair.phase1 + "_" + pair.phase2;
                sd_groups[group_key][obs.satellite] = {
                    &obs, pair, mw_raw, mw_bia,
                    sat_bias, has_bia, lambda_wl,
                    /*is_combined=*/false, {}
                };
            } else {
                // No BIA: undifferenced processing, no integer locking
                const std::string stats_key = obs.satellite + "_" + pair.phase1 + "_" + pair.phase2;
                const MwLane lane{ obs.satellite, pair.phase1, pair.phase2,
                                   pair.code1, pair.code2, {}, stats_key };
                const MwSample sample{ mw_raw, lambda_wl, /*bia_corrected=*/false,
                                       /*sat_mw_bias=*/0.0 };
                if (const auto v = processMwSample(obs, lane, sample, MwSdFrame{})) {
                    emitLaneSlip_(results, lane, *v, lambda_wl,
                                  /*bia_corrected=*/false);
                    logger->warn("Scheduling reset for {}_{}, {}_{} (wl_known={}, slip_wl={})",
                                 obs.satellite, pair.phase1, obs.satellite, pair.phase2,
                                 v->wl_known, v->slip_cycles);
                }
            }
        }
    }

    // ===================================================================================
    // E2a  electPivot_: the group's MW pivot
    // ===================================================================================

    std::string MwSlipDetector::electPivot_(
        const std::map<std::string, SatMwEntry>& sat_entries) const {
        // Select pivot: highest elevation satellite, but skip any sat
        // flagged via notifyPivotForcedHandover.  Falls back to the
        // overall highest-elevation candidate if every sat in this
        // group is deprioritized (single-sat groups, or a
        // constellation where every sat is flagged this epoch).
        std::string best_pivot;
        double      best_elev    = -999.0;
        std::string fallback_pivot;
        double      fallback_elev = -999.0;
        for (const auto& [sat, entry] : sat_entries) {
            if (entry.obs->elevation > fallback_elev) {
                fallback_elev = entry.obs->elevation;
                fallback_pivot = sat;
            }
            if (deprioritized_pivot_sats_.count(sat)) continue;
            if (entry.obs->elevation > best_elev) {
                best_elev = entry.obs->elevation;
                best_pivot = sat;
            }
        }
        if (best_pivot.empty()) {
            if (fallback_pivot.empty()) return {};
            best_pivot = fallback_pivot;
        }
        return best_pivot;
    }

    // ===================================================================================
    // E2b  reReferenceGroup_: a changed pivot (S-transform or erase)
    // ===================================================================================

    void MwSlipDetector::reReferenceGroup_(
        MwEpochWork& w, const std::string& group_key,
        const std::map<std::string, SatMwEntry>& sat_entries,
        const std::string& best_pivot) {
        const auto& logger = w.logger;

        // Detect pivot change.  Rather than erasing every locked arc
        // (which floats the WLs through every handover), S-transform
        // each non-pivot arc to be referenced against the new pivot:
        //
        //     SD_new(sat) = SD_old(sat) − SD_old(new_pivot)
        //     int_new(sat) = int_old(sat) − int_old(new_pivot)
        //
        // where SD_old(new_pivot) is the new pivot's running mean
        // under the prior referencing (it was a non-pivot then).
        // The transform is valid only when the new pivot was
        // BIA-corrected and integer-locked at the prior epoch, i.e.
        // a reliable integer SD reference exists.  When that
        // precondition fails, the bia_corrected non-pivot arcs are
        // erased and restart (the legacy behavior).
        //
        // Statistics on epoch-to-epoch differences (diff_window, M2,
        // last_dev, last_sigma) are unchanged by the SD-reference
        // change to first order, because the new and old pivot have
        // similar BIA-corrected MW noise levels.  The transform below
        // shifts the level quantities (mean, mean_lag, prev_mw,
        // the local ring, a demoted-level stash),
        // re-references prev_diff, shifts the locked integer
        // (rounded_wl) and drops a pending TD confirmation.
        auto prev_pivot_it = sd_pivot_sat_.find(group_key);
        if (prev_pivot_it == sd_pivot_sat_.end() || prev_pivot_it->second == best_pivot)
            return;
        const std::string old_pivot = prev_pivot_it->second;
        const std::string& new_pivot = best_pivot;

        const auto& new_piv_entry = sat_entries.at(new_pivot);
        const std::string new_piv_old_key =
            new_pivot + "_" + new_piv_entry.pair.phase1 + "_"
            + new_piv_entry.pair.phase2;
        auto new_piv_arc_it = mw_stats_.find(new_piv_old_key);

        // Capture reference values before any erase below.
        bool   can_s_transform = false;
        double piv_old_mean    = 0.0;
        double piv_old_prev_mw = 0.0;
        double piv_old_prev_diff =
            std::numeric_limits<double>::quiet_NaN();
        int    piv_old_int     = 0;
        if (new_piv_arc_it != mw_stats_.end()
            && new_piv_arc_it->second.bia_corrected
            && new_piv_arc_it->second.wl_locked) {
            can_s_transform = true;
            piv_old_mean    = new_piv_arc_it->second.mean;
            piv_old_prev_mw = new_piv_arc_it->second.prev_mw;
            piv_old_prev_diff = new_piv_arc_it->second.prev_diff;
            piv_old_int     = new_piv_arc_it->second.rounded_wl;
        }

        // In-house bias S-transform: when the new pivot had a stored
        // bias under the old frame, every other sat's stored bias
        // re-references by subtracting it.  Without an anchor the
        // affected entries are erased; re-bootstrapping is safer
        // than applying a stale-frame bias.
        auto piv_bias_it = sat_pair_bias_.find(new_piv_old_key);
        bool   can_s_transform_bias = false;
        double piv_old_bias = 0.0;
        if (piv_bias_it != sat_pair_bias_.end()
            && piv_bias_it->second.commit_count > 0) {
            can_s_transform_bias = true;
            piv_old_bias = piv_bias_it->second.bias_m;
        }

        int n_xform  = 0;
        int n_erased = 0;
        int n_bias_xform  = 0;
        int n_bias_erased = 0;
        int n_old_pivot_dropped = 0;
        for (const auto& [sat, entry] : sat_entries) {
            if (sat == new_pivot) continue;
            const std::string stats_key =
                sat + "_" + entry.pair.phase1 + "_" + entry.pair.phase2;

            // The old pivot, now becoming a non-pivot, holds a
            // ZD-frame arc (bia_corrected=false) whose Hatch mean
            // still contains the receiver WL bias and is not
            // comparable to the SD-frame samples that start
            // arriving this epoch.  Without an estimate of the
            // receiver bias it cannot be S-transformed into the new
            // SD frame; left alone, the next sample would appear as
            // a multi-cycle pseudo-slip against the stale mean and
            // trigger a spurious targeted reset/repair.  Drop the
            // arc and any stale bias entry; a clean SD arc restarts
            // against the new pivot at the next sample.
            if (sat == old_pivot) {
                auto arc_it_op = mw_stats_.find(stats_key);
                if (arc_it_op != mw_stats_.end()) {
                    mw_stats_.erase(arc_it_op);
                    ++n_old_pivot_dropped;
                }
                auto bias_it_op = sat_pair_bias_.find(stats_key);
                if (bias_it_op != sat_pair_bias_.end()) {
                    sat_pair_bias_.erase(bias_it_op);
                }
                continue;
            }

            auto arc_it = mw_stats_.find(stats_key);
            if (arc_it != mw_stats_.end()) {
                auto& arc = arc_it->second;
                if (!arc.bia_corrected) {
                    // building; SD ref doesn't matter yet
                } else if (can_s_transform) {
                    arc.mean     -= piv_old_mean;
                    arc.mean_lag -= piv_old_mean;   // lagged snapshot rides the same frame (MwArcStats doc)
                    arc.prev_mw  -= piv_old_prev_mw;
                    // A demoted-level stash for this arc lives in
                    // the same SD frame; shift it by the same
                    // constant so the across-gap difference
                    // (new mean − stash) stays frame-invariant.
                    if (auto dl = demoted_levels_.find(stats_key);
                        dl != demoted_levels_.end())
                        dl->second.pre_level_m -= piv_old_mean;
                    // The local raw-MW ring holds old-SD-frame
                    // samples; shift by the same constant as the
                    // mean, or the epoch after every handover each
                    // surviving arc would show a local deviation of
                    // piv_old_mean (typically multiple WL cycles),
                    // a false slip across the whole constellation.
                    shiftLocalWindow(arc, -piv_old_mean);
                    // prev_diff (TD1[k−1]) must be re-referenced to
                    // the new frame too: TD1_new = TD1_old −
                    // TD1_old(new pivot), exactly, because the
                    // contribution of the shared old-pivot samples
                    // (slip-sized after a slip-forced handover, not
                    // just epoch noise) cancels identically.  Left
                    // untouched, the next epoch's TD2 on every
                    // surviving arc would read +dN_oldpivot·λ, the
                    // constellation-wide false slip this transform
                    // prevents, recreated in the TD2 statistic.  NaN
                    // (either side) degrades to the documented
                    // fresh-arc TD1 fallback for one epoch.
                    arc.prev_diff =
                        (std::isfinite(arc.prev_diff)
                         && std::isfinite(piv_old_prev_diff))
                            ? arc.prev_diff - piv_old_prev_diff
                            : std::numeric_limits<double>::quiet_NaN();
                    // A pending TD confirmation stores old-frame
                    // level quantities (jump / pre-level); after
                    // the frame change its hypotheses are
                    // meaningless, so drop it (the lock survives;
                    // if the flag was a real step, drift demotion
                    // remains the backstop for this rare case).
                    arc.td_confirm_pending = false;
                    if (arc.wl_locked) {
                        arc.rounded_wl -= piv_old_int;
                    }
                    ++n_xform;
                } else {
                    mw_stats_.erase(arc_it);
                    ++n_erased;
                }
            }

            auto bias_it = sat_pair_bias_.find(stats_key);
            if (bias_it != sat_pair_bias_.end()
                && bias_it->second.commit_count > 0) {
                if (can_s_transform_bias) {
                    bias_it->second.bias_m -= piv_old_bias;
                    ++n_bias_xform;
                } else {
                    sat_pair_bias_.erase(bias_it);
                    ++n_bias_erased;
                }
            }
        }

        // Drop the new pivot's own old (non-pivot) arc.  It'll be
        // re-initialized this epoch as the pivot (BIA-corrected,
        // un-differenced, no integer lock).
        if (new_piv_arc_it != mw_stats_.end()) mw_stats_.erase(new_piv_arc_it);

        // Drop the new pivot's own stored bias entry: it is the
        // SD anchor now (zero by definition), with no bias to track.
        if (piv_bias_it != sat_pair_bias_.end()) {
            sat_pair_bias_.erase(piv_bias_it);
        }

        if (can_s_transform) {
            logger->info("BSSD MW pivot change for {}: {} -> {}: "
                         "S-transformed {} non-pivot arcs, dropped {} stale old-pivot arc "
                         "(int shift {} cyc, mean shift {:.4f} m); locks preserved",
                         group_key, old_pivot, new_pivot,
                         n_xform, n_old_pivot_dropped,
                         piv_old_int, piv_old_mean);
        } else {
            logger->info("BSSD MW pivot change for {}: {} -> {}: "
                         "new pivot not integer-locked; erased {} non-pivot arcs, "
                         "dropped {} stale old-pivot arc (no S-transform possible)",
                         group_key, old_pivot, new_pivot,
                         n_erased, n_old_pivot_dropped);
        }
        if (n_bias_xform > 0 || n_bias_erased > 0) {
            logger->info("In-house bias re-reference for {}: "
                         "xform={}, erased={} (anchor={}, shift={:.4f} m)",
                         group_key, n_bias_xform, n_bias_erased,
                         can_s_transform_bias ? "available" : "missing",
                         piv_old_bias);
        }
    }

    // ===================================================================================
    // E2c  processPivot_: the pivot's own ZD sample
    // ===================================================================================

    MwSlipDetector::MwSdFrame MwSlipDetector::processPivot_(
        MwEpochWork& w, const std::map<std::string, SatMwEntry>& sat_entries,
        const std::string& best_pivot) {
        std::vector<MwSlipResult>& results = w.results;
        const auto& logger = w.logger;

        const auto& pivot_entry = sat_entries.at(best_pivot);

        // --- Pivot first: its undifferenced (ZD) slip anchors the frame ---
        // The non-pivot arcs below are single-differenced against this
        // pivot (sd_mw = MW_sat - MW_pivot), so processMwSample sees the SD
        // jump dN_sat - dN_pivot.  Downstream consumers expect each result's
        // slip_wl_cycles to be the satellite's own undifferenced (ZD) slip,
        // so the pivot's ZD slip is captured here and added back below:
        // dN_sat = (dN_sat - dN_pivot) + dN_pivot.  When the pivot's own
        // integer cannot be resolved the conversion is impossible, so the
        // non-pivot slips are flagged wl_known=false (-> reset) rather than
        // emitting an SD value under a ZD contract.
        int    pivot_zd_slip  = 0;     // dN_pivot, WL cycles (0 if pivot clean)
        bool   pivot_zd_known = true;  // pivot clean => dN_pivot = 0, known
        const std::string piv_key = best_pivot + "_"
            + pivot_entry.pair.phase1 + "_" + pivot_entry.pair.phase2;
        // Pivot uses undifferenced BIA-corrected MW: it cannot integer-
        // lock (receiver bias still present), but the jump-based estimate
        // still resolves the slip and that estimate is already ZD.
        const MwLane lane{ best_pivot, pivot_entry.pair.phase1, pivot_entry.pair.phase2,
                           pivot_entry.pair.code1, pivot_entry.pair.code2,
                           pivot_entry.if_phase_key, piv_key };
        const MwSample sample{ pivot_entry.mw_bia, pivot_entry.lambda_wl,
                               /*bia_corrected=*/false, pivot_entry.sat_mw_bias };
        const MwSdFrame pivot_frame{ best_pivot, /*pivot_zd_slip=*/0,
                                     /*pivot_zd_known=*/true };
        if (const auto v = processMwSample(*pivot_entry.obs, lane, sample, pivot_frame)) {
            pivot_zd_slip  = v->slip_cycles;
            pivot_zd_known = v->wl_known;
            // Lane class: the sample carried bia_corrected=false into
            // processMwSample (the pivot cannot integer-lock, because
            // the receiver bias is still in it), but the value is the
            // BIA-corrected mw_bia and this arc is the S-transform
            // reference of its group.  A pivot re-reference belongs to
            // the false-alarm population the evidence rule must refuse,
            // so the reported class is the lane's, not the call's: true.
            emitLaneSlip_(results, lane, *v, pivot_entry.lambda_wl, /*bia_corrected=*/true);
            logger->warn("Scheduling reset for {} (pivot, wl_known={}, slip_wl={} ZD)",
                         pivot_entry.is_combined
                             ? (best_pivot + "_" + pivot_entry.if_phase_key)
                             : (best_pivot + "_" + pivot_entry.pair.phase1 + "+"
                                + pivot_entry.pair.phase2),
                         v->wl_known, v->slip_cycles);
        }
        return MwSdFrame{ best_pivot, pivot_zd_slip, pivot_zd_known };
    }

    // ===================================================================================
    // E2d  processNonPivots_: the SD samples, re-referenced to ZD
    // ===================================================================================

    void MwSlipDetector::processNonPivots_(
        MwEpochWork& w, const std::map<std::string, SatMwEntry>& sat_entries,
        const std::string& best_pivot, const MwSdFrame& frame) {
        std::vector<MwSlipResult>& results = w.results;
        const auto& logger = w.logger;

        const auto& pivot_entry   = sat_entries.at(best_pivot);
        const int  pivot_zd_slip  = frame.pivot_zd_slip;
        const bool pivot_zd_known = frame.pivot_zd_known;
        for (const auto& [sat, entry] : sat_entries) {
            if (sat == best_pivot) continue;  // ZD slip handled before the loop

            const std::string stats_key =
                sat + "_" + entry.pair.phase1 + "_" + entry.pair.phase2;

            // Non-pivot: single-difference MW to cancel the receiver WL bias.
            //   SD_MW = (N_WL_sat - N_WL_pivot) * lambda_wl + noise
            const double sd_mw = entry.mw_bia - pivot_entry.mw_bia;

            const MwLane lane{ sat, entry.pair.phase1, entry.pair.phase2,
                               entry.pair.code1, entry.pair.code2,
                               entry.if_phase_key, stats_key };
            const MwSample sample{ sd_mw, entry.lambda_wl, /*bia_corrected=*/true,
                                   entry.sat_mw_bias };
            if (auto v = processMwSample(*entry.obs, lane, sample, frame)) {
                // Re-reference the SD jump to this satellite's own ZD slip:
                // dN_sat = SD + dN_pivot.  Adding an integer leaves the
                // rounding fraction (slip_frac) unchanged.  Unusable if the
                // pivot integer was unknown -> consumer resets.
                v->slip_cycles = v->slip_cycles + pivot_zd_slip;
                v->wl_known = v->wl_known && pivot_zd_known;
                // The float re-references identically, and becomes
                // unanchored (NaN) when the pivot's own ZD slip is
                // unresolved, like wl_known.
                v->slip_wl_float = (pivot_zd_known && std::isfinite(v->slip_wl_float))
                    ? v->slip_wl_float + static_cast<double>(pivot_zd_slip)
                    : std::numeric_limits<double>::quiet_NaN();
                // Lane class true: this arc is single-differenced against the
                // detector's MW pivot, so its "slip" population contains the
                // pivot's own S-transform re-references (observed as many
                // satellites at one station stepping in the same epoch).  The
                // evidence rule refuses this class outright.
                emitLaneSlip_(results, lane, *v, entry.lambda_wl, /*bia_corrected=*/true);
                logger->warn("Scheduling reset for {} (wl_known={}, slip_wl={} ZD)",
                             entry.is_combined ? (sat + "_" + entry.if_phase_key)
                                               : (sat + "_" + entry.pair.phase1 + "+" + entry.pair.phase2),
                             v->wl_known, v->slip_cycles);
            }
        }
    }

    // ===================================================================================
    // E3   endEpoch_: hints, cross-satellite tiers, trace flush
    // ===================================================================================

    std::vector<MwSlipResult> MwSlipDetector::endEpoch_(MwEpochWork& w) {
        std::vector<MwSlipResult>& results = w.results;

        // Consume the one-shot deprioritization hints.  Each call to
        // notifyPivotForcedHandover applies to exactly one downstream
        // detectLargeMwResets pass; the upstream caller must re-issue
        // the hint each epoch the deprioritization should hold.
        deprioritized_pivot_sats_.clear();

        // Finalize the cross-sat σ tiers (per constellation × band-pair,
        // per constellation, global) from this epoch's pool of per-sat
        // deviations.  Next epoch's computeAdaptiveThreshold picks up the
        // result via cross_sat_sigma_m_ and the coarser tiers.  (The
        // one-epoch lag is intentional: the current epoch's pool is not
        // fully assembled until every sat has been processed.)
        finalizeCrossSatSigmas();

        // Per-epoch flush so the trace survives non-graceful termination
        // (console close, second Ctrl-C, crash).  Nothing else flushes this
        // stream (RealTimeEngine::flush() only drains pending observations),
        // so without this the tail of the run stays in the stream buffer and
        // is lost with the process, truncating the file at a buffer boundary.
        // At ≤10 Hz the flush cost is negligible.
        if (mwwl_trace_.is_open()) mwwl_trace_.flush();

        return std::move(results);
    }

    // ===================================================================================
    // emitLaneSlip_: the results of one flagged lane
    // ===================================================================================

    void MwSlipDetector::emitLaneSlip_(std::vector<MwSlipResult>& results,
                                       const MwLane& lane, const MwVerdict& verdict,
                                       double lambda_wl, bool bia_corrected) {
        auto emit = [&](const std::string& amb_key) {
            results.push_back({ amb_key, verdict.slip_cycles, verdict.wl_known, lambda_wl,
                                lane.sig1, lane.sig2, lane.satellite, verdict.slip_frac,
                                verdict.slip_wl_float, verdict.evidence_ratio,
                                verdict.arc_epochs, bia_corrected });
        };
        if (!lane.if_label.empty()) {             // a combined lane: its IF ambiguity
            emit(lane.satellite + "_" + lane.if_label);
            return;
        }
        emit(lane.satellite + "_" + lane.sig1);   // an uncombined lane: both signals
        emit(lane.satellite + "_" + lane.sig2);
    }

} // namespace GnssProcessor
