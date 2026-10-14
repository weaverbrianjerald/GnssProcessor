/**
 * @file CycleSlipDetection.cpp
 * @brief Sections S1 to S8 of @c CycleSlipManager::detectAndRepair: the
 *        detectors run, TurboEdit and the soft-LLI bridge resolve, and the MW
 *        results are grouped per satellite.
 *
 * Each section reads and writes the epoch's @c SlipEpochWork through the alias
 * preamble at its top.  Bodies moved verbatim from CycleSlipManager.cpp (card
 * UOM step U3; scripts/uom_u3_verify.py).
 */

#include "slip/CycleSlipManager.hpp"
#include "slip/CycleSlipEpochWork.hpp"
#include <iterator>

namespace GnssProcessor {

// ===========================================================================
// S1: soft-LLI gap-hold registration
// ===========================================================================

void CycleSlipManager::registerSoftLliGapHolds(SlipEpochWork& w) {
    const auto& epoch_prefix  = w.epoch_prefix;
    const auto& modeled_obs   = w.modeled_obs;
    const auto& state_manager = w.state_manager;
    const auto& epoch_now_s   = w.epoch_now_s;
    const auto& is_bssd_pivot = w.is_bssd_pivot;

    // --- Gap-hold registration (pre-pass) ----------------------------------
    // A phase signal that dropped out this epoch (no observation, no LLI) and
    // has a recent converged snapshot is held instead of being left to the
    // measurement model's prune-on-first-miss.  The held key is
    // prune-protected and KF-excluded (via getHeldPendingBridgeKeys) until the
    // TurboEdit gap bridge adjudicates the outage on the signal's return:
    // repair (snapshot+dN·λ in resolveHolds below), reset, or timeout.  Runs
    // before turbo_->resolve() so the resolver sees the signal as
    // held-continuous from the first missing epoch.  No-op unless
    // turbo_edit_bridge_lli is on.
    if (!turbo_ || !soft_lli_.enabled()) return;

    soft_lli_.registerGapHolds(modeled_obs, state_manager, is_bssd_pivot,
                               *turbo_, epoch_now_s, epoch_prefix);
}

// ===========================================================================
// S2: auxiliary detectors
// ===========================================================================

void CycleSlipManager::runAuxiliaryDetectors(SlipEpochWork& w) {
    const auto& modeled_obs = w.modeled_obs;
    auto& aux_events = w.aux_events;

    // Run the auxiliary detectors through the polymorphic contract and gather
    // their unified SlipEvents (applied as BSSD-aware resets after the MW
    // pass).  The MW detector is consumed below via its concrete
    // detectLargeMwResets: its specialized BSSD-aware pipeline needs the full
    // MwSlipResult WL-pair semantics that the unified SlipEvent projection
    // does not preserve, so calling detect() on it would only build a
    // projection the manager discards.
    SlipEpochContext slip_ctx(modeled_obs);
    for (const auto& d : detectors_) {
        if (d->kind() == SlipDetectorKind::MelbourneWubbena) continue;
        std::vector<SlipEvent> ev = d->detect(slip_ctx);
        aux_events.insert(aux_events.end(),
                          std::make_move_iterator(ev.begin()),
                          std::make_move_iterator(ev.end()));
    }
}

// ===========================================================================
// S4: MW / LLI detection
// ===========================================================================

void CycleSlipManager::detectMwSlips(SlipEpochWork& w) {
    const auto& modeled_obs = w.modeled_obs;
    auto& slip_results = w.slip_results;

    // --- MW / LLI detection (all dual-frequency pairs including EWL) ---
    slip_results =
        mw_ ? mw_->detectLargeMwResets(modeled_obs)
            : std::vector<MwSlipResult>{};
}

// ===========================================================================
// S5: TurboEdit MW+GF resolution
// ===========================================================================

void CycleSlipManager::resolveTurboEdit(SlipEpochWork& w) {
    const auto& log           = w.log;
    const auto& epoch_prefix  = w.epoch_prefix;
    const auto& modeled_obs   = w.modeled_obs;
    const auto& epoch_now_s   = w.epoch_now_s;
    const auto& is_bssd_pivot = w.is_bssd_pivot;
    const auto& slip_results  = w.slip_results;
    auto& te                 = w.te;
    auto& te_culprits_by_sat = w.te_culprits_by_sat;
    auto& te_slips_by_sat    = w.te_slips_by_sat;
    auto& te_suppress_by_sat = w.te_suppress_by_sat;

    if (!turbo_) return;

    // mw_ is guaranteed non-null when turbo_ exists (constructor
    // refuses TurboEdit without the MW detector).  The clean-pair
    // set gates the resolver's GF-only equal-slip split on
    // affirmative MW evidence rather than mere MW silence; the
    // across-gap WL evidence feeds the bridged solve's WL-settle
    // deferral (the post-restart MW mean re-estimating the gap step).
    te = turbo_->resolve(slip_results, mw_->getEpochCleanPairs(),
                         modeled_obs, sample_interval_s_,
                         mw_->getAcrossGapWlEvidence(epoch_now_s));
    for (const auto& r : te.repairs) {
        te_culprits_by_sat[r.satellite].insert(r.signal);
        te_slips_by_sat[r.satellite][r.signal] = r.dN;
    }
    for (const auto& ce : te.code_events) {
        te_suppress_by_sat[ce.satellite].insert(ce.sig1);
        te_suppress_by_sat[ce.satellite].insert(ce.sig2);
    }
    for (const auto& [msat, sigs] : te.moot_signals)
        te_suppress_by_sat[msat].insert(sigs.begin(), sigs.end());
    for (const auto& [csat, sigs] : te.clean_signals)
        te_suppress_by_sat[csat].insert(sigs.begin(), sigs.end());

    // WL-settle deferral: a pending satellite's still-slipped phase
    // stays out of the filter while the resolver waits for the
    // post-restart MW mean.  Holds are registered for its recently
    // converged phase signals (idempotent across the deferral; the
    // registered keys feed both the KF exclusion and the prune
    // protection).  Pivots are excluded as everywhere else; a
    // pivot's deferral runs to the resolver's timeout-reset.
    for (const auto& psat : te.pending_sats) {
        if (!soft_lli_.enabled()) break;
        if (is_bssd_pivot(psat)) {
            log->warn("{}TurboEdit WL-settle pending for BSSD pivot {} "
                      "— holds unavailable for a pivot; deferral will "
                      "run to its timeout-reset",
                      epoch_prefix, psat);
            continue;
        }
        soft_lli_.registerHold(psat, {}, *turbo_, epoch_now_s,
                               epoch_prefix);
    }
}

// ===========================================================================
// S6: soft-LLI bridge resolution
// ===========================================================================

void CycleSlipManager::resolveSoftLliHolds(SlipEpochWork& w) {
    const auto& epoch_prefix  = w.epoch_prefix;
    const auto& modeled_obs   = w.modeled_obs;
    auto& state_manager       = w.state_manager;
    const auto& meas_model    = w.meas_model;
    const auto& epoch_now_s   = w.epoch_now_s;
    const auto& te            = w.te;
    auto& result              = w.result;
    auto& soft_lli_repaired   = w.soft_lli_repaired;
    auto& reset_sats          = w.reset_sats;

    // --- Soft-LLI bridge resolution (pre-pass) -------------------------
    // Resolve outstanding holds against this epoch's bridge output before
    // the MW reset loop runs.  A satellite whose held ambiguities are
    // integer-repaired here is added to @c soft_lli_repaired, and the loop
    // below skips its MW mandate (wl_known=false → full reset), because the
    // covariance-restoring snapshot+dN·λ repair already handled the slip.
    // No-op unless turbo_edit_bridge_lli holds something.
    if (!turbo_ || soft_lli_.empty()) return;

    const SoftLliBridge::ResetFn reset_fn =
        [&](const std::string& key, const std::string& sat,
            const std::string& sig) {
            resetAmbiguityBssdAware(key, sat, sig, modeled_obs, state_manager,
                                    meas_model, epoch_prefix);
        };
    soft_lli_.resolveHolds(te, *turbo_, modeled_obs, state_manager,
                           epoch_now_s, epoch_prefix, soft_lli_repaired,
                           result.n_repaired, result.n_reset, reset_fn);
    if (!soft_lli_repaired.empty()) result.state_modified = true;
    reset_sats.insert(soft_lli_repaired.begin(), soft_lli_repaired.end());
    // A soft-LLI bridge "repair" overwrites the absolute value and
    // rebuilds the covariance from the pre-gap anchor
    // (SoftLliBridge::applyRestore); it is not an additive,
    // covariance-preserving repair.  The smoother therefore segments
    // these instead of compensating them (audit F1 / C2).
    epoch_reset_sats_.insert(soft_lli_repaired.begin(), soft_lli_repaired.end());
}

// ===========================================================================
// S7: MW results grouped per satellite
// ===========================================================================

void CycleSlipManager::groupMwPairsBySatellite(SlipEpochWork& w) const {
    const auto& slip_results = w.slip_results;
    auto& slips_by_sat       = w.slips_by_sat;
    auto& all_pairs_by_sat   = w.all_pairs_by_sat;

    // Group all MW results (slipped and clean) by satellite for
    // cross-consistency analysis.  The MW detector returns results only
    // for slipped pairs, so the clean pairs are recovered separately below.

    // Helper: unique pair key from sig1 + sig2 for deduplication.
    // The MW detector emits two result entries per uncombined pair (one
    // per signal's amb_key), but they represent the same MW constraint.
    auto pairKey = [](const MwSlipResult& sr) -> std::string {
        return sr.sig1 + "_" + sr.sig2;
    };

    // Build per-satellite groups of slipped results (deduplicated by pair)
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
        // MW arcs.  An active arc that produced no slip result means
        // dN_i - dN_j = 0 for that pair, which is the reference the
        // cross-consistency analysis needs.
        for (const auto& stats_key :
             (mw_ ? mw_->getActiveArcKeys() : std::vector<std::string>{})) {
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
}

// ===========================================================================
// S8: mandated keys per satellite
// ===========================================================================

void CycleSlipManager::collectMwMandates(SlipEpochWork& w) {
    const auto& slip_results = w.slip_results;
    auto& reset_keys_by_sat  = w.reset_keys_by_sat;
    auto& lli_sats           = w.lli_sats;

    // Collect all distinct amb_keys that need reset, grouped by satellite.
    // This includes LLI resets (empty sig1/sig2) and MW pair resets.
    for (const auto& sr : slip_results) {
        const std::string sat_id = sr.satellite.empty()
            ? satIdFromAmbiguityKey(sr.amb_key) : sr.satellite;
        reset_keys_by_sat[sat_id].insert(sr.amb_key);
        if (sr.sig1.empty() || sr.sig2.empty()) lli_sats.insert(sat_id);
        // Cache one representative slip per sat for the satellite-
        // status trace (Application::writeEpochTraces "Slip" rows).
        // Deliberately includes code-event-vetoed sats: the MW flag
        // is factual even when the phase reset gets suppressed.
        last_mw_slips_[sat_id] = sr;
    }
}

} // namespace GnssProcessor
