/**
 * @file test_network_mdb.cpp
 * @brief BAARDA RELIABILITY NUMBERS / MDB on the network path
 *        (docs/SPEC_network_mdb_reliability.md).
 *
 * MDB answers the service question the redundancy census only answers yes/no:
 * not "is this row checkable?" but "how large a fault could hide in it right
 * now?", in metres.  Everything about it fails SILENTLY if it is wrong — a
 * mis-derived δ₀ still produces plausible metres, a scaled column quietly
 * back-filled with the formal value still produces a number, and a feed site
 * that skips the uncheckable rows still produces a clean-looking distribution
 * that says the network cross-checks everything.  So the tests here pin the four
 * claims, not the arithmetic running:
 *
 *   1. THE CONVENTION IS BAARDA'S, not merely self-consistent.  δ₀ = k + Φ⁻¹(0.80)
 *      and MDB = σδ₀/√r are checked against an INDEPENDENT closed form across a
 *      (σ, r, k) grid, and test 4 checks the whole thing against its physical
 *      meaning by Monte Carlo.  Two files agreeing on a WRONG δ₀ would pass a
 *      file-to-file comparison and fail both of these.
 *   2. r → 0 gives +infinity, and the infinite population reconciles with the
 *      redundancy census's uncheckable population as a strict SUBSET — the two
 *      thresholds differ (1e-6 arithmetic vs network.redundancy_eps policy) and
 *      an equality assertion between them would be a bug, not a check.
 *   3. mdb_scaled = mdb_formal × σ̂₀,f exactly (a positive scale commutes with
 *      every order statistic), AND absent σ̂₀ is FLAGGED rather than back-filled
 *      — in particular the free-ambiguity phase pool's σ̂₀ = 0 must read absent,
 *      because scaling by it would publish "any fault is detectable" for exactly
 *      the rows that carry no redundancy at all.
 *   4. An injected bias of exactly MDBᵢ is detected with ~80 % power at the k
 *      threshold.  This is what pins δ₀ to its BAARDA MEANING rather than to the
 *      other file: it would fail for any other power convention.
 *   5. OFF is structurally inert — default config, no knobs, no accumulation —
 *      and an armed config WITHOUT a redundancy source is a HARD ERROR, because
 *      that configuration would otherwise report a clean zero-row run that reads
 *      exactly like "this network cross-checks everything".
 *
 * ⚠ A note on test 1 and the spec's wording.  §5.1 asks the network helper to
 * reproduce "the KalmanFilter path's MDB … one convention, two paths".  This
 * card implemented the port by SHARING the convention
 * (filter/BaardaReliability.hpp) rather than by copying it, so a file-to-file
 * comparison is now a tautology; constructing a KalmanFilter here would need a
 * ProductParser + RinexParser + PppModel fixture to compare a formula against
 * itself.  What is pinned instead is strictly stronger: the shared helper
 * against an independent oracle, the two frozen constants as literals, and the
 * δ₀ the PPP path actually forms from a stock config's
 * `dataSnoopingCritical()`.  The CONVENTION is what has to hold, and any edit
 * that moves it moves both paths and fails here.
 */

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <limits>
#include <random>
#include <string>
#include <vector>

#include <Eigen/Dense>

#include "app/NetworkRunContext.hpp"
#include "config/GnssConfig.hpp"
#include "filter/BaardaReliability.hpp"
#include "filter/NetworkMdb.hpp"
#include "filter/NetworkRowClassify.hpp"
#include "logging/LogManager.hpp"

namespace fs = std::filesystem;
using namespace GnssProcessor;

namespace {

/// Independent closed form of Baarda's MDB — deliberately NOT the production
/// helper, and deliberately spelled with the quantile as a literal so a change
/// to kBaardaPowerQuantile has to be made in two places to pass.
double oracleMdb(double sigma, double r, double k) {
    return sigma * (k + 0.8416) / std::sqrt(r);
}

// --- config-load fixtures (the house pattern, test_network_process_noise_config) --

fs::path writeTempConfig(const std::string& tag, const std::string& body) {
    const fs::path p = fs::temp_directory_path() / ("netmdb_cfg_" + tag + ".json");
    std::ofstream f(p);
    f << body;
    f.close();
    return p;
}

/// A minimal but loadable PPP-RTK network config; @p extra is spliced into the
/// network block.
std::string networkConfig(const std::string& extra = "") {
    return std::string(
        "{\n"
        "  \"processing\": {\n"
        "    \"mode\": \"ppp-rtk-network\",\n"
        "    \"active_systems\": [ \"GPS\", \"Galileo\" ],\n"
        "    \"signal_priorities\": { \"GPS\": \"WC\", \"Galileo\": \"CB\" }\n"
        "  },\n"
        "  \"network\": {\n"
        "    \"clock_datum_station\": \"AAAA\",\n"
        + extra +
        "    \"stations\": [\n"
        "      { \"name\": \"AAAA\", \"position_ecef\": [ -2493304.99, -4655214.79, 3565497.67 ] },\n"
        "      { \"name\": \"BBBB\", \"position_ecef\": [ -2281621.75, -1453585.08, 5756964.89 ] }\n"
        "    ]\n"
        "  }\n"
        "}\n");
}

bool loadConfig(const std::string& tag, const std::string& body, GnssConfig& cfg) {
    LogManager log;
    const fs::path p = writeTempConfig(tag, body);
    const bool ok = cfg.loadFromJson(p, log);
    std::error_code ec; fs::remove(p, ec);
    return ok;
}

/// Load @p body expecting the loader to REJECT it (the hard-error path surfaces
/// as loadFromJson == false: GnssConfig::loadFromJson catches std::exception).
bool loadFails(const std::string& tag, const std::string& body) {
    GnssConfig cfg;
    return !loadConfig(tag, body, cfg);
}

}  // namespace

