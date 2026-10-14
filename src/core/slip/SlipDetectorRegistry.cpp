/**
 * @file SlipDetectorRegistry.cpp
 * @brief Factory implementation for cycle-slip detectors.
 *
 * Holds the kind→creator table (a Meyers singleton, mirroring
 * @c baselineFixturePresets) and the kind→token mapping.  The table registers
 * MW, GRAPHIC and EWL.  The geometry-free detector is staged (not compiled) and
 * the ionosphere-free-phase detector is built directly by the network path, so
 * neither has a creator; @c createEnabled warns and skips either if a config
 * selects it.
 */

#include "slip/SlipDetectorRegistry.hpp"
#include "slip/MwSlipDetector.hpp"
#include "slip/GraphicSlipDetector.hpp"
#include "slip/EwlSlipDetector.hpp"
#include <utility>

namespace GnssProcessor {

std::string toString(SlipDetectorKind kind) {
    switch (kind) {
        case SlipDetectorKind::MelbourneWubbena: return "mw";
        case SlipDetectorKind::Graphic:          return "graphic";
        case SlipDetectorKind::ExtraWideLane:    return "ewl";
        case SlipDetectorKind::GeometryFree:     return "gf";
        case SlipDetectorKind::IonoFreePhase:    return "if";
    }
    return "unknown";
}

const std::map<SlipDetectorKind, SlipDetectorRegistry::Creator>&
SlipDetectorRegistry::creators() {
    static const std::map<SlipDetectorKind, Creator> table = {
        { SlipDetectorKind::MelbourneWubbena,
          [](const GnssConfig& c, LogManager& l) -> std::unique_ptr<ISlipDetector> {
              return std::make_unique<MwSlipDetector>(c, l);
          } },
        { SlipDetectorKind::Graphic,
          [](const GnssConfig& c, LogManager& l) -> std::unique_ptr<ISlipDetector> {
              return std::make_unique<GraphicSlipDetector>(c, l);
          } },
        { SlipDetectorKind::ExtraWideLane,
          [](const GnssConfig& c, LogManager& l) -> std::unique_ptr<ISlipDetector> {
              return std::make_unique<EwlSlipDetector>(c, l);
          } },
        // No creator for GeometryFree: it is staged and its .cpp is excluded from
        // the build.  No creator for IonoFreePhase either: it is compiled, but the
        // network path instantiates it directly (ApplicationNetworkSetup, gated on
        // network.cycle_slip.if_phase_td).  createEnabled warns and skips both if a
        // config requests them.
    };
    return table;
}

std::unique_ptr<ISlipDetector> SlipDetectorRegistry::create(
    SlipDetectorKind kind, const GnssConfig& config, LogManager& log_manager) {
    const auto& table = creators();
    auto it = table.find(kind);
    if (it == table.end()) return nullptr;
    return it->second(config, log_manager);
}

std::vector<std::unique_ptr<ISlipDetector>> SlipDetectorRegistry::createEnabled(
    const GnssConfig& config, LogManager& log_manager) {
    auto log = log_manager.getLogger("cycle_slip_manager");
    const CycleSlipConfig& cfg = config.getCycleSlipConfig();

    // Fixed run order: the MW detector first (it is the primary and the source
    // of WL integers for AR / BSSD pivots), then the auxiliaries.
    const std::vector<std::pair<SlipDetectorKind, bool>> selection = {
        { SlipDetectorKind::MelbourneWubbena, cfg.mw },
        { SlipDetectorKind::Graphic,          cfg.graphic },
        { SlipDetectorKind::ExtraWideLane,    cfg.ewl },
        { SlipDetectorKind::GeometryFree,     cfg.gf },
        { SlipDetectorKind::IonoFreePhase,    cfg.ifp },
    };

    std::vector<std::unique_ptr<ISlipDetector>> detectors;
    for (const auto& [kind, enabled] : selection) {
        if (!enabled) continue;
        auto d = create(kind, config, log_manager);
        if (!d) {
            log->warn("Cycle-slip detector '{}' requested but unavailable in "
                      "this build (staged/uncompiled); skipping.",
                      toString(kind));
            continue;
        }
        log->info("Cycle-slip detector enabled: {}", d->name());
        detectors.push_back(std::move(d));
    }
    if (detectors.empty()) {
        log->warn("No cycle-slip detectors enabled — cycle-slip detection is OFF.");
    }
    return detectors;
}

} // namespace GnssProcessor
