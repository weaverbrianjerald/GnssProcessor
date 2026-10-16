/**
 * @file MwSlipDetectorSample.cpp
 * @brief The MW detector's per-sample steps: processMwSample's twelve stages.
 *
 * processMwSample (MwSlipDetector.cpp) runs these in order over one
 * MwSlipDetector::MwSampleWork (slip/MwDetectorWork.hpp, which lists them); the sample
 * ends at the first stage that flags a slip.  Each stage opens with an alias block that
 * binds the names its code had in the former single function; card UOM step U5m-d cut
 * the bodies out unchanged apart from the declared edits (scripts/uom_u5m_verify.py
 * proves them against f823a36): the out-parameters are the verdict's fields, and the
 * values one stage hands to the next are the work value's members.
 */

#include "slip/MwSlipDetector.hpp"
#include "slip/MwDetectorWork.hpp"
#include "math/TimeUtils.hpp"
#include "common/PppUtils.hpp"
#include <algorithm>
#include <cmath>
#include <limits>
#include <string>

namespace GnssProcessor {

    using namespace mw;  // the constants table and the per-arc state (slip/MwArcState.hpp)

    // ===================================================================================
    // S1   stampArc_: the arc's stamps before any decision
    // ===================================================================================

    void MwSlipDetector::stampArc_(MwSampleWork& w) {
        const PppObservation& obs = w.obs;
        const std::string& stats_key = w.lane.stats_key;
        const std::string& sig1 = w.lane.sig1;
        const std::string& sig2 = w.lane.sig2;
        const std::string& code1 = w.lane.code1;
        const std::string& code2 = w.lane.code2;
        double& mw_m = w.sample.mw_m;
        const double lambda_wl = w.sample.lambda_wl;
        bool& bia_corrected = w.sample.bia_corrected;
        double& epoch_s = w.epoch_s;
        bool& lli_shadowed = w.lli_shadowed;

        // Apply stored in-house bias (if any) before the sample is
        // accumulated.  Modifies mw_m and bia_corrected in place; downstream
        // logic then treats the arc as if external BIA had been applied.
        if (config_.getInhouseBiasEstimation()) {
            applyEstimatedBias(mw_m, bia_corrected, stats_key);
        }

        w.stats = &mw_stats_[stats_key];
        mw::MwArcStats& stats = *w.stats;

        epoch_s = TimeUtils::continuousGpsSeconds(obs.time);

        // Detect data gaps: if the satellite was absent for longer than
        // the configured threshold, discard the stale arc and start fresh.
        // The threshold is overridable via setMaxGapSeconds; Application
        // tightens it to 1.5 × the sampling interval when IMU isn't
        // available and the data rate is coarser than 5 s.
        if (stats.count > 0 && stats.last_epoch_s > 0.0) {
            const double gap = epoch_s - stats.last_epoch_s;
            if (gap > max_gap_seconds_) {
                log_manager_.getLogger("mw_slip_detector")->debug(
                    "Data gap {:.3f}s > {:.3f}s for {}: resetting MW arc (n={})",
                    gap, max_gap_seconds_, stats_key, stats.count);
                stats = MwArcStats{};          // reset to default-initialized
            }
        }

        // Record the code channels the caller formed this MW from (after the gap
        // reset, so a fresh arc is stamped too).  Bookkeeping only: mw_m arrives
        // pre-computed, so this cannot change a detection.  Served by arcZdMean so a
        // consumer modelling the arc's satellite code bias names the OSB column the
        // observable carries instead of guessing it from the phase attribute.
        if (!code1.empty()) stats.code1 = code1;
        if (!code2.empty()) stats.code2 = code2;

        // Snapshot the pre-update arc state for potential rollback
        // by resetSatellite / resetSatelliteSignal.  Used when the KF
        // post-fit residual check identifies a slip that the MW test
        // missed: the reset functions restore this snapshot (preserving
        // accumulated noise statistics) instead of wiping the arc.  For
        // first-epoch arcs (count==0) and gap-reset arcs (just zeroed
        // above) the snapshot captures the empty MwArcStats, so a
        // rollback behaves like an erase.
        arc_snapshots_[stats_key] = stats;

        // Common-band scope (getEpochPairStatus, SPEC_event_harness §14): stamp the
        // arc as processed this epoch here, after the gap reset (so an arc the gap
        // just zeroed reports its fresh, immature count and cannot be read as
        // evidence of silence) and before every early return below, including the
        // two that erase the arc.  A sample that reaches the flag decision
        // overwrites this with the real threshold and verdict; one that leaves
        // earlier keeps the count, which is all a maturity gate needs.  Bookkeeping
        // only.  LLI shadow (§14.11): is this the first sample after an epoch the
        // receiver flagged on one of this pair's bands?  Computed once here and
        // carried into the second stamp below, so both records agree.
        lli_shadowed = [&] {
            const auto it = prev_lli_bands_.find(obs.satellite);
            return it != prev_lli_bands_.end()
                && (it->second.count(sig1) != 0 || it->second.count(sig2) != 0);
        }();
        {
            auto& st = epoch_pair_status_[stats_key];
            st.arc_n_at_sample = static_cast<int>(stats.count);
            st.flagged         = false;
            st.thresh_m        = std::numeric_limits<double>::quiet_NaN();
            st.lambda_wl       = lambda_wl;
            st.lli_shadow      = lli_shadowed;
        }
    }

    // ===================================================================================
    // S2   initArc_: the arc's first sample
    // ===================================================================================

