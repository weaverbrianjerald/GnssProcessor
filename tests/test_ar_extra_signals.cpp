/**
 * @file test_ar_extra_signals.cpp
 * @brief Unit tests for the extra signals of a satellite pair in the WL-constrained resolver
 *        (@c estimation.ambiguity_resolution.extra_signals and @c primary_bands).
 *
 * Four layers.  The admission of one extra-wide lane (@ref admitExtraWideLane) and the split of a
 * pair's signals into primary and extra (@ref splitPrimaryAndExtra) are pure functions and are
 * tested without a filter.  The resolver is driven through resolveWlConstrained on a constructed
 * state and covariance: a satellite pair on three signals whose float ambiguities are built from
 * chosen integers, so the integer of the third signal is known by algebra
 * (@f$N_x = N_{lo} - E@f$).  The held-pair release is driven through the resolver's own
 * held-pair memory.  The two keys and their loader behaviour close the file.
 *
 * Suite names @c ArExtraSignalsAdmissionTest, @c ArExtraSignalsSplitTest,
 * @c ArExtraSignalsResolverTest and @c ArExtraSignalsConfigTest are owned solely by this file.
 */

#include <gtest/gtest.h>

#include "common/Constants.hpp"
#include "config/GnssConfig.hpp"
#include "filter/AmbiguityResolver.hpp"
#include "filter/AmbiguityResolverSetup.hpp"
#include "logging/LogManager.hpp"
#include "support/TestTempDir.hpp"

#include <spdlog/sinks/ostream_sink.h>
#include <spdlog/spdlog.h>

#include <cmath>
#include <filesystem>
#include <fstream>
#include <limits>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

namespace GnssProcessor {

/// Test seam for the private pieces of the resolver (friend in AmbiguityResolver.hpp).
struct ArExtraSignalsTestAccess {
    using Pair = AmbiguityResolver::DdPair;
    static AmbiguityResolution run(AmbiguityResolver& r, const std::vector<Pair>& pairs,
                                   const Eigen::VectorXd& x, const Eigen::MatrixXd& P,
                                   const std::vector<PppObservation>& obs) {
        return r.resolveWlConstrained(pairs, x, P, {}, obs);
    }
    /// What AmbiguityResolver::resolve does with a final fix: remember its held pairs.
    static void remember(AmbiguityResolver& r, const AmbiguityResolution& fix) {
        for (const auto& h : fix.held_pairs)
            r.held_pairs_[h.sat_i + "|" + h.sat_ref + "|" + h.sig_hi + "|" + h.sig_lo] = h;
    }
    static std::size_t heldCount(const AmbiguityResolver& r) { return r.held_pairs_.size(); }
};

} // namespace GnssProcessor

using namespace GnssProcessor;
namespace fs = std::filesystem;

