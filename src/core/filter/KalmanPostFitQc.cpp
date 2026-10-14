/**
 * @file KalmanPostFitQc.cpp
 * @brief The post-fit outlier screen of KalmanFilter::update: validatePostFitResiduals.
 *
 * Reports the residuals, runs the coherent-offset group detectors, the Huber
 * R-inflation pass, the gate and the per-row dispatch, the complete-reset backstop and
 * the clean-commit bookkeeping.  The responses it dispatches to (ambiguity reset, row
 * exclusion, complete reset) are in KalmanOutlierResponse.cpp.  Moved here from
 * KalmanFilter.cpp unchanged by card UOM step U2b.
 */

#include "filter/KalmanFilter.hpp"
#include "filter/KalmanRowIdentity.hpp"   // splitBssdSatId, bandWavelengthForSat
#include "common/PppUtils.hpp"
#include "slip/CycleSlipManager.hpp"      // MW tiebreak
#include <spdlog/spdlog.h>
#include <Eigen/Dense>
#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace GnssProcessor {

    using kalman::splitBssdSatId;
    using kalman::bandWavelengthForSat;

    // ===================================================================
    // Post-fit residual validation
    // ===================================================================

    std::pair<bool, bool> KalmanFilter::validatePostFitResiduals(
        EpochMeasurement& measurement, Eigen::VectorXd& x, Eigen::MatrixXd& P) {

        auto klogger = log_manager_.getLogger("kalman_filter");
        const std::string epoch_prefix;  // see KalmanFilter::update for rationale

        const double threshold = config_.getResidualThreshold();
        const double bad_meas_percent = config_.getBadMeasPercent();

        const Eigen::VectorXd& res  = measurement.residuals;
        const Eigen::MatrixXd& vres = measurement.residuals_covariance;
        const Eigen::VectorXd& sres = measurement.residuals_standardized;

        const size_t num_obs = static_cast<size_t>(res.size());
        if (num_obs == 0) {
            klogger->warn("{}No observations for post-fit validation", epoch_prefix);
            return { false, false };
        }

        // Pass-invariant denominator for the complete-reset fraction below: the
        // rows this epoch started with, i.e. the current usable rows plus every
        // row that earlier passes excluded.  At this point excluded_measurements
        // holds exactly the prior exclusions (this pass's dispatch has not run
        // yet), so the sum is the original count before any peeling.
        //
        // With the shrinking per-pass num_obs as the denominator, the reset
        // would trip after excluding only about p/(1+p) of the original set
        // (33 % at p = 0.5, not the configured 50 %), aborting a converging peel
        // and re-inflating the filter every epoch while a code outlier persists.
        const size_t total_obs_count =
            num_obs + measurement.excluded_measurements.size();

        const size_t meta_n = std::min({ num_obs, measurement.satellite_ids.size(), measurement.signal_types.size() });

        // --- Per-measurement diagnostic log ---
        // Helper: emit the per-residual table at a given log level.
        auto logResidualTable = [&](spdlog::level::level_enum lvl) {
            for (size_t i = 0; i < meta_n; ++i) {
                const auto ei = static_cast<Eigen::Index>(i);
                const double var_i = (i < static_cast<size_t>(vres.rows()) && i < static_cast<size_t>(vres.cols()))
                    ? vres(ei, ei) : std::numeric_limits<double>::quiet_NaN();
                const double sigma_i = std::sqrt(std::max(var_i, 0.0));
                const double s_i = sres(ei);
                const double s_abs = std::abs(s_i);
                klogger->log(lvl,
                    "{}  res[{:02d}] {} {}: v={:+.4f} m, sigma={:.4f} m, s={:+.3f}, |s|={:.3f}, outlier={}",
                    epoch_prefix, static_cast<int>(i),
                    measurement.satellite_ids[i], measurement.signal_types[i],
                    res(ei), sigma_i, s_i, s_abs,
                    (std::isfinite(s_abs) && s_abs > threshold) ? "YES" : "no");
            }
        };

        klogger->debug("{}Post-fit residual check: n_obs={}, threshold={:.3f}, bad_count={}, bad_percent={:.3f}",
            epoch_prefix, num_obs, threshold, measurement.bad_residual_count, bad_meas_percent);

        if (meta_n != num_obs) {
            klogger->warn("{}Residual metadata size mismatch: res={}, sat_ids={}, sig_types={}. Logged {} entries.",
                epoch_prefix, num_obs, measurement.satellite_ids.size(), measurement.signal_types.size(), meta_n);
        }

        // --- Find worst residual ---
        const Eigen::VectorXd sres_abs = sres.cwiseAbs();
        size_t max_idx = 0;
        const double max_sres = sres_abs.maxCoeff(&max_idx);

        size_t num_over = 0;
        for (Eigen::Index i = 0; i < sres.size(); ++i)
            if (std::isfinite(std::abs(sres(i))) && std::abs(sres(i)) > threshold)
                ++num_over;

        // Per-row residual table.  Output gets strictly more concise as the
        // threshold rises:
        //   trace: full table every epoch
        //   debug: table only on outlier events
        //   info : none (the Residual summary line covers it)
        //   warn : none (pivot warnings and outlier-flagged warns only)
        if (klogger->should_log(spdlog::level::trace)) {
            logResidualTable(spdlog::level::trace);
        } else if (max_sres > threshold && klogger->should_log(spdlog::level::debug)) {
            logResidualTable(spdlog::level::debug);
        }

        // INFO when this pass commits cleanly (max_sres ≤ threshold), giving one
        // summary line per converged epoch; DEBUG when outliers will be handled,
        // since mid-rebuild summaries are diagnostic.  A pivot-slip escalation
        // carries its own warn-level message, so info here would be redundant.
        const auto residual_summary_level =
            (max_sres > threshold) ? spdlog::level::debug : spdlog::level::info;

        // RMS of the standardized residuals over finite entries, a one-number
        // check of the filter against its own covariance.  Because sres is
        // floored at R, a consistent filter gives RMS² near the mean
        // redundancy (below 1); values well above 1 mean the filter is
        // over-confident (R or P too tight), values far below it mean it is
        // over-conservative.
        double sum_sq = 0.0;
        std::size_t n_valid_sres = 0;
        for (Eigen::Index i = 0; i < sres.size(); ++i) {
            const double s = sres(i);
            if (std::isfinite(s)) {
                sum_sq += s * s;
                ++n_valid_sres;
            }
        }
        const double rms_sres = (n_valid_sres > 0)
            ? std::sqrt(sum_sq / static_cast<double>(n_valid_sres)) : 0.0;

        // Position σ from the predicted-state covariance diagonal.
        // ECEF position lives at state indices 0..2 by the
        // StateManager schema (see include/filter/StateKeySchema.hpp).
        // Reported in metres so users can eyeball convergence at a
        // glance from a single info line.
        const double sx = (P.rows() >= 3) ? std::sqrt(std::max(P(0, 0), 0.0)) : 0.0;
        const double sy = (P.rows() >= 3) ? std::sqrt(std::max(P(1, 1), 0.0)) : 0.0;
        const double sz = (P.rows() >= 3) ? std::sqrt(std::max(P(2, 2), 0.0)) : 0.0;

        klogger->log(residual_summary_level,
            "{}Residual summary: max|sres|={:.3f} at idx={}, RMS|sres|={:.3f}, "
            "over_threshold={}/{}, σ_pos=({:.3f}, {:.3f}, {:.3f}) m",
            epoch_prefix, max_sres, max_idx, rms_sres, num_over, num_obs, sx, sy, sz);

        // --- Statistical QC reporting (item #9) ---------------------------
        // Diagnostics only; the propagated state and covariance are untouched.
        // The variance factor rescales a reporting copy of σ_pos
        // (σ_pos → formal·√σ̂₀²) and never feeds back into P.
        if (config_.enableVarianceFactor()) {
            const double vf = measurement.variance_factor;
            const double sf = (std::isfinite(vf) && vf > 0.0) ? std::sqrt(vf) : 1.0;
            klogger->log(residual_summary_level,
                "{}Variance factor: sigma0^2={:.3f}, dof={:.2f}, "
                "sigma_pos(scaled)=({:.3f}, {:.3f}, {:.3f}) m",
                epoch_prefix, vf, measurement.variance_dof,
                sx * sf, sy * sf, sz * sf);
        }
        if (config_.enableDataSnooping() &&
            measurement.residual_wtest.size() == sres.size() &&
            measurement.residual_redundancy.size() == sres.size()) {
            constexpr double kUncheckableRedundancy = 0.1;
            const double w_crit = config_.dataSnoopingCritical();
            size_t n_uncheckable = 0, n_flagged = 0;
            double max_w = 0.0;
            size_t max_w_idx = 0;
            for (Eigen::Index i = 0; i < measurement.residual_wtest.size(); ++i) {
                if (measurement.residual_redundancy(i) < kUncheckableRedundancy) {
                    ++n_uncheckable;
                    continue;
                }
                const double w_i = std::abs(measurement.residual_wtest(i));
                if (!std::isfinite(w_i)) continue;
                if (w_i > max_w) { max_w = w_i; max_w_idx = static_cast<size_t>(i); }
                if (w_i > w_crit) ++n_flagged;
            }
            const std::string w_sat = (max_w_idx < measurement.satellite_ids.size())
                ? measurement.satellite_ids[max_w_idx] : std::string("?");
            const std::string w_sig = (max_w_idx < measurement.signal_types.size())
                ? measurement.signal_types[max_w_idx] : std::string("?");
            klogger->log(residual_summary_level,
                "{}Data-snooping: max|w|={:.3f} ({} {}) vs w_crit={:.2f}, "
                "flagged={}, uncheckable={}/{}",
                epoch_prefix, max_w, w_sat, w_sig, w_crit,
                n_flagged, n_uncheckable, num_obs);
        }

        // --- Pivot slip detector (MAD-based, percentage-thresholded) ---
        //
        // Runs before the per-row dispatch.  A BSSD pivot with a phase glitch
        // (sub-cycle drift, multipath ringing, partial reacquisition) offsets
        // every SD residual that uses it by the same sign and magnitude on the
        // same band.  The per-row |sres| > threshold check would read those as N
        // independent outliers and peel them one by one, resetting each
        // non-pivot ambiguity and clearing the MW arc histories of satellites
        // that did not slip.
        //
        // Rows are grouped by (pivot, signal) using the full RINEX-3 code, so
        // "L1W" and "L1C" are separate groups.  That matches the state
        // granularity of uncombined PPP-RTK and preserves sensitivity to a
        // disturbance confined to one tracking attribute, which band-level
        // grouping would dilute.  Each group is tested for the coherent-offset
        // signature:
        //   1. ≥ MIN_GROUP_SIZE rows (statistical power).
        //   2. ≥ ABOVE_FRACTION_MIN of |sres| above the per-row threshold; a
        //      fraction rather than a count, so the trigger scales with visibility.
        //   3. |median(residual)| ≥ MEDIAN_FLOOR_M, so numerical noise cannot fire it.
        //   4. MAD/|median| ≤ MAD_RATIO_MAX.  This is the robustness step: the
        //      median ignores one or two satellites that also slipped, and the
        //      MAD measures spread without being inflated by them.  Tight means
        //      a coherent pivot offset; loose means independent outliers.
        //
        // On detection the detector requests a forced pivot handover for the
        // constellation, records the satellites the handover must not elect,
        // and excludes all of the group's rows from the rebuild.  The per-row
        // dispatch is skipped for this pass, since the pivot slip explains
        // every outlier.
        {
            constexpr size_t MIN_GROUP_SIZE      = 4;
            constexpr size_t MIN_POOL_SIZE       = 3;  // cross-band detector
            constexpr size_t MIN_DISTINCT_BANDS  = 2;  // cross-band detector
            constexpr double ABOVE_FRACTION_MIN  = 0.5;
            constexpr double MAD_RATIO_MAX       = 0.4;
            constexpr double MEDIAN_FLOOR_M      = 0.005;

            // Robust band (k·MAD, floored) that classifies which non-pivot SD
            // rows in a firing group disagree with the coherent median.  Those
            // satellites are co-disturbed or slipping independently, so they
            // become replacement-pivot exclusions and the handover lands on a
            // consensus satellite.  k = 3 sits just inside the observed
            // co-offset separation (a second offset satellite lands about
            // 3.2·MAD off the median); the floor guards MAD → 0.  The band is
            // permissive on purpose: PivotManager's retired-pivot backstop
            // guarantees termination regardless.
            constexpr double HANDOVER_EXCLUDE_MAD_K   = 3.0;
            constexpr double HANDOVER_EXCLUDE_FLOOR_M = 0.02;

            // Wavelength-fraction floor for the median.  A real cycle slip
            // is an integer number of carrier wavelengths; below ~λ/4 the
            // residual cannot be a slip, only sub-cycle bias (multipath,
            // PCV miscal, kinematic dynamics).  Config-tunable per the
            // GnssConfig::getPivotSlipMinCycles() docstring; 0 disables.
            // Helper is shared with the per-row phase outlier handler;
            // see @c bandWavelengthForSat in include/filter/KalmanRowIdentity.hpp.
            const double cyc_floor_fraction = config_.getPivotSlipMinCycles();

            struct Group {
                std::vector<size_t> indices;
                std::vector<double> residuals;
                size_t above = 0;
            };
            // Per-signal groups keyed by (pivot, full RINEX-3 signal), so "L1W"
            // and "L1C" stay separate.
            std::map<std::pair<std::string, std::string>, Group> groups;
            // Cross-band per-pivot pools, used as a fallback when individual
            // signals carry too few non-pivot rows to clear MIN_GROUP_SIZE
            // but the multi-band picture still shows a coherent slip pattern.
            // @c pivot_pool_bands tracks distinct bands (the frequency axis),
            // not signals, because the cross-band detector's evidence depends
            // on independent frequency channels; two attributes on the same
            // carrier (L1W + L1C) are not independent.
            std::map<std::string, Group> pivot_pools;
            std::map<std::string, std::set<std::string>> pivot_pool_bands;

            // Exclude SD rows whose ATX correction is frequency-fallback-
            // substituted on either sat or pivot.  The substituted PCV adds a
            // small per-sat bias that, pooled across non-pivot rows in one band,
            // mimics a pivot slip (large median, tight MAD).  Because that bias
            // is structural rather than transient, without this gate "all"-mode
            // runs covering bands like GPS L5 flip-flop forever: every new pivot
            // reads as slipped.
            auto row_atx_substituted = [&](const std::string& sat,
                                            const std::string& pivot_sat,
                                            const std::string& signal) -> bool {
                if (sat.empty() || pivot_sat.empty() || signal.empty()) return false;
                const System sys =
                    sat[0] == 'G' ? System::GPS :
                    sat[0] == 'E' ? System::Galileo :
                    sat[0] == 'R' ? System::GLONASS :
                    sat[0] == 'C' ? System::BeiDou :
                    sat[0] == 'J' ? System::QZSS :
                    sat[0] == 'S' ? System::SBAS : System::Unknown;
                if (sys == System::Unknown) return false;
                if (ppp_model_.wasAtxSubstituted(sat, signal, sys)) return true;
                if (ppp_model_.wasAtxSubstituted(pivot_sat, signal, sys)) return true;
                return false;
            };

            for (size_t i = 0; i < meta_n; ++i) {
                const auto& sat_id = measurement.satellite_ids[i];
                const auto& sig    = measurement.signal_types[i];
                if (sig.size() < 2 || sig[0] != 'L') continue;
                const auto [prim, pivot] = splitBssdSatId(sat_id);
                if (pivot.empty() || prim == pivot) continue;  // not a non-pivot SD row
                if (row_atx_substituted(prim, pivot, sig)) continue;  // ATX-substituted: bias mimics slip
                const std::string band = sig.substr(0, 2);     // "L1", "L2", "L5": the cross-band gate axis only
                const double r  = res(static_cast<Eigen::Index>(i));
                const double sa = std::abs(sres(static_cast<Eigen::Index>(i)));
                const bool   ab = std::isfinite(sa) && sa > threshold;

                // Per-signal grouping (full RINEX code) for the per-band
                // detector below, matching the per-signal state granularity
                // of uncombined PPP-RTK.
                auto& g = groups[{ pivot, sig }];
                g.indices.push_back(i);
                g.residuals.push_back(r);
                if (ab) ++g.above;

                auto& pp = pivot_pools[pivot];
                pp.indices.push_back(i);
                pp.residuals.push_back(r);
                if (ab) ++pp.above;
                pivot_pool_bands[pivot].insert(band);  // band, not signal: the independence axis
            }

            // Helper: median (modifies its argument).  Even-count median
            // averages the two middle values for stability.
            auto medianInPlace = [](std::vector<double>& v) -> double {
                const size_t n = v.size();
                std::nth_element(v.begin(), v.begin() + n / 2, v.end());
                if (n % 2 != 0) return v[n / 2];
                const double upper = v[n / 2];
                std::nth_element(v.begin(), v.begin() + n / 2 - 1, v.end());
                return 0.5 * (v[n / 2 - 1] + upper);
            };

            // Record the satellites a forced handover must not elect as the
            // new pivot for @p cons: the flagged @p pivot_sat itself, plus any
            // non-pivot row whose SD residual sits outside the coherent band
            // around @p med.  Such rows are co-disturbed or slipping
            // independently; electing one would re-anchor the SD frame on
            // another offset satellite and cause a further handover.  Keyed
            // per constellation; drained by takeRequestedPivotHandoverExcludeSats.
            auto recordHandoverExcludes = [&](const Group& grp, double med,
                                              double mad,
                                              const std::string& pivot_sat,
                                              const std::string& cons) {
                auto& ex = pivot_handover_exclude_sats_[cons];
                ex.insert(pivot_sat);
                const double band = std::max(HANDOVER_EXCLUDE_MAD_K * mad,
                                             HANDOVER_EXCLUDE_FLOOR_M);
                for (size_t k = 0; k < grp.indices.size(); ++k) {
                    if (std::abs(grp.residuals[k] - med) <= band) continue;
                    const auto [prim, _piv] =
                        splitBssdSatId(measurement.satellite_ids[grp.indices[k]]);
                    if (!prim.empty()) ex.insert(prim);
                }
            };

            // Record every non-pivot member of a firing group for post-handover
            // ionosphere / sat-bias re-inflation (code-detector path only; see
            // takeRequestedPivotHandoverIonoReinflateSats).  recordHandoverExcludes
            // keeps only the pivot and the few deviators outside the coherent
            // band; this set also holds the coherent majority inside the band,
            // which are the satellites that absorbed the pivot's code bias.
            // Re-inflating them lets the post-handover frame re-estimate the
            // states that the lossless re-reference cannot correct.
            auto recordIonoReinflate = [&](const Group& grp,
                                           const std::string& cons) {
                auto& reinf = pivot_handover_reinflate_sats_[cons];
                for (size_t k = 0; k < grp.indices.size(); ++k) {
                    const auto [prim, _piv] =
                        splitBssdSatId(measurement.satellite_ids[grp.indices[k]]);
                    if (!prim.empty()) reinf.insert(prim);
                }
            };

            bool any_pivot_slip = false;
            for (auto& [key, g] : groups) {
                const size_t n = g.indices.size();
                if (n < MIN_GROUP_SIZE) continue;
                const double frac = static_cast<double>(g.above) / static_cast<double>(n);
                if (frac < ABOVE_FRACTION_MIN) continue;

                std::vector<double> r_copy = g.residuals;
                const double med = medianInPlace(r_copy);
                if (std::abs(med) < MEDIAN_FLOOR_M) continue;

                const std::string pivot_sat = key.first;

                // Compute the MAD around the median first, so the cycle-floor
                // decision below can use cluster tightness as evidence.  A
                // tight cluster shows that the residuals are not independent
                // noise: multipath or atmosphere at sub-cycle scale gives
                // MAD/|med| well above the cutoff, while a coherent pivot
                // disturbance typically gives MAD/|med| ≤ 0.1.
                std::vector<double> abs_dev;
                abs_dev.reserve(g.residuals.size());
                for (double v : g.residuals) abs_dev.push_back(std::abs(v - med));
                const double mad = medianInPlace(abs_dev);
                const double mad_ratio = mad / std::abs(med);
                if (mad_ratio > MAD_RATIO_MAX) continue;

                // The λ-fraction floor rejects sub-cycle medians as "bias, not
                // slip".  This detector requests a handover, not a per-row
                // reset, and re-referencing is the right response to any
                // coherent pivot disturbance: wind-up error, a half-cycle
                // ambiguity, pivot-only multipath and a hardware bias change all
                // show sub-cycle coherent SD residuals.  The floor is therefore
                // bypassed at or below TIGHT_CLUSTER_MAD_RATIO, where a common
                // pivot disturbance is the only plausible explanation across
                // several rows (the handover then fires even on a 1 cm median).
                // Looser clusters keep the floor as a hedge against
                // single-frequency atmospheric events.
                constexpr double TIGHT_CLUSTER_MAD_RATIO = 0.2;
                if (cyc_floor_fraction > 0.0 && mad_ratio > TIGHT_CLUSTER_MAD_RATIO) {
                    // key.second is the full signal (e.g. "L1W"); the
                    // wavelength is per-band, so strip the attribute.
                    const std::string band_for_lambda = key.second.substr(0, 2);
                    const double lambda = bandWavelengthForSat(pivot_sat, band_for_lambda);
                    if (lambda > 0.0) {
                        const double abs_floor = cyc_floor_fraction * lambda;
                        if (std::abs(med) < abs_floor) {
                            klogger->debug(
                                "{}Pivot-slip candidate suppressed by λ/{:.2f} floor "
                                "on {} {} signal: |med|={:.4f} m < floor={:.4f} m "
                                "(λ={:.4f} m, MAD/|med|={:.2f} > tight-cluster "
                                "threshold {:.2f})",
                                epoch_prefix, 1.0 / cyc_floor_fraction,
                                pivot_sat, key.second, std::abs(med), abs_floor,
                                lambda, mad_ratio, TIGHT_CLUSTER_MAD_RATIO);
                            continue;
                        }
                    }
                }

                // Identify the constellation from the pivot's PRN prefix.  All
                // non-pivot rows in this group share the constellation by
                // construction (BSSD groups SD pairs within a constellation).
                const std::string cons      = PppUtils::getSystemString(pivot_sat);

                // Temporal corroboration via the residual-history tracker.
                //
                // The spatial criteria above also match genuine atmospheric
                // events (storm fronts, scintillation) that shift many
                // satellites at once.  The tracker adds a second axis, whether
                // each row is anomalous against its own history: among rows
                // with at least warmup_n samples, it counts the fraction with
                // |z| > z_threshold.  When that fraction reaches the tracker's
                // fraction threshold the log reports HIGH confidence; spatial
                // evidence alone reports MEDIUM.
                //
                // The handover fires either way: the temporal signal is
                // diagnostic only.  Promoting it to a gate is a one-line change
                // here once the false-positive rate is known.
                std::string confidence_tag = "MEDIUM (spatial-only)";
                size_t temporal_total = 0, temporal_outliers = 0;
                if (residual_history_.isEnabled()) {
                    for (size_t i = 0; i < g.indices.size(); ++i) {
                        const size_t row_idx = g.indices[i];
                        const auto& sat_id = measurement.satellite_ids[row_idx];
                        const auto& sig    = measurement.signal_types[row_idx];
                        const auto [prim, _piv] = splitBssdSatId(sat_id);
                        const double r = g.residuals[i];
                        const auto ev = residual_history_.evaluate(prim, sig, r);
                        if (ev.warmed_up) {
                            ++temporal_total;
                            if (ev.is_outlier) ++temporal_outliers;
                        }
                    }
                    if (temporal_total > 0) {
                        const double temporal_frac =
                            static_cast<double>(temporal_outliers) / static_cast<double>(temporal_total);
                        if (temporal_frac >= residual_history_.fractionThreshold()) {
                            confidence_tag = std::string("HIGH (temporal agrees: ")
                                + std::to_string(temporal_outliers) + "/"
                                + std::to_string(temporal_total) + " rows over z-threshold)";
                        } else {
                            confidence_tag = std::string("MEDIUM (temporal disagrees: ")
                                + std::to_string(temporal_outliers) + "/"
                                + std::to_string(temporal_total) + " rows over z-threshold, "
                                + "below fraction threshold)";
                        }
                    } else {
                        confidence_tag = "MEDIUM (temporal: no warmed-up rows yet)";
                    }
                }

                klogger->warn(
                    "{}PIVOT SLIP [{}] on {} {} {} signal: {}/{} SD residuals coherent "
                    "(median={:+.4f} m, MAD={:.4f} m, MAD/|med|={:.2f}, threshold-pass fraction={:.2f}). "
                    "Requesting forced pivot handover.",
                    epoch_prefix, confidence_tag, cons, pivot_sat, key.second /* signal */,
                    g.above, n, med, mad, mad_ratio, frac);

                // Request a pivot handover for this constellation. Application
                // forwards this to MeasurementModel::requestPivotHandover after
                // KF returns NeedsMeasurementRebuild; the next pass's
                // computeMeasurementUpdate triggers PivotManager's handover
                // path, which losslessly re-references all SD ambiguities,
                // ionosphere states, and satellite biases to a new pivot.
                // Resetting every non-pivot SD ambiguity in place would be
                // wrong: the non-pivots are stable, and only the pivot's
                // observation drifted.
                requested_pivot_handovers_.insert(cons);
                recordHandoverExcludes(g, med, mad, pivot_sat, cons);

                // Exclude every row in the affected group from this rebuild
                // pass so the per-row outlier handler doesn't pile on with
                // false-positive resets.  After the handover (next pass),
                // these rows will be re-formed against the new pivot and
                // residuals against clean satellites will be near zero.
                for (size_t row_idx : g.indices) {
                    const auto& sat_id = measurement.satellite_ids[row_idx];
                    const auto& sig_t  = measurement.signal_types[row_idx];
                    const auto [prim, _piv] = splitBssdSatId(sat_id);
                    measurement.excluded_measurements.insert(prim + "_" + sig_t);
                }
                any_pivot_slip = true;
            }

            // Cross-band pivot-slip detector.
            //
            // The per-band pass needs ≥ MIN_GROUP_SIZE rows on one signal.
            // Sparse per-band coverage (mixed BeiDou-2/3 with C2I/C7I against
            // C1X/C5X, new-block satellites with partial signal sets) can leave
            // every group short of that while the pooled picture is
            // unambiguous.  Rows are therefore pooled across bands per pivot,
            // with MIN_POOL_SIZE = 3 and at least MIN_DISTINCT_BANDS distinct
            // bands.  The multi-band requirement makes the pooled evidence
            // stronger than a within-band group: coherent same-sign residuals
            // across independent frequency channels are hard for multipath or
            // atmospherics to mimic.
            for (auto& [pivot_sat, pp] : pivot_pools) {
                const std::string cons = PppUtils::getSystemString(pivot_sat);
                // Skip if the per-signal detector already requested a
                // handover for this constellation: its row set is a
                // subset of this pool's rows, and a second warning would
                // report the same physical event again.
                if (requested_pivot_handovers_.count(cons)) continue;

                const size_t n = pp.indices.size();
                if (n < MIN_POOL_SIZE) continue;

                const auto& bands_set = pivot_pool_bands[pivot_sat];
                if (bands_set.size() < MIN_DISTINCT_BANDS) continue;

                const double frac = static_cast<double>(pp.above) / static_cast<double>(n);
                if (frac < ABOVE_FRACTION_MIN) continue;

                std::vector<double> r_copy = pp.residuals;
                const double med = medianInPlace(r_copy);
                if (std::abs(med) < MEDIAN_FLOOR_M) continue;

                std::vector<double> abs_dev;
                abs_dev.reserve(pp.residuals.size());
                for (double v : pp.residuals) abs_dev.push_back(std::abs(v - med));
                const double mad = medianInPlace(abs_dev);
                const double mad_ratio = mad / std::abs(med);
                if (mad_ratio > MAD_RATIO_MAX) continue;

                // Build a comma-joined band list for the log line.
                std::string bands_str;
                for (const auto& b : bands_set) {
                    if (!bands_str.empty()) bands_str += ",";
                    bands_str += b;
                }

                klogger->warn(
                    "{}PIVOT SLIP [cross-band] on {} {} ({} bands [{}]): {}/{} "
                    "SD residuals coherent across bands (median={:+.4f} m, "
                    "MAD={:.4f} m, MAD/|med|={:.2f}, threshold-pass fraction={:.2f}). "
                    "Requesting forced pivot handover.",
                    epoch_prefix, cons, pivot_sat, bands_set.size(), bands_str,
                    pp.above, n, med, mad, mad_ratio, frac);

                requested_pivot_handovers_.insert(cons);
                recordHandoverExcludes(pp, med, mad, pivot_sat, cons);

                for (size_t row_idx : pp.indices) {
                    const auto& sat_id = measurement.satellite_ids[row_idx];
                    const auto& sig_t  = measurement.signal_types[row_idx];
                    const auto [prim, _piv] = splitBssdSatId(sat_id);
                    measurement.excluded_measurements.insert(prim + "_" + sig_t);
                }
                any_pivot_slip = true;
            }

            // Code-side pivot disturbance detector (per-band + cross-band).
            //
            // The phase detectors above, gated on sig[0]=='C' instead of 'L'.  A
            // pivot-side code bias (DCB drift, band/attribute ATX miscalibration,
            // SV hardware glitch) puts the same offset on every SD code row that
            // uses that pivot.  Without a coherence test the per-row dispatch
            // peels the rows one at a time, the uncombined iono state absorbs the
            // bias through repeated updates, and bad_residual_count climbs until
            // the rebuild budget runs out.  The signature is outliers on every SD
            // pair against one pivot for one code signal, growing each epoch.
            //
            // Differences from the phase detector:
            //   - MEDIAN_FLOOR_M_CODE = 0.30 m, against 5 mm for phase.  Routine
            //     code multipath reaches 0.1-0.2 m without any pivot problem;
            //     0.30 m is about 0.6× a typical code σ, above multipath and
            //     below a real disturbance.
            //   - No wavelength floor (cycles do not apply to code).
            //   - No temporal corroboration: the EWMA tracker is tuned for phase
            //     variance, and code would need its own tuning.
            //   - No non-pivot multi-band variant.  Without the integer-cycle
            //     check, coherent code outliers on a non-pivot are bad
            //     measurements that the per-row dispatch already handles.
            //
            // The action matches the phase detectors (bulk-exclude and request
            // a forced handover) because the bias persists: firing across
            // consecutive epochs, it contaminates the pivot's iono state, and
            // re-referencing is the only structural fix.
            //
            // Reuses Group, medianInPlace, row_atx_substituted, any_pivot_slip
            // and requested_pivot_handovers_ from the enclosing block.
            {
                constexpr size_t MIN_GROUP_CODE        = 4;
                constexpr size_t MIN_POOL_CODE         = 3;
                constexpr size_t MIN_DISTINCT_BANDS_C  = 2;
                constexpr double ABOVE_FRACTION_MIN_C  = 0.5;
                constexpr double MAD_RATIO_MAX_C       = 0.4;
                constexpr double MEDIAN_FLOOR_M_CODE   = 0.30;  // m

                std::map<std::pair<std::string, std::string>, Group> code_groups;
                std::map<std::string, Group> code_pivot_pools;
                std::map<std::string, std::set<std::string>> code_pivot_pool_bands;

                for (size_t i = 0; i < meta_n; ++i) {
                    const auto& sat_id = measurement.satellite_ids[i];
                    const auto& sig    = measurement.signal_types[i];
                    if (sig.size() < 2 || sig[0] != 'C') continue;
                    // Skip combined / iono-free pseudo-signals: their
                    // residual isn't a clean per-signal bias and the
                    // coherence test would mix scales.
                    if (sig.find('_') != std::string::npos) continue;
                    const auto [prim, pivot] = splitBssdSatId(sat_id);
                    if (pivot.empty() || prim == pivot) continue;  // not a non-pivot SD row
                    if (row_atx_substituted(prim, pivot, sig)) continue;  // ATX-substituted: bias mimics disturbance
                    const std::string band = sig.substr(0, 2);  // "C5", "C7", "C1", ...: the cross-band gate axis only
                    const double r  = res(static_cast<Eigen::Index>(i));
                    const double sa = std::abs(sres(static_cast<Eigen::Index>(i)));
                    const bool   ab = std::isfinite(sa) && sa > threshold;

                    // Per-signal grouping (full RINEX code, e.g. "C1W"
                    // vs "C1C") matches the per-signal state granularity of
                    // uncombined PPP-RTK: satellite biases and ATX corrections
                    // are keyed per signal, so an attribute-specific pivot
                    // disturbance affects only the matching states and is
                    // detected at that resolution.
                    auto& g = code_groups[{ pivot, sig }];
                    g.indices.push_back(i);
                    g.residuals.push_back(r);
                    if (ab) ++g.above;

                    auto& pp = code_pivot_pools[pivot];
                    pp.indices.push_back(i);
                    pp.residuals.push_back(r);
                    if (ab) ++pp.above;
                    code_pivot_pool_bands[pivot].insert(band);  // band, not signal: the independence axis
                }

                // --- Per-band code pivot-disturbance detector ---
                // Runs regardless of whether a phase detector already
                // requested a handover for this constellation: the row
                // exclusion still matters because phase exclusion didn't
                // touch the code rows.  The set insert into
                // @c requested_pivot_handovers_ is idempotent.
                for (auto& [key, g] : code_groups) {
                    const size_t n = g.indices.size();
                    if (n < MIN_GROUP_CODE) continue;
                    const double frac = static_cast<double>(g.above) / static_cast<double>(n);
                    if (frac < ABOVE_FRACTION_MIN_C) continue;

                    std::vector<double> r_copy = g.residuals;
                    const double med = medianInPlace(r_copy);
                    if (std::abs(med) < MEDIAN_FLOOR_M_CODE) continue;

                    std::vector<double> abs_dev;
                    abs_dev.reserve(g.residuals.size());
                    for (double v : g.residuals) abs_dev.push_back(std::abs(v - med));
                    const double mad = medianInPlace(abs_dev);
                    const double mad_ratio = mad / std::abs(med);
                    if (mad_ratio > MAD_RATIO_MAX_C) continue;

                    const std::string& pivot_sat = key.first;
                    const std::string  cons      = PppUtils::getSystemString(pivot_sat);

                    klogger->warn(
                        "{}PIVOT CODE DISTURBANCE on {} {} {} signal: {}/{} SD code "
                        "residuals coherent (median={:+.4f} m, MAD={:.4f} m, "
                        "MAD/|med|={:.2f}, threshold-pass fraction={:.2f}). "
                        "Requesting forced pivot handover.",
                        epoch_prefix, cons, pivot_sat, key.second /* signal */,
                        g.above, n, med, mad, mad_ratio, frac);

                    requested_pivot_handovers_.insert(cons);
                    recordHandoverExcludes(g, med, mad, pivot_sat, cons);
                    recordIonoReinflate(g, cons);
                    for (size_t row_idx : g.indices) {
                        const auto& sat_id = measurement.satellite_ids[row_idx];
                        const auto& sig_t  = measurement.signal_types[row_idx];
                        const auto [prim, _piv] = splitBssdSatId(sat_id);
                        measurement.excluded_measurements.insert(prim + "_" + sig_t);
                    }
                    any_pivot_slip = true;
                }

                // --- Cross-band code pivot-disturbance detector ---
                // Skip when a handover is already queued for the
                // constellation: the cross-band pool's row set is a
                // superset of any per-band group already excluded,
                // and the cross-band log line would otherwise duplicate
                // the per-band warning for the same physical event.
                for (auto& [pivot_sat, pp] : code_pivot_pools) {
                    const std::string cons = PppUtils::getSystemString(pivot_sat);
                    if (requested_pivot_handovers_.count(cons)) continue;

                    const size_t n = pp.indices.size();
                    if (n < MIN_POOL_CODE) continue;

                    const auto& bands_set = code_pivot_pool_bands[pivot_sat];
                    if (bands_set.size() < MIN_DISTINCT_BANDS_C) continue;

                    const double frac = static_cast<double>(pp.above) / static_cast<double>(n);
                    if (frac < ABOVE_FRACTION_MIN_C) continue;

                    std::vector<double> r_copy = pp.residuals;
                    const double med = medianInPlace(r_copy);
                    if (std::abs(med) < MEDIAN_FLOOR_M_CODE) continue;

                    std::vector<double> abs_dev;
                    abs_dev.reserve(pp.residuals.size());
                    for (double v : pp.residuals) abs_dev.push_back(std::abs(v - med));
                    const double mad = medianInPlace(abs_dev);
                    const double mad_ratio = mad / std::abs(med);
                    if (mad_ratio > MAD_RATIO_MAX_C) continue;

                    std::string bands_str;
                    for (const auto& b : bands_set) {
                        if (!bands_str.empty()) bands_str += ",";
                        bands_str += b;
                    }

                    klogger->warn(
                        "{}PIVOT CODE DISTURBANCE [cross-band] on {} {} ({} bands [{}]): "
                        "{}/{} SD code residuals coherent (median={:+.4f} m, MAD={:.4f} m, "
                        "MAD/|med|={:.2f}, threshold-pass fraction={:.2f}). "
                        "Requesting forced pivot handover.",
                        epoch_prefix, cons, pivot_sat, bands_set.size(), bands_str,
                        pp.above, n, med, mad, mad_ratio, frac);

                    requested_pivot_handovers_.insert(cons);
                    recordHandoverExcludes(pp, med, mad, pivot_sat, cons);
                    recordIonoReinflate(pp, cons);
                    for (size_t row_idx : pp.indices) {
                        const auto& sat_id = measurement.satellite_ids[row_idx];
                        const auto& sig_t  = measurement.signal_types[row_idx];
                        const auto [prim, _piv] = splitBssdSatId(sat_id);
                        measurement.excluded_measurements.insert(prim + "_" + sig_t);
                    }
                    any_pivot_slip = true;
                }
            }

            // Non-pivot multi-band cycle-slip detector.
            //
            // The cross-band pivot detector with its coherence axes rotated 90°:
            // one non-pivot satellite slipping on several bands at once gives
            // one satellite × many bands of same-sign anomalous residuals
            // against a clean pivot.  A real slip jumps the ambiguity by N
            // cycles on every band, so the SD residual is N·λ_band:
            // band-dependent in metres, band-independent in cycles.  That
            // integer-cycle consistency is the strongest discriminator from
            // atmospheric and multipath events, which scale with 1/f² in metres
            // (iono) or stay roughly constant in metres (tropo, multipath).
            //
            // Valuable as a GLONASS backstop, where the FDMA inter-channel WL
            // bias inflates the MW detector's MAD floor and costs sensitivity.
            //
            // On detection the satellite's ambiguities are marked for re-seed
            // and its rows are excluded from this pass.  No pivot handover is
            // requested, since the pivot is unaffected.
            {
                constexpr size_t MIN_NONPIVOT_BANDS = 2;
                constexpr double MIN_RESIDUAL_M     = 0.05;  // ~quarter L1 cycle
                constexpr double INT_CYCLE_TOL      = 0.30;
                constexpr int    MAX_SLIP_CYCLES    = 50;    // sanity cap

                struct NpvEntry {
                    std::vector<size_t> indices;
                    std::vector<double> residuals;
                    std::set<std::string> bands;
                    size_t above = 0;
                    int    sign = 0;     // 0=mixed/empty, ±1 = consistent
                };
                std::map<std::string, NpvEntry> nonpivot_pools;

                for (size_t i = 0; i < meta_n; ++i) {
                    const auto& sat_id = measurement.satellite_ids[i];
                    const auto& sig    = measurement.signal_types[i];
                    if (sig.size() < 2 || sig[0] != 'L') continue;
                    const auto [prim, pivot] = splitBssdSatId(sat_id);
                    if (pivot.empty() || prim == pivot) continue;

                    const double r  = res(static_cast<Eigen::Index>(i));
                    const double sa = std::abs(sres(static_cast<Eigen::Index>(i)));
                    const bool   ab = std::isfinite(sa) && sa > threshold;

                    auto& e = nonpivot_pools[prim];
                    e.indices.push_back(i);
                    e.residuals.push_back(r);
                    e.bands.insert(sig.substr(0, 2));
                    if (ab) {
                        ++e.above;
                        // Sign tracking only over anomalous rows: noise-driven
                        // sign flips on quiet rows shouldn't disqualify the sat.
                        const int rs = (r > 0.0) ? +1 : (r < 0.0 ? -1 : 0);
                        if (e.sign == 0 && rs != 0)        e.sign = rs;
                        else if (rs != 0 && rs != e.sign)  e.sign = 0;  // mixed
                    }
                }

                // Local PppUtils for wavelength lookup.  Default GLONASS slot
                // table is used (accurate to <0.5 MHz, fine for the 0.3-cycle
                // tolerance in the integer check below).
                PppUtils ppp_utils_local(log_manager_);

                auto satToSystem = [](const std::string& sat) -> System {
                    if (sat.empty()) return System::Unknown;
                    switch (sat[0]) {
                    case 'G': return System::GPS;
                    case 'E': return System::Galileo;
                    case 'R': return System::GLONASS;
                    case 'C': return System::BeiDou;
                    case 'J': return System::QZSS;
                    case 'S': return System::SBAS;
                    default:  return System::Unknown;
                    }
                };

                const double amb_init_std = config_.getInitialStd().ambiguity;

                for (auto& [sat, e] : nonpivot_pools) {
                    if (e.bands.size() < MIN_NONPIVOT_BANDS) continue;
                    if (e.above       < MIN_NONPIVOT_BANDS) continue;
                    if (e.sign == 0) continue;  // mixed signs → not a slip

                    std::vector<double> abs_residuals;
                    abs_residuals.reserve(e.residuals.size());
                    for (double r : e.residuals) abs_residuals.push_back(std::abs(r));
                    const double med_abs = medianInPlace(abs_residuals);
                    if (med_abs < MIN_RESIDUAL_M) continue;

                    // Integer-cycle consistency: each row's |residual| / λ_band
                    // must round to the same non-zero integer with tolerance
                    // INT_CYCLE_TOL.  This is what distinguishes a real cycle
                    // slip from atmospheric / multipath events.
                    const System sys = satToSystem(sat);
                    int   common_cycles    = 0;
                    bool  first            = true;
                    bool  integer_consistent = true;
                    for (size_t k = 0; k < e.indices.size(); ++k) {
                        const auto& sig_t = measurement.signal_types[e.indices[k]];
                        const double lam = ppp_utils_local.getWavelength(sys, sig_t, sat);
                        if (!(std::isfinite(lam) && lam > 0.0)) {
                            integer_consistent = false; break;
                        }
                        const double cyc = std::abs(e.residuals[k]) / lam;
                        const int    ric = static_cast<int>(std::lround(cyc));
                        if (ric == 0 || ric > MAX_SLIP_CYCLES
                         || std::abs(cyc - static_cast<double>(ric)) > INT_CYCLE_TOL) {
                            integer_consistent = false; break;
                        }
                        if (first) { common_cycles = ric; first = false; }
                        else if (ric != common_cycles) {
                            integer_consistent = false; break;
                        }
                    }
                    if (!integer_consistent) continue;

                    std::string bands_str;
                    for (const auto& b : e.bands) {
                        if (!bands_str.empty()) bands_str += ",";
                        bands_str += b;
                    }
                    const std::string cons = PppUtils::getSystemString(sat);

                    klogger->warn(
                        "{}NON-PIVOT SLIP on {} {} ({} bands [{}], sign={:+d}, "
                        "N={} cycles, |median res|={:.4f} m, {}/{} rows above threshold). "
                        "Resetting ambiguities for this satellite.",
                        epoch_prefix, cons, sat, e.bands.size(), bands_str,
                        e.sign, common_cycles, med_abs, e.above, e.indices.size());

                    // Single-sat reset: mark each ambiguity for re-seed and
                    // wipe its row/col cross-covariance with diagonal inflated
                    // to the initial-ambiguity variance.  ensureRequiredStates
                    // on the next rebuild pass will compute a fresh SD seed
                    // against the (unchanged) pivot.
                    for (size_t row_idx : e.indices) {
                        const auto& sat_id = measurement.satellite_ids[row_idx];
                        const auto& sig_t  = measurement.signal_types[row_idx];
                        const auto [prim, _piv] = splitBssdSatId(sat_id);
                        const std::string amb_key = prim + "_" + sig_t;
                        measurement.excluded_measurements.insert(amb_key);

                        const int amb_idx = state_manager_.getAmbiguityIndex(amb_key);
                        if (amb_idx >= 0 && amb_idx < static_cast<int>(P.rows())) {
                            const auto ai = static_cast<Eigen::Index>(amb_idx);
                            P.row(ai).setZero();
                            P.col(ai).setZero();
                            P(ai, ai) = amb_init_std * amb_init_std;
                        }
                        state_manager_.markAmbiguityPendingReseed(amb_key);
                    }
                    // Record at satellite granularity only.  Every phase
                    // ambiguity of the satellite was reset, which is the
                    // satellite-wide signature reinflateResetSatStates keys on,
                    // and it makes the callers' arc-clearing loops drop the whole
                    // MW / TurboEdit history, all of which a multi-band
                    // common-integer slip invalidates.  Without this entry the
                    // reset is invisible to arc clearing, re-inflation and
                    // smoother segmentation.
                    last_reset_satellites_.push_back(sat);
                    any_pivot_slip = true;
                }
            }

            if (any_pivot_slip) {
                ++measurement.bad_residual_count;
                klogger->debug("{}Pivot slip handled. Measurement rebuild required.", epoch_prefix);
                klogger->debug("{}Post-fit residual check result: outlier_found={}, complete_reset={}",
                    epoch_prefix, "true", "false");
                return { /*outlier_found=*/true, /*complete_reset=*/false };
            }
        }

        // --- Huber R-inflation pass (post-fit) ----------------------------
        // For sub-cycle phase rows whose post-fit |sres| exceeds the Huber
        // threshold, queue a target R(i,i) so the next rebuild pass downweights
        // the row until |sres| lands at that threshold, and set
        // outlier_found=true to trigger that pass.  Rows with |res| ≥ cyc_floor·λ
        // are cycle-slip territory and go to the reset dispatch instead.
        {
            const double huber_k     = config_.getPhaseHuberSresThreshold();
            const double cyc_floor   = config_.getPivotSlipMinCycles();
            const bool   use_innov   = config_.enableHuberInnovation();
            const bool   huber_on    = config_.enableHuberRInflation();
            // Rebuild-loop exit tolerance.  The post-fit IRLS fixed point
            // is |sres| = k exactly, so a bare "|sres| <= k" test would
            // chase an asymptote; the gate below requires a pass's
            // relative R step to clear this tolerance before re-queuing.
            // The innovation scheme lands on k in one shot, so there the
            // gate only blocks a no-op floating-point re-trigger.
            constexpr double kHuberRConvergenceTol = 0.05;
            bool huber_triggered     = false;
            if (huber_on && huber_k > 0.0) {
                for (Eigen::Index i = 0; i < static_cast<Eigen::Index>(num_obs); ++i) {
                    const auto& sig = measurement.signal_types[static_cast<size_t>(i)];
                    if (sig.empty() || sig[0] != 'L') continue;
                    if (sig.find('_') != std::string::npos) continue;  // iono-free combos
                    if (sig.size() < 2) continue;

                    const auto& sat_id = measurement.satellite_ids[static_cast<size_t>(i)];
                    const auto [prim, _hub_piv] = splitBssdSatId(sat_id);
                    const std::string band_str = sig.substr(0, 2);
                    const double lambda = bandWavelengthForSat(prim, band_str);
                    if (!(lambda > 0.0)) continue;

                    // Two R-inflation schemes, selected by
                    // estimation.enable_huber_innovation:
                    //
                    //  - innovation one-shot: |s| = |v|/sqrt(HPHᵀ + R) is monotone
                    //    in R, so the R landing |s| = k is closed-form,
                    //    R_target = v²/k² − HPHᵀ, with HPHᵀ = (H·P⁻·Hᵀ)(i,i) the
                    //    R-independent prior variance in that row.  One pass.
                    //
                    //  - post-fit IRLS: |sres| = |res|/sqrt(R), inflated by
                    //    (|sres|/k)² per pass.  The post-fit residual drifts up as
                    //    the row loses weight, so this converges to k only
                    //    asymptotically and can overshoot first.
                    double abs_res  = 0.0;
                    double abs_sres = 0.0;
                    double r_target = 0.0;
                    if (use_innov) {
                        const double v = measurement.innovations(i);
                        abs_res = std::abs(v);
                        if (cyc_floor > 0.0 && abs_res >= cyc_floor * lambda) continue;
                        const double hph =
                            (measurement.H.row(i) * P).dot(measurement.H.row(i));
                        abs_sres = abs_res / std::sqrt(hph + R_(i, i));
                        if (abs_sres <= huber_k) continue;
                        r_target = (v * v) / (huber_k * huber_k) - hph;
                    } else {
                        abs_res = std::abs(res(i));
                        if (cyc_floor > 0.0 && abs_res >= cyc_floor * lambda) continue;
                        abs_sres = std::abs(sres(i));
                        if (abs_sres <= huber_k) continue;
                        const double factor = (abs_sres / huber_k) * (abs_sres / huber_k);
                        r_target = R_(i, i) * factor;
                    }

                    const std::string key = prim + "|" + sig;
                    auto it = pending_r_inflations_.find(key);
                    const double r_existing = (it != pending_r_inflations_.end())
                                            ? it->second : R_(i, i);
                    if (r_target > r_existing * (1.0 + kHuberRConvergenceTol)) {
                        pending_r_inflations_[key] = r_target;
                        huber_triggered = true;
                        if (use_innov) {
                            klogger->info(
                                "{}Huber R-inflation (innovation one-shot) for {} {}: "
                                "|v|/sqrt(S)={:.2f} > k={:.1f}, |innov|={:.4f} m "
                                "(sub-cycle, λ={:.4f} m); R(i,i): {:.3e} → {:.3e} m²",
                                epoch_prefix, prim, sig, abs_sres, huber_k,
                                abs_res, lambda, R_(i, i), r_target);
                        } else {
                            klogger->info(
                                "{}Huber R-inflation queued for {} {}: |sres|={:.2f} > k={:.1f}, "
                                "|res|={:.4f} m (sub-cycle, λ={:.4f} m); R(i,i): {:.3e} → {:.3e} m²",
                                epoch_prefix, prim, sig, abs_sres, huber_k,
                                abs_res, lambda, R_(i, i), r_target);
                        }
                    }
                }
            }

            if (huber_triggered) {
                klogger->debug(
                    "{}Huber R-inflation pass requesting measurement rebuild "
                    "({} rows queued)",
                    epoch_prefix, pending_r_inflations_.size());
                klogger->debug("{}Post-fit residual check result: outlier_found={}, complete_reset={}",
                    epoch_prefix, "true", "false");
                return { /*outlier_found=*/true, /*complete_reset=*/false };
            }
        }

        // --- Baarda data-snooping gate override ---------------------------
        // By default the gate fires on the fixed σ threshold (gate_* ≡ max_sres /
        // max_idx / threshold), so the dispatch below is byte-identical with
        // data-snooping off.
        //
        // When enabled, it fires on |wᵢ| > dataSnoopingCritical() over checkable
        // rows (redundancy ≥ kUncheckableRedundancy) and picks the worst such
        // row.  Low-redundancy rows are uncheckable: a blunder there is absorbed
        // into the estimate, so resetting its ambiguity would discard good
        // information.  The MW tiebreaker still works in the σ domain; when the
        // w-test catches something the σ cut missed, the tiebreaker collects no
        // candidates and the w-selected row is handled directly.
        double gate_stat = max_sres;
        size_t gate_idx  = max_idx;
        double gate_crit = threshold;
        if (config_.enableDataSnooping() &&
            measurement.residual_wtest.size() == sres.size() &&
            measurement.residual_redundancy.size() == sres.size()) {
            constexpr double kUncheckableRedundancy = 0.1;
            double max_w = -1.0;
            size_t max_w_idx = max_idx;
            for (Eigen::Index i = 0; i < measurement.residual_wtest.size(); ++i) {
                if (measurement.residual_redundancy(i) < kUncheckableRedundancy) continue;
                const double w_i = std::abs(measurement.residual_wtest(i));
                if (std::isfinite(w_i) && w_i > max_w) {
                    max_w = w_i;
                    max_w_idx = static_cast<size_t>(i);
                }
            }
            if (max_w >= 0.0) {
                gate_stat = max_w;
                gate_idx  = max_w_idx;
                gate_crit = config_.dataSnoopingCritical();
            }
        }

        // --- Dispatch outlier handling ---
        bool is_outlier_found = false;
        bool is_complete_reset = false;

        if (gate_stat > gate_crit) {

            // (The per-row residual table for outlier diagnosis is
            // logged by the level-dependent block above: at debug
            // level it is emitted when max_sres > threshold, at
            // info and warn it is not.  Setting the kalman_filter
            // level to debug shows the per-row context of an
            // outlier event.)

            // --- MW tiebreaker for competing phase outliers ---
            // With several satellites in one constellation at near-equal |sres|
            // the raw maximum is an unreliable pick, so MW deviation scores
            // choose the one most likely to have slipped.
            //
            // chosen_idx starts at gate_idx (≡ max_idx unless data-snooping
            // reselected via the w-test).  The tiebreaker refines only among rows
            // the σ cut also flags, so a w-test-only hit keeps gate_idx.
            size_t chosen_idx = gate_idx;
            if (!measurement.signal_types[max_idx].empty()
                && measurement.signal_types[max_idx][0] == 'L'
                && cycle_slip_manager_) {

                // Ratio threshold: a competitor is "close" if its |sres|
                // is within 5% of the maximum.
                constexpr double kRatioThresh = 0.95;

                const auto [max_prim, max_ref] = splitBssdSatId(
                    measurement.satellite_ids[max_idx]);
                const std::string max_cons = PppUtils::getSystemString(max_prim);

                // Collect all phase-outlier indices for the same constellation
                // whose |sres| is close to the maximum.
                struct Candidate {
                    size_t idx;
                    std::string sat;   // primary satellite
                    double sres_abs;
                    double mw_score;
                };
                std::vector<Candidate> candidates;

                for (size_t i = 0; i < meta_n; ++i) {
                    const auto& sig = measurement.signal_types[i];
                    if (sig.empty() || sig[0] != 'L') continue;
                    const double sa = std::abs(sres(static_cast<Eigen::Index>(i)));
                    if (!std::isfinite(sa) || sa <= threshold) continue;
                    if (sa < max_sres * kRatioThresh) continue;

                    const auto [prim, ref] = splitBssdSatId(
                        measurement.satellite_ids[i]);
                    if (PppUtils::getSystemString(prim) != max_cons) continue;

                    // Deduplicate by satellite: one entry per satellite, the
                    // first qualifying row in row order.
                    bool found = false;
                    for (auto& c : candidates) {
                        if (c.sat == prim) { found = true; break; }
                    }
                    if (!found) {
                        candidates.push_back({i, prim, sa, 0.0});
                    }
                }

                if (candidates.size() > 1) {
                    // Query MW scores (skip if the MW detector is disabled).
                    if (const auto* mw = cycle_slip_manager_->mwDetector()) {
                        for (auto& c : candidates)
                            c.mw_score = mw->getWorstMwScore(c.sat);
                    }

                    // Pick the candidate with the highest MW score.
                    auto best = std::max_element(candidates.begin(),
                        candidates.end(), [](const Candidate& a, const Candidate& b) {
                            return a.mw_score < b.mw_score;
                        });

                    if (best != candidates.end() && best->mw_score > 0.0
                        && best->idx != max_idx) {
                        klogger->debug(
                            "{}MW tiebreaker: {} candidates within {:.0f}% of max "
                            "|sres|={:.3f}. Choosing {} (MW score={:.2f}) over {} "
                            "(MW score={:.2f})",
                            epoch_prefix, candidates.size(),
                            (1.0 - kRatioThresh) * 100.0, max_sres,
                            best->sat, best->mw_score,
                            candidates[0].sat == best->sat
                                ? candidates[1].sat : candidates[0].sat,
                            candidates[0].sat == best->sat
                                ? candidates[1].mw_score : candidates[0].mw_score);
                        chosen_idx = best->idx;
                    }
                }
            }

            const std::string outlier_sat = measurement.satellite_ids[chosen_idx];
            const std::string outlier_sig = measurement.signal_types[chosen_idx];
            // Capture residual values by value before dispatch: handleCodeOutlier
            // shifts measurement.residuals* to drop the offending row, after which
            // sres/res(chosen_idx) refer to the row that sat at chosen_idx+1.
            const double outlier_sres     = sres(static_cast<Eigen::Index>(chosen_idx));
            const double outlier_sres_abs = std::abs(outlier_sres);
            const double outlier_res      = res(static_cast<Eigen::Index>(chosen_idx));
            const double sigma_max = std::sqrt(std::max(
                vres(static_cast<Eigen::Index>(chosen_idx),
                     static_cast<Eigen::Index>(chosen_idx)), 0.0));

            klogger->warn(
                "{}Max standardized residual for {} {}: |s|={:.3f} (s={:+.3f}, residual {:.3f} m, sigma {:.3f} m, idx {})",
                epoch_prefix, outlier_sat, outlier_sig,
                outlier_sres_abs, outlier_sres, outlier_res, sigma_max, chosen_idx);

            if (!outlier_sig.empty() && outlier_sig[0] == 'L') {
                // λ-fraction floor on the raw phase residual (with sub-cycle slip
                // suppression enabled).  A real slip moves the observation by an
                // integer number of wavelengths, so anything below
                // cyc_floor_fraction × λ cannot be one, and an ambiguity reset
                // (variance inflation, loss of converged precision) is the wrong
                // response.  The row is still excluded from this rebuild: its
                // sub-cycle bias may be real (multipath, PCV miscalibration,
                // kinematic dynamics) and would pull position and clock if absorbed.
                //
                // Not applied to iono-free combinations ("L1C_L2W"-style): the
                // combined residual is not a clean multiple of any single λ, so
                // those rows still go through the reset.
                const double cyc_floor_fraction = config_.getPivotSlipMinCycles();
                const bool   subcycle_on        = config_.enableSubcycleSlipSuppression();
                const bool is_iono_free = (outlier_sig.find('_') != std::string::npos);
                bool suppressed_by_floor = false;
                if (subcycle_on && cyc_floor_fraction > 0.0 && !is_iono_free && outlier_sig.size() >= 2) {
                    const auto [prim_for_lambda, _] = splitBssdSatId(outlier_sat);
                    const std::string band_str = outlier_sig.substr(0, 2);
                    const double lambda = bandWavelengthForSat(prim_for_lambda, band_str);
                    if (lambda > 0.0
                        && std::abs(outlier_res) < cyc_floor_fraction * lambda) {
                        klogger->info(
                            "{}Phase outlier suppressed by λ/{:.2f} floor on {} {}: "
                            "|res|={:.4f} m < floor={:.4f} m (λ={:.4f} m); "
                            "excluding row, ambiguity state preserved",
                            epoch_prefix, 1.0 / cyc_floor_fraction,
                            outlier_sat, outlier_sig,
                            std::abs(outlier_res),
                            cyc_floor_fraction * lambda, lambda);
                        suppressed_by_floor = true;
                    }
                }

                const auto [prim, ref] = splitBssdSatId(outlier_sat);
                if (!suppressed_by_floor) {
                    handlePhaseOutlier(measurement, outlier_sat, outlier_sig, x, P, epoch_prefix);
                }
                measurement.excluded_measurements.insert(prim + "_" + outlier_sig);

                // Check for correlated phase outliers on other frequencies
                // for the same satellite.  When a cycle slip affects an SD
                // pair, all phase signals for that satellite typically show
                // outliers with nearly equal |sres| and opposite signs.
                // Resetting only one frequency leaves the others wrong and
                // costs extra rebuild passes.
                //
                // The λ-floor applies here too: sub-cycle correlated
                // residuals are bias, not slip, on every band.
                for (size_t i = 0; i < meta_n; ++i) {
                    if (i == chosen_idx) continue;
                    const auto& co_sig = measurement.signal_types[i];
                    if (co_sig.empty() || co_sig[0] != 'L') continue;
                    if (measurement.satellite_ids[i] != outlier_sat) continue;
                    const double co_abs = std::abs(sres(static_cast<Eigen::Index>(i)));
                    if (!std::isfinite(co_abs) || co_abs <= threshold) continue;

                    const double co_res = res(static_cast<Eigen::Index>(i));
                    bool co_suppressed = false;
                    if (subcycle_on && cyc_floor_fraction > 0.0
                        && co_sig.find('_') == std::string::npos
                        && co_sig.size() >= 2) {
                        const std::string co_band = co_sig.substr(0, 2);
                        const double co_lambda = bandWavelengthForSat(prim, co_band);
                        if (co_lambda > 0.0
                            && std::abs(co_res) < cyc_floor_fraction * co_lambda) {
                            klogger->info(
                                "{}Correlated phase outlier suppressed by λ/{:.2f} floor "
                                "on {} {}: |res|={:.4f} m < floor={:.4f} m",
                                epoch_prefix, 1.0 / cyc_floor_fraction,
                                outlier_sat, co_sig, std::abs(co_res),
                                cyc_floor_fraction * co_lambda);
                            co_suppressed = true;
                        }
                    }

                    klogger->debug(
                        "{}Correlated phase outlier on same satellite: {} {} "
                        "|s|={:.3f}, {} alongside {} {}",
                        epoch_prefix, outlier_sat, co_sig, co_abs,
                        co_suppressed ? "excluding (floor)" : "resetting",
                        outlier_sat, outlier_sig);

                    if (!co_suppressed) {
                        handlePhaseOutlier(measurement, outlier_sat, co_sig, x, P, epoch_prefix);
                    }
                    measurement.excluded_measurements.insert(prim + "_" + co_sig);
                }
            } else {
                handleCodeOutlier(measurement, outlier_sat, outlier_sig, chosen_idx, epoch_prefix);
            }

            is_outlier_found = true;
            measurement.bad_residual_count++;

            klogger->warn("{}Outlier flagged: idx={}, sat={}, sig={}, max_sres={:.3f} > threshold={:.3f}; bad_residual_count={}",
                epoch_prefix, chosen_idx, outlier_sat, outlier_sig,
                outlier_sres_abs,
                threshold, measurement.bad_residual_count);
        } else {
            klogger->debug("{}No outliers: max_sres={:.3f} <= threshold={:.3f}",
                epoch_prefix, max_sres, threshold);
        }

        // --- Complete reset if too many outliers accumulated ---
        // The denominator is the original measurement count before peeling (see
        // total_obs_count above), so the backstop fires at the configured
        // bad-measurement fraction rather than aborting the peel early.
        if (measurement.bad_residual_count > static_cast<size_t>(bad_meas_percent * total_obs_count)) {
            klogger->warn("{}Resetting filter: too many outliers ({}/{}), limit={:.3f}*n={}",
                epoch_prefix, measurement.bad_residual_count, total_obs_count,
                bad_meas_percent, static_cast<size_t>(bad_meas_percent * total_obs_count));

            resetFilterCovariance(P, epoch_prefix);

            is_outlier_found = true;
            is_complete_reset = true;
            measurement.bad_residual_count = 0;
        }

        klogger->debug("{}Post-fit residual check result: outlier_found={}, complete_reset={}",
            epoch_prefix,
            is_outlier_found ? "true" : "false",
            is_complete_reset ? "true" : "false");

        // Feed accepted residuals into the residual-history tracker, on
        // the clean-commit path only (no outlier detected, no complete
        // reset).  Excluded measurements are skipped because their
        // residuals were not applied to the state, so the EWMA
        // statistics describe only the rows that updated the state.
        if (residual_history_.isEnabled() && !is_outlier_found && !is_complete_reset) {
            for (size_t i = 0; i < meta_n; ++i) {
                const auto& sat_id = measurement.satellite_ids[i];
                const auto& sig    = measurement.signal_types[i];
                if (sat_id.empty() || sig.empty()) continue;
                const auto [prim, _piv] = splitBssdSatId(sat_id);
                const std::string excl_key = prim + "_" + sig;
                if (measurement.excluded_measurements.count(excl_key)) continue;
                const double r = res(static_cast<Eigen::Index>(i));
                if (!std::isfinite(r)) continue;
                residual_history_.record(prim, sig, r);
            }

            // --- Stuck-code-bias scan ----------------------------------
            // Queue resets (applied after the commit) for sat-bias states that
            // are either outlier-rejected for too many consecutive epochs (the
            // closed loop in which an over-converged bias keeps tripping the
            // gate on its own row) or whose post-fit EWMA mean has drifted past
            // the bias floor (the slow-drift case, in which the row survives but
            // the converged bias has moved away from the true value).
            //
            // BSSD pivots are skipped: PivotManager freezes their sat-bias at
            // (value 0, variance 0), and resetting it would break the SD frame.
            // A genuine pivot bias shows up as a coherent shift across the group
            // and belongs to the spatial pivot detectors instead.
            const auto& stuck_cfg = config_.getStuckCodeBiasConfig();
            if (stuck_cfg.enabled && config_.estimateSatBiases()) {
                const auto& pivots = state_manager_.getFrozenPivotSatellites();
                for (const auto& [key, _idx] : state_manager_.getSatBiasStates()) {
                    const auto bar = key.find('|');
                    if (bar == std::string::npos) continue;
                    const std::string sat = key.substr(0, bar);
                    const std::string sig = key.substr(bar + 1);

                    if (pivots.count(sat)) continue;  // frozen pivot

                    const auto ev = residual_history_.evaluateStuckBias(
                        sat, sig,
                        stuck_cfg.bias_floor_m,
                        stuck_cfg.min_streak);
                    if (!ev.stuck) continue;

                    // Queue (key, EWMA mean), applied post-commit in update().
                    // The EWMA mean is the sign-consistent residual offset, a much
                    // better re-seed than inflating variance and waiting for
                    // innovations to pull the bias back (many epochs with a
                    // converged P).  On the streak-only trigger (warmed_up false)
                    // the mean is 0 and only variance inflates, so the filter
                    // absorbs the next surviving innovation freely.
                    pending_sat_bias_resets_[key] = ev.warmed_up ? ev.ewma_mean : 0.0;

                    klogger->warn(
                        "{}Stuck code-bias detected on {} {}: "
                        "streak={}, EWMA mean={:+.4f} m (std={:.4f}, warmed_up={}); "
                        "queued reset (value+={:+.4f} m, variance -> cold start)",
                        epoch_prefix, sat, sig,
                        ev.streak, ev.ewma_mean, ev.ewma_std,
                        ev.warmed_up ? "yes" : "no",
                        ev.warmed_up ? ev.ewma_mean : 0.0);
                }
            }

            // Emit the per-epoch residual-history digest at info
            // level alongside the per-pass "Residual summary" line
            // above: how many rows are tracked and warmed up, and the
            // median and largest σ across rows, without per-row trace
            // logs.
            const auto rh_summary = residual_history_.summarize();
            if (rh_summary.entry_count > 0) {
                klogger->info(
                    "{}ResidualHistory: tracking {} (sat,sig) entries "
                    "({} warmed up); σ across rows: median={:.4f} m, max={:.4f} m",
                    epoch_prefix, rh_summary.entry_count, rh_summary.warmed_up,
                    rh_summary.median_std, rh_summary.max_std);
            }
        }

        return { is_outlier_found, is_complete_reset };
    }

} // namespace GnssProcessor
