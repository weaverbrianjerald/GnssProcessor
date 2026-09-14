/**
 * @file test_amb_float_vehicle.cpp
 * @brief Contract tests for @c network.ambiguity_vehicle — the RF-1 AMBIGUITY VEHICLE
 *        knob, its supporting knobs, its loader interlocks, the two @ref
 *        GnssProcessor::LocalCarry fields it adds, and the per-arc carry-state trace
 *        (@c network.trace_amb_state -> @c network_amb_state.csv).
 *
 * Card: docs/refound/RF-1_float_ambiguities_design.md (steps 1 and 2 of §7.3's commit
 * order), under docs/refound/NETWORK_REFOUNDING_2026-09-14.md.
 *
 * WHY the card exists, in the sentence the tests are shaped around: today a network
 * ambiguity is a per-epoch FREE parameter made time-linked by a zero-process-noise
 * SCALAR prior that carries last epoch's marginal variance, is born with no prior at
 * all from ONE epoch of data at a variance the model reports rather than the value
 * deserves, and is returned to that state by four un-instrumented reset paths (design
 * §1.8) — a managed level, not an estimated state.
 *
 * ⚠ WHAT THIS BUILD IS.  Steps 1 and 2 only: the vehicle is REGISTERED and carries NO
 * mechanism, and the trace is MEASURE-ONLY.  So the load-bearing property these cases
 * pin is the OFF/ON contract itself — that arming @c float_kf or @c trace_amb_state
 * changes no solve input — plus the interlocks that stop an arm being armed where it
 * could not act (standing invariant #4: an inert arm reads exactly like a clean one).
 * The 1/n fold test, the drop-restores-the-birth-variance test and the declared
 * S-transform exactness test of design §6.2 belong to steps 3–5 and are deliberately
 * NOT stubbed here — a passing test for an unbuilt mechanism is worse than none.
 *
 * What a unit test can reach here: the loader (the enum grammar, the fallback, the two
 * hard-error interlocks, the two refusals and the WARN), the carried type's new fields
 * and their defaults, and the trace writer's row rule against a synthetic carry.  What
 * it cannot reach is the live harvest / inject seam that supplies the row — which is
 * why @ref GnssProcessor::ambStateFormatRow is factored out of the writer, so the ROW
 * RULE, not a paraphrase of it, is what both the seam and this file call.
 */

#include <gtest/gtest.h>

#include "config/GnssConfig.hpp"
#include "config/GnssConfigTypes.hpp"
#include "filter/NetworkAmbStateTrace.hpp"
#include "filter/NetworkRunTypes.hpp"
#include "logging/LogManager.hpp"
#include "logging/NetworkRunTraces.hpp"

#include <cmath>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

using GnssProcessor::AmbFoldRoute;
using GnssProcessor::AmbStateRow;
using GnssProcessor::ambStateFormatRow;
using GnssProcessor::ambStateRouteName;
using GnssProcessor::GnssConfig;
using GnssProcessor::LocalCarry;
using GnssProcessor::LogManager;
using GnssProcessor::kAmbStateHeader;
using GnssProcessor::NetworkAmbiguityVehicle;

namespace fs = std::filesystem;