namespace {

constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();

// ---------------------------------------------------------------------------
// The admission of one extra-wide lane
// ---------------------------------------------------------------------------

TEST(ArExtraSignalsAdmissionTest, AnExtraWideLaneNearAnIntegerIsAdmittedWithThatInteger) {
    const auto a = admitExtraWideLane(2.01, 0.01, 0.40);
    EXPECT_TRUE(a.admitted);
    EXPECT_EQ(a.integer, 2);
}

TEST(ArExtraSignalsAdmissionTest, ANegativeExtraWideLaneRoundsToTheNearestInteger) {
    const auto a = admitExtraWideLane(-3.04, 0.01, 0.40);
    EXPECT_TRUE(a.admitted);
    EXPECT_EQ(a.integer, -3);
}

TEST(ArExtraSignalsAdmissionTest, AFractionNearAHalfIsRefused) {
    EXPECT_FALSE(admitExtraWideLane(2.45, 0.001, 0.40).admitted);
}

TEST(ArExtraSignalsAdmissionTest, ALargeSigmaIsRefusedEvenAtAnInteger) {
    // 0 + 3 * 0.15 = 0.45 > 0.40.
    EXPECT_FALSE(admitExtraWideLane(2.0, 0.15, 0.40).admitted);
}

TEST(ArExtraSignalsAdmissionTest, TheLimitItselfIsAdmitted) {
    // 0.25 + 3 * 0.0625 = 0.4375 exactly.
    const auto a = admitExtraWideLane(2.25, 0.0625, 0.4375);
    EXPECT_TRUE(a.admitted);
    EXPECT_EQ(a.integer, 2);
    EXPECT_FALSE(admitExtraWideLane(2.25, 0.0625, 0.4374).admitted);
}

TEST(ArExtraSignalsAdmissionTest, ANonFiniteValueIsRefused) {
    EXPECT_FALSE(admitExtraWideLane(kNaN, 0.01, 0.40).admitted);
    EXPECT_FALSE(admitExtraWideLane(2.0, kNaN, 0.40).admitted);
    EXPECT_FALSE(admitExtraWideLane(std::numeric_limits<double>::infinity(), 0.01, 0.40).admitted);
}

TEST(ArExtraSignalsAdmissionTest, AnExtraWideLaneOfCloseFrequenciesNeedsNoAmplificationGuard) {
    // Galileo E5a-E5b is 9.8 m wide, but its extra-wide lane is a difference of cycle counts:
    // the same numbers are admitted whatever the two frequencies are.
    EXPECT_TRUE(admitExtraWideLane(-7.02, 0.01, 0.40).admitted);
}

// ---------------------------------------------------------------------------
// The split of a pair's signals
// ---------------------------------------------------------------------------

constexpr double kFL1 = 1575.42e6, kFL2 = 1227.60e6, kFL5 = 1176.45e6;
constexpr double kFE5b = 1207.14e6, kFE5 = 1191.795e6, kFE6 = 1278.75e6;

std::vector<PairSignal> gpsSignals() {
    return { { "L1C", kFL1 }, { "L2W", kFL2 }, { "L5Q", kFL5 } };
}

TEST(ArExtraSignalsSplitTest, GpsL1L2AreThePrimarySignalsAndL5IsExtra) {
    const auto s = splitPrimaryAndExtra("GPS", gpsSignals(), defaultArPrimaryBands());
    ASSERT_TRUE(s.usable);
    EXPECT_EQ(s.hi, 0);
    EXPECT_EQ(s.lo, 1);
    EXPECT_EQ(s.extras, (std::vector<int>{ 2 }));
}

TEST(ArExtraSignalsSplitTest, TheInputOrderDoesNotChangeWhichSignalsArePrimary) {
    const std::vector<PairSignal> sig = { { "L5Q", kFL5 }, { "L2W", kFL2 }, { "L1C", kFL1 } };
    const auto s = splitPrimaryAndExtra("GPS", sig, defaultArPrimaryBands());
    ASSERT_TRUE(s.usable);
    EXPECT_EQ(s.hi, 2);          // L1C, the higher frequency
    EXPECT_EQ(s.lo, 1);          // L2W
    EXPECT_EQ(s.extras, (std::vector<int>{ 0 }));
}

TEST(ArExtraSignalsSplitTest, GalileoE1E5aArePrimaryAndE5bE6AndAltBocAreExtra) {
    const std::vector<PairSignal> sig = { { "L1C", kFL1 }, { "L5Q", kFL5 }, { "L7Q", kFE5b },
                                          { "L8Q", kFE5 }, { "L6C", kFE6 } };
    const auto s = splitPrimaryAndExtra("Galileo", sig, defaultArPrimaryBands());
    ASSERT_TRUE(s.usable);
    EXPECT_EQ(s.hi, 0);
    EXPECT_EQ(s.lo, 1);
    EXPECT_EQ(s.extras, (std::vector<int>{ 2, 3, 4 }));
}

TEST(ArExtraSignalsSplitTest, ThePrimaryBandsOfTheTableDecideNotTheOrderOfTheDigits) {
    // Galileo told to use E5a and E1 in that order: the higher frequency is still hi.
    const std::map<std::string, std::string> bands = { { "Galileo", "51" } };
    const std::vector<PairSignal> sig = { { "L5Q", kFL5 }, { "L1C", kFL1 }, { "L7Q", kFE5b } };
    const auto s = splitPrimaryAndExtra("Galileo", sig, bands);
    ASSERT_TRUE(s.usable);
    EXPECT_EQ(s.hi, 1);
    EXPECT_EQ(s.lo, 0);
}

TEST(ArExtraSignalsSplitTest, AConstellationWithoutAnEntryIsProcessedOnlyWithExactlyTwoSignals) {
    const std::vector<PairSignal> two = { { "L1C", 1602.0e6 }, { "L2C", 1246.0e6 } };
    const auto s2 = splitPrimaryAndExtra("GLONASS", two, defaultArPrimaryBands());
    ASSERT_TRUE(s2.usable);
    EXPECT_EQ(s2.hi, 0);
    EXPECT_EQ(s2.lo, 1);
    EXPECT_TRUE(s2.extras.empty());

    const std::vector<PairSignal> three = { { "L1C", 1602.0e6 }, { "L2C", 1246.0e6 },
                                            { "L3Q", 1202.0e6 } };
    EXPECT_FALSE(splitPrimaryAndExtra("GLONASS", three, defaultArPrimaryBands()).usable);
    EXPECT_FALSE(splitPrimaryAndExtra("GLONASS", { { "L1C", 1602.0e6 } }, defaultArPrimaryBands()).usable);
}

TEST(ArExtraSignalsSplitTest, AMissingPrimarySignalSkipsThePair) {
    // GPS on L1 and L5 only: the L2 primary is missing, although the pair has two signals.
    EXPECT_FALSE(splitPrimaryAndExtra("GPS", { { "L1C", kFL1 }, { "L5Q", kFL5 } },
                                      defaultArPrimaryBands()).usable);
    EXPECT_FALSE(splitPrimaryAndExtra("GPS", { { "L2W", kFL2 }, { "L5Q", kFL5 } },
                                      defaultArPrimaryBands()).usable);
    EXPECT_FALSE(splitPrimaryAndExtra("GPS", { { "L1C", kFL1 } }, defaultArPrimaryBands()).usable);
    EXPECT_FALSE(splitPrimaryAndExtra("GPS", {}, defaultArPrimaryBands()).usable);
}

TEST(ArExtraSignalsSplitTest, AnEmptyTableIsTheRuleOfExactlyTwoSignals) {
    const std::map<std::string, std::string> none;
    EXPECT_FALSE(splitPrimaryAndExtra("GPS", gpsSignals(), none).usable);
    // Two signals, even ones off the default bands, are processed as before the key.
    const auto s = splitPrimaryAndExtra("GPS", { { "L1C", kFL1 }, { "L5Q", kFL5 } }, none);
    ASSERT_TRUE(s.usable);
    EXPECT_EQ(s.hi, 0);
    EXPECT_EQ(s.lo, 1);
}

TEST(ArExtraSignalsSplitTest, EqualFrequenciesPutTheSecondSignalHigh) {
    // The convention of the two-signal rule before the key: hi is the first only when strictly higher.
    const auto s = splitPrimaryAndExtra("X", { { "L1C", kFL1 }, { "L2W", kFL1 } }, {});
    ASSERT_TRUE(s.usable);
    EXPECT_EQ(s.hi, 1);
    EXPECT_EQ(s.lo, 0);
}

TEST(ArExtraSignalsSplitTest, ASecondSignalOnAPrimaryBandIsAnExtra) {
    const std::vector<PairSignal> sig = { { "L1C", kFL1 }, { "L2W", kFL2 }, { "L2L", kFL2 } };
    const auto s = splitPrimaryAndExtra("GPS", sig, defaultArPrimaryBands());
    ASSERT_TRUE(s.usable);
    EXPECT_EQ(s.lo, 1);
    EXPECT_EQ(s.extras, (std::vector<int>{ 2 }));
}

TEST(ArExtraSignalsSplitTest, AMalformedEntryMakesNothingPrimary) {
    const std::map<std::string, std::string> bad = { { "GPS", "1" } };
    EXPECT_FALSE(splitPrimaryAndExtra("GPS", gpsSignals(), bad).usable);
}

// ---------------------------------------------------------------------------
// Through resolveWlConstrained
// ---------------------------------------------------------------------------

namespace {

/// A signal of the scene: its name, frequency and role (0 higher primary, 1 lower primary, 2 extra).
struct Sig {
    const char* name;
    double      f;
    int         role;
    double lam() const { return Constants::SPEED_OF_LIGHT / f; }
};

const Sig kL1{ "L1C", kFL1, 0 };
const Sig kL2{ "L2W", kFL2, 1 };
const Sig kL5{ "L5Q", kFL5, 2 };

/// One satellite against the reference G12, and the float it realizes.
struct SatSpec {
    const char* sat;
    double n1;      ///< Float DD ambiguity on the higher primary signal [cycles].
    double w;       ///< Float wide lane n_1 - n_2 [cycles].
    double e;       ///< Float extra-wide lane n_2 - n_x of every extra signal [cycles].
};

/// The constructed state of a scene: reference states first (one per signal), then each
/// satellite's states (one per signal), all values in metres, the reference at zero.
struct Scene {
    std::vector<ArExtraSignalsTestAccess::Pair> pairs;
    Eigen::VectorXd x;
    Eigen::MatrixXd P;
    std::vector<PppObservation> obs;
    int n_sig{ 0 };
    /// State index of satellite number @p sat_no (0-based) on signal number @p sig_no.
    int stateOf(int sat_no, int sig_no) const { return n_sig * (1 + sat_no) + sig_no; }
};

Scene makeScene(const std::vector<SatSpec>& sats, const std::vector<Sig>& sigs, double tow = 123.0) {
    Scene s;
    s.n_sig = static_cast<int>(sigs.size());
    const int n = s.n_sig * (1 + static_cast<int>(sats.size()));
    s.x = Eigen::VectorXd::Zero(n);
    s.P = Eigen::MatrixXd::Identity(n, n) * 1e-8;     // 0.1 mm on every ambiguity
    for (int j = 0; j < static_cast<int>(sats.size()); ++j) {
        const SatSpec& sp = sats[j];
        const double n2 = sp.n1 - sp.w;
        for (int k = 0; k < s.n_sig; ++k) {
            const Sig& sg = sigs[k];
            const double cycles = sg.role == 0 ? sp.n1 : sg.role == 1 ? n2 : n2 - sp.e;
            s.x(s.stateOf(j, k)) = cycles * sg.lam();
            s.pairs.push_back({ s.stateOf(j, k), k, sp.sat, "G12", sg.name });
        }
        PppObservation o;
        o.satellite = sp.sat;
        o.system    = System::GPS;
        o.time.tow  = tow;
        s.obs.push_back(o);
    }
    return s;
}

AmbiguityResolverConfig extraConfig(bool extra_signals, double hold_release_cyc = 1.0) {
    AmbiguityResolverConfig c;
    c.ar_mode            = 3;
    c.wl_from_float      = true;
    c.nl_iono_free_row   = true;
    c.wl_max_frac        = 0.40;
    c.ratio_upper_bound  = 1e30;
    c.min_success_rate   = 0.0;
    c.hold_variance      = 1e-14;      // the pseudo-observation moves the state onto the integer
    c.hold_release_cyc   = hold_release_cyc;
    c.extra_signals      = extra_signals;
    return c;
}

/// A resolver that can be driven directly, with its log captured at debug level.
struct Fixture {
    LogManager        log;
    std::ostringstream text;
    std::unique_ptr<AmbiguityResolver> resolver;

