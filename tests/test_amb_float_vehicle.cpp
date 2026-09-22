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
#include "filter/NetworkAmbFloat.hpp"
#include "filter/NetworkAmbStateTrace.hpp"
#include "filter/NetworkCoupling.hpp"
#include "filter/NetworkHandoverStransform.hpp"
#include "filter/NetworkEstimator.hpp"
#include "filter/NetworkRunTypes.hpp"
#include "logging/LogManager.hpp"
#include "logging/NetworkRunTraces.hpp"

#include <Eigen/Dense>

#include <cmath>
#include <map>
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
using GnssProcessor::NetworkEstimator;

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

// =============================================================================
// (6) THE 1/n GATE (design §6.2 test 2) — the single most direct test of §3.2,
//     run at the REAL seam: CouplingRowBuilder::buildLocalBlock over a registered
//     NetworkEstimator, driven through the producer's own recursion
//     (assemble -> fold -> solve -> harvest -> fold ...).
//
// THE FIXTURE, and why it is the right minimal one.  ONE station, ONE satellite,
// TWO bands, no shared border.  Local states [dt_r, I, N1, N2]; four rows per
// epoch — two code (sigma 1 m) and two phase (sigma 3 mm) — so the block is
// exactly determined each epoch and the ONLY thing that can make the carried
// variance fall is the prior fold.  That is the F1 comment made executable: the
// B = 2 carried columns of ONE line of sight are tied to each other through the
// station-common receiver clock and through the LOS's own free slant iono, so
// folding them as TWO independent marginals asserts those directions twice per
// epoch where the joint block asserts them once.
//
// THE ALGEBRA THE TEST PINS.  Let S be the Schur complement of one epoch's normal
// matrix onto (N1, N2) after eliminating (dt_r, I).  A birth epoch has no prior,
// so C_1 = S^-1.  Folding the JOINT prior adds exactly C_{n-1}^-1 on that
// subspace, so C_n = (S + C_{n-1}^-1)^-1 = S^-1/n — i.e. var(n)*n/var(1) = 1 for
// every n, to numerical precision.  Folding diag(1/C_11, 1/C_22) instead is the
// legacy route, and the contrast between the two is the test.
// =============================================================================

