/**
 * @file test_network_rx_extra_band_delay.cpp
 * @brief Card L5B: @c network.estimate_rx_extra_band_code_delay, the knob that turns the
 *        receiver IFB drop of the declared rule @c RxIfbRule off
 *        (network/assembly/ParamInventory.hpp).
 *
 * Seven contracts:
 *  - the loader: default off, and parsed when set;
 *  - the one predicate (@c rxExtraBandTracked, @c rxExtraBandDelayEffective) and the rule
 *    built from it: the drop stays with the knob off, and with the knob armed where it
 *    cannot act (no satellite code biases, or no third band under the frequency mode, for
 *    every constellation); it goes, with the governed set unchanged, where it can;
 *  - the inventory: a governed instance estimated under the knob is noted as estimated by
 *    it, with no WARNING and its count in the estimated_local column, and the manifest
 *    record names the knob only when it acted (the default record's bytes are unchanged);
 *  - the snapshot fingerprint: its token only where the knob acts (the goldens' dual-band
 *    posture keeps an unarmed fingerprint);
 *  - the rows the split excludes: an extra-band code row whose satellite code bias is not
 *    a shared column is excluded (zero weight); registration edges and the family signals
 *    are kept per station; a family re-seed is recognized by signal;
 *  - the run-end report's lines: the inert reasons, the inventory's dispositions, the row
 *    and carry counts, the stations the carry never folded, and the carried values per
 *    (key, family) as a level and a datum-invariant spread;
 *  - the loader interlocks: inert without the satellite code biases and on every
 *    dual-band posture, not inert on the multi-band modes, the closing datum named in the
 *    posture line, the RXCB-DATUM line, and the warnings for the receiver code-bias datum,
 *    the pre-PD fix and the diagonal route's per-key tools; nothing printed with the knob
 *    off.
 */
#include <gtest/gtest.h>

#include "network/state/RunContext.hpp"   // the snapshot fingerprint (NetworkRunContext, NetStation)
#include "common/EpochMeasurement.hpp"
#include "config/GnssConfig.hpp"
#include "network/assembly/ParamInventory.hpp"
#include "filter/StateKeySchema.hpp"
#include "filter/StateManager.hpp"
#include "logging/LogManager.hpp"

#include <spdlog/sinks/ostream_sink.h>
#include <spdlog/spdlog.h>
#include <nlohmann/json.hpp>

#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

using namespace GnssProcessor;
namespace K  = GnssProcessor::StateKeys;
namespace fs = std::filesystem;

namespace {

using PD = ParamDisposition;

constexpr const char* kKnob = "network.estimate_rx_extra_band_code_delay";

/// The temp path of one test's file, a fixed name per tag (the pattern of
/// test_codebias_network_config.cpp), so repeated runs reuse it instead of accumulating.
fs::path tempPath(const std::string& tag, const std::string& ext) {
    return fs::temp_directory_path() / ("l5b_rxifb_" + tag + ext);
}

/// A minimal loadable network config.  @p frequency_mode is the
/// processing.frequency_mode value, @p systems the active_systems JSON array body, and
/// @p network_extra is spliced into the network block.
std::string networkConfig(const std::string& frequency_mode, const std::string& network_extra,
                          const std::string& systems = "\"GPS\"") {
    return std::string("{\n") +
        "  \"processing\": {\n"
        "    \"mode\": \"ppp-rtk-network\",\n"
        "    \"frequency_mode\": \"" + frequency_mode + "\",\n"
        "    \"active_systems\": [ " + systems + " ],\n"
        "    \"signal_priorities\": { \"GPS\": \"WC\" }\n"
        "  },\n"
        "  \"network\": {\n"
        "    \"clock_datum_station\": \"AAAA\",\n"
        + network_extra +
        "    \"stations\": [\n"
        "      { \"name\": \"AAAA\", \"position_ecef\": [ -2493304.99, -4655214.79, 3565497.67 ] },\n"
        "      { \"name\": \"BBBB\", \"position_ecef\": [ -2281621.75, -1453585.08, 5756964.89 ] }\n"
        "    ]\n"
        "  }\n"
        "}\n";
}

/// The W6 posture of the goldens (no ionosphere constraint), as JSON lines.
const std::string kW6 = "    \"estimate_sat_code_bias\": true,\n"
                        "    \"w6_iono_constraint\": \"none\",\n";
const std::string kArm = "    \"estimate_rx_extra_band_code_delay\": true,\n";
const std::string kRxDatum = "    \"rx_code_bias_datum\": true,\n";
const std::string kPrePd = "    \"station_prior_before_pd\": true,\n";

/// What a load returned and what the "gnss_config" logger printed.
struct CapturedLoad {
    bool        ok{false};
    std::string text;
    GnssConfig  cfg;
};

/// Load a config with the loader's logger captured (the pattern of
/// test_codebias_network_config.cpp: an initialized manager, one directory per tag removed
/// before use, and only the two logger names this helper creates dropped before and after;
/// spdlog::drop_all would fault other suites).  @p tag must be unique per call.
CapturedLoad loadCapturing(const std::string& tag, const std::string& frequency_mode,
                           const std::string& network_extra,
                           const std::string& systems = "\"GPS\"") {
    CapturedLoad out;
    const fs::path p = tempPath(tag, ".json");
    { std::ofstream f(p); f << networkConfig(frequency_mode, network_extra, systems); }
    const fs::path dir = tempPath(tag + "_log", "");
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir, ec);
    LogManager log;
    spdlog::drop("gnss_processor");
    spdlog::drop("gnss_config");
    log.initialize(dir, LogLevel::Info);
    auto cfg_log = log.getLogger("gnss_config");
    std::ostringstream oss;
    cfg_log->sinks().push_back(std::make_shared<spdlog::sinks::ostream_sink_mt>(oss));
    out.ok = out.cfg.loadFromJson(p, log);
    cfg_log->flush();
    out.text = oss.str();
    spdlog::drop("gnss_processor");
    spdlog::drop("gnss_config");
    fs::remove(p, ec);
    return out;
}

