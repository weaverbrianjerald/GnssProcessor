#pragma once

/**
 * @file ISlipDetector.hpp
 * @brief Polymorphic contract shared by the registry-built cycle-slip detectors.
 *
 * Each detector (Melbourne-Wübbena, GRAPHIC, extra-wide-lane, the staged
 * geometry-free detector and the network-only ionosphere-free phase detector)
 * has its own per-epoch entry point (@c detectLargeMwResets, @c detect, ...)
 * and its own result struct (@c MwSlipResult, @c GraphicSlipResult, ...).
 *
 * @c ISlipDetector places one contract over them:
 *   - a single per-epoch entry point @c detect(SlipEpochContext) returning a
 *     vector of unified @c SlipEvent records, and
 *   - the lifecycle hooks the detectors share (@c resetSatellite,
 *     @c resetSatelliteSignal, @c setCorrections, @c kind, @c name).
 *
 * @c SlipDetectorRegistry constructs a config-selected set of detectors as
 * @c std::unique_ptr<ISlipDetector>, and @c CycleSlipManager drives them in
 * one loop.  The MW detector's detector-specific API (the WL-lock queries
 * @c AmbiguityResolver reads, the pivot-handover hints, the per-satellite
 * worst-deviation score the KalmanFilter's outlier tiebreak reads) is reached
 * through the typed down-cast @c asMelbourneWubbena and stays off the base
 * class.
 *
 * Implemented by @c MwSlipDetector, @c GraphicSlipDetector and
 * @c EwlSlipDetector.  @c GfSlipDetector and @c IfPhaseSlipDetector do not
 * derive from this interface.
 *
 * @par What is dispatched through the base (post-processing only)
 * @c CycleSlipManager calls @c detect (GRAPHIC and EWL only: the MW detector
 * is skipped by @c kind and driven through its concrete
 * @c detectLargeMwResets), @c resetSatellite, @c resetSatelliteSignal,
 * @c setCorrections, @c kind and @c asMelbourneWubbena; the registry calls
 * @c name.  Every method of the interface is dispatched by one of them.  The
 * hard-gap restart and the CSV trace are not part of the contract: no caller
 * dispatched them through the base, so card UOM step U5m-a removed the three
 * virtuals (@c resetAll, @c enableTracing, @c isTracingEnabled), and the MW
 * detector keeps @c resetAll and @c enableTracing as plain methods of its own,
 * called on the concrete type.  The real-time, baseline and network pipelines
 * hold concrete detector types and use none of this interface.  Whether the
 * interface stays or becomes a declared detector table is a decision owed by
 * the author (card UOM section 7 item 1).
 */

#include "common/PppTypes.hpp"
#include <Eigen/Core>
#include <string>
#include <vector>

namespace GnssProcessor {

class GnssCorrections;  // forward decl: only a pointer is held
class MwSlipDetector;   // forward decl: target of the typed down-cast accessor

/**
 * @brief Identifies the algorithm behind a slip detector.
 *
 * Used as the registry key, the @c SlipEvent provenance tag, and (through
 * @c toString) the token printed in the slip log lines.
 */
enum class SlipDetectorKind {
    MelbourneWubbena, ///< Wide-lane MW combination (primary; the registry always has a creator for it).
    Graphic,          ///< GRAPHIC (code+phase average) per-signal detector.
    ExtraWideLane,    ///< Extra-wide-lane MW for satellites with 3 or more frequencies.
    GeometryFree,     ///< 2nd-order time-differenced geometry-free (staged: not compiled, no registry creator).
    IonoFreePhase     ///< Between-satellite, between-epoch IF phase OMC; no registry creator (network.cycle_slip.if_phase_td).
};

/**
 * @brief Whether a @c SlipEvent concerns one signal or a band difference.
 *
 * GRAPHIC, GF and IF detectors flag a single carrier signal.  MW and EWL
 * detectors measure a between-band difference @f$\Delta N_{WL}=\Delta N_1 -
 * \Delta N_2@f$ and cannot attribute the jump to one signal on their own.
 * @c CycleSlipManager identifies the slipped signal by comparing the MW pairs
 * that slipped with the pairs that stayed clean (@c identifySlippedSignals).
 */
enum class SlipGranularity {
    PerSignal,      ///< @c sig1 is the slipped signal; @c sig2 is empty.
    BandDifference  ///< @c sig1 / @c sig2 are the (hi, lo) band pair of the combination.
};

/**
 * @brief Unified per-detection cycle-slip record.
 *
 * Each detector maps its own result struct into this record.  Detectors that
 * cannot size the slip (GF, IF) leave @c slip_known false; band-difference
 * detectors (MW, EWL) report the wide-lane integer in @c slip_cycles with
 * @c granularity == @c BandDifference and populate @c sig2.
 *
 * The only reader, @c CycleSlipManager::applyAuxiliarySlipEvents, uses
 * @c satellite, @c sig1, @c sig2, @c amb_key, @c granularity and @c source
 * (its response is a reset); @c slip_cycles, @c slip_known, @c fractional and
 * @c lambda are written by the detectors and read by no caller.
 */
struct SlipEvent {
    std::string satellite;       ///< Satellite identifier (e.g. "G01").
    std::string sig1;            ///< Slipped signal (PerSignal) or hi-band signal (BandDifference), e.g. "L1C".
    std::string sig2;            ///< Empty (PerSignal) or lo-band signal (BandDifference), e.g. "L2W".
    std::string amb_key;         ///< Ambiguity key to act on (e.g. "G01_L1C").
    int    slip_cycles{ 0 };     ///< Integer slip magnitude (WL cycles for BandDifference); 0 if unknown.
    bool   slip_known{ false };  ///< True iff @c slip_cycles is a reliable integer.
    double fractional{ 0.0 };    ///< |x - round(x)| before rounding (0 = exact, 0.5 = worst); detection-quality proxy.
    double lambda{ 0.0 };        ///< Relevant wavelength (m): per-signal λ, or WL/EWL λ for a band difference.
    SlipGranularity  granularity{ SlipGranularity::PerSignal }; ///< Single-signal vs band-difference.
    SlipDetectorKind source{ SlipDetectorKind::MelbourneWubbena }; ///< Detector that emitted this event.
};

/**
 * @brief Per-epoch inputs passed to every detector's @c detect().
 *
 * The bundle keeps @c ISlipDetector::detect to one signature although
 * detectors need different context.  The registered detectors (MW, GRAPHIC,
 * EWL) read only @c observations.  @c position_sigma_3d and
 * @c receiver_position were meant to carry what the ionosphere-free phase
 * detector needs to form and gate its double-differenced OMC; that detector
 * does not implement this interface, and no caller sets either field or any
 * detector reads it.  A detector ignores the fields it does not need.
 */
struct SlipEpochContext {
    const std::vector<PppObservation>& observations; ///< Modeled observations for this epoch.
    double position_sigma_3d{ 0.0 };                 ///< 3-D position std (m); 0 = unknown (IF detector gate).
    Eigen::Vector3d receiver_position{ Eigen::Vector3d::Zero() }; ///< Receiver ECEF (m) (IF detector).