namespace {

fs::path writeTempConfig(const std::string& tag, const std::string& body) {
    const fs::path p = fs::temp_directory_path() / ("ambfloat_cfg_" + tag + ".json");
    std::ofstream f(p);
    f << body;
    f.close();
    return p;
}

/// A minimal but LOADABLE ppp-rtk-network config; @p extra is spliced into the
/// `network` object ahead of the station roster.
std::string networkConfig(const std::string& extra) {
    return std::string("{\n")
        + "  \"processing\": {\n"
          "    \"mode\": \"ppp-rtk-network\",\n"
          "    \"active_systems\": [ \"GPS\" ],\n"
          "    \"signal_priorities\": { \"GPS\": \"WC\" }\n"
          "  },\n"
          "  \"network\": {\n"
          "    \"clock_datum_station\": \"AAAA\",\n"
        + extra +
          "    \"stations\": [\n"
          "      { \"name\": \"AAAA\", \"position_ecef\": [ -2493304.99, -4655214.79, 3565497.67 ] },\n"
          "      { \"name\": \"BBBB\", \"position_ecef\": [ -2281621.75, -1453585.08, 5756964.89 ] }\n"
          "    ]\n"
          "  }\n"
          "}\n";
}

/// RF-0's frozen posture, reduced to the knobs the vehicle interlocks read: the
/// LEVELING vehicle (phase_leveling on, carry_invariant off) with Card-1's conditional
/// coupling, F1 and F3 absent ⇒ off.
const char* kRf0Posture =
    "    \"phase_leveling\": true,\n"
    "    \"carry_invariant\": false,\n"
    "    \"carry_conditional_coupling\": true,\n"
    "    \"shared_carry_full_cov\": true,\n";

/// The OTHER vehicle — the Card-4 invariant DD carry the field golden ran until the
/// re-founding decision.  @c carry_invariant has its own loader prerequisite
/// (@c phase_datum_eq34: the DD cycle basis is defined only on the eq34 chain datum), so
/// it is carried here — without it the interlock case below would pass on an unrelated
/// validation failure.
const char* kCarryInvariantPosture =
    "    \"phase_leveling\": true,\n"
    "    \"carry_invariant\": true,\n"
    "    \"phase_datum_eq34\": true,\n"
    "    \"carry_conditional_coupling\": true,\n"
    "    \"shared_carry_full_cov\": true,\n";

bool loadInto(const std::string& tag, const std::string& body, GnssConfig& cfg) {
    LogManager log;
    const fs::path p = writeTempConfig(tag, body);
    const bool ok = cfg.loadFromJson(p, log);
    std::error_code ec; fs::remove(p, ec);
    return ok;
}

/// A carried entry at a plausible settled posture; the fields the RF-1 columns read
/// are set explicitly so a row assertion names them.
AmbStateRow sampleRow() {
    AmbStateRow r;
    r.week = 2313; r.tow = 345600.0;
    r.station = "WROC00POL"; r.sat = "G07"; r.signal = "L1C";
    r.value_abs_m = -12.3456789; r.var_m2 = 4.5e-4; r.var_first_m2 = 0.82;
    r.var_birth_m2 = 0.82; r.fold_var_m2 = 4.7e-4;
    r.n_folds = 17; r.age_ep = 19; r.age_since_relevel_ep = 19;
    r.folded = true; r.route = AmbFoldRoute::CouplingScalar; r.event.clear();
    return r;
}

/// Split one CSV line, dropping the terminating newline and KEEPING trailing empty
/// fields.  A `std::getline` loop would silently drop the last cell when it is empty,
/// which is exactly the case the row rule has to get right: `event` is empty on an
/// ordinary fold, and a reader that loses that column loses D3.
std::vector<std::string> splitCsv(const std::string& line) {
    std::string s = line;
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
    std::vector<std::string> out;
    std::size_t start = 0;
    for (std::size_t i = 0; i <= s.size(); ++i) {
        if (i == s.size() || s[i] == ',') {
            out.push_back(s.substr(start, i - start));
            start = i + 1;
        }
    }
    return out;
}

}  // namespace

// =============================================================================
// (1) THE ENUM GRAMMAR.  A string enum on the established loader pattern.
// =============================================================================

