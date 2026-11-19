/**
 * @file test_realtime_reaction_generation.cpp
 * @brief The user engine's reaction to a change of the producer's generation id, and the
 *        integer-fixing eligibility of a signal whose phase bias is or is not applied.
 *
 * A change of the generation id on the correction stream means the producer restarted or
 * re-realized its datum: every network-corrected ambiguity must be refloated, and the state
 * is trusted again only after each refloated key is re-seeded.  Integer-fixing eligibility
 * decides which keys the integer search may fix: a key whose satellite phase bias is not
 * removed from the phase observable carries a non-integer offset and must not be fixed.
 *
 * What the cases pin, as the code behaves today:
 *  - the phase-bias reaction seam (reset path and declared-step carry path) does not read the
 *    generation id: an epoch where only the id changes moves no ambiguity and no ledger, and a
 *    discontinuity bump in the same epoch is still counted as a carry;
 *  - the reset the generation reaction makes (StateManager::resetAmbiguity at the configured
 *    ambiguity sigma, once per key) keeps the value, raises the variance, marks the key for a
 *    re-seed and gives it a new ambiguity generation; the next measurement pre-pass re-seeds
 *    the value and clears the mark without a second generation;
 *  - under the default eligibility gate a key is eligible only through a .BIA entry: with no
 *    .BIA product every key is refused, whether the stream phase bias is applied or aged out;
 *  - with model_corrections.bias_coverage_requires_apply on, eligibility follows the stream
 *    bias through the same age limit the apply site uses (max age times the stale-hold
 *    factor), in both directions, with no refloat at either edge; with the age gate off an
 *    old bias stays eligible.
 *
 * The generation reaction itself (RealTimeEngine::reactToGenerationChange) is private and no
 * test access type the engine befriends calls it; driving it through processEpoch needs a
 * fully initialised engine with products.  Its cases, and the cases that need a counter the
 * code does not keep, are written as skips that name what is missing.
 *
 * The engine seam is reached through the access type the engine already befriends for the
 * phase-bias reactions, copied exactly from its original test file; the measurement pre-pass
 * through the shared model stack of the observation-equation tests.
 */

#include <gtest/gtest.h>

#include "filter/StateKeySchema.hpp"
#include "measurement/RowRules.hpp"
#include "products/ProductCache.hpp"
#include "realtime/RealTimeEngine.hpp"
#include "support/ModelStack.hpp"
#include "support/SsrStoreFixture.hpp"
#include "support/TestTempDir.hpp"   // per-process, per-test scratch directory

#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <system_error>
#include <vector>

namespace GnssProcessor {

// House friend-shim idiom: expose only the engine seams this suite needs.
struct PbHoldPresenceTestAccess {
    static void initialize(RealTimeEngine& e, const SsrCorrectionStore& store) {
        e.state_manager_ = std::make_unique<StateManager>(5, e.log_manager_, e.config_);
        e.ssr_store_override_ = &store;
    }
    static StateManager& states(RealTimeEngine& e) { return *e.state_manager_; }
    /// One epoch of the phase-bias reaction (the hold seam, then the configured one).
    static void react(RealTimeEngine& e, const PppObservation& obs) {
        const std::vector<RinexObservation> raw;
        RealTimeEngine::RtEpochContext ctx{raw};
        ctx.modeled_obs = {obs};
        ctx.current_time = obs.time;
        e.resetOnPhaseBiasDisc(ctx);
    }
};

namespace {

constexpr const char* kSat = "G05";   // fixture phase signals of a GPS satellite: L1C, L2W

/// Reason shared by every case that needs the generation reaction itself.
constexpr const char* kNoGenerationSeam =
    "RealTimeEngine::reactToGenerationChange is private, no test access type the engine "
    "befriends calls it, and processEpoch needs a fully initialised engine with products";

class RealtimeReactionGeneration : public ::testing::Test {
protected:
    LogManager log;

    // --- The engine half: the phase-bias reaction seam over a hand-built state -------------
    GnssConfig config;
    SsrCorrectionStore store;
    std::unique_ptr<RealTimeEngine> engine;
    gnss_test::SsrStoreState stream;   // what the store holds at the next engine epoch

    // --- The model half: the measurement pre-pass over the shared model stack --------------
    gnss_test::ModelStack stack;
    ClockRefMap clock_refs;            // empty: no case reads the process-wide product cache

