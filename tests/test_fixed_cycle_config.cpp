/**
 * @file test_fixed_cycle_config.cpp
 * @brief Loader tests of the fixed-cycle fold (step FCF-2 and its review fixes, step
 *        FCF-3): @c network.ambiguity_resolution.fixed_cycles.*.
 *
 * Suite @c FixedCycleConfig:
 *  - the knobs default to the unarmed posture (off, source unset, hold 0.003 cycle, alphas
 *    1e-6 and 1e-4, three passes, 0.5 m, persist_ep 10, lapse_ep 0 (off), no trace, no
 *    acknowledgement);
 *  - Observe and the D1 Emit arm (source epoch with emit_measurement_only, the ambiguity-resolution
 *    slip fixes, every station on a sim bed) load on the fold's posture (the search on the
 *    cycle basis, float_kf, the held S-basis);
 *  - the fixed-cycle fold's refusals: Emit without emit_measurement_only, Emit with source
 *    lattice (step FCF-4 is not built), and an unnamed source; source lattice under Observe
 *    loads (step FCF-3) with its persist_ep and lapse_ep (lapse_ep named inert under source
 *    epoch); the fold's interlocks (no search, no cycle basis, the level vehicle, no held basis,
 *    carry_invariant, phase_datum_eq34, cycle_slip.turbo_edit,
 *    cycle_slip.relevel_delta_bridge, sd_ewl.fix_enable, the smoothed hold under Emit);
 *    malformed values;
 *  - the review fixes: Emit refuses a posture missing any of the slip fixes
 *    (and the knobs that make them act) and Observe names it; Emit refuses a state snapshot
 *    path (a snapshot written or resumed under Emit); Emit refuses a station off a sim bed (the
 *    acknowledgement is checked); a gross-LOS screen without its emit exclusion is named;
 *  - the producer AR record's warning: its text, byte for byte, with the fold off and under
 *    Observe; its Emit variant, in its place, under Emit.
 *
 * Suite name owned by this file alone (suite-hygiene rule in CLAUDE.md).
 */

#include <gtest/gtest.h>

#include "config/GnssConfig.hpp"
#include "logging/LogManager.hpp"

#include <spdlog/sinks/ostream_sink.h>
#include <spdlog/spdlog.h>

#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <string>