namespace {

/// GPS L1/L2 iono scaling gamma = (f1/f2)^2 — the real ratio, so the two carried
/// columns are correlated through I exactly as they are in the producer.
constexpr double kGamma = (154.0 / 120.0) * (154.0 / 120.0);
constexpr Eigen::Index kColN1 = 2, kColN2 = 3;

/// The station's per-epoch design over [dt_r, I, N1, N2]: P1, P2, L1, L2.
Eigen::MatrixXd oneEpochDesign() {
    Eigen::MatrixXd A(4, 4);
    A << 1.0,     1.0, 0.0, 0.0,
         1.0,  kGamma, 0.0, 0.0,
         1.0,    -1.0, 1.0, 0.0,
         1.0, -kGamma, 0.0, 1.0;
    return A;
}

Eigen::VectorXd oneEpochWeights() {
    Eigen::VectorXd w(4);
    const double sc = 1.0 / (1.0 * 1.0), sp = 1.0 / (0.003 * 0.003);
    w << sc, sc, sp, sp;
    return w;
}

/// A reproducible per-epoch noise draw — a fixed xorshift, so the recursion
/// wanders and converges like a real one and the test stays deterministic.
struct Lcg {
    unsigned long long s{88172645463325252ULL};
    double next() {                       // ~U(-0.5, 0.5)
        s ^= s << 13; s ^= s >> 7; s ^= s << 17;
        return static_cast<double>(s % 1000000ULL) / 1000000.0 - 0.5;
    }
};

struct RecursionResult {
    std::vector<double> var_n;    ///< harvested (N_rr^-1)_{N1,N1} after each epoch
    std::vector<double> value_n;  ///< the carried absolute N1 after each epoch
};

/// One arm of the producer's recursion.  @p joint selects the vehicle's fold (the
/// whitened joint block) over the legacy one (N independent diagonal priors).
///
/// @p drift_m_per_ep adds a slow PHASE-ONLY common bias (metres per epoch) that the
/// model has no state for — a drifting receiver phase bias, which the four-row design
/// cannot put in @c dt_r (that is common to the code rows too) nor in @c I (dispersive).
/// It therefore ratchets into the carried ambiguities, which is design §3.5's
/// "any real drift in what the ambiguity column ABSORBS has nowhere to go".  0 = the
/// clean fixture.
RecursionResult runRecursion(bool joint, int n_epochs, double drift_m_per_ep = 0.0) {
    const Eigen::MatrixXd A_r = oneEpochDesign();
    const Eigen::VectorXd w   = oneEpochWeights();
    Eigen::VectorXd x_true(4);
    x_true << 0.0, 2.0, 7.5, 11.3;      // dt_r, I (m at L1), N1, N2 (m)

    NetworkEstimator est;
    GnssProcessor::GlobalVtecShConfig shc{};
    std::map<std::tuple<GnssProcessor::SharedParamKind, std::string, std::string>, double> tu_hist;
    const NetworkEstimator::Datum datum{};          // no shared border, nothing to pin
    const std::vector<Eigen::Index> carried{kColN1, kColN2};

    Lcg rng;
    RecursionResult out;
    bool have_carry = false;
    LocalCarry c1, c2;

    for (int n = 1; n <= n_epochs; ++n) {
        est.beginEpoch();
        GnssProcessor::StationAssembly R;
        R.A_s = Eigen::MatrixXd::Zero(4, est.numShared());
        R.A_r = A_r;
        R.w   = w;
        R.y   = A_r * x_true;
        // Code noise at the metre scale, phase noise at the millimetre scale, so the
        // recursion has something to average and `value` convergence is meaningful.
        R.y(0) += 1.0   * rng.next();
        R.y(1) += 1.0   * rng.next();
        R.y(2) += 0.003 * rng.next();
        R.y(3) += 0.003 * rng.next();
        // The un-modelled PHASE-ONLY drift, on the two phase rows only.
        const double drift = drift_m_per_ep * static_cast<double>(n - 1);
        R.y(2) += drift;
        R.y(3) += drift;

        if (have_carry && joint) {
            // THE VEHICLE'S FOLD.  Sigma's diagonal is each column's carried variance;
            // its off-diagonals come from the SHARED harvest block, which is exactly
            // what makes this a joint prior rather than two marginals.
            GnssProcessor::CouplingRowBuilder cb{
                est, shc, /*q_clock=*/0.0, /*q_cb=*/0.0, /*q_pb=*/0.0,
                /*joint=*/false, /*tu=*/false, /*shmarg=*/false,
                /*gauge=*/false, /*reinit=*/false, tu_hist, R};
            const bool ok = cb.buildLocalBlock({
                GnssProcessor::LocalBlockCol{kColN1, 0.0, &c1, c1.var, 0.0, 1.0},
                GnssProcessor::LocalBlockCol{kColN2, 0.0, &c2, c2.var, 0.0, 1.0}});
            EXPECT_TRUE(ok) << "buildLocalBlock went non-SPD at epoch " << n;
            cb.appendCouplingRows();
        }
        const std::size_t id = est.addStation(R.A_s, R.A_r, R.w, R.y);
        if (have_carry && !joint) {
            // THE LEGACY FOLD.  N independent diagonal rows, each asserting its own
            // column's MARGINAL precision — the over-count the card names.
            Eigen::VectorXd lam(2), eta(2);
            lam << 1.0 / c1.var, 1.0 / c2.var;
            eta << lam(0) * c1.value_abs, lam(1) * c2.value_abs;   // x_lin = 0
            est.addStationPrior(id, carried, lam, eta);
        }
        const Eigen::VectorXd x_s = est.solve(datum);
        const Eigen::VectorXd xr  = est.reconstructStation(id, x_s);

        // The harvest: each carried column's PURE conditional variance and the SAME
        // solve read at the sibling — the gather the block's off-diagonals are.
        const auto r1 = est.stationRegression(id, kColN1, /*carry_aware=*/false, &carried);
        const auto r2 = est.stationRegression(id, kColN2, /*carry_aware=*/false, &carried);
        auto blk = std::make_shared<GnssProcessor::PlHarvestBlock>();
        blk->C = Eigen::MatrixXd::Zero(2, 2);
        blk->C.row(0) = r1.cross.transpose();
        blk->C.row(1) = r2.cross.transpose();
        blk->stamp_s = static_cast<double>(n);

        c1 = LocalCarry{xr(kColN1), r1.cond_var, static_cast<double>(n)};
        c2 = LocalCarry{xr(kColN2), r2.cond_var, static_cast<double>(n)};
        c1.blk = blk; c1.blk_idx = 0;
        c2.blk = blk; c2.blk_idx = 1;
        have_carry = true;

        out.var_n.push_back(r1.cond_var);
        out.value_n.push_back(xr(kColN1));
    }
    return out;
}

/// b in `log var = a - b log n`, fitted by OLS over the whole trajectory — the
/// same statistic scripts/amb_state_read.py's D1 reports from the field trace.
double fitB(const std::vector<double>& var_n) {
    const std::size_t n = var_n.size();
    double mx = 0.0, my = 0.0;
    for (std::size_t i = 0; i < n; ++i) {
        mx += std::log(static_cast<double>(i + 1));
        my += std::log(var_n[i]);
    }
    mx /= static_cast<double>(n); my /= static_cast<double>(n);
    double sxx = 0.0, sxy = 0.0;
    for (std::size_t i = 0; i < n; ++i) {
        const double dx = std::log(static_cast<double>(i + 1)) - mx;
        sxx += dx * dx;
        sxy += dx * (std::log(var_n[i]) - my);
    }
    return -sxy / sxx;
}

}  // namespace

