/**
 * @file RealTimeEngineEstimate.cpp
 * @brief Estimation tail for RealTimeEngine::processEpoch.
 *
 * Measurement update, the two pseudo-observation constraints, ambiguity
 * resolution and the trace sweep.
 *
 * The constraints sit AFTER the update and BEFORE AR on purpose: both
 * shrink the covariance the resolver reads (applyPseudoObservation updates
 * state AND covariance, which is what lets height recover � a value-only
 * clamp would not).
 */

#include "realtime/RealTimeEngine.hpp"
#include "app/EpochUpdateSupport.hpp"
#include "math/TimeUtils.hpp"
#include "products/SsrCorrectionStore.hpp"
#include <cmath>
#include <set>

namespace GnssProcessor {

/**
 * @brief KF measurement update with the outlier / pivot-handover rebuild loop.
 *
 * Relocated verbatim from the pre-cut RealTimeEngine::processEpoch
 * (frozen lines 1961-2045); see scripts/rt_seam_verify.py.
 */
void RealTimeEngine::runMeasurementUpdate(RtEpochContext& ctx) {
    const std::string& epoch_prefix = ctx.epoch_prefix;
    const std::vector<PppObservation>& modeled_obs = ctx.modeled_obs;
    EpochMeasurement& meas_update = ctx.meas_update;
    bool& update_applied = ctx.update_applied;

    // Pass budget + forced-pivot-handover forwarding are shared with the
    // post-processing driver via EpochUpdateSupport so the two paths can
    // never diverge on them again (the divergence that once left real-time
    // with no handover at all).
    const int max_passes =
        computeRebuildPassBudget(*state_manager_, config_, modeled_obs);

    std::set<std::string> carry_excluded;
    // Soft-LLI bridge: phase signals held across a receiver LLI gap pending
    // a bridge repair are excluded from the update so their still-slipped
    // ambiguities cannot contaminate shared states across the MW one-epoch
    // confirmation delay.  Empty unless turbo_edit_bridge_lli holds.
    if (soft_lli_) {
        auto held = soft_lli_->heldKeys();
        carry_excluded.insert(held.begin(), held.end());
        // The held ambiguity STATES must also survive the measurement
        // model's prune-on-first-miss for the duration of the outage —
        // deleting them would destroy the pre-gap state the bridge
        // repair restores.  Same source set as the exclusion above.
        meas_model_->setProtectedAmbiguityKeys(std::move(held));
    }
    size_t carry_bad = 0;
    for (int pass = 0; pass < max_passes; ++pass) {
        meas_update = meas_model_->computeMeasurementUpdate(
            modeled_obs, state_manager_->getStateVector(), carry_excluded);
        if (meas_update.innovations.size() == 0
            || !meas_model_->validateMeasurementUpdate(meas_update))
            break;

        meas_update.bad_residual_count = carry_bad;
        const auto result = kalman_filter_->update(meas_update);
        if (result != KalmanFilter::UpdateResult::NeedsMeasurementRebuild) {
            update_applied = true;   // clean commit (or post-reset commit)
            break;
        }

        // Rebuild requested: carry the running tally/exclusions, then forward
        // any forced pivot handovers (re-references the SD frame and
        // re-inflates contaminated non-pivot states on the next pass).
        carry_bad = meas_update.bad_residual_count;
        carry_excluded = meas_update.excluded_measurements;
        forwardKfPivotHandovers(
            *kalman_filter_, *meas_model_, mw_slip_detector_.get(),
            log_manager_, epoch_prefix, carry_excluded);

        // Re-inflate iono / sat-bias variances for any sat whose phase
        // ambiguities the KF fully reset this pass, so the reseed doesn't
        // bake a stale partition.  Shared with the post-processing driver;
        // previously absent on the real-time path (the known asymmetry).
        // No-op on a complete reset / when slip_state_reinflation is off.
        reinflateResetSatStates(*kalman_filter_, *state_manager_,
                                *meas_model_, config_);

        // Clear MW / TurboEdit arc history for everything the KF just
        // reset, mirroring Application::runFilterUpdateLoop.  Per the
        // MwSlipDetector contract (resetSatellite docs), an externally
        // reset ambiguity must drop its arc: a stale pre-reset MW mean
        // or GF level otherwise flags against the reseeded state on the
        // next epoch, and the TurboEdit joint solve can convert that
        // echo into a confident wrong integer repair on top of a state
        // the KF already corrected.
        if (kalman_filter_->wasCompleteReset()) {
            for (const auto& obs : modeled_obs) {
                mw_slip_detector_->resetSatellite(obs.satellite);
                if (turbo_edit_resolver_)
                    turbo_edit_resolver_->resetSatellite(obs.satellite);
            }
        } else {
            std::set<std::string> covered_sats;
            for (const auto& [sat_id, sig] :
                 kalman_filter_->getLastResetSignals()) {
                mw_slip_detector_->resetSatelliteSignal(sat_id, sig);
                if (turbo_edit_resolver_)
                    turbo_edit_resolver_->resetSatelliteSignal(sat_id, sig);
                covered_sats.insert(sat_id);
            }
            for (const auto& sat_id :
                 kalman_filter_->getLastResetSatellites()) {
                if (covered_sats.count(sat_id)) continue;
                mw_slip_detector_->resetSatellite(sat_id);
                if (turbo_edit_resolver_)
                    turbo_edit_resolver_->resetSatellite(sat_id);
            }
        }
    }
}

/**
 * @brief Soft a-priori ZWD constraint (one pseudo-observation row).
 *
 * Relocated verbatim from the pre-cut RealTimeEngine::processEpoch
 * (frozen lines 2063-2076); see scripts/rt_seam_verify.py.
 */
void RealTimeEngine::applyZwdConstraint(RtEpochContext& ctx) {
    const auto& zc = config_.getZwdConstraint();
    if (zc.enabled && zc.sigma_m > 0.0) {
        const Eigen::VectorXd x = state_manager_->getStateVector();
        const size_t tropo_idx = state_manager_->getTropoIndex();
        if (tropo_idx < static_cast<size_t>(x.size())) {
            Eigen::MatrixXd H = Eigen::MatrixXd::Zero(1, x.size());
            H(0, static_cast<Eigen::Index>(tropo_idx)) = 1.0;
            Eigen::VectorXd z(1);
            z(0) = zc.apriori_m - x(static_cast<Eigen::Index>(tropo_idx));
            Eigen::MatrixXd R(1, 1);
            R(0, 0) = zc.sigma_m * zc.sigma_m;
            kalman_filter_->applyPseudoObservation(H, z, R);
        }
    }
}

/**
 * @brief Network slant-ionosphere prior (ionosphere-weighted / BLUP).
 *
 * Relocated verbatim from the pre-cut RealTimeEngine::processEpoch
 * (frozen lines 2100-2163); see scripts/rt_seam_verify.py.
 */
void RealTimeEngine::applyNetworkIonoPrior(RtEpochContext& ctx) {
    const GnssTime& current_time = ctx.current_time;
    const std::vector<PppObservation>& modeled_obs = ctx.modeled_obs;
    const double dt = ctx.dt;

    if (consume_network_iono_ && kalman_filter_ && state_manager_) {
        const auto& iw = config_.getIonosphereWeightingConfig();
        const SsrCorrectionStore& store = ssrStore();
        const Eigen::VectorXd x = state_manager_->getStateVector();
        const std::set<std::string>& frozen_pivots =
            state_manager_->getFrozenPivotSatellites();
        // Same-epoch supply only: reject a value the producer did not refresh this
        // epoch (its pierce point / STEC is stale), rather than trust it at the
        // tight per-LOS σ.  The producer pushes received_time == the epoch time.
        const double iono_max_age = (dt > 0.0) ? 0.5 * dt : 30.0;

        std::vector<int>    idx;    // iono-state column index
        std::vector<double> innov;  // network slant delay - current iono state (m)
        std::vector<double> var;    // per-row pseudo-obs variance (m^2)
        std::vector<System> cons;   // constellation, for the common-mode block

        for (const auto& obs : modeled_obs) {
            if (obs.elevation < iw.elevation_cutoff_deg) continue;
            const int iono_idx = state_manager_->getIonosphereIndex(obs.satellite);
            if (iono_idx < 0 || iono_idx >= static_cast<int>(x.size())) continue;
            if (frozen_pivots.count(obs.satellite) ||
                state_manager_->getStateVariance(iono_idx) <= 0.0) {
                continue;
            }
            const auto si = store.getSlantIono(obs.satellite, current_time, iono_max_age);
            if (!si || !(si->sigma_m > 0.0)) continue;
            double sg = iw.std_scale * si->sigma_m;   // consumer-side tightening knob
            // T3f: floor the tightened σ so std_scale < 1 cannot drive the supplied iono prior
            // below the per-LOS representativeness floor (an over-tight prior locks the slant
            // iono to an under-observed SH value).  Gated by the producer toggle ⇒ byte-identical off.
            if (config_.getNetworkConfig().iono_supply_formal_sigma)
                sg = std::max(iw.floor_m, sg);
            if (!(sg > 0.0)) continue;
            idx.push_back(iono_idx);
            innov.push_back(si->iono_m - x(iono_idx));
            var.push_back(sg * sg);
            cons.push_back(obs.system);
        }

        const int m = static_cast<int>(idx.size());
        if (m > 0) {
            const int n = static_cast<int>(x.size());
            Eigen::MatrixXd H = Eigen::MatrixXd::Zero(m, n);
            Eigen::VectorXd z(m);
            for (int k = 0; k < m; ++k) {
                H(k, idx[k]) = 1.0;
                z(k) = innov[k];
            }
            const double common_var = iw.common_mode_sigma_m * iw.common_mode_sigma_m;
            Eigen::MatrixXd R = Eigen::MatrixXd::Zero(m, m);
            for (int r = 0; r < m; ++r) {
                for (int c = 0; c < m; ++c)
                    if (cons[r] == cons[c]) R(r, c) = common_var;
                R(r, r) += var[r];
            }
            kalman_filter_->applyPseudoObservation(H, z, R);

            double sum_abs = 0.0;
            for (double e : innov) sum_abs += std::abs(e);
            log_manager_.getLogger("ionosphere")->debug(
                "Network iono prior: {} sats constrained, mean|innov|={:.3f} m", m,
                sum_abs / static_cast<double>(m));
        }
    }
}

/**
 * @brief Integer ambiguity resolution (MLAMBDA).
 *
 * Relocated verbatim from the pre-cut RealTimeEngine::processEpoch
 * (frozen lines 2169-2193); see scripts/rt_seam_verify.py.
 */
void RealTimeEngine::resolveAmbiguities(RtEpochContext& ctx) {
    auto log = log_manager_.getLogger("rt_engine");
    const std::string& epoch_prefix = ctx.epoch_prefix;
    const std::vector<PppObservation>& modeled_obs = ctx.modeled_obs;
    bool& update_applied = ctx.update_applied;

    last_ar_ran_ = false;
    last_ar_result_ = AmbiguityResolution{};
    if (ambiguity_resolver_ && update_applied) {
        // Default: feed the MW-resolved WL integers (halves the LAMBDA search).
        // Closed-loop network-bias validation: suppress them so AR runs PURE
        // MLAMBDA on the KF float, which carries the consumed network phase bias
        // — the MW WL is in the separate .BIA frame and would re-inject a
        // conflicting WL datum (see setArIgnoreMwWl).
        const std::map<std::string, int>    no_wl_ambs;
        const std::map<std::string, double> no_wl_fracs;
        const auto wl_ambs  = ar_ignore_mw_wl_ ? no_wl_ambs
                                               : mw_slip_detector_->getResolvedWlAmbiguities();
        const auto wl_fracs = ar_ignore_mw_wl_ ? no_wl_fracs
                                               : mw_slip_detector_->getResolvedWlFractions();
        auto ar_result = ambiguity_resolver_->resolve(*state_manager_, modeled_obs, wl_ambs, wl_fracs);
        last_ar_ran_ = true;
        last_ar_result_ = ar_result;   // recorded even when not accepted (carries num_candidates)
        if (ar_result.success) {
            state_manager_->setStateVector(ar_result.fixed_state);
            state_manager_->setCovariance(ar_result.fixed_covariance);
            ambiguity_resolver_->holdAmbiguities(*state_manager_, ar_result);
            log->info("{}AR: fixed {}/{} ambs, ratio={:.2f}",
                      epoch_prefix, ar_result.num_fixed, ar_result.num_candidates, ar_result.ratio);
        }
    }
}

/**
 * @brief Posterior state extract, per-epoch summary line and the trace-logger sweep.
 *
 * Relocated verbatim from the pre-cut RealTimeEngine::processEpoch
 * (frozen lines 2196-2243); see scripts/rt_seam_verify.py.
 */
void RealTimeEngine::writeEpochTraces(RtEpochContext& ctx) {
    auto log = log_manager_.getLogger("rt_engine");
    const std::string& epoch_prefix = ctx.epoch_prefix;
    const GnssTime& current_time = ctx.current_time;
    const std::vector<RinexObservation>& epoch_obs = ctx.epoch_obs;
    const std::vector<PppObservation>& modeled_obs = ctx.modeled_obs;
    const std::map<std::string, DropReason>& drop_reasons = ctx.drop_reasons;
    EpochMeasurement& meas_update = ctx.meas_update;
    bool& update_applied = ctx.update_applied;

    state_manager_->updatePppStateFromVector(
        state_manager_->getStateVector(), ppp_state_, *state_manager_);

    ++epoch_count_;

    // RTA-13: periodic obs-code attribute-mismatch visibility (one line every
    // 300 processed epochs; silent when no mismatch was ever counted).
    if (epoch_count_ % 300 == 0) {
        logBiasAttrMismatchSummary("periodic");
        logOrbitEchoMismatchSummary("periodic");   // Card 7: same cadence, same silence rule
    }

    log->info("{}Epoch {}: pos=({:.3f},{:.3f},{:.3f}) clk={:.3f} zwd={:.4f} sats={}",
              epoch_prefix, epoch_count_,
              ppp_state_.position.x(), ppp_state_.position.y(), ppp_state_.position.z(),
              ppp_state_.clock_bias, ppp_state_.troposphere_zwd,
              modeled_obs.size());

    // --- Trace logging ---
    if (trace_logger_ && trace_logger_->isInitialized()) {
        // Trace files carry pure GPS time so timestamps line up exactly
        // with RINEX epoch records.  See Application::writeEpochTraces for
        // the rationale (no leap-second conversion in either direction).
        const GnssTime& gps_time = current_time;
        trace_logger_->logState(gps_time, *state_manager_);
        trace_logger_->logAmbiguities(gps_time, *state_manager_);
        trace_logger_->logIonosphere(gps_time, *state_manager_);
        trace_logger_->logSatBiases(gps_time, *state_manager_);
        trace_logger_->logReceiverCodeBiases(gps_time, *state_manager_);
        trace_logger_->logResiduals(gps_time, meas_update);

        // Satellite-status strip chart: real-time engine has no CSM-driven
        // MW slip set (slip detection lives in the post-processing
        // Application path), so the only slip signal here is the KF's
        // ambiguity-reset list from the latest update.  Raw epoch_obs is
        // forwarded so the logger can surface BelowMask rows for sats
        // dropped by ModelGeometry.  No-op for an unconfigured trace.
        const auto& reset_vec = kalman_filter_->getLastResetSatellites();
        const std::set<std::string> reset_sats(reset_vec.begin(), reset_vec.end());
        trace_logger_->logSatelliteStatus(
            gps_time, epoch_obs, modeled_obs, drop_reasons,
            meas_update, /*have_applied=*/update_applied,
            *state_manager_, /*slipped_sats=*/{}, reset_sats,
            config_.getElevationMaskDeg());
        trace_logger_->logDops(
            gps_time, modeled_obs, meas_update, /*have_applied=*/update_applied,
            *state_manager_);
    }
}

} // namespace GnssProcessor