    bool MwSlipDetector::initArc_(MwSampleWork& w) {
        const PppObservation& obs = w.obs;
        const std::string& stats_key = w.lane.stats_key;
        const std::string& sig1 = w.lane.sig1;
        const std::string& sig2 = w.lane.sig2;
        const double mw_m = w.sample.mw_m;
        const double lambda_wl = w.sample.lambda_wl;
        const bool bia_corrected = w.sample.bia_corrected;
        const double sat_mw_bias = w.sample.sat_mw_bias;
        const std::string& pivot_sat = w.frame.pivot_sat;
        mw::MwArcStats& stats = *w.stats;
        const double epoch_s = w.epoch_s;
        MwVerdict& verdict = w.verdict;

        constexpr double NaN = std::numeric_limits<double>::quiet_NaN();

        // First epoch: initialize arc.
        stats.mean         = mw_m;
        stats.M2           = 0.0;
        stats.count        = 1;
        stats.prev_mw      = mw_m;
        stats.last_epoch_s = epoch_s;
        stats.diff_window.clear();
        stats.bia_corrected = bia_corrected;
        stats.lambda_wl     = lambda_wl;
        stats.wl_locked     = false;
        stats.rounded_wl    = 0;
        stats.mw_window_head = 0;
        stats.mw_window_size = 0;
        pushLocalWindow(stats, mw_m);  // seed the local ring like the mean

        // WL integer locking waits for the statistical lock criterion
        // below (at least kWlLockMinN samples).  A single MW sample is
        // too noisy (especially for EWL pairs) for a reliable rounding
        // decision.

        // Sanity check: a BIA-corrected MW value should land near
        // integer × λ_WL on a clean phase track.  When the very first
        // sample of a fresh arc has a large fractional offset, the
        // arc was almost certainly initialized with a slip already
        // included (e.g., a slip occurred during the data gap that
        // triggered the reset, or this is the first observation of
        // the satellite after a long absence and the tracking loop
        // is still settling).  Declare a slip on init so the
        // ambiguity gets reset, instead of letting the corrupt value
        // become the new running mean; an unflagged corrupt baseline
        // lets even a 30-cycle slip pass through to the post-fit
        // residuals.
        if (bia_corrected && lambda_wl > 0.0) {
            const double init_cyc = mw_m / lambda_wl;
            const double frac = std::abs(init_cyc - std::round(init_cyc));

            // Effective threshold: config-driven init_frac_threshold_
            // floor, lifted by 3σ from the cross-sat MAD pool when
            // available (3σ = conventional 99.7% Gaussian gate).
            // Stochastic-σ lifts (zenith / predicted) are
            // intentionally excluded (see computeAdaptiveThreshold).
            double effective_thresh = init_frac_threshold_;
            {
                const std::string group_key = makeGroupKey(stats_key);
                if (!group_key.empty()) {
                    auto it = cross_sat_sigma_m_.find(group_key);
                    if (it != cross_sat_sigma_m_.end() && std::isfinite(it->second)) {
                        // The pool holds active-statistic deviations
                        // (TD-scaled in the TimeDifference modes), but
                        // the quantity gated here, the fractional
                        // offset of a single level sample from
                        // integer·λ, has level noise.  De-scale back
                        // to the level domain (÷√2 TD1, ÷√6 TD2; no-op
                        // in the level modes), or the 3σ lift inflates
                        // past the structural 0.5-frac ceiling and
                        // silently disables this check in the mode
                        // that relies on it most (TD erase-restarts).
                        const double sigma_xsat_level_m =
                            it->second / activeStatScale();
                        const double sigma_xsat_cyc = sigma_xsat_level_m / lambda_wl;
                        effective_thresh = std::max(effective_thresh, 3.0 * sigma_xsat_cyc);
                    }
                }
            }

            if (frac > effective_thresh) {
                log_manager_.getLogger("mw_slip_detector")->warn(
                    "MW init suspicious for {}: BIA-corrected MW={:.4f} m → "
                    "{:.3f} WL cyc has fractional |frac|={:.3f} > {:.2f}. "
                    "Forcing ambiguity reset (unknown integer slip during arc reset).",
                    stats_key, mw_m, init_cyc, frac, effective_thresh);

                // Trace record: write the actual quantities the
                // decision used.  dev = frac · λ is the suspicious
                // offset from integer × λ in meters; thresh =
                // effective_thresh · λ is the meter equivalent of
                // the cycle-domain gate.  Matches the column
                // semantics of regular slip rows so trace consumers
                // can compare init-slip rows to running-slip rows
                // on the same |dev| vs thresh basis.
                if (mwwl_trace_.is_open()) {
                    const double dev_init_m    = frac * lambda_wl;
                    const double thresh_init_m = effective_thresh * lambda_wl;
                    writeTraceRecord(obs, sig1, sig2, mw_m, stats.mean,
                                     dev_init_m, NaN, stats.count, lambda_wl,
                                     thresh_init_m, /*cross_sat_floor_m=*/NaN,
                                     /*is_slip=*/true, /*excessive=*/false,
                                     bia_corrected, sat_mw_bias,
                                     stats.rounded_wl, stats.wl_locked, pivot_sat);
                }

                verdict.slip_cycles = 0;     // unknown integer
                verdict.wl_known    = false;
                verdict.slip_frac   = frac;
                // Strength: the gate tested frac against effective_thresh, so
                // that ratio is this path's |dev|/threshold.  arc_epochs stays
                // 0: an init-suspicious flag fires on the arc's first sample
                // and has no maturity to report (which is why the evidence
                // rule's maturity floor refuses it).
                if (effective_thresh > 0.0)
                    verdict.evidence_ratio = frac / effective_thresh;
                return true;
            }
        }

        if (mwwl_trace_.is_open())
            writeTraceRecord(obs, sig1, sig2, mw_m, stats.mean, 0.0, NaN,
                             stats.count, lambda_wl, NaN, /*cross_sat_floor_m=*/NaN,
                             false, false,
                             bia_corrected, sat_mw_bias,
                             stats.rounded_wl, stats.wl_locked, pivot_sat);
        return false;
    }

    // ===================================================================================
    // S3   resolveTdConfirm_: this sample's TD1; a pending locked-arc confirmation
    // ===================================================================================

