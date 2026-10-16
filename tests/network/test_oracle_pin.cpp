/**
 * @file test_oracle_pin.cpp
 * @brief The sim-only oracle ambiguity pin (`network.inject.events[] kind
 *        "oracle_pin"`, include/network/slip/OraclePin.hpp).
 *
 * Four suites, all structural (this file does not validate estimation accuracy):
 *
 *  * `OraclePinConfig`: the parse contract.  Every scope form loads, an invalid
 *    scope is refused by name, a missing scope and a non-positive `sigma_m` are
 *    refused, a second selector (`sat` / `station` / `signal`) alongside `scope`
 *    is refused, and an `oracle_pin` event with no truth root fails the load.  The
 *    last rule keeps the knob off field lineages: a field lineage has no truth
 *    root, so the knob cannot be armed there.
 *  * `OraclePinTruth`: the `truth_ambiguities.csv` parser (header and row errors)
 *    and `N(t) = N_int + Σ delta over slips with s <= t`, inclusive at t.
 *  * `OraclePinFit`: the gauge-consistent two-way fit, on a synthetic carry with
 *    no estimator.  The pin claims to assert integer information and no gauge of
 *    its own, so the assertions are the three properties of the header banner:
 *    every pinned double difference is exactly λ·DD(N_true), no anchor arc is
 *    ever in the table, and the free per-band constant of (β, u) moves nothing.
 *  * `OraclePinScope`: `covers()` for each form.
 *
 * Not covered here: the PASS-B row itself.  "The row folds with Λ = 1/σ² and
 * η = Λ(value − x_lin)" is a caller-side property certified by the same-build
 * byte-identity A/B and the run-end closure counters
 * (`offered == rows + gauge_pinned_skipped + no_column`), not by a unit test; the
 * same division tests/network/test_slip_oracle.cpp draws.
 */
#include <gtest/gtest.h>

#include "common/GnssTypes.hpp"
#include "common/PppUtils.hpp"
#include "config/GnssConfig.hpp"
#include "network/slip/OraclePin.hpp"
#include "logging/LogManager.hpp"

#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>
#include <spdlog/sinks/null_sink.h>

#include <cmath>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <tuple>
#include <vector>

namespace GnssProcessor {
namespace {

namespace fs = std::filesystem;

/// GPS week 2313, the sim fixtures' frame; 30 s cadence.
constexpr double kW = 2313.0 * 604800.0;

std::shared_ptr<spdlog::logger> nullLogger() {
    return std::make_shared<spdlog::logger>(
        "test_oracle_pin", std::make_shared<spdlog::sinks::null_sink_mt>());
}

// ===========================================================================
// Config harness
// ===========================================================================

fs::path writeTempConfig(const std::string& tag, const std::string& body) {
    const fs::path p = fs::temp_directory_path() / ("opin_cfg_" + tag + ".json");
    std::ofstream f(p);
    f << body;
    return p;
}

/// Forward slashes only: the JSON body below does not escape backslashes.
std::string jsonPath(const fs::path& p) {
    std::string s = p.string();
    for (auto& ch : s) if (ch == '\\') ch = '/';
    return s;
}

/// Minimum loadable PPP-RTK network config; @p inject is spliced in verbatim and
/// @p root becomes `network.cycle_slip.oracle_truth_root` (omitted when empty).
std::string networkConfig(const std::string& inject, const std::string& root) {
    return std::string(
        "{\n"
        "  \"processing\": {\n"
        "    \"mode\": \"ppp-rtk-network\",\n"
        "    \"active_systems\": [ \"GPS\" ],\n"
        "    \"signal_priorities\": { \"GPS\": \"WC\" }\n"
        "  },\n"
        "  \"network\": {\n"
        "    \"clock_datum_station\": \"STA1\",\n"
        "    \"trace_events\": true,\n")
        + (root.empty() ? std::string()
                        : "    \"cycle_slip\": { \"oracle_truth_root\": \"" + root + "\" },\n")
        + inject +
        "    \"stations\": [\n"
        "      { \"name\": \"STA1\", \"position_ecef\": [ -2493304.99, -4655214.79, 3565497.67 ] },\n"
        "      { \"name\": \"STA2\", \"position_ecef\": [ -2281621.75, -1453585.08, 5756964.89 ] }\n"
        "    ]\n"
        "  }\n"
        "}\n";
}

/// One `oracle_pin` event, ready to splice as the `inject` block.
std::string pinBlock(const nlohmann::json& ev) {
    nlohmann::json j;
    j["enable"] = true;
    j["events"] = nlohmann::json::array({ev});
    return "    \"inject\": " + j.dump() + ",\n";
}

bool loadPin(const nlohmann::json& ev, const std::string& root, GnssConfig& cfg,
             LogManager& log, const std::string& tag) {
    const fs::path p = writeTempConfig(tag, networkConfig(pinBlock(ev), root));
    const bool ok = cfg.loadFromJson(p, log);
    std::error_code ec; fs::remove(p, ec);
    return ok;
}

/// A scratch fixture tree (one station dir with both truth files) that cleans up.
struct TempTree {
    fs::path root;
    explicit TempTree(const std::string& tag)
        : root(fs::temp_directory_path() / ("gnss_oracle_pin_" + tag)) {
        std::error_code ec;
        fs::remove_all(root, ec);
        fs::create_directories(root);
    }
    ~TempTree() { std::error_code ec; fs::remove_all(root, ec); }
    TempTree(const TempTree&) = delete;
    TempTree& operator=(const TempTree&) = delete;