// THE BAR (design §7.6 S4 / §7.4 D1): the joint fold makes the carried variance
// fall as EXACTLY 1/n.  Asserted at every fold, not only at the end, so a
// trajectory that is right only on average cannot pass.
TEST(AmbFloatVehicleTest, JointBlockFoldMakesTheCarriedVarianceFallAsOneOverN) {
    const RecursionResult j = runRecursion(/*joint=*/true, 30);
    ASSERT_EQ(j.var_n.size(), 30u);
    for (std::size_t i = 0; i < j.var_n.size(); ++i) {
        const double ratio = j.var_n[i] * static_cast<double>(i + 1) / j.var_n[0];
        EXPECT_NEAR(ratio, 1.0, 1e-6)
            << "fold " << (i + 1) << ": var(n)*n/var(1) = " << ratio;
    }
    EXPECT_NEAR(fitB(j.var_n), 1.0, 1e-4);
}

// …and the LEGACY fold on the SAME fixture does NOT — the contrast is what proves
// the joint block is doing the work rather than the fixture being easy.  The
// legacy route folds each column's MARGINAL precision as an independent row, so
// the station-common direction and the LOS's iono direction are asserted twice
// per epoch and the variance collapses faster than 1/n, exactly as the F1 comment
// predicts and as the field trace measured (b = 3.44, RF-1 steps 1-2).
TEST(AmbFloatVehicleTest, LegacyDiagonalFoldCollapsesFasterThanOneOverN) {
    const RecursionResult d = runRecursion(/*joint=*/false, 30);
    ASSERT_EQ(d.var_n.size(), 30u);
    const double b = fitB(d.var_n);
    EXPECT_GT(b, 1.15) << "legacy b = " << b << " — the over-count did not appear, so the "
                          "1/n test above is not discriminating on this fixture";
    const double ratio30 = d.var_n[29] * 30.0 / d.var_n[0];
    EXPECT_LT(ratio30, 0.5) << "legacy var(30)*30/var(1) = " << ratio30;
    // And the two arms must actually differ — a guard against both routes silently
    // taking the same code path.
    const RecursionResult j = runRecursion(/*joint=*/true, 30);
    EXPECT_LT(d.var_n[29], 0.5 * j.var_n[29]);
}

// =============================================================================
// (7) THE DROP (design §6.2 tests 3 and 4) — RF-1 step 4.
//
// "A slip is a DROP of the prior, and only that."  The operator's contract is what
// separates it from the two things it replaces: a free epoch (which removes ALL
// information for one epoch, opening an unbounded direction the shared parameters
// re-balance into) and an erase (which destroys the arc's identity along with its
// confidence).  These cases pin both halves.
// =============================================================================