    bool MwSlipDetector::resolveTdConfirm_(MwSampleWork& w) {
        const PppObservation& obs = w.obs;
        const std::string& stats_key = w.lane.stats_key;
        const std::string& sig1 = w.lane.sig1;
        const std::string& sig2 = w.lane.sig2;
        const double mw_m = w.sample.mw_m;
        const double lambda_wl = w.sample.lambda_wl;
        const bool bia_corrected = w.sample.bia_corrected;
        const double sat_mw_bias = w.sample.sat_mw_bias;
        const std::string& pivot_sat = w.frame.pivot_sat;
        const int pivot_zd_slip_in = w.frame.pivot_zd_slip;
        const bool pivot_zd_known_in = w.frame.pivot_zd_known;
        mw::MwArcStats& stats = *w.stats;
        const double epoch_s = w.epoch_s;
        double& mw_diff = w.mw_diff;
        MwVerdict& verdict = w.verdict;
        constexpr double NaN = std::numeric_limits<double>::quiet_NaN();

        mw_diff = mw_m - stats.prev_mw;
        // ---- Locked-arc TD confirmation: resolution (TimeDifference) ----
        // One epoch ago the TD gate fired on this locked arc and set
        // td_confirm_pending instead of silently absorbing.  A step and a
        // one-epoch outlier are distinguishable now: after an outlier the
        // level returns to the pre-jump track, and after a real step it
        // stays on the post-jump track.  Compare the two level hypotheses
        // against the current sample and act.
        if (!stats.td_confirm_pending) return false;
        const double jump_m      = stats.td_confirm_jump_m;
        const double pre_mw      = stats.td_confirm_pre_mw;
        const double flag_dev    = stats.td_confirm_dev_m;
        const double flag_thresh = stats.td_confirm_thresh_m;
        const int    flag_mode   = stats.td_confirm_mode;
        stats.td_confirm_pending = false;  // resolved (or moot) either way

        // Resolution requires the same frame the hypotheses were formed
        // in: still TD mode, still locked, and the incoming sample in
        // the same bias frame (a bia_corrected flip would compare
        // levels across a frame jump).  Otherwise the pending flag is
        // dropped, and drift demotion remains the backstop for these rare
        // cases.
        if (detect_reference_ == MwDetectReference::TimeDifference
            && stats.wl_locked
            && bia_corrected == stats.bia_corrected) {
            // A pivot ZD slip at this epoch moves the SD frame between
            // hypothesis formation (flag epoch) and testing (now): both
            // residuals would be measured against old-frame levels and
            // the outcome would be meaningless.  The flag (a k·σ gate
            // exceedance) is evidence of a real event, so unclassifiable
            // evidence resolves to the safe action: unknown-integer slip.
            const bool frame_clean =
                (pivot_zd_slip_in == 0) && pivot_zd_known_in;

            const double r_outlier = std::abs(mw_m - pre_mw);
            const double r_step    = std::abs(mw_m - (pre_mw + jump_m));

            if (!frame_clean || r_step < r_outlier) {
                // Confirmed step (or unclassifiable under a moved
                // frame): demote and emit the slip now instead of
                // waiting about a dozen epochs for Hatch-mean migration
                // to trip drift demotion (during which the stale integer
                // would be re-exposed to AR).  The arc is erased (its
                // mean/M2/windows straddle the step), and the init-frac
                // check independently verifies the restart, as on the
                // unlocked-slip path.
                //
                // Always an unknown integer (downstream reset, never
                // repair): a deferred result is one epoch late, so a
                // published integer would (a) be SD→ZD re-referenced
                // with the wrong epoch's pivot slip, (b) pair in
                // TurboEdit with this epoch's GF value, the sign-flipped
                // echo of the real jump, and (c) conflict with the KF
                // post-fit / TurboEdit actions already taken at the
                // flag epoch.  Reset is the only safe deferred action
                // (reset-over-repair).
                log_manager_.getLogger("mw_slip_detector")->warn(
                    "MW locked-arc TD slip CONFIRMED for {} (epoch {}): "
                    "jump={:.3f} m ({:.2f} WL), {}. Demoting N_WL={} and "
                    "restarting arc; unknown integer -> reset.",
                    stats_key, stats.count, jump_m,
                    lambda_wl > 0.0 ? jump_m / lambda_wl : 0.0,
                    frame_clean
                        ? "level held post-jump (r_step < r_outlier)"
                        : "SD frame moved by a pivot slip this epoch "
                          "(hypotheses unclassifiable)",
                    stats.rounded_wl);

                // Flagged trace row (the flag epoch's row already
                // showed is_slip=1; this row is the action).  The gate
                // quantities are carried from the flag epoch rather
                // than recomputed, because recomputation does not
                // reproduce the firing gate for warmup-branch locks (the
                // cross-sat EWMA floor updates every epoch and
                // re-expands at the new elevation) or for TD1-fallback
                // flags inside TD2 order; so |active_dev|/threshold > 1
                // holds on this row by construction.
                if (mwwl_trace_.is_open()) {
                    double local_sigma_conf = NaN;
                    const double local_ref_conf =
                        computeLocalReference(stats, &local_sigma_conf);
                    writeTraceRecord(obs, sig1, sig2, mw_m, stats.mean,
                                     /*dev_from_mean=*/mw_m - stats.mean,
                                     computeMadSigma(stats), stats.count,
                                     lambda_wl, flag_thresh,
                                     crossSatSigmaFloor(stats_key, obs.elevation),
                                     /*is_slip=*/true, /*is_lli=*/false,
                                     stats.bia_corrected, sat_mw_bias,
                                     stats.rounded_wl, stats.wl_locked,
                                     pivot_sat, local_ref_conf,
                                     local_sigma_conf,
                                     flag_mode,
                                     /*active_dev_m=*/flag_dev);
                }

                verdict.slip_cycles = 0;
                verdict.wl_known    = false;
                verdict.slip_frac   = 0.5;
                // Strength: the gate quantities are the ones carried from the
                // flag epoch (see the trace comment above; recomputing them
                // here does not reproduce the firing gate), so the ratio is
                // |flag_dev| / flag_thresh, which is > 1 by construction.
                if (flag_thresh > 0.0)
                    verdict.evidence_ratio =
                        std::abs(flag_dev) / flag_thresh;
                verdict.arc_epochs = static_cast<int>(stats.count);
                // Float step estimate (cycles): the integer stays
                // withheld by the deferred-path contract above, but the
                // confirmed level gap's magnitude is sound, so it is
                // exported to select candidates (TurboEdit gap-bridge solve).
                // Withheld when the SD frame moved this epoch (a pivot
                // slip makes the gap's frame ambiguous).
                if (frame_clean && lambda_wl > 0.0)
                    verdict.slip_wl_float = jump_m / lambda_wl;
                // Stash the pre-jump level for the settling across-gap
                // WL estimate (getAcrossGapWlEvidence): the float above
                // is a single-sample jump biased by the re-acquisition
                // NL-code transient; (post-restart Hatch mean − this
                // level) re-estimates the step with √n-averaged noise
                // as the new arc accumulates.  Same SD frame as the
                // live arcs (re-anchors shift it alongside); frame
                // comparability is re-checked at query time.
                if (frame_clean && lambda_wl > 0.0)
                    demoted_levels_[stats_key] = DemotedArcLevel{
                        pre_mw, lambda_wl, epoch_s, stats.bia_corrected };
                mw_stats_.erase(stats_key);
                return true;
            }

            // Outlier: the level came back to the pre-jump track.  Keep
            // the lock and excise the outlier epoch from the TD chain:
            // this epoch's TD re-bases on the pre-outlier level (the
            // jump with the outlier removed; otherwise the gate would
            // fire again on the −jump echo), and prev_diff is
            // invalidated so TD2 takes its one-epoch TD1 fallback
            // instead of differencing against the outlier-sized TD1.
            log_manager_.getLogger("mw_slip_detector")->debug(
                "MW locked-arc TD flag for {} resolved as one-epoch "
                "outlier (r_outlier={:.3f} m < r_step={:.3f} m); lock "
                "kept, outlier epoch excised from the TD chain.",
                stats_key, r_outlier, r_step);
            mw_diff = mw_m - pre_mw;
            stats.prev_diff = NaN;
        }
        return false;
    }

    // ===================================================================================
    // S4   tryLock_: the statistical WL lock
    // ===================================================================================

    void MwSlipDetector::tryLock_(MwSampleWork& w) const {
        const std::string& stats_key = w.lane.stats_key;
        const double lambda_wl = w.sample.lambda_wl;
        mw::MwArcStats& stats = *w.stats;

        // Statistical WL lock criterion.  The Hatch mean is a Gaussian
        // estimator of integer × λ_WL plus per-sample noise; its standard
        // error decreases as σ_per_sample / √n.  Lock when the worst-case
        // (kσ-bounded) fractional residual is comfortably inside the
        // rounding basin:
        //
        //     |frac| + k_σ · σ_mean / λ_WL  <  0.5
        //
        // with σ_mean derived from the Welford accumulator
        //
        //     σ²_mean = M2 / (n · (n − 1))
        //
        // and k_σ = 3 (the ±3σ interval stays inside the rounding basin, so
        // round(mean/λ_WL) lands on the true integer with 99.7% confidence).
        // The gate adapts to the arc instead of using a fixed count and
        // frac heuristic:
        //
        //   - Clean BIA-corrected arcs (sub-cm phase, modest code noise)
        //     have shrunk σ_mean enough by n=kWlLockMinN to pass the kσ
        //     test on the next eligible epoch.
        //   - Noisy arcs (low elevation, high multipath) wait until √n
        //     has averaged the noise down enough; locks happen when
        //     warranted, not on a fixed clock.
        //
        // Hard floor of n ≥ kWlLockMinN: the empirical Welford σ from a
        // handful of samples severely underestimates the long-term noise
        // envelope on a low-elevation arc.  The first few epochs are
        // routinely tight by chance, and a kσ test on that tight σ passes
        // a premature lock that later snaps to the wrong integer once the
        // true noise envelope emerges.  A floor of 10 lets the empirical
        // variance stabilize before the lock test trusts it, without
        // significantly delaying clean arcs (whose σ_mean shrinks
        // monotonically with n, so they pass at n=10 as cleanly as they
        // would at n=3).
        //
        // Hard cap on raw |frac|: the statistical test alone trusts a
        // sufficiently tight σ_mean to round 3.33 → 3 with 99.7%
        // confidence, but if a sat has a real per-(sat, signal) bias
        // not captured by BIA, the running mean settles to a non-integer
        // value with small standard error.  The math is internally
        // consistent (mean is well-determined to be 3.33) but the
        // rounded integer can be off by a cycle if the true integer is
        // 4 with bias −0.67 instead of 3 with bias +0.33.  Refuse to
        // lock when |frac| exceeds @c kWlMaxFrac regardless of
        // statistical confidence; treat large |frac| as a real bias
        // signal that disqualifies the lock.
        //
        // Note: on lock the running mean is not snapped to integer × λ.
        // Keeping it live lets Welford continue to track the arc, which
        // is the basis of the drift-demotion check applied below (after
        // the slip-detection branch).  If the empirical mean drifts past
        // kWlLockDriftMaxCyc cycles from the locked integer, the lock is
        // demoted immediately.  That safety net catches both locks that
        // were wrong from the start and accumulated slip sequences that
        // the per-epoch slip detector absorbed as outliers.
        constexpr double kWlLockKSigma  = 3.0;
        constexpr size_t kWlLockMinN    = 10;
        if (stats.bia_corrected && !stats.wl_locked && lambda_wl > 0.0
            && stats.count >= kWlLockMinN) {
            const double mean_cyc = stats.mean / lambda_wl;
            const double frac     = mean_cyc - std::round(mean_cyc);
            const double var_per_sample = stats.M2 / static_cast<double>(stats.count - 1);
            const double sigma_mean_m   = std::sqrt(var_per_sample / static_cast<double>(stats.count));
            const double sigma_mean_cyc = sigma_mean_m / lambda_wl;

            const double worst_case_frac = std::abs(frac) + kWlLockKSigma * sigma_mean_cyc;
            if (worst_case_frac < 0.5 && std::abs(frac) < kWlMaxFrac) {
                stats.rounded_wl = static_cast<int>(std::round(mean_cyc));
                stats.wl_locked  = true;
                log_manager_.getLogger("mw_slip_detector")->info(
                    "WL locked for {} (epoch {}): N_WL={}, mean_cyc={:.3f}, "
                    "frac={:+.3f}, σ_mean={:.3f} cyc, worst-case={:.3f} cyc",
                    stats_key, stats.count, stats.rounded_wl, mean_cyc,
                    frac, sigma_mean_cyc, worst_case_frac);
            }
        }
    }

