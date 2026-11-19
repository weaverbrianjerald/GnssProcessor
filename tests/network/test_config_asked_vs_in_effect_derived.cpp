/**
 * @file test_config_asked_vs_in_effect_derived.cpp
 * @brief What the field reference configuration asks against what the user-engine
 *        configuration derived for the held-out station holds.
 *
 * A network run with the closed loop builds the held-out station's user-engine configuration as
 * a copy of the file's configuration passed through deriveClosedLoopConsumerConfig (which runs
 * the shared applyNetworkConsumerCouplings).  The copy is rewritten member by member, and the
 * derivation has no logger, so a request the derivation replaces does not show in the run's log.
 *
 * Each "asked" case loads config/golden/golden_field_refound.json with one key set to a value
 * other than the one the derivation writes, derives the user configuration, and accepts either
 * outcome a reader can see: the file's value stands, or a line at warn level or above, written
 * while the file is loaded or the configuration derived, names the key and the held-out station
 * (the words consumer, held-out or closed loop).  The replacements are deliberate: the code
 * comments of the derivation give the reason for each (producer levers pinned off the station,
 * the user defaults restored on a single receiver, harness keys carrying their own tuning, the
 * bias age gate armed under consumption), and the effective values show in the consumer
 * engine's config echo, which these tests do not capture.  A replacement without a warn line is
 * therefore kept as a skip that records a design pin, not a defect: it names the key, what was
 * asked and what the derived configuration holds, and passes on its own once the derivation
 * announces the replacement or lets the request stand.  One case does the same for the
 * standalone real-time profile (applyNetworkConsumerProfile), which runs the shared couplings
 * on a user's own configuration.  The signal-rule pin is not compared between the two paths:
 * it sits in the closed-loop-only part of the derivation, because the derived configuration is
 * a copy of the producer's, while the standalone profile leaves the rule to the user.
 *
 * The positive cases pin what must stand: an explicit false of the bias age gate, the AR
 * constellation list when the harness gives none, processing switches the harness keys never
 * clear, and the producer-side network levers pinned off the station while the producer keeps
 * them.  The last case lists every member the derivation changes on the unmodified reference
 * configuration, compared through the public getters against the plain copy, so a new change
 * of a listed member fails it.
 *
 * Runtime: the reference configuration is parsed once per process, and its station registry
 * (parsed whole by every load under ocean tide loading from the registry, about 13 s of a load in
 * Debug) is replaced by copies cut to the roster (tests/support/ReferenceConfigCache.hpp).  A load
 * then differs from a load of the committed file only in getStationInfoPaths and the station count
 * of one info line; neither is read here, and neither writes a line at warn level.
 */
#include <gtest/gtest.h>

#include "config/GnssConfig.hpp"
#include "logging/LogManager.hpp"
#include "../support/ReferenceConfigCache.hpp"  // the reference parsed once, registry cut to the roster
#include "../support/TestTempDir.hpp"   // per-process, per-test scratch directory

#include <nlohmann/json.hpp>
#include <spdlog/sinks/ostream_sink.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <set>
#include <sstream>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

using namespace GnssProcessor;
namespace fs = std::filesystem;

namespace {

using Edit = std::function<void(nlohmann::json&)>;

/// The reference configuration loaded once with an edit, its derived user configuration and
/// the standalone profile applied to a copy, with the warn-level lines of each phase.
struct Derived {
    bool ok{ false };
    GnssConfig plain;                              ///< the configuration as loaded
    std::unique_ptr<GnssConfig> derived;           ///< deriveClosedLoopConsumerConfig()
    std::unique_ptr<GnssConfig> profiled;          ///< a copy after applyNetworkConsumerProfile
    std::vector<std::string> derive_warns;         ///< warn+ lines of the load and the derivation
    std::vector<std::string> profile_warns;        ///< warn+ lines of the profile
};

std::vector<std::string> splitLines(const std::string& text) {
    std::vector<std::string> out;
    std::istringstream in(text);
    std::string line;
    while (std::getline(in, line))
        if (!line.empty()) out.push_back(line);
    return out;
}

void detachSink(const std::shared_ptr<spdlog::logger>& l,
                const std::shared_ptr<spdlog::sinks::sink>& s) {
    if (!l) return;
    auto& v = l->sinks();
    v.erase(std::remove(v.begin(), v.end(), s), v.end());
}

/// Loads the field reference configuration with @p edit applied, derives the held-out station's
/// configuration and applies the standalone profile to a copy, capturing every line at warn
/// level or above that the "gnss_config" and main loggers write in each phase.
std::unique_ptr<Derived> loadAndDerive(const std::string& tag, const Edit& edit) {
    auto out = std::make_unique<Derived>();
    const fs::path src = fs::path(GNSS_PROJECT_ROOT) / "config" / "golden" /
                         "golden_field_refound.json";
    if (!fs::exists(src)) return out;
    // Parsed once per process, with the station registry cut to the roster.
    nlohmann::json j = gnss_test::referenceConfigWithRosterStationInfo("golden/golden_field_refound.json");
    edit(j);

    const fs::path dir = gnss_test::tempDir() / ("derived_" + tag);
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir, ec);
    const fs::path cfg_path = dir / "config.json";
    { std::ofstream f(cfg_path); f << j.dump(2); }

