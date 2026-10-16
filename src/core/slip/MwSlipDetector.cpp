/**
 * @file MwSlipDetector.cpp
 * @brief The MW slip detector: construction and its two drivers.
 *
 * The class is declared in slip/MwSlipDetector.hpp, whose banner lists the files its
 * bodies live in.  This file holds what a reader follows first: the constructor (every
 * knob the detector reads, logged once), the per-epoch driver detectLargeMwResets and
 * the per-sample driver processMwSample, and the ISlipDetector projection detect.
 *
 * Part of MwSlipDetector (slip/MwSlipDetector.hpp lists every file of the class).
 */

#include "slip/MwSlipDetector.hpp"
#include "slip/MwDetectorWork.hpp"
#include "math/TimeUtils.hpp"
#include <algorithm>
#include <cmath>
#include <limits>
#include <optional>
#include <set>
#include <string>

namespace GnssProcessor {

    using namespace mw;  // the constants table and the per-arc state (slip/MwArcState.hpp)

    // =================================================================
    // Construction
    // =================================================================

    MwSlipDetector::MwSlipDetector(const GnssConfig& config, LogManager& log_manager)
        : config_(config),
          log_manager_(log_manager),
          ppp_utils_(log_manager),
          init_frac_threshold_(config.getMwInitFracThreshold()),
          cross_sat_ewma_alpha_(config.getMwCrossSatEwmaAlpha()),
          sigma_multiplier_(config.getCycleSlipConfig().mw_sigma_multiplier),
          detect_reference_(config.getCycleSlipConfig().mw_detect_reference),
          td_order_(config.getCycleSlipConfig().mw_td_order),
          local_window_len_(std::clamp<size_t>(
              static_cast<size_t>(std::max(config.getCycleSlipConfig().mw_local_window, 1)),
              kMinEpochsForAdaptive, kMwLocalWindowCap)) {
        auto freqLabel = [](FrequencyMode m) {
            switch (m) {
            case FrequencyMode::Triple: return "triple";
            case FrequencyMode::All:    return "all";
            case FrequencyMode::Dual:
            default:                    return "dual";
            }
        };
        log_manager_.getLogger("mw_slip_detector")->info(
            "MwSlipDetector initialized (adaptive MAD-based, k={:.1f}, sigma_floor={:.2f} m, "
            "init_frac_thresh={:.2f} WL, min_epochs={}, "
            "wl_lock=adaptive(3σ), gap_tol={:.1f}s, mw_freq={}, mw_chan={}, inhouse_bias={}, "
            "xsat_ewma_α={:.2f}, detect_ref={}, td_order={}, local_window={})",
            sigma_multiplier_, kMinSigmaMeters,
            init_frac_threshold_,
            kMinEpochsForAdaptive, max_gap_seconds_,
            freqLabel(config_.getResolvedMwFrequencyMode()),
            config_.getResolvedMwChannelMode() == ChannelMode::All ? "all" : "single",
            config_.getInhouseBiasEstimation() ? "enabled" : "disabled",
            cross_sat_ewma_alpha_,
            detect_reference_ == MwDetectReference::LocalMedian    ? "local_median"
          : detect_reference_ == MwDetectReference::TimeDifference ? "time_difference"
                                                                   : "hatch_mean",
            td_order_ == MwTdOrder::Second ? "second" : "first",
            local_window_len_);
    }

    void MwSlipDetector::setMaxGapSeconds(double seconds) {
        if (seconds > 0.0) {
            max_gap_seconds_ = seconds;
            log_manager_.getLogger("mw_slip_detector")->info(
                "MW gap tolerance set to {:.1f}s (was default {:.1f}s)",
                seconds, kDefaultMaxGapSeconds);
        }
    }

    // =================================================================
    // ISlipDetector contract
    // =================================================================