TEST(AmbFloatVehicleTest, DefaultsToTheLevelVehicle) {
    GnssConfig cfg;
    ASSERT_TRUE(loadInto("default", networkConfig(kRf0Posture), cfg));
    EXPECT_EQ(cfg.getNetworkConfig().ambiguity_vehicle, NetworkAmbiguityVehicle::Level);
    // The supporting knobs are all default-inert; §3.5 exists precisely so the q
    // default is EXPLICIT rather than implied, so it is pinned here.
    EXPECT_DOUBLE_EQ(cfg.getNetworkConfig().amb_float_birth_var_m2, 0.0);
    EXPECT_DOUBLE_EQ(cfg.getNetworkConfig().amb_float_q_m2s, 0.0);
    EXPECT_FALSE(cfg.getNetworkConfig().amb_float_declared_handover);
    EXPECT_FALSE(cfg.getNetworkConfig().trace_amb_state);
    EXPECT_EQ(cfg.getNetworkConfig().trace_amb_state_every, 1);
}

TEST(AmbFloatVehicleTest, ParsesLevelAndFloatKf) {
    GnssConfig lvl;
    ASSERT_TRUE(loadInto("lvl", networkConfig(std::string(kRf0Posture)
                                              + "    \"ambiguity_vehicle\": \"level\",\n"), lvl));
    EXPECT_EQ(lvl.getNetworkConfig().ambiguity_vehicle, NetworkAmbiguityVehicle::Level);

    GnssConfig fkf;
    ASSERT_TRUE(loadInto("fkf", networkConfig(std::string(kRf0Posture)
                                              + "    \"ambiguity_vehicle\": \"float_kf\",\n"), fkf));
    EXPECT_EQ(fkf.getNetworkConfig().ambiguity_vehicle, NetworkAmbiguityVehicle::FloatKf);
}

// An unknown value must not kill a long run, and must not pass silently either: the
// established pattern (network.trace_dd_fractional_ref, network.raim_erase_band_scope)
// is log->error naming what the knob changes, then the default.
TEST(AmbFloatVehicleTest, UnknownVehicleFallsBackToLevel) {
    GnssConfig cfg;
    ASSERT_TRUE(loadInto("typo", networkConfig(std::string(kRf0Posture)
                                               + "    \"ambiguity_vehicle\": \"floatkf\",\n"), cfg));
    EXPECT_EQ(cfg.getNetworkConfig().ambiguity_vehicle, NetworkAmbiguityVehicle::Level);
}

TEST(AmbFloatVehicleTest, EmptyVehicleStringIsTheDefault) {
    GnssConfig cfg;
    ASSERT_TRUE(loadInto("empty", networkConfig(std::string(kRf0Posture)
                                                + "    \"ambiguity_vehicle\": \"\",\n"), cfg));
    EXPECT_EQ(cfg.getNetworkConfig().ambiguity_vehicle, NetworkAmbiguityVehicle::Level);
}

TEST(AmbFloatVehicleTest, SupportingKnobsParse) {
    GnssConfig cfg;
    ASSERT_TRUE(loadInto("support",
                         networkConfig(std::string(kRf0Posture)
                                       + "    \"ambiguity_vehicle\": \"float_kf\",\n"
                                         "    \"amb_float_birth_var_m2\": 0.25,\n"
                                         "    \"amb_float_q_m2s\": 1e-9,\n"
                                         "    \"amb_float_declared_handover\": true,\n"
                                         "    \"trace_amb_state\": true,\n"
                                         "    \"trace_amb_state_every\": 5,\n"),
                         cfg));
    EXPECT_DOUBLE_EQ(cfg.getNetworkConfig().amb_float_birth_var_m2, 0.25);
    EXPECT_DOUBLE_EQ(cfg.getNetworkConfig().amb_float_q_m2s, 1e-9);
    EXPECT_TRUE(cfg.getNetworkConfig().amb_float_declared_handover);
    EXPECT_TRUE(cfg.getNetworkConfig().trace_amb_state);
    EXPECT_EQ(cfg.getNetworkConfig().trace_amb_state_every, 5);
}

