/**
 * @file test_prefit_census.cpp
 * @brief Prefit census, structural fuse, seed-birth tripwire, topology ledger and
 *        flight recorder (docs/SPEC_prefit_census_observability.md).
 *
 * The producer's only always-on consistency statistic is a post-fit one, and free
 * ambiguities plus free local states absorb prefit inconsistency out of it by
 * construction.  Example (era 2): a steady +1.88e5 m prefit code innovation on
 * (BSHM00ISR, G07), folded at w ≈ 2 every epoch for minutes, left the post-fit σ̂₀
 * clean throughout.
 *
 * The tests target claims that would fail silently if wrong, not whether the
 * arithmetic runs:
 *
 *   1. The metric has the common mode removed.  A raw-|y| screen would fire on every
 *      1 ms receiver clock step (3e5 m on every row of a station at once, a
 *      legitimate quantity the white clock state absorbs exactly).  The centred
 *      metric is ~0 there and large (1.88e5 m × √w, 2.7e5 at the era-2 w ≈ 2) on a
 *      single parked cell in the same station.  With this reversed, the instrument
 *      would generate false positives and the fuse would cap their weights.
 *   2. Pins and coupling rows are not data.  A healthy gauge pin runs s ≈ 1e7 by
 *      design; admitting one into the pool would swamp the census and make the fuse
 *      cut the datum.
 *   3. The fuse caps rows and does not drop them (the prefer-fallback directive).
 *      It caps to exactly an S_fuse-equivalent standardized load, so the row keeps
 *      its sign and its ordering.
 *   4. A thin type has no centre.  Three rows do not make a median; treating a
 *      missing centre as m = 0 would turn every sparse station into an exceedance
 *      (and, with the fuse armed, into a solve change).
 *   5. Structural inertness when off.  Every increment is off by default and leaves
 *      the weights bit-identical and the counters at zero.
 *   6. The recorder latch is an era latch, not a fire log: one trip per era, re-armed
 *      when the era clears.  Otherwise a 95-minute withhold era would write 190
 *      attribution frames.
 */

#include <gtest/gtest.h>

#include <Eigen/Dense>

#include <cmath>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

#include <spdlog/sinks/null_sink.h>
#include <spdlog/spdlog.h>

#include "app/NetworkRunContext.hpp"
#include "config/GnssConfig.hpp"
#include "filter/NetworkEstimator.hpp"
#include "filter/NetworkPrefitCensus.hpp"
#include "filter/NetworkRhsAttrib.hpp"
#include "filter/NetworkRowClassify.hpp"
#include "filter/NetworkRunTypes.hpp"
#include "logging/LogManager.hpp"
#include "logging/NetworkRunTraces.hpp"
#include "rinex/MultiRinexSync.hpp"

namespace fs = std::filesystem;
using namespace GnssProcessor;

