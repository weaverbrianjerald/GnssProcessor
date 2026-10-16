/*!
 * \file StateManagerFamilies.cpp
 * \brief The run-start table "state families as armed".
 *
 * Owns StateManager::familiesAsArmed and StateManager::logFamiliesAsArmed: one line
 * per row of kStateFamilies (filter/StateFamily.hpp), stating whether this
 * configuration arms the family's birth, its label prefix and snapshot tag, the
 * source and value of its initial standard deviation, its process-noise rule and
 * value, and the events wired to it.  Every column is read from the family table
 * and the configuration; nothing here decides behaviour.
 */

#include "filter/StateManager.hpp"
#include "logging/LogManager.hpp"
#include <fmt/format.h>
#include <spdlog/spdlog.h>
#include <string>
#include <vector>

namespace GnssProcessor {

    namespace {

    /// Whether @p c arms the birth site of a family with gate @p gate.
    bool birthArmed(BirthGate gate, const GnssConfig& c) {
        // The baseline (DD) layout holds the position, the optional differential
        // troposphere and the DD ambiguities only (StateManager::layoutBaseline_), so no
        // other family is born there whatever the processing switches say.
        if (c.isBaselineMode() && gate != BirthGate::PhaseRows) return false;
        switch (gate) {
            case BirthGate::PhaseRows:
                return !(c.isBaselineMode() && c.getBaselineConfig().code_only);
            case BirthGate::Uncombined:               return c.isUncombinedEnabled();
            case BirthGate::UncombinedAndIonoRate:    return c.isUncombinedEnabled() && c.enableIonoRate();
            case BirthGate::UncombinedAndEstimateIfb: return c.isUncombinedEnabled() && c.estimateIfb();
            case BirthGate::EstimateSatBiases:        return c.estimateSatBiases();
            case BirthGate::NetworkSatPhaseBiases:    return c.isNetworkMode() && c.estimateSatPhaseBiases();
            case BirthGate::NetworkPhaseDatumEq34:    return c.isNetworkMode() && c.networkPhaseDatumEq34();
            case BirthGate::NetworkPhaseColour:       return c.getNetworkConfig().phase_colour_state;
            case BirthGate::NetworkSatClocks:         return c.isNetworkMode() && c.estimateSatClocks();
        }
        return false;
    }

    /// The configuration keys of a birth gate, as printed.
    const char* birthText(BirthGate gate) {
        switch (gate) {
            case BirthGate::PhaseRows:                return "every processed phase row";
            case BirthGate::Uncombined:               return "processing.uncombined_enabled";
            case BirthGate::UncombinedAndIonoRate:    return "uncombined_enabled and estimation.enable_iono_rate";
            case BirthGate::UncombinedAndEstimateIfb: return "uncombined_enabled and estimation.estimate_ifb";
            case BirthGate::EstimateSatBiases:        return "estimation.estimate_sat_biases";
            case BirthGate::NetworkSatPhaseBiases:    return "network producer and estimation.estimate_sat_phase_biases";
            case BirthGate::NetworkPhaseDatumEq34:    return "network producer and network.phase_datum_eq34";
            case BirthGate::NetworkPhaseColour:       return "network.phase_colour_state";
            case BirthGate::NetworkSatClocks:         return "network producer and estimation.estimate_sat_clocks";
        }
        return "?";
    }

    /// The initial standard deviation column of row @p s.
    std::string initialStdText(const StateFamilySpec& s, const GnssConfig& c) {
        switch (s.initial_std_rule) {
            case InitialStdRule::ConfigField:
                return fmt::format("filter.initial_std.{} = {:g}", s.initial_std_key,
                                   c.getInitialStd().*s.initial_std);
            case InitialStdRule::AmbiguityStd:
                return fmt::format("GnssConfig::getAmbiguityStd() = {:g} (no JSON key; a caller "
                                   "override wins)", c.getAmbiguityStd());
            case InitialStdRule::CallerSupplied:
                return "supplied by the creating code";
        }
        return "?";
    }