/// Load without capturing (the rule and parse tests).
GnssConfig load(const std::string& tag, const std::string& frequency_mode,
                const std::string& network_extra, const std::string& systems = "\"GPS\"") {
    LogManager log;
    const fs::path p = tempPath(tag, ".json");
    { std::ofstream f(p); f << networkConfig(frequency_mode, network_extra, systems); }
    GnssConfig cfg;
    EXPECT_TRUE(cfg.loadFromJson(p, log)) << tag;
    std::error_code ec;
    fs::remove(p, ec);
    return cfg;
}

bool has(const std::string& text, const std::string& what) {
    return text.find(what) != std::string::npos;
}

bool anyLineHas(const std::vector<std::string>& lines, const std::string& what) {
    for (const auto& l : lines)
        if (has(l, what)) return true;
    return false;
}

ParamInstanceId codeBias(const std::string& key) {
    const auto id = classifyStateColumn(StateContainer::CodeBiasStates, key);
    EXPECT_TRUE(id.has_value()) << key;
    return id.value_or(ParamInstanceId{});
}

/// A station slot that reached the split, with one record per (key, disposition).
StationParamSlot slotOf(const std::vector<std::pair<std::string, PD>>& cols) {
    StationParamSlot s;
    s.filled = true;
    for (const auto& [key, d] : cols) s.records.push_back({codeBias(key), d});
    return s;
}

/// The rule of a run where the knob acts (the goldens' ionosphere posture).
RxIfbRule effectiveRule() {
    RxIfbRule r;
    r.drop               = false;
    r.estimated_by_knob  = true;
    r.extra_band_tracked = true;
    r.w6_iono_none       = true;
    return r;
}

/// The text of the report lines, with "W:" before the warnings.
std::vector<std::string> texts(const std::vector<RxIfbReportLine>& lines) {
    std::vector<std::string> out;
    for (const auto& l : lines) out.push_back((l.warn ? "W:" : "I:") + l.text);
    return out;
}

}  // namespace

// ---------------------------------------------------------------------------
// The loader
// ---------------------------------------------------------------------------

TEST(NetworkRxExtraBandDelayTest, LoaderDefaultIsOffAndTheKeyParses) {
    EXPECT_FALSE(load("default", "triple", kW6).getNetworkConfig().estimate_rx_extra_band_code_delay);
    EXPECT_TRUE(load("armed", "triple", kW6 + kArm).getNetworkConfig().estimate_rx_extra_band_code_delay);
}

// ---------------------------------------------------------------------------
// The one predicate and the rule
// ---------------------------------------------------------------------------