    /// Write a truth_ambiguities.csv with an explicit header + body.
    fs::path writeAmb(const std::string& sta, const std::string& header,
                      const std::string& rows) {
        const auto dir = root / sta;
        fs::create_directories(dir);
        const auto f = dir / "truth_ambiguities.csv";
        std::ofstream out(f);
        out << header << "\n" << rows;
        return f;
    }
};

/// An event object with only the fields a case cares about.
nlohmann::json pinEvent(const std::string& scope, double tow = 279300.0) {
    nlohmann::json j;
    j["kind"]  = "oracle_pin";
    j["tow"]   = tow;
    j["scope"] = scope;
    return j;
}

// ===========================================================================
// Fit harness: 6 stations x 8 satellites x 2 bands, no estimator
// ===========================================================================

/// The synthetic truth the fit recovers up to its one free per-band constant.
///
/// Ground model (the header banner's):  alpha_rsj = beta_sj + u_rj + lambda_j*N_rsj.
/// Anchors sit on stations 0, 1 and 2 (three different stations, so a bug that
/// assumed one anchor station shows up), each with a target != 0 taken from the
/// same model, so the anchor's enforced pin is consistent with the carried arcs
/// the fit sees.
struct FitBed {
    static constexpr int kNSta  = 6;
    static constexpr int kNSat  = 8;
    static constexpr const char* kBand[2] = {"L1C", "L2W"};
    /// Station index 5 carries only two arcs => its band offsets are unfittable.
    static constexpr int kSparseSta = 5;

    LogManager log_manager;
    NetworkOraclePin pin;
    std::vector<NetStation> stations;
    net::ArcTable carry;
    std::map<std::string, std::string> anchor_of;
    double lam[2]{0.0, 0.0};

    static std::string sat(int s) {
        return "G0" + std::to_string(s + 1);           // G01..G08
    }
    static std::string sta(int r) {
        return "S" + std::to_string(r) + "000SIM";
    }
    /// Deterministic "truth" values with no RNG, so a failure is reproducible
    /// from the case name alone.
    static int    trueN(int r, int s, int j) { return 13 * r - 7 * s + 29 * j - 40; }
    static double trueBeta(int s, int j)     { return 0.37 * s - 1.9 * j + 4.11; }
    static double trueU(int r, int j)        { return 0.083 * r + 0.57 * j - 0.21; }
    /// 0.1 cycle of deterministic "noise" on the carried value, alternating sign
    /// so the per-station-band median is not biased by it.
    static double noiseCyc(int r, int s, int j) {
        return 0.1 * (((r + s + j) % 3) - 1);
    }