    // ===================================================================================
    // S5   tryInhouseCommit_: the in-house bias commit
    // ===================================================================================

    void MwSlipDetector::tryInhouseCommit_(MwSampleWork& w) {
        const std::string& stats_key = w.lane.stats_key;
        double& mw_m = w.sample.mw_m;
        const double lambda_wl = w.sample.lambda_wl;
        bool& bia_corrected = w.sample.bia_corrected;
        mw::MwArcStats& stats = *w.stats;
        const double epoch_s = w.epoch_s;

        // In-house bias commit path: runs when the toggle is enabled, no
        // external BIA was applied, and the running mean has converged
        // statistically close enough to an integer for a confident commit.
        // Promotes the arc to bia_corrected + wl_locked in one step (the
        // function stores the residual as the bias and subtracts it from
        // the mean, which centers the mean near integer × λ).  After
        // commit, apply the just-stored bias to the current sample so the
        // dev / slip-check / trace below operate in the bias-corrected
        // frame.  Without this rebalancing, dev would equal the (large)
        // bias on the commit epoch, the slip detector would false-fire,
        // and the trace would show a non-integer mw_m next to an
        // integer-near mw_mean_m, inconsistent with wl_locked=1.
        if (config_.getInhouseBiasEstimation()
            && !stats.bia_corrected
            && !stats.wl_locked) {
            if (tryCommitOrUpdateBias(stats, stats_key, lambda_wl, epoch_s)) {
                auto it = sat_pair_bias_.find(stats_key);
                if (it != sat_pair_bias_.end()) {
                    mw_m -= it->second.bias_m;
                }
                bia_corrected = true;
            }
        }
    }

    // ===================================================================================
    // S6   selectStatistic_: the deviation and spread the gate tests
    // ===================================================================================

    void MwSlipDetector::selectStatistic_(MwSampleWork& w) const {
        const double mw_m = w.sample.mw_m;
        mw::MwArcStats& stats = *w.stats;
        const double mw_diff = w.mw_diff;
        constexpr double NaN = std::numeric_limits<double>::quiet_NaN();
        double& dev_hatch = w.dev_hatch;
        double& local_sigma_scaled = w.local_sigma_scaled;
        double& local_ref = w.local_ref;
        double& td2 = w.td2;
        bool& use_local = w.use_local;
        bool& use_td = w.use_td;
        double& dev = w.dev;
        double& mode_sigma = w.mode_sigma;
        int& detect_mode_code = w.detect_mode_code;

        // Statistic selection.  dev_hatch (vs the slow recursive mean) is
        // always computed: it backs the legacy reference mode, the
        // epoch-clean affirmation below, and the trace record.  (The
        // unlocked-arc level-disagreement backstop is level-vs-level: it
        // compares the Hatch mean itself against the local median and
        // never reads dev_hatch.)  The local reference (median of the
        // per-arc raw-MW ring, samples through k−1 only) is likewise
        // always computed; it is cheap, and tracing it in every mode gives
        // a free offline A/B.
        //
        // The active statistic (the deviation and mature-spread pair the
        // gate tests) is then picked per detect_reference_.  Deviation and
        // spread are the same quantity by construction in every mode except
        // the legacy HatchMean pairing:
        //  - HatchMean:      dev = mw − hatch_mean,    σ = MAD(TD1).
        //    (Legacy pairing; mismatched under trends; kept for A/B.)
        //  - LocalMedian:    dev = mw − ring median,   σ = √2·MAD(ring).
        //    Trend-immune to first order (lag ~N/2, MAD grows with trend),
        //    but a soft quadratic still walks the level off the median.
        //  - TimeDifference: dev = TD1 (or TD2),       σ = MAD(TD1/TD2).
        //    Memoryless, so immune to trend and curvature; pays √2 (√6)
        //    noise amplification.  TD2's first computable epoch per arc is
        //    the third sample; until then the TD1 pair substitutes (gated
        //    by warmup floors regardless at that age).
        dev_hatch = mw_m - stats.mean;
        local_sigma_scaled = NaN;
        local_ref = computeLocalReference(stats, &local_sigma_scaled);
        td2 = std::isfinite(stats.prev_diff)
                               ? (mw_diff - stats.prev_diff) : NaN;

        use_local = detect_reference_ == MwDetectReference::LocalMedian
                               && std::isfinite(local_ref);
        use_td    = detect_reference_ == MwDetectReference::TimeDifference;

        if (use_local) {
            dev              = mw_m - local_ref;
            mode_sigma       = local_sigma_scaled;
            detect_mode_code = 1;
        } else if (use_td && td_order_ == MwTdOrder::Second && std::isfinite(td2)) {
            dev              = td2;
            mode_sigma       = computeTd2MadSigma(stats);
            detect_mode_code = 3;
        } else if (use_td) {
            dev              = mw_diff;
            mode_sigma       = computeMadSigma(stats);
            detect_mode_code = 2;
        } else if (detect_reference_ == MwDetectReference::HatchMean) {
            // Legacy pairing: the TD1 MAD governs the mature branch exactly
            // as it always has (bit-identical to the pre-refactor gate).
            dev              = dev_hatch;
            mode_sigma       = computeMadSigma(stats);
            detect_mode_code = 0;
        } else {
            // LocalMedian with an empty ring: unreachable after arc init
            // (the ring is seeded with the first sample); conservative
            // fallback to the legacy deviation under warmup floors.
            dev              = dev_hatch;
            mode_sigma       = NaN;
            detect_mode_code = 0;
        }
    }

    // ===================================================================================
    // S7   gate_: the threshold, the verdicts, the decision line
    // ===================================================================================