    // The stream and its sink outlive the log manager (declared first), and the sink is
    // detached before the loggers are dropped.
    std::ostringstream oss;
    const auto sink = std::make_shared<spdlog::sinks::ostream_sink_mt>(oss);
    sink->set_level(spdlog::level::warn);
    sink->set_pattern("%v");

    // Only the names this helper creates are dropped (never drop_all()).
    spdlog::drop("gnss_processor");
    spdlog::drop("gnss_config");
    {
        LogManager log;
        log.initialize(dir, LogLevel::Info);
        const auto cfg_log  = log.getLogger("gnss_config");
        const auto main_log = log.getLogger();
        cfg_log->sinks().push_back(sink);
        main_log->sinks().push_back(sink);

        out->ok = out->plain.loadFromJson(cfg_path, log);
        if (out->ok)
            out->derived = std::make_unique<GnssConfig>(out->plain.deriveClosedLoopConsumerConfig());
        cfg_log->flush();
        main_log->flush();
        out->derive_warns = splitLines(oss.str());
        oss.str("");
        oss.clear();

        if (out->ok) {
            out->profiled = std::make_unique<GnssConfig>(out->plain);
            out->profiled->applyNetworkConsumerProfile(log);
        }
        cfg_log->flush();
        main_log->flush();
        out->profile_warns = splitLines(oss.str());

        detachSink(cfg_log, sink);
        detachSink(main_log, sink);
    }
    spdlog::drop("gnss_processor");
    spdlog::drop("gnss_config");
    fs::remove_all(dir, ec);
    return out;
}

bool textHas(const std::string& hay, const std::string& needle) {
    return hay.find(needle) != std::string::npos;
}

/// True when one of @p lines names @p key and the held-out station.
bool reportsPin(const std::vector<std::string>& lines, const std::string& key) {
    for (const std::string& l : lines) {
        if (!textHas(l, key)) continue;
        if (textHas(l, "consumer") || textHas(l, "held-out") || textHas(l, "closed-loop")
            || textHas(l, "closed_loop") || textHas(l, "closed loop"))
            return true;
    }
    return false;
}

/// Where the value in effect of a derivation pin can be read when no warn line names it.
const char* const kDerivationEcho =
    "the derivation has no logger; the value in effect shows in the consumer engine's config echo";

/// Empty when the request stands or a warn line reports the replacement; otherwise the skip
/// message recording a design pin: the key, the request, the value in effect, and where a
/// reader finds that value.  The replacement is deliberate (the code comments give the reason),
/// so the message records a pin without a warn line, not a defect.
std::string silentPin(bool stands, const std::vector<std::string>& lines, const std::string& key,
                      const std::string& asked, const std::string& held,
                      const std::string& where = "the derived configuration of the held-out station",
                      const std::string& echo = kDerivationEcho) {
    if (stands || reportsPin(lines, key)) return {};
    return "design pin, no warn line: " + key + " asked " + asked + ", " + where + " holds " +
           held + "; the replacement is deliberate and documented in the code comments, and " +
           echo;
}

std::string asText(bool b) { return b ? "true" : "false"; }
std::string asText(double d) {
    std::ostringstream o;
    o.precision(17);
    o << d;
    return o.str();
}
std::string asText(int i) { return std::to_string(i); }
std::string asText(const std::string& s) { return "\"" + s + "\""; }
std::string asText(const std::vector<std::string>& v) {
    std::string s = "[";
    for (std::size_t i = 0; i < v.size(); ++i) s += (i ? ", " : "") + v[i];
    return s + "]";
}
std::string asText(CanonicalSignalRule r) {
    return r == CanonicalSignalRule::Priority ? "\"priority\"" : "\"count\"";
}
std::string asText(ChannelMode m) { return m == ChannelMode::All ? "\"all\"" : "\"single\""; }
std::string asText(ProcessingMode m) { return "mode#" + std::to_string(static_cast<int>(m)); }