TEST(AmbFloatVehicleTest, DropRestoresTheBirthVarianceAndClearsTheOldRegression) {
    LocalCarry lc{-12.0, /*var=*/1.0e-6, /*last_seen_s=*/500.0};
    lc.var_first = 0.82;
    lc.var_birth = 0.82;
    lc.birth_ep  = 11;
    lc.birth_s   = 100.0;
    lc.relevel_s = 100.0;
    lc.n_folds   = 40;
    lc.coupling  = {GnssProcessor::CoupledShared{GnssProcessor::SharedParamKind::Clock,
                                                 "G01", "", 0, 0, 'C', 0, 0.8, 1.5, 0.2}};
    auto blk = std::make_shared<GnssProcessor::PlHarvestBlock>();
    blk->C = Eigen::MatrixXd::Identity(2, 2);
    lc.blk = blk;
    lc.blk_idx = 1;

    EXPECT_TRUE(GnssProcessor::ambFloatDrop(lc, /*floor_m2=*/0.0, /*now_s=*/900.0,
                                            /*now_ep=*/31));
    EXPECT_DOUBLE_EQ(lc.var, 0.82);              // restored to the birth prior, exactly
    EXPECT_TRUE(lc.coupling.empty());            // the regression belonged to the OLD value
    EXPECT_EQ(lc.blk, nullptr);
    EXPECT_EQ(lc.blk_idx, -1);
    EXPECT_DOUBLE_EQ(lc.relevel_s, 900.0);       // the age clock restarts
    EXPECT_EQ(lc.dropped_ep, 31);
}

// A DROP IS NOT AN ERASE (design §6.2 test 4).  The arc's identity survives, so the age
// gates, the sim slip oracle's window floor and the restored-arc sentinel all keep their
// meaning — and the level is still there for the lane-preserving refloat to use later.
TEST(AmbFloatVehicleTest, DropKeepsTheValueAndTheArcsIdentity) {
    LocalCarry lc{-12.0, 1.0e-6, 500.0};
    lc.var_first = 0.82; lc.var_birth = 0.82;
    lc.birth_ep = 11; lc.birth_s = 100.0; lc.n_folds = 40;
    GnssProcessor::ambFloatDrop(lc, 0.0, 900.0, 31);
    EXPECT_DOUBLE_EQ(lc.value_abs, -12.0);
    EXPECT_DOUBLE_EQ(lc.var_first, 0.82);
    EXPECT_DOUBLE_EQ(lc.var_birth, 0.82);
    EXPECT_EQ(lc.birth_ep, 11);
    EXPECT_DOUBLE_EQ(lc.birth_s, 100.0);
    EXPECT_DOUBLE_EQ(lc.last_seen_s, 500.0);
    EXPECT_EQ(lc.n_folds, 40);
}

// The floor can only LOOSEN.  An arc born tighter than the posture's floor is dropped to
// the floor; an arc born looser keeps its own (larger) birth variance.
TEST(AmbFloatVehicleTest, DropTakesTheLooserOfBirthVarianceAndFloor) {
    LocalCarry tight{0.0, 1e-6, 0.0};  tight.var_first = 0.01; tight.var_birth = 0.01;
    EXPECT_TRUE(GnssProcessor::ambFloatDrop(tight, /*floor_m2=*/0.25, 1.0, 1));
    EXPECT_DOUBLE_EQ(tight.var, 0.25);
    LocalCarry loose{0.0, 1e-6, 0.0};  loose.var_first = 4.0;  loose.var_birth = 4.0;
    EXPECT_TRUE(GnssProcessor::ambFloatDrop(loose, /*floor_m2=*/0.25, 1.0, 1));
    EXPECT_DOUBLE_EQ(loose.var, 4.0);
}

// An idempotent re-drop (two detectors on one arc in one epoch) must not tighten the
// prior — and must still restart the age clock, because the LEVEL is doubted either way.
TEST(AmbFloatVehicleTest, ReDroppingAnAlreadyDroppedArcDoesNotTightenIt) {
    LocalCarry lc{0.0, 1e-6, 0.0};
    lc.var_first = 0.5; lc.var_birth = 0.5;
    EXPECT_TRUE(GnssProcessor::ambFloatDrop(lc, 0.0, 10.0, 1));
    EXPECT_DOUBLE_EQ(lc.var, 0.5);
    EXPECT_FALSE(GnssProcessor::ambFloatDrop(lc, 0.0, 20.0, 2));   // already at birth
    EXPECT_DOUBLE_EQ(lc.var, 0.5);
    EXPECT_DOUBLE_EQ(lc.relevel_s, 20.0);
    EXPECT_EQ(lc.dropped_ep, 2);
}