    void MwSlipDetector::gate_(MwSampleWork& w) {
        const PppObservation& obs = w.obs;
        const std::string& stats_key = w.lane.stats_key;
        const std::string& sig1 = w.lane.sig1;
        const std::string& sig2 = w.lane.sig2;
        const double lambda_wl = w.sample.lambda_wl;
        mw::MwArcStats& stats = *w.stats;
        const double epoch_s = w.epoch_s;
        const bool lli_shadowed = w.lli_shadowed;
        const double dev_hatch = w.dev_hatch;
        const double dev = w.dev;
        const double mode_sigma = w.mode_sigma;
        double& thresh = w.thresh;
        double& sigma = w.sigma;
        bool& is_slip = w.is_slip;
        bool& is_slip_level = w.is_slip_level;

        thresh  = computeAdaptiveThreshold(stats, stats_key,
                                                        obs.elevation, mode_sigma);
        sigma   = computeMadSigma(stats);
        is_slip   = std::isfinite(thresh) && std::abs(dev) > thresh;

        // Complete this sample's detection record (SPEC_event_harness §14) now
        // that the adaptive threshold and the flag verdict exist.  stats.count is
        // still the count before this sample (the Welford increment is further
        // down), the same maturity the cleanliness set tests, so the two records
        // agree on what "n" means.
        {
            auto& st = epoch_pair_status_[stats_key];
            st.arc_n_at_sample = static_cast<int>(stats.count);
            st.flagged         = is_slip;
            st.thresh_m        = thresh;
            st.lambda_wl       = lambda_wl;
            st.lli_shadow      = lli_shadowed;
        }

        // Decision-line readout (SPEC_event_harness §14.10): the same four numbers
        // as a log line.  The per-sample record above is only readable through a
        // verdict, and a verdict exists only on epochs where some pair fired, so
        // without this line the epochs that matter most (the silent lane on either
        // side of a slip) cannot be read.  Debug level: silent at the shipped
        // `warn` level, and a log line cannot change a decision, so an armed run
        // stays byte-identical on every product.  `n` is the arc count before this
        // sample, so a small n on a settled arc means the arc was just reset (gap
        // or LLI); that distinguishes "evaluated against the arc" from "arc
        // restarted and this sample re-based".
        log_manager_.getLogger("mw_slip_detector")->debug(
            "MWDEC sta={} {} t={:.1f} dev={:+.4f} m ({:+.3f} WL) thr={:.4f} m ({:.3f} WL) "
            "n={} lam_wl={:.4f} flag={} lli={} lli_shadow={}",
            station_label_,
            stats_key, epoch_s, dev, (lambda_wl > 0.0 ? dev / lambda_wl : 0.0),
            thresh, (lambda_wl > 0.0 ? thresh / lambda_wl : 0.0),
            stats.count, lambda_wl, is_slip ? 1 : 0,
            (obs.lli_flags.count(sig1) ? obs.lli_flags.at(sig1) : 0)
                | (obs.lli_flags.count(sig2) ? obs.lli_flags.at(sig2) : 0),
            lli_shadowed ? 1 : 0);

        // Level-frame slip indicator with the legacy pairing (Hatch dev vs
        // the TD1-MAD gate), used only to protect the Welford M2
        // admission below.  M2 integrates the Hatch-frame delta, so its
        // "no slip energy" rule must be judged in the Hatch frame: in the
        // memoryless TD modes a locked arc's post-slip samples pass the TD
        // gate as clean (TD ≈ 0) while still sitting dN·λ off the
        // not-yet-migrated mean, and admitting them would add (dN·λ)² per
        // epoch to the per-sample variance estimator until drift demotion
        // fires.  In HatchMean mode this is identical to is_slip
        // (bit-identity preserved); in TD1 mode thresh_level is recomputed
        // from the same TD1 spread and equals the gate above.
        const double thresh_level =
            (detect_reference_ == MwDetectReference::HatchMean)
                ? thresh
                : computeAdaptiveThreshold(stats, stats_key, obs.elevation, sigma);
        is_slip_level =
            std::isfinite(thresh_level) && std::abs(dev_hatch) > thresh_level;
    }

    // ===================================================================================
    // S8   recordEpochEvidence_: score cache, clean set, pool, trace row
    // ===================================================================================

    void MwSlipDetector::recordEpochEvidence_(MwSampleWork& w) {
        const PppObservation& obs = w.obs;
        const std::string& stats_key = w.lane.stats_key;
        const std::string& sig1 = w.lane.sig1;
        const std::string& sig2 = w.lane.sig2;
        const double mw_m = w.sample.mw_m;
        const double lambda_wl = w.sample.lambda_wl;
        const double sat_mw_bias = w.sample.sat_mw_bias;
        const std::string& pivot_sat = w.frame.pivot_sat;
        mw::MwArcStats& stats = *w.stats;
        constexpr double NaN = std::numeric_limits<double>::quiet_NaN();
        const double dev_hatch = w.dev_hatch;
        const double local_sigma_scaled = w.local_sigma_scaled;
        const double local_ref = w.local_ref;
        const double dev = w.dev;
        const double mode_sigma = w.mode_sigma;
        const int detect_mode_code = w.detect_mode_code;
        const double thresh = w.thresh;
        const double sigma = w.sigma;
        const bool is_slip = w.is_slip;

        // Cache for external queries (e.g. outlier tiebreaker).  Active-
        // reference quantities so the |dev|/σ score keeps its meaning in
        // every mode; while the active statistic's window is still immature
        // the gate σ is floor-driven, so fall back to the TD1 σ for the
        // score.  That is scale-consistent in every branch except TD2 (last_dev
        // would be √6-scaled against a √2-scaled σ, inflating the score
        // ~√3 during the one-epoch td2_window maturity lag); there, leave
        // the σ NaN so getWorstMwScore's isfinite guard skips the arc for
        // that epoch instead of returning a mis-scaled score.
        stats.last_dev   = dev;
        stats.last_sigma = std::isfinite(mode_sigma)
                               ? mode_sigma
                               : (detect_mode_code == 3 ? NaN : sigma);

        // Affirmative per-epoch cleanliness for the TurboEdit GF-only gate
        // (getEpochCleanPairs): the arc did not flag and the sample sits
        // within a conservative fraction of λ_WL of the arc mean, so the
        // wide-lane integer is demonstrably unchanged this epoch.
        // Deliberately not keyed on wl_locked: an unlocked mature arc with
        // a small dev is equally valid evidence of ΔN_WL = 0.  A locked
        // arc absorbing a real slip (is_slip true, no result emitted) is
        // the state this set must exclude.
        //
        // Affirmation must have discriminating power, so two extra
        // conditions apply beyond the 0.3-cycle bound:
        //  - real maturity: count ≥ kEpochCleanMinArcN.  A finite adaptive
        //    threshold is not a maturity proxy (the warm-up and cold-start
        //    branches return finite values for 2-sample arcs).
        //  - σ-awareness: a true ΔN_WL = ±1 slip (|dev| ≈ λ_WL) must sit
        //    ≥ 3σ outside the clean bound, i.e. 3σ ≤ (1 − 0.3)·λ_WL.
        //    Otherwise noise can pull a real slip inside the bound, and the
        //    false affirmation feeds TurboEdit a wrong ΔN_WL = 0 split
        //    (which the frac gate accepts on L1/L5-class pairs, where
        //    λ1/(λ1−λ2) ≈ −2.95 lands near-integer).  With the 3σ
        //    condition the false-affirm probability is ≤ Φ(−3) ≈ 0.13 %.
        // The 0.3-cycle bound itself is a cleanliness gate on a
        // known-integer hypothesis, not a detection threshold, so it stays
        // fixed instead of scaling with the empirical σ tiers; the σ
        // condition only decides whether the test is usable at all
        // (noisy low-elevation arcs never affirm, and GF-only flags there
        // fall back to reset).
        // The deviation bound deliberately uses the Hatch deviation in every
        // reference mode.  The local median absorbs a missed step within
        // ~N/2 epochs, after which the local deviation of a genuinely
        // slipped arc reads near zero; affirming cleanliness on the local
        // dev would hand TurboEdit a confident false ΔN_WL = 0 for its
        // GF-only split.  The ~100-epoch Hatch memory keeps an absorbed
        // step visible here long after the local window has lost it;
        // combined with !is_slip (the active-reference test) the gate is
        // conjunctive across both references in LocalMedian mode.
        constexpr double kEpochCleanMaxDevWlCyc = 0.3;
        constexpr std::size_t kEpochCleanMinArcN = 10;
        if (!is_slip && lambda_wl > 0.0
            && stats.count >= kEpochCleanMinArcN
            && std::isfinite(sigma)
            && 3.0 * sigma <= (1.0 - kEpochCleanMaxDevWlCyc) * lambda_wl
            && std::abs(dev_hatch) <= kEpochCleanMaxDevWlCyc * lambda_wl) {
            epoch_clean_pairs_.insert(stats_key);
        }

        // Contribute this dev to the cross-satellite pool for this
        // (constellation × band-pair) group, but only when this sample
        // was not itself flagged as a slip.  The pool is finalized at
        // the end of detectLargeMwResets into cross_sat_sigma_m_, which
        // feeds the next epoch's warmup floor.  Skipping the push on
        // is_slip keeps a cycle-jump dev (multi-cycle × λ) out of the
        // constellation noise estimator.  MAD is statistically robust
        // to a single outlier in 5+, but the slip-dev is not noise: it
        // is a discrete event, wrong to include in a noise model, and
        // it compounds when slips happen on multiple sats in close
        // succession.  The group's MW pivot arc is processed
        // undifferenced and contributes its own deviation like any
        // other arc.
        const std::string group_key = makeGroupKey(stats_key);
        if (!group_key.empty() && !is_slip) {
            // Zenith-normalize before pooling so the cross-sat MAD is a clean
            // homoscedastic estimate rather than a blend of low-el (noisy) and
            // high-el (quiet) arcs.  computeAdaptiveThreshold re-expands it
            // per-arc via mwElevationWeight.
            //
            // The pooled quantity is the active deviation: the warmup floor
            // this pool feeds gates active-statistic deviations, so pooling
            // Hatch deviations in the other modes would mix statistics.  A
            // post-absorbed step or trend-lagged Hatch dev (multi-decimeter)
            // would inflate the EWMA floor for the whole constellation and
            // mask fresh-arc slips.  (In the TimeDifference modes the pool
            // therefore holds TD deviations, already on the differenced
            // noise scale the TD gate needs.)
            cross_sat_devs_[group_key].push_back(dev / mwElevationWeight(obs.elevation));
        }

        if (mwwl_trace_.is_open())
            writeTraceRecord(obs, sig1, sig2, mw_m, stats.mean,
                             /*dev_from_mean=*/dev_hatch, sigma,
                             stats.count, lambda_wl, thresh,
                             crossSatSigmaFloor(stats_key, obs.elevation),
                             is_slip, false,
                             stats.bia_corrected, sat_mw_bias,
                             stats.rounded_wl, stats.wl_locked, pivot_sat,
                             local_ref, local_sigma_scaled,
                             detect_mode_code, /*active_dev_m=*/dev);
    }

