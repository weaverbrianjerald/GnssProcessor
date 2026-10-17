/**
 * @file RealTimeEngineEstimate.cpp
 * @brief Estimation tail for RealTimeEngine::processEpoch.
 *
 * Measurement update, the two pseudo-observation constraints, ambiguity
 * resolution and the trace sweep.
 *
 * The constraints sit after the update and before AR on purpose: both
 * shrink the covariance the resolver reads (applyPseudoObservation updates
 * state AND covariance, which is what lets height recover � a value-only
 * clamp would not).
 */

#include "realtime/RealTimeEngine.hpp"
#include "app/EpochUpdateSupport.hpp"
#include "math/TimeUtils.hpp"
#include "products/SsrCorrectionStore.hpp"
#include "common/Constants.hpp"         // SPEED_OF_LIGHT
#include "common/PppUtils.hpp"          // carrier wavelengths
#include "filter/StateKeySchema.hpp"    // makeAmbiguityKey
#include <algorithm>
#include <cmath>
#include <iterator>
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

    // The pass budget and the forced-pivot-handover forwarding are shared with
    // the post-processing driver via EpochUpdateSupport, so the two paths
    // cannot diverge on them (a divergence once left real-time with no
    // handover at all).
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
        // The held ambiguity states must also survive the measurement
        // model's prune-on-first-miss for the duration of the outage;
        // deleting them would destroy the pre-gap state the bridge
        // repair restores.  Same source set as the exclusion above.
        meas_model_->setProtectedAmbiguityKeys(std::move(held));
    }
    size_t carry_bad = 0;
    // Pre-update innovation screen: tests every row's innovation against the predicted
    // state before the first update pass can spread a gross row over the states, and
    // builds that first pass itself (with the flagged rows held out).  False when the
    // screen is off, so the loop builds the pass as it always did.
    const bool first_pass_built = screenInnovations_(ctx, carry_excluded);
    for (int pass = 0; pass < max_passes; ++pass) {
        if (!(pass == 0 && first_pass_built))
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
        // bake a stale partition.  Shared with the post-processing driver, so
        // both paths re-inflate the same way.
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
            // Floor the tightened σ so std_scale < 1 cannot drive the supplied iono prior
            // below the per-LOS representativeness floor (an over-tight prior locks the slant
            // iono to an under-observed SH value).  Gated by the producer toggle, so
            // byte-identical when off (T3f).
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
        // Closed-loop network-bias validation: suppress them so AR runs pure
        // MLAMBDA on the KF float, which carries the consumed network phase bias;
        // the MW WL is in the separate .BIA frame and would re-inject a
        // conflicting WL datum (see setArIgnoreMwWl).
        const std::map<std::string, int>    no_wl_ambs;
        const std::map<std::string, double> no_wl_fracs;
        const auto wl_ambs  = ar_ignore_mw_wl_ ? no_wl_ambs
                                               : mw_slip_detector_->getResolvedWlAmbiguities();
        const auto wl_fracs = ar_ignore_mw_wl_ ? no_wl_fracs
                                               : mw_slip_detector_->getResolvedWlFractions();
        // Convert the detector's lock quality into the resolver's vocabulary so
        // the WL-only FFR gate can bound each arc's rounding failure rate.  The
        // map is empty when the MW feed is suppressed, and an empty map makes
        // the gate reject every WL constraint, which is correct: with no MW
        // integers there is nothing for the WL-only path to assert.
        std::map<std::string, WlArcQuality> wl_quality;
        if (!ar_ignore_mw_wl_) {
            for (const auto& [k, q] : mw_slip_detector_->getResolvedWlQuality())
                wl_quality[k] = WlArcQuality{ q.sigma_mean_cycles, q.count, q.exact };
        }
        auto ar_result = ambiguity_resolver_->resolve(*state_manager_, modeled_obs, wl_ambs, wl_fracs, wl_quality);
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

    // Periodic obs-code attribute-mismatch summary (RTA-13): one line every
    // 300 processed epochs; silent when no mismatch was ever counted.
    if (epoch_count_ % 300 == 0) {
        logBiasAttrMismatchSummary("periodic");
        logOrbitEchoMismatchSummary("periodic");   // same cadence and silence rule
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


// ---------------------------------------------------------------------------
// Consumer re-validation of float ambiguities against their own code level
//: the re-coupling edge of invariant #7 for those two carries.
// ---------------------------------------------------------------------------
//
// New code, not relocated from the pre-cut source.  The declared-step carry and the phase-bias translation both
// preserve an ambiguity across a phase-bias product step; neither checks whether
// the preserved value is still correct.  PBT sec.7 found that mass resets, by
// re-levelling accumulated ambiguity error from code, left the reset-only arm
// nearer the truth than the translated arm.  This seam performs that
// re-levelling deliberately, with both edges instrumented.
//
// The levels come from the consumer's own corrected observations, never from a
// re-read of the products.  ctx.meas_update.innovations holds the pre-fit
// innovation of every committed row, i.e. observed minus the predicted
// observable of MeasurementModel::buildJacobianRow, in which the satellite clock
// (obs.modeled plus the clock states), the satellite code bias and the satellite
// phase bias (ssr_bias_term) have all been applied.  Two classical combinations
// of the four innovations of a dual-frequency satellite are therefore levels of
// the ambiguity error (AmbLevelCheck.hpp derives both):
//
//   WL, cycles : [ (f1*ip1 - f2*ip2)/(f1-f2) - (f1*ic1 + f2*ic2)/(f1+f2) ]/lambda_WL
//   IF, metres : (f1^2*(ip1-ic1) - f2^2*(ip2-ic2))/(f1^2-f2^2)
//
// Both are geometry-, troposphere- and (first-order) ionosphere-free, so the
// receiver clock common to code and phase and the slant-iono states cancel.  A
// phase-only receiver clock does not cancel, and neither do code or phase
// correction errors, so a trip alone does not prove an ambiguity error.  Each is a
// paired per-epoch (level - state) difference, so the window's own sample sigma
// is the noise of the quantity tested.
//
// Window continuity (ALC-ARC): StateManager assigns a new generation on birth,
// reset/reseed request, translation and explicit recovery edits.  Both selected
// signals are compared with the last sampled pair before a sample is pushed.
// These tokens survive reseeding and the measurement update, unlike reset
// variance, which the update may already have contracted.  They also survive
// missing pairs and epochs without an update.  An explicit satellite
// notification clears the window immediately, so it cannot be lost at the next
// epoch boundary either.  A changed arc starts at sample 1 and needs window_ep
// fresh samples to be tested.
//
// The variance guard is retained as a conservative readiness check only.  It
// does not prove arc continuity or convergence; the generations establish
// continuity.
//
// Mode.  `act` refloats both of the satellite's ambiguities through the existing
// first-sighting/reset path (the same StateManager::resetAmbiguity call, at the
// same ambiguity_std, that the reset_disc path in RealTimeEngineSsrBreaks.cpp
// uses).  `observe` writes nothing; it only counts and emits the
// AMB_LEVEL rows.
//
// Placement.  After the update, the constraints and AR, and before the CST
// harvest.  The innovations it reads are the epoch's committed ones; the epoch's
// own position is not changed retroactively by the refloat (the value is
// unchanged, only the variance is re-inflated, and the re-seed lands at the next
// epoch's ensureRequiredStates); and the CST row for this epoch reports both the
// `refloat_level` event and the re-inflated amb_var.

void RealTimeEngine::noteAmbLevelDisturbed_(const std::string& sat) {
    if (!config_.ambLevelCheck() || sat.empty()) return;
    amb_level_check_.restart(sat);
}

void RealTimeEngine::checkAmbiguityLevel_(RtEpochContext& ctx) {
    if (!config_.ambLevelCheck()) return;

    // Resolve the three knobs once (the config cannot change mid-run).
    if (!amb_level_configured_) {
        AmbLevelCheck::Config c;
        c.window_ep = config_.getAmbLevelCheckWindowEp();
        c.k         = config_.getAmbLevelCheckK();
        c.mode      = (config_.getAmbLevelCheckMode() == "observe")
                    ? AmbLevelCheck::Mode::Observe : AmbLevelCheck::Mode::Act;
        amb_level_check_.configure(c);
        amb_level_configured_ = true;
    }

    // An epoch with no committed update has no innovations; every window keeps
    // its samples (a deferred epoch is not a disturbance, it is an absence).
    if (!ctx.update_applied) return;

    auto log = log_manager_.getLogger("rt_engine");
    const std::string& epoch_prefix = ctx.epoch_prefix;

    // Row-aligned (sat, signal) -> pre-fit innovation, the same harvest
    // traceConsumerSat_ makes (RealTimeEngineSsrDiag.cpp): BSSD / DD rows are
    // labelled "<sat_i>-<sat_p>" and are not a single-satellite quantity, so they
    // are skipped rather than attributed to one satellite.  A BSSD consumer
    // therefore contributes no rows at all and the check is inert there.
    std::map<std::string, double> innov_of;
    {
        const auto& mu = ctx.meas_update;
        const auto n_lbl = static_cast<Eigen::Index>(
            std::min(mu.satellite_ids.size(), mu.signal_types.size()));
        for (Eigen::Index i = 0; i < n_lbl; ++i) {
            const std::size_t u = static_cast<std::size_t>(i);
            const std::string& sat = mu.satellite_ids[u];
            if (sat.find('-') != std::string::npos) continue;
            if (i < mu.innovations.size())
                innov_of[sat + "|" + mu.signal_types[u]] = mu.innovations(i);
        }
    }
    if (innov_of.empty()) return;

    const PppUtils& utils = ppp_model_->pppUtils();
    const double reset_std = config_.getAmbiguityStd();
    const double reset_var = reset_std * reset_std;
    const bool   act = (amb_level_check_.config().mode == AmbLevelCheck::Mode::Act);

    for (const auto& obs : ctx.modeled_obs) {
        const std::string& sat = obs.satellite;
        if (sat.empty()) continue;

        // One raw phase signal per band, chosen deterministically: the
        // lexicographically smallest signal code of that band that carries both a
        // phase and a paired code innovation this epoch ("L1W" -> "C1W").  A
        // satellite tracked on L1C + L1W + L2W therefore contributes the L1C/L2W
        // pair, not an arbitrary one.  Bands beyond the two lowest band digits
        // present are ignored, since the combinations are two-frequency.
        std::map<char, std::string> band_sig;
        for (const auto& kv : obs.modeled) {
            const std::string& sig = kv.first;
            if (sig.size() < 2 || sig[0] != 'L') continue;
            if (sig.find('_') != std::string::npos) continue;
            const std::string code_sig = "C" + sig.substr(1);
            if (obs.modeled.find(code_sig) == obs.modeled.end()) continue;
            if (!innov_of.count(sat + "|" + sig) || !innov_of.count(sat + "|" + code_sig))
                continue;
            const char band = sig[1];
            const auto it = band_sig.find(band);
            if (it == band_sig.end() || sig < it->second) band_sig[band] = sig;
        }
        if (band_sig.size() < 2) continue;               // not tracked on two frequencies

        auto b1 = band_sig.begin();
        auto b2 = std::next(b1);
        const std::string& sig1 = b1->second;
        const std::string& sig2 = b2->second;

        const double lam1 = utils.getWavelength(obs.system, sig1, sat);
        const double lam2 = utils.getWavelength(obs.system, sig2, sat);
        if (!(lam1 > 0.0) || !(lam2 > 0.0)
            || !std::isfinite(lam1) || !std::isfinite(lam2)) continue;
        const double f1 = Constants::SPEED_OF_LIGHT / lam1;
        const double f2 = Constants::SPEED_OF_LIGHT / lam2;
        if (!(std::fabs(f1 - f2) > 1.0)) continue;        // same band => no wide lane
        const double lambda_wl = Constants::SPEED_OF_LIGHT / (f1 - f2);

        // --- Arc identity survives resets followed by covariance contraction. ---
        const std::string amb1 = StateKeys::makeAmbiguityKey(sat, sig1);
        const std::string amb2 = StateKeys::makeAmbiguityKey(sat, sig2);
        const int i1 = state_manager_->getAmbiguityIndex(amb1);
        const int i2 = state_manager_->getAmbiguityIndex(amb2);
        const auto generations = std::make_pair(
            state_manager_->getAmbiguityGeneration(amb1),
            state_manager_->getAmbiguityGeneration(amb2));
        const auto previous = amb_level_generations_.find(sat);
        bool restart = (previous == amb_level_generations_.end()
                        || previous->second != generations);
        if (i1 < 0 || i2 < 0) restart = true;
        else if (state_manager_->getStateVariance(i1) >= reset_var
                 || state_manager_->getStateVariance(i2) >= reset_var) restart = true;
        if (restart) {
            amb_level_check_.restart(sat);
            if (i1 < 0 || i2 < 0) continue;   // no state to compare against at all
        }

        amb_level_generations_[sat] = generations;

        const double ip1 = innov_of[sat + "|" + sig1];
        const double ip2 = innov_of[sat + "|" + sig2];
        const double ic1 = innov_of[sat + "|C" + sig1.substr(1)];
        const double ic2 = innov_of[sat + "|C" + sig2.substr(1)];

        const double wl_cyc = AmbLevelCheck::wlLevelMinusState(f1, f2, lambda_wl,
                                                               ip1, ip2, ic1, ic2);
        const double if_m   = AmbLevelCheck::ifLevelMinusState(f1, f2, ip1, ip2, ic1, ic2);

        const auto v = amb_level_check_.push(sat, wl_cyc, if_m);
        if (v.tested) ++amb_level_epoch_checks_;
        if (v.test == AmbLevelCheck::Test::None) continue;

        const char* which = AmbLevelCheck::testName(v.test);
        const std::string detail = fmt::format(
            "sig={}+{} k={:.2f} wl_mean={:.4f}cyc wl_sig={:.4f} if_mean={:.4f}m "
            "if_sig={:.4f} mode={}",
            sig1, sig2, amb_level_check_.config().k, v.wl_mean, v.wl_sigma,
            v.if_mean, v.if_sigma, act ? "act" : "observe");

        // Decoupling edge: the test fired.  Emitted in both modes.
        amb_level_events_.push_back(AmbLevelEvent{
            sat, "trip", which, v.diff, v.sigma, v.n, detail });
        log->info("{}Ambiguity LEVEL check ({}): {} {} + {} |state-level| {:.4f} > {:.2f} x "
                  "sigma {:.4f} (n {})", epoch_prefix, which, sat, sig1, sig2,
                  v.diff, amb_level_check_.config().k, v.sigma, v.n);

        if (!act) continue;                   // observe: counted, nothing written

        // Re-coupling edge: the response goes through the existing reset path,
        // StateManager::resetAmbiguity, with the same call and ambiguity_std as the
        // reset_disc path.  Both bands are refloated, because the tests
        // are two-frequency combinations and neither one identifies which band
        // carries the error.
        const bool r1 = state_manager_->resetAmbiguity(amb1, reset_std);
        const bool r2 = state_manager_->resetAmbiguity(amb2, reset_std);
        amb_level_check_.noteRefloat();
        noteAmbLevelDisturbed_(sat);
        amb_level_check_.restart(sat);
        noteConsumerSatEvent_(sat + "|" + sig1, "refloat_level");   // (no-op unarmed)
        noteConsumerSatEvent_(sat + "|" + sig2, "refloat_level");
        amb_level_events_.push_back(AmbLevelEvent{
            sat, "refloat", which, v.diff, v.sigma, v.n,
            fmt::format("{} reset_std={:.3f}m amb1={}({}) amb2={}({})",
                        detail, reset_std, amb1, r1 ? "ok" : "miss", amb2,
                        r2 ? "ok" : "miss") });
        log->info("{}Ambiguity LEVEL refloat: {} {} and {} re-floated at std {:.3f} m "
                  "({} test)", epoch_prefix, sat, amb1, amb2, reset_std, which);
    }
}

void RealTimeEngine::logAmbLevelCheckSummary_(const char* tag) {
    if (!config_.ambLevelCheck()) return;   // knob off: never printed (byte-identical)
    const auto& c = amb_level_check_.counters();
    log_manager_.getLogger("rt_engine")->info(
        "Consumer ambiguity LEVEL check (ALC, {}, mode '{}', window {} ep, k {:.2f}): "
        "sats {} / samples {} -> n_amb_level_checks {}; n_amb_level_trips_wl {} / "
        "n_amb_level_trips_if {}; n_amb_level_refloats {}; window restarts {}",
        tag, config_.getAmbLevelCheckMode(), config_.getAmbLevelCheckWindowEp(),
        config_.getAmbLevelCheckK(), c.sats, c.samples, c.checks, c.trips_wl,
        c.trips_if, c.refloats, c.restarts);
}

} // namespace GnssProcessor