TEST(NetworkRxExtraBandDelayTest, ExtraBandTrackedOnlyWithAThirdCdmaBand) {
    using FM = FrequencyMode;
    // Dual: every constellation's two bands are its clock-reference pair.
    for (const char* sys : {"GPS", "Galileo", "BeiDou", "QZSS", "GLONASS", "SBAS"})
        EXPECT_FALSE(rxExtraBandTracked(FM::Dual, {sys})) << sys;
    EXPECT_FALSE(rxExtraBandTracked(
        FM::Dual, {"GPS", "Galileo", "GLONASS", "BeiDou", "QZSS", "SBAS"}));
    // Triple and wider: a third band for every CDMA constellation but SBAS.
    for (const FM m : {FM::Triple, FM::All, FM::Independent}) {
        for (const char* sys : {"GPS", "Galileo", "BeiDou", "QZSS"})
            EXPECT_TRUE(rxExtraBandTracked(m, {sys})) << sys;
        EXPECT_FALSE(rxExtraBandTracked(m, {"GLONASS"}));   // FDMA: the per-slot form only
        EXPECT_FALSE(rxExtraBandTracked(m, {"SBAS"}));      // two bands in every mode
        EXPECT_TRUE(rxExtraBandTracked(m, {"GLONASS", "GPS"}));
    }
    EXPECT_FALSE(rxExtraBandTracked(FM::Triple, {"NotASystem"}));
    EXPECT_FALSE(rxExtraBandTracked(FM::Triple, {}));
}

TEST(NetworkRxExtraBandDelayTest, RuleDropsWithSatCodeBiasAndTheKnobOff) {
    const GnssConfig cfg = load("drop", "triple", kW6);
    const RxIfbRule r = networkRxIfbRule(cfg);
    EXPECT_TRUE(r.drop);
    EXPECT_FALSE(r.estimated_by_knob);
    EXPECT_FALSE(rxExtraBandDelayEffective(cfg));
    EXPECT_TRUE(r.drops(codeBias(K::makeIfbKey("GPS", '5'))));
    EXPECT_FALSE(r.drops(codeBias(K::makeDcbKey("GPS"))));   // the pair's DCB is never dropped
}

TEST(NetworkRxExtraBandDelayTest, RuleDoesNotDropWithTheKnobOn) {
    const GnssConfig cfg = load("keep", "triple", kW6 + kArm);
    EXPECT_TRUE(rxExtraBandDelayEffective(cfg));
    const RxIfbRule r = networkRxIfbRule(cfg);
    EXPECT_FALSE(r.drop);
    EXPECT_TRUE(r.estimated_by_knob);
    EXPECT_TRUE(r.extra_band_tracked);
    EXPECT_STREQ(r.estimated_by, kKnob);
    for (const std::string key : {K::makeIfbKey("GPS", '5'), K::makeIfbKey("Galileo", '7'),
                                  K::makeIfbKey("Galileo", '6')}) {
        const ParamInstanceId id = codeBias(key);
        EXPECT_TRUE(RxIfbRule::governs(id.cls)) << key;   // the governed set is unchanged
        EXPECT_FALSE(r.drops(id)) << key;
    }
    // The GLONASS per-slot form stays outside the rule either way.
    EXPECT_FALSE(RxIfbRule::governs(codeBias(K::makeGlonassPerSlotIfbKey("GLONASS", '1', 3)).cls));
}

TEST(NetworkRxExtraBandDelayTest, KnobWithoutSatCodeBiasRecordsNothing) {
    // No satellite code bias: nothing is dropped, so the knob turned nothing off.
    const GnssConfig cfg = load("nosat", "triple", kArm);
    EXPECT_FALSE(rxExtraBandDelayEffective(cfg));
    const RxIfbRule r = networkRxIfbRule(cfg);
    EXPECT_FALSE(r.drop);
    EXPECT_FALSE(r.estimated_by_knob);
}

TEST(NetworkRxExtraBandDelayTest, KnobOnADualBandPostureKeepsTheUnarmedRule) {
    // The goldens' frequency mode: the knob cannot act, so the rule (and with it the
    // fingerprint token and the manifest's inventory record) is the unarmed one.
    for (const std::string systems : {"\"GPS\"", "\"Galileo\"", "\"GPS\", \"Galileo\"",
                                      "\"BeiDou\"", "\"QZSS\""}) {
        const GnssConfig cfg = load("dualrule", "dual", kW6 + kArm, systems);
        EXPECT_FALSE(rxExtraBandDelayEffective(cfg)) << systems;
        const RxIfbRule armed = networkRxIfbRule(cfg);
        const RxIfbRule unarmed = networkRxIfbRule(load("dualrule0", "dual", kW6, systems));
        EXPECT_TRUE(armed.drop) << systems;
        EXPECT_FALSE(armed.estimated_by_knob) << systems;
        EXPECT_FALSE(armed.extra_band_tracked) << systems;
        EXPECT_EQ(armed.drop, unarmed.drop) << systems;
        const ParamInventoryTally empty;
        EXPECT_EQ(paramInventoryJson(empty, armed, "run"), paramInventoryJson(empty, unarmed, "run"))
            << systems;
    }
}