namespace {

// ---------------------------------------------------------------------------
//  Column layout used by every fixture below.  Four shared columns suffice to
//  exercise all four data classes plus both pin classes:
//    0 CLK:G07      (a clock-only code row touches it alone)
//    1 CB:G07:C1W   (a code row's code bias)
//    2 PB:G07:L1C   (a phase row's phase bias)
//    3 SH:C:0:0     (the SH<->slant tie)
//  The class of a row is read from its tag (card NOM step N2b), which makeStation sets
//  from the recipe (tagOf); the columns give the rows their geometry.
// ---------------------------------------------------------------------------
constexpr int kCols = 4;
enum Col { kClk = 0, kCb = 1, kPb = 2, kSh = 3 };

/// Row recipe: which shared column it touches, its innovation and its weight.
/// col = -1 marks a row with no shared partial (the pin classes).
struct RowSpec {
    int    col;      // shared column, or -1 for a local-only row
    double y;
    double w;
};

/// The contribution tag a row of the recipe carries (card NOM step N2b: the classifier
/// reads the tag, not the columns): a code-bias row and a clock-only row are code
/// observations, a phase-bias row a phase observation, an SH row a tie, a local-only row
/// an ambiguity pin (its weight decides pivot or local), and a row at or past
/// @p cpl_row_start a coupling row.  Every tag names G07 except the ties': a production
/// tie names its own line of sight, the fixture's ties name none, so they stay out of the
/// per-satellite cells these tests count (presence, births, row kinds), as the ties of
/// other lines of sight would.
RowTag tagOf(const RowSpec& s, Eigen::Index row, Eigen::Index cpl_row_start) {
    using CK = ContributionKind;
    if (cpl_row_start >= 0 && row >= cpl_row_start) return makeRowTag(CK::CouplingScalar, "G07", "L1C");
    switch (s.col) {
        case kCb:  return makeRowTag(CK::DataCode, "G07", "C1W");
        case kClk: return makeRowTag(CK::DataCode, "G07", "C1W");
        case kPb:  return makeRowTag(CK::DataPhase, "G07", "L1C");
        case kSh:  return makeRowTag(CK::ShIonoTie, "");
        default:   return makeRowTag(CK::AmbiguityPin, "G07", "L1C");
    }
}

/// Build a station block from row recipes, with its row tags.  One local column exists
/// so the local-only rows have somewhere to point.
StationAssembly makeStation(const std::vector<RowSpec>& rows,
                            Eigen::Index cpl_row_start = -1) {
    const auto m = static_cast<Eigen::Index>(rows.size());
    StationAssembly R;
    R.ok = true;
    R.A_s = Eigen::MatrixXd::Zero(m, kCols);
    R.A_r = Eigen::MatrixXd::Zero(m, 1);
    R.w.resize(m);
    R.y.resize(m);
    R.cpl_row_start = cpl_row_start;
    for (Eigen::Index i = 0; i < m; ++i) {
        const RowSpec& s = rows[static_cast<std::size_t>(i)];
        if (s.col >= 0) R.A_s(i, s.col) = 1.0;
        else            R.A_r(i, 0) = 1.0;
        R.y(i) = s.y;
        R.w(i) = s.w;
        R.row_tags.push_back(tagOf(s, i, cpl_row_start));
    }
    return R;
}

/// n identical code rows; a caller appends any displaced row itself.
std::vector<RowSpec> codeRows(int n, double y, double w) {
    return std::vector<RowSpec>(static_cast<std::size_t>(n), RowSpec{ kCb, y, w });
}

std::shared_ptr<spdlog::logger> nullLogger() {
    static auto lg = std::make_shared<spdlog::logger>(
        "prefit_census_test", std::make_shared<spdlog::sinks::null_sink_mt>());
    return lg;
}

fs::path scratchDir(const std::string& tag) {
    const fs::path p = fs::temp_directory_path() / ("prefit_census_" + tag);
    std::error_code ec;
    fs::create_directories(p, ec);
    return p;
}

std::string configJson(const std::string& net_extra) {
    return std::string("{\n")
        + "  \"processing\": {\n"
          "    \"mode\": \"ppp-rtk-network\",\n"
          "    \"active_systems\": [ \"GPS\" ],\n"
          "    \"signal_priorities\": { \"GPS\": \"WC\" }\n"
          "  },\n"
          "  \"network\": {\n"
          "    \"clock_datum_station\": \"AAAA\",\n"
        + net_extra +
        "    \"stations\": [\n"
        "      { \"name\": \"AAAA\", \"position_ecef\": [ -2493304.99, -4655214.79, 3565497.67 ] },\n"
        "      { \"name\": \"BBBB\", \"position_ecef\": [ -2281621.75, -1453585.08, 5756964.89 ] }\n"
        "    ]\n"
        "  }\n"
        "}\n";
}

std::unique_ptr<GnssConfig> loadCfg(LogManager& log, const std::string& tag,
                                    const std::string& net_extra) {
    auto cfg = std::make_unique<GnssConfig>();
    const fs::path p = fs::temp_directory_path() / ("prefit_census_cfg_" + tag + ".json");
    { std::ofstream f(p); f << configJson(net_extra); }
    const bool ok = cfg->loadFromJson(p, log);
    std::error_code ec; fs::remove(p, ec);
    EXPECT_TRUE(ok) << "loadFromJson failed for tag " << tag;
    return cfg;
}

/// An estimator carrying exactly the four shared columns of the layout above, so the
/// census seam's own column pass reproduces it.
void registerColumns(NetworkEstimator& est) {
    est.registerClock("G07");
    est.registerCodeBias("G07", "C1W");
    est.registerPhaseBias("G07", "L1C");
    est.registerVtecCoeff(0, 0, 'C');
}

SyncedEpoch epochAt(double tow) {
    SyncedEpoch se;
    se.time.week = 2313;
    se.time.tow = tow;
    return se;
}

/// Count rows in an events CSV whose `event` column equals @p want.
int countEvents(const fs::path& csv, const std::string& want) {
    std::ifstream f(csv);
    std::string line;
    int n = 0;
    while (std::getline(f, line)) {
        // week,tow,tier,event,...
        std::size_t p = 0;
        int field = 0;
        std::string ev;
        while (field <= 3) {
            const std::size_t q = line.find(',', p);
            const std::string tok = line.substr(p, q == std::string::npos ? q : q - p);
            if (field == 3) ev = tok;
            if (q == std::string::npos) break;
            p = q + 1;
            ++field;
        }
        if (ev == want) ++n;
    }
    return n;
}

} // namespace

// ===========================================================================
//  1. The metric: common mode removed, not raw
// ===========================================================================
//
// The design depends on this property.  A 1 ms receiver clock step puts |y| = 3e5 m
// on every row of a station at once and is absorbed exactly by the white
// receiver-clock state; such steps occur on the validation leg.  A parked cell
// displaces one row against its station's own median.  Raw magnitude cannot tell
// the two apart; the centred statistic separates them (0 against 3.76e5 here).
TEST(PrefitCensusObservability, CentredMetricCancelsAClockStepAndKeepsAParkedCell) {

    // Six code rows at 3e5 m (the clock step) and one at 3e5 + 1.88e5 (the park).
    std::vector<RowSpec> rows = codeRows(6, 3.0e5, 4.0);
    rows.push_back({ kCb, 3.0e5 + 1.88e5, 4.0 });
    const StationAssembly R = makeStation(rows);

    const PrefitStationCentres c = prefitStationCentres(R);
    ASSERT_TRUE(c.have[static_cast<int>(NetRowClass::ObsCode)]);
    EXPECT_DOUBLE_EQ(c.m[static_cast<int>(NetRowClass::ObsCode)], 3.0e5);

    const double m = c.m[static_cast<int>(NetRowClass::ObsCode)];
    for (int i = 0; i < 6; ++i)
        EXPECT_DOUBLE_EQ(prefitStandardizedDeviation(R.y(i), R.w(i), m), 0.0)
            << "the clock-step rows must vanish under centring, row " << i;
    // 1.88e5 m x sqrt(4) = 3.76e5, the era-2 cell's order of magnitude.
    EXPECT_NEAR(prefitStandardizedDeviation(R.y(6), R.w(6), m), 3.76e5, 1.0);

    // …and the raw screen this replaces would have flagged all seven rows.
    for (Eigen::Index i = 0; i < R.y.size(); ++i)
        EXPECT_GT(prefitStandardizedDeviation(R.y(i), R.w(i), 0.0), 1000.0);
}

