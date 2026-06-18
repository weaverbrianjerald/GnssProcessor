/**
/**
 * @file PivotManager.cpp
 * @brief Between-satellite single-difference pivot selection and handover.
 */

#include "filter/PivotManager.hpp"
#include "filter/CovarianceReReference.hpp"
#include <algorithm>
#include <cmath>
#include <limits>

namespace GnssProcessor {

// ===========================================================================
// Construction
// ===========================================================================

PivotManager::PivotManager(const GnssConfig& config, LogManager& log_manager)
    : config_(config), log_manager_(log_manager) {
}

// ===========================================================================
// Epoch map builders (static helpers)
// ===========================================================================

void PivotManager::buildEpochMaps(
    const std::vector<PppObservation>& observations,
    const StateManager& state_manager,
    std::map<std::string, SatInfo>& sat_info,
    std::map<std::string, ConstellationInfo>& cons_info) {

    sat_info.clear();
    cons_info.clear();

    for (const auto& obs : observations) {
        // Skip GLONASS by default — its FDMA per-channel receiver delay
        // breaks the standard BSSD assumption that receiver-side terms
        // cancel between satellites.  Opt back in via enable_glonass_bssd
        // (combined with estimate_sat_biases to absorb the ICB).
        if (obs.system == System::GLONASS && !config_.enableGlonassBssd()) continue;

        const std::string cons = PppUtils::getSystemString(obs.satellite);

        // Count distinct phase signal types for this satellite.
        size_t n_phase = 0;
        for (const auto& [sig, val] : obs.observed)
            if (!sig.empty() && sig[0] == 'L')
                ++n_phase;

        SatInfo info{ cons, obs.elevation, n_phase, -1.0 };
        info.eph_toe_age_s = obs.eph_toe_age_s;   // broadcast extrapolation magnitude (|age|) for pivot freshness

        // Worst-case float-ambiguity variance across this sat's phase signals.
        // The handover's T·P·T' re-reference inflates every other satellite's
        // same-signal SD ambiguity by P(pivot_amb, pivot_amb), so a single
        // poorly-converged signal on the candidate pivot hurts the entire
        // SD frame — using the worst per-signal variance is the right pivot-
        // fitness signal.
        for (const auto& [sig, val] : obs.observed) {
            if (sig.empty() || sig[0] != 'L') continue;
            const int aidx = state_manager.getAmbiguityIndex(obs.satellite + "_" + sig);
            if (aidx < 0) continue;
            const double v = state_manager.getStateVariance(aidx);
            if (v > info.max_amb_variance) info.max_amb_variance = v;
        }

        sat_info[obs.satellite] = info;

        auto& ci = cons_info[cons];
        if (n_phase > ci.max_phase_sigs)
            ci.max_phase_sigs = n_phase;
    }

    // Second pass: pick the best candidate per constellation using
    // lexicographic (coverage, score) ordering.  Full signal coverage is
    // a hard tier — any full-coverage candidate beats any partial-coverage
    // candidate regardless of score, because under BSSD the pivot must
    // track every signal a non-pivot tracks (else the non-pivot is
    // dropped as an orphan).  Within a coverage tier, @c scorePivotCandidate
    // breaks ties on float-ambiguity convergence and elevation.  Fallback
    // to highest-available coverage is implicit: when no satellite has
    // full coverage the comparator naturally selects max-coverage.
    for (auto& [cons_name, ci] : cons_info) {
        for (const auto& [sat, si] : sat_info) {
            if (si.constellation != cons_name) continue;

            const double score = scorePivotCandidate(sat, si);

            const bool higher_cov  = si.phase_sig_count > ci.best_coverage;
            const bool same_cov_higher_score =
                (si.phase_sig_count == ci.best_coverage) && (score > ci.best_score);
            if (ci.best_sat.empty() || higher_cov || same_cov_higher_score) {
                ci.best_sat      = sat;
                ci.best_score    = score;
                ci.best_coverage = si.phase_sig_count;
            }
            if (si.elevation > ci.highest_el) {
                ci.highest_sat = sat;
                ci.highest_el  = si.elevation;
            }
        }
    }
}

std::set<std::string> PivotManager::collectPhaseSignals(
    const std::vector<PppObservation>& observations) {

    std::set<std::string> phase_sigs;
    for (const auto& obs : observations)
        for (const auto& [sig, val] : obs.observed)
            if (!sig.empty() && sig[0] == 'L')
                phase_sigs.insert(sig);
    return phase_sigs;
}

// ===========================================================================
// Pivot candidate scoring
// ===========================================================================

double PivotManager::scorePivotCandidate(
    const std::string& /*sat*/, const SatInfo& si) const {

    double score = 0.0;

    // State-convergence penalty.  On handover the new pivot's variance is
    // added to every same-group SD state via P'(i,i) = P(i,i) + P(piv,piv)
    // − 2·P(i,piv).  Penalising √(worst ambiguity variance) keeps the
    // selector away from pivots whose own state is still settling.
    //
    // Form: smooth penalty saturating at -1.0 (one metre of equivalent
    // 1-sigma ambiguity uncertainty); flat -1.0 sentinel for "no state
    // yet" (the candidate would inherit a fresh initial-std state if
    // elected, which is worse than any established candidate).  On the
    // first BSSD epoch every candidate within a coverage tier has no
    // state, so the penalty is constant across the tier and ordering
    // falls through to elevation — no special initial-election path
    // needed.
    if (si.max_amb_variance < 0.0) {
        score -= 1.0;
    } else {
        score -= std::min(1.0, std::sqrt(std::max(0.0, si.max_amb_variance)));
    }

    // Elevation magnitude (0..1).  Within a tier (same coverage, similar
    // convergence) the higher-elevation candidate wins.
    score += si.elevation / 90.0;

    // Low-elevation guardrail: below 20° the troposphere and multipath
    // residuals dominate the formal stochastic model, so we keep this
    // hard penalty regardless of how well-converged the float state
    // looks numerically.
    if (si.elevation < 20.0)
        score -= 0.5;

    return score;
}

// ===========================================================================
// Pivot candidate selection
// ===========================================================================

std::map<std::string, std::string> PivotManager::selectPivotCandidates(
    const std::map<std::string, ConstellationInfo>& cons_info,
    const std::map<std::string, SatInfo>& sat_info,
    const std::set<std::string>& exclude_sats) const {

    const double pivot_min_el = config_.getBssdPivotMinElevation();
    std::map<std::string, std::string> candidates;

    // Ephemeris-freshness guardrail for pivot election.  A satellite whose
    // broadcast ephemeris is much staler (by |obs − toe|) than its
    // constellation's freshest qualified peer is barred from being the PIVOT:
    // a stale-eph pivot's extrapolated orbit/clock steps every SD row it
    // anchors (observed live as a Galileo pivot pinned to a 1 h-old I/NAV that
    // detonated the whole SD frame while its peers were minutes fresh).  Gated
    // on SPREAD: when every candidate is within kPivotToeSpreadMaxS of the
    // freshest, ranking is unchanged — only an outlier-stale candidate is
    // displaced.  It is still used as a NON-pivot; the freshest peer always
    // survives the gate (diff 0), so the candidate set is never emptied.
    constexpr double kPivotToeSpreadMaxS = 1200.0;   // 20 min
    auto freshestToeAgeAbs = [&](const std::string& cons) -> double {
        double best = std::numeric_limits<double>::max();
        for (const auto& [sat, si] : sat_info) {
            if (si.constellation != cons) continue;
            if (exclude_sats.count(sat)) continue;
            if (si.elevation < pivot_min_el) continue;
            best = std::min(best, std::abs(si.eph_toe_age_s));
        }
        return best;
    };

    // Helper: pick the best (coverage, score) sat in @p cons that isn't
    // in @p exclude_sats and meets the elevation threshold.  Mirrors the
    // ranking from buildEpochMaps that populates ci.best_sat, but
    // applied online with the exclude filter so a sat with a known
    // current-epoch MW slip can never be selected.  Returns empty when
    // nothing qualifies (caller falls back).
    auto pickExclusionAwareBest = [&](const std::string& cons) -> std::string {
        std::string picked;
        size_t      best_coverage = 0;
        double      best_score    = -1e9;
        double      highest_el    = -1.0;
        std::string fallback_el_pick;
        const double fresh_abs = freshestToeAgeAbs(cons);   // constellation's freshest |toe age|
        for (const auto& [sat, si] : sat_info) {
            if (si.constellation != cons) continue;
            if (exclude_sats.count(sat)) continue;
            if (si.elevation < pivot_min_el) continue;
            // Bar an outlier-stale-ephemeris candidate from pivot election (still
            // usable as a non-pivot); the freshest peer always passes (diff 0).
            if (std::abs(si.eph_toe_age_s) > fresh_abs + kPivotToeSpreadMaxS) continue;

            const double score = scorePivotCandidate(sat, si);
            const bool higher_cov  = si.phase_sig_count > best_coverage;
            const bool same_cov_higher_score =
                (si.phase_sig_count == best_coverage) && (score > best_score);
            if (picked.empty() || higher_cov || same_cov_higher_score) {
                picked        = sat;
                best_coverage = si.phase_sig_count;
                best_score    = score;
            }
            if (si.elevation > highest_el) {
                fallback_el_pick = sat;
                highest_el       = si.elevation;
            }
        }
        return picked.empty() ? fallback_el_pick : picked;
    };

    for (const auto& [cons, ci] : cons_info) {
        auto prev_it = pivot_sat_.find(cons);
        if (prev_it != pivot_sat_.end()) {
            const std::string& old_pivot = prev_it->second;
            auto si = sat_info.find(old_pivot);
            // Sticky logic: keep the previous pivot if it's still high
            // enough AND not in the exclude set.  A pivot whose current
            // epoch carries an MW slip on the constellation's affected
            // signal should never be sticky — the forced-handover caller
            // built exclude_sats precisely to break that stickiness.
            if (si != sat_info.end()
                && si->second.elevation >= pivot_min_el
                && !exclude_sats.count(old_pivot)
                && std::abs(si->second.eph_toe_age_s)
                       <= freshestToeAgeAbs(cons) + kPivotToeSpreadMaxS) {
                candidates[cons] = old_pivot;
                continue;
            }
        }
        // Always use the elevation-gated, coverage(=phase)-preferring online
        // pick (previously only when an exclude set was active).  pickExclusion-
        // AwareBest filters on pivot_min_el and ranks by phase_sig_count, so a
        // cm-grade (phase-carrying) sat always beats a broadcast-grade (phase-
        // less, coverage-0) one for any constellation that has both — closing
        // the cold-start hole where the precomputed best_sat/highest_sat (no
        // elevation gate) could seat a low-el or broadcast-grade pivot and inject
        // a coherent offset into every SD code row.  Falls back to the
        // precomputed best/highest only when NO satellite clears the elevation
        // mask (a genuine code-only-float constellation, where a code-only pivot
        // is correct).  Broadcast-grade sats are also removed upstream when
        // drop_broadcast_grade is set, making this belt-and-suspenders there.
        const std::string picked = pickExclusionAwareBest(cons);
        candidates[cons] = !picked.empty()
                         ? picked
                         : (ci.best_sat.empty() ? ci.highest_sat : ci.best_sat);
    }
    return candidates;
}

// ===========================================================================
// Pivot disturbance handling
// ===========================================================================

bool PivotManager::isPivotDisturbed(
    const std::string& sat,
    const std::set<std::string>& phase_sigs,
    const StateManager& state_manager,
    const Eigen::VectorXd& x,
    const Eigen::MatrixXd& P) const {

    for (const auto& sig : phase_sigs) {
        const std::string key = sat + "_" + sig;
        const int idx = state_manager.getAmbiguityIndex(key);
        if (idx < 0) continue;
        if (std::abs(x(idx)) > 1e-6 || P(idx, idx) > 1e-12)
            return true;
    }
    return false;
}

bool PivotManager::handleDisturbedPivot(
    const std::string& cons,
    std::string& pivot,
    const std::map<std::string, SatInfo>& sat_info,
    const std::set<std::string>& phase_sigs,
    const StateManager& state_manager,
    Eigen::VectorXd& x,
    Eigen::MatrixXd& P,
    const std::string& epoch_prefix,
    const std::set<std::string>& exclude_sats) {

    auto log = log_manager_.getLogger("bssd_pivot_manager");

    log->warn("{}BSSD pivot {} disturbed in {}. "
              "Searching for healthy replacement.",
              epoch_prefix, pivot, cons);

    // Find the best replacement in this constellation (excluding the
    // current disturbed pivot AND any sat in @p exclude_sats) using the
    // same lex (coverage, score) ordering as @c buildEpochMaps: any
    // full-coverage candidate beats any partial-coverage one, score
    // breaks ties within a tier.  exclude_sats lets the caller block
    // sats with known current-epoch issues (e.g. MW slip on the same
    // signal whose ambiguity reset triggered this handover) from
    // becoming the replacement, which would just propagate the slip
    // into the new SD frame.
    std::string best_replacement;
    double best_score      = -1e9;
    size_t best_coverage   = 0;
    for (const auto& [sat, si] : sat_info) {
        if (si.constellation != cons || sat == pivot) continue;
        if (exclude_sats.count(sat)) continue;
        const double score = scorePivotCandidate(sat, si);

        const bool higher_cov  = si.phase_sig_count > best_coverage;
        const bool same_cov_higher_score =
            (si.phase_sig_count == best_coverage) && (score > best_score);
        if (best_replacement.empty() || higher_cov || same_cov_higher_score) {
            best_replacement = sat;
            best_score       = score;
            best_coverage    = si.phase_sig_count;
        }
    }

    if (!best_replacement.empty()) {
        auto si_it = sat_info.find(best_replacement);
        const double repl_el = (si_it != sat_info.end()) ? si_it->second.elevation : -1.0;
        log->info("{}BSSD pivot disturbance recovery for {}: {} -> {} "
                  "(elev={:.1f}, score={:.2f}). Using lossless handover.",
                  epoch_prefix, cons, pivot, best_replacement, repl_el, best_score);
        pivot = best_replacement;
        return true;
    }

    // Single-satellite constellation � re-zero the disturbed pivot in place.
    log->warn("{}BSSD pivot disturbance in {}: no replacement found. "
              "Re-zeroing disturbed pivot {} in place.",
              epoch_prefix, cons, pivot);

    for (const auto& sig : phase_sigs) {
        const std::string piv_key = pivot + "_" + sig;
        const int piv_idx = state_manager.getAmbiguityIndex(piv_key);
        if (piv_idx < 0) continue;
        const auto pidx = static_cast<Eigen::Index>(piv_idx);
        x(pidx) = 0.0;
        P.row(pidx).setZero();
        P.col(pidx).setZero();
    }
    return false;
}

// ===========================================================================
// Common T�P�T' covariance transform
// ===========================================================================

void PivotManager::applyCovarianceReReference(
    const std::vector<Eigen::Index>& group_indices,
    Eigen::Index pivot_idx,
    Eigen::MatrixXd& P) {
    // Delegate to the shared BSSD transform (single source of truth, reused by
    // the network producer).  The user-side path stays UNGUARDED — at user
    // scale the groups are small and handovers rare, so no PSD guard is applied
    // here (see CovarianceReReference.hpp / conformance doc §8.7).  The network
    // producer pairs applyBssd with CovReReference::enforcePsd.
    CovReReference::applyBssd(group_indices, pivot_idx, P);
}

// ===========================================================================
// Re-reference: ambiguities
// ===========================================================================

void PivotManager::reReferenceAmbiguities(
    const std::string& cons,
    const std::string& pivot,
    const std::set<std::string>& phase_sigs,
    StateManager& state_manager,
    Eigen::VectorXd& x,
    Eigen::MatrixXd& P,
    const std::string& epoch_prefix) {

    auto log = log_manager_.getLogger("bssd_pivot_manager");

    for (const auto& sig : phase_sigs) {
        const std::string new_ref_key = pivot + "_" + sig;
        const int new_ref_idx = state_manager.getAmbiguityIndex(new_ref_key);
        if (new_ref_idx < 0) continue;

        const double n_new_ref = x(new_ref_idx);
        const auto ri = static_cast<Eigen::Index>(new_ref_idx);

        // Single pass: collect group indices and transform state vector.
        std::vector<Eigen::Index> group_indices;
        for (const auto& [amb_key, amb_idx] : state_manager.getAmbiguities()) {
            const auto us = amb_key.find('_');
            if (us == std::string::npos) continue;
            const std::string amb_sat = amb_key.substr(0, us);
            const std::string amb_sig = amb_key.substr(us + 1);
            if (amb_sig != sig) continue;
            if (PppUtils::getSystemString(amb_sat) != cons) continue;

            const auto aidx = static_cast<Eigen::Index>(amb_idx);
            group_indices.push_back(aidx);

            const double old_val = x(aidx);
            x(aidx) = (amb_sat == pivot) ? 0.0 : (old_val - n_new_ref);

            log->debug("{}  Handover {}: {:.4f} -> {:.4f}",
                       epoch_prefix, amb_key, old_val, x(aidx));
        }

        applyCovarianceReReference(group_indices, ri, P);
    }
}

// ===========================================================================
// Re-reference: ionosphere
// ===========================================================================

void PivotManager::reReferenceIonosphere(
    const std::string& cons,
    const std::string& pivot,
    StateManager& state_manager,
    Eigen::VectorXd& x,
    Eigen::MatrixXd& P,
    const std::string& epoch_prefix) {

    if (!config_.isUncombinedEnabled()) return;

    auto log = log_manager_.getLogger("bssd_pivot_manager");

    const int new_piv_iono_idx = state_manager.getIonosphereIndex(pivot);
    if (new_piv_iono_idx < 0) return;
    const auto pi = static_cast<Eigen::Index>(new_piv_iono_idx);
    const double iono_piv = x(pi);

    // Single pass: collect group and transform state vector.
    std::vector<Eigen::Index> iono_group;
    for (const auto& [sat, idx] : state_manager.getIonosphereStates()) {
        if (PppUtils::getSystemString(sat) != cons) continue;
        const auto ii = static_cast<Eigen::Index>(idx);
        iono_group.push_back(ii);

        const double old_val = x(ii);
        x(ii) = (sat == pivot) ? 0.0 : (old_val - iono_piv);
        log->debug("{}  Iono handover {}: {:.4f} -> {:.4f}",
                   epoch_prefix, sat, old_val, x(ii));
    }

    applyCovarianceReReference(iono_group, pi, P);
}

// ===========================================================================
// Re-reference: satellite biases
// ===========================================================================

void PivotManager::reReferenceSatBiases(
    const std::string& cons,
    const std::string& pivot,
    StateManager& state_manager,
    Eigen::VectorXd& x,
    Eigen::MatrixXd& P,
    const std::string& epoch_prefix) {

    if (!config_.estimateSatBiases() || !config_.isUncombinedEnabled()) return;

    auto log = log_manager_.getLogger("bssd_pivot_manager");

    // Group sat bias states by signal within this constellation.
    std::map<std::string, std::vector<std::pair<std::string, Eigen::Index>>> sig_groups;
    std::map<std::string, Eigen::Index> sig_pivot_idx;

    for (const auto& [sb_key, sb_idx] : state_manager.getSatBiasStates()) {
        const auto bar = sb_key.find('|');
        if (bar == std::string::npos) continue;
        const std::string sat = sb_key.substr(0, bar);
        if (PppUtils::getSystemString(sat) != cons) continue;
        const std::string sig = sb_key.substr(bar + 1);
        const auto si = static_cast<Eigen::Index>(sb_idx);
        sig_groups[sig].emplace_back(sat, si);
        if (sat == pivot) sig_pivot_idx[sig] = si;
    }

    for (const auto& [sig, members] : sig_groups) {
        auto pi_it = sig_pivot_idx.find(sig);
        if (pi_it == sig_pivot_idx.end()) continue;
        const Eigen::Index pi = pi_it->second;
        const double piv_val = x(pi);

        // Single pass: collect group and transform state vector.
        std::vector<Eigen::Index> group_indices;
        for (const auto& [sat, si] : members) {
            group_indices.push_back(si);
            const double old_val = x(si);
            x(si) = (sat == pivot) ? 0.0 : (old_val - piv_val);
            log->debug("{}  SatBias handover {}: {:.4f} -> {:.4f}",
                       epoch_prefix, sat + "|" + sig, old_val, x(si));
        }

        applyCovarianceReReference(group_indices, pi, P);
    }
}

// ===========================================================================
// Main pivot handover
// ===========================================================================

void PivotManager::requestForcedHandover(const std::string& constellation,
                                          const std::string& reason,
                                          const std::set<std::string>& exclude_sats,
                                          const std::set<std::string>& reinflate_sats) {
    auto log = log_manager_.getLogger("bssd_pivot_manager");
    log->warn("Forced pivot handover requested for {} ({}). "
              "Will be processed at next handlePivotHandover call.",
              constellation, reason);
    forced_handover_requests_.insert(constellation);
    if (!exclude_sats.empty()) {
        auto& ex = forced_handover_exclude_[constellation];
        ex.insert(exclude_sats.begin(), exclude_sats.end());
    }
    if (!reinflate_sats.empty()) {
        auto& rf = forced_handover_reinflate_[constellation];
        rf.insert(reinflate_sats.begin(), reinflate_sats.end());
    }
}

void PivotManager::handlePivotHandover(
    const std::vector<PppObservation>& observations,
    StateManager& state_manager,
    const std::string& epoch_prefix,
    bool forced_only,
    const std::set<std::string>& exclude_sats) {

    auto log = log_manager_.getLogger("bssd_pivot_manager");

    // --- Build epoch snapshot (single pass over observations) ---
    std::map<std::string, SatInfo> sat_info;
    std::map<std::string, ConstellationInfo> cons_info;
    buildEpochMaps(observations, state_manager, sat_info, cons_info);

    // Reset the per-epoch retired-pivot guard on each epoch's first-pass
    // handover (forced_only == false).  Within the epoch's rebuild loop the
    // forced re-entries (forced_only == true) then accumulate retirements,
    // so a satellite handed off this epoch can never be bounced back to on a
    // later pass — the backstop that breaks the post-fit handover flip-flop.
    if (!forced_only) {
        retired_pivots_this_epoch_.clear();
        // Smoother handover log is per-epoch; the first-pass handover marks the
        // epoch boundary (handlePivotHandover runs every BSSD epoch via
        // preparePivots).  Cleared here, populated by clean legs below.
        clean_handovers_.clear();
    }

    // Effective replacement-exclusion set: the caller-supplied set (MW
    // pre-filter path) unioned with the post-fit detector's per-request
    // excludes and every satellite already retired as a pivot this epoch.
    // Satellite IDs are constellation-prefixed, so a single flat set is
    // safe to share across constellations.
    std::set<std::string> effective_exclude = exclude_sats;
    for (const auto& [c, sats] : forced_handover_exclude_)
        effective_exclude.insert(sats.begin(), sats.end());
    for (const auto& [c, sats] : retired_pivots_this_epoch_)
        effective_exclude.insert(sats.begin(), sats.end());

    // --- Sticky pivot selection with elevation hysteresis ---
    // @c effective_exclude is forwarded so the sticky/fallback selection
    // skips the disturbed pivot, the detector's co-disturbed outliers, and
    // any satellite already retired this epoch.
    auto new_pivot_sat = selectPivotCandidates(cons_info, sat_info, effective_exclude);

    // First BSSD epoch: just record pivots, no handover needed.
    if (pivot_sat_.empty()) {
        pivot_sat_ = new_pivot_sat;
        for (const auto& [cons, sat] : pivot_sat_) {
            auto si_it = sat_info.find(sat);
            const double el = (si_it != sat_info.end()) ? si_it->second.elevation : -1.0;
            const double sc = (si_it != sat_info.end())
                                  ? scorePivotCandidate(sat, si_it->second) : 0.0;
            log->info("{}BSSD initial pivot for {}: {} (elev={:.1f}, score={:.2f}, phase_sigs={})",
                      epoch_prefix, cons, sat, el, sc,
                      (si_it != sat_info.end()) ? si_it->second.phase_sig_count : 0);
        }
        return;
    }

    // --- Collect phase signal types ---
    const auto phase_sigs = collectPhaseSignals(observations);

    Eigen::VectorXd x = state_manager.getStateVector();
    Eigen::MatrixXd P = state_manager.getCovariance();
    bool state_modified = false;
    bool cov_modified   = false;

    const double pivot_min_el = config_.getBssdPivotMinElevation();

    // Code-detected handovers carry a per-constellation set of non-pivot sats
    // whose iono / sat-bias state absorbed the pivot's code bias.  We capture
    // them here as the forced request is consumed, but defer the actual
    // re-inflation until AFTER the lossless re-reference has been written back
    // (re-inflating the local @c P before the @c T·P·Tᵀ would corrupt the
    // re-reference; re-inflating after commit lets us reuse StateManager's
    // value-preserving, pivot-skipping primitive unchanged).
    std::map<std::string, std::set<std::string>> reinflate_pending;

    for (auto& [cons, pivot] : new_pivot_sat) {
        const std::string& old_pivot = pivot_sat_.count(cons)
                                     ? pivot_sat_.at(cons) : pivot;

        // --- Check for pivot disturbance ---
        // Only the *current* pivot (old_pivot) can be disturbed � its
        // frozen (0, 0) state may have been corrupted by an external
        // mechanism (cycle-slip reset, outlier handler, etc.).
        //
        // The candidate (pivot) from selectPivotCandidates is a regular
        // non-pivot satellite with non-zero ambiguity and variance;
        // checking it would always flag "disturbed" and trigger a
        // spurious recovery search.
        //
        // When the candidate differs from the old pivot (elevation-based
        // handover), no disturbance check is needed � the old pivot is
        // simply being replaced because it dropped below the elevation
        // threshold, and the lossless handover handles this correctly.
        // Forced-handover requests come from external detectors (KF
        // post-fit residual analyzer) when a pivot's *observation* has
        // slipped while its filter STATE remains intact.  Treat the
        // constellation as if isPivotDisturbed had returned true so the
        // existing handleDisturbedPivot recovery path runs end-to-end:
        // pick a healthy replacement, fall through to the lossless
        // re-referencing block below.  Consume the request whether or not
        // a replacement was found — a fresh request must be re-issued
        // each epoch the detector still wants the handover.
        // The disturbance check is suppressed in @c forced_only mode
        // (same-epoch rebuild re-entry): we only want to honour
        // explicit KF requests there, never re-detect disturbance on
        // a pivot whose ambiguity was just touched by an in-rebuild
        // outlier handler — that's the cascading-handover scenario
        // the same-epoch gate was originally added to prevent.
        const bool forced = forced_handover_requests_.count(cons) > 0;
        const bool disturbed = !forced_only
            && isPivotDisturbed(old_pivot, phase_sigs, state_manager, x, P);

        // Consume the forced request (and its stashed exclusions) up front,
        // the moment we start handling this constellation.  effective_exclude
        // was snapshotted before the loop, so erasing here does not affect
        // this pass's selection.  Consuming here — rather than only inside the
        // sticky disturbance branch below — ensures the request never lingers
        // when the candidate selection itself already moved the pivot off an
        // excluded old pivot; otherwise the stale request would re-fire a
        // spurious handover off the new, healthy pivot on the following pass.
        // A fresh request must be re-issued each pass the detector still wants
        // the handover.
        if (forced) {
            forced_handover_requests_.erase(cons);
            forced_handover_exclude_.erase(cons);
            auto rf_it = forced_handover_reinflate_.find(cons);
            if (rf_it != forced_handover_reinflate_.end()) {
                reinflate_pending[cons] = std::move(rf_it->second);
                forced_handover_reinflate_.erase(rf_it);
            }
        }

        if (pivot == old_pivot && (forced || disturbed)) {
            if (forced) {
                log->warn("{}BSSD pivot {} in {}: forced handover honored "
                          "(KF-detected pivot slip)",
                          epoch_prefix, old_pivot, cons);
            }
            if (!handleDisturbedPivot(cons, pivot, sat_info, phase_sigs,
                                      state_manager, x, P, epoch_prefix,
                                      effective_exclude)) {
                // Re-zeroed in place, no handover.
                state_modified = true;
                cov_modified   = true;
                continue;
            }
            // pivot was updated to the replacement — fall through to handover.
        }

        if (pivot == old_pivot) continue;

        // The outgoing pivot is retired for the remainder of this epoch so a
        // later rebuild pass cannot bounce the SD-frame anchor back to it.
        retired_pivots_this_epoch_[cons].insert(old_pivot);

        // --- Lossless handover ---
        const bool was_disturbed = isPivotDisturbed(old_pivot, phase_sigs,
                                                     state_manager, x, P);
        auto si_old = sat_info.find(old_pivot);
        auto si_new = sat_info.find(pivot);
        const double new_score = (si_new != sat_info.end())
                                     ? scorePivotCandidate(pivot, si_new->second) : 0.0;
        log->info("{}BSSD pivot handover for {}: {} -> {} "
                  "(old elev={:.1f}, new elev={:.1f}, score={:.2f}, threshold={:.1f}, disturbed={})",
                  epoch_prefix, cons, old_pivot, pivot,
                  (si_old != sat_info.end()) ? si_old->second.elevation : -1.0,
                  (si_new != sat_info.end()) ? si_new->second.elevation : -1.0,
                  new_score, pivot_min_el, was_disturbed ? "yes" : "no");

        // Record CLEAN handovers (undisturbed, non-forced datum change) for the
        // RTS smoother.  Only these are losslessly invertible back to the old
        // pivot: a forced / disturbed handover fires precisely because the old
        // pivot's ambiguity slipped (discontinuous across the epoch), so the
        // smoother must segment it instead.  old_pivot is captured by value here
        // (the reference is rebound when pivot_sat_ is overwritten below).  P2-1.
        if (!forced && !disturbed && !was_disturbed) {
            clean_handovers_[cons] = { old_pivot, pivot };
        }

        reReferenceAmbiguities(cons, pivot, phase_sigs,
                               state_manager, x, P, epoch_prefix);
        reReferenceIonosphere(cons, pivot, state_manager, x, P, epoch_prefix);
        reReferenceSatBiases(cons, pivot, state_manager, x, P, epoch_prefix);

        // When the handover was triggered by a disturbed pivot, the old
        // pivot's ambiguity values may be corrupt (they were frozen at 0
        // but with inflated variance).  After re-referencing those values
        // become  0 ? x(new_pivot)  which is incorrect.
        // Re-seed them with proper SD observation-based seeds so the KF
        // starts from a meaningful state rather than producing large
        // innovations that cascade through dozens of outlier passes.
        if (was_disturbed) {
            const double amb_std = config_.getInitialStd().ambiguity;
            const PppObservation* obs_old = nullptr;
            const PppObservation* obs_new = nullptr;
            for (const auto& obs : observations) {
                if (obs.satellite == old_pivot) obs_old = &obs;
                if (obs.satellite == pivot)     obs_new = &obs;
                if (obs_old && obs_new) break;
            }

            for (const auto& sig : phase_sigs) {
                const std::string key = old_pivot + "_" + sig;
                const int idx = state_manager.getAmbiguityIndex(key);
                if (idx < 0) continue;
                const auto aidx = static_cast<Eigen::Index>(idx);

                double sd_seed = 0.0;
                if (obs_old && obs_new) {
                    // The new pivot may use a different tracking attribute
                    // on the same band (e.g. L1W vs L1C).  Find the
                    // actual signal for a correct absolute seed.
                    std::string new_sig = sig;
                    if (!obs_new->observed.count(sig) && sig.size() >= 2) {
                        const std::string band = sig.substr(0, 2);
                        for (const auto& [s, v] : obs_new->observed) {
                            if (s.size() >= 2 && s.substr(0, 2) == band) {
                                new_sig = s;
                                break;
                            }
                        }
                    }
                    sd_seed = state_manager.computeAmbiguitySeed(*obs_old, sig, x)
                            - state_manager.computeAmbiguitySeed(*obs_new, new_sig, x);
                }

                log->info("{}  Re-seed disturbed {}: {:.4f} -> {:.4f} (std={:.1f})",
                          epoch_prefix, key, x(aidx), sd_seed, amb_std);
                x(aidx) = sd_seed;
                P.row(aidx).setZero();
                P.col(aidx).setZero();
                P(aidx, aidx) = amb_std * amb_std;
            }
        }

        // --- Reset orphaned ambiguities for signals the new pivot lacks ---
        // After re-referencing, any phase signal for which the new pivot
        // has no ambiguity state is still referenced to the old pivot.
        // Even if the new pivot covers the same band (e.g. L1W covers L1),
        // the SD ambiguity was parameterized as N(sat) - N(old_pivot) and
        // must be reset so ensureRequiredStates() can re-seed it as
        // N(sat) - N(new_pivot) using band-matched observations.
        {
            // Collect the new pivot's exact phase signal types.
            std::set<std::string> pivot_phase_sigs;
            for (const auto& obs : observations) {
                if (obs.satellite != pivot) continue;
                for (const auto& [sig, val] : obs.observed)
                    if (!sig.empty() && sig[0] == 'L')
                        pivot_phase_sigs.insert(sig);
                break;
            }

            const double amb_std = config_.getInitialStd().ambiguity;
            for (const auto& sig : phase_sigs) {
                // The new pivot has this exact signal � reReferenceAmbiguities
                // already handled it.
                if (pivot_phase_sigs.count(sig)) continue;

                // All same-constellation ambiguities for this signal are
                // orphaned (still referenced to the old pivot).
                for (const auto& [amb_key, amb_idx] : state_manager.getAmbiguities()) {
                    const auto us = amb_key.find('_');
                    if (us == std::string::npos) continue;
                    const std::string amb_sat = amb_key.substr(0, us);
                    const std::string amb_sig = amb_key.substr(us + 1);
                    if (amb_sig != sig) continue;
                    if (PppUtils::getSystemString(amb_sat) != cons) continue;

                    const auto aidx = static_cast<Eigen::Index>(amb_idx);
                    log->info("{}  Orphaned ambiguity {} (new pivot {} lacks {}): "
                              "resetting for re-seed (old={:.4f})",
                              epoch_prefix, amb_key, pivot, sig, x(aidx));
                    P.row(aidx).setZero();
                    P.col(aidx).setZero();
                    P(aidx, aidx) = amb_std * amb_std;
                    state_manager.markAmbiguityPendingReseed(amb_key);
                }
            }
        }

        state_modified = true;
        cov_modified   = true;
    }

    if (state_modified) state_manager.setStateVector(x);
    if (cov_modified)   state_manager.setCovariance(P);

    pivot_sat_ = new_pivot_sat;

    // --- Post-handover code-domain state scrub --------------------------
    // Re-inflate the iono / sat-bias covariance of the non-pivot satellites
    // whose state absorbed a coherent pivot-side CODE bias before this forced
    // handover.  Done AFTER the re-referenced covariance is committed above so
    // these StateManager primitives operate on the new SD frame; they preserve
    // the (re-labelled) state VALUE and break only the covariance partition,
    // letting the now-clean frame re-estimate the contaminated states over the
    // next few epochs instead of carrying a converged-but-wrong value under a
    // tight P — the gap that made code-only pivot outliers recur every epoch.
    // The primitives skip frozen pivots via their internal diag==0 guard; we
    // also skip the freshly-elected pivot explicitly to avoid a noisy refusal
    // log.  No-op when there was no code contamination (reinflate_pending empty)
    // or the feature is disabled; iono/sat-bias branches self-disable when those
    // states don't exist (e.g. combined-IF or phase-present runs).
    const auto& sr_cfg = config_.getSlipStateReinflationConfig();
    if (sr_cfg.enabled) {
        for (const auto& [cons, sats] : reinflate_pending) {
            auto piv_it = pivot_sat_.find(cons);
            const std::string new_piv =
                (piv_it != pivot_sat_.end()) ? piv_it->second : std::string();
            std::string scrubbed;
            for (const auto& sat : sats) {
                if (sat == new_piv) continue;  // now the frozen datum
                if (sr_cfg.iono_std_m > 0.0)
                    state_manager.reinflateIonosphereVariance(
                        sat, sr_cfg.iono_std_m * sr_cfg.iono_std_m);
                if (sr_cfg.sat_bias_std_m > 0.0)
                    state_manager.reinflateSatBiasVariancesForSat(
                        sat, sr_cfg.sat_bias_std_m * sr_cfg.sat_bias_std_m);
                if (!scrubbed.empty()) scrubbed += ",";
                scrubbed += sat;
            }
            if (!scrubbed.empty())
                log->info("{}Code-handover state scrub for {}: re-inflated "
                          "iono/sat-bias variance on {} (iono_std={:.2f} m, "
                          "sb_std={:.2f} m) so the new SD frame re-estimates the "
                          "absorbed pivot code bias",
                          epoch_prefix, cons, scrubbed,
                          sr_cfg.iono_std_m, sr_cfg.sat_bias_std_m);
        }
    }
}

// ===========================================================================
// Freeze pivot covariances
// ===========================================================================

void PivotManager::freezePivotAmbiguityCovariance(StateManager& state_manager) {
    if (pivot_sat_.empty()) return;

    Eigen::MatrixXd Pf = state_manager.getCovariance();
    bool any_frozen = false;
    for (const auto& [cons, piv] : pivot_sat_) {
        for (const auto& [amb_key, amb_idx] : state_manager.getAmbiguities()) {
            const auto us = amb_key.find('_');
            if (us == std::string::npos) continue;
            if (amb_key.substr(0, us) != piv) continue;
            const std::string amb_sig = amb_key.substr(us + 1);
            if (amb_sig.empty() || amb_sig[0] != 'L') continue;
            const auto aidx = static_cast<Eigen::Index>(amb_idx);
            if (Pf(aidx, aidx) != 0.0) {
                Pf.row(aidx).setZero();
                Pf.col(aidx).setZero();
                any_frozen = true;
            }
        }
    }
    if (any_frozen) state_manager.setCovariance(Pf);
}

void PivotManager::freezePivotIonoAndBiasCovariance(StateManager& state_manager) {
    if (pivot_sat_.empty()) return;

    // Always register the pivot list with StateManager so process noise
    // is suppressed for *all* frozen pivot states (ambiguities, iono,
    // sat-biases).  This must run in every BSSD mode — the ambiguity
    // freeze is what prevents the spurious-handover flip-flop, and
    // pivot ambiguities exist in both PPP IF combined and PPP-RTK
    // uncombined modes.
    std::set<std::string> frozen_sats;
    for (const auto& [cons, piv] : pivot_sat_)
        frozen_sats.insert(piv);
    state_manager.setFrozenPivotSatellites(frozen_sats);

    // Iono and sat-bias states only exist in uncombined mode — only
    // bother zeroing those P entries when those states actually exist.
    if (!config_.isUncombinedEnabled()) return;

    Eigen::MatrixXd Pf = state_manager.getCovariance();
    bool any_frozen = false;
    for (const auto& [cons, piv] : pivot_sat_) {
        // Freeze ionosphere
        const int iono_idx = state_manager.getIonosphereIndex(piv);
        if (iono_idx >= 0) {
            const auto idx = static_cast<Eigen::Index>(iono_idx);
            if (Pf(idx, idx) != 0.0) {
                Pf.row(idx).setZero();
                Pf.col(idx).setZero();
                any_frozen = true;
            }
        }

        // Freeze satellite biases
        for (const auto& [sb_key, sb_raw_idx] : state_manager.getSatBiasStates()) {
            if (sb_key.compare(0, piv.size() + 1, piv + "|") != 0) continue;
            const auto si = static_cast<Eigen::Index>(sb_raw_idx);
            if (si < Pf.rows() && Pf(si, si) != 0.0) {
                Pf.row(si).setZero();
                Pf.col(si).setZero();
                any_frozen = true;
            }
        }
    }
    if (any_frozen) state_manager.setCovariance(Pf);
}

// ===========================================================================
// Gap-recovery: forget all sticky pivots
// ===========================================================================

void PivotManager::dropAllPivots(StateManager& state_manager) {
    auto log = log_manager_.getLogger("pivot_manager");
    const auto n_pivots  = pivot_sat_.size();
    const auto n_forced  = forced_handover_requests_.size();

    pivot_sat_.clear();
    forced_handover_requests_.clear();
    forced_handover_exclude_.clear();
    forced_handover_reinflate_.clear();
    retired_pivots_this_epoch_.clear();
    clean_handovers_.clear();
    state_manager.setFrozenPivotSatellites({});

    if (log) {
        log->info("Dropped all BSSD pivots (was: {} cons, "
                  "{} forced-handover requests); next epoch will elect fresh pivots",
                  n_pivots, n_forced);
    }
}

} // namespace GnssProcessor