// ---------------------------------------------------------------------------
// The inventory: the note and the manifest record
// ---------------------------------------------------------------------------

TEST(NetworkRxExtraBandDelayTest, InventoryNotesAGovernedInstanceAsEstimatedByTheKnob) {
    ParamInventoryTally t;
    for (int s = 0; s < 3; ++s)
        t.addStation(s, slotOf({{K::makeDcbKey("GPS"), PD::EstimatedLocal},
                                {K::makeIfbKey("GPS", '5'), PD::EstimatedLocal}}));
    t.epochs = 1;

    const RxIfbRule rule = effectiveRule();
    const auto lines = paramInventoryTable(t, rule, "run");
    EXPECT_TRUE(anyLineHas(lines, "rule rx_ifb: estimated (network.estimate_rx_extra_band_code_delay); "
                                  "not dropped by network.estimate_sat_code_bias"));
    EXPECT_FALSE(anyLineHas(lines, "WARNING"));   // the premise is moot: nothing is held
    EXPECT_TRUE(anyLineHas(lines, "receiver IFB, GPS L5"));

    // The class record's counts, in disposition order (estimated_border, estimated_local,
    // held_at_seed, absorbed, pinned, dropped, unobserved): three estimated_local.
    const std::string j = paramInventoryJson(t, rule, "run");
    EXPECT_TRUE(has(j, "\"label\":\"receiver IFB, GPS L5\",\"counts\":[0,3,0,0,0,0,0]")) << j;
    EXPECT_TRUE(has(j, "\"premise_holds\":false,\"estimated_by\":\"network.estimate_rx_extra_band_code_delay\"}],"
                       "\"reasons\":[")) << j;

    // The default rule (knob off): the former note and a record without the field.
    RxIfbRule off;
    off.drop = true;
    EXPECT_TRUE(anyLineHas(paramInventoryTable(t, off, "run"), "rule rx_ifb: held_at_seed"));
    const std::string j_off = paramInventoryJson(t, off, "run");
    EXPECT_FALSE(has(j_off, "estimated_by")) << j_off;
    EXPECT_TRUE(has(j_off, "\"premise_holds\":false}],\"reasons\":[")) << j_off;
    RxIfbRule none;   // no satellite code bias
    EXPECT_TRUE(anyLineHas(paramInventoryTable(t, none, "run"),
                           "rule rx_ifb: not armed (network.estimate_sat_code_bias off), estimated"));
}

// ---------------------------------------------------------------------------
// The row plan of the split
// ---------------------------------------------------------------------------

TEST(NetworkRxExtraBandDelayTest, RowsWithoutASharedSatelliteColumnAreExcluded) {
    LogManager lm;
    GnssConfig cfg;
    StateManager sm{5, lm, cfg};
    const int ifb = static_cast<int>(sm.addCodeBiasState(K::makeIfbKey("GPS", '5'), 0.0));
    const int dcb = static_cast<int>(sm.addCodeBiasState(K::makeDcbKey("GPS"), 0.0));
    const int b03 = static_cast<int>(sm.addSatBiasState(K::makeSatBiasKey("G03", "C5Q"), 0.0));
    const int b08 = static_cast<int>(sm.addSatBiasState(K::makeSatBiasKey("G08", "C5Q"), 0.0));
    // G06 C5Q: no satellite column at this station (pruned in PASS A).
    const int n = static_cast<int>(sm.getStateSize());

    // Rows: G03 C5Q, G06 C5Q, G08 C5Q (the IFB's rows), and G03 C1C (a pair-band row).
    EpochMeasurement em;
    em.H = Eigen::MatrixXd::Zero(4, n);
    em.satellite_ids = {"G03", "G06", "G08", "G03"};
    em.signal_types  = {"C5Q", "C5Q", "C5Q", "C1C"};
    em.H(0, ifb) = 1.0; em.H(0, b03) = 1.0;
    em.H(1, ifb) = 1.0;
    em.H(2, ifb) = 1.0; em.H(2, b08) = 1.0;
    em.H(3, dcb) = -1.5457;

    // G03 shared; G08 not (a CB-ADM sponge: in the state manager, not in the border).
    RxIfbStationState s;
    EXPECT_EQ(rxIfbExcludedRows(s, sm, em, std::set<int>{b03}), (std::vector<int>{1, 2}));
    EXPECT_EQ(s.rows_unregistered, 2);
    EXPECT_EQ(s.cols_unregistered, 0);
    EXPECT_EQ(s.reg_edges + s.dereg_edges, 0);   // first sighting of every row: no edge
    EXPECT_EQ(s.signals.at(K::makeIfbKey("GPS", '5')), (std::set<std::string>{"C5Q"}));

    // Next epoch G08 registers and G03 leaves the border: one edge each way.
    EXPECT_EQ(rxIfbExcludedRows(s, sm, em, std::set<int>{b08}), (std::vector<int>{0, 1}));
    EXPECT_EQ(s.reg_edges, 1);
    EXPECT_EQ(s.dereg_edges, 1);

    // No shared satellite column at all: every IFB row is excluded, and the column with them.
    EXPECT_EQ(rxIfbExcludedRows(s, sm, em, std::set<int>{}), (std::vector<int>{0, 1, 2}));
    EXPECT_EQ(s.cols_unregistered, 1);
    EXPECT_EQ(s.dereg_edges, 2);
    EXPECT_EQ(s.rows_unregistered, 2 + 2 + 3);
}

