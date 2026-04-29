/**
/**
* @file CycleSlipManager.cpp
* @brief Unified MW-only cycle slip detection and reset coordinator.
*
* Coordinates MW slip detection with multi-frequency cross-consistency
* analysis and targeted ambiguity resets.
*/

#include "slip/CycleSlipManager.hpp"
#include <cmath>
#include <limits>

namespace GnssProcessor {

// Helper: extract satellite ID from an ambiguity key "G01_L1C" -> "G01"
static std::string satIdFromAmbiguityKey(const std::string& key) {
    auto pos = key.find('_');
    return (pos != std::string::npos) ? key.substr(0, pos) : key;
}

// Helper: extract signal from an ambiguity key "G01_L1C" -> "L1C"
static std::string signalFromAmbiguityKey(const std::string& key) {
    auto pos = key.find('_');
    return (pos != std::string::npos) ? key.substr(pos + 1) : "";
}

// ===========================================================================
// Construction
// ===========================================================================

CycleSlipManager::CycleSlipManager(const GnssConfig& config, LogManager& log_manager)
    : config_(config),
      log_manager_(log_manager),
      ppp_utils_(log_manager),
      mw_detector_(config, log_manager) {
}

// ===========================================================================
// Public API
// ===========================================================================

std::map<std::string, int> CycleSlipManager::getResolvedWlAmbiguities() const {
    return mw_detector_.getResolvedWlAmbiguities();
}

void CycleSlipManager::resetSatellite(const std::string& satellite) {
    mw_detector_.resetSatellite(satellite);
}

void CycleSlipManager::setCorrections(const GnssCorrections* corrections) {
    mw_detector_.setCorrections(corrections);
}

bool CycleSlipManager::enableMwTracing(const std::filesystem::path& log_dir) {
    return mw_detector_.enableTracing(log_dir);
}

// ===========================================================================
// BSSD-aware ambiguity reset
// ===========================================================================

void CycleSlipManager::resetAmbiguityBssdAware(
    const std::string& amb_key,
    const std::string& sat_id,
    const std::string& signal,
    const std::vector<PppObservation>& modeled_obs,
    StateManager& state_manager,
    const MeasurementModel& meas_model,
    const std::string& epoch_prefix) {

    const double reset_std_m = config_.getAmbiguityStd();

    if (!config_.enableBetweenSatelliteSd()) {
        state_manager.resetAmbiguity(amb_key, reset_std_m);
        return;
    }

    const auto& pivots = meas_model.getBssdPivotSatellites();
    const std::string cons = PppUtils::getSystemString(sat_id);
    auto piv_it = pivots.find(cons);

    if (piv_it == pivots.end()) {
        state_manager.resetAmbiguity(amb_key, reset_std_m);
        return;
    }

    const std::string& pivot_sat = piv_it->second;
    auto log = log_manager_.getLogger("cycle_slip_manager");

    // Pivot satellite: inflate the frozen variance so that the downstream
    // BssdPivotManager::handlePivotHandover() detects the disturbance
    // (via P > 1e-12) and performs a re-referencing to a clean satellite.
    // The state value is left at 0.0  setting it to a non-zero poison
    // value would propagate through the T·P·T' re-referencing and
    // corrupt all SD ambiguities in the constellation, causing a cascade
    // of large innovations and dozens of outlier-rebuild passes.
    if (sat_id == pivot_sat) {
        const int amb_idx = state_manager.getAmbiguityIndex(amb_key);
        if (amb_idx >= 0) {
            Eigen::MatrixXd P = state_manager.getCovariance();
            const auto aidx = static_cast<Eigen::Index>(amb_idx);
            P(aidx, aidx) = reset_std_m * reset_std_m;
            state_manager.setCovariance(P);
        }
        log->info("{}BSSD: pivot {} slipped on {} -- variance inflated to trigger handover",
                  epoch_prefix, sat_id, signal);
        return;
    }

    // Non-pivot: compute SD seed.
    const int amb_idx = state_manager.getAmbiguityIndex(amb_key);
    if (amb_idx < 0) {
        state_manager.resetAmbiguity(amb_key, reset_std_m);
        return;
    }

    const Eigen::VectorXd x = state_manager.getStateVector();

    const PppObservation* obs_sat = nullptr;
    const PppObservation* obs_piv = nullptr;
    for (const auto& obs : modeled_obs) {
        if (obs.satellite == sat_id)    obs_sat = &obs;
        if (obs.satellite == pivot_sat) obs_piv = &obs;
        if (obs_sat && obs_piv) break;
    }

    double sd_seed = 0.0;
    if (obs_sat && obs_piv) {
        // The pivot may use a different tracking attribute on the same
        // frequency band (e.g. L1W vs L1C).  Find the pivot's actual
        // signal on the same band for a correct absolute seed.
        std::string piv_signal = signal;  // default: same signal
        if (!obs_piv->observed.count(signal) && signal.size() >= 2) {
            const std::string band = signal.substr(0, 2);
            for (const auto& [sig, val] : obs_piv->observed) {
                if (sig.size() >= 2 && sig.substr(0, 2) == band) {
                    piv_signal = sig;
                    break;
                }
            }
        }
        sd_seed = state_manager.computeAmbiguitySeed(*obs_sat, signal, x)
                - state_manager.computeAmbiguitySeed(*obs_piv, piv_signal, x);
    } else if (obs_sat) {
        sd_seed = state_manager.computeAmbiguitySeed(*obs_sat, signal, x);
    }

    Eigen::VectorXd x_mod = x;
    Eigen::MatrixXd P = state_manager.getCovariance();
    const auto aidx = static_cast<Eigen::Index>(amb_idx);

    const double old_val = x_mod(aidx);
    x_mod(aidx) = sd_seed;
    P.row(aidx).setZero();
    P.col(aidx).setZero();
    P(aidx, aidx) = reset_std_m * reset_std_m;

    state_manager.setStateVector(x_mod);
    state_manager.setCovariance(P);

    log->info("{}BSSD SD-aware reset {}: {:.4f} -> {:.4f} (pivot={})",
              epoch_prefix, amb_key, old_val, sd_seed, pivot_sat);
}

// ===========================================================================
// Multi-frequency MW cross-consistency analysis
// ===========================================================================

std::set<std::string> CycleSlipManager::identifySlippedSignals(
    const std::vector<MwSlipResult>& sat_slips,
    const std::vector<MwSlipResult>& all_results) const {

    // Collect signals that appear in slipped pairs and in clean pairs.
    std::set<std::string> slipped_pair_signals;
    std::set<std::string> clean_pair_signals;

    // Build set of slipped pair keys for matching at the pair level
    // (not amb_key level, since uncombined pairs emit two amb_keys per pair).
    std::set<std::string> slipped_pair_keys;
    for (const auto& sr : sat_slips) {
        if (!sr.sig1.empty() && !sr.sig2.empty())
            slipped_pair_keys.insert(sr.sig1 + "_" + sr.sig2);
    }

    for (const auto& sr : all_results) {
        if (sr.sig1.empty() || sr.sig2.empty()) continue;
        const std::string pk = sr.sig1 + "_" + sr.sig2;
        if (slipped_pair_keys.count(pk) > 0) {
            // This pair slipped
            slipped_pair_signals.insert(sr.sig1);
            slipped_pair_signals.insert(sr.sig2);
        } else {
            // This pair is clean
            clean_pair_signals.insert(sr.sig1);
            clean_pair_signals.insert(sr.sig2);
        }
    }

    if (clean_pair_signals.empty()) {
        // All pairs slipped or we have no clean reference  inconclusive
        return {};
    }

    // Signals in slipped pairs but NOT in any clean pair are the culprits.
    // Example: L1W_L2W slipped, L1W_L5Q clean.
    //   slipped_pair_signals = {L1W, L2W}
    //   clean_pair_signals = {L1W, L5Q}
    //   result = {L2W}  (L1W appears in a clean pair, so it's OK)
    std::set<std::string> culprits;
    for (const auto& sig : slipped_pair_signals) {
        if (clean_pair_signals.count(sig) == 0)
            culprits.insert(sig);
    }

    return culprits;
}

// ===========================================================================
// Pre-estimation: MW detection and reset
// ===========================================================================

CycleSlipEpochResult CycleSlipManager::detectAndRepair(
    const std::string& epoch_prefix,
    const std::vector<PppObservation>& modeled_obs,
    StateManager& state_manager,
    const MeasurementModel& meas_model) {

    auto log = log_manager_.getLogger("cycle_slip_manager");
    CycleSlipEpochResult result;
    last_mw_slips_.clear();

    // --- MW / LLI detection (all dual-frequency pairs including EWL) ---
    std::set<std::string> reset_sats;
    {
        const auto slip_results = mw_detector_.detectLargeMwResets(modeled_obs);

        // Group ALL MW results (slipped and non-slipped) by satellite for
        // cross-consistency analysis.
        // The MW detector returns results only for slipped pairs, so we also
        // need to know which pairs did NOT slip.

        // Helper: unique pair key from sig1 + sig2 for deduplication.
        // The MW detector emits two result entries per uncombined pair (one
        // per signal's amb_key), but they represent the same MW constraint.
        auto pairKey = [](const MwSlipResult& sr) -> std::string {
            return sr.sig1 + "_" + sr.sig2;
        };

        // Build per-satellite groups of slipped results (deduplicated by pair)
        std::map<std::string, std::vector<MwSlipResult>> slips_by_sat;
        {
            std::map<std::string, std::set<std::string>> seen_slip_pairs;
            for (const auto& sr : slip_results) {
                if (sr.sig1.empty() || sr.sig2.empty()) continue;
                const std::string sat_id = sr.satellite.empty()
                    ? satIdFromAmbiguityKey(sr.amb_key) : sr.satellite;
                const std::string pk = pairKey(sr);
                if (seen_slip_pairs[sat_id].insert(pk).second)
                    slips_by_sat[sat_id].push_back(sr);
            }
        }

        // Build per-satellite list of all MW signal pairs (slipped + clean),
        // deduplicated by (sig1, sig2).
        // Use getActiveArcKeys() which returns all mature arcs (those with
        // enough history to have detected a slip if one occurred), providing
        // clean-pair references for cross-consistency analysis.
        std::map<std::string, std::vector<MwSlipResult>> all_pairs_by_sat;
        {
            std::map<std::string, std::set<std::string>> seen_pairs;
            // First, add deduplicated slipped pairs
            for (const auto& [sat_id, slips] : slips_by_sat) {
                for (const auto& sr : slips) {
                    seen_pairs[sat_id].insert(pairKey(sr));
                    all_pairs_by_sat[sat_id].push_back(sr);
                }
            }
            // Then identify clean (non-slipped) signal pairs from all active
            // MW arcs.  An active arc that didn't produce a slip result means
            // dN_i - dN_j = 0 for that pair  a crucial reference for
            // cross-consistency analysis.
            for (const auto& stats_key : mw_detector_.getActiveArcKeys()) {
                // stats_key = "G01_L1C_L2W" -> sat="G01", sig1="L1C", sig2="L2W"
                const auto first_us = stats_key.find('_');
                if (first_us == std::string::npos) continue;
                const std::string sat_id = stats_key.substr(0, first_us);
                const std::string rest = stats_key.substr(first_us + 1);
                const auto second_us = rest.find('_');
                if (second_us == std::string::npos) continue;
                const std::string s1 = rest.substr(0, second_us);
                const std::string s2 = rest.substr(second_us + 1);

                const std::string pk = s1 + "_" + s2;
                if (!seen_pairs[sat_id].insert(pk).second)
                    continue; // already present (slipped or duplicate)

                MwSlipResult clean_entry;
                clean_entry.amb_key = sat_id + "_" + s1;
                clean_entry.sig1 = s1;
                clean_entry.sig2 = s2;
                clean_entry.satellite = sat_id;
                clean_entry.slip_wl_known = true;
                clean_entry.slip_wl_cycles = 0;
                all_pairs_by_sat[sat_id].push_back(clean_entry);
            }
        }

        if (!slip_results.empty()) {
            // Collect all distinct amb_keys that need reset, grouped by satellite.
            // This includes LLI resets (empty sig1/sig2) and MW pair resets.
            std::map<std::string, std::set<std::string>> reset_keys_by_sat;
            for (const auto& sr : slip_results) {
                const std::string sat_id = sr.satellite.empty()
                    ? satIdFromAmbiguityKey(sr.amb_key) : sr.satellite;
                reset_keys_by_sat[sat_id].insert(sr.amb_key);
                // Cache one representative slip for post-estimation repair
                last_mw_slips_[sat_id] = sr;
            }

            // Process each satellite once.
            for (auto& [sat_id, amb_keys] : reset_keys_by_sat) {
                const auto& sat_all_pairs = all_pairs_by_sat[sat_id];
                const auto slips_it = slips_by_sat.find(sat_id);
                const bool has_pair_slips = slips_it != slips_by_sat.end()
                                         && !slips_it->second.empty();

                // --- Multi-frequency cross-consistency analysis ---
                // If this satellite has 3+ MW pairs (slipped + clean), try to
                // identify which individual signal slipped rather than resetting all.
                if (has_pair_slips && sat_all_pairs.size() >= 2
                    && slips_it->second.size() < sat_all_pairs.size()) {
                    const auto culprit_signals = identifySlippedSignals(
                        slips_it->second, sat_all_pairs);
                    if (!culprit_signals.empty()) {
                        // Targeted reset: only reset the identified signal(s)
                        for (const auto& culprit : culprit_signals) {
                            const std::string targeted_key = sat_id + "_" + culprit;
                            resetAmbiguityBssdAware(targeted_key, sat_id, culprit,
                                                    modeled_obs, state_manager, meas_model,
                                                    epoch_prefix);
                            ++result.n_repaired;
                            result.state_modified = true;
                            log->info("{}MW cross-consistency: targeted reset {} (culprit signal {})",
                                      epoch_prefix, targeted_key, culprit);
                        }
                        reset_sats.insert(sat_id);
                        continue;
                    }
                }

                // --- Fall through to full reset ---
                log->debug("{}MW full reset for {} ({} slipped pairs, {} total mature pairs, {} amb_keys)",
                           epoch_prefix, sat_id,
                           has_pair_slips ? slips_it->second.size() : 0,
                           sat_all_pairs.size(), amb_keys.size());
                for (const auto& key : amb_keys) {
                    const std::string signal = signalFromAmbiguityKey(key);
                    resetAmbiguityBssdAware(key, sat_id, signal, modeled_obs,
                                            state_manager, meas_model, epoch_prefix);
                    ++result.n_reset;
                    result.state_modified = true;
                }
                reset_sats.insert(sat_id);
            }
            if (result.n_repaired > 0 || result.n_reset > 0)
                log->warn("{}MW: {} targeted, {} full reset",
                          epoch_prefix, result.n_repaired,
                          result.n_reset);
        }
    }

    return result;
}

} // namespace GnssProcessor