    /// Whether the process-noise rule of row @p s gives its states Q on this run.  The
    /// two ionosphere rules apply only in PPP_RTK mode (StateManager::addIonosphereQ_);
    /// every other rule applies whenever the family is present.
    bool processNoiseApplied(const StateFamilySpec& s, const GnssConfig& c) {
        if (s.q_rule == QRule::ObliquityRandomWalk || s.q_rule == QRule::CwnaWithLevel)
            return c.getProcessingMode() == ProcessingMode::PPP_RTK;
        return s.q_rule != QRule::None;
    }

    /// The process-noise column of row @p s.
    std::string processNoiseText(const StateFamilySpec& s, const GnssConfig& c) {
        if (s.q_rule == QRule::None) return "none";
        const bool mode_gated = s.q_rule != QRule::RandomWalk;
        const char* rule = s.q_rule == QRule::RandomWalk          ? "random walk"
                         : s.q_rule == QRule::ObliquityRandomWalk ? "obliquity random walk (PPP_RTK mode only)"
                         :                                          "CWNA with the level (PPP_RTK mode only)";
        return fmt::format("{}, filter.process_noise.{} = {:g}{}", rule, s.q_key,
                           c.getProcessNoise().*s.q_psd,
                           !mode_gated ? ""
                           : processNoiseApplied(s, c) ? ", applied on this run"
                                                       : ", NOT applied on this run");
    }

    /// The events column of row @p s.
    std::string eventsText(const StateFamilySpec& s) {
        std::string out;
        auto add = [&out](const std::string& e) { out += (out.empty() ? "" : ", ") + e; };
        if (s.reinflation != kNoReinflation)
            add(std::string("re-inflation ") + kReinflationNames[static_cast<std::size_t>(s.reinflation)]);
        if (s.has_reset) add("reset");
        add(s.prune_cache ? "prune (to the warm-restart cache)" : "prune");
        if (s.gap_decouple) add("gap decouple");
        if (s.q_rule != QRule::None && s.sat_of_key != SatOfKey::None) add("frozen-pivot Q skip");
        if (s.snapshot_tag) add("RT snapshot");
        return out;
    }

    } // anonymous namespace

    std::vector<std::string> StateManager::familiesAsArmed() const {
        std::vector<std::string> lines;
        lines.push_back("State families as armed (born: the configuration arms the family's "
                        "birth site; the core layout is logged above):");
        for (const StateFamilySpec& s : kStateFamilies) {
            lines.push_back(fmt::format(
                "  {:<21} born {} ({}); label {}; snapshot {}; initial std {}; Q {}; events: {}",
                std::string(s.name) + ":", birthArmed(s.birth, config_) ? "yes" : "no",
                birthText(s.birth), s.label_prefix, s.snapshot_tag ? s.snapshot_tag : "none",
                initialStdText(s, config_), processNoiseText(s, config_), eventsText(s)));
        }
        // A family the configuration creates whose process-noise rule does not apply
        // on this run: its states are observed and never random-walk.  Stated here so
        // a reader sees it at run start.
        for (const StateFamilySpec& s : kStateFamilies) {
            if (s.q_rule == QRule::None || !birthArmed(s.birth, config_)) continue;
            if (processNoiseApplied(s, config_)) continue;
            lines.push_back(fmt::format(
                "WARNING: {} states are born on this run ({}) but get no process noise: "
                "the rule applies in PPP_RTK mode only and this run's mode is not PPP_RTK",
                s.name, birthText(s.birth)));
        }
        return lines;
    }

    void StateManager::logFamiliesAsArmed() const {
        auto logger = log_manager_.getLogger("state_manager");
        for (const std::string& line : familiesAsArmed()) {
            if (line.rfind("WARNING: ", 0) == 0) logger->warn("{}", line.substr(9));
            else                                 logger->info("{}", line);
        }
    }

} // namespace GnssProcessor
