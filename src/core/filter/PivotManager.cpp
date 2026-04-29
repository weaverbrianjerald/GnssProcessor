/**
/**
 * @file PivotManager.cpp
 * @brief Between-satellite single-difference pivot selection and handover.
 */

#include "filter/PivotManager.hpp"
#include "slip/MwSlipDetector.hpp"
#include <cmath>

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
    std::map<std::string, SatInfo>& sat_info,
    std::map<std::string, ConstellationInfo>& cons_info) {

    sat_info.clear();
    cons_info.clear();

    for (const auto& obs : observations) {
        if (obs.system == System::GLONASS) continue;

        const std::string cons = PppUtils::getSystemString(obs.satellite);

        double rate = 0.0;
        auto prev_it = prev_elevation_.find(obs.satellite);
        if (prev_it != prev_elevation_.end())
            rate = obs.elevation - prev_it->second;

        // Count distinct phase signal types for this satellite.
        size_t n_phase = 0;
        for (const auto& [sig, val] : obs.observed)
            if (!sig.empty() && sig[0] == 'L')
                ++n_phase;

        sat_info[obs.satellite] = { cons, obs.elevation, rate, n_phase };

        auto& ci = cons_info[cons];
        if (n_phase > ci.max_phase_sigs)
            ci.max_phase_sigs = n_phase;
    }

    // Second pass: score candidates now that max_phase_sigs is known.
    for (auto& [cons_name, ci] : cons_info) {
        for (const auto& [sat, si] : sat_info) {
            if (si.constellation != cons_name) continue;

            const double score = scorePivotCandidate(sat, si, ci.max_phase_sigs);

            if (score > ci.best_score) {
                ci.best_sat   = sat;
                ci.best_score = score;
            }
            if (si.elevation > ci.highest_el) {
                ci.highest_sat = sat;
                ci.highest_el  = si.elevation;
            }
        }
    }

    // Update elevation history for next epoch.
    prev_elevation_.clear();
    for (const auto& [sat, si] : sat_info)
        prev_elevation_[sat] = si.elevation;
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
    const std::string& sat, const SatInfo& si, size_t max_phase_sigs) const {

    double score = 0.0;

    // Elevation component: normalized 0..1.
    score += si.elevation / 90.0;

    // Ascending preference: satellites going up will stay visible longer
    // and move toward lower-noise zenith geometry.
    if (si.elev_rate > 0.0 && si.elevation > 20.0)
        score += 0.3;

    // Low-elevation penalty: satellites below 20 deg are unreliable as
    // pivots due to troposphere/multipath and imminent setting risk.
    if (si.elevation < 20.0)
        score -= 0.5;

    // Signal coverage bonus: prefer satellites that track more phase
    // signals.  A pivot with fewer signals than the constellation's
    // maximum leaves orphaned SD ambiguities that cannot be properly
    // re-referenced, forcing a costly re-seed of those states.
    if (max_phase_sigs > 0 && si.phase_sig_count > 0) {
        const double coverage = static_cast<double>(si.phase_sig_count)
                              / static_cast<double>(max_phase_sigs);
        score += 0.6 * coverage;
    }

    // MW arc quality (when available).
    if (mw_detector_) {
        const auto mq = mw_detector_->getSatelliteMwQuality(sat);
        if (mq.num_pairs > 0) {
            // Mature arc: the slip detector has had enough epochs to
            // confirm no slip occurred � the arc is clean.
            if (mq.min_arc_length >= 5)
                score += 0.5;

            // Low MW noise: well-behaved code/phase tracking.
            if (mq.worst_sigma < 0.15)
                score += 0.3;

            // All WL integers locked: strongest quality indicator.
            if (mq.all_wl_locked)
                score += 0.4;
        }
    }

    return score;
}

// ===========================================================================
// Pivot candidate selection
// ===========================================================================