    // ===================================================================================
    // S9   onFlag_: a flagged sample
    // ===================================================================================

    bool MwSlipDetector::onFlag_(MwSampleWork& w) {
        const PppObservation& obs = w.obs;
        const std::string& stats_key = w.lane.stats_key;
        const double mw_m = w.sample.mw_m;
        const double lambda_wl = w.sample.lambda_wl;
        const bool bia_corrected = w.sample.bia_corrected;
        const int pivot_zd_slip_in = w.frame.pivot_zd_slip;
        const bool pivot_zd_known_in = w.frame.pivot_zd_known;
        mw::MwArcStats& stats = *w.stats;
        const double mw_diff = w.mw_diff;
        const double local_ref = w.local_ref;
        const bool use_local = w.use_local;
        const bool use_td = w.use_td;
        const double dev = w.dev;
        const double mode_sigma = w.mode_sigma;
        const int detect_mode_code = w.detect_mode_code;
        const double thresh = w.thresh;
        const double sigma = w.sigma;
        const bool is_slip = w.is_slip;
        MwVerdict& verdict = w.verdict;

        if (!is_slip) return false;
        auto logger = log_manager_.getLogger("mw_slip_detector");

        if (stats.wl_locked) {
            // Locked arc: a single-sample threshold exceedance is
            // treated as a noise-outlier candidate rather than a
            // slip-shift trigger.  Rounding a single sample that lands
            // two cycles off the mean to a new integer and shifting
            // the arc there would be a brittle decision on noisy
            // low-elevation arcs, where |dev| > thresh routinely comes
            // from variance bursts, not real slips.
            //
            // Level modes (HatchMean / LocalMedian): absorb into
            // Welford (the mean is not pinned to rounded_wl×λ on
            // lock), so noise outliers barely perturb the mean while
            // accumulated genuinely-slipped samples migrate it past
            // kWlLockDriftMaxCyc, at which point the drift-demotion
            // block below unlocks the arc and signals a slip with
            // unknown integer for downstream re-resolution.  A real
            // slip keeps re-flagging every epoch in these modes, so
            // absorption loses nothing.
            //
            // TimeDifference modes: the memoryless TD flags a real
            // step exactly once, so silent absorption would discard
            // the only detection and leave the stale locked integer
            // exposed to AR for the dozen or so epochs of mean
            // migration before drift demotion.  Instead, defer one
            // epoch: set the confirmation pending state (resolved at
            // the top of the next processed sample, where outlier and
            // step are separable) and withhold the integer from AR
            // meanwhile.  The sample is still absorbed below as in the
            // level modes, so a declared outlier leaves
            // legacy-identical arc state.
            if (use_td && bia_corrected != stats.bia_corrected) {
                // Frame-flip artifact: the incoming sample is in a
                // different bias frame than the arc (per-epoch
                // code-channel pairing can flip the BIA availability
                // of the same key between epochs).  The deviation
                // reflects the frame change, not a step/outlier
                // hypothesis, so it must not enter the pending store as
                // a jump; take the legacy absorb instead.
                logger->debug(
                    "MW TD flag on locked {} ({}) coincides with a "
                    "bias-frame flip (sample bia={}, arc bia={}); "
                    "absorbing as a frame artifact, no confirmation.",
                    obs.satellite, PppUtils::getSystemString(obs.system),
                    bia_corrected, stats.bia_corrected);
            } else if (use_td && (pivot_zd_slip_in != 0 || !pivot_zd_known_in)) {
                // The pivot's own ZD slipped this epoch: every locked
                // peer's SD series steps by −dN_pivot in common mode.
                // Deferring would desynchronize from the pass-2 SD→ZD
                // re-referencing and from CycleSlipManager's pivot-slip
                // consensus / in-place-repair handling, which operate
                // on this epoch's results.  Behave like the unlocked
                // path with an unknown integer instead: immediate slip,
                // erase, downstream reset, synchronous with the handling
                // built for this event.
                logger->warn(
                    "MW TD flag on locked {} ({}) during a pivot ZD "
                    "slip (pivot dN={}, known={}): emitting immediate "
                    "unknown-integer slip and restarting arc (no "
                    "deferral across a moved SD frame).",
                    obs.satellite, PppUtils::getSystemString(obs.system),
                    pivot_zd_slip_in, pivot_zd_known_in);
                verdict.slip_cycles = 0;
                verdict.wl_known    = false;
                verdict.slip_frac   = 0.5;
                // Strength: this epoch's own gate quantities (the flag is
                // immediate here, not deferred), read before the erase.
                if (thresh > 0.0)
                    verdict.evidence_ratio = std::abs(dev) / thresh;
                verdict.arc_epochs = static_cast<int>(stats.count);
                mw_stats_.erase(stats_key);
                return true;
            } else if (use_td) {
                stats.td_confirm_pending = true;
                stats.td_confirm_jump_m  = mw_diff;
                // Algebraic reference (mw − jump) rather than prev_mw:
                // identical in the normal case, but stays correct when
                // this epoch's TD was itself re-based by an outlier
                // excision (prev_mw then holds the excised outlier).
                stats.td_confirm_pre_mw  = mw_m - mw_diff;
                // Carry the firing gate for the action trace row.
                stats.td_confirm_dev_m    = dev;
                stats.td_confirm_thresh_m = thresh;
                stats.td_confirm_mode     = detect_mode_code;
                logger->info(
                    "MW TD flag on locked {} ({}): |dev|={:.3f} m "
                    "({:.1f} WL) > thresh={:.3f} m, N_WL={}; pending "
                    "one-epoch step/outlier confirmation (integer "
                    "withheld from AR).",
                    obs.satellite, PppUtils::getSystemString(obs.system),
                    std::abs(dev), std::abs(dev) / lambda_wl, thresh,
                    stats.rounded_wl);
            } else {
                logger->debug(
                    "MW outlier on locked {} ({}): |dev|={:.3f} m ({:.1f} WL) "
                    "> thresh={:.3f} m, N_WL={}, mean_cyc={:.3f}; "
                    "absorbing into Welford (drift-demotion will fire if "
                    "persistent).",
                    obs.satellite, PppUtils::getSystemString(obs.system),
                    std::abs(dev), std::abs(dev) / lambda_wl, thresh,
                    stats.rounded_wl, stats.mean / lambda_wl);
            }
            // Fall through to the Welford / drift-demotion block.
        } else {
            // Unlocked arc: no anchored integer to repair against,
            // so the slip is either resolved via a jump-based
            // estimate (BSSD pivot with BIA where dev/λ is near-
            // integer by construction) or surfaced as an unknown
            // slip; in both cases the arc is erased so the next
            // sample initializes a fresh one.
            int slip_cycles = 0;
            bool wl_known   = false;
            // In the TimeDifference modes the published integer is
            // rounded from a single differenced sample whose noise is
            // √2 (TD1) / √6 (TD2) times the level noise, so the fixed
            // 0.25-frac gate alone passes nearly everything once σ_frac
            // approaches a quarter cycle (TD2 at ordinary MW noise).
            // Require the integer to be statistically resolvable from
            // one sample (3·σ_active ≤ 0.25·λ, mirroring the epoch-clean
            // σ-awareness condition) before publishing; otherwise leave
            // wl_known=false so downstream resets instead of repairing
            // (reset-over-repair for low-confidence integers).  Not
            // applied to the level modes (their repair behavior is the
            // shipping baseline); a NaN σ (warmup) refuses to publish.
            const bool integer_resolvable =
                !use_td || (std::isfinite(mode_sigma)
                            && 3.0 * mode_sigma <= 0.25 * lambda_wl);
            if (lambda_wl > 0.0 && integer_resolvable) {
                const double dev_cyc  = dev / lambda_wl;
                const double dev_frac = dev_cyc - std::round(dev_cyc);
                if (std::abs(dev_frac) < 0.25 && std::abs(dev_cyc) >= 0.5) {
                    slip_cycles = static_cast<int>(std::round(dev_cyc));
                    wl_known    = true;
                    verdict.slip_frac = std::abs(dev_frac);
                    logger->info(
                        "WL slip estimated from jump for {}: dN_WL={}, "
                        "dev_cyc={:.3f}, frac={:.3f}",
                        stats_key, slip_cycles, dev_cyc, dev_frac);
                }
            }
            // Float jump estimate (cycles), regardless of the rounding
            // gate above: even an unresolvable integer carries a sound
            // magnitude for downstream candidate selection.
            if (lambda_wl > 0.0)
                verdict.slip_wl_float = dev / lambda_wl;
            verdict.slip_cycles = slip_cycles;
            verdict.wl_known    = wl_known;

            const char* stat_name =
                detect_mode_code == 1 ? "local_median"
              : detect_mode_code == 2 ? "td1"
              : detect_mode_code == 3 ? "td2"
                                      : "hatch_mean";
            logger->warn(
                "MW slip detected for {} ({}): |dev|={:.3f} m ({:.1f} WL) "
                "> thresh={:.3f} m ({:.1f} WL), ref={:.3f} m ({}), "
                "sigma={:.4f} m, arc_n={}, lambda_wl={:.3f} m, bia={}, "
                "locked=no, delta_wl={}",
                obs.satellite, PppUtils::getSystemString(obs.system),
                std::abs(dev), std::abs(dev) / lambda_wl,
                thresh, thresh / lambda_wl,
                use_local ? local_ref
                          : (use_td ? stats.prev_mw : stats.mean),
                stat_name,
                std::isfinite(sigma) ? sigma : -1.0,
                stats.count, lambda_wl,
                stats.bia_corrected ? "yes" : "no",
                slip_cycles != 0 ? std::to_string(slip_cycles) + " WL" : "n/a");

            // Strength (the principal emission path): |dev| / thresh is the
            // gate this branch just failed, on the active statistic and the
            // arc's own adaptive threshold.  A detect-only consumer needs
            // this quantity, and slip_frac cannot substitute for it.
            // stats.count is read before the erase below.
            if (thresh > 0.0)
                verdict.evidence_ratio = std::abs(dev) / thresh;
            verdict.arc_epochs = static_cast<int>(stats.count);

            mw_stats_.erase(stats_key);
            return true;
        }
        return false;
    }