TEST(NetworkRxExtraBandDelayTest, FamilyReseedIsRecognizedBySignal) {
    RxIfbStationState q, x;
    q.signals[K::makeIfbKey("GPS", '5')] = {"C5Q"};
    x.signals[K::makeIfbKey("GPS", '5')] = {"C5X"};
    const std::set<std::string> reseeded{"G|C5Q"};
    EXPECT_TRUE(rxIfbFamilyReseeded(q, K::makeIfbKey("GPS", '5'), reseeded));
    EXPECT_FALSE(rxIfbFamilyReseeded(x, K::makeIfbKey("GPS", '5'), reseeded));
    EXPECT_FALSE(rxIfbFamilyReseeded(q, K::makeIfbKey("GPS", '5'), {}));
    EXPECT_FALSE(rxIfbFamilyReseeded(q, K::makeIfbKey("GPS", '5'), {"E|C5Q"}));   // another constellation
    EXPECT_FALSE(rxIfbFamilyReseeded(q, K::makeDcbKey("GPS"), reseeded));          // not governed
}

// ---------------------------------------------------------------------------
// The run-end report's lines
// ---------------------------------------------------------------------------

TEST(NetworkRxExtraBandDelayTest, ReportNamesWhyAnArmedKnobWasInert) {
    const ParamInventoryTally t;
    RxIfbRule nosat;   // no satellite code biases: nothing to turn off
    nosat.extra_band_tracked = true;
    auto l = texts(rxExtraBandDelayReport(nosat, t, {}, {}, true, true));
    ASSERT_EQ(l.size(), 1u);
    EXPECT_TRUE(has(l[0], "W:network.estimate_rx_extra_band_code_delay was ARMED and INERT")) << l[0];
    EXPECT_TRUE(has(l[0], "network.estimate_sat_code_bias is off")) << l[0];

    RxIfbRule dual;   // W6 on, no third band
    dual.drop = true;
    l = texts(rxExtraBandDelayReport(dual, t, {}, {}, true, true));
    ASSERT_EQ(l.size(), 1u);
    EXPECT_TRUE(has(l[0], "no active constellation tracks a band outside a two-band pair")) << l[0];
}

TEST(NetworkRxExtraBandDelayTest, ReportWarnsWhenNoColumnWasEstimatedOrOneWasHeld) {
    ParamInventoryTally t;
    t.addStation(0, slotOf({{K::makeIfbKey("GPS", '5'), PD::Unobserved}}));
    auto l = texts(rxExtraBandDelayReport(effectiveRule(), t, {}, {}, true, true));
    EXPECT_TRUE(anyLineHas(l, "W:network.estimate_rx_extra_band_code_delay is armed and effective, "
                              "but no receiver IFB column was estimated")) << l.back();

    t.addStation(1, slotOf({{K::makeIfbKey("GPS", '5'), PD::EstimatedLocal}}));
    t.addStation(2, slotOf({{K::makeIfbKey("GPS", '5'), PD::HeldAtSeed}}));
    l = texts(rxExtraBandDelayReport(effectiveRule(), t, {}, {}, true, true));
    EXPECT_TRUE(anyLineHas(l, "I:receiver IFB, GPS L5: estimated in the local block on 1 "
                              "station-epoch(s); unobserved on 1"));
    EXPECT_TRUE(anyLineHas(l, "held at the seed on 1; 3 station(s)"));
    EXPECT_TRUE(anyLineHas(l, "W:receiver IFB, GPS L5: 1 station-epoch(s) HELD at the seed"));
    EXPECT_FALSE(anyLineHas(l, "no receiver IFB column was estimated"));
}