// ===========================================================================
//  1. The convention
// ===========================================================================

// The two frozen constants, as LITERALS.  They are the whole reason a PPP MDB
// and a network MDB are commensurable, and nothing else in the tree would fail
// if someone "tidied" 0.8416 into 0.84.
TEST(NetworkMdbReliability, ConventionConstantsAreFrozen) {
    EXPECT_DOUBLE_EQ(kBaardaPowerQuantile, 0.8416);      // Phi^-1(0.80), beta = 0.20
    EXPECT_DOUBLE_EQ(kBaardaRedundancyFloor, 1e-6);      // the arithmetic r-floor
    // delta_0 at the project's default data-snooping critical value.
    EXPECT_NEAR(baardaDelta0(3.29), 4.1316, 1e-12);
}

// The helper against an INDEPENDENT closed form over a grid.  Agreement with
// another FILE would not establish this: two copies of a wrong delta_0 agree
// perfectly.
TEST(NetworkMdbReliability, MdbMatchesTheBaardaClosedFormAcrossAGrid) {
    const double sigmas[] = {0.003, 0.3, 1.0, 12.7};
    const double reds[]   = {1e-4, 0.001, 0.05, 0.37, 0.5, 1.0};
    const double ks[]     = {2.0, 3.29, 5.0};
    for (double s : sigmas)
        for (double r : reds)
            for (double k : ks) {
                const double got = baardaMinimalDetectableBias(s, r, baardaDelta0(k));
                const double want = oracleMdb(s, r, k);
                EXPECT_NEAR(got, want, 1e-12 * std::max(1.0, std::abs(want)))
                    << "sigma=" << s << " r=" << r << " k=" << k;
            }
}

// The PPP path forms delta_0 as baardaDelta0(config.dataSnoopingCritical()); the
// network path does the same.  Pin the STOCK value that makes them equal, so a
// drift in the config default (which would silently re-scale every MDB in both
// paths at once) fails here rather than in a graded run.
TEST(NetworkMdbReliability, StockConfigGivesTheDocumentedDelta0) {
    GnssConfig cfg;
    EXPECT_DOUBLE_EQ(cfg.dataSnoopingCritical(), 3.29);
    EXPECT_NEAR(baardaDelta0(cfg.dataSnoopingCritical()), 4.1316, 1e-12);
    // ... and the number a code row of sigma = 0.3 m at r = 0.25 therefore gets.
    EXPECT_NEAR(baardaMinimalDetectableBias(0.3, 0.25,
                                            baardaDelta0(cfg.dataSnoopingCritical())),
                oracleMdb(0.3, 0.25, 3.29), 1e-12);
}

// sigma comes from the assembled information weight on the network path.  A
// non-usable weight must produce a NON-FINITE sigma so the caller can COUNT the
// row rather than bin a fabricated MDB.
TEST(NetworkMdbReliability, SigmaFromWeightRejectsUnusableWeights) {
    EXPECT_DOUBLE_EQ(baardaSigmaFromWeight(4.0), 0.5);
    EXPECT_DOUBLE_EQ(baardaSigmaFromWeight(1.0), 1.0);
    EXPECT_FALSE(std::isfinite(baardaSigmaFromWeight(0.0)));
    EXPECT_FALSE(std::isfinite(baardaSigmaFromWeight(-1.0)));
    EXPECT_FALSE(std::isfinite(baardaSigmaFromWeight(
        std::numeric_limits<double>::quiet_NaN())));
}

// ===========================================================================
//  2. r -> 0 and the uncheckable join
// ===========================================================================