std::map<std::string, std::string> PivotManager::selectPivotCandidates(
    const std::map<std::string, ConstellationInfo>& cons_info,
    const std::map<std::string, SatInfo>& sat_info) const {

    const double pivot_min_el = config_.getBssdPivotMinElevation();
    std::map<std::string, std::string> candidates;

    for (const auto& [cons, ci] : cons_info) {
        auto prev_it = pivot_sat_.find(cons);
        if (prev_it != pivot_sat_.end()) {
            const std::string& old_pivot = prev_it->second;
            auto si = sat_info.find(old_pivot);
            if (si != sat_info.end() && si->second.elevation >= pivot_min_el) {
                candidates[cons] = old_pivot;
                continue;
            }
        }
        // Use the quality-scored best satellite; fall back to highest
        // elevation when no scores are available.
        candidates[cons] = ci.best_sat.empty() ? ci.highest_sat : ci.best_sat;
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
    const std::string& epoch_prefix) {

    auto log = log_manager_.getLogger("bssd_pivot_manager");

    log->warn("{}BSSD pivot {} disturbed in {}. "
              "Searching for healthy replacement.",
              epoch_prefix, pivot, cons);

    // Find the best-scored satellite in this constellation
    // (excluding the current disturbed pivot).
    size_t max_phase_sigs = 0;
    for (const auto& [sat, si] : sat_info)
        if (si.constellation == cons && si.phase_sig_count > max_phase_sigs)
            max_phase_sigs = si.phase_sig_count;

    std::string best_replacement;
    double best_score = -1e9;
    for (const auto& [sat, si] : sat_info) {
        if (si.constellation != cons || sat == pivot) continue;
        const double score = scorePivotCandidate(sat, si, max_phase_sigs);
        if (score > best_score) {
            best_replacement = sat;
            best_score       = score;
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

    // Row operations: T * P
    for (const Eigen::Index gi : group_indices) {
        if (gi == pivot_idx) continue;
        P.row(gi) -= P.row(pivot_idx);
    }
    // Column operations: (T * P) * T'
    for (const Eigen::Index gi : group_indices) {
        if (gi == pivot_idx) continue;
        P.col(gi) -= P.col(pivot_idx);
    }
    // Freeze the pivot row/col
    P.row(pivot_idx).setZero();
    P.col(pivot_idx).setZero();
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

void PivotManager::handlePivotHandover(
    const std::vector<PppObservation>& observations,
    StateManager& state_manager,
    const std::string& epoch_prefix) {

    if (!config_.isUncombinedEnabled()) return;

    auto log = log_manager_.getLogger("bssd_pivot_manager");

    // --- Build epoch snapshot (single pass over observations) ---
    std::map<std::string, SatInfo> sat_info;
    std::map<std::string, ConstellationInfo> cons_info;
    buildEpochMaps(observations, sat_info, cons_info);

    // --- Sticky pivot selection with elevation hysteresis ---
    auto new_pivot_sat = selectPivotCandidates(cons_info, sat_info);

    // First BSSD epoch: just record pivots, no handover needed.
    if (pivot_sat_.empty()) {
        pivot_sat_ = new_pivot_sat;
        for (const auto& [cons, sat] : pivot_sat_) {
            auto si_it = sat_info.find(sat);
            const double el = (si_it != sat_info.end()) ? si_it->second.elevation : -1.0;
            auto ci_it = cons_info.find(cons);
            const size_t mps = (ci_it != cons_info.end()) ? ci_it->second.max_phase_sigs : 0;
            const double sc = (si_it != sat_info.end())
                                  ? scorePivotCandidate(sat, si_it->second, mps) : 0.0;
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
        if (pivot == old_pivot &&
            isPivotDisturbed(old_pivot, phase_sigs, state_manager, x, P)) {
            if (!handleDisturbedPivot(cons, pivot, sat_info, phase_sigs,
                                      state_manager, x, P, epoch_prefix)) {
                // Re-zeroed in place, no handover.
                state_modified = true;
                cov_modified   = true;
                continue;
            }
            // pivot was updated to the replacement � fall through to handover.
        }

        if (pivot == old_pivot) continue;

        // --- Lossless handover ---
        const bool was_disturbed = isPivotDisturbed(old_pivot, phase_sigs,
                                                     state_manager, x, P);
        auto si_old = sat_info.find(old_pivot);
        auto si_new = sat_info.find(pivot);
        auto ci_it = cons_info.find(cons);
        const size_t mps = (ci_it != cons_info.end()) ? ci_it->second.max_phase_sigs : 0;
        const double new_score = (si_new != sat_info.end())
                                     ? scorePivotCandidate(pivot, si_new->second, mps) : 0.0;
        log->info("{}BSSD pivot handover for {}: {} -> {} "
                  "(old elev={:.1f}, new elev={:.1f}, score={:.2f}, threshold={:.1f}, disturbed={})",
                  epoch_prefix, cons, old_pivot, pivot,
                  (si_old != sat_info.end()) ? si_old->second.elevation : -1.0,
                  (si_new != sat_info.end()) ? si_new->second.elevation : -1.0,
                  new_score, pivot_min_el, was_disturbed ? "yes" : "no");

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
    if (!config_.isUncombinedEnabled()) return;

    // Inform StateManager which iono states are frozen so that
    // getProcessNoiseMatrix() can suppress their process noise.
    std::set<std::string> frozen_sats;
    for (const auto& [cons, piv] : pivot_sat_)
        frozen_sats.insert(piv);
    state_manager.setFrozenIonosphereSatellites(frozen_sats);

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

} // namespace GnssProcessor
