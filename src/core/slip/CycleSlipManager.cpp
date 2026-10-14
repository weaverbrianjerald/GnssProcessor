/**
* @file CycleSlipManager.cpp
* @brief Cycle slip detection, repair and reset coordinator.
*
* Coordinates MW slip detection with multi-frequency cross-consistency
* analysis, the optional TurboEdit MW+GF joint repair and soft-LLI bridge,
* the auxiliary detectors from the registry, and BSSD-aware ambiguity
* repairs and resets (including pivot handling).
*
* This file holds the construction, the public API, the per-epoch driver
* @c detectAndRepair with its first and last sections (S0, S17), and the
* soft-LLI delegation.  The other sections live by concern:
*   - CycleSlipDetection.cpp     S1 to S8: detectors run, TurboEdit and the
*                                soft-LLI bridge resolve, MW results grouped
*   - CycleSlipAdjudication.cpp  S11 and S14: per-satellite culprit analysis
*                                and the action taken on it
*   - CycleSlipPivot.cpp         S9, S12, S13: the BSSD pivot frame
*   - CycleSlipNonMwPasses.cpp   S15, S16: TurboEdit GF-only and auxiliary
*                                detector resets
*   - CycleSlipRepair.cpp        the reset and repair primitives
* The working sets the sections share are the members of @c SlipEpochWork
* (slip/CycleSlipEpochWork.hpp).
*/

#include "slip/CycleSlipManager.hpp"
#include "slip/CycleSlipEpochWork.hpp"
#include "slip/SlipDetectorRegistry.hpp"
#include "math/TimeUtils.hpp"