// THE BIRTH FLOOR IS ELEVATION-MAPPED like the code row weights (design §3.3(b)).  A flat
// zenith floor would be optimistic at exactly the low elevations that own the metre-plus
// tail, so this pins the mapping, its zenith identity, and its two guards.
TEST(AmbFloatVehicleTest, BirthFloorIsElevationMappedLikeTheCodeWeights) {
    using GnssProcessor::ambFloatBirthFloor;
    constexpr double p = 1.0;                       // the default code elevation power
    EXPECT_DOUBLE_EQ(ambFloatBirthFloor(0.25, 90.0, p), 0.25);   // zenith = the knob
    // 30 degrees: sin = 0.5, so sigma^2 scales by 1/0.5^2 = 4.
    EXPECT_NEAR(ambFloatBirthFloor(0.25, 30.0, p), 1.0, 1e-12);
    // 15 degrees (the usual mask) is ~14.9x zenith at p = 1.
    EXPECT_NEAR(ambFloatBirthFloor(0.25, 15.0, p),
                0.25 / std::pow(std::sin(15.0 * 3.14159265358979323846 / 180.0), 2.0), 1e-12);
    // A gentler power gives a gentler ramp — the knob is the posture's, not a constant.
    EXPECT_LT(ambFloatBirthFloor(0.25, 15.0, 0.5), ambFloatBirthFloor(0.25, 15.0, 1.0));
    // Guards: the floor is OFF at 0, an unavailable elevation reads as zenith (the
    // conservative direction), and a grazing elevation cannot produce an infinite floor.
    EXPECT_DOUBLE_EQ(ambFloatBirthFloor(0.0, 15.0, p), 0.0);
    EXPECT_DOUBLE_EQ(ambFloatBirthFloor(0.25, -1.0, p), 0.25);
    EXPECT_TRUE(std::isfinite(ambFloatBirthFloor(0.25, 0.001, p)));
    EXPECT_GE(ambFloatBirthFloor(0.25, 0.001, p), 0.25);
}

// =============================================================================
// (7b) THE DROP-ONLY FLOOR — RF-3a
//      (docs/refound/RF-3_code_row_qc_2026-09-22.md §4 A1/A4).
//
// The RF-3 read separated two populations that the birth floor treats as one: a DROP
// hands back a variance the level no longer deserves (STHL00GBR G11 restored 0.036 m2
// from two hours earlier for a level wrong by 40 m, which then walked down for 24
// epochs), while a BIRTH is honest and a floor there costs 0.10 m of hour 1 at the
// held-out consumer.  These cases pin the separation: the drop floor raises what a drop
// restores, it never touches the birth stash, and it is mapped exactly as the birth
// floor is.
// =============================================================================

TEST(AmbFloatVehicleTest, DropFloorRaisesTheRestoredVarianceAboveBirthAndVarFirst) {
    // The arc's own history says 0.01 m2 and the posture's BIRTH floor says 0.25; the
    // DROP floor says 4.  The drop restores the largest of the three, and reports that
    // the drop floor is where the number came from.
    LocalCarry lc{7.5, /*var=*/1.0e-6, /*last_seen_s=*/0.0};
    lc.var_first = 0.01;
    lc.var_birth = 0.25;
    const auto o = GnssProcessor::ambFloatDropFloored(lc, /*birth_floor_m2=*/0.25,
                                                      /*drop_floor_m2=*/4.0,
                                                      /*now_s=*/900.0, /*now_ep=*/31);
    EXPECT_TRUE(o.acted);
    EXPECT_TRUE(o.from_drop_floor);
    EXPECT_DOUBLE_EQ(lc.var, 4.0);
    EXPECT_DOUBLE_EQ(lc.value_abs, 7.5);    // a drop is about confidence, not identity
    EXPECT_DOUBLE_EQ(lc.var_first, 0.01);   // the handover ratio's denominator is untouched
    EXPECT_DOUBLE_EQ(lc.var_birth, 0.25);   // so is the recorded birth prior
    EXPECT_EQ(lc.dropped_ep, 31);
}

TEST(AmbFloatVehicleTest, DropFloorIsNotTheSourceWhenBirthSideAlreadyExceedsIt) {
    // An arc born looser than the drop floor keeps its own variance, and the counter
    // must NOT claim the drop floor supplied it — otherwise the ON-path telemetry reads
    // as evidence where there is none (standing invariant #4).
    LocalCarry loose{0.0, 1e-6, 0.0};
    loose.var_first = 9.0;
    loose.var_birth = 9.0;
    const auto o1 = GnssProcessor::ambFloatDropFloored(loose, 0.25, 4.0, 1.0, 1);
    EXPECT_TRUE(o1.acted);
    EXPECT_FALSE(o1.from_drop_floor);
    EXPECT_DOUBLE_EQ(loose.var, 9.0);
    // The same when the BIRTH floor is the larger of the two floors.
    LocalCarry tight{0.0, 1e-6, 0.0};
    tight.var_first = 0.01;
    tight.var_birth = 0.01;
    const auto o2 = GnssProcessor::ambFloatDropFloored(tight, /*birth=*/6.0,
                                                       /*drop=*/4.0, 1.0, 1);
    EXPECT_TRUE(o2.acted);
    EXPECT_FALSE(o2.from_drop_floor);
    EXPECT_DOUBLE_EQ(tight.var, 6.0);
}