    // ===================================================================================
    // S10  foldSample_: the Welford/Hatch fold and the windows
    // ===================================================================================

    void MwSlipDetector::foldSample_(MwSampleWork& w) const {
        const double mw_m = w.sample.mw_m;
        mw::MwArcStats& stats = *w.stats;
        const double epoch_s = w.epoch_s;
        const double mw_diff = w.mw_diff;
        const double td2 = w.td2;
        const bool is_slip = w.is_slip;
        const bool is_slip_level = w.is_slip_level;

        // Update running statistics (Welford / Hatch).  Always runs,
        // including for locked arcs: the live mean is the basis of the
        // drift-demotion check below.  M2 (and therefore σ_emp) is not
        // updated on slip epochs: a slip-sized delta would inflate the
        // variance accumulator with a discrete-event contribution that
        // does not represent the underlying per-sample noise.  The mean
        // update still runs, so a real cycle slip migrates the running
        // mean past kWlLockDriftMaxCyc and trips drift demotion below; for
        // a noise outlier the 1/n weighting keeps the mean shift negligible.
        // Lagged snapshot before this epoch's value folds in: arcZdMean serves
        // these so a same-epoch read (the SD pre-reduce EWL rows) never re-ingests
        // the current observations (SPEC_mw_wl_card §3.3 double-count guard).
        stats.mean_lag  = stats.mean;
        stats.M2_lag    = stats.M2;
        stats.count_lag = stats.count;
        stats.count++;
        {
            const double eff_n  = static_cast<double>(std::min(stats.count, kMaxHatchCount));
            const double delta  = mw_m - stats.mean;
            stats.mean += delta / eff_n;
            // M2 admission is gated in both frames, the active statistic
            // (is_slip) and the Hatch frame (is_slip_level), because the
            // accumulated delta is Hatch-frame (see the is_slip_level
            // comment above).  Identical to !is_slip in HatchMean mode.
            if (!is_slip && !is_slip_level) {
                const double delta2 = mw_m - stats.mean;
                stats.M2 += delta * delta2;
            }
        }

        // diff_window push: skip slip epochs so cycle-jump diffs
        // (multi-meter on WL) don't enter the per-arc MAD estimator.
        // MAD is statistically robust to a single outlier but
        // partially-filled windows or successive slip events
        // compromise the robustness, and a slip-diff is not a noise
        // sample in any case, so excluding it is correct.  The local
        // raw-MW ring follows the same admission rule so a locked arc
        // absorbing a real slip keeps its pre-slip level as the local
        // reference (the deviation stays visible until drift demotion
        // resolves it) and a noise outlier never tilts the median.
        if (!is_slip) {
            stats.diff_window.push_back(mw_diff);
            if (stats.diff_window.size() > kMwWindowSize)
                stats.diff_window.pop_front();
            pushLocalWindow(stats, mw_m);
            // TD2 history (spread source for MwTdOrder::Second), same
            // admission rule and cap.  Maintained in every mode (one
            // subtraction and a deque push), so the statistic is warm if
            // the user switches modes between runs on the same data.
            if (std::isfinite(td2)) {
                stats.td2_window.push_back(td2);
                if (stats.td2_window.size() > kMwWindowSize)
                    stats.td2_window.pop_front();
            }
        }
        // prev_mw / prev_diff update unconditionally (slip epochs included),
        // as prev_mw always has.  After a locked arc absorbs a real step, the
        // next TD1 is post−post ≈ 0 (no echo).  A locked-arc noise outlier
        // echoes once (next TD1 = clean − outlier); where that echo is
        // flagged and absorbed again it is benign and kept out of every
        // window by the !is_slip admission rule, and in the TimeDifference
        // modes the confirmation above excises a declared outlier before
        // the echo is tested.  Unlocked arcs erase at the slip epoch, so
        // neither case arises.
        stats.prev_diff    = mw_diff;
        stats.prev_mw      = mw_m;
        stats.last_epoch_s = epoch_s;
    }