// A stride below 1 is a clamp, not an error: it can only come from a typo, and the
// conservative direction (trace everything) never loses a row.
TEST(AmbFloatVehicleTest, TraceStrideIsClampedToAtLeastOne) {
    GnssConfig cfg;
    ASSERT_TRUE(loadInto("stride0",
                         networkConfig(std::string(kRf0Posture)
                                       + "    \"trace_amb_state\": true,\n"
                                         "    \"trace_amb_state_every\": 0,\n"),
                         cfg));
    EXPECT_EQ(cfg.getNetworkConfig().trace_amb_state_every, 1);
}

// =============================================================================
// (2) THE INTERLOCKS.  One vehicle, two beds — and never an arm that cannot act.
// =============================================================================

// float_kf replaces the LEVELING carry's scalar prior.  With phase_leveling off there
// is no leveling carry to replace, so the arm could not move a byte (invariant #4).
TEST(AmbFloatVehicleTest, FloatKfWithoutPhaseLevelingIsAHardError) {
    GnssConfig cfg;
    EXPECT_FALSE(loadInto("nolevel",
                          networkConfig("    \"carry_invariant\": false,\n"
                                        "    \"carry_conditional_coupling\": true,\n"
                                        "    \"shared_carry_full_cov\": true,\n"
                                        "    \"ambiguity_vehicle\": \"float_kf\",\n"),
                          cfg));
}

// On a carry_invariant lineage the Card-4 DD carry IS the ambiguity time-link and
// ApplicationNetworkAssembly's do_amb_carry skips the leveling inject outright — the
// other vehicle, and the divergence the re-founding decision closes.
TEST(AmbFloatVehicleTest, FloatKfUnderCarryInvariantIsAHardError) {
    GnssConfig cfg;
    EXPECT_FALSE(loadInto("ci", networkConfig(std::string(kCarryInvariantPosture)
                                              + "    \"ambiguity_vehicle\": \"float_kf\",\n"),
                          cfg));
}

// The SAME posture without the vehicle must still load — otherwise the test above
// would be passing on an unrelated validation failure.
TEST(AmbFloatVehicleTest, CarryInvariantLoadsFineOnTheLevelVehicle) {
    GnssConfig cfg;
    EXPECT_TRUE(loadInto("ci_level", networkConfig(kCarryInvariantPosture), cfg));
    EXPECT_EQ(cfg.getNetworkConfig().ambiguity_vehicle, NetworkAmbiguityVehicle::Level);
}

// F1 (×B_eff) and F3 (the station-wide whitened block) are the mechanisms float_kf
// SUBSUMES — the joint block removes EXACTLY the over-count F1 corrects approximately,
// and F3's buildLocalBlock IS the fold the vehicle makes structural.  Refused with a
// line and forced off, so the effective fold is never attributable to two knobs.
TEST(AmbFloatVehicleTest, FloatKfRefusesF1AndF3) {
    GnssConfig cfg;
    ASSERT_TRUE(loadInto("refuse",
                         networkConfig(std::string(kRf0Posture)
                                       + "    \"ambiguity_vehicle\": \"float_kf\",\n"
                                         "    \"phase_leveling_los_scale\": true,\n"
                                         "    \"phase_leveling_local_block\": true,\n"),
                         cfg));
    EXPECT_FALSE(cfg.networkPhaseLevelingLosScale());
    EXPECT_FALSE(cfg.networkPhaseLevelingLocalBlock());
}

// …and leaves them alone on the level vehicle: the refusal is scoped to the arm, not a
// blanket disable that would silently re-posture every existing F3 lineage.
TEST(AmbFloatVehicleTest, LevelVehicleLeavesF1AndF3Armed) {
    GnssConfig cfg;
    ASSERT_TRUE(loadInto("keep",
                         networkConfig(std::string(kRf0Posture)
                                       + "    \"phase_leveling_los_scale\": true,\n"
                                         "    \"phase_leveling_local_block\": true,\n"),
                         cfg));
    EXPECT_TRUE(cfg.networkPhaseLevelingLosScale());
    EXPECT_TRUE(cfg.networkPhaseLevelingLocalBlock());
}