// ===========================================================================
//  2. Classification: pins and coupling rows are not data
// ===========================================================================
//
// A healthy pivot pin carries w = 1e10 against a zero innovation and would
// contribute s ~ 1e7 as soon as it entered a pool.  A card-1 coupling row is a
// statement about the carry, not a measurement.  Both are excluded from the census
// and, more importantly, from the fuse, which would otherwise cap the datum.
TEST(PrefitCensusObservability, PinAndCouplingRowsAreExcludedFromEveryPool) {
    const std::vector<RowSpec> rows = {
        { kCb, 1.0, 4.0 },        // 0 obs_code
        { kPb, 0.01, 1.0e4 },     // 1 obs_phase
        { kClk, 0.5, 1.0 },       // 2 obs_other  (its tag is made unset below)
        { kSh, 2.0, 1.0 },        // 3 iono_sh_row
        { -1, 0.0, 1.0e10 },      // 4 pivot_pin   (an ambiguity pin, hard weight)
        { -1, 0.3, 1.0 },         // 5 local_pin_row
        { kCb, 9.0, 1.0 },        // 6 cpl_row     (a coupling row, past cpl_row_start)
    };
    StationAssembly R = makeStation(rows, /*cpl_row_start=*/6);
    R.row_tags[2].kind = ContributionKind::Unset;   // a row no site named: unclassified

    EXPECT_EQ(classifyRow(R, 0, nullptr), NetRowClass::ObsCode);
    EXPECT_EQ(classifyRow(R, 1, nullptr), NetRowClass::ObsPhase);
    EXPECT_EQ(classifyRow(R, 2, nullptr), NetRowClass::ObsOther);
    EXPECT_EQ(classifyRow(R, 3, nullptr), NetRowClass::IonoShRow);
    EXPECT_EQ(classifyRow(R, 4, nullptr), NetRowClass::PivotPin);
    EXPECT_EQ(classifyRow(R, 5, nullptr), NetRowClass::LocalPinRow);
    EXPECT_EQ(classifyRow(R, 6, nullptr), NetRowClass::CplRow);

    EXPECT_FALSE(isDataRowClass(NetRowClass::PivotPin));
    EXPECT_FALSE(isDataRowClass(NetRowClass::LocalPinRow));
    EXPECT_FALSE(isDataRowClass(NetRowClass::CplRow));

    // Exactly one row per data class reached a pool; the three excluded classes
    // contributed nothing.
    const PrefitStationCentres c = prefitStationCentres(R);
    for (int t = 0; t < kNetDataClasses; ++t) EXPECT_EQ(c.n[t], 1) << "class " << t;

    // The row's satellite comes from its tag.
    std::string sat;
    classifyRow(R, 0, &sat);
    EXPECT_EQ(sat, "G07");
}

// ===========================================================================
//  3. iono_sh_row is not centred; a thin type has no centre
// ===========================================================================
//
// The SH<->slant tie carries no receiver clock, so subtracting a median from it
// would remove the field offset the row exists to observe.  Conversely, a type with
// three rows has no reliable median: treating the absent centre as m = 0 would turn
// every sparse station into an exceedance and, with the fuse armed, into a silent
// solve change.
TEST(PrefitCensusObservability, ShTiesAreUncentredAndThinTypesHaveNoCentre) {
    std::vector<RowSpec> rows(4, RowSpec{ kSh, 2.0, 1.0 });   // 4 identical SH ties
    rows.push_back({ kCb, 5.0, 1.0 });                        // 3 code rows only:
    rows.push_back({ kCb, 5.0, 1.0 });                        //   below the minimum
    rows.push_back({ kCb, 5.0, 1.0 });
    const StationAssembly R = makeStation(rows);

    const PrefitStationCentres c = prefitStationCentres(R);
    const int sh = static_cast<int>(NetRowClass::IonoShRow);
    const int cd = static_cast<int>(NetRowClass::ObsCode);

    EXPECT_TRUE(c.have[sh]);
    EXPECT_DOUBLE_EQ(c.m[sh], 0.0) << "an SH tie row must keep its raw innovation";
    EXPECT_DOUBLE_EQ(prefitStandardizedDeviation(R.y(0), R.w(0), c.m[sh]), 2.0);

    EXPECT_EQ(c.n[cd], 3);
    EXPECT_FALSE(c.have[cd]) << "3 < kNetPrefitMinTypeRows: no centre, so no statistic";
    EXPECT_EQ(kNetPrefitMinTypeRows, 4);
}