const Edit kNoEdit = [](nlohmann::json&) {};

// ---- Requests of the user engine replaced by the derivation -------------------------------

TEST(ConfigAskedVsInEffectDerived, SignalRuleAskedPriority) {
    // The reference configuration asks the priority rule of every engine.
    const auto r = loadAndDerive("rule", kNoEdit);
    ASSERT_TRUE(r->ok);
    ASSERT_EQ(r->plain.canonicalSignalRule(), CanonicalSignalRule::Priority)
        << "the reference configuration no longer asks the priority rule";
    const CanonicalSignalRule held = r->derived->canonicalSignalRule();
    const std::string why = silentPin(held == CanonicalSignalRule::Priority, r->derive_warns,
                                      "processing.canonical_signal_rule",
                                      asText(CanonicalSignalRule::Priority), asText(held));
    if (!why.empty()) GTEST_SKIP() << why;
}

TEST(ConfigAskedVsInEffectDerived, SignalRuleStationScopeNamingTheHeldOutStation) {
    const auto r = loadAndDerive("scope", [](nlohmann::json& j) {
        j["processing"]["canonical_signal_priority_stations"] = nlohmann::json::array({ "WTZZ00DEU" });
    });
    ASSERT_TRUE(r->ok);
    ASSERT_EQ(r->plain.canonicalSignalPriorityStations(), std::vector<std::string>{ "WTZZ00DEU" });
    const std::vector<std::string>& held = r->derived->canonicalSignalPriorityStations();
    const std::string why = silentPin(held == r->plain.canonicalSignalPriorityStations(),
                                      r->derive_warns,
                                      "processing.canonical_signal_priority_stations",
                                      asText(r->plain.canonicalSignalPriorityStations()), asText(held));
    if (!why.empty()) GTEST_SKIP() << why;
}

TEST(ConfigAskedVsInEffectDerived, SignalRuleCodeFollowsPhaseAskedFalse) {
    const auto r = loadAndDerive("follows", [](nlohmann::json& j) {
        j["processing"]["canonical_signal_code_follows_phase"] = false;
    });
    ASSERT_TRUE(r->ok);
    ASSERT_FALSE(r->plain.canonicalSignalCodeFollowsPhase());
    const bool held = r->derived->canonicalSignalCodeFollowsPhase();
    const std::string why = silentPin(!held, r->derive_warns,
                                      "processing.canonical_signal_code_follows_phase",
                                      asText(false), asText(held));
    if (!why.empty()) GTEST_SKIP() << why;
}

TEST(ConfigAskedVsInEffectDerived, NoBiasCodeWeightAskedOne) {
    // The reference configuration asks 1.0 (code on signals without a bias fully weighted).
    const auto r = loadAndDerive("nobia", kNoEdit);
    ASSERT_TRUE(r->ok);
    ASSERT_DOUBLE_EQ(r->plain.noBiaCodeDownweight(), 1.0)
        << "the reference configuration no longer asks 1.0";
    const double held = r->derived->noBiaCodeDownweight();
    const std::string why = silentPin(held == 1.0, r->derive_warns,
                                      "estimation.no_bia_code_downweight", asText(1.0), asText(held));
    if (!why.empty()) GTEST_SKIP() << why;
}

TEST(ConfigAskedVsInEffectDerived, BiasAgeGateLeftUnset) {
    // The reference configuration does not set the key; the loader default is off.
    const auto r = loadAndDerive("agegate", kNoEdit);
    ASSERT_TRUE(r->ok);
    ASSERT_FALSE(r->plain.enableSsrBiasMaxAge());
    const bool held = r->derived->enableSsrBiasMaxAge();
    const std::string why = silentPin(
        !held, r->derive_warns, "processing.enable_ssr_bias_max_age",
        "nothing (unset, loader default false)",
        asText(held) + " (age " + asText(r->derived->getSsrBiasMaxAge()) + " s, hold x" +
            asText(r->derived->getSsrBiasStaleHoldFactor()) + ")");
    if (!why.empty()) GTEST_SKIP() << why;
}