// The young-arc inflate is a time-decaying patch on the inject for the same defect the
// vehicle's floored birth prior fixes at the source — a WARN, not a refusal, because an
// arm may deliberately want the contrast.  Pinned as "still armed after the load".
TEST(AmbFloatVehicleTest, FloatKfWarnsButKeepsTheYoungArcInflate) {
    GnssConfig cfg;
    ASSERT_TRUE(loadInto("yng",
                         networkConfig(std::string(kRf0Posture)
                                       + "    \"ambiguity_vehicle\": \"float_kf\",\n"
                                         "    \"phase_leveling_young_inflate_var_m2\": 0.09,\n"),
                         cfg));
    EXPECT_DOUBLE_EQ(cfg.networkPhaseLevelingYoungInflateVarM2(), 0.09);
}

TEST(AmbFloatVehicleTest, NegativeSupportingKnobsAreRejected) {
    GnssConfig a, b;
    EXPECT_FALSE(loadInto("negbirth",
                          networkConfig(std::string(kRf0Posture)
                                        + "    \"amb_float_birth_var_m2\": -1.0,\n"), a));
    EXPECT_FALSE(loadInto("negq",
                          networkConfig(std::string(kRf0Posture)
                                        + "    \"amb_float_q_m2s\": -1e-9,\n"), b));
}

// =============================================================================
// (3) THE CARRIED TYPE.  var_birth and dropped_ep: declared, defaulted, inert.
// =============================================================================

TEST(AmbFloatVehicleTest, LocalCarryDefaultsTheNewFields) {
    LocalCarry lc{1.0, 2.0, 3.0};
    EXPECT_DOUBLE_EQ(lc.var_birth, 0.0);
    EXPECT_EQ(lc.dropped_ep, 0);
}

// A warm-resume-restored entry has NO snapshotted birth prior — design §6.1 names
// var_birth as the field that must join the snapshot record when seams come back, and
// 0 is the honest "unknown", not a floor of zero.
TEST(AmbFloatVehicleTest, RestoredCarryHasNoBirthPrior) {
    const LocalCarry lc = GnssProcessor::makeRestoredLocalCarry(
        /*value_abs=*/-3.0, /*var=*/1e-3, /*last_seen_s=*/1000.0, /*var_first=*/0.5);
    EXPECT_DOUBLE_EQ(lc.var_birth, 0.0);
    EXPECT_EQ(lc.dropped_ep, 0);
    // The restored-arc AGE sentinels are untouched by this card.
    EXPECT_EQ(lc.birth_ep, LocalCarry::kBirthEpRestored);
    EXPECT_DOUBLE_EQ(lc.birth_s, LocalCarry::kBirthSRestored);
}

// =============================================================================
// (4) THE TRACE ROW.  §7.4's header, verbatim, and the row rule that fills it.
// =============================================================================

// The header is pre-registered in the design and scripts/amb_state_read.py parses by
// NAME — but a column reordered or renamed silently changes what every read means, so
// the exact string is pinned here.
TEST(AmbFloatVehicleTest, TraceHeaderIsTheDesignsHeaderVerbatim) {
    EXPECT_EQ(std::string(kAmbStateHeader),
              "week,tow,station,sat,signal,value_abs_m,var_m2,var_first_m2,var_birth_m2,"
              "fold_var_m2,n_folds,age_ep,age_since_relevel_ep,folded,route,event\n");
}

TEST(AmbFloatVehicleTest, TraceRowHasSixteenColumnsInHeaderOrder) {
    const std::string hdr(kAmbStateHeader);
    const std::string row = ambStateFormatRow(sampleRow());
    ASSERT_FALSE(row.empty());
    ASSERT_EQ(row.back(), '\n');
    EXPECT_EQ(splitCsv(hdr).size(), splitCsv(row).size());
    EXPECT_EQ(splitCsv(row).size(), 16u);
}