// The r -> 0 convention is +infinity, NOT a large finite number and NOT a clamp.
// A finite value there would be a lie a consumer could act on: no residual-based
// test can see that row's misfit at any magnitude.
TEST(NetworkMdbReliability, UncheckableRowIsInfiniteNotMerelyLarge) {
    const double d0 = baardaDelta0(3.29);
    EXPECT_TRUE(std::isinf(baardaMinimalDetectableBias(0.3, 0.0, d0)));
    EXPECT_TRUE(std::isinf(baardaMinimalDetectableBias(0.3, kBaardaRedundancyFloor, d0)));
    EXPECT_TRUE(std::isinf(baardaMinimalDetectableBias(0.3, -0.5, d0)));
    // Just ABOVE the floor it is finite — and enormous, which is the honest
    // statement about a nearly-uncheckable row.
    const double just_above = baardaMinimalDetectableBias(0.3, 2e-6, d0);
    EXPECT_TRUE(std::isfinite(just_above));
    EXPECT_GT(just_above, 800.0);
}

// THE JOIN.  Two thresholds, one domain.  The seam's independent recount of
// "DATA rows" and "rows below network.redundancy_eps" must match what the
// redundancy census counts over the same rows, and the infinite population must
// be a strict SUBSET of the uncheckable one (1e-6 vs 0.05 — four decades apart).
TEST(NetworkMdbReliability, InfiniteRowsAreASubsetOfTheCensusUncheckablePopulation) {
    constexpr double kEps = 0.05;                 // network.redundancy_eps default
    NetworkMdbEpoch acc;
    const std::string station = "AAAA";
    // r values spanning: exactly-zero, below the arithmetic floor, between the
    // two floors, and healthy.
    const double rs[] = {0.0, 1e-9, 1e-7, 1e-3, 0.02, 0.05, 0.4, 0.9};
    long long want_unchk = 0, want_inf = 0;
    for (double r : rs) {
        if (r < kEps) ++want_unchk;
        if (!(r > kBaardaRedundancyFloor)) ++want_inf;
    }

    {
        NetworkMdbStationFeed feed(acc, 100.0, baardaDelta0(3.29), /*event_m=*/1.0,
                                   station, NetMdbSource::Census, kEps);
        for (double r : rs) feed.add("G07", NetRowClass::ObsCode, /*w=*/11.1, r);
        // Non-DATA rows are NOT part of either instrument's domain: a pivot pin
        // runs a standardized deviation of ~1e7 by design and its MDB is not a
        // statement about data.  Feeding one must move NOTHING.
        feed.add("", NetRowClass::PivotPin, 1e18, 0.9);
        feed.add("", NetRowClass::CplRow, 1.0, 0.5);
        feed.finish();
    }

    EXPECT_EQ(acc.census_rows, static_cast<long long>(std::size(rs)));
    EXPECT_EQ(acc.census_unchk, want_unchk);

    long long got_inf = 0, got_rows = 0;
    for (int s = 0; s < kNetMdbConstellations; ++s)
        for (int c = 0; c < kNetDataClasses; ++c) {
            got_inf  += acc.bin[s][c].n_inf;
            got_rows += acc.bin[s][c].n_rows;
        }
    EXPECT_EQ(got_rows, static_cast<long long>(std::size(rs)));
    EXPECT_EQ(got_inf, want_inf);
    // The SUBSET relation the spec's "reconcile" actually means.  Equality here
    // would be wrong, and asserting it would have hard-coded a bug.
    EXPECT_LE(got_inf, acc.census_unchk);
    EXPECT_LT(got_inf, acc.census_unchk) << "the fixture must exercise the gap "
                                            "between the two thresholds";
    // GPS rows land in the GPS bin, code rows in the code class.
    EXPECT_EQ(acc.bin[netMdbConstIndex('G')][static_cast<int>(NetRowClass::ObsCode)].n_rows,
              static_cast<long long>(std::size(rs)));
}

// A row whose weight is unusable is COUNTED as skipped, never binned with a
// fabricated sigma — but it still joins the census recount, because the census
// counts it too and a join is only a check if both walk one domain.
TEST(NetworkMdbReliability, UnusableWeightIsCountedNotFabricated) {
    NetworkMdbEpoch acc;
    const std::string station = "BBBB";
    {
        NetworkMdbStationFeed feed(acc, 200.0, baardaDelta0(3.29), 1.0, station,
                                   NetMdbSource::Census, 0.05);
        feed.add("E11", NetRowClass::ObsPhase, /*w=*/0.0, 0.5);
        feed.add("E11", NetRowClass::ObsPhase, /*w=*/-3.0, 0.5);
        feed.add("E11", NetRowClass::ObsPhase, /*w=*/4.0, 0.5);   // the only good one
        feed.finish();
    }
    EXPECT_EQ(acc.skipped_weight, 2);
    EXPECT_EQ(acc.census_rows, 3);            // the join domain is all three
    const NetworkMdbBin& b =
        acc.bin[netMdbConstIndex('E')][static_cast<int>(NetRowClass::ObsPhase)];
    EXPECT_EQ(b.n_rows, 1);
    ASSERT_EQ(b.mdb.size(), 1u);
    EXPECT_NEAR(b.mdb[0], oracleMdb(0.5, 0.5, 3.29), 1e-12);
}