    static std::string testName() {
        return ::testing::UnitTest::GetInstance()->current_test_info()->name();
    }

    // ----------------------------------------------------------------- engine half

    /// An engine with the bias age gate on (60 s, stale-hold factor 2), the declared-step
    /// carry as asked, two converged ambiguities of kSat (variance 0.01) and a stream that
    /// holds kSat as the network producer emits it, at generation 7.
    void buildEngine(bool carry) {
        const auto path = gnss_test::tempDir() / ("rrg_" + testName() + ".json");
        {
            std::ofstream out(path);
            out << R"({
                "processing": {
                    "mode": "ppp-rtk",
                    "active_systems": ["GPS"],
                    "enable_ssr_bias_max_age": true,
                    "ssr_bias_max_age_s": 60.0,
                    "ssr_phase_bias_disc_carry": )" << (carry ? "true" : "false") << R"(
                },
                "estimation": {
                    "estimate_velocity": false,
                    "estimate_phase_clock": false,
                    "estimate_ifb": false,
                    "estimate_sat_biases": false,
                    "ambiguity_resolution": {"mode": 0},
                    "between_satellite_sd": {"enabled": false}
                },
                "filter": {"initial_std": {
                    "position": 1.0, "clock": 1.0, "troposphere": 0.1,
                    "ionosphere": 1.0, "ambiguity": 100.0
                }}
            })";
        }
        const bool loaded = config.loadFromJson(path, log);
        std::error_code ec;
        std::filesystem::remove(path, ec);
        ASSERT_TRUE(loaded);
        ASSERT_TRUE(config.enableSsrBiasMaxAge());
        ASSERT_EQ(config.ssrPhaseBiasDiscCarry(), carry);
        engine = std::make_unique<RealTimeEngine>(config, log);
        PbHoldPresenceTestAccess::initialize(*engine, store);
        engineStates().addAmbiguity(ambKey("L1C"), 4.25, 0.1);
        engineStates().addAmbiguity(ambKey("L2W"), -7.5, 0.1);
        stream = gnss_test::networkSsrState({kSat}, 7, GnssTime{2313, 261600.0});
    }

    StateManager& engineStates() { return PbHoldPresenceTestAccess::states(*engine); }

    static std::string ambKey(const std::string& sig) { return StateKeys::makeAmbiguityKey(kSat, sig); }

    /// One engine epoch: the stream into the store, the phase-bias reaction seam on kSat at
    /// the stream epoch, then the stream moves 30 s on (every record re-stamped fresh).
    void engineStep() {
        gnss_test::writeSsrState(store, stream);
        PppObservation o{};
        o.satellite = kSat;
        o.system = System::GPS;
        o.time = stream.epoch;
        o.modeled["L1C"] = 2.2e7;
        o.modeled["L2W"] = 2.2e7;
        PbHoldPresenceTestAccess::react(*engine, o);
        gnss_test::advanceSsrEpoch(stream, 30.0);
    }

    double engineValue(const std::string& sig) {
        const int i = engineStates().getAmbiguityIndex(ambKey(sig));
        EXPECT_GE(i, 0);
        return i >= 0 ? engineStates().getStateVector()(i) : 0.0;
    }
    double engineVar(const std::string& sig) {
        const int i = engineStates().getAmbiguityIndex(ambKey(sig));
        EXPECT_GE(i, 0);
        return i >= 0 ? engineStates().getCovariance()(i, i) : 0.0;
    }

    // ----------------------------------------------------------------- model half

    /// The model stack in uncombined PPP-RTK with no .BIA product, the eligibility key and
    /// the bias age gate (60 s, stale-hold factor 2, so a bias applies up to 120 s) as asked.
    void loadStack(bool requires_apply, bool age_gate) {
        ProductCache::getInstance().clearCache();   // no .BIA entry from an earlier case
        const std::string processing =
            age_gate ? R"(, "enable_ssr_bias_max_age": true, "ssr_bias_max_age_s": 60.0)" : "";
        const std::string top =
            requires_apply ? R"(, "model_corrections": {"bias_coverage_requires_apply": true})" : "";
        ASSERT_TRUE(stack.load(log, "rrg_" + testName(), "ppp-rtk", processing, "", false, top));
        ASSERT_EQ(stack.config.getModelCorrections().bias_coverage_requires_apply, requires_apply);
        ASSERT_EQ(stack.config.enableSsrBiasMaxAge(), age_gate);
        ASSERT_DOUBLE_EQ(stack.config.getSsrBiasStaleHoldFactor(), 2.0);
        stack.model->setClockRefs(&clock_refs);
    }