TEST(AmbFloatVehicleTest, TraceRowCarriesTheIdentitiesAndTheFoldFacts) {
    const auto c = splitCsv(ambStateFormatRow(sampleRow()));
    ASSERT_EQ(c.size(), 16u);
    EXPECT_EQ(c[0], "2313");
    EXPECT_EQ(c[2], "WROC00POL");
    EXPECT_EQ(c[3], "G07");
    EXPECT_EQ(c[4], "L1C");
    EXPECT_EQ(c[10], "17");          // n_folds — D1's abscissa
    EXPECT_EQ(c[11], "19");          // age_ep
    EXPECT_EQ(c[12], "19");          // age_since_relevel_ep
    EXPECT_EQ(c[13], "1");           // folded
    EXPECT_EQ(c[14], "coupling-scalar");
    // The last cell is `event`, empty on an ordinary fold; std::getline drops a
    // trailing empty field, so the row must still end with the separator + newline.
    const std::string row = ambStateFormatRow(sampleRow());
    EXPECT_EQ(row.substr(row.size() - 2), ",\n");
}

// `route` is the fold as the INJECT actually took it — the column that makes design
// risk #3 (a joint-block fallback silently changing the vehicle on one station-epoch)
// visible per arc rather than only as a run total.
TEST(AmbFloatVehicleTest, RouteNamesAreTheDesignsVocabulary) {
    EXPECT_STREQ(ambStateRouteName(AmbFoldRoute::None), "");
    EXPECT_STREQ(ambStateRouteName(AmbFoldRoute::Diag), "diag");
    EXPECT_STREQ(ambStateRouteName(AmbFoldRoute::Block), "block");
    EXPECT_STREQ(ambStateRouteName(AmbFoldRoute::CouplingScalar), "coupling-scalar");
}

// An UNFOLDED arc still gets a row — that is the whole point of D2/D3: the epochs a
// carry did not act are exactly where the level is re-drawn from one epoch of code.
TEST(AmbFloatVehicleTest, UnfoldedRowReportsZeroFoldVarAndNoRoute) {
    AmbStateRow r = sampleRow();
    r.folded = false;
    r.route = AmbFoldRoute::None;
    r.fold_var_m2 = 0.0;
    r.event = "drop:relevel_free";
    const auto c = splitCsv(ambStateFormatRow(r));
    ASSERT_EQ(c.size(), 16u);
    EXPECT_EQ(c[13], "0");
    EXPECT_EQ(c[14], "");
    EXPECT_EQ(c[15], "drop:relevel_free");
}

// The value and the variances are the quantities D1/D2/D4 are fitted on, so they are
// written at full double precision — a 6-digit default would quantize fold_var at the
// 1e-4 m² scale a settled arc lives at and flatten D1's own slope.
TEST(AmbFloatVehicleTest, TraceRowKeepsFullPrecisionOnTheGradedQuantities) {
    AmbStateRow r = sampleRow();
    r.value_abs_m = -12.345678901234567;
    r.fold_var_m2 = 1.2345678901234e-4;
    const auto c = splitCsv(ambStateFormatRow(r));
    ASSERT_EQ(c.size(), 16u);
    EXPECT_NEAR(std::stod(c[5]), r.value_abs_m, 1e-12);
    EXPECT_NEAR(std::stod(c[9]), r.fold_var_m2, 1e-18);
}

// =============================================================================
// (5) THE OFF CONTRACT.  Default OFF ⇒ no stream, no file, no rows.
// =============================================================================

TEST(AmbFloatVehicleTest, TraceStreamStaysClosedWhenUnarmed) {
    const fs::path dir = fs::temp_directory_path() / "ambstate_off_case";
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir, ec);
    {
        GnssProcessor::NetworkRunTraces traces(dir);
        EXPECT_FALSE(traces.amb_state_trace.is_open());
        EXPECT_EQ(traces.amb_state_trace_path, dir / "network_amb_state.csv");
    }
    EXPECT_FALSE(fs::exists(dir / "network_amb_state.csv"));
    fs::remove_all(dir, ec);
}