// The accumulator is STAMPED, not driver-cleared: a feed for a new epoch resets
// it.  That is what makes an epoch which fed rows and never reached the flush
// (a withhold, a failed solve) leak nothing into the next epoch's aggregates.
TEST(NetworkMdbReliability, ANewEpochStampResetsTheAccumulator) {
    NetworkMdbEpoch acc;
    const std::string station = "AAAA";
    { NetworkMdbStationFeed f(acc, 100.0, baardaDelta0(3.29), 1.0, station,
                              NetMdbSource::Census, 0.05);
      f.add("G01", NetRowClass::ObsCode, 11.1, 0.5); f.finish(); }
    EXPECT_EQ(acc.census_rows, 1);
    EXPECT_EQ(acc.cand.size(), 1u);
    EXPECT_TRUE(acc.fed_census);
    { NetworkMdbStationFeed f(acc, 130.0, baardaDelta0(3.29), 1.0, station,
                              NetMdbSource::Raim, 0.0);
      f.add("G01", NetRowClass::ObsPhase, 1e4, 0.5); f.finish(); }
    EXPECT_DOUBLE_EQ(acc.tow, 130.0);
    EXPECT_EQ(acc.census_rows, 0);            // the previous epoch's rows are gone
    EXPECT_EQ(acc.rows_census, 0);
    EXPECT_EQ(acc.rows_raim, 1);
    EXPECT_FALSE(acc.fed_census);
    EXPECT_TRUE(acc.fed_raim);
    EXPECT_EQ(acc.bin[netMdbConstIndex('G')][static_cast<int>(NetRowClass::ObsCode)].n_rows, 0);
}

// ===========================================================================
//  3. The scaled variant and the absent flag
// ===========================================================================

// Order statistics, and the property that makes flush-time scaling EXACT rather
// than approximate: a positive scale commutes with the median, the p95 and the
// maximum, so aggregating the formal pool and multiplying the three numbers is
// identical to scaling every row first.
TEST(NetworkMdbReliability, ScaledAggregatesEqualFormalAggregatesTimesSigma0) {
    std::vector<double> v{5.0, 1.0, 4.0, 2.0, 3.0, 9.0, 7.0, 6.0, 8.0, 10.0};
    std::vector<double> scaled;
    constexpr double kS0 = 17.5;
    for (double x : v) scaled.push_back(x * kS0);

    std::vector<double> a = v, b = scaled;
    const NetworkMdbStats sa = networkMdbStats(a);
    const NetworkMdbStats sb = networkMdbStats(b);
    EXPECT_NEAR(sb.med, sa.med * kS0, 1e-9);
    EXPECT_NEAR(sb.p95, sa.p95 * kS0, 1e-9);
    EXPECT_NEAR(sb.max, sa.max * kS0, 1e-9);
    // The conventions themselves: upper median, nearest-rank p95, true max.
    EXPECT_DOUBLE_EQ(sa.med, 6.0);            // n=10 -> index 5 of the sorted pool
    EXPECT_DOUBLE_EQ(sa.p95, 10.0);           // ceil(0.95*10)-1 = 9
    EXPECT_DOUBLE_EQ(sa.max, 10.0);
}

// A one-row pool reports that row rather than interpolating against nothing;
// an empty pool reports zeros, which the caller only ever writes alongside the
// population (an empty bin must never read as a bin of zeros).
TEST(NetworkMdbReliability, StatsDegradeHonestlyOnTinyPools) {
    std::vector<double> empty;
    const NetworkMdbStats se = networkMdbStats(empty);
    EXPECT_DOUBLE_EQ(se.med, 0.0);
    EXPECT_DOUBLE_EQ(se.p95, 0.0);
    EXPECT_DOUBLE_EQ(se.max, 0.0);
    std::vector<double> one{42.0};
    const NetworkMdbStats s1 = networkMdbStats(one);
    EXPECT_DOUBLE_EQ(s1.med, 42.0);
    EXPECT_DOUBLE_EQ(s1.p95, 42.0);
    EXPECT_DOUBLE_EQ(s1.max, 42.0);
}