TEST(ConfigAskedVsInEffectDerived, ChannelModeAskedAllWithoutTheHarnessKey) {
    // The reference configuration runs the float vehicle with the held basis, so "all" survives
    // the network loader's interlock on the producer.
    const auto r = loadAndDerive("channel", [](nlohmann::json& j) {
        j["processing"]["channel_mode"] = "all";
    });
    ASSERT_TRUE(r->ok);
    ASSERT_EQ(r->plain.getChannelMode(), ChannelMode::All);
    const ChannelMode held = r->derived->getChannelMode();
    const std::string why = silentPin(held == ChannelMode::All, r->derive_warns,
                                      "processing.channel_mode", asText(ChannelMode::All), asText(held));
    if (!why.empty()) GTEST_SKIP() << why;
}

TEST(ConfigAskedVsInEffectDerived, ArModeAskedWithoutTheHarnessKey) {
    // With network.closed_loop.ar_mode absent, the AR mode of the estimation section is the
    // only AR mode the file asks.
    const auto r = loadAndDerive("armode", [](nlohmann::json& j) {
        j["estimation"]["ambiguity_resolution"]["mode"] = 2;
        j["network"]["closed_loop"].erase("ar_mode");
    });
    ASSERT_TRUE(r->ok);
    ASSERT_EQ(r->plain.getArMode(), 2);
    const int held = r->derived->getArMode();
    const std::string why = silentPin(held == 2, r->derive_warns,
                                      "estimation.ambiguity_resolution.mode", asText(2), asText(held));
    if (!why.empty()) GTEST_SKIP() << why;
}

TEST(ConfigAskedVsInEffectDerived, ArWideLaneOnlyAskedTrue) {
    const auto r = loadAndDerive("wlonly", [](nlohmann::json& j) {
        j["estimation"]["ambiguity_resolution"]["wl_only"] = true;
    });
    ASSERT_TRUE(r->ok);
    ASSERT_TRUE(r->plain.getArWlOnly());
    const bool held = r->derived->getArWlOnly();
    const std::string why = silentPin(held, r->derive_warns,
                                      "estimation.ambiguity_resolution.wl_only", asText(true), asText(held));
    if (!why.empty()) GTEST_SKIP() << why;
}

TEST(ConfigAskedVsInEffectDerived, ArPositionSigmaGate) {
    // Within the 0.20 m cap the request stands; above it the undifferenced user is capped.
    {
        const auto r = loadAndDerive("arsig_in", [](nlohmann::json& j) {
            j["estimation"]["ambiguity_resolution"]["max_position_sigma_m"] = 0.10;
        });
        ASSERT_TRUE(r->ok);
        EXPECT_DOUBLE_EQ(r->derived->getArMaxPositionSigma(), 0.10);
    }
    const auto r = loadAndDerive("arsig_out", [](nlohmann::json& j) {
        j["estimation"]["ambiguity_resolution"]["max_position_sigma_m"] = 0.5;
    });
    ASSERT_TRUE(r->ok);
    ASSERT_DOUBLE_EQ(r->plain.getArMaxPositionSigma(), 0.5);
    const double held = r->derived->getArMaxPositionSigma();
    const std::string why = silentPin(held == 0.5, r->derive_warns,
                                      "estimation.ambiguity_resolution.max_position_sigma_m",
                                      asText(0.5), asText(held));
    if (!why.empty()) GTEST_SKIP() << why;
}

TEST(ConfigAskedVsInEffectDerived, BetweenSatelliteDifferencingAskedOn) {
    // Allowed on the network path because the closed loop gives the layout a consumer.
    const auto r = loadAndDerive("bssd", [](nlohmann::json& j) {
        j["estimation"]["between_satellite_sd"]["enabled"] = true;
    });
    ASSERT_TRUE(r->ok);
    ASSERT_TRUE(r->plain.enableBetweenSatelliteSd());
    const bool held = r->derived->enableBetweenSatelliteSd();
    const std::string why = silentPin(held, r->derive_warns,
                                      "estimation.between_satellite_sd.enabled", asText(true), asText(held));
    if (!why.empty()) GTEST_SKIP() << why;
}