    /// @param c  the free per-band constant: the bed is built with (beta − c,
    ///          u + c), which leaves every input, and therefore the pin table,
    ///          identical.
    explicit FitBed(double c = 0.0, bool with_noise = true) {
        PppUtils utils(log_manager);
        for (int j = 0; j < 2; ++j)
            lam[j] = utils.getWavelength(System::GPS, kBand[j], "G01");

        stations.resize(kNSta);
        for (int r = 0; r < kNSta; ++r) stations[static_cast<std::size_t>(r)].name = sta(r);

        pin.utils = std::make_unique<PppUtils>(log_manager);

        // --- truth for every arc of the day (the fit's universe) -------------
        for (int r = 0; r < kNSta; ++r)
            for (int s = 0; s < kNSat; ++s)
                for (int j = 0; j < 2; ++j) {
                    NetworkOraclePin::Truth t;
                    t.n_int    = trueN(r, s, j);
                    t.lambda_m = lam[j];
                    pin.truth[{r, sat(s), kBand[j]}] = t;
                }

        // --- anchors: 15 of the 16 families; (G08, L2W) has none on purpose ---
        for (int s = 0; s < kNSat; ++s)
            for (int j = 0; j < 2; ++j) {
                if (s == kNSat - 1 && j == 1) continue;      // the no-anchor family
                const int a = (2 * s + j) % 3;               // stations 0 / 1 / 2
                const std::string key = sat(s) + "_" + kBand[j];
                anchor_of[key] = sta(a);
                // The enforced pin target is the anchor arc's own alpha in this
                // model: non-zero, and read by the fit (never assumed 0).
                stations[static_cast<std::size_t>(a)].pinned_amb_targets[key] =
                    (trueBeta(s, j) - c) + (trueU(a, j) + c) + lam[j] * trueN(a, s, j);
            }

        // --- the carry: every non-anchor arc, minus the intended holes --------
        for (int r = 0; r < kNSta; ++r)
            for (int s = 0; s < kNSat; ++s)
                for (int j = 0; j < 2; ++j) {
                    const std::string key = sat(s) + "_" + kBand[j];
                    const auto ait = anchor_of.find(key);
                    if (ait != anchor_of.end() && ait->second == sta(r)) continue;
                    // The sparse station carries exactly two arcs (< 3 => its band
                    // offset is not a station term, it is one arc's own error).
                    if (r == kSparseSta && !(j == 0 && s < 2)) continue;
                    // Two arcs absent from the carry but present in truth + scope:
                    // they are still pinned (the pin is independent of the carry).
                    if (r == 3 && j == 0 && (s == 4 || s == 5)) continue;
                    LocalCarry lc{};
                    lc.value_abs = (trueBeta(s, j) - c) + (trueU(r, j) + c)
                                 + lam[j] * trueN(r, s, j)
                                 + (with_noise ? noiseCyc(r, s, j) * lam[j] : 0.0);
                    lc.var         = 1.0e-5;
                    lc.last_seen_s = kW + 279270.0;
                    lc.birth_s     = kW + 270000.0;
                    net::putArcIn(carry, {r, sat(s), kBand[j]}, &net::ArcState::amb) = lc;
                }

        NetworkOraclePin::Armed a;
        a.ev_index = 0;
        a.sigma_m  = 1.0e-4;
        a.tow      = 279300.0;
        std::string why;
        a.scope = oraclePinParseScope("all", why);
        pin.scopes.push_back(std::move(a));
    }