namespace GnssProcessor {
namespace {

namespace fs = std::filesystem;

/// The search on the arc graph's cycle basis.
const std::string kSearch =
    "\"enable\": true, \"tree_basis\": true, \"decorrelated_partial\": true, "
    "\"block_search\": true, \"cycle_basis\": true";

/// The search with the suspect-corner mask (the ambiguity_resolution half of the slip
/// fixes).
const std::string kSearchSuspect = kSearch + ", \"suspect_epochs\": 10";

/// The two stations, without RINEX paths (not a sim bed).
const std::string kStationsOffBed =
    "      { \"name\": \"AAAA\", \"position_ecef\": [ -2493304.99, -4655214.79, 3565497.67 ] },\n"
    "      { \"name\": \"BBBB\", \"position_ecef\": [ -2281621.75, -1453585.08, 5756964.89 ] }\n";

/// The two stations on a sim bed: each RINEX path in a directory that holds a
/// truth_ambiguities.csv (created once, in a temporary directory of its own).
std::string simBedStations() {
    const fs::path bed = fs::temp_directory_path() / "fcf_config_simbed";
    std::error_code ec;
    for (const char* sta : {"AAAA", "BBBB"}) {
        fs::create_directories(bed / sta, ec);
        std::ofstream(bed / sta / "truth_ambiguities.csv") << "tow,sat,sig,n\n";
    }
    auto path = [&](const char* sta) {
        return (bed / sta / (std::string(sta) + "_R_20241290000_01D_30S_MO.rnx")).generic_string();
    };
    return std::string("      { \"name\": \"AAAA\", \"rinex_path\": \"") + path("AAAA")
         + "\", \"position_ecef\": [ -2493304.99, -4655214.79, 3565497.67 ] },\n"
           "      { \"name\": \"BBBB\", \"rinex_path\": \"" + path("BBBB")
         + "\", \"position_ecef\": [ -2281621.75, -1453585.08, 5756964.89 ] }\n";
}

/// Card ARL section 13's slip fixes with the knobs that make them act, as the network key
/// @c cycle_slip (comma-led, for @c config's @p net_extra).  A non-empty @p key is given
/// @p value, or removed when @p value is empty.
std::string slipFixes(const std::string& key = "", const std::string& value = "") {
    std::map<std::string, std::string> kv = {{"if_phase_td", "true"},
                                             {"relevel_free_epoch", "true"},
                                             {"gf_arc_jump", "true"},
                                             {"gf_arc_jump_k", "6"},
                                             {"if_phase_td_reseed_on_flag", "true"}};
    if (!key.empty()) {
        if (value.empty()) kv.erase(key);
        else kv[key] = value;
    }
    std::string body;
    for (const auto& [k, v] : kv) body += (body.empty() ? "" : ", ") + ("\"" + k + "\": " + v);
    return ",\n    \"cycle_slip\": { " + body + " }";
}

/// A network config with @p ar_body as network.ambiguity_resolution and @p net_extra (a
/// comma-led list of network keys) on the fold's posture: float_kf on phase leveling with the
/// prioritized, held phase datum.
std::string config(const std::string& ar_body, const std::string& net_extra = "",
                   const std::string& vehicle = "float_kf", bool held_basis = true,
                   const std::string& stations = kStationsOffBed) {
    return std::string("{\n")
        + "  \"processing\": {\n"
          "    \"mode\": \"ppp-rtk-network\",\n"
          "    \"active_systems\": [ \"GPS\" ],\n"
          "    \"signal_priorities\": { \"GPS\": \"WC\" }\n"
          "  },\n"
          "  \"network\": {\n"
          "    \"clock_datum_station\": \"AAAA\",\n"
          "    \"phase_leveling\": true,\n"
          "    \"shared_carry_full_cov\": true,\n"
          "    \"carry_conditional_coupling\": true,\n"
          "    \"phase_datum_prioritized\": true,\n"
          "    \"ambiguity_vehicle\": \"" + vehicle + "\",\n"
          "    \"phase_datum_held_basis\": " + (held_basis ? "true" : "false") + net_extra + ",\n"
          "    \"ambiguity_resolution\": { " + ar_body + " },\n"
          "    \"stations\": [\n" + stations +
          "    ]\n"
          "  }\n"
          "}\n";
}

/// The fold block as a JSON object member.
std::string fold(const std::string& body) { return ", \"fixed_cycles\": { " + body + " }"; }

/// The D1 Emit arm's posture around @p fold_body: the search with the suspect mask, the slip
/// fixes (@p slip), @p net_extra, and both stations on a sim bed.
std::string emitPosture(const std::string& fold_body, const std::string& net_extra = "",
                        const std::string& slip = slipFixes()) {
    return config(kSearchSuspect + fold(fold_body), slip + net_extra, "float_kf", true,
                  simBedStations());
}

/// One load with the loader's two loggers captured: "gnss_config" (its warnings) and
/// "gnss_processor" (the refusal, logged where loadFromJson catches it).  An initialized manager
/// in a directory of its own, the two names dropped before and after (the pattern of
/// test_active_system_admission.cpp).
struct Loaded {
    bool            ok{false};
    std::string     text;
    NetworkArConfig ar;
};

Loaded load(const std::string& tag, const std::string& json) {
    Loaded out;
    const fs::path dir = fs::temp_directory_path() / ("fcf_config_" + tag);
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir, ec);
    const fs::path p = dir / "cfg.json";
    { std::ofstream f(p); f << json; }
    spdlog::drop("gnss_processor");
    spdlog::drop("gnss_config");
    {
        LogManager log;
        log.initialize(dir, LogLevel::Info);
        std::ostringstream oss;
        auto sink = std::make_shared<spdlog::sinks::ostream_sink_mt>(oss);
        auto cfg_log = log.getLogger("gnss_config");
        auto main_log = log.getLogger();
        cfg_log->sinks().push_back(sink);
        main_log->sinks().push_back(sink);
        GnssConfig cfg;
        out.ok = cfg.loadFromJson(p, log);
        cfg_log->flush();
        main_log->flush();
        out.text = oss.str();
        out.ar = cfg.getNetworkConfig().ambiguity_resolution;
    }
    spdlog::drop("gnss_processor");
    spdlog::drop("gnss_config");
    fs::remove_all(dir, ec);
    return out;
}

/// The D1 Emit arm's fold block.
const std::string kEmitD1 = "\"fold\": \"emit\", \"source\": \"epoch\", \"emit_measurement_only\": true";
/// Observe at source epoch.
const std::string kObserve = "\"fold\": \"observe\", \"source\": \"epoch\"";

/// The record's first clause (GnssConfigLoadNetwork.cpp, the producer AR warning).
constexpr const char* kRecord = "Network producer AR ARMED, but the emitted product stays FLOAT";
/// The Emit variant's first clause.
constexpr const char* kEmitVariant = "Network producer AR ARMED with fixed_cycles.fold EMIT";
/// The slip-fix refusal (Emit) and warning (Observe).
constexpr const char* kSlipRefusal = "requires the slip fixes of card ARL section 13";
constexpr const char* kSlipWarning = "fixed_cycles.fold observe runs without the slip fixes";

TEST(FixedCycleConfig, DefaultsAreTheUnarmedPosture) {
    const Loaded l = load("default", config(kSearch));
    ASSERT_TRUE(l.ok) << l.text;
    EXPECT_EQ(l.ar.fixed_cycle_fold, FixedCycleFold::Off);
    EXPECT_EQ(l.ar.fixed_cycle_source, FixedCycleSource::Unset);
    EXPECT_DOUBLE_EQ(l.ar.fixed_cycle_sigma_cyc, 0.003);
    EXPECT_DOUBLE_EQ(l.ar.fixed_cycle_unit_alpha, 1e-6);
    EXPECT_DOUBLE_EQ(l.ar.fixed_cycle_gate_alpha, 1e-4);
    EXPECT_EQ(l.ar.fixed_cycle_max_iter, 3);
    EXPECT_DOUBLE_EQ(l.ar.fixed_cycle_max_shift_m, 0.5);
    EXPECT_EQ(l.ar.fixed_cycle_persist_ep, 10);
    EXPECT_EQ(l.ar.fixed_cycle_lapse_ep, 0);
    EXPECT_FALSE(l.ar.fixed_cycle_trace);
    EXPECT_FALSE(l.ar.fixed_cycle_emit_measurement_only);
}

TEST(FixedCycleConfig, ObserveAndTheD1EmitArmLoadOnTheFoldPosture) {
    const Loaded o = load("observe", config(kSearch + fold(kObserve + ", "
                                                           "\"sigma_cyc\": 0.004, \"unit_alpha\": 1e-5, "
                                                           "\"gate_alpha\": 1e-3, \"max_iter\": 2, "
                                                           "\"max_shift_m\": 0.25, \"trace\": true")));
    ASSERT_TRUE(o.ok) << o.text;
    EXPECT_EQ(o.ar.fixed_cycle_fold, FixedCycleFold::Observe);
    EXPECT_EQ(o.ar.fixed_cycle_source, FixedCycleSource::Epoch);
    EXPECT_DOUBLE_EQ(o.ar.fixed_cycle_sigma_cyc, 0.004);
    EXPECT_DOUBLE_EQ(o.ar.fixed_cycle_unit_alpha, 1e-5);
    EXPECT_DOUBLE_EQ(o.ar.fixed_cycle_gate_alpha, 1e-3);
    EXPECT_EQ(o.ar.fixed_cycle_max_iter, 2);
    EXPECT_DOUBLE_EQ(o.ar.fixed_cycle_max_shift_m, 0.25);
    EXPECT_TRUE(o.ar.fixed_cycle_trace);
    EXPECT_NE(o.text.find("fixed-cycle fold ARMED, mode observe"), std::string::npos);
    const Loaded e = load("emit", emitPosture(kEmitD1));
    ASSERT_TRUE(e.ok) << e.text;
    EXPECT_EQ(e.ar.fixed_cycle_fold, FixedCycleFold::Emit);
    EXPECT_TRUE(e.ar.fixed_cycle_emit_measurement_only);
    EXPECT_EQ(e.text.find(kSlipRefusal), std::string::npos);
}

TEST(FixedCycleConfig, SectionTwoRefusals) {
    struct Case { const char* tag; std::string json; const char* why; };
    const Case cases[] = {
        {"emit_no_ack", emitPosture("\"fold\": \"emit\", \"source\": \"epoch\""), "emit_measurement_only"},
        {"emit_lattice", emitPosture("\"fold\": \"emit\", \"source\": \"lattice\", \"emit_measurement_only\": true"),
         "the hold from the ledger (step FCF-4) is not built"},
        {"no_source", config(kSearch + fold("\"fold\": \"observe\"")), "requires fixed_cycles.source"},
    };
    for (const Case& c : cases) {
        const Loaded l = load(c.tag, c.json);
        EXPECT_FALSE(l.ok) << c.tag;
        EXPECT_NE(l.text.find(c.why), std::string::npos) << c.tag << ": " << l.text;
        EXPECT_NE(l.text.find(""), std::string::npos) << c.tag;
    }
}

// Step FCF-3: source lattice runs under Observe, with the ledger's admission streak.
TEST(FixedCycleConfig, ObserveLatticeLoadsWithItsPersistEpAndLapseEp) {
    const Loaded l = load("observe_lattice",
                          config(kSearchSuspect + fold("\"fold\": \"observe\", \"source\": \"lattice\", "
                                                       "\"persist_ep\": 7, \"lapse_ep\": 5, \"trace\": true"),
                                 slipFixes()));
    ASSERT_TRUE(l.ok) << l.text;
    EXPECT_EQ(l.ar.fixed_cycle_source, FixedCycleSource::Lattice);
    EXPECT_EQ(l.ar.fixed_cycle_persist_ep, 7);
    EXPECT_EQ(l.ar.fixed_cycle_lapse_ep, 5);
    EXPECT_NE(l.text.find("source lattice (the L1/L2 pairs the ledger of fixed cycles implies, "
                          "admitted after 7 epoch(s)"), std::string::npos) << l.text;
    EXPECT_NE(l.text.find("a unit lapses after 5 epoch(s) the search does not determine its pair"),
              std::string::npos) << l.text;
    EXPECT_EQ(l.text.find("lapse_ep is set and INERT"), std::string::npos) << l.text;
    EXPECT_NE(l.text.find("logs/network_fixed_cycle_events.csv"), std::string::npos) << l.text;
    EXPECT_EQ(l.text.find(kSlipWarning), std::string::npos) << l.text;
    // Without the slip fixes Observe is named, in the ledger's words.
    const Loaded bare = load("observe_lattice_bare",
                             config(kSearch + fold("\"fold\": \"observe\", \"source\": \"lattice\"")));
    ASSERT_TRUE(bare.ok) << bare.text;
    EXPECT_NE(bare.text.find("so the ledger keeps holding it, also after the search stops fixing it"),
              std::string::npos) << bare.text;
    EXPECT_NE(bare.text.find("no lapse"), std::string::npos) << bare.text;
    // Under source epoch the lapse has nothing to act on: named inert.
    const Loaded ep = load("lapse_epoch", config(kSearch + fold(kObserve + ", \"lapse_ep\": 5")));
    ASSERT_TRUE(ep.ok) << ep.text;
    EXPECT_NE(ep.text.find("fixed_cycles.lapse_ep is set and INERT under source epoch"), std::string::npos)
        << ep.text;
}

TEST(FixedCycleConfig, TheFoldsInterlocksRefuse) {
    const std::string obs = fold(kObserve);
    const Loaded no_search = load("no_search", config("\"enable\": false" + obs));
    EXPECT_FALSE(no_search.ok);
    EXPECT_NE(no_search.text.find("requires network.ambiguity_resolution.enable"), std::string::npos);
    const Loaded no_basis = load("no_basis", config("\"enable\": true, \"tree_basis\": true" + obs));
    EXPECT_FALSE(no_basis.ok);
    EXPECT_NE(no_basis.text.find("requires network.ambiguity_resolution.cycle_basis"), std::string::npos);
    const Loaded no_held = load("no_held", config(kSearch + obs, "", "float_kf", false));
    EXPECT_FALSE(no_held.ok);
    EXPECT_NE(no_held.text.find("requires network.phase_datum_held_basis"), std::string::npos);
    // The level vehicle: the held basis itself refuses it first; the fold is refused either way.
    EXPECT_FALSE(load("level", config(kSearch + obs, "", "level", false)).ok);
    const Loaded smoothed = load("smoothed", emitPosture(
        kEmitD1,
        ",\n    \"emit_pb_hold\": true, \"emit_pb_hold_smoothed\": true, "
        "\"raim\": true, \"raim_within_epoch\": true, \"clock_temporal_anchor\": true, "
        "\"carry_coupling_exact_update\": true, \"carry_coupling_time_update\": true"));
    EXPECT_FALSE(smoothed.ok);
    EXPECT_NE(smoothed.text.find("refuses network.emit_pb_hold_smoothed"), std::string::npos) << smoothed.text;
}

// The interlocks that refuse another integer path, another phase datum or an integer repair of
// the float, each under Observe (the interlocks hold for any fold that is not off), each met by
// the fold's own refusal.  carry_invariant is refused earlier, by its own requirement of the
// eq34 datum (and float_kf refuses it too); the fold's refusal stands behind them, protecting
// the held solve's precondition (unfolded station factors).
TEST(FixedCycleConfig, TheRemainingInterlocksRefuse) {
    struct Case { const char* tag; std::string net_extra; const char* why; };
    const Case cases[] = {
        {"carry_invariant", ",\n    \"carry_invariant\": true", "network.carry_invariant"},
        {"eq34", ",\n    \"phase_datum_eq34\": true", "refuses network.phase_datum_eq34"},
        {"turbo_edit", ",\n    \"cycle_slip\": { \"turbo_edit\": true }", "refuses network.cycle_slip.turbo_edit"},
        // The bridge disarms itself without the free epoch, the MW detector and IF-TD.
        {"delta_bridge",
         ",\n    \"cycle_slip\": { \"relevel_free_epoch\": true, \"mw_detect\": true, "
         "\"if_phase_td\": true, \"relevel_delta_bridge\": true }",
         "refuses network.cycle_slip.relevel_delta_bridge"},
        {"sd_ewl_fix", ",\n    \"sd_ewl\": { \"enable\": true, \"fix_enable\": true }",
         "refuses network.sd_ewl.fix_enable"},
    };
    for (const Case& c : cases) {
        const Loaded l = load(std::string("ilk_") + c.tag, config(kSearch + fold(kObserve), c.net_extra));
        EXPECT_FALSE(l.ok) << c.tag;
        EXPECT_NE(l.text.find(c.why), std::string::npos) << c.tag << ": " << l.text;
    }
}

// Card FCFB section 5: a slip no detector flags leaves the zero-q float on the wrong integer,
// which the per-unit test and block acceptance cannot see, so Emit requires every slip fix (and
// the knobs that make two of them act) and Observe names a missing one.
TEST(FixedCycleConfig, EmitRequiresTheSlipFixesAndObserveNamesThem) {
    struct Case { const char* tag; std::string json; const char* named; };
    const Case cases[] = {
        {"no_gf", emitPosture(kEmitD1, "", slipFixes("gf_arc_jump", "")), "network.cycle_slip.gf_arc_jump"},
        {"gf_k8", emitPosture(kEmitD1, "", slipFixes("gf_arc_jump_k", "8")), "gf_arc_jump_k at most 6"},
        {"no_free", emitPosture(kEmitD1, "", slipFixes("relevel_free_epoch", "")),
         "network.cycle_slip.relevel_free_epoch"},
        {"no_iftd", emitPosture(kEmitD1, "", slipFixes("if_phase_td", "")), "network.cycle_slip.if_phase_td"},
        {"no_reseed", emitPosture(kEmitD1, "", slipFixes("if_phase_td_reseed_on_flag", "")),
         "network.cycle_slip.if_phase_td_reseed_on_flag"},
        {"no_suspect", config(kSearch + fold(kEmitD1), slipFixes(), "float_kf", true, simBedStations()),
         "suspect_epochs above 0"},
    };
    for (const Case& c : cases) {
        const Loaded e = load(std::string("slip_e_") + c.tag, c.json);
        EXPECT_FALSE(e.ok) << c.tag;
        EXPECT_NE(e.text.find(kSlipRefusal), std::string::npos) << c.tag << ": " << e.text;
        EXPECT_NE(e.text.find(c.named), std::string::npos) << c.tag << ": " << e.text;
    }
    // k 0 is the floor alone, a bar at least as tight as k 6: accepted.
    EXPECT_TRUE(load("slip_e_k0", emitPosture(kEmitD1, "", slipFixes("gf_arc_jump_k", "0"))).ok);
    // Observe: named, not refused; with the fixes, silent.
    const Loaded bare = load("slip_o_bare", config(kSearch + fold(kObserve)));
    ASSERT_TRUE(bare.ok) << bare.text;
    EXPECT_NE(bare.text.find(kSlipWarning), std::string::npos) << bare.text;
    const Loaded p1 = load("slip_o_p1", config(kSearchSuspect + fold(kObserve), slipFixes()));
    ASSERT_TRUE(p1.ok) << p1.text;
    EXPECT_EQ(p1.text.find(kSlipWarning), std::string::npos) << p1.text;
}

// Emit refuses a state snapshot path: a snapshot written under Emit would carry emitter state
// derived from held values into a later leg, and a resume into Emit would read state the
// snapshot does not carry.  Observe keeps the path.
TEST(FixedCycleConfig, EmitRefusesAStateSnapshotPath) {
    const std::string snap = ",\n    \"state_snapshot_path\": \"fcf_emit.snap\"";
    const Loaded e = load("snap_emit", emitPosture(kEmitD1, snap));
    EXPECT_FALSE(e.ok);
    EXPECT_NE(e.text.find("refuses network.state_snapshot_path"), std::string::npos) << e.text;
    const Loaded o = load("snap_observe", config(kSearchSuspect + fold(kObserve), slipFixes() + snap));
    EXPECT_TRUE(o.ok) << o.text;
}

// The acknowledgement is checked: Emit refuses a station whose RINEX file does not lie in a sim
// fixture (no path, or no truth_ambiguities.csv beside it).
TEST(FixedCycleConfig, EmitRequiresEveryStationOnASimBed) {
    const Loaded e = load("off_bed", config(kSearchSuspect + fold(kEmitD1), slipFixes()));
    EXPECT_FALSE(e.ok);
    EXPECT_NE(e.text.find("not on a sim bed: AAAA, BBBB"), std::string::npos) << e.text;
}

TEST(FixedCycleConfig, MalformedValuesAreRefused) {
    const char* const bodies[] = {"\"fold\": \"maybe\"", "\"source\": \"yesterday\"",
                                  "\"sigma_cyc\": 0", "\"unit_alpha\": 1", "\"gate_alpha\": 0",
                                  "\"max_iter\": -1", "\"max_shift_m\": 0", "\"persist_ep\": 0",
                                  "\"lapse_ep\": -1"};
    int k = 0;
    for (const char* b : bodies) {
        const Loaded l = load("bad" + std::to_string(k++), config(kSearch + fold(b)));
        EXPECT_FALSE(l.ok) << b;
        EXPECT_NE(l.text.find("network.ambiguity_resolution.fixed_cycles."), std::string::npos) << b;
    }
}

// persist_ep is read since step FCF-3 (the ledger runs under trace_lattice too), so it is no
// longer named inert.  A gross-LOS screen without its emit exclusion is named.
TEST(FixedCycleConfig, LateKnobsAreNamedAndPersistEpIsRead) {
    const Loaded pe = load("persist", config(kSearch + fold(kObserve + ", \"persist_ep\": 4")));
    ASSERT_TRUE(pe.ok) << pe.text;
    EXPECT_EQ(pe.ar.fixed_cycle_persist_ep, 4);
    EXPECT_EQ(pe.text.find("persist_ep is set and INERT"), std::string::npos) << pe.text;
    const char* const late = "acts after that epoch's fixed-cycle fold";
    const Loaded g = load("gross", config(kSearch + fold(kObserve), ",\n    \"gross_los_screen\": true"));
    ASSERT_TRUE(g.ok) << g.text;
    EXPECT_NE(g.text.find(late), std::string::npos) << g.text;
    const Loaded gx = load("gross_x", config(kSearch + fold(kObserve),
                                             ",\n    \"gross_los_screen\": true, \"gross_los_emit_exclude\": true"));
    ASSERT_TRUE(gx.ok) << gx.text;
    EXPECT_EQ(gx.text.find(late), std::string::npos) << gx.text;
}

TEST(FixedCycleConfig, TheRecordsWarningIsUnchangedUnlessEmit) {
    const Loaded off = load("warn_off", config(kSearch));
    const Loaded obs = load("warn_observe", config(kSearch + fold(kObserve)));
    const Loaded emit = load("warn_emit", emitPosture(kEmitD1));
    ASSERT_TRUE(off.ok && obs.ok && emit.ok) << emit.text;
    // The record's line, the same text with the fold off and under Observe.
    auto recordLine = [](const std::string& text) {
        const std::size_t a = text.find(kRecord);
        if (a == std::string::npos) return std::string();
        return text.substr(a, text.find('\n', a) - a);
    };
    const std::string l_off = recordLine(off.text), l_obs = recordLine(obs.text);
    ASSERT_FALSE(l_off.empty());
    EXPECT_EQ(l_off, l_obs);
    EXPECT_EQ(off.text.find(kEmitVariant), std::string::npos);
    EXPECT_EQ(obs.text.find(kEmitVariant), std::string::npos);
    // Under Emit the variant replaces it, and names the slip fixes as the guard against a slip
    // no detector flags.
    EXPECT_EQ(emit.text.find(kRecord), std::string::npos);
    EXPECT_NE(emit.text.find(kEmitVariant), std::string::npos);
    EXPECT_NE(emit.text.find("screened by a per-unit test"), std::string::npos);
    EXPECT_NE(emit.text.find("the guard is the slip fixes of card ARL section 13"), std::string::npos);
}

}  // namespace
}  // namespace GnssProcessor