    /// Convenience constructor for the common observations-only case.
    explicit SlipEpochContext(const std::vector<PppObservation>& obs) : observations(obs) {}
};

/**
 * @class ISlipDetector
 * @brief Abstract base that every registry-built cycle-slip detector implements.
 *
 * The contract is narrow on purpose: a per-epoch @c detect plus the shared
 * lifecycle hooks.  Detector-specific queries (WL locks, iono-stability
 * flags, jump magnitudes, quality scoring) are not on this interface; callers
 * that need them hold the concrete type, obtained through
 * @c asMelbourneWubbena or by constructing the detector directly.
 */
class ISlipDetector {
public:
    virtual ~ISlipDetector() = default;

    /**
     * @brief Detect cycle slips for one epoch.
     * @param context Bundled epoch inputs (observations + optional position info).
     * @return Unified slip events (empty if none).
     */
    virtual std::vector<SlipEvent> detect(const SlipEpochContext& context) = 0;

    /**
     * @brief Clear all arc state for a satellite (every signal/pair).
     * @param satellite Satellite identifier (e.g. "G01").
     */
    virtual void resetSatellite(const std::string& satellite) = 0;

    /**
     * @brief Clear arc state touching a specific (satellite, signal) only.
     *
     * The MW detector overrides this to clear the arcs that involve the
     * signal and keep unrelated pairs.  The default falls back to a full
     * per-satellite reset; GRAPHIC and EWL use the default.
     *
     * @param satellite Satellite identifier (e.g. "G01").
     * @param signal    Signal identifier (e.g. "L1C").
     */
    virtual void resetSatelliteSignal(const std::string& satellite,
                                      const std::string& signal) {
        (void)signal;
        resetSatellite(satellite);
    }

    /**
     * @brief Provide satellite bias corrections (BIA) for bias-aware detection.
     *
     * Default no-op for detectors that do not use external biases
     * (GRAPHIC, GF, IF); MW and EWL override.
     *
     * @param corrections Bias source (may be nullptr to disable); owned externally.
     */
    virtual void setCorrections(const GnssCorrections* corrections) { (void)corrections; }

    /// @brief Algorithm tag for this detector.
    virtual SlipDetectorKind kind() const = 0;

    /// @brief Human-readable detector name (for logs/traces).
    virtual std::string name() const = 0;

    /**
     * @brief Typed access to the MW detector when this instance is one.
     *
     * Returns @c this for @c MwSlipDetector and nullptr otherwise.
     * @c CycleSlipManager uses it to reach the MW detector's full API
     * (WL-lock queries, pivot hints, quality scoring) from a registry-built
     * @c ISlipDetector handle without RTTI.
     */
    virtual MwSlipDetector* asMelbourneWubbena() { return nullptr; }
};

} // namespace GnssProcessor