    static PppObservation observation(int epoch) {
        return gnss_test::syntheticObservation(kSat, System::GPS, {"C1C", "L1C", "C2W", "L2W"}, epoch);
    }

    /// The store holds kSat as the network producer emits it, its phase-bias record
    /// @p phase_age_s old at @p t.
    void streamAt(const GnssTime& t, double phase_age_s) {
        gnss_test::SsrStoreState st = gnss_test::networkSsrState({kSat}, 0, t);
        ASSERT_TRUE(gnss_test::ageSatellite(st, kSat, phase_age_s, gnss_test::SsrRecordKind::PhaseBias));
        gnss_test::writeSsrState(stack.store, st);
    }

    /// The measurement pre-pass of one epoch (births, re-seeds, eligibility marks).
    void update(const PppObservation& obs) {
        stack.model->computeMeasurementUpdate({obs}, stack.states->getStateVector());
    }

    bool covered(const std::string& sig, const GnssTime& t) const {
        return phaseBiasCoveredForAr(RowProducts{stack.config, *stack.ppp, stack.store}, kSat, sig, t);
    }
    bool unresolvable(const std::string& sig) const {
        return stack.states->isAmbiguityUnresolvable(ambKey(sig));
    }
};

// ===========================================================================
// The phase-bias reaction seam in a generation-change epoch
// ===========================================================================

// Reset path: an epoch where only the generation id moves (7 -> 8; counters, values and
// ages unchanged) moves no ambiguity.  The id is read by the generation reaction alone.
// Fails if the reset path begins to read the generation id.
TEST_F(RealtimeReactionGeneration, GenerationOnlyEpochMovesNothingOnTheResetPath) {
    ASSERT_NO_FATAL_FAILURE(buildEngine(/*carry=*/false));
    engineStep();
    engineStep();
    const double v1 = engineValue("L1C"), v2 = engineValue("L2W");
    const auto g1 = engineStates().getAmbiguityGeneration(ambKey("L1C"));
    const auto g2 = engineStates().getAmbiguityGeneration(ambKey("L2W"));
    ASSERT_EQ(gnss_test::setGeneration(stream, 8), 8u);
    engineStep();
    ASSERT_EQ(store.getGenerationId().value_or(0), 8u);
    engineStep();
    EXPECT_EQ(engineValue("L1C"), v1);
    EXPECT_EQ(engineValue("L2W"), v2);
    EXPECT_NEAR(engineVar("L1C"), 0.01, 1e-12) << "no reset on the phase-bias seam";
    EXPECT_NEAR(engineVar("L2W"), 0.01, 1e-12);
    EXPECT_FALSE(engineStates().isAmbiguityPendingReseed(ambKey("L1C")));
    EXPECT_FALSE(engineStates().isAmbiguityPendingReseed(ambKey("L2W")));
    EXPECT_EQ(engineStates().getAmbiguityGeneration(ambKey("L1C")), g1);
    EXPECT_EQ(engineStates().getAmbiguityGeneration(ambKey("L2W")), g2);
}

// Carry path: the same epoch leaves the carry ledger without an event and the ambiguities
// untouched.  Fails if the carry body begins to read the generation id or clears its ledger
// on a generation change.
TEST_F(RealtimeReactionGeneration, GenerationOnlyEpochLeavesTheCarryLedgerIdle) {
    ASSERT_NO_FATAL_FAILURE(buildEngine(/*carry=*/true));
    engineStep();
    engineStep();
    const double v1 = engineValue("L1C"), v2 = engineValue("L2W");
    gnss_test::setGeneration(stream, 8);
    engineStep();
    engineStep();
    const auto& c = engine->pbDiscCarryCounters();
    EXPECT_EQ(c.events, 0u);
    EXPECT_EQ(c.disc_bumps, 0u);
    EXPECT_EQ(c.flips, 0u);
    EXPECT_EQ(c.resets(), 0u);
    EXPECT_EQ(engineValue("L1C"), v1);
    EXPECT_EQ(engineValue("L2W"), v2);
    EXPECT_NEAR(engineVar("L1C"), 0.01, 1e-12);
    EXPECT_NEAR(engineVar("L2W"), 0.01, 1e-12);
}