    int fit() {
        return networkOraclePinFit(pin, stations, carry, anchor_of,
                                   kW + 279300.0, {0});
    }
};

} // namespace

// ===========================================================================
// OraclePinConfig
// ===========================================================================

TEST(OraclePinConfig, EveryScopeFormParsesWithTheDefaultSigma) {
    TempTree tree("cfgscope");
    tree.writeAmb("STA1", "satellite,signal,N_int,wavelength_m", "G01,L1C,3,0.19\n");
    const std::string root = jsonPath(tree.root);
    for (const char* sc : {"all", "band:L1C+L2W", "station:STA1", "sat:G08",
                           "arc:STA1/G08/L5Q"}) {
        LogManager log; GnssConfig cfg;
        ASSERT_TRUE(loadPin(pinEvent(sc), root, cfg, log, "scope_ok"))
            << "scope '" << sc << "' should load";
        const auto& ev = cfg.getNetworkConfig().inject.events;
        ASSERT_EQ(ev.size(), 1u);
        EXPECT_EQ(ev[0].kind, "oracle_pin");
        EXPECT_EQ(ev[0].scope, sc);
        EXPECT_DOUBLE_EQ(ev[0].sigma_m, 1.0e-4);   // the documented default
    }
}

TEST(OraclePinConfig, GarbageScopeIsRefusedByName) {
    TempTree tree("cfgbad");
    tree.writeAmb("STA1", "satellite,signal,N_int,wavelength_m", "G01,L1C,3,0.19\n");
    const std::string root = jsonPath(tree.root);
    // Each of these fails for a different reason, and all five are refused: a
    // scope that silently widened (or emptied) would be graded as though it were
    // the one the operator wrote.
    for (const char* sc : {"everything",        // unknown, no ':'
                           "bands:L1C",         // unknown selector
                           "band:",             // empty argument
                           "band:C1C",          // not a phase code
                           "arc:STA1/G08"}) {   // missing the third component
        LogManager log; GnssConfig cfg;
        EXPECT_FALSE(loadPin(pinEvent(sc), root, cfg, log, "scope_bad"))
            << "scope '" << sc << "' must be refused";
    }
}

TEST(OraclePinConfig, MissingScopeIsRefused) {
    TempTree tree("cfgnoscope");
    tree.writeAmb("STA1", "satellite,signal,N_int,wavelength_m", "G01,L1C,3,0.19\n");
    nlohmann::json ev;
    ev["kind"] = "oracle_pin";
    ev["tow"]  = 279300.0;
    LogManager log; GnssConfig cfg;
    EXPECT_FALSE(loadPin(ev, jsonPath(tree.root), cfg, log, "noscope"));
}

TEST(OraclePinConfig, NonPositiveSigmaIsRefused) {
    TempTree tree("cfgsigma");
    tree.writeAmb("STA1", "satellite,signal,N_int,wavelength_m", "G01,L1C,3,0.19\n");
    const std::string root = jsonPath(tree.root);
    for (const double s : {0.0, -1.0e-4}) {
        nlohmann::json ev = pinEvent("all");
        ev["sigma_m"] = s;
        LogManager log; GnssConfig cfg;
        EXPECT_FALSE(loadPin(ev, root, cfg, log, "sigma_bad"))
            << "sigma_m " << s << " must be refused";
    }
    nlohmann::json ok = pinEvent("all");
    ok["sigma_m"] = 1.0e-3;
    LogManager log; GnssConfig cfg;
    ASSERT_TRUE(loadPin(ok, root, cfg, log, "sigma_ok"));
    EXPECT_DOUBLE_EQ(cfg.getNetworkConfig().inject.events[0].sigma_m, 1.0e-3);
}

TEST(OraclePinConfig, ASecondSelectorAlongsideScopeIsRefused) {
    TempTree tree("cfgsel");
    tree.writeAmb("STA1", "satellite,signal,N_int,wavelength_m", "G01,L1C,3,0.19\n");
    const std::string root = jsonPath(tree.root);
    for (const char* field : {"sat", "station", "signal"}) {
        nlohmann::json ev = pinEvent("all");
        ev[field] = "G08";
        LogManager log; GnssConfig cfg;
        EXPECT_FALSE(loadPin(ev, root, cfg, log, "sel_bad"))
            << "a '" << field << "' beside 'scope' must be refused";
    }
}

TEST(OraclePinConfig, AnEmptyTruthRootFailsTheLoad) {
    // This keeps the knob off field lineages: a field lineage has no truth root,
    // so an oracle_pin event cannot be armed on one.  The load fails outright
    // rather than warning and disarming, because a silently unpinned run would
    // report the model's error budget without the ambiguities held at truth and
    // be read as the ceiling.
    {
        LogManager log; GnssConfig cfg;
        EXPECT_FALSE(loadPin(pinEvent("all"), "", cfg, log, "noroot"));
    }
    {   // A root that does not exist is refused the same way.
        LogManager log; GnssConfig cfg;
        EXPECT_FALSE(loadPin(pinEvent("all"),
                             jsonPath(fs::temp_directory_path() / "gnss_opin_absent"),
                             cfg, log, "gone"));
    }
}

TEST(OraclePinConfig, AbsentInjectLeavesNoOraclePinEventToConstructFrom) {
    // The driver constructs NetworkRunContext::oracle_pin iff network.inject is
    // enabled and holds an `oracle_pin` event; with the section absent that
    // predicate is false, so the pointer stays null and no branch of the seam is
    // reachable (standing invariant #6's structural byte-identity).  The test
    // asserts the predicate, because a NetworkRunContext needs a solve.
    const fs::path p = writeTempConfig("noinject", networkConfig("", ""));
    LogManager log; GnssConfig cfg;
    ASSERT_TRUE(cfg.loadFromJson(p, log));
    std::error_code ec; fs::remove(p, ec);
    const auto& ic = cfg.getNetworkConfig().inject;
    EXPECT_FALSE(ic.enable);
    EXPECT_TRUE(ic.events.empty());
    bool any_pin = false;
    for (const auto& e : ic.events) if (e.kind == "oracle_pin") any_pin = true;
    EXPECT_FALSE(any_pin);
}

// ===========================================================================
// OraclePinTruth
// ===========================================================================

TEST(OraclePinTruth, ParsesTheFixtureHeaderAndRows) {
    TempTree tree("parse");
    const auto f = tree.writeAmb("S01000SIM", "satellite,signal,N_int,wavelength_m",
                                 "G01,L1C,-30,0.190293672798\n"
                                 "G01,L2W,-9,0.244210213425\n"
                                 "E07,L5Q,104,0.254828048790\n");
    const auto m = networkOraclePinParseAmbCsv(f.string());
    ASSERT_EQ(m.size(), 3u);
    EXPECT_EQ(m.at({"G01", "L1C"}).first, -30);
    EXPECT_NEAR(m.at({"G01", "L1C"}).second, 0.190293672798, 1e-12);
    EXPECT_EQ(m.at({"G01", "L2W"}).first, -9);
    EXPECT_EQ(m.at({"E07", "L5Q"}).first, 104);
}

TEST(OraclePinTruth, RejectsAMalformedHeaderAndAMalformedRow) {
    {   // A fixture that renamed or reordered a column throws here rather than
        // silently mis-reading N_int.
        TempTree tree("hdr");
        const auto f = tree.writeAmb("S01000SIM", "satellite,signal,N,wavelength_m",
                                     "G01,L1C,3,0.19\n");
        EXPECT_THROW(networkOraclePinParseAmbCsv(f.string()), std::runtime_error);
    }
    {   // Too few fields.
        TempTree tree("row");
        const auto f = tree.writeAmb("S01000SIM", "satellite,signal,N_int,wavelength_m",
                                     "G01,L1C,3\n");
        EXPECT_THROW(networkOraclePinParseAmbCsv(f.string()), std::runtime_error);
    }
    {   // A non-numeric integer.
        TempTree tree("num");
        const auto f = tree.writeAmb("S01000SIM", "satellite,signal,N_int,wavelength_m",
                                     "G01,L1C,threeish,0.19\n");
        EXPECT_THROW(networkOraclePinParseAmbCsv(f.string()), std::runtime_error);
    }
    {   // An empty file has no header at all.
        TempTree tree("empty");
        const auto dir = tree.root / "S01000SIM";
        fs::create_directories(dir);
        const auto f = dir / "truth_ambiguities.csv";
        std::ofstream(f).close();
        EXPECT_THROW(networkOraclePinParseAmbCsv(f.string()), std::runtime_error);
    }
    {   // A duplicate arc would make N(t) depend on file order.
        TempTree tree("dup");
        const auto f = tree.writeAmb("S01000SIM", "satellite,signal,N_int,wavelength_m",
                                     "G01,L1C,3,0.19\nG01,L1C,4,0.19\n");
        EXPECT_THROW(networkOraclePinParseAmbCsv(f.string()), std::runtime_error);
    }
}

TEST(OraclePinTruth, TrueIntegerFollowsTheSlipsInAscendingTime) {
    NetworkOraclePin::Truth t;
    t.n_int = -30;
    t.slips.push_back({kW + 279000.0, +5});
    t.slips.push_back({kW + 279600.0, -12});
    EXPECT_EQ(networkOraclePinTrueN(t, kW + 278000.0), -30);         // before both
    EXPECT_EQ(networkOraclePinTrueN(t, kW + 279300.0), -25);         // after the first
    EXPECT_EQ(networkOraclePinTrueN(t, kW + 280000.0), -37);         // after both
}

TEST(OraclePinTruth, ASlipStampedExactlyAtTheEpochCounts) {
    // Inclusive upper bound: the observation at that epoch already carries the
    // jump, so the true integer contains it too.  An exclusive bound would put
    // the pin one jump behind the phase for one epoch.
    NetworkOraclePin::Truth t;
    t.n_int = 7;
    t.slips.push_back({kW + 279300.0, +3});
    EXPECT_EQ(networkOraclePinTrueN(t, kW + 279270.0), 7);
    EXPECT_EQ(networkOraclePinTrueN(t, kW + 279300.0), 10);
}

// ===========================================================================
// OraclePinFit
// ===========================================================================

TEST(OraclePinFit, EveryPinnedDoubleDifferenceIsExactlyLambdaTimesTheTrueDD) {
    // The central property.  beta and u both cancel in a double difference, so if
    // the table carries anything but integer information this fails.  Every chord
    // of every band over every pinned (station, satellite) pair is checked, not a
    // sample: the fit is cheap and a partial check could miss one family.
    FitBed bed;
    const int rounds = bed.fit();
    EXPECT_GT(rounds, 0);
    // Determination, not iteration: every station-band whose family anchors are
    // already resolved is fixed in one round and then frozen, so the pass is
    // bounded by the station count and on this bed every station resolves
    // straight off its band's root anchor.
    EXPECT_LE(rounds, FitBed::kNSta);
    ASSERT_FALSE(bed.pin.table.empty());

    long long chords = 0;
    for (int j = 0; j < 2; ++j)
        for (int s1 = 0; s1 < FitBed::kNSat; ++s1)
            for (int s2 = s1 + 1; s2 < FitBed::kNSat; ++s2)
                for (int r1 = 0; r1 < FitBed::kNSta; ++r1)
                    for (int r2 = r1 + 1; r2 < FitBed::kNSta; ++r2) {
                        const auto a = bed.pin.table.find({r1, FitBed::sat(s1), FitBed::kBand[j]});
                        const auto b = bed.pin.table.find({r1, FitBed::sat(s2), FitBed::kBand[j]});
                        const auto c = bed.pin.table.find({r2, FitBed::sat(s1), FitBed::kBand[j]});
                        const auto d = bed.pin.table.find({r2, FitBed::sat(s2), FitBed::kBand[j]});
                        if (a == bed.pin.table.end() || b == bed.pin.table.end()
                            || c == bed.pin.table.end() || d == bed.pin.table.end())
                            continue;
                        const double dd = (a->second.value_m - b->second.value_m)
                                        - (c->second.value_m - d->second.value_m);
                        const double dd_true = bed.lam[j]
                            * static_cast<double>((FitBed::trueN(r1, s1, j) - FitBed::trueN(r1, s2, j))
                                                - (FitBed::trueN(r2, s1, j) - FitBed::trueN(r2, s2, j)));
                        EXPECT_NEAR(dd, dd_true, 1.0e-9)
                            << "chord " << r1 << "/" << r2 << " " << s1 << "/" << s2
                            << " band " << FitBed::kBand[j];
                        ++chords;
                    }
    EXPECT_GT(chords, 100) << "the chord population must be non-trivial";
}

TEST(OraclePinFit, AnchorArcsAreNeverInTheTable) {
    // The anchor's 1e10 datum pin owns its family's frame; a second row there
    // would fight the gauge the fit is expressed in.
    FitBed bed;
    bed.fit();
    for (const auto& [key, anchor_name] : bed.anchor_of) {
        const auto us = key.rfind('_');
        const std::string sat = key.substr(0, us), sig = key.substr(us + 1);
        int a = -1;
        for (int r = 0; r < FitBed::kNSta; ++r)
            if (FitBed::sta(r) == anchor_name) a = r;
        ASSERT_GE(a, 0);
        EXPECT_EQ(bed.pin.table.count({a, sat, sig}), 0u)
            << "anchor arc " << anchor_name << "/" << key << " must not be pinned";
    }
}

TEST(OraclePinFit, AFamilyWithNoAnchorIsCountedAndNotPinned) {
    FitBed bed;
    bed.fit();
    const std::string sat = FitBed::sat(FitBed::kNSat - 1);   // G08
    const std::string sig = FitBed::kBand[1];                 // L2W: the no-anchor family
    for (int r = 0; r < FitBed::kNSta; ++r)
        EXPECT_EQ(bed.pin.table.count({r, sat, sig}), 0u);
    // Every station's arc of that family reaches the beta check and is counted:
    // without this the run-end clause could not tell "not in scope" from
    // "in scope but the family had no anchor this epoch".
    EXPECT_EQ(bed.pin.no_anchor, static_cast<long long>(FitBed::kNSta));
}

TEST(OraclePinFit, AStationWithTooFewCarriedArcsIsCountedNoStationTerm) {
    // A station-band offset fitted on fewer than 3 arcs is not a station term but
    // one arc's own error under another name, and pinning on it would inject that
    // error into every other arc of the station.
    FitBed bed;
    bed.fit();
    const int r = FitBed::kSparseSta;
    for (int s = 0; s < FitBed::kNSat; ++s)
        for (int j = 0; j < 2; ++j)
            EXPECT_EQ(bed.pin.table.count({r, FitBed::sat(s), FitBed::kBand[j]}), 0u)
                << "sparse station must not be pinned";
    // Its 16 arcs minus the one whose family has no anchor (counted there first).
    EXPECT_EQ(bed.pin.no_station_term,
              static_cast<long long>(FitBed::kNSat * 2 - 1));
}

TEST(OraclePinFit, ArcsAbsentFromTheCarryAreStillPinned) {
    // The pin is independent of the leveling carry: a newborn leg, or an arc the
    // slip response just freed, has no LocalCarry entry and is still held at
    // truth.  The instrument relies on that independence.
    FitBed bed;
    bed.fit();
    for (const int s : {4, 5}) {
        const auto key = std::make_tuple(3, FitBed::sat(s), std::string(FitBed::kBand[0]));
        EXPECT_EQ(net::getArc(bed.carry, key, &net::ArcState::amb), nullptr) << "the bed must not carry this arc";
        EXPECT_EQ(bed.pin.table.count(key), 1u) << "…and the pin must still hold it";
    }
}

TEST(OraclePinFit, TheFreePerBandConstantOfBetaAndUChangesNothing) {
    // (beta − c, u + c) is the same physical bed: alpha = beta + u + lambda*N is
    // invariant, so the anchor targets and the carried values describe the same
    // network and the table does too.  The assertion is that the pin reads only
    // that sum and introduces no gauge of its own.
    //
    // Tolerance, not bit-identity: the bed re-associates the sum as
    // (beta − c) + (u + c), which costs about one ULP in the inputs themselves
    // (9e−16 m at c = 7.25) before the pin sees them.  The invariance is exact in
    // exact arithmetic; 1e−12 m is about 5e−12 cycles, eleven decades below one
    // cycle.
    FitBed a(0.0), b(7.25);
    a.fit();
    b.fit();
    ASSERT_EQ(a.pin.table.size(), b.pin.table.size());
    for (const auto& [key, pa] : a.pin.table) {
        const auto it = b.pin.table.find(key);
        ASSERT_NE(it, b.pin.table.end());
        EXPECT_NEAR(pa.value_m, it->second.value_m, 1.0e-12);
        EXPECT_EQ(pa.n_true,  it->second.n_true);
    }
}

TEST(OraclePinFit, WithoutNoiseThePinnedValuesAreTheTruthAlphaExactly) {
    // The stronger statement the DD test cannot make on its own: with no noise
    // the fit recovers beta and u exactly (the anchor targets are consistent with
    // the carry), so each pinned value equals the model's own alpha.
    FitBed bed(0.0, /*with_noise=*/false);
    bed.fit();
    ASSERT_FALSE(bed.pin.table.empty());
    for (const auto& [key, p] : bed.pin.table) {
        const int r = std::get<0>(key);
        int s = -1, j = -1;
        for (int k = 0; k < FitBed::kNSat; ++k) if (FitBed::sat(k) == std::get<1>(key)) s = k;
        for (int k = 0; k < 2; ++k) if (FitBed::kBand[k] == std::get<2>(key)) j = k;
        ASSERT_GE(s, 0); ASSERT_GE(j, 0);
        const double alpha = FitBed::trueBeta(s, j) + FitBed::trueU(r, j)
                           + bed.lam[j] * FitBed::trueN(r, s, j);
        EXPECT_NEAR(p.value_m, alpha, 1.0e-9);
        EXPECT_EQ(p.n_true, FitBed::trueN(r, s, j));
    }
}

TEST(OraclePinFit, TheFitResidualReportsWhatThePinIsAboutToMove) {
    // Standing invariant #4: the residual is the actuation delta, so an arm whose
    // residual is ~0 pinned a solution already at truth and an arm with a large
    // one is measuring how far the float ambiguities sat from the integers.
    FitBed clean(0.0, /*with_noise=*/false);
    clean.fit();
    EXPECT_GT(clean.pin.fit_resid_n, 0);
    EXPECT_NEAR(clean.pin.fit_resid_rms_cyc, 0.0, 1.0e-9);

    FitBed noisy;                       // +/-0.1 cyc of deterministic offset
    noisy.fit();
    EXPECT_GT(noisy.pin.fit_resid_n, 0);
    EXPECT_GT(noisy.pin.fit_resid_max_cyc, 0.01);
    // The median absorbs the station-common part, so no arc's residual can be
    // more than the injected offset plus the offset the median landed on.
    EXPECT_LT(noisy.pin.fit_resid_max_cyc, 0.25);
}

TEST(OraclePinFit, ABandScopePinsOnlyThatBand) {
    FitBed bed;
    std::string why;
    bed.pin.scopes[0].scope = oraclePinParseScope("band:L1C", why);
    ASSERT_EQ(bed.pin.scopes[0].scope.kind, OraclePinScope::Kind::Band);
    bed.fit();
    ASSERT_FALSE(bed.pin.table.empty());
    for (const auto& [key, p] : bed.pin.table) {
        (void)p;
        EXPECT_EQ(std::get<2>(key), "L1C");
    }
}

// ===========================================================================
// OraclePinScope
// ===========================================================================

TEST(OraclePinScope, CoversEachFormExactly) {
    std::string why;
    const auto all = oraclePinParseScope("all", why);
    EXPECT_TRUE(all.covers("S01000SIM", "G08", "L5Q"));

    const auto band = oraclePinParseScope("band:L1C+L2W", why);
    ASSERT_EQ(band.kind, OraclePinScope::Kind::Band);
    EXPECT_TRUE (band.covers("S01000SIM", "G08", "L1C"));
    EXPECT_TRUE (band.covers("S02000SIM", "E11", "L2W"));
    EXPECT_FALSE(band.covers("S01000SIM", "G08", "L5Q"));

    const auto sta = oraclePinParseScope("station:S21000SIM", why);
    EXPECT_TRUE (sta.covers("S21000SIM", "G08", "L5Q"));
    EXPECT_FALSE(sta.covers("S21000SI",  "G08", "L5Q"));   // exact, not a prefix

    const auto sat = oraclePinParseScope("sat:G08", why);
    EXPECT_TRUE (sat.covers("S01000SIM", "G08", "L1C"));
    EXPECT_FALSE(sat.covers("S01000SIM", "G18", "L1C"));

    const auto arc = oraclePinParseScope("arc:S21000SIM/G08/L5Q", why);
    ASSERT_EQ(arc.kind, OraclePinScope::Kind::Arc);
    EXPECT_TRUE (arc.covers("S21000SIM", "G08", "L5Q"));
    EXPECT_FALSE(arc.covers("S21000SIM", "G08", "L1C"));
    EXPECT_FALSE(arc.covers("S22000SIM", "G08", "L5Q"));
}

TEST(OraclePinScope, AnInvalidScopeCoversNothing) {
    // Fail closed: a scope that failed to parse never silently pins the whole
    // network.  (The loader refuses it first; this is the second line of defence.)
    std::string why;
    const auto bad = oraclePinParseScope("nonsense", why);
    ASSERT_EQ(bad.kind, OraclePinScope::Kind::Invalid);
    EXPECT_FALSE(why.empty());
    EXPECT_FALSE(bad.covers("S01000SIM", "G08", "L1C"));
}

} // namespace GnssProcessor
