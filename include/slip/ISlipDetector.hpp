#pragma once

/**
 * @file ISlipDetector.hpp
 * @brief Polymorphic contract shared by every cycle-slip detector.
 *
 * Historically each detector (Melbourne-Wübbena, GRAPHIC, extra-wide-lane,
 * geometry-free, ionosphere-free phase) was a bespoke concrete class with its
 * own per-epoch entry-point name (@c detectLargeMwResets, @c detect, ...) and
 * its own result struct (@c MwSlipResult, @c GraphicSlipResult, ...).  Nothing
 * tied them together, so only the MW detector was ever wired in and the others
 * accreted as dead or staged code.
 *
 * @c ISlipDetector imposes one contract over all of them:
 *   - a single per-epoch entry point @c detect(SlipEpochContext) returning a
 *     vector of unified @c SlipEvent records, and
 *   - the lifecycle hooks every detector already shared verbatim
 *     (@c resetSatellite, @c enableTracing, @c setCorrections, ...).
 *
 * This lets @c SlipDetectorRegistry construct a config-selected set of
 * detectors as @c std::unique_ptr<ISlipDetector> and lets
 * @c CycleSlipManager drive them through one loop, while the
 * detector-specific richer API (e.g. the MW detector's WL-lock queries,
 * pivot-handover hints, and per-satellite quality scoring consumed by
 * @c AmbiguityResolver and the BSSD @c PivotManager) stays accessible via a
 * typed down-cast (@c asMelbourneWubbena) rather than polluting the base.
 */

#include "common/PppTypes.hpp"
#include <Eigen/Core>
#include <filesystem>
#include <string>
#include <vector>

namespace GnssProcessor {

class GnssCorrections;  // forward decl — only a pointer is held
class MwSlipDetector;   // forward decl — typed down-cast accessor target

/**
 * @brief Identifies the algorithm behind a slip detector.
 *
 * Used as the registry key, the @c SlipEvent provenance tag, and the
 * config-selection token (see @c slipDetectorKindFromString).
 */
enum class SlipDetectorKind {
    MelbourneWubbena, ///< Wide-lane MW combination (primary, always available).
    Graphic,          ///< GRAPHIC (code+phase average) per-signal detector.
    ExtraWideLane,    ///< Extra-wide-lane MW for 3+ frequency satellites.
    GeometryFree,     ///< 2nd-order time-differenced geometry-free (STAGED).
    IonoFreePhase     ///< Double-differenced ionosphere-free phase OMC (STAGED).
};

/**
 * @brief Whether a @c SlipEvent concerns one signal or a band difference.
 *
 * GRAPHIC / GF / IF detectors flag a single carrier signal.  MW / EWL
 * detectors measure a between-band difference @f$\Delta N_{WL}=\Delta N_1 -
 * \Delta N_2@f$ and cannot, on their own, attribute the jump to a single
 * signal — the cross-consistency analysis in @c CycleSlipManager resolves
 * the culprit by intersecting several @c BandDifference events.
 */
enum class SlipGranularity {
    PerSignal,      ///< @c sig1 is the slipped signal; @c sig2 is empty.
    BandDifference  ///< @c sig1 / @c sig2 are the (hi, lo) band pair of the combination.
};

/**
 * @brief Unified per-detection cycle-slip record.
 *
 * Superset of every legacy per-detector result struct.  Detectors that
 * cannot size the slip (GF, IF) leave @c slip_known false; band-difference
 * detectors (MW, EWL) report the wide-lane integer in @c slip_cycles with
 * @c granularity == @c BandDifference and populate @c sig2.
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
 * @brief Per-epoch inputs handed uniformly to every detector's @c detect().
 *
 * Bundling the inputs keeps @c ISlipDetector::detect single-signature even
 * though detectors need different context: most consume only
 * @c observations, while the ionosphere-free phase detector also needs the
 * current 3-D position uncertainty and the receiver ECEF to form and gate
 * its double-differenced OMC.  Fields a given detector does not need are
 * simply ignored.
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
 * @brief Abstract base every cycle-slip detector implements.
 *
 * The contract is deliberately narrow: a per-epoch @c detect plus the
 * lifecycle hooks the concrete detectors already shared.  Detector-specific
 * query surfaces (WL locks, iono-stability flags, jump magnitudes, quality
 * scoring) are intentionally kept off this interface — callers that need
 * them hold the concrete type (obtained via @c asMelbourneWubbena or by
 * constructing the detector directly).
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
     * Detectors that track per-pair arcs (MW/EWL) clear arcs involving the
     * signal while preserving unrelated pairs.  The default falls back to a
     * full per-satellite reset for detectors that lack finer granularity.
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
     * @brief Drop all per-arc state (hard-gap restart).
     *
     * Default no-op; detectors with cross-arc/pivot bookkeeping override.
     */
    virtual void resetAll() {}

    /**
     * @brief Enable CSV trace output for this detector.
     * @param log_dir Directory in which to create the trace file.
     * @return true if the trace file was opened.
     */
    virtual bool enableTracing(const std::filesystem::path& log_dir) = 0;

    /// @brief Whether tracing is currently active.
    virtual bool isTracingEnabled() const = 0;

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
     * Returns @c this for @c MwSlipDetector, nullptr otherwise.  Lets
     * @c CycleSlipManager recover the MW detector's rich API (WL-lock
     * queries, pivot hints, quality scoring) from a registry-built
     * @c ISlipDetector handle without RTTI.
     */
    virtual MwSlipDetector* asMelbourneWubbena() { return nullptr; }
};

} // namespace GnssProcessor