// ===========================================================================
//  4. The fuse caps to an S_fuse-equivalent load and never drops a row
// ===========================================================================
TEST(PrefitCensusObservability, FuseCapsToSFuseEquivalentLoadAndKeepsTheRow) {
    LogManager log;
    auto cfg = loadCfg(log, "fuse_cap", "    \"prefit_fuse_sigma\": 1000.0,\n");
    ASSERT_GT(cfg->getNetworkConfig().prefit_fuse_sigma, 0.0);
    NetworkRunContext ctx(*cfg, cfg->getNetworkConfig());
    ASSERT_DOUBLE_EQ(ctx.knobs.pc_fuse_sigma, 1000.0);

    NetworkRunTraces traces(scratchDir("fuse_cap"));

    std::vector<RowSpec> rows = codeRows(6, 0.0, 4.0);
    rows.push_back({ kCb, 1.0e5, 4.0 });        // s = 2e5, far over S_fuse
    rows.push_back({ kCb, 100.0, 4.0 });        // s = 200, under S_fuse
    StationAssembly R = makeStation(rows);
    const Eigen::VectorXd w0 = R.w;

    const int capped = networkPrefitFuseStation(ctx, traces, epochAt(1000.0).time, kCols,
                                                "BSHM00ISR", R, 1000.0);
    EXPECT_EQ(capped, 1);

    // The capped row still exists, still carries its sign, and now contributes
    // exactly an S_fuse-equivalent standardized load.
    EXPECT_DOUBLE_EQ(R.y(6), 1.0e5) << "the fuse must not touch the innovation";
    EXPECT_GT(R.w(6), 0.0) << "a cap is not a drop (prefer-fallback directive)";
    const double s_pre = 1.0e5 * std::sqrt(4.0);
    EXPECT_NEAR(R.w(6), 4.0 * (1000.0 / s_pre) * (1000.0 / s_pre), 1e-12);
    EXPECT_NEAR(prefitStandardizedDeviation(R.y(6), R.w(6), 0.0), 1000.0, 1e-6);

    // Every other row is bit-identical.
    for (Eigen::Index i = 0; i < R.w.size(); ++i)
        if (i != 6) EXPECT_EQ(R.w(i), w0(i)) << "row " << i;

    EXPECT_EQ(ctx.counters.pc.fuse_capped, 1);
    EXPECT_NEAR(ctx.counters.pc.fuse_max_s, s_pre, 1e-6);
}

// ===========================================================================
//  5. Fuse exclusions: pin weights and centre-less types
// ===========================================================================
//
// Both exclusions are necessary, and both are counted.  A pin at w = 1e10 would be
// capped to nothing (removing the datum); a row whose type has no median would be
// measured against an invented m = 0.
TEST(PrefitCensusObservability, FuseSkipsPinWeightsAndCentrelessTypes) {
    LogManager log;
    auto cfg = loadCfg(log, "fuse_skip", "    \"prefit_fuse_sigma\": 1000.0,\n");
    NetworkRunContext ctx(*cfg, cfg->getNetworkConfig());
    NetworkRunTraces traces(scratchDir("fuse_skip"));

    // A pin-weight data row (w >= 1e6) with a huge innovation, a code pool
    // deliberately one row short of a centre, and an SH tie that needs none.
    std::vector<RowSpec> rows = {
        { kPb, 1.0e6, 1.0e8 },    // pin/constraint weight regime: skipped
        { kCb, 1.0e5, 4.0 },      // its type has only 3 rows -> no centre
        { kCb, 0.0, 4.0 },
        { kCb, 0.0, 4.0 },
        { kSh, 1.0e4, 1.0 },      // SH tie, uncentred, s = 1e4 -> capped
    };
    StationAssembly R = makeStation(rows);
    const Eigen::VectorXd w0 = R.w;

    const int capped = networkPrefitFuseStation(ctx, traces, epochAt(1000.0).time, kCols,
                                                "AAAA", R, 1000.0);
    EXPECT_EQ(capped, 1) << "only the SH tie is both over threshold and eligible";
    EXPECT_EQ(R.w(0), w0(0)) << "pin-weight row untouched";
    EXPECT_EQ(R.w(1), w0(1)) << "centre-less type untouched";
    EXPECT_LT(R.w(4), w0(4));
    EXPECT_GE(ctx.counters.pc.fuse_skip_pin_w, 1);
    EXPECT_GE(ctx.counters.pc.fuse_skip_thin, 3);
}