    // ===================================================================================
    // S11  checkDriftDemotion_: a locked arc's drift
    // ===================================================================================

    bool MwSlipDetector::checkDriftDemotion_(MwSampleWork& w) {
        const std::string& stats_key = w.lane.stats_key;
        const double lambda_wl = w.sample.lambda_wl;
        mw::MwArcStats& stats = *w.stats;
        MwVerdict& verdict = w.verdict;

        // Drift-based lock demotion.  When the running mean has
        // drifted past kWlLockDriftMaxCyc cycles from the locked
        // integer, the locked integer does not match the
        // cumulative evidence: either the lock was premature or
        // the arc has absorbed enough slipped samples to migrate.
        // Unlock and signal a slip with unknown integer so the
        // downstream AR / KF reset the ambiguity and let the arc
        // re-lock at the right integer once Welford settles.
        // Mean / M2 / count / diff_window are intentionally kept:
        // they carry the now-current noise estimate, and the lock
        // criterion's |frac| < kWlMaxFrac gate will keep the arc
        // unlocked until the mean has migrated close enough to the
        // new integer to round confidently.
        if (!(stats.wl_locked && lambda_wl > 0.0)) return false;
        const double drift_cyc = stats.mean / lambda_wl - stats.rounded_wl;
        if (std::abs(drift_cyc) > kWlLockDriftMaxCyc) {
            log_manager_.getLogger("mw_slip_detector")->warn(
                "WL lock demoted for {} (epoch {}): mean drifted to "
                "{:.3f} cycles from N_WL={} (drift={:+.3f} cyc, "
                "threshold={:.2f}). Signaling unknown-integer slip.",
                stats_key, stats.count, stats.mean / lambda_wl,
                stats.rounded_wl, drift_cyc, kWlLockDriftMaxCyc);
            stats.wl_locked  = false;
            stats.rounded_wl = 0;
            verdict.slip_cycles = 0;
            verdict.wl_known    = false;
            verdict.slip_frac   = std::abs(drift_cyc);
            // Float estimate: the migrated mean's offset from the
            // demoted integer is the absorbed cumulative slip.
            verdict.slip_wl_float = drift_cyc;
            // Strength: dev = the drift, threshold = the lock-drift bound
            // (this path writes no trace row of its own).  Nothing is
            // erased here, so stats.count is already the count at the flag.
            verdict.evidence_ratio =
                std::abs(drift_cyc) / kWlLockDriftMaxCyc;
            verdict.arc_epochs = static_cast<int>(stats.count);
            return true;
        }
        return false;
    }

    // ===================================================================================
    // S12  checkLevelBackstop_: an unlocked arc's level disagreement
    // ===================================================================================

    bool MwSlipDetector::checkLevelBackstop_(MwSampleWork& w) {
        const PppObservation& obs = w.obs;
        const std::string& stats_key = w.lane.stats_key;
        const std::string& sig1 = w.lane.sig1;
        const std::string& sig2 = w.lane.sig2;
        const double mw_m = w.sample.mw_m;
        const double lambda_wl = w.sample.lambda_wl;
        const double sat_mw_bias = w.sample.sat_mw_bias;
        const std::string& pivot_sat = w.frame.pivot_sat;
        mw::MwArcStats& stats = *w.stats;
        const double local_sigma_scaled = w.local_sigma_scaled;
        const double local_ref = w.local_ref;
        const bool use_local = w.use_local;
        const bool use_td = w.use_td;
        const double sigma = w.sigma;
        MwVerdict& verdict = w.verdict;

        // Unlocked-arc level-disagreement backstop (LocalMedian and
        // TimeDifference modes).  The local median absorbs a missed step
        // within ~N/2 epochs and the memoryless TD statistic absorbs it in
        // one epoch with no re-test, both far faster than the Hatch mean's
        // ~kMaxHatchCount-epoch memory, and drift demotion above protects
        // only locked arcs.  Without a backstop, an unlocked arc that
        // quietly absorbed a real slip has no escape path: count keeps
        // rising, the statistical lock criterion eventually locks the wrong
        // integer (the frac/3σ lock gates only delay locking; they never
        // signal a slip), and that integer feeds AR via
        // getResolvedWlAmbiguities.  When the two level estimators (slow
        // Hatch mean vs short window median, both maintained and
        // frame-shifted identically in every mode) disagree by more than
        // the lock-drift bound, the arc's level history is internally
        // inconsistent: a step was absorbed, or the arc is on a drift
        // steep enough (≳ 4–5 mm/epoch sustained) that no level estimate
        // is trustworthy.  Mirror the unlocked-slip path: unknown-integer
        // slip, erase, fresh restart (the init-frac check then
        // independently verifies the new arc's first sample).  HatchMean
        // mode is excluded for bit-identity with the legacy gate; its
        // ~kMaxHatchCount-epoch dev_hatch persistence provides the re-test
        // behavior natively.
        const bool backstop_applies =
            use_local || (use_td && std::isfinite(local_ref));
        if (backstop_applies && !stats.wl_locked && lambda_wl > 0.0
            && stats.mw_window_size >= kMinEpochsForAdaptive) {
            const double level_gap_cyc = (stats.mean - local_ref) / lambda_wl;
            if (std::abs(level_gap_cyc) > kWlLockDriftMaxCyc) {
                log_manager_.getLogger("mw_slip_detector")->warn(
                    "MW level disagreement for {} (epoch {}): hatch mean "
                    "{:.3f} m vs local median {:.3f} m ({:+.3f} WL cyc > "
                    "{:.2f}) — an absorbed step or untrustworthy drift. "
                    "Signaling unknown-integer slip and restarting arc.",
                    stats_key, stats.count, stats.mean, local_ref,
                    level_gap_cyc, kWlLockDriftMaxCyc);

                // Trace record for the backstop reset: this epoch's regular
                // row was already written with is_slip=0 (the dev test
                // passed).  Without a flagged row this reset class would not
                // appear in the trace, the primary slip-chain verification
                // artifact, so mirror the init-slip row convention and write
                // the actual decision quantities:
                // dev = the level gap, thresh = the drift bound, giving
                // |dev|/thresh > 1 on the flagged row.
                if (mwwl_trace_.is_open()) {
                    writeTraceRecord(obs, sig1, sig2, mw_m, stats.mean,
                                     /*dev_from_mean=*/level_gap_cyc * lambda_wl,
                                     sigma, stats.count, lambda_wl,
                                     /*thresh_m=*/kWlLockDriftMaxCyc * lambda_wl,
                                     crossSatSigmaFloor(stats_key, obs.elevation),
                                     /*is_slip=*/true, /*is_lli=*/false,
                                     stats.bia_corrected, sat_mw_bias,
                                     stats.rounded_wl, stats.wl_locked, pivot_sat,
                                     local_ref, local_sigma_scaled,
                                     /*detect_mode=*/1,
                                     /*active_dev_m=*/level_gap_cyc * lambda_wl);
                }

                verdict.slip_cycles = 0;
                verdict.wl_known    = false;
                verdict.slip_frac   = std::abs(level_gap_cyc);
                // Float estimate: the absorbed step is the level gap with its
                // sign reversed (post-step local median minus the slow Hatch mean).
                verdict.slip_wl_float = -level_gap_cyc;
                // Strength: dev = the level gap, threshold = the drift bound,
                // the pairing this path's own trace row writes.  Count read
                // before the erase below.
                verdict.evidence_ratio =
                    std::abs(level_gap_cyc) / kWlLockDriftMaxCyc;
                verdict.arc_epochs = static_cast<int>(stats.count);
                mw_stats_.erase(stats_key);
                return true;
            }
        }
        return false;
    }

} // namespace GnssProcessor