TEST(AmbFloatVehicleTest, DropFloorAtZeroIsTodaysBehaviourAndAnIdempotentDropIsNotCounted) {
    // Byte-identity of the OFF path, stated as a test: with the drop floor at 0 the
    // operator is exactly max(var_birth, var_first, birth floor).
    LocalCarry a{0.0, 1e-6, 0.0}; a.var_first = 0.5; a.var_birth = 0.5;
    LocalCarry b = a;
    EXPECT_TRUE(GnssProcessor::ambFloatDrop(a, /*floor_m2=*/0.0, 10.0, 1));
    const auto ob = GnssProcessor::ambFloatDropFloored(b, 0.0, 0.0, 10.0, 1);
    EXPECT_TRUE(ob.acted);
    EXPECT_FALSE(ob.from_drop_floor);
    EXPECT_DOUBLE_EQ(a.var, b.var);
    // An arc ALREADY at or above the floor is an idempotent re-drop: the variance does
    // not move, so the drop floor is not its source even though it is armed.
    const auto o2 = GnssProcessor::ambFloatDropFloored(b, 0.0, 0.25, 20.0, 2);
    EXPECT_FALSE(o2.acted);
    EXPECT_FALSE(o2.from_drop_floor);
    EXPECT_DOUBLE_EQ(b.var, 0.5);
    EXPECT_DOUBLE_EQ(b.relevel_s, 20.0);   // the age clock still restarts
}

TEST(AmbFloatVehicleTest, DropFloorUsesTheSameElevationMappingAsTheBirthFloor) {
    // One mapping, one function: the drop floor is stated at zenith and read at the
    // line of sight's elevation through ambFloatBirthFloor, exactly as the birth floor
    // is, so a floor that is honest at zenith is not optimistic at 15 degrees.
    using GnssProcessor::ambFloatBirthFloor;
    constexpr double p = 1.0;
    const double zen = 4.0;
    const double low = ambFloatBirthFloor(zen, 15.0, p);
    EXPECT_GT(low, zen);
    LocalCarry hi{0.0, 1e-9, 0.0};  hi.var_first = 0.0; hi.var_birth = 0.0;
    LocalCarry lo = hi;
    GnssProcessor::ambFloatDropFloored(hi, 0.0, ambFloatBirthFloor(zen, 90.0, p), 1.0, 1);
    GnssProcessor::ambFloatDropFloored(lo, 0.0, low, 1.0, 1);
    EXPECT_DOUBLE_EQ(hi.var, zen);
    EXPECT_DOUBLE_EQ(lo.var, low);
    EXPECT_GT(lo.var, hi.var);
}

TEST(AmbFloatVehicleTest, EveryDropArmsTheWalkMetricIncludingAnIdempotentOne) {
    // RF-3a A2: the WALK metric's population is "every drop that doubted the level",
    // which is what lets an OFF run report the walk it suffers.  The phase is armed on
    // both branches of the operator and the harvest is what advances it.
    LocalCarry moved{0.0, 1e-6, 0.0}; moved.var_first = 0.5; moved.var_birth = 0.5;
    EXPECT_TRUE(GnssProcessor::ambFloatDrop(moved, 0.0, 10.0, 1));
    EXPECT_EQ(moved.drop_walk_ph, 1);
    LocalCarry idem{0.0, 4.0, 0.0}; idem.var_first = 0.5; idem.var_birth = 0.5;
    EXPECT_FALSE(GnssProcessor::ambFloatDrop(idem, 0.0, 10.0, 1));
    EXPECT_EQ(idem.drop_walk_ph, 1);
}