TEST(ConfigAskedVsInEffectDerived, ProcessingTuningUnderTheHarnessSwitches) {
    // A harness switch arms the processing mechanism on the station and carries its own tuning
    // values, which replace the processing tuning the file asks.
    const auto r = loadAndDerive("tuning", [](nlohmann::json& j) {
        auto& p = j["processing"];
        p["innovation_screen"] = true;
        p["innovation_screen_k"] = 8.0;
        p["amb_level_check"] = true;
        p["amb_level_check_k"] = 5.0;
        p["ssr_phase_bias_step_translate"] = true;
        p["ssr_phase_bias_step_translate_eps_m"] = 0.01;
        p["ssr_level_step_inflate"] = true;
        p["ssr_level_step_inflate_k"] = 2.0;
        auto& cl = j["network"]["closed_loop"];
        cl["innovation_screen"] = true;
        cl["amb_level_check"] = true;
        cl["pb_step_translate"] = true;
        cl["level_step_inflate"] = true;
    });
    ASSERT_TRUE(r->ok);
    struct Row { const char* key; double asked; double plain; double held; };
    const Row rows[] = {
        { "processing.innovation_screen_k", 8.0, r->plain.getInnovationScreenK(),
          r->derived->getInnovationScreenK() },
        { "processing.amb_level_check_k", 5.0, r->plain.getAmbLevelCheckK(),
          r->derived->getAmbLevelCheckK() },
        { "processing.ssr_phase_bias_step_translate_eps_m", 0.01,
          r->plain.getSsrPhaseBiasStepTranslateEpsM(),
          r->derived->getSsrPhaseBiasStepTranslateEpsM() },
        { "processing.ssr_level_step_inflate_k", 2.0, r->plain.ssrLevelStepInflateK(),
          r->derived->ssrLevelStepInflateK() },
    };
    std::string silent;
    for (const Row& row : rows) {
        SCOPED_TRACE(row.key);
        ASSERT_DOUBLE_EQ(row.plain, row.asked);
        const std::string why = silentPin(row.held == row.asked, r->derive_warns, row.key,
                                          asText(row.asked), asText(row.held));
        if (!why.empty()) silent += (silent.empty() ? "" : "; ") + why;
    }
    // The switches themselves are on in the derived configuration either way.
    EXPECT_TRUE(r->derived->innovationScreen());
    EXPECT_TRUE(r->derived->ambLevelCheck());
    EXPECT_TRUE(r->derived->ssrPhaseBiasStepTranslate());
    EXPECT_TRUE(r->derived->ssrLevelStepInflate());
    if (!silent.empty()) GTEST_SKIP() << silent;
}

// ---- The standalone profile ---------------------------------------------------------------

TEST(ConfigAskedVsInEffectDerived, StandaloneProfileNoBiasCodeWeight) {
    const auto r = loadAndDerive("profile_nobia", kNoEdit);
    ASSERT_TRUE(r->ok);
    ASSERT_DOUBLE_EQ(r->plain.noBiaCodeDownweight(), 1.0);
    const double held = r->profiled->noBiaCodeDownweight();
    const std::string why = silentPin(held == 1.0, r->profile_warns,
                                      "estimation.no_bia_code_downweight", asText(1.0), asText(held),
                                      "the standalone network-consumer profile",
                                      "the profile writes info lines only, and its summary line "
                                      "does not name the key");
    if (!why.empty()) GTEST_SKIP() << why;
}

// ---- What must stand ----------------------------------------------------------------------

TEST(ConfigAskedVsInEffectDerived, ExplicitFalseOfTheBiasAgeGateStands) {
    const auto r = loadAndDerive("agegate_false", [](nlohmann::json& j) {
        j["processing"]["enable_ssr_bias_max_age"] = false;
    });
    ASSERT_TRUE(r->ok);
    EXPECT_FALSE(r->derived->enableSsrBiasMaxAge());
    EXPECT_FALSE(r->profiled->enableSsrBiasMaxAge());
}

TEST(ConfigAskedVsInEffectDerived, ArConstellationsStandWithoutAHarnessList) {
    const auto r = loadAndDerive("arsys", [](nlohmann::json& j) {
        j["estimation"]["ambiguity_resolution"]["active_systems"] = nlohmann::json::array({ "GPS" });
    });
    ASSERT_TRUE(r->ok);
    ASSERT_TRUE(r->plain.getNetworkConfig().closed_loop.ar_active_systems.empty());
    EXPECT_EQ(r->derived->getArActiveSystems(), std::vector<std::string>{ "GPS" });
}

