#pragma once

/**
 * @file MwDetectorWork.hpp
 * @brief @internal The working values the MW slip detector's two drivers share with
 *        their steps.
 *
 * MwSlipDetector::detectLargeMwResets (one epoch) and MwSlipDetector::processMwSample
 * (one sample) are ordered drivers over named private steps (card UOM step U5m-d):
 *
 *   detectLargeMwResets  (MwSlipDetector.cpp; the steps in MwSlipDetectorEpoch.cpp)
 *     E0  beginEpoch_                 per-epoch resets, the LLI rotation, the epoch time
 *     E1  collectLaneSamples_         per satellite: the bridge fold, then its path:
 *           eraseArcsOnLli_             combined path under LLI: resets, erase the arcs
 *           collectCombinedLanes_       combined path: the IF lanes
 *           collectSupplementaryLanes_  combined path: band pairs the IF labels miss
 *           preserveArcsOnLli_          uncombined path under LLI: resets, keep the arcs
 *           collectUncombinedLanes_     uncombined path: the band-pair lanes
 *         A BIA-corrected lane joins its SD group; any other lane is processed
 *         undifferenced at once.
 *     E2  per SD group: electPivot_, reReferenceGroup_ (a changed pivot), processPivot_
 *         (the pivot's ZD slip anchors the frame), processNonPivots_ (SD samples,
 *         re-referenced to ZD)
 *     E3  endEpoch_                   hints consumed, cross-satellite tiers, trace flush
 *   emitLaneSlip_ forms the results of every flagged lane.
 *
 *   processMwSample  (MwSlipDetector.cpp; the steps in MwSlipDetectorSample.cpp)
 *     S1  stampArc_             in-house bias, gap reset, code stamps, snapshot, pair status
 *     S2  initArc_              the arc's first sample (and its init-suspicious check)
 *     S3  resolveTdConfirm_     this sample's TD1; a pending locked-arc TD confirmation
 *     S4  tryLock_              the statistical WL lock
 *     S5  tryInhouseCommit_     the in-house bias commit
 *     S6  selectStatistic_      the deviation and spread the gate tests
 *     S7  gate_                 the threshold, the verdicts, the decision line
 *     S8  recordEpochEvidence_  score cache, clean set, cross-satellite pool, trace row
 *     S9  onFlag_               a flagged sample: absorb, defer, or emit
 *     S10 foldSample_           the Welford/Hatch fold and the windows
 *     S11 checkDriftDemotion_   a locked arc's drift
 *     S12 checkLevelBackstop_   an unlocked arc's level disagreement
 *   The sample ends at the first step that flags a slip; the driver returns the verdict.
 *
 * Each step opens with an alias block binding the names its code had as locals or
 * parameters of the former single function (`const double lambda_wl = w.sample.lambda_wl;`),
 * so the bodies read as they did; scripts/uom_u5m_verify.py proves them against f823a36.
 * A step hands a result to later steps by writing its member.
 *
 * Included by MwSlipDetector.cpp, MwSlipDetectorEpoch.cpp and MwSlipDetectorSample.cpp.
 */

#include "slip/MwSlipDetector.hpp"
#include <spdlog/spdlog.h>
#include <limits>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace GnssProcessor {

    // -----------------------------------------------------------------
    // Intermediate storage for the BSSD two-pass MW approach.
    // Used for both combined (PPP) and uncombined (PPP-RTK) paths
    // whenever BIA satellite biases are available.  The internal
    // between-satellite differencing eliminates the receiver WL bias
    // and enables integer WL locking, regardless of the filter's
    // measurement model parameterization.
    //
    // Keyed by (constellation_char + "_" + phase1 + "_" + phase2).
    // -----------------------------------------------------------------
    struct MwSlipDetector::SatMwEntry {
        const PppObservation* obs;
        UncombinedPair pair;
        double mw_raw;          // raw MW in meters
        double mw_bia;          // MW after subtracting satellite BIA bias (meters)
        double sat_mw_bias;     // satellite BIA MW bias that was subtracted (meters)
        bool   has_bia;         // true if satellite BIA was available
        double lambda_wl;       // WL wavelength (meters)
        bool   is_combined;     // true if this came from the combined (PPP) path
        std::string if_phase_key; // IF phase label for combined reset keys (e.g. "L1C_L2W")
    };

    /**
     * @brief The working value of one @c MwSlipDetector::detectLargeMwResets call.
     *
     * Built by the driver, filled by the steps in order, discarded when the call returns.
     */
    struct MwSlipDetector::MwEpochWork {
        std::shared_ptr<spdlog::logger> logger;  ///< The "mw_slip_detector" logger.
        std::vector<MwSlipResult> results;       ///< The epoch's results, in emission order.
        // group_key -> satellite -> entry
        std::map<std::string, std::map<std::string, SatMwEntry>> sd_groups;  ///< E1 fills, E2 reads.
    };

    /**
     * @brief The working value of one @c MwSlipDetector::processMwSample call.
     *
     * The inputs, then the members each step sets, grouped by that step.  A member is
     * read only by the steps after the one that sets it.
     */
    struct MwSlipDetector::MwSampleWork {
        // --- the call's inputs ---------------------------------------------------------
        const PppObservation& obs;   ///< Observation (epoch time, elevation, tracing).
        const MwLane& lane;          ///< The lane.
        MwSample sample;             ///< The sample; a copy, since the in-house bias moves
                                     ///< @c mw_m and @c bia_corrected into the corrected frame.
        const MwSdFrame& frame;      ///< The single-difference frame.

        // --- S1 stampArc_ ----------------------------------------------------------------
        mw::MwArcStats* stats = nullptr;  ///< The arc (mw_stats_[lane.stats_key]); erased by
                                          ///< the step that emits a slip and erases the arc.
        double epoch_s = 0.0;        ///< Continuous GPS seconds of the sample.
        bool lli_shadowed = false;   ///< The first sample after an LLI on one of the bands.

        // --- S3 resolveTdConfirm_ --------------------------------------------------------
        double mw_diff = 0.0;        ///< TD1 = mw - prev_mw (re-based on an outlier excision).

        // --- S6 selectStatistic_ ---------------------------------------------------------
        double dev_hatch = 0.0;      ///< Deviation from the Hatch mean.
        double local_sigma_scaled = std::numeric_limits<double>::quiet_NaN();  ///< √2·MAD of the ring.
        double local_ref = std::numeric_limits<double>::quiet_NaN();           ///< Ring median.
        double td2 = std::numeric_limits<double>::quiet_NaN();                 ///< TD2, NaN until formed.
        bool use_local = false;      ///< The local-median reference governs.
        bool use_td = false;         ///< A time-difference reference governs.
        double dev = 0.0;             // deviation the gate tests
        double mode_sigma = 0.0;      // mature spread of the same statistic (NaN → warmup floors)
        int    detect_mode_code = 0;  // trace tag: 0=hatch, 1=local, 2=td1, 3=td2

        // --- S7 gate_ --------------------------------------------------------------------
        double thresh = 0.0;         ///< The adaptive threshold (meters).
        double sigma = 0.0;          ///< The TD1 MAD σ (meters; NaN while immature).
        bool is_slip = false;        ///< The active statistic's verdict.
        bool is_slip_level = false;  ///< The Hatch frame's verdict (guards the M2 admission).

        // --- the result ------------------------------------------------------------------
        MwVerdict verdict;           ///< Set by the step that flags.
    };

} // namespace GnssProcessor