// =============================================================================
// (8) THE DECLARED ANCHOR S-TRANSFORM (design §6.2 test 6) — RF-1 step 5.
//
// A frame step is a KNOWN, exact basis change, so the carry is EXPRESSED in the new
// basis rather than erased.  The operator's three references move TOGETHER — the
// per-station ambiguity levels, the satellite phase bias, and every coupling s_prev
// on that phase-bias column — because a Card-1 coupling row's correct y-change under
// a known frame step is the exact CANCELLATION of the two moves, and shifting one
// without the other injects the frame step as row bias
// (NetworkChainDatum.cpp:852-866, the filed latent defect the design names).
//
// The live seam needs the whole run context (the election, the ledger, the trace
// streams), which a unit test cannot stand up; what IS testable, and is what the
// defect is about, is the ALGEBRA — so these cases pin the operator's invariants on
// the same data structures the seam mutates.
// =============================================================================

TEST(AmbFloatVehicleTest, AnchorStransformShiftsEveryCarryByTheIncomingAnchorsLevel) {
    using Key = std::tuple<int, std::string, std::string>;
    // Three stations carrying G07/L1C; station 1 is the incoming anchor.
    const Key k0{0, "G07", "L1C"}, k1{1, "G07", "L1C"}, k2{2, "G07", "L1C"};
    const Key kOther{0, "G07", "L2W"};                       // a DIFFERENT family
    std::map<Key, LocalCarry> carry;
    carry[k0]     = LocalCarry{12.00, 1e-4, 100.0};
    carry[k1]     = LocalCarry{-3.50, 2e-4, 100.0};          // the incoming anchor
    carry[k2]     = LocalCarry{ 7.25, 3e-4, 100.0};
    carry[kOther] = LocalCarry{99.00, 5e-4, 100.0};
    const double delta = carry[k1].value_abs;

    for (auto& [k, lc] : carry) {
        if (std::get<1>(k) != "G07" || std::get<2>(k) != "L1C") continue;
        lc.value_abs -= delta;
    }
    // The incoming anchor reads 0 — what its w = 1e10 pin is about to assert.
    EXPECT_NEAR(carry[k1].value_abs, 0.0, GnssProcessor::kStsExactTolM);
    // Every other station moved by exactly the same Δ, so BETWEEN-STATION differences —
    // the only datum-invariant content of the family — are preserved exactly.
    EXPECT_DOUBLE_EQ(carry[k0].value_abs, 12.00 + 3.50);
    EXPECT_DOUBLE_EQ(carry[k2].value_abs,  7.25 + 3.50);
    EXPECT_DOUBLE_EQ(carry[k0].value_abs - carry[k2].value_abs, 12.00 - 7.25);
    // A different (sat, signal) family is untouched — the transform is per-family.
    EXPECT_DOUBLE_EQ(carry[kOther].value_abs, 99.00);
    // VARIANCES UNTOUCHED: this is a basis change, not a refloat.  That is the whole
    // difference from the erase it replaces and from step 4's drop.
    EXPECT_DOUBLE_EQ(carry[k0].var, 1e-4);
    EXPECT_DOUBLE_EQ(carry[k1].var, 2e-4);
    EXPECT_DOUBLE_EQ(carry[k2].var, 3e-4);
}

// THE FILED LATENT DEFECT (i): the coupling s_prev on the shifted PHASE-BIAS column must
// move WITH the levels, and only on that column.  rebaseCouplingPhaseBiasSprev is the
// one operator both D4 and this transform call, so the rule cannot drift between them.
TEST(AmbFloatVehicleTest, AnchorStransformMovesTheCouplingSprevOnTheSamePhaseBiasColumn) {
    using GnssProcessor::CoupledShared;
    using GnssProcessor::SharedParamKind;
    const double delta = -3.5;
    std::vector<CoupledShared> coupling = {
        CoupledShared{SharedParamKind::PhaseBias, "G07", "L1C", 0, 0, 'C', 0, 1.0, 20.0, 0.1},
        CoupledShared{SharedParamKind::PhaseBias, "G07", "L2W", 0, 0, 'C', 0, 1.0, 30.0, 0.1},
        CoupledShared{SharedParamKind::Clock,     "G07", "",    0, 0, 'C', 0, 0.8,  5.0, 0.1},
    };
    const std::map<std::pair<std::string, std::string>, double> shift{{{"G07", "L1C"}, +delta}};
    const long long n = GnssProcessor::rebaseCouplingPhaseBiasSprev(coupling, shift);
    EXPECT_EQ(n, 1);
    EXPECT_DOUBLE_EQ(coupling[0].s_prev, 20.0 + delta);   // the shifted PB column
    EXPECT_DOUBLE_EQ(coupling[1].s_prev, 30.0);           // a different signal — untouched
    EXPECT_DOUBLE_EQ(coupling[2].s_prev,  5.0);           // the CLOCK — never moved by this
}