// ===========================================================================
//  5b. The fuse domain keeps phase out by default
// ===========================================================================
//
// Recorded deviation from SPEC §4 ("DATA-class rows only"), on measured evidence: a
// phase row's prefit innovation carries its arc's free ambiguity linearization
// point, so on a warm-resume epoch, with every arc re-seeded at once, healthy rows
// reach s ~ 1e5 (a phase median of −4248 m two epochs after a resume on the FT63
// leg-19 replay).  Capping those would slow every new arc with no fault present.
// The default domain is therefore the classes with no free per-row absorber;
// widening it is a deliberate arm, and what the narrower domain skips is counted.
TEST(PrefitCensusObservability, FuseDomainExcludesPhaseByDefaultAndCountsWhatItGaveUp) {
    EXPECT_EQ(parseDataClassMask("code,sh"),
              dataClassBit(NetRowClass::ObsCode) | dataClassBit(NetRowClass::IonoShRow));
    EXPECT_EQ(parseDataClassMask("all"), 0xFu);
    EXPECT_EQ(parseDataClassMask("none"), 0u);
    EXPECT_EQ(parseDataClassMask("CODE + Phase"),
              dataClassBit(NetRowClass::ObsCode) | dataClassBit(NetRowClass::ObsPhase));
    std::vector<std::string> unknown;
    EXPECT_EQ(parseDataClassMask("code,bogus", &unknown),
              dataClassBit(NetRowClass::ObsCode));
    ASSERT_EQ(unknown.size(), 1u);
    EXPECT_EQ(unknown[0], "bogus");
    EXPECT_EQ(dataClassMaskName(parseDataClassMask("code,sh")), "obs_code+iono_sh_row");

    LogManager log;
    auto cfg = loadCfg(log, "fuse_domain", "    \"prefit_fuse_sigma\": 1000.0,\n");
    EXPECT_EQ(cfg->getNetworkConfig().prefit_fuse_classes, "code,sh");
    NetworkRunContext ctx(*cfg, cfg->getNetworkConfig());
    EXPECT_EQ(ctx.knobs.pc_fuse_mask,
              dataClassBit(NetRowClass::ObsCode) | dataClassBit(NetRowClass::IonoShRow));

    NetworkRunTraces traces(scratchDir("fuse_domain"));
    // A station mid-resume: every phase row displaced from an extreme median, plus
    // one parked code row.
    std::vector<RowSpec> rows = codeRows(6, 0.0, 4.0);
    rows.push_back({ kCb, 1.0e5, 4.0 });                       // code: in domain, capped
    for (int i = 0; i < 5; ++i) rows.push_back({ kPb, -4248.0, 8000.0 });
    rows.push_back({ kPb, 0.0, 8000.0 });                      // a freshly seeded arc
    StationAssembly R = makeStation(rows);
    const Eigen::VectorXd w0 = R.w;

    EXPECT_EQ(networkPrefitFuseStation(ctx, traces, epochAt(1000.0).time, kCols,
                                       "MRO100AUS", R, 1000.0), 1);
    EXPECT_LT(R.w(6), w0(6)) << "the parked CODE cell is still fused";
    for (Eigen::Index i = 7; i < R.w.size(); ++i)
        EXPECT_EQ(R.w(i), w0(i)) << "phase row " << i << " must be untouched by default";
    EXPECT_GT(ctx.counters.pc.fuse_skip_class, 0);
    EXPECT_GT(ctx.counters.pc.fuse_would_class, 0)
        << "the domain restriction must report what it gave up, not hide it";
}

// ===========================================================================
//  6. Off is structurally inert
// ===========================================================================
TEST(PrefitCensusObservability, DefaultsAreOffAndTheFuseIsStructurallyInert) {
    LogManager log;
    auto cfg = loadCfg(log, "off", "");
    const NetworkConfig& nc = cfg->getNetworkConfig();
    EXPECT_FALSE(nc.trace_prefit_census);
    EXPECT_FALSE(nc.trace_topology);
    EXPECT_FALSE(nc.rhs_attrib_on_trip);
    EXPECT_DOUBLE_EQ(nc.prefit_fuse_sigma, 0.0);
    EXPECT_DOUBLE_EQ(nc.seed_birth_check_sigma, 0.0);

    NetworkRunContext ctx(*cfg, nc);
    EXPECT_FALSE(ctx.knobs.pc_walk_on);
    EXPECT_FALSE(ctx.knobs.rec_on);
    EXPECT_DOUBLE_EQ(ctx.knobs.pc_fuse_sigma, 0.0);

    NetworkRunTraces traces(scratchDir("off"));
    std::vector<RowSpec> rows = codeRows(6, 0.0, 4.0);
    rows.push_back({ kCb, 1.0e9, 4.0 });     // absurd, and still untouched
    StationAssembly R = makeStation(rows);
    const Eigen::VectorXd w0 = R.w;

    EXPECT_EQ(networkPrefitFuseStation(ctx, traces, epochAt(1000.0).time, kCols,
                                       "AAAA", R, ctx.knobs.pc_fuse_sigma), 0);
    EXPECT_EQ(R.w, w0);
    EXPECT_EQ(ctx.counters.pc.fuse_station_epochs, 0)
        << "OFF must not even enter the per-station screen";
    EXPECT_EQ(ctx.counters.pc.fuse_rows, 0);

    // The trip latch is equally inert: an unarmed recorder never records an era,
    // so a later armed run cannot inherit a stale one.
    EXPECT_FALSE(networkRhsTripEdge(ctx, "sh_gate", true));
    EXPECT_EQ(ctx.counters.pc.rec_consults, 0);
    EXPECT_TRUE(ctx.carry.rhs_trip_pending.empty());
}