namespace GnssProcessor {

// Helper: extract satellite ID from an ambiguity key "G01_L1C" -> "G01"
std::string CycleSlipManager::satIdFromAmbiguityKey(const std::string& key) {
    auto pos = key.find('_');
    return (pos != std::string::npos) ? key.substr(0, pos) : key;
}

// Helper: extract signal from an ambiguity key "G01_L1C" -> "L1C"
std::string CycleSlipManager::signalFromAmbiguityKey(const std::string& key) {
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
      soft_lli_(config, log_manager) {
    // Build the config-selected detector set (MW-first, then auxiliaries).
    detectors_ = SlipDetectorRegistry::createEnabled(config, log_manager);
    // Keep a typed handle to the MW detector for its wider API (WL locks,
    // quality scoring, pivot hints); nullptr when MW is disabled in config.
    for (const auto& d : detectors_) {
        if (auto* mw = d->asMelbourneWubbena()) { mw_ = mw; break; }
    }

    // TurboEdit MW+GF joint repair: requires the MW detector (its source of
    // wide-lane integers).  Build the resolver only when both the toggle and
    // the MW detector are active; otherwise warn and stay in the MW-only path.
    const auto& cs = config.getCycleSlipConfig();
    if (cs.turbo_edit) {
        if (mw_) {
            turbo_ = std::make_unique<TurboEditResolver>(config, log_manager);
            log_manager.getLogger("cycle_slip_manager")->info(
                "TurboEdit MW+GF joint cycle-slip repair ENABLED (high-rate only).");
        } else {
            log_manager.getLogger("cycle_slip_manager")->warn(
                "turbo_edit requested but the MW detector (estimation.cycle_slip.mw) "
                "is disabled — TurboEdit needs the MW wide-lane integers; staying "
                "MW-only (TurboEdit OFF).");
        }
    }

    // Run-end counters: one detection row per built detector, in run order.
    for (const auto& d : detectors_)
        counters_.detections.emplace_back(toString(d->kind()), 0);
}

CycleSlipManager::~CycleSlipManager() {
    try {
        logRunEndCounters();
    } catch (...) {
        // A run-end report must not throw out of a destructor.
    }
}

// ===========================================================================
// Public API
// ===========================================================================

std::map<std::string, int> CycleSlipManager::getResolvedWlAmbiguities() const {
    return mw_ ? mw_->getResolvedWlAmbiguities() : std::map<std::string, int>{};
}

std::map<std::string, double> CycleSlipManager::getResolvedWlFractions() const {
    return mw_ ? mw_->getResolvedWlFractions() : std::map<std::string, double>{};
}

std::map<std::string, MwSlipDetector::WlLockQuality>
CycleSlipManager::getResolvedWlQuality() const {
    return mw_ ? mw_->getResolvedWlQuality()
               : std::map<std::string, MwSlipDetector::WlLockQuality>{};
}

void CycleSlipManager::resetSatellite(const std::string& satellite) {
    // Reset the arc state in every detector so an externally-driven reset
    // (e.g. the KF outlier handler) clears all of them, not just MW.
    for (const auto& d : detectors_) d->resetSatellite(satellite);
    // The TurboEdit resolver holds its own GF jump arcs (it is not an
    // ISlipDetector in detectors_), so clear them too; a stale pre-reset GF
    // value would otherwise enter the next time-difference.
    if (turbo_) turbo_->resetSatellite(satellite);
}

void CycleSlipManager::resetSatelliteSignal(const std::string& satellite,
                                              const std::string& signal) {
    for (const auto& d : detectors_) d->resetSatelliteSignal(satellite, signal);
    if (turbo_) turbo_->resetSatelliteSignal(satellite, signal);
}

void CycleSlipManager::setCorrections(const GnssCorrections* corrections) {
    // MW and EWL consume biases; the others ignore it via the default no-op.
    for (const auto& d : detectors_) d->setCorrections(corrections);
    if (turbo_) turbo_->setCorrections(corrections);
}

bool CycleSlipManager::enableMwTracing(const std::filesystem::path& log_dir) {
    return mw_ ? mw_->enableTracing(log_dir) : false;
}

bool CycleSlipManager::enableTurboEditTracing(const std::filesystem::path& log_dir) {
    return turbo_ ? turbo_->enableTracing(log_dir) : false;
}

void CycleSlipManager::setSampleInterval(double interval_s) {
    sample_interval_s_ = interval_s;
    soft_lli_.setSampleInterval(interval_s);
    // Tighten the TurboEdit GF-arc gap tolerance to ~1.5× the cadence so a
    // brief outage restarts the arc rather than absorbing a slip-during-gap
    // into a stale GF reference (mirrors the MW detector's gap handling).
    if (turbo_ && interval_s > 0.0)
        turbo_->setMaxGapSeconds(interval_s * 1.5);
}

// ===========================================================================
// Pre-estimation: MW detection and reset
// ===========================================================================

CycleSlipEpochResult CycleSlipManager::detectAndRepair(
    const std::string& epoch_prefix,
    const std::vector<PppObservation>& modeled_obs,
    StateManager& state_manager,
    const MeasurementModel& meas_model) {

    SlipEpochWork w(epoch_prefix, modeled_obs, state_manager, meas_model,
                    log_manager_.getLogger("cycle_slip_manager"));

    beginSlipEpoch(w);                          // S0  accumulators, epoch time, pivot predicate
    registerSoftLliGapHolds(w);                 // S1  gap holds (turbo_edit_bridge_lli)
    runAuxiliaryDetectors(w);                   // S2  GRAPHIC / EWL detect
    detectMwSlips(w);                           // S4  MW / LLI detection
    resolveTurboEdit(w);                        // S5  TurboEdit MW+GF resolution
    resolveSoftLliHolds(w);                     // S6  soft-LLI restore / reset / timeout
    groupMwPairsBySatellite(w);                 // S7  slipped and clean MW pairs per satellite

    if (!w.slip_results.empty()) {
        collectMwMandates(w);                   // S8  mandated keys, LLI satellites
        scanPivotCulprits(w);                   // S9  pivot-affected (constellation, signal)

        // Process each satellite once.
        for (auto& [sat_id, amb_keys] : w.reset_keys_by_sat) {
            handleSatelliteMandates(w, sat_id, amb_keys);  // S11
        }

        repairAcrossSlippedPivotInPlace(w);     // S12 pivot frame repaired in place
        forcePivotHandoverAndApplyDeferred(w);  // S13 forced handover, deferred repairs
        logMwEpochSummary(w);                   // S14 per-epoch MW warn line
    }

    applyTurboEditGfOnlyResolutions(w);         // S15 TurboEdit GF-only (MW blind spot)
    applyAuxiliarySlipEvents(w);                // S16 auxiliary-detector resets
    tallySlipEpoch(w);                          //     run-end counters
    return finishSlipEpoch(w);                  // S17 soft-LLI marker, smoother accumulators
}

// ===========================================================================
// S0 and S17: the epoch frame
// ===========================================================================

void CycleSlipManager::beginSlipEpoch(SlipEpochWork& w) {
    const auto& modeled_obs = w.modeled_obs;
    const auto& meas_model  = w.meas_model;
    auto& epoch_now_s   = w.epoch_now_s;
    auto& is_bssd_pivot = w.is_bssd_pivot;

    last_mw_slips_.clear();
    // Per-epoch smoother-action accumulators (see CycleSlipEpochResult docs).
    epoch_reset_sats_.clear();
    epoch_repairs_.clear();

    // Continuous GPS seconds of this epoch (for soft-LLI hold bookkeeping).
    for (const auto& obs : modeled_obs)
        if (!obs.satellite.empty()) {
            epoch_now_s = TimeUtils::continuousGpsSeconds(obs.time);
            break;
        }

    // BSSD pivot predicate, shared by the gap-hold pre-pass and the
    // WL-settle pending-hold registration below.  A pivot's SD ambiguity is
    // frozen at 0, so holds and in-frame integer repairs do not apply to it.
    is_bssd_pivot =
        [&](const std::string& sat) -> bool {
            if (!config_.enableBetweenSatelliteSd()) return false;
            const auto& pivots = meas_model.getBssdPivotSatellites();
            const auto pit = pivots.find(PppUtils::getSystemString(sat));
            return pit != pivots.end() && pit->second == sat;
        };
}

void CycleSlipManager::tallySlipEpoch(const SlipEpochWork& w) {
    ++counters_.epochs;
    for (auto& [token, count] : counters_.detections) {
        if (token == toString(SlipDetectorKind::MelbourneWubbena))
            count += static_cast<long long>(w.slip_results.size());
        for (const auto& ev : w.aux_events)
            if (toString(ev.source) == token) ++count;
    }
    for (const auto& [sat, keys] : w.reset_keys_by_sat)
        counters_.mw_mandates += static_cast<long long>(keys.size());
    counters_.soft_lli_bridge_repairs += static_cast<long long>(w.soft_lli_repaired.size());
    counters_.integer_repairs += static_cast<long long>(epoch_repairs_.size());
}

void CycleSlipManager::logRunEndCounters() const {
    if (counters_.epochs == 0) return;
    auto log = log_manager_.getLogger("cycle_slip_manager");
    for (const auto& line : cycleSlipCounterLines(counters_))
        log->info("{}", line);
}

CycleSlipEpochResult CycleSlipManager::finishSlipEpoch(SlipEpochWork& w) {
    auto& result = w.result;
    const auto& epoch_now_s = w.epoch_now_s;

    // Advance the previous-epoch marker for the soft-LLI measured-spacing gate.
    soft_lli_.advanceEpoch(epoch_now_s);

    // Hand the slip-phase frame actions to the smoother via the result: reset
    // sats are segmented, covariance-preserving repairs are compensated into
    // the stored a-priori (audit F1).  Accumulated across the MW loop, the
    // GF-only pass, the aux pass, and the helper functions.
    result.reset_satellites = epoch_reset_sats_;
    result.ambiguity_repairs = epoch_repairs_;

    return result;
}

// ===========================================================================
// Soft-LLI bridge (estimation.cycle_slip.turbo_edit_bridge_lli): delegation
// to the shared SoftLliBridge (the real-time path uses the same bridge).
// ===========================================================================

void CycleSlipManager::recordAmbiguitySnapshot(const StateManager& state_manager,
                                              double epoch_now_s) {
    soft_lli_.recordSnapshot(state_manager, epoch_now_s);
}

std::set<std::string> CycleSlipManager::getHeldPendingBridgeKeys() const {
    return soft_lli_.heldKeys();
}

void CycleSlipManager::resetSoftLliBridge() {
    soft_lli_.reset();
}

} // namespace GnssProcessor