TEST(NetworkRxExtraBandDelayTest, ReportCountsRowsCarryAndSplitsValuesByFamily) {
    ParamInventoryTally t;
    for (int s = 0; s < 4; ++s)
        t.addStation(s, slotOf({{K::makeIfbKey("GPS", '5'), PD::EstimatedLocal}}));
    const std::string key = K::makeIfbKey("GPS", '5');
    std::vector<RxIfbStationState> st(4);
    for (int s = 0; s < 4; ++s) {
        st[s].est_epochs = 10;
        st[s].blk_folds  = 9;
        st[s].signals[key] = {s < 3 ? "C5Q" : "C5X"};
    }
    st[0].rows_unregistered = 5; st[0].cols_unregistered = 1;
    st[0].reg_edges = 2;         st[0].dereg_edges = 3;
    st[1].recouple = 1;          st[2].reseed_erase = 4;
    st[2].blk_folds = 0;         // estimated on ten epochs, never folded
    std::vector<std::pair<std::string, const RxIfbStationState*>> stations;
    const char* names[] = {"AAAA", "BBBB", "CCCC", "DDDD"};
    for (int s = 0; s < 4; ++s) stations.emplace_back(names[s], &st[s]);
    // C5Q: 1.0, 1.5, 3.0 (median 1.5; spread 0.5, 1.5); C5X: -2.0.
    const std::map<std::pair<int, std::string>, double> carried{
        {{0, key}, 1.0}, {{1, key}, 1.5}, {{2, key}, 3.0}, {{3, key}, -2.0},
        {{0, K::makeDcbKey("GPS")}, 7.0}};   // the DCB is not reported

    auto l = texts(rxExtraBandDelayReport(effectiveRule(), t, stations, carried, true, true));
    EXPECT_TRUE(anyLineHas(l, "I:5 extra-band code row(s) excluded at zero weight"));
    EXPECT_TRUE(anyLineHas(l, "1 station-epoch(s) whose IFB column had no other row (unobserved); "
                              "2 registration and 3 deregistration edge(s)"));
    EXPECT_TRUE(anyLineHas(l, "carry: 27 fold(s) as rows of a factorized station block"));
    EXPECT_TRUE(anyLineHas(l, "1 re-coupling(s) after at least one epoch out of the block"));
    EXPECT_TRUE(anyLineHas(l, "4 carry entry erase(s)"));
    EXPECT_TRUE(anyLineHas(l, "W:the receiver code-bias carry never folded the receiver IFB at 1 "
                              "station(s) estimated on more than one epoch (CCCC)"));
    EXPECT_TRUE(anyLineHas(l, "I:GPS_IFB5 (C5Q) carried at 3 station(s) at run end: median 1.500 m"));
    EXPECT_TRUE(anyLineHas(l, "|value - median| p90 1.500 m, max 1.500 m"));
    EXPECT_TRUE(anyLineHas(l, "I:GPS_IFB5 (C5X) carried at 1 station(s) at run end: median -2.000 m"));
    EXPECT_FALSE(anyLineHas(l, "GPS_DCB"));

    // The diagonal route names its folds and withholds instead.
    st[0].diag_folds = 6; st[0].diag_innov_skips = 2;
    l = texts(rxExtraBandDelayReport(effectiveRule(), t, stations, carried, true, false));
    EXPECT_TRUE(anyLineHas(l, "carry: 6 diagonal fold(s), 2 withheld by the inject screen"));
    // With the carry off there is nothing to fold, and no station is named.
    l = texts(rxExtraBandDelayReport(effectiveRule(), t, stations, {}, false, false));
    EXPECT_TRUE(anyLineHas(l, "I:carry: off (network.carry_rx_code_bias)"));
    EXPECT_FALSE(anyLineHas(l, "never folded"));
}

// ---------------------------------------------------------------------------
// The loader interlocks and the posture line
// ---------------------------------------------------------------------------

TEST(NetworkRxExtraBandDelayTest, LoaderSaysNothingWithTheKnobOff) {
    const CapturedLoad c = loadCapturing("off", "triple", kW6 + kRxDatum);
    ASSERT_TRUE(c.ok);
    EXPECT_FALSE(has(c.text, "card L5B")) << c.text;
    // The RXCB-DATUM line keeps its unarmed text.
    EXPECT_TRUE(has(c.text, "but the receiver half is already determined through the satellite "
                            "gauge here."))
        << c.text;
}