// ===========================================================================
//  7. Event rate limiting is per (event, station, satellite, class)
// ===========================================================================
//
// A parked cell keeps logging its identity for as long as it persists (its duration
// is part of the evidence), without writing 120 rows an hour at 30 s.  The holdoff
// counts processed epochs, so the test drives ctx.counters.n_loop_epochs the way the
// driver does.
TEST(PrefitCensusObservability, EventsAreRateLimitedPerCellAndResumeAfterTheHoldoff) {
    LogManager log;
    auto cfg = loadCfg(log, "holdoff",
                       "    \"prefit_fuse_sigma\": 1000.0,\n"
                       "    \"prefit_event_holdoff_ep\": 5,\n"
                       "    \"trace_events\": true,\n");
    NetworkRunContext ctx(*cfg, cfg->getNetworkConfig());
    ASSERT_EQ(ctx.knobs.pc_event_holdoff, 5);

    const fs::path dir = scratchDir("holdoff");
    const fs::path csv = dir / "network_events.csv";
    NetworkRunTraces traces(dir);
    traces.events_trace.open(csv, std::ios::trunc);
    ASSERT_TRUE(traces.events_trace.is_open());

    for (int ep = 0; ep < 12; ++ep) {
        ctx.counters.n_loop_epochs = static_cast<std::size_t>(ep);
        std::vector<RowSpec> rows = codeRows(6, 0.0, 4.0);
        rows.push_back({ kCb, 1.0e5, 4.0 });     // the same parked cell, every epoch
        StationAssembly R = makeStation(rows);
        networkPrefitFuseStation(ctx, traces, epochAt(1000.0 + 30.0 * ep).time, kCols,
                                 "BSHM00ISR", R, 1000.0);
    }
    traces.events_trace.flush();

    // Every epoch is capped (the fuse is a weight action and is not rate limited)…
    EXPECT_EQ(ctx.counters.pc.fuse_capped, 12);
    // …but only epochs 0, 5 and 10 write an event.
    EXPECT_EQ(ctx.counters.pc.fuse_events, 3);
    EXPECT_EQ(ctx.counters.pc.fuse_events_held, 9);
    EXPECT_EQ(countEvents(csv, "prefit_fused"), 3);
}

// ===========================================================================
//  8. The census walk: exceedance events name the cell, births are births
// ===========================================================================
//
// INC-1 and INC-3 share the same walk.  The birth test is the stricter one: a LOS
// present -> absent -> present is a birth (its per-LOS states were re-created),
// while a LOS present in consecutive epochs is not, and the first walked epoch seeds
// the map rather than declaring the whole network newborn.
TEST(PrefitCensusObservability, CensusNamesTheCellAndBirthNeedsAPriorAbsence) {
    LogManager log;
    auto cfg = loadCfg(log, "walk",
                       "    \"trace_prefit_census\": true,\n"
                       "    \"prefit_event_sigma\": 1000.0,\n"
                       "    \"prefit_event_holdoff_ep\": 0,\n"
                       "    \"seed_birth_check_sigma\": 100.0,\n"
                       "    \"trace_events\": true,\n");
    NetworkRunContext ctx(*cfg, cfg->getNetworkConfig());
    ASSERT_TRUE(ctx.knobs.pc_walk_on);
    ASSERT_TRUE(ctx.knobs.pc_census_on);
    ASSERT_DOUBLE_EQ(ctx.knobs.pc_birth_sigma, 100.0);

    const fs::path dir = scratchDir("walk");
    const fs::path csv = dir / "network_events.csv";
    NetworkRunTraces traces(dir);
    traces.events_trace.open(csv, std::ios::trunc);

    NetworkEstimator est;
    registerColumns(est);
    ASSERT_EQ(est.numShared(), kCols);

    std::vector<NetStation> stations(1);
    stations[0].name = "BSHM00ISR";
    const std::vector<int> est_index_of{ 0 };

    // The parked cell: six clean code rows plus one displaced by 1e5 m.
    auto parkedEpoch = [] {
        std::vector<RowSpec> rows = codeRows(6, 0.0, 4.0);
        rows.push_back({ kCb, 1.0e5, 4.0 });
        return std::vector<StationAssembly>{ makeStation(rows) };
    };
    // The same station with no satellite-labelled rows at all (the LOS vanishes).
    auto absentEpoch = [] {
        std::vector<RowSpec> rows(6, RowSpec{ kSh, 0.0, 1.0 });
        return std::vector<StationAssembly>{ makeStation(rows) };
    };

    auto walk = [&](std::vector<StationAssembly> asm_res, double tow, int ep) {
        ctx.counters.n_loop_epochs = static_cast<std::size_t>(ep);
        networkPrefitCensus(ctx, *cfg, nullLogger(), traces, epochAt(tow), stations,
                            est, asm_res, est_index_of);
    };

    walk(parkedEpoch(), 1000.0, 0);   // seeds the presence map
    EXPECT_EQ(ctx.counters.pc.births, 0)
        << "the first walked epoch has no prior map — a first sighting is not a birth";
    EXPECT_EQ(ctx.counters.pc.exceedances, 1);
    EXPECT_EQ(ctx.counters.pc.events, 1);
    EXPECT_EQ(ctx.counters.pc.birth_events, 0);

    walk(parkedEpoch(), 1030.0, 1);   // continuously present: still not a birth
    EXPECT_EQ(ctx.counters.pc.births, 0);
    EXPECT_EQ(ctx.counters.pc.exceedances, 2);

    // The epoch scalars the status row picks up, on an established-cell epoch:
    // the code class carries the whole signal, the phase class is empty, and the
    // run maximum names the cell.
    const int cd = static_cast<int>(NetRowClass::ObsCode);
    EXPECT_GT(ctx.carry.ep_pc_max_s[cd], 1.0e5);
    EXPECT_EQ(ctx.carry.ep_pc_n_gt_1000[cd], 1);
    EXPECT_DOUBLE_EQ(ctx.carry.ep_pc_max_s[static_cast<int>(NetRowClass::ObsPhase)], 0.0);
    EXPECT_NE(ctx.counters.pc.max_s_id.find("BSHM00ISR"), std::string::npos);
    EXPECT_NE(ctx.counters.pc.max_s_id.find("G07"), std::string::npos);

    walk(absentEpoch(), 1060.0, 2);   // the LOS vanishes
    walk(parkedEpoch(), 1090.0, 3);   // …and returns: this is a birth
    EXPECT_EQ(ctx.counters.pc.births, 1);
    EXPECT_EQ(ctx.counters.pc.birth_events, 1)
        << "a born cell at s = 2e5 must trip the tighter seed threshold";

    // Births are not exceedances.  The birth epoch adds no INC-1 exceedance: a fresh
    // arc is seeded to ~0 innovation against whatever level its station carries, and
    // counting it gave 666 false events an hour on the FT63 leg.  The row is handed
    // to INC-3 and the hand-off is counted.
    EXPECT_EQ(ctx.counters.pc.exceedances, 2)
        << "the birth epoch must not raise an INC-1 exceedance";
    EXPECT_GT(ctx.counters.pc.rows_born_deferred, 0);
    EXPECT_EQ(ctx.counters.pc.cells_born_deferred, 1);

    // …and on the birth epoch the same columns read zero, because every row of the
    // only cell present was deferred.  The columns describe established cells, so
    // "max_s = 0" here means "no established cell exceeded", not "nothing happened".
    EXPECT_DOUBLE_EQ(ctx.carry.ep_pc_max_s[cd], 0.0);
    EXPECT_EQ(ctx.carry.ep_pc_n_gt_1000[cd], 0);

    traces.events_trace.flush();
    EXPECT_EQ(countEvents(csv, "prefit_exceedance"), 2);
    EXPECT_EQ(countEvents(csv, "seed_birth_exceedance"), 1);
}

