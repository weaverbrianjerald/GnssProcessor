/**
 * @file test_realtime_seams.cpp
 * @brief Stream-level real-time invariants the position grader cannot see.
 *
 * The real-time replay tier (manifest_realtime.json) grades the converged
 * position vs an independent site coordinate -- a strong end-to-end guard, but
 * blind to two failure modes that can still produce a *plausible-looking*
 * position while being wrong:
 *
 *   1. An old observation consuming a FUTURE-stamped SSR correction and
 *      extrapolating the orbit/clock backward (the obs<->SSR sync hole: high-rate
 *      obs back up behind real-time while sparse corrections stay fresh, so an
 *      old obs queries a fresh correction with a large *negative* age).
 *      Contract lives in SsrCorrectionStore freshness gating.
 *
 *   2. A broadcast ephemeris extrapolated far beyond its fit interval (a
 *      never-refreshed sat, or obs/eph desync) silently yielding a junk orbit or
 *      clock instead of dropping the satellite.  Contract lives in the
 *      LiveEphemerisStore fit-window guard (the BSSD epoch-blowup / RT
 *      model-degradation root).
 *
 * Both are unit-constructible without a PppModel / RINEX chain, deterministic,
 * and complementary to the position-accuracy regression cell.  See
 * docs/test_suite.md (Real-time replay tier).
 */

#include <gtest/gtest.h>

#include <cmath>

#include "products/SsrCorrectionStore.hpp"
#include "products/BroadcastEphemerisTypes.hpp"
#include "products/LiveEphemerisStore.hpp"
#include "products/ProductResolver.hpp"  // RTB-4 ULT naming statics (header-only use)
#include "realtime/RealTimeEngine.hpp"   // RTA-8(b) ingest-filter statics (header-only use)
#include "rtcm/RtcmTypes.hpp"
#include "common/Namespaces.hpp"