// A discontinuity bump and a 0.19 m step of the L1C bias in the epoch the generation id
// moves: the carry body counts a carried bump and shifts the ambiguity by the step, as on
// any epoch.  In the epoch driver the generation reaction has already reset this key; the
// carry body keeps no record of that, so the run-end counters report a carry of a key the
// re-seed will overwrite.  Fails once the carry skips (or attributes) a key the generation
// change reset in the same epoch, and only if that is decided inside the carry body.
TEST_F(RealtimeReactionGeneration, DiscBumpInAGenerationChangeEpochIsCountedAsACarry) {
    ASSERT_NO_FATAL_FAILURE(buildEngine(/*carry=*/true));
    engineStep();
    engineStep();
    const double before_l1 = engineValue("L1C");
    const double before_l2 = engineValue("L2W");
    gnss_test::setGeneration(stream, 8);
    ASSERT_EQ(gnss_test::bumpPhaseDiscontinuity(stream, kSat, "L1C"), 1);
    gnss_test::stepPhaseBias(stream, kSat, "L1C", 0.19);
    engineStep();
    const auto& c = engine->pbDiscCarryCounters();
    EXPECT_EQ(c.disc_bumps, 1u);
    EXPECT_EQ(c.events, 1u);
    EXPECT_EQ(c.disc_carried, 1u) << "the bump is counted as carried in a generation-change epoch";
    EXPECT_EQ(c.resets(), 0u);
    EXPECT_NEAR(engineValue("L1C"), before_l1 - 0.19, 1e-9) << "amb -= (applied now - applied before)";
    EXPECT_NEAR(engineVar("L1C"), 0.01, 1e-12) << "the carry keeps the covariance";
    EXPECT_EQ(engineValue("L2W"), before_l2);
}

// ===========================================================================
// The reset the generation reaction makes, and the re-seed that ends it
// ===========================================================================

// Starts from the call the generation reaction makes for every ambiguity key
// (StateManager::resetAmbiguity at the configured ambiguity sigma).  At the reset: value
// kept, variance sigma^2, key pending a re-seed, a new ambiguity generation (the hook that
// restarts the integer arc and the level-check window).  At the next pre-pass: the value is
// re-seeded, the mark cleared, the generation and the variance left as the reset set them.
// Fails if the reset or the re-seed changes any of these.
TEST_F(RealtimeReactionGeneration, ResetOfTheGenerationReactionEndsAtTheNextReseed) {
    ASSERT_NO_FATAL_FAILURE(loadStack(/*requires_apply=*/false, /*age_gate=*/false));
    update(observation(1));
    StateManager& sm = *stack.states;
    const std::vector<std::string> keys = {ambKey("L1C"), ambKey("L2W")};
    constexpr double kSentinel = 1234.5;
    Eigen::VectorXd x = sm.getStateVector();
    std::vector<std::uint64_t> born;
    for (const auto& k : keys) {
        const int i = sm.getAmbiguityIndex(k);
        ASSERT_GE(i, 0) << k << " is born by the first pre-pass";
        x(i) = kSentinel;
        born.push_back(sm.getAmbiguityGeneration(k));
    }
    sm.setStateVector(x);

    const double sigma = stack.config.getAmbiguityStd();
    ASSERT_DOUBLE_EQ(sigma, 100.0);
    std::vector<std::uint64_t> at_reset;
    for (std::size_t n = 0; n < keys.size(); ++n) {
        const auto& k = keys[n];
        ASSERT_TRUE(sm.resetAmbiguity(k, sigma));
        const int i = sm.getAmbiguityIndex(k);
        EXPECT_EQ(sm.getStateVector()(i), kSentinel) << "the reset keeps the value";
        EXPECT_NEAR(sm.getCovariance()(i, i), sigma * sigma, 1e-9 * sigma * sigma);
        EXPECT_TRUE(sm.isAmbiguityPendingReseed(k));
        EXPECT_GT(sm.getAmbiguityGeneration(k), born[n]) << "a reset is a new ambiguity generation";
        at_reset.push_back(sm.getAmbiguityGeneration(k));
    }

    update(observation(2));
    for (std::size_t n = 0; n < keys.size(); ++n) {
        const auto& k = keys[n];
        const int i = sm.getAmbiguityIndex(k);
        ASSERT_GE(i, 0);
        EXPECT_FALSE(sm.isAmbiguityPendingReseed(k)) << k << ": the pre-pass consumed the mark";
        EXPECT_NE(sm.getStateVector()(i), kSentinel) << k << ": the value is re-seeded";
        EXPECT_TRUE(std::isfinite(sm.getStateVector()(i)));
        EXPECT_EQ(sm.getAmbiguityGeneration(k), at_reset[n]) << k << ": the re-seed is no new generation";
        EXPECT_NEAR(sm.getCovariance()(i, i), sigma * sigma, 1e-9 * sigma * sigma)
            << k << ": the re-seed writes the value only";
    }
}