    Fixture(const AmbiguityResolverConfig& cfg, const std::string& tag) {
        const fs::path dir = gnss_test::tempDir() / ("ar_extra_signals_" + tag);
        std::error_code ec;
        fs::remove_all(dir, ec);
        fs::create_directories(dir, ec);
        spdlog::drop("ambiguity_resolver");
        log.initialize(dir, LogLevel::Info);
        log.getLogger("ambiguity_resolver")->set_level(spdlog::level::debug);
        log.getLogger("ambiguity_resolver")->sinks().push_back(
            std::make_shared<spdlog::sinks::ostream_sink_mt>(text));
        resolver = std::make_unique<AmbiguityResolver>(cfg, log);
    }
    ~Fixture() { spdlog::drop("ambiguity_resolver"); }

    AmbiguityResolution run(const Scene& s) {
        auto r = ArExtraSignalsTestAccess::run(*resolver, s.pairs, s.x, s.P, s.obs);
        log.getLogger("ambiguity_resolver")->flush();
        return r;
    }
    std::string logText() { return text.str(); }
};

/// True when @p r holds a row on both states.
bool hasRow(const AmbiguityResolution& r, int i, int ref) {
    for (const auto& row : r.fixed_dd_rows)
        if (row.state_idx_i == i && row.state_idx_ref == ref) return true;
    return false;
}

/// Float of a pair with N_1 = 8, W = 3 (N_lo = 5) and E = 2 (N_x = 3), each 0.02 cycle off.
SatSpec goodSat(const char* sat = "G13") { return { sat, 8.02, 3.0, 2.0 }; }

} // namespace

TEST(ArExtraSignalsResolverTest, KeyOffSkipsAPairOnThreeSignalsAsBefore) {
    Fixture f(extraConfig(false), "off_skip");
    const Scene s = makeScene({ goodSat() }, { kL1, kL2, kL5 });
    const auto r = f.run(s);
    EXPECT_FALSE(r.success);
    EXPECT_TRUE(r.fixed_dd_rows.empty());
    EXPECT_TRUE(r.held_pairs.empty());
    EXPECT_EQ(f.logText().find("WLFLOAT"), std::string::npos) << f.logText();
    EXPECT_EQ(f.logText().find("EWLFLOAT"), std::string::npos);
    EXPECT_EQ(f.resolver->extraSignalCounters().evaluated, 0);
    EXPECT_FALSE(f.resolver->extraSignalsActive());
}

TEST(ArExtraSignalsResolverTest, KeyOnFixesThePrimaryPairAndGivesTheThirdSignalItsIntegerNxEqualNloMinusE) {
    Fixture f(extraConfig(true), "on_fix");
    const Scene s = makeScene({ goodSat() }, { kL1, kL2, kL5 });
    const auto r = f.run(s);
    ASSERT_TRUE(r.success);
    EXPECT_EQ(r.num_fixed, 1);                       // one searched pair; the extra rides on it

    // The rows the hold pins: both primary signals and the extra signal.
    ASSERT_EQ(r.fixed_dd_rows.size(), 3u);
    EXPECT_TRUE(hasRow(r, s.stateOf(0, 0), 0));
    EXPECT_TRUE(hasRow(r, s.stateOf(0, 1), 1));
    EXPECT_TRUE(hasRow(r, s.stateOf(0, 2), 2));

    // N_1 = 8, W = 3, N_lo = 5, E = 2, N_x = 3.
    ASSERT_EQ(r.held_pairs.size(), 1u);
    const auto& h = r.held_pairs[0];
    EXPECT_EQ(h.n1, 8);
    EXPECT_EQ(h.wl, 3);
    ASSERT_EQ(h.extras.size(), 1u);
    EXPECT_EQ(h.extras[0].signal, "L5Q");
    EXPECT_EQ(h.extras[0].ewl, 2);
    EXPECT_EQ(h.extras[0].n, 3);
    EXPECT_DOUBLE_EQ(h.extras[0].lam, kL5.lam());

    // The pseudo-observation put the extra signal's DD ambiguity on N_x lambda_x.
    EXPECT_NEAR(r.fixed_state(s.stateOf(0, 2)) - r.fixed_state(2), 3.0 * kL5.lam(), 1e-6);
    EXPECT_NEAR(r.fixed_state(s.stateOf(0, 0)) - r.fixed_state(0), 8.0 * kL1.lam(), 1e-6);
    EXPECT_NEAR(r.fixed_state(s.stateOf(0, 1)) - r.fixed_state(1), 5.0 * kL2.lam(), 1e-6);

    const auto& c = f.resolver->extraSignalCounters();
    EXPECT_EQ(c.evaluated, 1);
    EXPECT_EQ(c.admitted, 1);
    EXPECT_EQ(c.fixed, 1);
    EXPECT_EQ(c.releases_caused, 0);
    const std::string log = f.logText();
    EXPECT_NE(log.find("EWLFLOAT tow=123.0 sat_i=G13 sat_ref=G12 sig_lo=L2W sig_x=L5Q"), std::string::npos) << log;
    EXPECT_NE(log.find("ewl=2 admitted=1"), std::string::npos) << log;
    EXPECT_NE(log.find("  EWL pair G13-G12 L2W/L5Q: N_x_fixed=3 ewl=2"), std::string::npos) << log;
}

TEST(ArExtraSignalsResolverTest, TheOrderOfTheSignalsDoesNotChangeTheFix) {
    Fixture f(extraConfig(true), "on_order");
    const Scene s = makeScene({ goodSat() }, { kL5, kL1, kL2 });
    const auto r = f.run(s);
    ASSERT_TRUE(r.success);
    ASSERT_EQ(r.held_pairs.size(), 1u);
    EXPECT_EQ(r.held_pairs[0].n1, 8);
    ASSERT_EQ(r.held_pairs[0].extras.size(), 1u);
    EXPECT_EQ(r.held_pairs[0].extras[0].n, 3);
    EXPECT_TRUE(hasRow(r, s.stateOf(0, 0), 0));          // the extra signal L5Q is first here
}

TEST(ArExtraSignalsResolverTest, AnExtraSignalWhoseExtraWideLaneFailsTheAdmissionIsLeftOut) {
    Fixture f(extraConfig(true), "on_extra_refused");
    SatSpec sat = goodSat();
    sat.e = 2.45;                                        // half a cycle from an integer
    const Scene s = makeScene({ sat }, { kL1, kL2, kL5 });
    const auto r = f.run(s);
    ASSERT_TRUE(r.success);                              // the primary pair still fixes
    EXPECT_EQ(r.fixed_dd_rows.size(), 2u);
    EXPECT_FALSE(hasRow(r, s.stateOf(0, 2), 2));
    ASSERT_EQ(r.held_pairs.size(), 1u);
    EXPECT_TRUE(r.held_pairs[0].extras.empty());
    const auto& c = f.resolver->extraSignalCounters();
    EXPECT_EQ(c.evaluated, 1);
    EXPECT_EQ(c.admitted, 0);
    EXPECT_EQ(c.fixed, 0);
    EXPECT_NE(f.logText().find("admitted=0"), std::string::npos) << f.logText();
    EXPECT_EQ(f.logText().find("EWL pair"), std::string::npos);
}

TEST(ArExtraSignalsResolverTest, WhenThePrimaryPairIsNotFixedNoExtraSignalIsFixed) {
    // The primary pair fails the per-pair cap (0.02 cycle against 0.001) and partial AR is off.
    AmbiguityResolverConfig cfg = extraConfig(true);
    cfg.nl_max_per_pair_frac = 0.001;
    Fixture f(cfg, "on_primary_unfixed");
    const Scene s = makeScene({ goodSat() }, { kL1, kL2, kL5 });
    const auto r = f.run(s);
    EXPECT_FALSE(r.success);
    EXPECT_TRUE(r.fixed_dd_rows.empty());
    EXPECT_TRUE(r.held_pairs.empty());
    const auto& c = f.resolver->extraSignalCounters();
    EXPECT_EQ(c.admitted, 1);                            // the extra-wide lane was fine
    EXPECT_EQ(c.fixed, 0);                               // but nothing is fixed without its pair
    EXPECT_EQ(f.logText().find("EWL pair"), std::string::npos);
}

TEST(ArExtraSignalsResolverTest, APrimaryPairTheWideLaneAdmissionRefusesTakesItsExtrasNowhere) {
    Fixture f(extraConfig(true), "on_wl_refused");
    SatSpec sat = goodSat();
    sat.w = 3.45;                                        // the primary wide lane fails wl_max_frac
    const Scene s = makeScene({ sat }, { kL1, kL2, kL5 });
    const auto r = f.run(s);
    EXPECT_FALSE(r.success);
    EXPECT_TRUE(r.fixed_dd_rows.empty());
    EXPECT_EQ(f.resolver->extraSignalCounters().evaluated, 0);   // never reached the extras
}

TEST(ArExtraSignalsResolverTest, APartialFixCarriesTheExtraSignalOfTheKeptPairOnly) {
    // Satellite G14 sits 0.3 cycle from its N_1 integer, beyond the per-pair cap, so the full set
    // fails; the partial fix keeps G13 and drops G14, extras with it.
    AmbiguityResolverConfig cfg = extraConfig(true);
    cfg.enable_partial_ar = true;
    Fixture f(cfg, "on_partial");
    SatSpec bad = goodSat("G14");
    bad.n1 = 7.70;
    const Scene s = makeScene({ goodSat("G13"), bad }, { kL1, kL2, kL5 });
    const auto r = f.run(s);
    ASSERT_TRUE(r.success) << f.logText();
    ASSERT_EQ(r.held_pairs.size(), 1u);
    EXPECT_EQ(r.held_pairs[0].sat_i, "G13");
    ASSERT_EQ(r.held_pairs[0].extras.size(), 1u);
    EXPECT_EQ(r.held_pairs[0].extras[0].n, 3);
    EXPECT_EQ(r.fixed_dd_rows.size(), 3u);
    EXPECT_TRUE(hasRow(r, s.stateOf(0, 2), 2));
    EXPECT_FALSE(hasRow(r, s.stateOf(1, 2), 2));
    EXPECT_EQ(f.resolver->extraSignalCounters().evaluated, 2);   // both satellites' extras were evaluated
    EXPECT_EQ(f.resolver->extraSignalCounters().fixed, 1);
    EXPECT_NE(f.logText().find("[PAR] EWL pair G13-G12 L2W/L5Q: N_x_fixed=3 ewl=2"), std::string::npos)
        << f.logText();
}

TEST(ArExtraSignalsResolverTest, TheTwoSignalCaseWithTheKeyOnIsIdenticalToKeyOff) {
    const Scene s = makeScene({ goodSat() }, { kL1, kL2 });
    Fixture off(extraConfig(false), "two_off");
    Fixture on(extraConfig(true), "two_on");
    const auto a = off.run(s);
    const auto b = on.run(s);
    ASSERT_TRUE(a.success);
    ASSERT_TRUE(b.success);
    EXPECT_EQ(a.num_fixed, b.num_fixed);
    EXPECT_EQ(a.ratio, b.ratio);
    EXPECT_EQ(a.p_succ, b.p_succ);
    ASSERT_EQ(a.fixed_dd_rows.size(), b.fixed_dd_rows.size());
    for (std::size_t k = 0; k < a.fixed_dd_rows.size(); ++k) {
        EXPECT_EQ(a.fixed_dd_rows[k].state_idx_i, b.fixed_dd_rows[k].state_idx_i);
        EXPECT_EQ(a.fixed_dd_rows[k].state_idx_ref, b.fixed_dd_rows[k].state_idx_ref);
    }
    ASSERT_EQ(a.held_pairs.size(), b.held_pairs.size());
    EXPECT_EQ(a.held_pairs[0].n1, b.held_pairs[0].n1);
    EXPECT_EQ(a.held_pairs[0].wl, b.held_pairs[0].wl);
    EXPECT_TRUE(b.held_pairs[0].extras.empty());
    EXPECT_TRUE(a.fixed_state == b.fixed_state);
    EXPECT_TRUE(a.fixed_covariance == b.fixed_covariance);
    EXPECT_EQ(on.resolver->extraSignalCounters().evaluated, 0);
}

TEST(ArExtraSignalsResolverTest, AHeldExtraWideLaneTheFloatContradictsReleasesTheSatellite) {
    Fixture f(extraConfig(true, 0.1), "release");
    const Scene fixed = makeScene({ goodSat() }, { kL1, kL2, kL5 });
    const auto first = f.run(fixed);
    ASSERT_TRUE(first.success);
    ArExtraSignalsTestAccess::remember(*f.resolver, first);
    ASSERT_EQ(ArExtraSignalsTestAccess::heldCount(*f.resolver), 1u);

    // The float moves 0.05 cycle in the extra-wide lane: inside the threshold, no release.
    SatSpec near_by = goodSat();
    near_by.e = 2.05;
    auto r = f.run(makeScene({ near_by }, { kL1, kL2, kL5 }));
    EXPECT_TRUE(r.release_satellites.empty());
    EXPECT_EQ(f.resolver->extraSignalCounters().releases_caused, 0);

    // It moves 0.30 cycle: the held extra-wide lane is contradicted, the primary pair is not.
    SatSpec far_off = goodSat();
    far_off.e = 2.30;
    r = f.run(makeScene({ far_off }, { kL1, kL2, kL5 }));
    EXPECT_EQ(r.release_satellites, (std::vector<std::string>{ "G13" }));
    EXPECT_FALSE(r.success);                             // the released satellite sits out the epoch
    EXPECT_EQ(f.resolver->extraSignalCounters().releases_caused, 1);
    EXPECT_EQ(f.resolver->holdReleaseCount(), 1);
    EXPECT_EQ(ArExtraSignalsTestAccess::heldCount(*f.resolver), 0u);
    const std::string log = f.logText();
    EXPECT_NE(log.find("HOLDREL tow=123.0 pair=G13-G12 L2W/L5Q held_ewl=2 d_ewl=+0.300"), std::string::npos) << log;
    EXPECT_NE(log.find("HOLDREL tow=123.0 refloat sat=G13"), std::string::npos) << log;
    // The primary measures alone did not contradict.
    EXPECT_EQ(log.find("pair=G13-G12 L1C/L2W"), std::string::npos) << log;
}

TEST(ArExtraSignalsResolverTest, AHeldExtraWideLaneIsNotCheckedWithoutAReleaseThreshold) {
    Fixture f(extraConfig(true, 0.0), "release_off");
    const auto first = f.run(makeScene({ goodSat() }, { kL1, kL2, kL5 }));
    ASSERT_TRUE(first.success);
    EXPECT_TRUE(first.held_pairs.empty());               // no memory without hold_release_cyc
    EXPECT_EQ(first.fixed_dd_rows.size(), 3u);           // the extra signal is still fixed and held
}

TEST(ArExtraSignalsResolverTest, TheRunEndLineReportsTheCounters) {
    Fixture f(extraConfig(true), "run_end");
    ASSERT_TRUE(f.run(makeScene({ goodSat() }, { kL1, kL2, kL5 })).success);
    const std::string line = f.resolver->extraSignalsRunEndLine();
    EXPECT_NE(line.find("1 extra-wide lanes evaluated"), std::string::npos) << line;
    EXPECT_NE(line.find("1 admitted"), std::string::npos) << line;
    EXPECT_NE(line.find("1 fixed"), std::string::npos) << line;
    EXPECT_NE(line.find("0 held pairs released"), std::string::npos) << line;
}

TEST(ArExtraSignalsResolverTest, TheKeyIsInertWithoutWlFromFloat) {
    AmbiguityResolverConfig cfg = extraConfig(true);
    cfg.wl_from_float = false;
    Fixture f(cfg, "inert");
    EXPECT_FALSE(f.resolver->extraSignalsActive());
    f.run(makeScene({ goodSat() }, { kL1, kL2, kL5 }));
    EXPECT_EQ(f.resolver->extraSignalCounters().evaluated, 0);
    EXPECT_EQ(f.logText().find("EWLFLOAT"), std::string::npos);
}

// ---------------------------------------------------------------------------
// The keys
// ---------------------------------------------------------------------------

namespace {

struct Loaded {
    GnssConfig  cfg;
    std::string log_text;
    bool        ok{ false };
};

/// A configuration loaded through the loader, with the "gnss_config" log captured.
Loaded loadWithLog(const std::string& ar_body, const std::string& tag) {
    Loaded out;
    const fs::path dir = gnss_test::tempDir() / ("ar_extra_signals_cfg_" + tag);
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir, ec);
    LogManager log;
    spdlog::drop("gnss_config");
    log.initialize(dir, LogLevel::Info);
    std::ostringstream text;
    log.getLogger("gnss_config")->sinks().push_back(
        std::make_shared<spdlog::sinks::ostream_sink_mt>(text));
    const fs::path file = dir / "config.json";
    {
        std::ofstream f(file);
        f << R"({ "processing": { "mode": "ppp-rtk", "active_systems": ["GPS"] },
                  "estimation": { "ambiguity_resolution": { )" << ar_body << R"( } } })";
    }
    out.ok = out.cfg.loadFromJson(file, log);
    log.getLogger("gnss_config")->flush();
    out.log_text = text.str();
    spdlog::drop("gnss_config");
    return out;
}

} // namespace