    std::vector<SlipEvent> MwSlipDetector::detect(const SlipEpochContext& context) {
        const std::vector<MwSlipResult> raw = detectLargeMwResets(context.observations);
        std::vector<SlipEvent> events;
        events.reserve(raw.size());
        for (const auto& r : raw) {
            SlipEvent ev;
            ev.satellite   = r.satellite;
            ev.sig1        = r.sig1;
            ev.sig2        = r.sig2;
            ev.amb_key     = r.amb_key;
            ev.slip_cycles = r.slip_wl_cycles;
            ev.slip_known  = r.slip_wl_known;
            ev.fractional  = r.slip_frac;
            ev.lambda      = r.lambda_wl;
            ev.granularity = r.sig2.empty() ? SlipGranularity::PerSignal
                                            : SlipGranularity::BandDifference;
            ev.source      = SlipDetectorKind::MelbourneWubbena;
            events.push_back(std::move(ev));
        }
        return events;
    }

    std::optional<MwSlipDetector::MwVerdict> MwSlipDetector::processMwSample(
        const PppObservation& obs, const MwLane& lane, const MwSample& sample,
        const MwSdFrame& frame) {
        // One MW sample against its arc, as ordered steps over one working value
        // (slip/MwDetectorWork.hpp lists them; the bodies are in
        // MwSlipDetectorSample.cpp).  The sample ends at the first step that flags a
        // slip, and the verdict is returned only then.
        MwSampleWork w{ obs, lane, sample, frame };
        stampArc_(w);                                    // S1
        if (w.stats->count == 0) {                       // S2 the arc's first sample
            if (initArc_(w)) return w.verdict;
            return std::nullopt;
        }
        if (resolveTdConfirm_(w)) return w.verdict;      // S3
        tryLock_(w);                                     // S4
        tryInhouseCommit_(w);                            // S5
        selectStatistic_(w);                             // S6
        gate_(w);                                        // S7
        recordEpochEvidence_(w);                         // S8
        if (onFlag_(w)) return w.verdict;                // S9
        foldSample_(w);                                  // S10
        if (checkDriftDemotion_(w)) return w.verdict;    // S11
        if (checkLevelBackstop_(w)) return w.verdict;    // S12
        return std::nullopt;
    }

    // =================================================================
    // Core detection logic
    // =================================================================

    std::vector<MwSlipResult> MwSlipDetector::detectLargeMwResets(
        const std::vector<PppObservation>& observations) {
        // One epoch as ordered steps over one working value (slip/MwDetectorWork.hpp
        // lists them; the bodies are in MwSlipDetectorEpoch.cpp): the per-epoch resets,
        // pass 1 per satellite, pass 2 per SD group, the epoch's end.
        MwEpochWork w{ log_manager_.getLogger("mw_slip_detector") };
        beginEpoch_(w, observations);                    // E0
        for (const auto& obs : observations) {
            if (obs.satellite.empty()) continue;
            if (!std::isfinite(obs.elevation) || obs.elevation < config_.getElevationMaskDeg()) continue;
            collectLaneSamples_(w, obs);                 // E1 pass 1, per satellite
        }
        // -----------------------------------------------------------------
        // Pass 2: BSSD single-difference MW processing
        //
        // For each (constellation, band-pair) group that has BIA-corrected
        // MW values, select a pivot (highest elevation) and form SD values.
        // The SD eliminates the receiver WL bias, making the result near-
        // integer in WL cycles when combined with the satellite BIA correction.
        //
        // This runs for both combined (PPP) and uncombined (PPP-RTK) paths.
        // The MW observable is always formed from raw RINEX signals and is
        // independent of the filter's measurement model parameterization.
        // -----------------------------------------------------------------
        for (auto& [group_key, sat_entries] : w.sd_groups) {
            if (sat_entries.empty()) continue;
            const std::string best_pivot = electPivot_(sat_entries);  // E2 pass 2, per group
            if (best_pivot.empty()) continue;
            reReferenceGroup_(w, group_key, sat_entries, best_pivot);
            sd_pivot_sat_[group_key] = best_pivot;
            const MwSdFrame frame = processPivot_(w, sat_entries, best_pivot);
            processNonPivots_(w, sat_entries, best_pivot, frame);
        }
        return endEpoch_(w);                             // E3
    }

} // namespace GnssProcessor