// Honesty rule 1.  Absent sigma0 must be FLAGGED — never back-filled with the
// formal value and never with 1.0 — and the four reasons must stay
// distinguishable, because they demand different fixes.
TEST(NetworkMdbReliability, AbsentSigma0IsFlaggedWithItsReason) {
    const int code  = static_cast<int>(NetRowClass::ObsCode);
    const int phase = static_cast<int>(NetRowClass::ObsPhase);
    const int other = static_cast<int>(NetRowClass::ObsOther);
    const int shrow = static_cast<int>(NetRowClass::IonoShRow);

    // The P1-4 pass is not armed at all.
    NetworkMdbScale f = networkMdbFamilyScale(code, /*pf_rows=*/0, 0, 0, 0.0, 0.0);
    EXPECT_FALSE(f.have);
    EXPECT_STREQ(f.src, "absent_no_pf");

    // Armed, but this class has no pool there by construction.
    f = networkMdbFamilyScale(other, 100, 60, 40, 0.31, 0.0);
    EXPECT_FALSE(f.have);
    EXPECT_STREQ(f.src, "absent_no_pool_for_class");
    f = networkMdbFamilyScale(shrow, 100, 60, 40, 0.31, 0.0);
    EXPECT_FALSE(f.have);
    EXPECT_STREQ(f.src, "absent_no_pool_for_class");

    // Armed, class has a pool, but it saw no rows this epoch.
    f = networkMdbFamilyScale(code, 100, /*pf_code_rows=*/0, 100, 0.0, 2.0);
    EXPECT_FALSE(f.have);
    EXPECT_STREQ(f.src, "absent_pool_empty");

    // Armed, rows present, sigma0 == 0 — THE free-ambiguity phase case.  This is
    // the one that matters: back-filling it (or treating 0 as a scale) would
    // publish every phase row's MDB as 0 m, i.e. "a fault of any size is
    // detectable here", for exactly the rows that carry no redundancy at all.
    f = networkMdbFamilyScale(phase, 100, 60, /*pf_phase_rows=*/40,
                              0.31, /*s0_phase=*/0.0);
    EXPECT_FALSE(f.have);
    EXPECT_STREQ(f.src, "absent_sigma0_zero");
    EXPECT_DOUBLE_EQ(f.sigma0, 1.0) << "the inert value must never reach the wire";

    // And the two paths that DO produce an INFLATING scale (sigma0 >= 1).
    f = networkMdbFamilyScale(code, 100, 60, 40, /*s0_code=*/3.29, 0.0);
    EXPECT_TRUE(f.have);
    EXPECT_STREQ(f.src, "postfit_code");
    EXPECT_DOUBLE_EQ(f.sigma0, 3.29);
    f = networkMdbFamilyScale(phase, 100, 60, 40, 3.29, /*s0_phase=*/12.5);
    EXPECT_TRUE(f.have);
    EXPECT_STREQ(f.src, "postfit_phase");
    EXPECT_DOUBLE_EQ(f.sigma0, 12.5);
}

// THE MIRROR-IMAGE TRAP.  sigma0 < 1 scales the MDB DOWN, which reads as a
// TIGHTER reliability guarantee — and on the phase family it is the exact
// opposite: that pool's post-fit dispersion is near zero precisely BECAUSE each
// phase row is fitted by its own free ambiguity, i.e. the same fact as "these
// rows carry no redundancy".  Measured on the FT63 cold smoke, sigma0_phase =
// 0.041 pulls the phase median MDB from 4.58 m to 0.19 m on rows that are 78 %
// uncheckable.  The value is REPORTED, never clamped (the prior-residual
// census's rule: the deflation is the measurement) — but it must be TAGGED, or
// a consumer keying on sigma0_src cannot tell it from a calibrated tightening.
TEST(NetworkMdbReliability, DeflatingSigma0IsReportedButTagged) {
    const int code  = static_cast<int>(NetRowClass::ObsCode);
    const int phase = static_cast<int>(NetRowClass::ObsPhase);

    NetworkMdbScale f = networkMdbFamilyScale(phase, 100, 60, 40, 3.29,
                                              /*s0_phase=*/0.041);
    EXPECT_TRUE(f.have) << "the deflation IS the measurement — do not clamp it away";
    EXPECT_DOUBLE_EQ(f.sigma0, 0.041);
    EXPECT_STREQ(f.src, "postfit_phase_deflating");

    f = networkMdbFamilyScale(code, 100, 60, 40, /*s0_code=*/0.5, 0.0);
    EXPECT_TRUE(f.have);
    EXPECT_STREQ(f.src, "postfit_code_deflating");

    // Exactly 1.0 is the boundary and is NOT a deflation.
    f = networkMdbFamilyScale(code, 100, 60, 40, /*s0_code=*/1.0, 0.0);
    EXPECT_TRUE(f.have);
    EXPECT_STREQ(f.src, "postfit_code");
}

// ===========================================================================
//  4. The physical meaning — Monte Carlo Baarda power
// ===========================================================================

/**
 * THE test that pins delta_0 to Baarda rather than to another file.
 *
 * On a 2-parameter weighted LS toy, inject a bias of exactly MDB_i into row i
 * and count how often the w-test flags it.  The theory this must reproduce:
 * with v = (I-H)y and R = sigma^2 I,
 *
 *   E[v_i] = r_i b,   Var(v_i) = sigma^2 r_i,   w_i = v_i / (sigma sqrt(r_i)),
 *
 * so E[w_i] = b sqrt(r_i)/sigma; substituting b = MDB_i = sigma delta_0/sqrt(r_i)
 * gives E[w_i] = delta_0 exactly, hence P(|w_i| > k) = Phi(delta_0 - k)
 * = Phi(0.8416) = 0.80.  A delta_0 built on any other power convention lands
 * outside the band below; so does a delta_0 that forgot the +Phi^-1 term
 * (power would collapse to ~0.5).
 */