TEST(ArExtraSignalsConfigTest, TheDefaultsAreOffAndTheTableIsGpsGalileoBeiDou) {
    const std::map<std::string, std::string> expect = { { "GPS", "12" }, { "Galileo", "15" },
                                                        { "BeiDou", "26" } };
    EXPECT_FALSE(AmbiguityResolverConfig{}.extra_signals);
    EXPECT_EQ(AmbiguityResolverConfig{}.primary_bands, expect);
    EXPECT_FALSE(GnssConfig{}.getArExtraSignals());
    EXPECT_EQ(GnssConfig{}.getArPrimaryBands(), expect);
    const Loaded l = loadWithLog(R"("mode": 3)", "default");
    ASSERT_TRUE(l.ok);
    EXPECT_FALSE(l.cfg.getArExtraSignals());
    EXPECT_EQ(l.cfg.getArPrimaryBands(), expect);
    EXPECT_EQ(l.log_text.find("extra_signals"), std::string::npos) << l.log_text;
    EXPECT_FALSE(makeAmbiguityResolverConfig(l.cfg).extra_signals);
}

TEST(ArExtraSignalsConfigTest, TheKeysParseAndReachTheResolverConfig) {
    const Loaded l = loadWithLog(
        R"("mode": 3, "wl_from_float": true, "extra_signals": true,
           "primary_bands": { "GPS": "15", "Galileo": "17" })", "parsed");
    ASSERT_TRUE(l.ok);
    EXPECT_TRUE(l.cfg.getArExtraSignals());
    const std::map<std::string, std::string> expect = { { "GPS", "15" }, { "Galileo", "17" } };
    EXPECT_EQ(l.cfg.getArPrimaryBands(), expect);      // a given object replaces the table
    EXPECT_NE(l.log_text.find("(extra_signals)"), std::string::npos) << l.log_text;
    const AmbiguityResolverConfig c = makeAmbiguityResolverConfig(l.cfg);
    EXPECT_TRUE(c.extra_signals);
    EXPECT_EQ(c.primary_bands, expect);
}

