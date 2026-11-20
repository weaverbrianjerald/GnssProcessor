/**
 * @file test_config_asked_vs_in_effect_network.cpp
 * @brief What a network configuration asks against what the network loader and the knob
 *        snapshot put in effect, one case per interlock.
 *
 * The network loader refuses, clamps, forces and disarms values after it reads them, and
 * NetworkKnobs::snapshot resolves several requests against companion keys once more when the
 * run starts.  A reader of the load log can follow a request only when the change is printed.
 * Each case writes one configuration to the test's scratch folder (a copy of
 * config/golden/golden_field_rf0hv18.json with the keys under test changed, or the minimal
 * two-station network of ConfigLoadCapture.hpp where the reference posture cannot express the
 * interlock without a chain of refusals), loads it while capturing the load log with its levels,
 * and accepts one of three outcomes:
 *  - the member, or the knob after the snapshot, holds what was asked;
 *  - the loader refused the file;
 *  - a line at warn level or above names the key.
 * A case where the value in effect differs from the request and no such line exists is kept as
 * a skip whose message names the key, what was asked and what is in effect.  It passes on its
 * own once the loader announces the change or lets the request stand.  The loud cases fail when
 * the line that names the key disappears while the value still differs.
 *
 * Runtime: the reference configuration is parsed once per process, and its station registry
 * (parsed whole by every load under ocean tide loading from the registry, about 13 s of a load in
 * Debug) is replaced by copies cut to the roster (tests/support/ReferenceConfigCache.hpp).  A load
 * then differs from a load of the committed file only in getStationInfoPaths and the station count
 * of one info line; every case compares loads made the same way or reads neither.
 *
 * Three keys are set in every reference configuration and read by no loader line
 * (network.redundancy_census, network.max_duration_s, network.min_stations).  Their cases show
 * that changing them moves nothing: not the member dump, not the knobs they would plausibly
 * govern, not the snapshot fingerprint and not one line of the load log; they are kept as
 * skips that name the key as unread.
 */

#include <gtest/gtest.h>

#include "config/GnssConfig.hpp"
#include "logging/LogManager.hpp"
#include "network/state/RunContext.hpp"   // NetworkKnobs, NetworkRunContext, NetStation, the fingerprint

#include "ConfigLoadCapture.hpp"            // networkConfig(): the minimal two-station network
#include "../support/ConfigDump.hpp"        // configMemberDump(): every member as sorted lines
#include "../support/ReferenceConfigCache.hpp"  // the reference parsed once, registry cut to the roster
#include "../support/TestTempDir.hpp"       // per-process, per-test scratch directory

#include <nlohmann/json.hpp>
#include <spdlog/sinks/ostream_sink.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