// ===========================================================================
//  8b. The event domain keeps phase out of the event channel by default
// ===========================================================================
//
// Measured on the FT63 leg-19 replay at threshold 1e3: obs_code raised 6 events,
// all from the era-2 injector (BSHM00ISR G07/G16); iono_sh_row raised 51, with one
// 40x outlier also at BSHM; obs_phase raised 608 across 55 stations, none of them
// real faults.  A phase row's prefit innovation is arc-specific (it carries that
// arc's ambiguity linearization point), so a station's phase rows are not a
// common-mode population and their median is not a centre.  The columns still
// report phase; only the event channel is narrowed, and the suppression is counted.
TEST(PrefitCensusObservability, EventDomainExcludesPhaseButColumnsStillReportIt) {
    LogManager log;
    auto cfg = loadCfg(log, "evdomain",
                       "    \"trace_prefit_census\": true,\n"
                       "    \"prefit_event_holdoff_ep\": 0,\n"
                       "    \"trace_events\": true,\n");
    EXPECT_EQ(cfg->getNetworkConfig().prefit_event_classes, "code,sh,other");
    NetworkRunContext ctx(*cfg, cfg->getNetworkConfig());
    EXPECT_EQ(ctx.knobs.pc_event_mask,
              dataClassBit(NetRowClass::ObsCode) | dataClassBit(NetRowClass::ObsOther)
                  | dataClassBit(NetRowClass::IonoShRow));

    const fs::path dir = scratchDir("evdomain");
    const fs::path csv = dir / "network_events.csv";
    NetworkRunTraces traces(dir);
    traces.events_trace.open(csv, std::ios::trunc);

    NetworkEstimator est;
    registerColumns(est);
    std::vector<NetStation> stations(1);
    stations[0].name = "CAS100ATA";
    const std::vector<int> est_index_of{ 0 };

    // One parked code cell and one extreme phase cell, both far over threshold.
    std::vector<RowSpec> rows = codeRows(6, 0.0, 4.0);
    rows.push_back({ kCb, 1.0e5, 4.0 });                         // code: raises an event
    for (int i = 0; i < 5; ++i) rows.push_back({ kPb, 177037.0, 8000.0 });
    rows.push_back({ kPb, -3317.0, 8000.0 });                    // phase: columns only
    std::vector<StationAssembly> a{ makeStation(rows) };
    ctx.counters.n_loop_epochs = 0;
    networkPrefitCensus(ctx, *cfg, nullLogger(), traces, epochAt(1000.0), stations,
                        est, a, est_index_of);
    traces.events_trace.flush();

    EXPECT_EQ(ctx.counters.pc.exceedances, 2) << "both cells exceed the threshold";
    EXPECT_EQ(ctx.counters.pc.events, 1) << "only the code cell may speak";
    EXPECT_EQ(ctx.counters.pc.events_class_skipped, 1)
        << "the phase suppression must be counted, not silent";
    EXPECT_EQ(countEvents(csv, "prefit_exceedance"), 1);

    // …and the phase column still carries the number, so nothing is hidden.
    const int ph = static_cast<int>(NetRowClass::ObsPhase);
    EXPECT_GT(ctx.carry.ep_pc_max_s[ph], 1.0e6);
    EXPECT_EQ(ctx.carry.ep_pc_n_gt_1000[ph], 1);
}