// ===========================================================================
// The generation reaction itself: not reachable, or needing what the code lacks
// ===========================================================================

// A change 7 -> 8 with two ambiguities: one change counted and two resets counted.
TEST_F(RealtimeReactionGeneration, GenerationChangeIsCountedWithItsResets) {
    GTEST_SKIP() << "The generation change has no counter and no count of the ambiguities it "
                    "resets (one warn line only); also " << kNoGenerationSeam << ".";
}

// The first id seen (0 -> 7) is a first receipt, not a change: nothing is reset.
TEST_F(RealtimeReactionGeneration, FirstGenerationIdIsNotAChange) {
    GTEST_SKIP() << "Cannot be driven: " << kNoGenerationSeam
                 << "; the code has no generation-change counter to read either.";
}

// The epoch after a change re-seeds both keys, counted by cause.
TEST_F(RealtimeReactionGeneration, ReseedsAfterAGenerationChangeAreCountedByCause) {
    GTEST_SKIP() << "Re-seeds are not counted (a debug line per key only) and carry no cause; "
                    "the re-seed itself is pinned by ResetOfTheGenerationReactionEndsAtTheNextReseed.";
}

// A generation change and a disc bump on one key in one epoch, carry armed: the key is
// attributed to the generation reset and not counted as a carry.
TEST_F(RealtimeReactionGeneration, GenerationResetIsAttributedBeforeTheCarry) {
    GTEST_SKIP() << "No per-key record of a generation reset reaches the carry body and no "
                    "reset-by-generation counter exists; today the bump is counted as carried "
                    "(DiscBumpInAGenerationChangeEpochIsCountedAsACarry).";
}

// What the generation change leaves: inter-system biases, receiver-satellite code biases,
// satellite biases, ionosphere and the reaction memories (applied flags, carry and translate
// ledgers, level-step memory); what it raises: receiver code-bias variances to 0.25 m^2.
TEST_F(RealtimeReactionGeneration, GenerationChangeScopeOfResetAndKeptStates) {
    GTEST_SKIP() << "Cannot be driven: " << kNoGenerationSeam << ".";
}

// ===========================================================================
// Integer-fixing eligibility of a key without an applied phase bias
// ===========================================================================

// Default gate (bias_coverage_requires_apply off), no .BIA product: every key is refused,
// with the stream bias applied (age 0) and with it aged out (200 s, past the 120 s limit)
// alike.  The default gate reads .BIA presence only and never the stream.  Fails if the
// default gate begins to follow the stream.
TEST_F(RealtimeReactionGeneration, DefaultGateRefusesEveryKeyWithoutABiaProduct) {
    ASSERT_NO_FATAL_FAILURE(loadStack(/*requires_apply=*/false, /*age_gate=*/true));
    const double max_age = stack.config.getSsrBiasMaxAge() * stack.config.getSsrBiasStaleHoldFactor();
    ASSERT_DOUBLE_EQ(max_age, 120.0);

    const PppObservation fresh = observation(1);
    ASSERT_NO_FATAL_FAILURE(streamAt(fresh.time, 0.0));
    ASSERT_TRUE(stack.store.getPhaseBias(kSat, "L1C", fresh.time, max_age).has_value())
        << "the stream bias is applied at this epoch";
    EXPECT_FALSE(covered("L1C", fresh.time));
    EXPECT_FALSE(covered("L2W", fresh.time));
    update(fresh);
    ASSERT_GE(stack.states->getAmbiguityIndex(ambKey("L1C")), 0);
    EXPECT_TRUE(unresolvable("L1C")) << "an applied stream bias does not make the key eligible";
    EXPECT_TRUE(unresolvable("L2W"));

    const PppObservation aged = observation(2);
    ASSERT_NO_FATAL_FAILURE(streamAt(aged.time, 200.0));
    ASSERT_FALSE(stack.store.getPhaseBias(kSat, "L1C", aged.time, max_age).has_value())
        << "the stream bias is aged out at this epoch";
    EXPECT_FALSE(covered("L1C", aged.time));
    update(aged);
    EXPECT_TRUE(unresolvable("L1C"));
    EXPECT_TRUE(unresolvable("L2W"));
}