TEST(NetworkRxExtraBandDelayTest, LoaderWarnsInertWithoutSatCodeBias) {
    const CapturedLoad c = loadCapturing("nosat_cap", "triple", kArm);
    ASSERT_TRUE(c.ok);
    EXPECT_TRUE(has(c.text, "estimate_rx_extra_band_code_delay is INERT without "
                            "network.estimate_sat_code_bias")) << c.text;
    EXPECT_FALSE(has(c.text, "is ESTIMATED")) << c.text;
}

TEST(NetworkRxExtraBandDelayTest, LoaderWarnsInertOnEveryDualBandPosture) {
    int k = 0;
    for (const std::string systems : {"\"GPS\"", "\"Galileo\"", "\"GPS\", \"Galileo\"",
                                      "\"BeiDou\"", "\"GPS\", \"BeiDou\", \"QZSS\""}) {
        const CapturedLoad c = loadCapturing("dual" + std::to_string(k++), "dual",
                                             kW6 + kArm + kRxDatum, systems);
        ASSERT_TRUE(c.ok) << systems;
        EXPECT_TRUE(has(c.text, "estimate_rx_extra_band_code_delay is INERT on "
                                "processing.frequency_mode dual with active systems ["))
            << systems << "\n" << c.text;
        EXPECT_FALSE(has(c.text, "is ESTIMATED")) << systems;
        EXPECT_FALSE(has(c.text, "LOAD-BEARING on this run")) << systems;
    }
}

TEST(NetworkRxExtraBandDelayTest, LoaderIsNotInertOnTheMultiBandModes) {
    for (const std::string mode : {"triple", "all", "independent"}) {
        const CapturedLoad c = loadCapturing("multi_" + mode, mode, kW6 + kArm + kRxDatum + kPrePd);
        ASSERT_TRUE(c.ok) << mode;
        EXPECT_FALSE(has(c.text, "estimate_rx_extra_band_code_delay is INERT")) << mode;
        EXPECT_TRUE(has(c.text, "is ESTIMATED as a local column")) << mode;
    }
}

TEST(NetworkRxExtraBandDelayTest, LoaderNamesTheSumToZeroClosureWithoutTheGfDatum) {
    const CapturedLoad c = loadCapturing("s2z", "triple", kW6 + kArm + kRxDatum + kPrePd);
    ASSERT_TRUE(c.ok);
    EXPECT_TRUE(has(c.text, "is ESTIMATED as a local column")) << c.text;
    EXPECT_TRUE(has(c.text, "An extra-band code row whose satellite code bias is not a shared "
                            "column carries that bias unmodelled and is excluded (zero weight)"))
        << c.text;
    EXPECT_TRUE(has(c.text, "per connected component of that family's station-satellite graph"))
        << c.text;
    EXPECT_TRUE(has(c.text, "per-(constellation, signal) sum-to-zero rows of the satellite "
                            "code-bias datum (network.sat_gf_datum none); one row per signal"))
        << c.text;
    EXPECT_TRUE(has(c.text, "closed by network.rx_code_bias_datum (sigma 10.0 m)")) << c.text;
    EXPECT_TRUE(has(c.text, "not carried (network.carry_rx_code_bias off)")) << c.text;
    // The RXCB-DATUM line says the datum is load-bearing, not redundant.
    EXPECT_TRUE(has(c.text, "LOAD-BEARING on this run")) << c.text;
    EXPECT_FALSE(has(c.text, "this datum is load-bearing only under")) << c.text;
    EXPECT_FALSE(has(c.text, "estimate_rx_extra_band_code_delay is INERT")) << c.text;
    EXPECT_FALSE(has(c.text, "network.rx_code_bias_datum is OFF")) << c.text;
    EXPECT_FALSE(has(c.text, "network.station_prior_before_pd is OFF")) << c.text;
}

TEST(NetworkRxExtraBandDelayTest, LoaderNamesTheAssertionClosureUnderTheGfDatum) {
    const CapturedLoad a = loadCapturing("t0", "triple",
        kW6 + kArm + kRxDatum + kPrePd + "    \"sat_gf_datum\": \"broadcast_tgd\",\n");
    ASSERT_TRUE(a.ok);
    EXPECT_TRUE(has(a.text, "the per-signal assertion rows of network.sat_gf_datum")) << a.text;

    const CapturedLoad b = loadCapturing("t1", "triple",
        kW6 + kArm + kRxDatum + kPrePd + "    \"sat_gf_datum\": \"broadcast_tgd\",\n"
                                         "    \"sat_gf_extra_band_free\": true,\n");
    ASSERT_TRUE(b.ok);
    EXPECT_TRUE(has(b.text, "sum-to-zero rows of the satellite code-bias datum "
                            "(network.sat_gf_extra_band_free on)")) << b.text;
}