TEST(ConfigAskedVsInEffectDerived, ProcessingSwitchesAreNeverClearedByTheHarness) {
    // The harness switches only arm; with them off, the processing switches and their tuning
    // reach the station as asked.
    const auto r = loadAndDerive("never_cleared", [](nlohmann::json& j) {
        auto& p = j["processing"];
        p["ssr_phase_bias_disc_carry"] = true;
        p["ssr_drop_unrecorded"] = true;
        p["ssr_drop_unrecorded_phase_bias"] = true;
        p["ssr_pb_hold_presence_flip"] = true;
        p["ssr_phase_bias_step_translate"] = true;
        p["ssr_level_step_inflate"] = true;
        p["ssr_level_step_inflate_k"] = 2.0;
        p["amb_level_check"] = true;
        p["innovation_screen"] = true;
        p["innovation_screen_k"] = 8.0;
        auto& cl = j["network"]["closed_loop"];
        cl["pb_hold_presence_flip"] = false;
        cl["pb_step_translate"] = false;
        cl["amb_level_check"] = false;
    });
    ASSERT_TRUE(r->ok);
    const GnssConfig& d = *r->derived;
    EXPECT_TRUE(d.ssrPhaseBiasDiscCarry());
    EXPECT_TRUE(d.ssrDropUnrecorded());
    EXPECT_TRUE(d.ssrDropUnrecordedPhaseBias());
    EXPECT_TRUE(d.ssrPbHoldPresenceFlip());
    EXPECT_TRUE(d.ssrPhaseBiasStepTranslate());
    EXPECT_TRUE(d.ssrLevelStepInflate());
    EXPECT_DOUBLE_EQ(d.ssrLevelStepInflateK(), 2.0);
    EXPECT_TRUE(d.ambLevelCheck());
    EXPECT_TRUE(d.innovationScreen());
    EXPECT_DOUBLE_EQ(d.getInnovationScreenK(), 8.0);
}

TEST(ConfigAskedVsInEffectDerived, NetworkLeversArePinnedOffTheStationAndKeptByTheProducer) {
    // Keys of the network section addressed to the network solve: the station is one receiver
    // with no signal family to support, so the derivation pins them to their no-op values.
    const auto r = loadAndDerive("levers", [](nlohmann::json& j) {
        auto& n = j["network"];
        n["extra_band_code_sigma_scale"] = 10.0;
        n["extra_band_phase_sigma_scale"] = 10.0;
        n["second_phase_channel"] = true;
        n["second_phase_sigma_scale"] = 5.0;
    });
    ASSERT_TRUE(r->ok);
    const GnssConfig& p = r->plain;
    const GnssConfig& d = *r->derived;
    EXPECT_DOUBLE_EQ(p.networkExtraBandCodeSigmaScale(), 10.0);
    EXPECT_DOUBLE_EQ(p.networkExtraBandPhaseSigmaScale(), 10.0);
    EXPECT_TRUE(p.networkSecondCodeChannel());
    EXPECT_DOUBLE_EQ(p.networkSecondCodeSigmaScale(), 10.0);
    EXPECT_TRUE(p.networkSecondPhaseChannel());
    EXPECT_DOUBLE_EQ(p.networkSecondPhaseSigmaScale(), 5.0);
    EXPECT_DOUBLE_EQ(d.networkExtraBandCodeSigmaScale(), 1.0);
    EXPECT_DOUBLE_EQ(d.networkExtraBandPhaseSigmaScale(), 1.0);
    EXPECT_FALSE(d.networkSecondCodeChannel());
    EXPECT_DOUBLE_EQ(d.networkSecondCodeSigmaScale(), 1.0);
    EXPECT_FALSE(d.networkSecondPhaseChannel());
    EXPECT_DOUBLE_EQ(d.networkSecondPhaseSigmaScale(), 1.0);
}

// ---- The complete list of members the derivation changes ----------------------------------