TEST(ArExtraSignalsConfigTest, TheKeyIsInertWithAWarningWithoutWlFromFloat) {
    const Loaded l = loadWithLog(R"("mode": 3, "extra_signals": true)", "no_float");
    ASSERT_TRUE(l.ok);
    EXPECT_FALSE(l.cfg.getArExtraSignals());
    EXPECT_NE(l.log_text.find("extra_signals needs wl_from_float"), std::string::npos) << l.log_text;
    EXPECT_FALSE(makeAmbiguityResolverConfig(l.cfg).extra_signals);
}

TEST(ArExtraSignalsConfigTest, AMalformedPrimaryBandsEntryKeepsTheDefaultOfItsConstellationWithAWarning) {
    for (const char* bad : { R"("GPS": "11")", R"("GPS": "1")", R"("GPS": "123")", R"("GPS": "1a")",
                             R"("GPS": 12)" }) {
        const Loaded l = loadWithLog(std::string(R"("mode": 3, "primary_bands": { )") + bad
                                         + R"(, "Galileo": "17" })", "bad");
        ASSERT_TRUE(l.ok) << bad;
        const std::map<std::string, std::string> expect = { { "GPS", "12" }, { "Galileo", "17" } };
        EXPECT_EQ(l.cfg.getArPrimaryBands(), expect) << bad;
        EXPECT_NE(l.log_text.find("primary_bands entry GPS"), std::string::npos) << l.log_text;
        EXPECT_NE(l.log_text.find("the default 12 is kept"), std::string::npos) << l.log_text;
    }
}

