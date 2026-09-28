/**
 * @file AidingSourceRegistry.cpp
 * @brief Factory implementation for navigation aiding sources.
 *
 * Holds the kind->creator table (a Meyers singleton, mirroring
 * @c SlipDetectorRegistry) and the string<->kind mapping.  Only sources whose
 * .cpp is compiled into @c gnss_core are registered.  The staged Vision source
 * is absent on purpose: if a config selects it before it is finished and added
 * to CMakeLists.txt, @c createEnabled warns and skips it.
 *
 * @see docs/aiding_source_abstraction_design.md
 */

#include "aiding/AidingSourceRegistry.hpp"
#include "aiding/ImuAidingSource.hpp"
#include "aiding/ZuptAidingSource.hpp"
#include <cctype>
#include <utility>

namespace GnssProcessor {

std::string toString(AidingSourceKind kind) {
    switch (kind) {
        case AidingSourceKind::Imu:       return "imu";
        case AidingSourceKind::Zupt:      return "zupt";
        case AidingSourceKind::Nhc:       return "nhc";
        case AidingSourceKind::WheelOdom: return "wheel";
        case AidingSourceKind::Baro:      return "baro";
        case AidingSourceKind::Vision:    return "vision";
    }
    return "unknown";
}

std::optional<AidingSourceKind> aidingKindFromString(const std::string& token) {
    std::string t;
    t.reserve(token.size());
    for (char c : token)
        t.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));

    if (t == "imu" || t == "ins" || t == "inertial")
        return AidingSourceKind::Imu;
    if (t == "zupt" || t == "zero_velocity" || t == "zerovelocity")
        return AidingSourceKind::Zupt;
    if (t == "nhc" || t == "non_holonomic" || t == "nonholonomic")
        return AidingSourceKind::Nhc;
    if (t == "wheel" || t == "wheel_odom" || t == "odometer" || t == "odometry")
        return AidingSourceKind::WheelOdom;
    if (t == "baro" || t == "barometer" || t == "baro_height")
        return AidingSourceKind::Baro;
    if (t == "vision" || t == "vo" || t == "visual_odometry")
        return AidingSourceKind::Vision;
    return std::nullopt;
}

const std::map<AidingSourceKind, AidingSourceRegistry::Creator>&
AidingSourceRegistry::creators() {
    static const std::map<AidingSourceKind, Creator> table = {
        { AidingSourceKind::Imu,
          [](const GnssConfig& c, LogManager& l) -> std::unique_ptr<IAidingSource> {
              return std::make_unique<ImuAidingSource>(c, l);
          } },
        { AidingSourceKind::Zupt,
          [](const GnssConfig& c, LogManager& l) -> std::unique_ptr<IAidingSource> {
              return std::make_unique<ZuptAidingSource>(c, l);
          } },
        // Nhc / WheelOdom / Baro / Vision have no implementation yet and so no
        // creator; Vision is staged, with its .cpp excluded from the build.
        // createEnabled warns and skips any selected kind without a creator here.
    };
    return table;
}

std::unique_ptr<IAidingSource> AidingSourceRegistry::create(
    AidingSourceKind kind, const GnssConfig& config, LogManager& log_manager) {
    const auto& table = creators();
    auto it = table.find(kind);
    if (it == table.end()) return nullptr;
    return it->second(config, log_manager);
}

bool AidingSourceRegistry::isAvailable(AidingSourceKind kind) {
    return creators().count(kind) > 0;
}

std::vector<AidingSourceKind> AidingSourceRegistry::availableKinds() {
    std::vector<AidingSourceKind> kinds;
    kinds.reserve(creators().size());
    for (const auto& kv : creators()) kinds.push_back(kv.first);
    return kinds;
}

std::vector<std::unique_ptr<IAidingSource>> AidingSourceRegistry::createEnabled(
    const GnssConfig& config, LogManager& log_manager) {
    auto log = log_manager.getLogger("aiding_source_manager");
    const AidingConfig& cfg = config.getAidingConfig();

    // Fixed run order: the IMU first (it is the primary and owns the INS error
    // states the TC Jacobian and gap-bridging depend on), then the auxiliaries.
    const std::vector<std::pair<AidingSourceKind, bool>> selection = {
        { AidingSourceKind::Imu,       cfg.imu },
        { AidingSourceKind::Zupt,      cfg.zupt },
        { AidingSourceKind::Nhc,       cfg.nhc },
        { AidingSourceKind::WheelOdom, cfg.wheel },
        { AidingSourceKind::Baro,      cfg.baro },
        { AidingSourceKind::Vision,    cfg.vision },
    };

    std::vector<std::unique_ptr<IAidingSource>> sources;
    for (const auto& [kind, enabled] : selection) {
        if (!enabled) continue;
        auto s = create(kind, config, log_manager);
        if (!s) {
            log->warn("Aiding source '{}' requested but unavailable in this "
                      "build (staged/uncompiled); skipping.", toString(kind));
            continue;
        }
        log->info("Aiding source enabled: {}", s->name());
        sources.push_back(std::move(s));
    }
    return sources;
}

} // namespace GnssProcessor