// ===========================================================================
//  9. The topology ledger: a tie-row count change is an event
// ===========================================================================
//
// The era-2 fault was one such change ("BSHM +1 tie row, gate weight 0 -> 0.25"),
// and without the ledger the run recorded it only in an aggregate counter.  The
// ledger seeds silently: a first sighting is not a topology change, otherwise every
// cold start would emit one event per station per column.
TEST(PrefitCensusObservability, TopologyLedgerSeedsSilentlyThenEventsTieRowDeltas) {
    LogManager log;
    auto cfg = loadCfg(log, "topo",
                       "    \"trace_topology\": true,\n"
                       "    \"trace_events\": true,\n");
    NetworkRunContext ctx(*cfg, cfg->getNetworkConfig());
    ASSERT_TRUE(ctx.knobs.pc_topo_on);

    const fs::path dir = scratchDir("topo");
    const fs::path csv = dir / "network_events.csv";
    NetworkRunTraces traces(dir);
    traces.events_trace.open(csv, std::ios::trunc);

    NetworkEstimator est;
    registerColumns(est);
    std::vector<NetStation> stations(1);
    stations[0].name = "BSHM00ISR";
    const std::vector<int> est_index_of{ 0 };

    auto epochWithTies = [](int n_tie) {
        std::vector<RowSpec> rows = codeRows(6, 0.0, 4.0);
        for (int k = 0; k < n_tie; ++k) rows.push_back({ kSh, 0.1, 1.0 });
        return std::vector<StationAssembly>{ makeStation(rows) };
    };
    auto walk = [&](std::vector<StationAssembly> a, double tow, int ep) {
        ctx.counters.n_loop_epochs = static_cast<std::size_t>(ep);
        networkPrefitCensus(ctx, *cfg, nullLogger(), traces, epochAt(tow), stations,
                            est, a, est_index_of);
    };

    walk(epochWithTies(2), 1000.0, 0);
    EXPECT_EQ(ctx.counters.pc.topo_tie_events, 0) << "seeding epoch must be silent";
    EXPECT_EQ(ctx.counters.pc.topo_col_add, 0);

    walk(epochWithTies(2), 1030.0, 1);
    EXPECT_EQ(ctx.counters.pc.topo_tie_events, 0) << "no change, no event";

    walk(epochWithTies(3), 1060.0, 2);            // a tie row re-folds
    EXPECT_EQ(ctx.counters.pc.topo_tie_events, 1);

    walk(epochWithTies(0), 1090.0, 3);            // …and cuts them all
    EXPECT_EQ(ctx.counters.pc.topo_tie_events, 2);

    // The re-coupling edge.  A station whose ties are all cut still holds a zero
    // entry in the ledger; otherwise its return would read as a first sighting and
    // the valve's other edge would go unrecorded, as the era-2 return did.
    // (Both edges, per invariant #7.)
    walk(epochWithTies(4), 1120.0, 4);
    EXPECT_EQ(ctx.counters.pc.topo_tie_events, 3)
        << "0 -> 4 is a RE-COUPLING event, not a first sighting";

    traces.events_trace.flush();
    EXPECT_EQ(countEvents(csv, "tie_rows"), 3);
    EXPECT_EQ(countEvents(csv, "row_kinds"), 0) << "the satellite never changed kind";
}

// ===========================================================================
//  10. The recorder latch is an era latch
// ===========================================================================
//
// The retired SH health gate held an era for 95+ minutes (measured on DOY-129); its
// "sh_gate" name stays below as a generic era literal.  A latch that fired per epoch
// would write ~190 attribution frame sets for one such event; a latch that never
// re-armed would miss the next era.  The latch trips once per era, and the era
// clears when the condition does.
TEST(PrefitCensusObservability, TripLatchFiresOncePerEraAndReArmsWhenItClears) {
    LogManager log;
    auto cfg = loadCfg(log, "trip", "    \"rhs_attrib_on_trip\": true,\n");
    NetworkRunContext ctx(*cfg, cfg->getNetworkConfig());
    ASSERT_TRUE(ctx.knobs.rec_on);

    EXPECT_TRUE(networkRhsTripEdge(ctx, "sh_gate", true));      // rising edge
    EXPECT_EQ(ctx.carry.rhs_trip_pending, "sh_gate");
    ctx.carry.rhs_trip_pending.clear();                         // the driver consumes it

    for (int k = 0; k < 190; ++k)                               // a 95-minute era
        EXPECT_FALSE(networkRhsTripEdge(ctx, "sh_gate", true)) << "epoch " << k;
    EXPECT_TRUE(ctx.carry.rhs_trip_pending.empty());

    EXPECT_FALSE(networkRhsTripEdge(ctx, "sh_gate", false));    // the era clears
    EXPECT_TRUE(networkRhsTripEdge(ctx, "sh_gate", true));      // and re-arms

    // Eras are independent: a live "sh_gate" era must not mask a sigma0 trip.
    EXPECT_TRUE(networkRhsTripEdge(ctx, "sigma0", true));

    EXPECT_EQ(ctx.counters.pc.rec_trips, 3);
    EXPECT_EQ(ctx.counters.pc.rec_consults, 194);
    EXPECT_GT(ctx.counters.pc.rec_consults, ctx.counters.pc.rec_trips)
        << "the consultation count is the invariant-#4 proof that the sites are wired";
}