// The transform is EXACTLY invertible and composes: applying it twice with the second
// Δ read from the already-shifted carry is the identity, which is what makes a chain of
// handovers frame-consistent instead of drifting.
TEST(AmbFloatVehicleTest, AnchorStransformIsIdempotentOnceTheAnchorReadsZero) {
    LocalCarry in{-3.5, 2e-4, 100.0};
    LocalCarry other{12.0, 1e-4, 100.0};
    double d1 = in.value_abs;
    in.value_abs -= d1;  other.value_abs -= d1;
    const double d2 = in.value_abs;                   // now 0
    EXPECT_DOUBLE_EQ(d2, 0.0);
    in.value_abs -= d2;  other.value_abs -= d2;       // a second transform is the identity
    EXPECT_DOUBLE_EQ(in.value_abs, 0.0);
    EXPECT_DOUBLE_EQ(other.value_abs, 15.5);
}

// The value must converge to the true ambiguity, not merely become precise: a
// variance that falls as 1/n around a WRONG level is exactly design risk #1 (the
// card could deliver a correctly-converging state carrying a wrong level).
//
// The bar is the estimator's OWN claimed precision, not a hand-picked metre
// figure: with zero epoch redundancy the ambiguity is separated from the LOS iono
// by CODE, so the settled error is a metre-scale draw averaged over n folds and
// any absolute bar would be a restatement of the noise model.  |error| < 3 sigma
// of the carried variance is the honest statement, and it tests BOTH halves at
// once — a variance that fell too fast would fail it even with the value right.
TEST(AmbFloatVehicleTest, JointBlockFoldConvergesToTheTrueAmbiguity) {
    const RecursionResult j = runRecursion(/*joint=*/true, 30);
    const double e1  = std::abs(j.value_n[0]  - 7.5);
    const double e30 = std::abs(j.value_n[29] - 7.5);
    EXPECT_LT(e30, e1) << "first-fold |error| " << e1 << " -> settled " << e30;
    EXPECT_LT(e30, 3.0 * std::sqrt(j.var_n[29]))
        << "settled |error| " << e30 << " m against 3 sigma = "
        << 3.0 * std::sqrt(j.var_n[29]) << " m (sigma = "
        << std::sqrt(j.var_n[29]) << " m)";
}

// D4 IN ONE ASSERTION, and the reason the two halves of the card are separable.
//
// Add the drift the ambiguity absorbs (a phase-only receiver bias the model has no
// state for) and BOTH routes' levels move — the level's CONTENT is not what the
// fold fixes, which is exactly the "vehicle correct but not sufficient" branch of
// §7.6's frozen readings.  What separates them is the variance they claim while it
// happens: the legacy route has already collapsed, so its standardized error is far
// larger.  That ratio is D4's σ̂₀ contrast (field 101 on the collapsed route, 0.23 on
// the sim's block route) reproduced at the seam, on 30 folds instead of 6 hours.
//
// ⚠ The clean fixture (no drift) does NOT show this — there both routes land on the
// truth and σ̂₀ stays near 1 whatever the variance does.  A collapsed variance is
// only DISHONEST against something that moves; that is why the field's σ̂₀ grows with
// arc age and why this case carries the drift explicitly rather than inheriting it.
TEST(AmbFloatVehicleTest, LegacyDiagonalFoldClaimsAPrecisionTheDriftingLevelDoesNotDeserve) {
    constexpr double kDrift = 0.004;    // 4 mm/epoch of un-modelled phase bias
    const RecursionResult d = runRecursion(/*joint=*/false, 30, kDrift);
    const RecursionResult j = runRecursion(/*joint=*/true,  30, kDrift);
    const double zd = std::abs(d.value_n[29] - 7.5) / std::sqrt(d.var_n[29]);
    const double zj = std::abs(j.value_n[29] - 7.5) / std::sqrt(j.var_n[29]);
    EXPECT_GT(zd, 2.0 * zj)
        << "legacy |error|/sigma = " << zd << " vs joint " << zj
        << " (legacy var " << d.var_n[29] << " vs joint " << j.var_n[29] << ")";
    // …and the drift is genuinely absorbed by the carry on BOTH routes — the
    // control that stops this passing because one route simply diverged.
    EXPECT_GT(std::abs(d.value_n[29] - 7.5), 0.01);
    EXPECT_GT(std::abs(j.value_n[29] - 7.5), 0.01);
}