TEST(NetworkMdbReliability, InjectedMdbBiasIsDetectedAtEightyPercentPower) {
    constexpr double kSigma = 0.25;      // metres
    constexpr double kK     = 3.29;      // the project's data-snooping critical value
    constexpr int    kTrials = 6000;

    // A 5x2 design with genuinely unequal leverage, so the tested row is neither
    // fully redundant nor uncheckable.
    Eigen::MatrixXd A(5, 2);
    A << 1.0,  0.0,
         1.0,  1.0,
         1.0,  2.0,
         1.0,  3.0,
         1.0, 10.0;
    const double w = 1.0 / (kSigma * kSigma);
    const Eigen::MatrixXd N   = w * (A.transpose() * A);
    const Eigen::MatrixXd cov = N.inverse();
    const Eigen::MatrixXd H   = w * A * cov * A.transpose();     // whitened hat matrix

    for (Eigen::Index i = 0; i < A.rows(); ++i) {
        const double r_i = 1.0 - H(i, i);
        ASSERT_GT(r_i, 0.05) << "row " << i << " is too high-leverage for this test";
        ASSERT_LT(r_i, 1.0);
        const double mdb = baardaMinimalDetectableBias(kSigma, r_i, baardaDelta0(kK));
        // Sanity: the injected fault is the one the closed form asks for.
        ASSERT_NEAR(mdb, oracleMdb(kSigma, r_i, kK), 1e-12);

        std::mt19937 rng(20260804u + static_cast<unsigned>(i));   // fixed seed
        std::normal_distribution<double> gauss(0.0, kSigma);
        int detected = 0;
        for (int t = 0; t < kTrials; ++t) {
            Eigen::VectorXd y(5);
            for (Eigen::Index j = 0; j < 5; ++j) y(j) = gauss(rng);   // x_true = 0
            y(i) += mdb;                                              // THE fault
            const Eigen::VectorXd xhat = cov * (w * (A.transpose() * y));
            const Eigen::VectorXd v = y - A * xhat;
            const double w_i = v(i) / (kSigma * std::sqrt(r_i));
            if (std::abs(w_i) > kK) ++detected;
        }
        const double power = static_cast<double>(detected) / kTrials;
        // Baarda's definition, with a band loose enough for 6000 trials
        // (SE ~ 0.005 at p = 0.8, so +/- 0.04 is ~8 sigma) but far too tight for
        // a different power convention: beta = 0.10 would land at 0.90, beta =
        // 0.50 at 0.50, and dropping the +Phi^-1 term entirely at ~0.50.
        EXPECT_NEAR(power, 0.80, 0.04)
            << "row " << i << " r=" << r_i << " mdb=" << mdb;
    }
}

// The same toy, read the other way: a fault SMALLER than the MDB is detected
// less than 80 % of the time, and a much larger one essentially always.  This is
// the "minimal" in minimal detectable bias — without it, a delta_0 that was
// merely large would also pass the test above.
TEST(NetworkMdbReliability, PowerIsMonotoneAndTheMdbIsTheThreshold) {
    constexpr double kSigma = 0.25, kK = 3.29;
    constexpr int kTrials = 4000;
    Eigen::MatrixXd A(5, 2);
    A << 1.0, 0.0, 1.0, 1.0, 1.0, 2.0, 1.0, 3.0, 1.0, 10.0;
    const double w = 1.0 / (kSigma * kSigma);
    const Eigen::MatrixXd cov = (w * (A.transpose() * A)).inverse();
    const Eigen::MatrixXd H   = w * A * cov * A.transpose();
    const Eigen::Index i = 1;
    const double r_i = 1.0 - H(i, i);
    const double mdb = baardaMinimalDetectableBias(kSigma, r_i, baardaDelta0(kK));

    auto powerAt = [&](double bias, unsigned seed) {
        std::mt19937 rng(seed);
        std::normal_distribution<double> gauss(0.0, kSigma);
        int hit = 0;
        for (int t = 0; t < kTrials; ++t) {
            Eigen::VectorXd y(5);
            for (Eigen::Index j = 0; j < 5; ++j) y(j) = gauss(rng);
            y(i) += bias;
            const Eigen::VectorXd v = y - A * (cov * (w * (A.transpose() * y)));
            if (std::abs(v(i) / (kSigma * std::sqrt(r_i))) > kK) ++hit;
        }
        return static_cast<double>(hit) / kTrials;
    };

    const double p_half = powerAt(0.5 * mdb, 11u);
    const double p_mdb  = powerAt(mdb,       12u);
    const double p_big  = powerAt(3.0 * mdb, 13u);
    EXPECT_LT(p_half, 0.55) << "half the MDB must be well under 80% power";
    EXPECT_NEAR(p_mdb, 0.80, 0.05);
    EXPECT_GT(p_big, 0.99);
    EXPECT_LT(p_half, p_mdb);
    EXPECT_LT(p_mdb, p_big);
}