TEST(ConfigAskedVsInEffectDerived, MembersChangedOnTheReferenceConfiguration) {
    const auto r = loadAndDerive("complete", kNoEdit);
    ASSERT_TRUE(r->ok);
    const GnssConfig& p = r->plain;
    const GnssConfig& d = *r->derived;

    // What the expected list depends on in the reference configuration.
    const ClosedLoopConfig& cl = p.getNetworkConfig().closed_loop;
    ASSERT_TRUE(cl.enable && cl.consume_network_products && cl.consume_network_clock
                && cl.consumer_undifferenced && cl.pb_hold_presence_flip)
        << "the reference configuration's closed loop changed";
    ASSERT_EQ(cl.ar_mode, 0);
    ASSERT_EQ(cl.channel_mode, "single");
    ASSERT_TRUE(p.estimateNetworkSatCodeBias()) << "the reference configuration changed";
    ASSERT_TRUE(p.networkSecondCodeChannel()) << "the reference configuration changed";

    using Get = std::function<std::string(const GnssConfig&)>;
    const std::vector<std::pair<std::string, Get>> rows = {
        { "getProcessingMode",          [](const GnssConfig& c) { return asText(c.getProcessingMode()); } },
        { "estimateSatClocks",          [](const GnssConfig& c) { return asText(c.estimateSatClocks()); } },
        { "estimateSatPhaseBiases",     [](const GnssConfig& c) { return asText(c.estimateSatPhaseBiases()); } },
        { "promoteSatCodeBias",         [](const GnssConfig& c) { return asText(c.promoteSatCodeBias()); } },
        { "estimateNetworkSatCodeBias", [](const GnssConfig& c) { return asText(c.estimateNetworkSatCodeBias()); } },
        { "estimateNetworkIonoSh",      [](const GnssConfig& c) { return asText(c.estimateNetworkIonoSh()); } },
        { "globalVtecSh.enabled",       [](const GnssConfig& c) { return asText(c.getGlobalVtecShConfig().enabled); } },
        { "estimateSatBiases",          [](const GnssConfig& c) { return asText(c.estimateSatBiases()); } },
        { "noBiaCodeDownweight",        [](const GnssConfig& c) { return asText(c.noBiaCodeDownweight()); } },
        { "enableBetweenSatelliteSd",   [](const GnssConfig& c) { return asText(c.enableBetweenSatelliteSd()); } },
        { "getArWlOnly",                [](const GnssConfig& c) { return asText(c.getArWlOnly()); } },
        { "getArMaxPositionSigma",      [](const GnssConfig& c) { return asText(c.getArMaxPositionSigma()); } },
        { "enableSsrBiasMaxAge",        [](const GnssConfig& c) { return asText(c.enableSsrBiasMaxAge()); } },
        { "ssrPhaseBiasDiscCarry",      [](const GnssConfig& c) { return asText(c.ssrPhaseBiasDiscCarry()); } },
        { "ssrDropUnrecorded",          [](const GnssConfig& c) { return asText(c.ssrDropUnrecorded()); } },
        { "ssrDropUnrecordedPhaseBias", [](const GnssConfig& c) { return asText(c.ssrDropUnrecordedPhaseBias()); } },
        { "ssrPbHoldPresenceFlip",      [](const GnssConfig& c) { return asText(c.ssrPbHoldPresenceFlip()); } },
        { "arObserve",                  [](const GnssConfig& c) { return asText(c.arObserve()); } },
        { "arObserveSearch",            [](const GnssConfig& c) { return asText(c.arObserveSearch()); } },
        { "arObserveDeterminedSigmaCyc",[](const GnssConfig& c) { return asText(c.arObserveDeterminedSigmaCyc()); } },
        { "consumerRosterLatch",        [](const GnssConfig& c) { return asText(c.consumerRosterLatch()); } },
        { "getChannelMode",             [](const GnssConfig& c) { return asText(c.getChannelMode()); } },
        { "ssrPhaseBiasStepTranslate",  [](const GnssConfig& c) { return asText(c.ssrPhaseBiasStepTranslate()); } },
        { "getSsrPhaseBiasStepTranslateEpsM", [](const GnssConfig& c) { return asText(c.getSsrPhaseBiasStepTranslateEpsM()); } },
        { "ssrLevelStepInflate",        [](const GnssConfig& c) { return asText(c.ssrLevelStepInflate()); } },
        { "ssrLevelStepInflateK",       [](const GnssConfig& c) { return asText(c.ssrLevelStepInflateK()); } },
        { "ambLevelCheck",              [](const GnssConfig& c) { return asText(c.ambLevelCheck()); } },
        { "getAmbLevelCheckWindowEp",   [](const GnssConfig& c) { return asText(c.getAmbLevelCheckWindowEp()); } },
        { "getAmbLevelCheckK",          [](const GnssConfig& c) { return asText(c.getAmbLevelCheckK()); } },
        { "getAmbLevelCheckMode",       [](const GnssConfig& c) { return asText(c.getAmbLevelCheckMode()); } },
        { "innovationScreen",           [](const GnssConfig& c) { return asText(c.innovationScreen()); } },
        { "getInnovationScreenK",       [](const GnssConfig& c) { return asText(c.getInnovationScreenK()); } },
        { "getInnovationScreenMode",    [](const GnssConfig& c) { return asText(c.getInnovationScreenMode()); } },
        { "getInnovationScreenRows",    [](const GnssConfig& c) { return asText(c.getInnovationScreenRows()); } },
        { "getInnovationScreenMaxFraction", [](const GnssConfig& c) { return asText(c.getInnovationScreenMaxFraction()); } },
        { "getInnovationScreenRefloatEp",   [](const GnssConfig& c) { return asText(c.getInnovationScreenRefloatEp()); } },
        { "getInnovationScreenPhaseFloorM", [](const GnssConfig& c) { return asText(c.getInnovationScreenPhaseFloorM()); } },
        { "getInnovationScreenCodeFloorM",  [](const GnssConfig& c) { return asText(c.getInnovationScreenCodeFloorM()); } },
        { "canonicalSignalRule",        [](const GnssConfig& c) { return asText(c.canonicalSignalRule()); } },
        { "canonicalSignalPriorityStations", [](const GnssConfig& c) { return asText(c.canonicalSignalPriorityStations()); } },
        { "canonicalSignalCodeFollowsPhase", [](const GnssConfig& c) { return asText(c.canonicalSignalCodeFollowsPhase()); } },
        { "networkExtraBandCodeSigmaScale",  [](const GnssConfig& c) { return asText(c.networkExtraBandCodeSigmaScale()); } },
        { "networkExtraBandPhaseSigmaScale", [](const GnssConfig& c) { return asText(c.networkExtraBandPhaseSigmaScale()); } },
        { "networkSecondCodeChannel",   [](const GnssConfig& c) { return asText(c.networkSecondCodeChannel()); } },
        { "networkSecondCodeSigmaScale",[](const GnssConfig& c) { return asText(c.networkSecondCodeSigmaScale()); } },
        { "networkSecondPhaseChannel",  [](const GnssConfig& c) { return asText(c.networkSecondPhaseChannel()); } },
        { "networkSecondPhaseSigmaScale",[](const GnssConfig& c) { return asText(c.networkSecondPhaseSigmaScale()); } },
        { "getArMode",                  [](const GnssConfig& c) { return asText(c.getArMode()); } },
        { "getArActiveSystems",         [](const GnssConfig& c) { return asText(c.getArActiveSystems()); } },
        // Controls the derivation does not write: they must stay equal to the plain copy.
        { "estimateIfb",                [](const GnssConfig& c) { return asText(c.estimateIfb()); } },
        { "getSsrBiasMaxAge",           [](const GnssConfig& c) { return asText(c.getSsrBiasMaxAge()); } },
        { "canonicalSignalMinCoverage", [](const GnssConfig& c) { return asText(c.canonicalSignalMinCoverage()); } },
    };

    // Every member the derivation changes on this configuration, and only those.
    const std::set<std::string> expected = {
        "getProcessingMode",          // network producer -> single-receiver PPP-RTK user
        "estimateSatClocks",          // forced on by the network loader, off at the station
        "estimateSatPhaseBiases",     // likewise
        "promoteSatCodeBias",         // forced on with the network's own code bias, off
        "estimateNetworkSatCodeBias", // producer-only switch
        "estimateSatBiases",          // off under the network clock
        "noBiaCodeDownweight",        // the file's 1.0 becomes 100
        "enableSsrBiasMaxAge",        // unset in the file, armed under consumption
        "ssrPbHoldPresenceFlip",      // armed by network.closed_loop.pb_hold_presence_flip
        "canonicalSignalRule",        // the file's priority becomes count
        "networkSecondCodeChannel",   // producer lever pinned off
        "networkSecondCodeSigmaScale",// producer lever pinned to 1
    };

    std::set<std::string> changed;
    std::string table;
    for (const auto& row : rows) {
        const std::string a = row.second(p);
        const std::string b = row.second(d);
        if (a != b) changed.insert(row.first);
        table += "  " + row.first + ": " + a + " -> " + b + "\n";
    }
    EXPECT_EQ(changed, expected) << "members as loaded -> as derived:\n" << table;
}

}  // namespace
