/**
 * @file SlipDetectorRegistry.cpp
 * @brief Factory implementation for cycle-slip detectors.
 *
 * Holds the kind→creator table (a Meyers singleton, mirroring
 * @c baselineFixturePresets) and the string↔kind mapping.  Only detectors
 * whose .cpp is compiled into @c gnss_core are registered; the staged
 * geometry-free and ionosphere-free-phase detectors are intentionally absent,
 * so @c createEnabled degrades gracefully (warn + skip) if a config selects
 * them before they are finished and added to CMakeLists.txt.
 */

#include "slip/SlipDetectorRegistry.hpp"
#include "slip/MwSlipDetector.hpp"
#include "slip/GraphicSlipDetector.hpp"
#include "slip/EwlSlipDetector.hpp"
#include <cctype>
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

std::optional<SlipDetectorKind> slipDetectorKindFromString(const std::string& token) {
    std::string t;
    t.reserve(token.size());
    for (char c : token)
        t.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));

    if (t == "mw" || t == "melbourne_wubbena" || t == "melbourne-wubbena" || t == "mw_wl")
        return SlipDetectorKind::MelbourneWubbena;
    if (t == "graphic")
        return SlipDetectorKind::Graphic;
    if (t == "ewl" || t == "extra_wide_lane" || t == "extrawidelane")
        return SlipDetectorKind::ExtraWideLane;
    if (t == "gf" || t == "geometry_free" || t == "geometryfree")
        return SlipDetectorKind::GeometryFree;
    if (t == "if" || t == "iono_free_phase" || t == "ionofreephase" || t == "ifphase")
        return SlipDetectorKind::IonoFreePhase;
    return std::nullopt;
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
        // GeometryFree is STAGED — its .cpp is excluded from the build, so no
        // creator is registered here.  IonoFreePhase IS compiled, but it is
        // instantiated directly on the network path (ApplicationNetworkSetup,
        // gated on network.cycle_slip.if_phase_td), not through this registry,
        // so no creator is registered for it here either.  createEnabled warns
        // and skips both if a config requests them.
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

bool SlipDetectorRegistry::isAvailable(SlipDetectorKind kind) {
    return creators().count(kind) > 0;
}

std::vector<SlipDetectorKind> SlipDetectorRegistry::availableKinds() {
    std::vector<SlipDetectorKind> kinds;
    kinds.reserve(creators().size());
    for (const auto& kv : creators()) kinds.push_back(kv.first);
    return kinds;
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