namespace GnssProcessor {
namespace {

SsrClockCorrection makeClock(const std::string& sat, double c0_m,
                             const GnssTime& received) {
    SsrClockCorrection clk;
    clk.sat           = sat;
    clk.c0            = c0_m;
    clk.received_time = received;
    clk.epoch_time    = received;
    return clk;
}

// ===========================================================================
// (1) obs<->SSR sync: freshness gating is three-region, and the FUTURE side is
// the one the obs-backlog bug exercised.  age = query - record.received_time:
//   age >  max_age   -> stale past record      -> REJECT  (covered elsewhere)
//   -max_age<=age<0  -> mild future (jitter)    -> ACCEPT
//   age < -max_age   -> record far in the future-> REJECT  (the backlog hole)
// Tested as a black box through getOrbitClock so it pins the consumed behaviour,
// not a private helper.
// ===========================================================================

// A correction received well AFTER the obs being processed (old obs, fresh
// correction) must be rejected -- never extrapolated backward onto stale obs.
TEST(RealtimeSsrFreshnessTest, FutureRecordBeyondMaxAgeRejected) {
    auto& store = SsrCorrectionStore::getInstance();
    store.clear();
    // record stamped 100 s in the FUTURE relative to the query; max_age 30 s.
    store.pushClock(makeClock("G07", 0.05, GnssTime{2300, 200.0}), SsrHeader{});
    auto q = store.getOrbitClock("G07", GnssTime{2300, 100.0}, /*max_age_s=*/30.0);
    EXPECT_FALSE(q.has_value());
    store.clear();
}

// A small negative age (receipt jitter / sub-epoch drift) is within tolerance
// and must still be consumed -- otherwise normal stream timing would drop
// corrections.
TEST(RealtimeSsrFreshnessTest, MildFutureRecordAccepted) {
    auto& store = SsrCorrectionStore::getInstance();
    store.clear();
    // record only 20 s ahead of the query; max_age 30 s -> mild future -> accept.
    store.pushClock(makeClock("G07", 0.05, GnssTime{2300, 120.0}), SsrHeader{});
    auto q = store.getOrbitClock("G07", GnssTime{2300, 100.0}, /*max_age_s=*/30.0);
    ASSERT_TRUE(q.has_value());
    EXPECT_TRUE(q->has_clock);
    EXPECT_NEAR(q->c0, 0.05, 1e-12);
    store.clear();
}

// ===========================================================================
// (2) Fit-interval guard: an ephemeris extrapolated beyond ~2x its broadcast fit
// interval is rejected (sat skipped) rather than emitting a silently-wrong
// orbit/clock.  Per-system windows: GPS/QZSS 4 h, GAL/BDS 2 h, GLONASS 0.5 h.
// ===========================================================================

BroadcastEphemeris gpsEph(const std::string& sat = "G07") {
    BroadcastEphemeris eph;
    eph.satellite = sat;
    eph.system    = System::GPS;
    eph.iode      = 10;
    auto& k = eph.kepler;
    k.toe = GnssTime{2300, 7200.0};
    k.toc = GnssTime{2300, 7200.0};
    k.sqrt_a = 5153.6; k.e = 0.0;
    k.i0 = 0.96; k.omega0 = 0.5; k.omega = 0.3; k.m0 = 0.2;
    return eph;
}

// Just inside the GPS 4 h window: orbit resolves. Just beyond it: rejected.
TEST(RealtimeEphStalenessTest, GpsOrbitDroppedBeyondFitWindow) {
    auto& store = LiveEphemerisStore::getInstance();
    store.clear();
    store.insert(gpsEph());

    // toe + 3 h 50 m  (< 4 h) -> still usable.
    auto ok = store.computeSatellitePV("G07", GnssTime{2300, 7200.0 + 3.83 * 3600.0});
    EXPECT_TRUE(ok.has_value());

    // toe + 4 h 10 m  (> 4 h) -> rejected, not silently extrapolated.
    auto stale = store.computeSatellitePV("G07", GnssTime{2300, 7200.0 + 4.17 * 3600.0});
    EXPECT_FALSE(stale.has_value());
    store.clear();
}

// The clock path enforces the same guard (so a stale sat drops entirely, never
// contributing a junk clock that the position filter would have to reject).
TEST(RealtimeEphStalenessTest, GpsClockDroppedBeyondFitWindow) {
    auto& store = LiveEphemerisStore::getInstance();
    store.clear();
    store.insert(gpsEph());

    EXPECT_TRUE(store.computeSatelliteClock(
        "G07", GnssTime{2300, 7200.0 + 1800.0}).has_value());
    EXPECT_FALSE(store.computeSatelliteClock(
        "G07", GnssTime{2300, 7200.0 + 5.0 * 3600.0}).has_value());
    store.clear();
}

// Galileo's window is tighter (2 h): a query stale for GPS-standards (3 h) is
// already rejected for Galileo -- pins the per-system table, not a global const.
TEST(RealtimeEphStalenessTest, GalileoWindowTighterThanGps) {
    auto& store = LiveEphemerisStore::getInstance();
    store.clear();
    BroadcastEphemeris e = gpsEph("E11");
    e.system = System::Galileo;
    e.iode   = 44;
    store.insert(e);

    // toe + 3 h: inside GPS's 4 h but beyond Galileo's 2 h -> dropped.
    EXPECT_FALSE(store.computeSatellitePV(
        "E11", GnssTime{2300, 7200.0 + 3.0 * 3600.0}).has_value());
    // toe + 1 h: inside Galileo's 2 h -> usable.
    EXPECT_TRUE(store.computeSatellitePV(
        "E11", GnssTime{2300, 7200.0 + 1.0 * 3600.0}).has_value());
    store.clear();
}

// ===========================================================================
// (3) RT0-1 -- clock-only IODE gate.  A network / IGS-SSR CLOCK subtype carries
// no orbit IODE, so getOrbitClock must report iode = -1 ("unknown") for it.  The
// downstream clock gate (GnssCorrections) computes
//     ssr_iode  = has_clock ? ssr->iode : -1;
//     brdc_iode = live broadcast IODE (>=0 in RT, -1 in post-processing);
// and applies the SSR clock only when SsrCorrectionStore::iodeMatches passes.
// A {0} default (the pre-fix bug) collided with a real broadcast IODE and
// silently dropped EVERY network clock in live RT, while post-processing's empty
// live store (brdc_iode=-1) stayed permissive and masked it.  These pin both the
// store-seam value and the consumed gate behaviour without a PppModel chain.
// ===========================================================================

// A standalone clock record reports the -1 "unknown" IODE sentinel, not {0}.
TEST(RealtimeSsrIodeGateTest, ClockOnlyRecordReportsUnknownIode) {
    auto& store = SsrCorrectionStore::getInstance();
    store.clear();
    store.pushClock(makeClock("G07", 0.05, GnssTime{2300, 100.0}), SsrHeader{});
    auto q = store.getOrbitClock("G07", GnssTime{2300, 100.0}, /*max_age_s=*/30.0);
    ASSERT_TRUE(q.has_value());
    EXPECT_TRUE(q->has_clock);
    EXPECT_FALSE(q->has_orbit);      // network emitter pushes clock only (orbit from IGS)
    EXPECT_EQ(q->iode, -1);          // unknown -> gate-permissive (NOT the {0} default)
    store.clear();
}

// Card headline: with a POPULATED live ephemeris store (brdc_iode >= 0, the RT
// condition), a clock-only network correction is APPLIED, not dropped.  The
// pre-fix {0} default would have failed iodeMatches against a non-zero broadcast
// IODE -- pinned below as the regression witness.
TEST(RealtimeSsrIodeGateTest, NetworkClockAppliesAgainstPopulatedBroadcastIode) {
    auto& eph_store = LiveEphemerisStore::getInstance();
    auto& ssr_store = SsrCorrectionStore::getInstance();
    eph_store.clear();
    ssr_store.clear();

    // Live RT: broadcast ephemeris present with a real, non-zero IODE (10).
    eph_store.insert(gpsEph("G07"));

    // Network producer pushes a CLOCK-ONLY correction (orbit comes from IGS).
    const GnssTime t{2300, 7200.0};
    ssr_store.pushClock(makeClock("G07", 0.05, t), SsrHeader{});

    const auto ssr = ssr_store.getOrbitClock("G07", t, /*max_age_s=*/30.0);
    ASSERT_TRUE(ssr.has_value());
    ASSERT_TRUE(ssr->has_clock);
    ASSERT_FALSE(ssr->has_orbit);

    // Reproduce the GnssCorrections clock gate exactly (GnssCorrections.cpp: the
    // ssr_iode / brdc_iode / iodeMatches trio) against the SHARED gate function.
    const int  ssr_iode  = (ssr && ssr->has_clock) ? ssr->iode : -1;
    const auto bc        = eph_store.computeSatelliteClock("G07", t, ssr_iode);
    ASSERT_TRUE(bc.has_value());
    const int  brdc_iode = bc->iode;

    EXPECT_EQ(ssr_iode, -1);         // clock-only -> permissive
    EXPECT_GE(brdc_iode, 0);         // populated live store -> real IODE (the RT trap)
    EXPECT_EQ(brdc_iode, 10);        // gpsEph IODE, unchanged by the -1 re-reference

    // Gate MUST pass -> the network clock is applied.
    EXPECT_TRUE(SsrCorrectionStore::iodeMatches(brdc_iode, ssr->iode));

    // ...and the gate's OTHER co-condition holds (GnssCorrections.cpp:242 gates on
    // std::isfinite(clock_corr) AND iodeMatches, where clock_corr = broadcast
    // baseline + SSR delta).  Assert a finite, non-trivial correction is genuinely
    // applied -- an IODE match alone does not prove the clock is usable.
    const double applied    = SsrCorrectionStore::applyClockToBias(
        ssr->c0, ssr->c1, ssr->c2, ssr->hr_clock, /*dt=*/0.0);   // query epoch == SSR epoch
    const double clock_corr = bc->bias + applied;                // seconds
    EXPECT_TRUE(std::isfinite(clock_corr));
    EXPECT_GT(std::abs(applied), 0.0);                           // the 0.05 m c0 actually contributes

    // Regression witness: the pre-fix {0} default would have collided here
    // (brdc_iode 10 != 0) and dropped the clock.  If a future change reintroduces
    // a {0} default, ClockOnlyRecordReportsUnknownIode fails; this pins WHY.
    EXPECT_FALSE(SsrCorrectionStore::iodeMatches(brdc_iode, /*old default=*/0));

    eph_store.clear();
    ssr_store.clear();
}

// ===========================================================================
// (4) RTA-8(b) — per-stream SSR ingest filter.  The record-in → push-decision-
// out matrix is a pure static (ssrPushAction) factored out of feedRtcmFrame so
// the policy is pinned without a parser/engine; the combined-record
// decomposition is pinned end-to-end through an OWNED store.
// ===========================================================================

// Default policy All: every record kind pushes — the byte-identical legacy path.
TEST(SsrIngestFilterTest, AllPolicyPushesEveryKind) {
    using RTE = RealTimeEngine;
    for (auto kind : { RTE::SsrRecordKind::Orbit,    RTE::SsrRecordKind::Clock,
                       RTE::SsrRecordKind::Combined, RTE::SsrRecordKind::CodeBias,
                       RTE::SsrRecordKind::PhaseBias, RTE::SsrRecordKind::Ura,
                       RTE::SsrRecordKind::HighRateClock }) {
        EXPECT_EQ(RTE::ssrPushAction(RTE::SsrAcceptPolicy::All, kind),
                  RTE::SsrPushAction::Push);
    }
}

// OrbitOnly: orbit + URA push as today; combined decomposes; every clock- or
// bias-bearing kind drops — including the high-rate clock (clock content).
TEST(SsrIngestFilterTest, OrbitOnlyPolicyMatrix) {
    using RTE = RealTimeEngine;
    const auto p = RTE::SsrAcceptPolicy::OrbitOnly;
    EXPECT_EQ(RTE::ssrPushAction(p, RTE::SsrRecordKind::Orbit),
              RTE::SsrPushAction::Push);
    EXPECT_EQ(RTE::ssrPushAction(p, RTE::SsrRecordKind::Ura),
              RTE::SsrPushAction::Push);
    EXPECT_EQ(RTE::ssrPushAction(p, RTE::SsrRecordKind::Combined),
              RTE::SsrPushAction::DecomposeOrbit);
    EXPECT_EQ(RTE::ssrPushAction(p, RTE::SsrRecordKind::Clock),
              RTE::SsrPushAction::Drop);
    EXPECT_EQ(RTE::ssrPushAction(p, RTE::SsrRecordKind::CodeBias),
              RTE::SsrPushAction::Drop);
    EXPECT_EQ(RTE::ssrPushAction(p, RTE::SsrRecordKind::PhaseBias),
              RTE::SsrPushAction::Drop);
    EXPECT_EQ(RTE::ssrPushAction(p, RTE::SsrRecordKind::HighRateClock),
              RTE::SsrPushAction::Drop);
}

// A combined orbit+clock record decomposed at the push site yields an
// orbit-only store entry: has_orbit without has_clock, the RAC fields / IODE /
// content epoch carried over, the clock polynomial gone.  The store's
// combined-wins merge logic is untouched — the combined record never entered.
TEST(SsrIngestFilterTest, CombinedDecomposesToOrbitOnlyPush) {
    SsrCombinedCorrection comb;
    comb.sat           = "G07";
    comb.iode          = 42;
    comb.delta_rac     = Eigen::Vector3d{0.5, -0.2, 0.1};
    comb.delta_rac_dot = Eigen::Vector3d{1e-4, -2e-4, 3e-5};
    comb.c0            = 0.8;              // clock content that must NOT survive
    comb.c1            = 1e-4;
    comb.c2            = 1e-7;
    comb.received_time = GnssTime{2300, 100.0};
    comb.epoch_time    = GnssTime{2300,  95.0};
    comb.iod_ssr       = 7;

    const SsrOrbitCorrection orb = RealTimeEngine::combinedOrbitPart(comb);
    EXPECT_EQ(orb.sat, "G07");
    EXPECT_EQ(orb.iode, 42);
    EXPECT_TRUE(orb.delta_rac.isApprox(comb.delta_rac));
    EXPECT_TRUE(orb.delta_rac_dot.isApprox(comb.delta_rac_dot));
    EXPECT_EQ(orb.received_time.week, 2300);
    EXPECT_NEAR(orb.received_time.tow, 100.0, 1e-12);
    EXPECT_NEAR(orb.epoch_time.tow,     95.0, 1e-12);
    EXPECT_EQ(orb.iod_ssr, 7);

    SsrCorrectionStore store;   // owned: no singleton coupling
    store.pushOrbit(orb, SsrHeader{});
    const auto q = store.getOrbitClock("G07", GnssTime{2300, 100.0}, 30.0);
    ASSERT_TRUE(q.has_value());
    EXPECT_TRUE(q->has_orbit);
    EXPECT_FALSE(q->has_clock);              // no clock content leaked
    EXPECT_NEAR(q->c0, 0.0, 1e-15);
    EXPECT_EQ(q->iode, 42);
    EXPECT_TRUE(q->delta_rac.isApprox(comb.delta_rac));
    EXPECT_NEAR(q->orbit_epoch_time.tow, 95.0, 1e-12);  // orbit dt base preserved
}

// RTB-5(b): the combined→orbit decomposition must carry the SSR reference-
// point tag — a legacy (APC-referenced) 1060/1066 record decomposed under
// the orbit_only ingest policy is STILL an APC-referenced orbit, and losing
// the tag would silently re-enable the satellite-PCO double count under
// realtime.honor_ssr_reference_point.
TEST(SsrIngestFilterTest, CombinedDecompositionPreservesReferencePoint) {
    SsrCombinedCorrection comb;
    comb.sat             = "E11";
    comb.iode            = 44;
    comb.received_time   = GnssTime{2300, 100.0};
    comb.epoch_time      = GnssTime{2300, 100.0};
    comb.reference_point = SsrReferencePoint::APC;   // legacy dialect

    const SsrOrbitCorrection orb = RealTimeEngine::combinedOrbitPart(comb);
    EXPECT_EQ(orb.reference_point, SsrReferencePoint::APC);

    SsrCorrectionStore store;   // owned: no singleton coupling
    store.pushOrbit(orb, SsrHeader{});
    const auto q = store.getOrbitClock("E11", GnssTime{2300, 100.0}, 30.0);
    ASSERT_TRUE(q.has_value());
    EXPECT_TRUE(q->has_orbit);
    EXPECT_EQ(q->orbit_reference_point, SsrReferencePoint::APC);

    // And the default stays CoM so every non-decoder construction is unchanged.
    SsrCombinedCorrection def;
    EXPECT_EQ(RealTimeEngine::combinedOrbitPart(def).reference_point,
              SsrReferencePoint::CoM);
}

// Config-string parsing: "all"/empty → All, "orbit_only" (any case) →
// OrbitOnly, unknown → All with recognized=false so the loader can warn.
TEST(SsrIngestFilterTest, ParseAcceptPolicy) {
    using RTE = RealTimeEngine;
    bool ok = false;
    EXPECT_EQ(RTE::parseSsrAcceptPolicy("all", &ok), RTE::SsrAcceptPolicy::All);
    EXPECT_TRUE(ok);
    EXPECT_EQ(RTE::parseSsrAcceptPolicy("", &ok), RTE::SsrAcceptPolicy::All);
    EXPECT_TRUE(ok);
    EXPECT_EQ(RTE::parseSsrAcceptPolicy("orbit_only", &ok), RTE::SsrAcceptPolicy::OrbitOnly);
    EXPECT_TRUE(ok);
    EXPECT_EQ(RTE::parseSsrAcceptPolicy("ORBIT_ONLY", &ok), RTE::SsrAcceptPolicy::OrbitOnly);
    EXPECT_TRUE(ok);
    EXPECT_EQ(RTE::parseSsrAcceptPolicy("bogus", &ok), RTE::SsrAcceptPolicy::All);
    EXPECT_FALSE(ok);
}

// ===========================================================================
// (5) RTA-9(b) — clock staleness ladder.  clockHoldWindow/classifyClockAge are
// the single decision helpers GnssCorrections (apply window) and
// MeasurementModel (R-inflation rung) share; the store-level test pins that a
// held clock is genuinely retrievable through the widened window.
// ===========================================================================

TEST(SsrClockStaleHoldTest, WindowAndClassification) {
    using Store = SsrCorrectionStore;
    // Factor 0 (default): window is the base gate — the rung is off.
    EXPECT_DOUBLE_EQ(Store::clockHoldWindow(30.0, 0.0), 30.0);
    // Factor 3: window widens; factors ≤ 1 cannot shrink the base gate.
    EXPECT_DOUBLE_EQ(Store::clockHoldWindow(30.0, 3.0), 90.0);
    EXPECT_DOUBLE_EQ(Store::clockHoldWindow(30.0, 0.5), 30.0);

    using Rung = Store::ClockAgeRung;
    // Rung off (factor 0): the historic two-state gate.
    EXPECT_EQ(Store::classifyClockAge(20.0, 30.0, 0.0), Rung::Fresh);
    EXPECT_EQ(Store::classifyClockAge(45.0, 30.0, 0.0), Rung::Rejected);
    // Rung armed (factor 3): fresh ≤ 30 < held ≤ 90 < rejected.
    EXPECT_EQ(Store::classifyClockAge(30.0, 30.0, 3.0), Rung::Fresh);      // boundary: still fresh
    EXPECT_EQ(Store::classifyClockAge(30.1, 30.0, 3.0), Rung::StaleHeld);
    EXPECT_EQ(Store::classifyClockAge(90.0, 30.0, 3.0), Rung::StaleHeld);  // boundary: last held age
    EXPECT_EQ(Store::classifyClockAge(90.1, 30.0, 3.0), Rung::Rejected);
    // Future side mirrors freshEnough: mild negative fresh, far-future rejected.
    EXPECT_EQ(Store::classifyClockAge(-20.0,  30.0, 3.0), Rung::Fresh);
    EXPECT_EQ(Store::classifyClockAge(-100.0, 30.0, 3.0), Rung::Rejected);
}

// Store-level: a clock aged past ssr_max_age but inside the hold window is
// rejected by the base gate yet retrievable through clockHoldWindow — exactly
// the lookup GnssCorrections performs when the rung is armed.
TEST(SsrClockStaleHoldTest, HeldClockRetrievableThroughWiderWindow) {
    SsrCorrectionStore store;   // owned: no singleton coupling
    SsrClockCorrection clk = makeClock("G07", 0.05, GnssTime{2300, 100.0});
    clk.c1 = 1e-3;              // drift, so the held C0+C1·dt evaluation matters
    store.pushClock(clk, SsrHeader{});

    const GnssTime q{2300, 145.0};   // age 45 s
    EXPECT_FALSE(store.getOrbitClock("G07", q, /*max_age_s=*/30.0).has_value());

    const double window = SsrCorrectionStore::clockHoldWindow(30.0, 3.0);
    const auto held = store.getOrbitClock("G07", q, window);
    ASSERT_TRUE(held.has_value());
    EXPECT_TRUE(held->has_clock);
    EXPECT_NEAR(held->c0, 0.05, 1e-12);
    EXPECT_NEAR(held->c1, 1e-3, 1e-15);
    EXPECT_EQ(SsrCorrectionStore::classifyClockAge(45.0, 30.0, 3.0),
              SsrCorrectionStore::ClockAgeRung::StaleHeld);
}

// ===========================================================================
// (6) RTB-4 — ultra-rapid (ULT) orbit naming.  ProductResolver's public
// naming statics pin the IGS long-name grammar, the per-analysis-centre
// sampling token, the token fallback pair, and the start-hour walk-back
// candidate sequences — all without touching the network.  Span convention
// (load-bearing, documented at the statics): 02D = 48 h from the filename
// epoch, 24 h OBSERVED then 24 h PREDICTED, release ≈ epoch + ~27 h.
// ===========================================================================

// Per-centre sampling token: COD publishes 05M ultra-rapids, the IGS combined
// product is 15M; unknown centres default to the IGS-combined 15M.  The
// alternate token is always the other member of the {05M, 15M} pair.
TEST(UltOrbitNamingTest, PerSourceSamplingTokenAndAlternate) {
    EXPECT_EQ(ProductResolver::ultSamplingToken("COD"), "05M");
    EXPECT_EQ(ProductResolver::ultSamplingToken("IGS"), "15M");
    EXPECT_EQ(ProductResolver::ultSamplingToken("GFZ"), "15M");   // unknown -> default
    EXPECT_EQ(ProductResolver::ultSamplingToken(""),    "15M");

    EXPECT_EQ(ProductResolver::ultAlternateSamplingToken("05M"), "15M");
    EXPECT_EQ(ProductResolver::ultAlternateSamplingToken("15M"), "05M");
}

// Long-name grammar: <SRC>0OPSULT_<YYYYDDDHHMM>_02D_<SMP>_ORB.SP3.gz with
// zero-padded DOY/hour and MM fixed at "00".
TEST(UltOrbitNamingTest, FilenameGrammar) {
    EXPECT_EQ(ProductResolver::buildUltOrbFilename("IGS", 2024, 129, 0, "15M"),
              "IGS0OPSULT_20241290000_02D_15M_ORB.SP3.gz");
    EXPECT_EQ(ProductResolver::buildUltOrbFilename("COD", 2024, 129, 18, "05M"),
              "COD0OPSULT_20241291800_02D_05M_ORB.SP3.gz");
    EXPECT_EQ(ProductResolver::buildUltOrbFilename("COD", 2026, 5, 6, "05M"),
              "COD0OPSULT_20260050600_02D_05M_ORB.SP3.gz");
}

// Obs-day walk-back (anchor (D, 00)): the first candidate's OBSERVED half
// covers the whole processing day; each subsequent candidate is 6 h earlier
// across the previous day, degrading day-D coverage toward the predicted
// half.  Six candidates cover the worst-case live release latency
// (newest published start epoch up to ~33 h old just before a release).
TEST(UltOrbitNamingTest, ObsDayWalkBackSequence) {
    const auto c = ProductResolver::ultStartCandidates(2024, 129, 0);
    ASSERT_EQ(c.size(), static_cast<size_t>(ProductResolver::kUltWalkBackTries));
    const int expected[][3] = {
        { 2024, 129,  0 },
        { 2024, 128, 18 },
        { 2024, 128, 12 },
        { 2024, 128,  6 },
        { 2024, 128,  0 },
        { 2024, 127, 18 },
    };
    for (size_t i = 0; i < c.size(); ++i) {
        EXPECT_EQ(c[i].year, expected[i][0]) << "candidate " << i;
        EXPECT_EQ(c[i].doy,  expected[i][1]) << "candidate " << i;
        EXPECT_EQ(c[i].hour, expected[i][2]) << "candidate " << i;
    }
}

// Previous-day margin walk-back (anchor (D-1, 18)): the freshest file whose
// start precedes D 00:00, walking back in the same 6 h steps.
TEST(UltOrbitNamingTest, PrevDayMarginCandidateSequence) {
    const auto c = ProductResolver::ultStartCandidates(2024, 128, 18);
    ASSERT_EQ(c.size(), static_cast<size_t>(ProductResolver::kUltWalkBackTries));
    const int expected[][3] = {
        { 2024, 128, 18 },
        { 2024, 128, 12 },
        { 2024, 128,  6 },
        { 2024, 128,  0 },
        { 2024, 127, 18 },
        { 2024, 127, 12 },
    };
    for (size_t i = 0; i < c.size(); ++i) {
        EXPECT_EQ(c[i].year, expected[i][0]) << "candidate " << i;
        EXPECT_EQ(c[i].doy,  expected[i][1]) << "candidate " << i;
        EXPECT_EQ(c[i].hour, expected[i][2]) << "candidate " << i;
    }
}

// Day and year boundaries: DOY 1 walks into the previous year's final day
// (leap-aware via shiftDate).  2023 is not a leap year (365); 2024 is (366).
TEST(UltOrbitNamingTest, WalkBackCrossesYearBoundary) {
    const auto c24 = ProductResolver::ultStartCandidates(2024, 1, 0);
    ASSERT_GE(c24.size(), 2u);
    EXPECT_EQ(c24[1].year, 2023);
    EXPECT_EQ(c24[1].doy,  365);
    EXPECT_EQ(c24[1].hour, 18);

    const auto c25 = ProductResolver::ultStartCandidates(2025, 1, 0);
    ASSERT_GE(c25.size(), 2u);
    EXPECT_EQ(c25[1].year, 2024);
    EXPECT_EQ(c25[1].doy,  366);   // leap year
    EXPECT_EQ(c25[1].hour, 18);
}

} // namespace
} // namespace GnssProcessor