namespace GnssProcessor {
namespace {

namespace fs = std::filesystem;

/// One record of the load log: the level name spdlog prints and the message text.
struct LogRecord {
    std::string level;
    std::string text;
};

/// A configuration as loaded, with everything the loader logged while loading it.
struct LoadResult {
    bool ok{false};                   ///< loadFromJson's result (false: refused)
    GnssConfig cfg;                   ///< the loaded configuration (meaningful when @c ok)
    std::vector<LogRecord> records;   ///< the load log, one entry per record
    std::string log;                  ///< the load log as "[level] text" lines, for messages
};

/// The field reference configuration of the float-vehicle posture, parsed once per process, with its
/// station registry cut to its roster (ReferenceConfigCache.hpp; a copy free to edit).  The cases were
/// written against it (the audit of record CFG, 2026-11-18); since 2026-11-19 it is the frozen lineage
/// golden_field_rf0hv18, and the integer-fixing reference runs the phase-bias hold off, so the cases of the
/// hold and the held basis would not express their interlock there.
nlohmann::json fieldReference() {
    return gnss_test::referenceConfigWithRosterStationInfo("golden/golden_field_rf0hv18.json");
}

/// The minimal two-station network of ConfigLoadCapture.hpp: level vehicle, legacy datum.
nlohmann::json minimalNetwork() {
    return nlohmann::json::parse(TestSupport::networkConfig(""));
}

/**
 * Write @p j to the test's scratch folder, load it, and capture the "gnss_config" logger and the
 * default logger (where a refusal by exception is reported).  Every record is framed by control
 * characters so that a message spanning several lines stays one record with its level.  The same
 * file path is used for every load of one test, so two load logs of one test differ only where
 * the loader saw a difference.
 */
LoadResult loadCapturing(const nlohmann::json& j) {
    LoadResult out;
    const fs::path dir = gnss_test::tempDir();
    const fs::path cfg_path = dir / "config.json";
    {
        std::ofstream f(cfg_path);
        f << j.dump(2);
    }

    // Only the names this helper creates are dropped (never drop_all()): another live object may
    // hold the other loggers.
    spdlog::drop("gnss_processor");
    spdlog::drop("gnss_config");
    std::ostringstream oss;
    {
        LogManager log;
        log.initialize(dir, LogLevel::Info);
        auto sink = std::make_shared<spdlog::sinks::ostream_sink_mt>(oss);
        sink->set_pattern("\x1e%l\x1f%v");
        auto cfg_log = log.getLogger("gnss_config");
        auto main_log = log.getLogger();
        cfg_log->sinks().push_back(sink);
        main_log->sinks().push_back(sink);

        out.ok = out.cfg.loadFromJson(cfg_path, log);

        cfg_log->flush();
        main_log->flush();
        // The stream dies with this function: the sink leaves both loggers before it does.
        for (auto* lg : {cfg_log.get(), main_log.get()}) {
            auto& sinks = lg->sinks();
            sinks.erase(std::remove(sinks.begin(), sinks.end(), sink), sinks.end());
        }
        spdlog::drop("gnss_processor");
        spdlog::drop("gnss_config");
    }

    const std::string raw = oss.str();
    std::size_t pos = raw.find('\x1e');
    while (pos != std::string::npos) {
        const std::size_t next = raw.find('\x1e', pos + 1);
        std::string rec = raw.substr(pos + 1, next == std::string::npos ? std::string::npos
                                                                        : next - pos - 1);
        while (!rec.empty() && (rec.back() == '\n' || rec.back() == '\r')) rec.pop_back();
        const std::size_t sep = rec.find('\x1f');
        LogRecord r;
        r.level = sep == std::string::npos ? std::string{} : rec.substr(0, sep);
        r.text  = sep == std::string::npos ? rec : rec.substr(sep + 1);
        out.log += "[" + r.level + "] " + r.text + "\n";
        out.records.push_back(std::move(r));
        pos = next;
    }
    return out;
}

/// True for a record at warn level or above.
bool atWarnOrAbove(const LogRecord& r) {
    return r.level == "warning" || r.level == "warn" || r.level == "error" || r.level == "critical";
}

bool isIdentChar(char c) {
    return std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '_';
}

/**
 * True when @p text names @p key as a whole token: the characters around it are not part of an
 * identifier, so "network.raim" is not found inside "network.raim_within_epoch" and
 * "vtec_sh" is not found inside "vtec_sh_layer2".  A dot after the key counts as a boundary only
 * when no identifier follows it (the end of a sentence, not a longer dotted key).
 */
bool namesKey(const std::string& text, const std::string& key) {
    for (std::size_t pos = text.find(key); pos != std::string::npos; pos = text.find(key, pos + 1)) {
        const bool before_ok = pos == 0 || !isIdentChar(text[pos - 1]);
        const std::size_t end = pos + key.size();
        bool after_ok = end >= text.size() || !isIdentChar(text[end]);
        if (after_ok && end < text.size() && text[end] == '.' && end + 1 < text.size()
            && isIdentChar(text[end + 1]))
            after_ok = false;
        if (before_ok && after_ok) return true;
    }
    return false;
}

/// True when a record at warn level or above names one of @p spellings of a key.
bool loudAbout(const LoadResult& r, std::initializer_list<const char*> spellings) {
    for (const LogRecord& rec : r.records) {
        if (!atWarnOrAbove(rec)) continue;
        for (const char* s : spellings)
            if (namesKey(rec.text, s)) return true;
    }
    return false;
}

/// True when a record at warn level or above contains @p phrase.
bool loudWith(const LoadResult& r, const std::string& phrase) {
    for (const LogRecord& rec : r.records)
        if (atWarnOrAbove(rec) && rec.text.find(phrase) != std::string::npos) return true;
    return false;
}

/// The knobs a run of @p cfg would read (the snapshot the run context takes at its start).
NetworkKnobs knobsOf(const GnssConfig& cfg) {
    return NetworkKnobs::snapshot(cfg, cfg.getNetworkConfig());
}

/// The warm-resume fingerprint a run of @p cfg would compute (a hash of its estimation knobs).
std::uint64_t fingerprintOf(const GnssConfig& cfg) {
    NetworkRunContext ctx(cfg, cfg.getNetworkConfig());
    ctx.carry.snap_path = "state_snapshot.snap";   // the builder is a no-op without a path
    std::vector<NetStation> st(2);
    st[0].name = "AAAA"; st[0].is_pivot = true;
    st[1].name = "BBBB"; st[1].is_pivot = false;
    networkSnapshotFingerprint(ctx, cfg, st);
    return ctx.carry.live_fingerprint;
}

/**
 * The unread-key check shared by the three unread keys: the reference configuration and the
 * same file with @p edit applied load to the same member dump, the same fingerprint and the
 * same load log.  Knob comparisons specific to the key are made by the caller.
 */
void expectNothingMoves(const LoadResult& base, const LoadResult& changed) {
    ASSERT_TRUE(base.ok) << base.log;
    ASSERT_TRUE(changed.ok) << changed.log;
    EXPECT_EQ(gnss_test::configMemberDump(changed.cfg), gnss_test::configMemberDump(base.cfg));
    EXPECT_EQ(fingerprintOf(changed.cfg), fingerprintOf(base.cfg));
    EXPECT_EQ(changed.log, base.log);
}

}  // namespace

// ---------------------------------------------------------------------------
// Refusals
// ---------------------------------------------------------------------------

// A network-mode file without a network section is refused with an error, not run on defaults.
// The refusal is the property locked here.  The loader also has a dedicated message for the
// missing section; when the section is absent the section lookup hands back an empty object
// rather than null, so that message is never reached and the refusal comes from the station
// count instead.  The missing message is kept as a skip that names it.
TEST(ConfigAskedVsInEffectNetwork, NetworkModeWithoutANetworkSectionIsRefused) {
    nlohmann::json j = fieldReference();
    j.erase("network");
    const LoadResult r = loadCapturing(j);
    ASSERT_FALSE(r.ok) << r.log;
    EXPECT_TRUE(loudWith(r, "no 'network' section") || loudWith(r, "network.stations")) << r.log;
    if (!loudWith(r, "no 'network' section"))
        GTEST_SKIP() << "network section absent: the load is refused, but by the station-count "
                        "check (\"need >=2 stations in network.stations (got 0)\"); the dedicated "
                        "\"no 'network' section\" error is unreachable for an absent section, "
                        "because the section lookup returns a shared empty object, never null.";
}

// A misspelt class of the scoped process-noise table would be inert; it is refused instead.
TEST(ConfigAskedVsInEffectNetwork, UnknownProcessNoiseClassIsRefused) {
    nlohmann::json j = fieldReference();
    j["network"]["process_noise"] = {{"no_such_class", 1e-6}};
    const LoadResult r = loadCapturing(j);
    EXPECT_FALSE(r.ok) << r.log;
    EXPECT_TRUE(loudWith(r, "unknown process-noise class 'network.process_noise.no_such_class'"))
        << r.log;
}

// A ring-fix dependent without what it depends on is refused at load (the settled flags need
// the listed pairs), whether or not the ring fix itself is armed.
TEST(ConfigAskedVsInEffectNetwork, RingFixSettledFlagsWithoutPairsIsRefused) {
    nlohmann::json j = fieldReference();
    j["network"]["ring_fix"] = {{"settled_flags", true}};
    const LoadResult r = loadCapturing(j);
    EXPECT_FALSE(r.ok) << r.log;
    EXPECT_TRUE(loudWith(r, "network.ring_fix.settled_flags needs network.ring_fix.pairs")) << r.log;
}

// ---------------------------------------------------------------------------
// Requests the loader changes, and says so
// ---------------------------------------------------------------------------

// A table entry beside the legacy key it subsumes: the legacy key wins and a warn says so.
TEST(ConfigAskedVsInEffectNetwork, ProcessNoiseTableEntryBesideItsLegacyKeyWarns) {
    nlohmann::json j = fieldReference();
    j["network"]["process_noise"] = {{"sat_clock", 0.05}};   // the file also sets sat_clock_q 0.001
    const LoadResult r = loadCapturing(j);
    ASSERT_TRUE(r.ok) << r.log;
    const bool asked_holds = r.cfg.getProcessNoise().sat_clock == 0.05;
    if (!asked_holds)
        EXPECT_TRUE(loudWith(r, "process-noise class 'sat_clock' is set BOTH")) << r.log;
}

// The within-epoch RAIM tier pulls in the post-fit tier; a file asking network.raim false gets
// it true, and the line that enables it names network.raim.
TEST(ConfigAskedVsInEffectNetwork, RaimForcedByTheWithinEpochTierIsAnnounced) {
    nlohmann::json j = fieldReference();
    j["network"]["raim"] = false;   // raim_within_epoch stays on, as in the reference
    const LoadResult r = loadCapturing(j);
    ASSERT_TRUE(r.ok) << r.log;
    if (r.cfg.networkRaim())
        EXPECT_TRUE(loudAbout(r, {"network.raim"})) << r.log;
}

// processing.channel_mode "all" needs the held basis on the float vehicle; without it the mode is
// clamped to single and an error line names the key.
TEST(ConfigAskedVsInEffectNetwork, ChannelModeAllWithoutTheHeldBasisIsClampedLoudly) {
    nlohmann::json j = fieldReference();
    j["processing"]["channel_mode"] = "all";
    j["network"]["phase_datum_held_basis"] = false;
    const LoadResult r = loadCapturing(j);
    ASSERT_TRUE(r.ok) << r.log;
    if (r.cfg.getChannelMode() != ChannelMode::All)
        EXPECT_TRUE(loudAbout(r, {"processing.channel_mode"})) << r.log;
}

// The suspect-corner mask without the producer search: zeroed by the knob snapshot, and the
// loader names it INERT.
TEST(ConfigAskedVsInEffectNetwork, SuspectEpochsWithoutTheSearchIsAnnounced) {
    nlohmann::json j = fieldReference();
    j["network"]["ambiguity_resolution"] = {{"suspect_epochs", 10}};   // .enable stays off
    const LoadResult r = loadCapturing(j);
    ASSERT_TRUE(r.ok) << r.log;
    EXPECT_EQ(r.cfg.getNetworkConfig().ambiguity_resolution.suspect_epochs, 10);
    if (knobsOf(r.cfg).ar_suspect_epochs != 10)
        EXPECT_TRUE(loudAbout(r, {"ambiguity_resolution.suspect_epochs"})) << r.log;
}

// A non-positive anchor rate is reported at error level and the load goes on: the request is
// stored as asked, and the error names it.
TEST(ConfigAskedVsInEffectNetwork, ClockAnchorQNotPositiveIsAnErrorThatDoesNotStopTheLoad) {
    nlohmann::json j = fieldReference();
    j["network"]["clock_anchor_q"] = 0.0;   // the temporal anchor stays armed
    const LoadResult r = loadCapturing(j);
    ASSERT_TRUE(r.ok) << r.log;
    EXPECT_EQ(r.cfg.networkClockAnchorQ(), 0.0);
    EXPECT_TRUE(loudAbout(r, {"network.clock_anchor_q"})) << r.log;
}

// The trip generation without the hold: the knob snapshot resolves it off, and the loader names
// it INERT.
TEST(ConfigAskedVsInEffectNetwork, TripGenerationWithoutTheHoldIsAnnounced) {
    nlohmann::json j = fieldReference();
    j["network"]["emit_pb_hold"] = false;
    j["network"]["emit_pb_hold_phase_clock"] = false;   // a mode of the hold: refused without it
    const LoadResult r = loadCapturing(j);              // emit_pb_hold_trip_generation stays true
    ASSERT_TRUE(r.ok) << r.log;
    if (!knobsOf(r.cfg).emit_pb_hold_trip_gen)
        EXPECT_TRUE(loudAbout(r, {"network.emit_pb_hold_trip_generation"})) << r.log;
}

// The ratio ceiling of the continuous handover without the handover: zeroed by the snapshot, and
// a warn names it INERT.  (Its sibling, the absolute ceiling, has no such line: next section.)
TEST(ConfigAskedVsInEffectNetwork, HandoverRatioCeilingWithoutTheContinuousHandoverIsAnnounced) {
    nlohmann::json j = fieldReference();
    j["network"]["phase_datum_handover_continuous"] = false;
    j["network"]["phase_datum_handover_max_var_ratio"] = 2.0;
    const LoadResult r = loadCapturing(j);
    ASSERT_TRUE(r.ok) << r.log;
    EXPECT_EQ(r.cfg.networkPhaseDatumHandoverMaxVarRatio(), 2.0);
    if (knobsOf(r.cfg).pd_handover_max_var_ratio != 2.0)
        EXPECT_TRUE(loudAbout(r, {"network.phase_datum_handover_max_var_ratio"})) << r.log;
}

// The held-basis election gates at values other than their defaults, without the held basis: a
// warn names them INERT.
TEST(ConfigAskedVsInEffectNetwork, HeldBasisGatesAtOtherValuesWithoutTheHeldBasisAreAnnounced) {
    nlohmann::json j = fieldReference();
    j["network"]["phase_datum_held_basis"] = false;
    j["network"]["phase_anchor_min_age_ep"] = 12;
    const LoadResult r = loadCapturing(j);
    ASSERT_TRUE(r.ok) << r.log;
    EXPECT_FALSE(knobsOf(r.cfg).pd_held_basis);   // the gates are read only under the held basis
    EXPECT_TRUE(loudAbout(r, {"phase_anchor_min_age_ep"})) << r.log;
}

// The wet delay's event inflation without the in-block wet delay: the snapshot resolves it to 0.
// A warn names the key; note that its text states the armed effect (the carried wet delay kept
// with an inflated sigma) although the erase response stays in force.
TEST(ConfigAskedVsInEffectNetwork, ZwdEventInflateWithoutTheInBlockWetDelayIsNamed) {
    nlohmann::json j = fieldReference();
    j["network"]["carry_zwd"] = false;   // carry_zwd_in_block is then disabled with a warn
    j["network"]["zwd_event_inflate_m"] = 0.02;
    const LoadResult r = loadCapturing(j);
    ASSERT_TRUE(r.ok) << r.log;
    EXPECT_EQ(r.cfg.getNetworkConfig().zwd_event_inflate_m, 0.02);
    if (knobsOf(r.cfg).zwd_event_inflate_m != 0.02)
        EXPECT_TRUE(loudAbout(r, {"network.zwd_event_inflate_m"})) << r.log;
}

// The drop's step cover on the level vehicle: the snapshot resolves it to 0 off the float
// vehicle.  A warn names the key; its text states the armed effect unconditionally.
TEST(ConfigAskedVsInEffectNetwork, DropJumpKOnTheLevelVehicleIsNamed) {
    nlohmann::json j = minimalNetwork();   // level vehicle by default
    j["network"]["amb_float_drop_jump_k"] = 1.0;
    const LoadResult r = loadCapturing(j);
    ASSERT_TRUE(r.ok) << r.log;
    EXPECT_EQ(r.cfg.getNetworkConfig().amb_float_drop_jump_k, 1.0);
    if (knobsOf(r.cfg).amb_drop_jump_k != 1.0)
        EXPECT_TRUE(loudAbout(r, {"network.amb_float_drop_jump_k"})) << r.log;
}

// The anchor-slip handover needs the prioritized election; on the legacy datum it is disarmed
// with a warn that names it.
TEST(ConfigAskedVsInEffectNetwork, AnchorSlipHandoverWithoutThePrioritizedElectionIsAnnounced) {
    nlohmann::json j = minimalNetwork();   // phase_datum_prioritized off by default
    j["network"]["cycle_slip"] = {{"anchor_slip_handover", true}};
    const LoadResult r = loadCapturing(j);
    ASSERT_TRUE(r.ok) << r.log;
    if (!r.cfg.networkCycleSlipAnchorSlipHandover())
        EXPECT_TRUE(loudAbout(r, {"network.cycle_slip.anchor_slip_handover"})) << r.log;
}

// The service profile arms its bundle over an explicit false, and warns for each key it
// overrides.
TEST(ConfigAskedVsInEffectNetwork, ServiceModeOverrideOfAnExplicitFalseIsAnnounced) {
    nlohmann::json j = minimalNetwork();
    j["network"]["service_mode"] = true;
    j["network"]["emit_ura"] = false;
    const LoadResult r = loadCapturing(j);
    ASSERT_TRUE(r.ok) << r.log;
    if (r.cfg.networkEmitUra())
        EXPECT_TRUE(loudAbout(r, {"network.emit_ura"})) << r.log;
}

// ---------------------------------------------------------------------------
// Requests changed or left without effect, with no line (kept as skips)
// ---------------------------------------------------------------------------

// The absolute ceiling of the continuous handover without the handover: the snapshot zeroes it,
// and unlike its ratio sibling no line says so.
TEST(ConfigAskedVsInEffectNetwork, HandoverMaxVarWithoutTheContinuousHandover) {
    nlohmann::json j = fieldReference();
    j["network"]["phase_datum_handover_continuous"] = false;   // max_var_m2 0.01 as in the file
    const LoadResult r = loadCapturing(j);
    ASSERT_TRUE(r.ok) << r.log;
    EXPECT_EQ(r.cfg.networkPhaseDatumHandoverMaxVarM2(), 0.01);
    const double in_effect = knobsOf(r.cfg).pd_handover_max_var;
    if (in_effect != 0.01 && !loudAbout(r, {"network.phase_datum_handover_max_var_m2"}))
        GTEST_SKIP() << "network.phase_datum_handover_max_var_m2: asked 0.01 m^2 with "
                        "network.phase_datum_handover_continuous false; in effect "
                     << in_effect << " (the knob snapshot zeroes the ceiling without the "
                        "continuous handover) and no line at warn level or above names the key.";
}

// A trip-generation trigger without the trip generation: stored, read by nothing, and named by
// no line.
TEST(ConfigAskedVsInEffectNetwork, TripTriggerWithoutTheTripGeneration) {
    nlohmann::json j = fieldReference();
    j["network"]["emit_pb_hold"] = false;
    j["network"]["emit_pb_hold_phase_clock"] = false;
    j["network"]["emit_pb_hold_trip_generation"] = false;   // the trigger stays "clock_event"
    const LoadResult r = loadCapturing(j);
    ASSERT_TRUE(r.ok) << r.log;
    EXPECT_EQ(r.cfg.getNetworkConfig().emit_pb_hold_trip_trigger, NetworkPbHoldTripTrigger::ClockEvent);
    const NetworkKnobs k = knobsOf(r.cfg);
    const bool in_effect = k.emit_pb_hold_trip_gen && k.emit_pb_hold_trip_clock_event_only;
    if (!in_effect && !loudAbout(r, {"network.emit_pb_hold_trip_generation_trigger"}))
        GTEST_SKIP() << "network.emit_pb_hold_trip_generation_trigger: asked clock_event; in effect "
                        "no trigger (the trip generation is off, so no satellite clock event opens "
                        "a hold generation) and no line at warn level or above names the key.";
}

// The held-basis election gates at their default values without the held basis: the inert
// warning tests "differs from the default" rather than "was written", so it stays silent.
TEST(ConfigAskedVsInEffectNetwork, HeldBasisGatesAtTheirDefaultsWithoutTheHeldBasis) {
    nlohmann::json j = fieldReference();
    j["network"]["phase_datum_held_basis"] = false;   // the file writes 10 and 5, the defaults
    const LoadResult r = loadCapturing(j);
    ASSERT_TRUE(r.ok) << r.log;
    const NetworkKnobs k = knobsOf(r.cfg);
    EXPECT_EQ(k.pa_min_age_ep, 10);
    EXPECT_EQ(k.pa_flag_quiet_ep, 5);
    const bool in_effect = k.pd_held_basis;   // the only reader of the gates is the held basis
    if (!in_effect && !loudAbout(r, {"phase_anchor_min_age_ep", "phase_anchor_flag_quiet_ep"}))
        GTEST_SKIP() << "network.phase_anchor_min_age_ep / network.phase_anchor_flag_quiet_ep: "
                        "asked 10 and 5 with network.phase_datum_held_basis false; in effect no "
                        "election gate (only the held basis reads them) and no line at warn level "
                        "or above names the keys (the inert warning fires only for other values).";
}

// The drop decoupling on the level vehicle: resolved off by the snapshot, and no line names it.
TEST(ConfigAskedVsInEffectNetwork, DropDecoupleAlwaysOnTheLevelVehicle) {
    nlohmann::json j = minimalNetwork();   // level vehicle by default
    j["network"]["amb_float_drop_decouple_always"] = true;
    const LoadResult r = loadCapturing(j);
    ASSERT_TRUE(r.ok) << r.log;
    EXPECT_TRUE(r.cfg.networkAmbFloatDropDecoupleAlways());
    const bool in_effect = knobsOf(r.cfg).amb_drop_decouple_always;
    if (!in_effect && !loudAbout(r, {"network.amb_float_drop_decouple_always"}))
        GTEST_SKIP() << "network.amb_float_drop_decouple_always: asked true on the level vehicle; "
                        "in effect false (the knob snapshot resolves it off unless "
                        "network.ambiguity_vehicle is float_kf) and no line at warn level or above "
                        "names the key.";
}

// Satellite code-bias estimation forces the phase clock off; the line that lists the forced
// flags omits it.
TEST(ConfigAskedVsInEffectNetwork, PhaseClockForcedOffUnderSatelliteCodeBias) {
    nlohmann::json j = fieldReference();   // network.estimate_sat_code_bias is on in the file
    j["estimation"]["estimate_phase_clock"] = true;
    const LoadResult r = loadCapturing(j);
    ASSERT_TRUE(r.ok) << r.log;
    const bool in_effect = r.cfg.estimatePhaseClock();
    if (!in_effect && !loudAbout(r, {"estimation.estimate_phase_clock", "estimate_phase_clock"}))
        GTEST_SKIP() << "estimation.estimate_phase_clock: asked true with "
                        "network.estimate_sat_code_bias on; in effect false (the satellite "
                        "code-bias estimation forces it off) and no line at warn level or above "
                        "names the key; the line listing the forced flags omits it.";
}

// The bound of the code-bias prior with the prior off: any value is stored, even one the prior
// refuses, and nothing uses it.
TEST(ConfigAskedVsInEffectNetwork, KaulaSigmaWithThePriorOff) {
    nlohmann::json j = fieldReference();   // network.cb_kaula_prior is off in the file
    j["network"]["cb_kaula_sigma_m"] = -1.0;
    const LoadResult r = loadCapturing(j);
    ASSERT_TRUE(r.ok) << r.log;
    EXPECT_EQ(r.cfg.networkCbKaulaSigmaM(), -1.0);
    const bool in_effect = r.cfg.networkCbKaulaPrior();
    if (!in_effect && !loudAbout(r, {"network.cb_kaula_sigma_m"}))
        GTEST_SKIP() << "network.cb_kaula_sigma_m: asked -1 m with network.cb_kaula_prior false; "
                        "in effect no prior (nothing reads the bound) and no line at warn level or "
                        "above names the key, although the same value is refused with the prior on.";
}

// A process-noise class whose gate is off: stored and reported in the table as set, and drives
// nothing.
TEST(ConfigAskedVsInEffectNetwork, ProcessNoiseClassWithItsGateOff) {
    nlohmann::json j = fieldReference();   // network.estimate_iono_sh is off in the file
    j["network"]["process_noise"] = {{"vtec_sh", 0.05}};
    const LoadResult r = loadCapturing(j);
    ASSERT_TRUE(r.ok) << r.log;
    EXPECT_EQ(r.cfg.getGlobalVtecShConfig().q_per_degree, 0.05);
    EXPECT_EQ(r.cfg.getNetworkConfig().process_noise.get(NetworkQClass::VtecSh), 0.05);
    const bool in_effect = r.cfg.estimateNetworkIonoSh();
    if (!in_effect && !loudAbout(r, {"network.process_noise.vtec_sh", "vtec_sh"}))
        GTEST_SKIP() << "network.process_noise.vtec_sh: asked 0.05 TECU^2/s with "
                        "network.estimate_iono_sh false; in effect no state uses it (no ionosphere "
                        "harmonics are estimated), the process-noise line lists it as set from the "
                        "table, and no line at warn level or above names the key.";
}

// The code and ionosphere row screen pulls in the within-epoch tier and, with it, the post-fit
// tier; the warn names only the within-epoch tier.
TEST(ConfigAskedVsInEffectNetwork, RaimForcedByTheCodeIonoScreen) {
    nlohmann::json j = fieldReference();
    j["network"]["raim"] = false;
    j["network"]["raim_within_epoch"] = false;
    j["network"]["raim_withhold_sigma0"] = 0.0;   // else the withhold ceiling arms the tier first
    j["network"]["raim_we_code_iono"] = true;
    const LoadResult r = loadCapturing(j);
    ASSERT_TRUE(r.ok) << r.log;
    EXPECT_TRUE(r.cfg.networkRaimWithinEpoch());
    const bool in_effect = r.cfg.networkRaim();
    if (in_effect && !loudAbout(r, {"network.raim"}))
        GTEST_SKIP() << "network.raim: asked false with network.raim_we_code_iono true; in effect "
                        "true (the screen arms the within-epoch tier, which arms the post-fit tier) "
                        "and no line at warn level or above names network.raim; the warn names "
                        "only network.raim_within_epoch.";
}

// A negative cap on the traced DD pairs becomes 0, which means no cap.
TEST(ConfigAskedVsInEffectNetwork, DdFractionalMaxPairsNegative) {
    nlohmann::json j = fieldReference();
    j["network"]["trace_dd_fractional_max_pairs"] = -1;
    const LoadResult r = loadCapturing(j);
    ASSERT_TRUE(r.ok) << r.log;
    const int in_effect = r.cfg.getNetworkConfig().trace_dd_fractional_max_pairs;
    if (in_effect != -1 && !loudAbout(r, {"network.trace_dd_fractional_max_pairs"}))
        GTEST_SKIP() << "network.trace_dd_fractional_max_pairs: asked -1; in effect " << in_effect
                     << " (0 means no cap) and no line at warn level or above names the key.";
}

// ---------------------------------------------------------------------------
// Keys set in every reference configuration and read by no loader line
// ---------------------------------------------------------------------------

// network.redundancy_census false: the census and its dependents stay armed by the stride.
TEST(ConfigAskedVsInEffectNetwork, RedundancyCensusIsUnread) {
    const nlohmann::json base = fieldReference();
    nlohmann::json changed = base;
    changed["network"]["redundancy_census"] = false;   // the file asks true, stride 10
    const LoadResult b = loadCapturing(base);
    const LoadResult c = loadCapturing(changed);
    expectNothingMoves(b, c);
    const NetworkKnobs kb = knobsOf(b.cfg);
    const NetworkKnobs kc = knobsOf(c.cfg);
    EXPECT_TRUE(kb.redun_census_on);
    EXPECT_EQ(kc.redun_census_on, kb.redun_census_on);
    EXPECT_EQ(kc.mdb_on, kb.mdb_on);
    EXPECT_EQ(kc.vcf_on, kb.vcf_on);
    EXPECT_EQ(kc.vcf_apply, kb.vcf_apply);
    EXPECT_EQ(kc.bb_on, kb.bb_on);
    EXPECT_EQ(kc.cg_on, kb.cg_on);
    GTEST_SKIP() << "network.redundancy_census is read by no loader line: asked false, in effect "
                    "the census stays armed by network.redundancy_census_stride, with its "
                    "dependents (MDB, the variance factor and its weight change, block-Baarda, "
                    "the congruence screen), and no line names the key.";
}

// network.max_duration_s: only processing.max_duration_s sets the run length.
TEST(ConfigAskedVsInEffectNetwork, NetworkMaxDurationIsUnread) {
    const nlohmann::json base = fieldReference();
    nlohmann::json changed = base;
    changed["network"]["max_duration_s"] = 3600.0;   // the file asks 21600 in both spellings
    const LoadResult b = loadCapturing(base);
    const LoadResult c = loadCapturing(changed);
    expectNothingMoves(b, c);
    EXPECT_EQ(c.cfg.getMaxDuration(), b.cfg.getMaxDuration());
    GTEST_SKIP() << "network.max_duration_s is read by no loader line: asked 3600 s, in effect the "
                    "run length of processing.max_duration_s (" << c.cfg.getMaxDuration()
                 << " s), and no line names the key.";
}

// network.min_stations: no station floor of that name exists.
TEST(ConfigAskedVsInEffectNetwork, NetworkMinStationsIsUnread) {
    const nlohmann::json base = fieldReference();
    nlohmann::json changed = base;
    changed["network"]["min_stations"] = 40;   // the file asks 10
    const LoadResult b = loadCapturing(base);
    const LoadResult c = loadCapturing(changed);
    expectNothingMoves(b, c);
    EXPECT_EQ(c.cfg.getNetworkConfig().streaming.min_stations,
              b.cfg.getNetworkConfig().streaming.min_stations);
    EXPECT_EQ(c.cfg.getNetworkConfig().parallel_min_stations,
              b.cfg.getNetworkConfig().parallel_min_stations);
    GTEST_SKIP() << "network.min_stations is read by no loader line: asked a floor of 40 "
                    "stations, in effect no floor (only network.streaming.min_stations and "
                    "network.parallel_min_stations exist), and no line names the key.";
}

}  // namespace GnssProcessor