// ===========================================================================
//  5. OFF inertness and the arming contract
// ===========================================================================

// Structural OFF.  A stock network config leaves every MDB path dead: the knob
// is false, both source flags are irrelevant, and the master predicate the two
// feed sites and the flush read is false — so an unarmed run never enters the
// translation unit and never allocates the accumulator.
TEST(NetworkMdbReliability, StockConfigLeavesEveryMdbPathInert) {
    GnssConfig cfg;
    ASSERT_TRUE(loadConfig("off", networkConfig(), cfg));
    const NetworkConfig& nc = cfg.getNetworkConfig();
    EXPECT_FALSE(nc.mdb);
    EXPECT_DOUBLE_EQ(nc.mdb_event_m, 1.0);
    EXPECT_EQ(nc.mdb_event_holdoff_ep, 20);

    const NetworkKnobs k = NetworkKnobs::snapshot(cfg, nc);
    EXPECT_FALSE(k.mdb_on);

    // And the accumulator itself is inert until something feeds it.
    NetworkMdbEpoch acc;
    EXPECT_DOUBLE_EQ(acc.tow, -1.0);
    EXPECT_FALSE(acc.fed_raim);
    EXPECT_FALSE(acc.fed_census);
    EXPECT_EQ(acc.census_rows, 0);
    EXPECT_TRUE(acc.cand.empty());
    for (int s = 0; s < kNetMdbConstellations; ++s)
        for (int c = 0; c < kNetDataClasses; ++c) {
            EXPECT_EQ(acc.bin[s][c].n_rows, 0);
            EXPECT_TRUE(acc.bin[s][c].mdb.empty());
        }
}

// THE arming contract.  network.mdb with no r_i source is a HARD ERROR, not a
// silent disarm: that configuration runs, bins zero rows and prints a clean
// summary indistinguishable from "this network cross-checks everything" — the
// live-counter-over-dead-wiring failure this project keeps paying for.
TEST(NetworkMdbReliability, ArmingWithoutARedundancySourceIsRejected) {
    EXPECT_TRUE(loadFails("nosrc", networkConfig("    \"mdb\": true,\n")))
        << "network.mdb with neither the census nor RAIM redundancy must not load";
    // raim_redundancy_normalize WITHOUT network.raim is equally dead — the
    // post-fit RAIM seam is not called at all, so no r_i is ever formed.
    EXPECT_TRUE(loadFails("raimoff", networkConfig(
        "    \"mdb\": true,\n"
        "    \"raim\": false,\n"
        "    \"raim_redundancy_normalize\": true,\n")));
}

// Armed via the redundancy census: loads, and the resolved knobs say WHICH feed
// site is live (the run-end invariant-#4 lines depend on that distinction).
TEST(NetworkMdbReliability, ArmedViaTheCensusResolvesTheCensusFeed) {
    GnssConfig cfg;
    ASSERT_TRUE(loadConfig("census", networkConfig(
        "    \"mdb\": true,\n"
        "    \"mdb_event_m\": 2.5,\n"
        "    \"mdb_event_holdoff_ep\": 7,\n"
        "    \"redundancy_census_stride\": 10,\n"), cfg));
    const NetworkConfig& nc = cfg.getNetworkConfig();
    EXPECT_TRUE(nc.mdb);
    EXPECT_DOUBLE_EQ(nc.mdb_event_m, 2.5);
    EXPECT_EQ(nc.mdb_event_holdoff_ep, 7);

    const NetworkKnobs k = NetworkKnobs::snapshot(cfg, nc);
    EXPECT_TRUE(k.mdb_on);
    EXPECT_TRUE(k.mdb_from_census);
    EXPECT_FALSE(k.mdb_from_raim);
    EXPECT_DOUBLE_EQ(k.mdb_event_m, 2.5);
    EXPECT_EQ(k.mdb_event_holdoff, 7);
}

// Armed via the RAIM pool: same, from the other side.
TEST(NetworkMdbReliability, ArmedViaRaimResolvesTheRaimFeed) {
    GnssConfig cfg;
    ASSERT_TRUE(loadConfig("raim", networkConfig(
        "    \"mdb\": true,\n"
        "    \"raim\": true,\n"
        "    \"raim_redundancy_normalize\": true,\n"), cfg));
    const NetworkConfig& nc = cfg.getNetworkConfig();
    const NetworkKnobs k = NetworkKnobs::snapshot(cfg, nc);
    EXPECT_TRUE(k.mdb_on);
    EXPECT_TRUE(k.mdb_from_raim);
    EXPECT_FALSE(k.mdb_from_census);
}