// The default gate with a .BIA product and an aged-out stream bias: the key stays eligible,
// counted as eligible without an applied phase bias.
TEST_F(RealtimeReactionGeneration, EligibleKeysWithoutAnAppliedPhaseBiasAreCounted) {
    GTEST_SKIP() << "No counter or identity of eligible keys without an applied phase bias "
                    "exists; the .BIA-present input also needs a bias product loaded into the "
                    "process-wide product cache, which no unit seam provides.";
}

// Key on, age gate on: eligibility follows the stream bias through the apply limit
// (60 s x 2).  Age 0 and 90 s (inside the stale hold): eligible.  200 s: refused.  Back to
// age 0: eligible again.  Neither edge resets, re-marks or re-validates the ambiguity: no
// pending re-seed and no new ambiguity generation.  Fails if eligibility uses the bare max
// age, stops following the stream, or an edge starts to refloat the key.
TEST_F(RealtimeReactionGeneration, ApplyKeyFollowsTheAgeGatedStreamAtBothEdges) {
    ASSERT_NO_FATAL_FAILURE(loadStack(/*requires_apply=*/true, /*age_gate=*/true));

    const PppObservation e1 = observation(1);
    ASSERT_NO_FATAL_FAILURE(streamAt(e1.time, 0.0));
    EXPECT_TRUE(covered("L1C", e1.time));
    update(e1);
    ASSERT_GE(stack.states->getAmbiguityIndex(ambKey("L1C")), 0);
    EXPECT_FALSE(unresolvable("L1C"));
    EXPECT_FALSE(unresolvable("L2W"));
    const auto gen = stack.states->getAmbiguityGeneration(ambKey("L1C"));

    const PppObservation e2 = observation(2);
    ASSERT_NO_FATAL_FAILURE(streamAt(e2.time, 90.0));
    EXPECT_TRUE(covered("L1C", e2.time)) << "90 s is inside the stale hold of 120 s";
    update(e2);
    EXPECT_FALSE(unresolvable("L1C"));

    const PppObservation e3 = observation(3);
    ASSERT_NO_FATAL_FAILURE(streamAt(e3.time, 200.0));
    EXPECT_FALSE(covered("L1C", e3.time));
    update(e3);
    EXPECT_TRUE(unresolvable("L1C")) << "the aged-out bias makes the key ineligible";
    EXPECT_TRUE(unresolvable("L2W"));
    EXPECT_FALSE(stack.states->isAmbiguityPendingReseed(ambKey("L1C"))) << "the decoupling edge is no reset";
    EXPECT_EQ(stack.states->getAmbiguityGeneration(ambKey("L1C")), gen);

    const PppObservation e4 = observation(4);
    ASSERT_NO_FATAL_FAILURE(streamAt(e4.time, 0.0));
    EXPECT_TRUE(covered("L1C", e4.time));
    update(e4);
    EXPECT_FALSE(unresolvable("L1C")) << "eligible again at the next pre-pass";
    EXPECT_FALSE(unresolvable("L2W"));
    EXPECT_FALSE(stack.states->isAmbiguityPendingReseed(ambKey("L1C"))) << "the re-coupling edge is no refloat";
    EXPECT_EQ(stack.states->getAmbiguityGeneration(ambKey("L1C")), gen);
}

// Key on, age gate off: the stream read has no age limit, so a bias an hour old keeps the key
// eligible (the apply site applies it too).  Fails if an age limit is imposed without the
// gate.
TEST_F(RealtimeReactionGeneration, ApplyKeyWithoutTheAgeGateKeepsAnOldBiasEligible) {
    ASSERT_NO_FATAL_FAILURE(loadStack(/*requires_apply=*/true, /*age_gate=*/false));
    const PppObservation obs = observation(1);
    ASSERT_NO_FATAL_FAILURE(streamAt(obs.time, 3600.0));
    EXPECT_TRUE(covered("L1C", obs.time));
    EXPECT_TRUE(covered("L2W", obs.time));
    update(obs);
    EXPECT_FALSE(unresolvable("L1C"));
    EXPECT_FALSE(unresolvable("L2W"));
}

} // namespace
} // namespace GnssProcessor