TEST(NetworkRxExtraBandDelayTest, LoaderWarnsWithoutTheReceiverCodeBiasDatumOrThePrePdFix) {
    const CapturedLoad c = loadCapturing("nodatum", "triple", kW6 + kArm);
    ASSERT_TRUE(c.ok);
    EXPECT_TRUE(has(c.text, "closed by network.rx_code_bias_datum, which is OFF on this run"))
        << c.text;
    EXPECT_TRUE(has(c.text, "network.rx_code_bias_datum is OFF.  With the receiver extra-band "
                            "code delay estimated")) << c.text;

    const CapturedLoad d = loadCapturing("noprepd", "triple", kW6 + kArm + kRxDatum);
    ASSERT_TRUE(d.ok);
    EXPECT_TRUE(has(d.text, "network.station_prior_before_pd is OFF.  The receiver DCB datum is "
                            "the only closure")) << d.text;
}

TEST(NetworkRxExtraBandDelayTest, LoaderReportsTheDiagonalCarry) {
    const std::string carry = "    \"carry_rx_code_bias\": true,\n"
                              "    \"code_datum_time_linked\": true,\n";
    const CapturedLoad plain = loadCapturing("diag", "triple", kW6 + kArm + kRxDatum + kPrePd + carry);
    ASSERT_TRUE(plain.ok);
    EXPECT_TRUE(has(plain.text, "carried as a diagonal prior (network.carry_rx_code_bias)"))
        << plain.text;
}

// ---------------------------------------------------------------------------
// The snapshot fingerprint
// ---------------------------------------------------------------------------

namespace {

/// Load the field golden with @p edit applied (the pattern of test_exl_switch_on_resume.cpp).
bool loadFieldGoldenWith(const std::function<void(nlohmann::json&)>& edit, GnssConfig& cfg,
                         const std::string& tag) {
    const fs::path src = fs::path(GNSS_PROJECT_ROOT) / "config" / "golden" / "golden_field_refound.json";
    std::ifstream in(src);
    nlohmann::json j = nlohmann::json::parse(in, nullptr, true, /*ignore_comments=*/true);
    edit(j);
    const fs::path p = tempPath("golden_" + tag, ".json");
    { std::ofstream out(p); out << j.dump(2); }
    LogManager log;
    const bool ok = cfg.loadFromJson(p, log);
    std::error_code ec;
    fs::remove(p, ec);
    return ok;
}

/// The live snapshot fingerprint a run of @p cfg would compute.
uint64_t liveFingerprint(const GnssConfig& cfg) {
    NetworkRunContext ctx(cfg, cfg.getNetworkConfig());
    ctx.carry.snap_path = "state_snapshot.snap";   // the builder is a no-op without a path
    std::vector<NetStation> st(2);
    st[0].name = "AAAA"; st[0].is_pivot = true;
    st[1].name = "BBBB"; st[1].is_pivot = false;
    networkSnapshotFingerprint(ctx, cfg, st);
    return ctx.carry.live_fingerprint;
}

}  // namespace

TEST(NetworkRxExtraBandDelayTest, FingerprintTokenOnlyWhereTheKnobActs) {
    const auto arm = [](nlohmann::json& j) { j["network"]["estimate_rx_extra_band_code_delay"] = true; };
    const auto triple = [](nlohmann::json& j) { j["processing"]["frequency_mode"] = "triple"; };

    // The goldens' dual-band posture: armed, the knob cannot act, so a snapshot written
    // without it still resumes (the fingerprint is the unarmed one).
    GnssConfig dual_off, dual_on;
    ASSERT_TRUE(loadFieldGoldenWith([](nlohmann::json&) {}, dual_off, "dual_off"));
    ASSERT_TRUE(loadFieldGoldenWith(arm, dual_on, "dual_on"));
    ASSERT_FALSE(rxExtraBandDelayEffective(dual_on));
    EXPECT_NE(liveFingerprint(dual_off), 0u);
    EXPECT_EQ(liveFingerprint(dual_on), liveFingerprint(dual_off));

    // Three bands: the knob changes the carried state set, so the fingerprint moves.
    GnssConfig tri_off, tri_on;
    ASSERT_TRUE(loadFieldGoldenWith(triple, tri_off, "tri_off"));
    ASSERT_TRUE(loadFieldGoldenWith([&](nlohmann::json& j) { triple(j); arm(j); }, tri_on, "tri_on"));
    ASSERT_TRUE(rxExtraBandDelayEffective(tri_on));
    EXPECT_NE(liveFingerprint(tri_on), liveFingerprint(tri_off));
}