// A negative reporting threshold would event every finite row; the loader
// restores the default rather than accepting it.  (Zero is legal and means "no
// events" — the aggregates are the product, the events are a convenience.)
TEST(NetworkMdbReliability, NegativeReportingThresholdIsClamped) {
    GnssConfig cfg;
    ASSERT_TRUE(loadConfig("negthr", networkConfig(
        "    \"mdb\": true,\n"
        "    \"mdb_event_m\": -4.0,\n"
        "    \"redundancy_census_stride\": 10,\n"), cfg));
    EXPECT_DOUBLE_EQ(cfg.getNetworkConfig().mdb_event_m, 1.0);

    // Zero is LEGAL and means "aggregates only, no events" — the trace is the
    // product; the event stream is a convenience.  It must survive the loader.
    GnssConfig zcfg;
    ASSERT_TRUE(loadConfig("zerothr", networkConfig(
        "    \"mdb\": true,\n"
        "    \"mdb_event_m\": 0.0,\n"
        "    \"redundancy_census_stride\": 10,\n"), zcfg));
    EXPECT_DOUBLE_EQ(zcfg.getNetworkConfig().mdb_event_m, 0.0);
    EXPECT_DOUBLE_EQ(NetworkKnobs::snapshot(zcfg, zcfg.getNetworkConfig()).mdb_event_m, 0.0);
}

// The event candidates are the per (station x class) WORST finite row — the
// bounded reporting stream.  An uncheckable row can never BE a candidate (its
// MDB is not a finite metre count), which is what keeps the event channel about
// "how big a fault could hide here" rather than "here is another infinity".
TEST(NetworkMdbReliability, CandidatesAreThePerClassWorstFiniteRow) {
    NetworkMdbEpoch acc;
    const std::string station = "CCCC";
    {
        NetworkMdbStationFeed feed(acc, 300.0, baardaDelta0(3.29), 1.0, station,
                                   NetMdbSource::Raim, 0.0);
        feed.add("G01", NetRowClass::ObsPhase, 1e4, 0.90);   // small MDB
        feed.add("G02", NetRowClass::ObsPhase, 1e4, 0.02);   // the worst finite one
        feed.add("G03", NetRowClass::ObsPhase, 1e4, 0.0);    // infinite: not a candidate
        feed.add("G04", NetRowClass::ObsCode,  11.1, 0.30);  // a second class
        feed.finish();
    }
    ASSERT_EQ(acc.cand.size(), 2u);
    const NetworkMdbCandidate* phase = nullptr;
    const NetworkMdbCandidate* code = nullptr;
    for (const auto& c : acc.cand) {
        if (c.cls == static_cast<int>(NetRowClass::ObsPhase)) phase = &c;
        if (c.cls == static_cast<int>(NetRowClass::ObsCode))  code  = &c;
    }
    ASSERT_NE(phase, nullptr);
    ASSERT_NE(code, nullptr);
    EXPECT_EQ(phase->sat, "G02");
    EXPECT_EQ(phase->station, station);
    EXPECT_NEAR(phase->mdb, oracleMdb(0.01, 0.02, 3.29), 1e-12);
    EXPECT_EQ(code->sat, "G04");
    // And the bin-level worst identity agrees with the candidate.
    const NetworkMdbBin& b =
        acc.bin[netMdbConstIndex('G')][static_cast<int>(NetRowClass::ObsPhase)];
    EXPECT_EQ(b.worst_sat, "G02");
    EXPECT_EQ(b.n_inf, 1);
    EXPECT_EQ(b.n_rows, 3);
    EXPECT_EQ(b.mdb.size(), 2u);
}

// Constellation binning: known letters get their own bin, an unknown letter and
// an unlabelled row share the catch-all.  A dropped row would silently break
// the census join, so nothing is ever dropped for want of a name.
TEST(NetworkMdbReliability, UnknownConstellationsAreBinnedNotDropped) {
    EXPECT_EQ(netMdbConstIndex('G'), 0);
    EXPECT_EQ(netMdbConstIndex('R'), 1);
    EXPECT_EQ(netMdbConstIndex('E'), 2);
    EXPECT_EQ(netMdbConstIndex('C'), 3);
    EXPECT_EQ(netMdbConstIndex('X'), kNetMdbConstellations - 1);
    EXPECT_EQ(netMdbConstChar(0), 'G');
    EXPECT_EQ(netMdbConstChar(kNetMdbConstellations - 1), '?');

    NetworkMdbEpoch acc;
    const std::string station = "DDDD";
    {
        NetworkMdbStationFeed feed(acc, 400.0, baardaDelta0(3.29), 1.0, station,
                                   NetMdbSource::Census, 0.05);
        feed.add("X99", NetRowClass::ObsCode, 11.1, 0.5);
        feed.add("",    NetRowClass::IonoShRow, 11.1, 0.5);
        feed.finish();
    }
    EXPECT_EQ(acc.census_rows, 2);
    EXPECT_EQ(acc.bin[kNetMdbConstellations - 1][static_cast<int>(NetRowClass::ObsCode)].n_rows, 1);
    EXPECT_EQ(acc.bin[kNetMdbConstellations - 1][static_cast<int>(NetRowClass::IonoShRow)].n_rows, 1);
}