TEST(ArExtraSignalsConfigTest, AMalformedEntryOfAConstellationWithoutADefaultIsDropped) {
    const Loaded l = loadWithLog(R"("mode": 3, "primary_bands": { "GLONASS": "11", "GPS": "12" })", "drop");
    ASSERT_TRUE(l.ok);
    const std::map<std::string, std::string> expect = { { "GPS", "12" } };
    EXPECT_EQ(l.cfg.getArPrimaryBands(), expect);
    EXPECT_NE(l.log_text.find("primary_bands entry GLONASS"), std::string::npos) << l.log_text;
    EXPECT_NE(l.log_text.find("the entry is dropped"), std::string::npos) << l.log_text;
}

TEST(ArExtraSignalsConfigTest, APrimaryBandsThatIsNotAnObjectKeepsTheDefaultTable) {
    const Loaded l = loadWithLog(R"("mode": 3, "primary_bands": "12")", "not_object");
    ASSERT_TRUE(l.ok);
    EXPECT_EQ(l.cfg.getArPrimaryBands(), defaultArPrimaryBands());
    EXPECT_NE(l.log_text.find("primary_bands is not an object"), std::string::npos) << l.log_text;
}

TEST(ArExtraSignalsConfigTest, ABandPairIsTwoDistinctDigits) {
    EXPECT_TRUE(isValidArPrimaryBandPair("12"));
    EXPECT_TRUE(isValidArPrimaryBandPair("51"));
    EXPECT_FALSE(isValidArPrimaryBandPair("11"));
    EXPECT_FALSE(isValidArPrimaryBandPair("1"));
    EXPECT_FALSE(isValidArPrimaryBandPair("123"));
    EXPECT_FALSE(isValidArPrimaryBandPair(""));
    EXPECT_FALSE(isValidArPrimaryBandPair("1a"));
    EXPECT_FALSE(isValidArPrimaryBandPair("L1"));
}

} // namespace
