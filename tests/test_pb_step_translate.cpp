/**
 * @file test_pb_step_translate.cpp
 * @brief The consumer translates every phase-bias product step into its own
 *        ambiguity (@c PbStepTranslateLedger and the
 *        @c network.closed_loop.pb_step_translate plumbing).
 *
 * Covered here:
 *   - a 0.30 m product step translates the (sat, signal) ambiguity by exactly
 *     −0.30 m, with the covariance bitwise unchanged and no other state touched.
 *     The undifferenced consumer's ambiguity state is in metres: the phase row in
 *     MeasurementModel::buildJacobianRow uses @c amb_term = x(amb_idx) with a unit
 *     Jacobian beside a metres @c ssr_bias_term, so "−Δ/λ cycles" and "−Δ metres"
 *     are the same write.  The basis where λ would appear (NL/WL) is one of the
 *     refusal branches below;
 *   - a 2 mm change is skipped (nothing written, counted as skipped_eps), and a
 *     declared 2 mm change is skipped too: that is the empty-declaration
 *     population CST §7.1 measured at 117 of 390;
 *   - an undeclared step is translated in the same way as a declared one (the EV6
 *     population: 451 of 545 de-commoned steps > 0.10 m arrive undeclared);
 *   - with both knobs armed, a declared step is translated once: the engine's
 *     reaction dispatches the translation first and returns, so the declared-step carry
 *     seam is not entered and its counters stay 0;
 *   - the refusal ladder: a declared cell falls back to the existing reset with
 *     its own counter (first sighting, NL-WL basis, BSSD, unknown key), while an
 *     undeclared one is a counted no-op.  The reset path does nothing for an
 *     undeclared cell, and a reset on every sub-threshold move would add a new,
 *     harmful reaction;
 *   - the counter closure over a mixed sequence;
 *   - the config plumbing: defaults off, the harness knob reaches only the
 *     derived consumer, a standalone consumer sets the processing key, and the
 *     eps travels with it.
 *
 * The engine-level half (the per-key sweep, the applied-value read, the CST
 * `ev_pb` kinds) needs a full engine and product stream and is validated on
 * field and sim arms, as for the declared-step carry.
 */

#include <gtest/gtest.h>

#include "config/GnssConfig.hpp"
#include "config/GnssConfigTypes.hpp"
#include "filter/StateManager.hpp"
#include "logging/LogManager.hpp"
#include "realtime/PbDiscCarryLedger.hpp"
#include "realtime/PbStepTranslateLedger.hpp"
#include "realtime/RealTimeEngine.hpp"

#include <Eigen/Dense>

#include <cmath>
#include <filesystem>
#include <fstream>
#include <string>
#include "support/TestTempDir.hpp"   // per-process, per-test scratch directory

namespace fs = std::filesystem;
using namespace GnssProcessor;

namespace {

constexpr double kResetStd = 100.0;   // the reaction's ambiguity_std stand-in
constexpr double kEps      = 0.005;   // the knob's default dead band (m)

/// A StateManager with a core block + two undifferenced ambiguities and a dense
/// (fully correlated) covariance, the shape a converged consumer carries.
struct TranslateBed {
    LogManager   lm;
    GnssConfig   cfg;
    StateManager sm{ 5, lm, cfg };
    Eigen::Index i32{ -1 }, i12{ -1 };

    TranslateBed() {
        i32 = static_cast<Eigen::Index>(sm.addAmbiguity("G32_L1W", 7.125, 10.0));
        i12 = static_cast<Eigen::Index>(sm.addAmbiguity("G12_L2W", -2.5, 10.0));
        const Eigen::Index n = static_cast<Eigen::Index>(sm.getStateSize());
        Eigen::MatrixXd A = Eigen::MatrixXd::Zero(n, n);
        for (Eigen::Index r = 0; r < n; ++r)
            for (Eigen::Index c = 0; c < n; ++c)
                A(r, c) = 0.05 * std::sin(1.0 + 0.7 * r + 1.3 * c) + (r == c ? 1.0 : 0.0);
        sm.setCovariance(A * A.transpose() + 0.01 * Eigen::MatrixXd::Identity(n, n));
    }
};

bool bitwiseEqual(const Eigen::MatrixXd& a, const Eigen::MatrixXd& b) {
    if (a.rows() != b.rows() || a.cols() != b.cols()) return false;
    for (Eigen::Index i = 0; i < a.rows(); ++i)
        for (Eigen::Index j = 0; j < a.cols(); ++j)
            if (!(a(i, j) == b(i, j))) return false;
    return true;
}

bool bitwiseEqualExcept(const Eigen::VectorXd& a, const Eigen::VectorXd& b, Eigen::Index skip) {
    if (a.size() != b.size()) return false;
    for (Eigen::Index i = 0; i < a.size(); ++i)
        if (i != skip && !(a(i) == b(i))) return false;
    return true;
}

/// The reset's signature: row/column zero except the diagonal at std².
void expectResetSignature(const StateManager& sm, Eigen::Index idx, double std_m) {
    const Eigen::MatrixXd P = sm.getCovariance();
    for (Eigen::Index k = 0; k < P.rows(); ++k) {
        if (k == idx) continue;
        EXPECT_EQ(P(idx, k), 0.0) << "row " << k;
        EXPECT_EQ(P(k, idx), 0.0) << "col " << k;
    }
    EXPECT_DOUBLE_EQ(P(idx, idx), std_m * std_m);
}

fs::path writeTempConfig(const std::string& tag, const std::string& body) {
    const fs::path p = gnss_test::tempDir() / ("pbt_step_translate_" + tag + ".json");
    std::ofstream f(p);
    f << body;
    f.close();
    return p;
}

/// A loadable network config with the closed-loop block under test.
std::string networkConfig(const std::string& closed_loop_extra) {
    return std::string("{\n") +
        "  \"processing\": {\n"
        "    \"mode\": \"ppp-rtk-network\",\n"
        "    \"active_systems\": [ \"GPS\" ],\n"
        "    \"signal_priorities\": { \"GPS\": \"WC\" }\n"
        "  },\n"
        "  \"network\": {\n"
        "    \"clock_datum_station\": \"AAAA\",\n"
        "    \"closed_loop\": { \"enable\": false" + closed_loop_extra + " },\n"
        "    \"stations\": [\n"
        "      { \"name\": \"AAAA\", \"position_ecef\": [ -2493304.99, -4655214.79, 3565497.67 ] },\n"
        "      { \"name\": \"BBBB\", \"position_ecef\": [ -2281621.75, -1453585.08, 5756964.89 ] }\n"
        "    ]\n"
        "  }\n"
        "}\n";
}

}  // namespace

// ---------------------------------------------------------------------------
// The translation itself.
// ---------------------------------------------------------------------------
TEST(PbStepTranslateTest, ThirtyCentimetreStepTranslatesExactlyAndLeavesVarianceUntouched) {
    TranslateBed b;
    PbStepTranslateLedger led;
    // k−1: the model applied 1.000 m.
    led.record("G32|L1W", true, 1.000);
    const Eigen::VectorXd x0 = b.sm.getStateVector();
    const Eigen::MatrixXd P0 = b.sm.getCovariance();

    // k: it applies 1.300 m, a 0.30 m product step that is not declared.
    const auto d = led.react(b.sm, "G32|L1W", "G32_L1W", /*declared=*/false,
                             /*applied_now=*/true, 1.300,
                             /*basis_nlwl=*/false, /*bssd=*/false, kEps, kResetStd);

    EXPECT_EQ(d.action, PbStepTranslateLedger::Action::TranslateUndeclared);
    EXPECT_DOUBLE_EQ(d.delta_m, 0.300);
    const Eigen::VectorXd x1 = b.sm.getStateVector();
    // Exactly −0.30 m on the one state (the undifferenced ambiguity state is in
    // metres with a unit Jacobian; see the file banner).
    EXPECT_DOUBLE_EQ(x1(b.i32), x0(b.i32) - 0.300);
    EXPECT_TRUE(bitwiseEqualExcept(x0, x1, b.i32));               // nothing else moved
    EXPECT_TRUE(bitwiseEqual(P0, b.sm.getCovariance()));          // variance untouched, bit for bit

    const auto& c = led.counters();
    EXPECT_EQ(c.cells, 1u);
    EXPECT_EQ(c.translate_undeclared, 1u);
    EXPECT_EQ(c.translate_declared, 0u);
    EXPECT_DOUBLE_EQ(c.sum_abs_translate_m, 0.300);
    EXPECT_DOUBLE_EQ(c.max_abs_translate_m, 0.300);
    EXPECT_TRUE(c.closureHolds());
}

TEST(PbStepTranslateTest, DeclaredStepTranslatesTheSameWayAndIsCountedSeparately) {
    TranslateBed b;
    PbStepTranslateLedger led;
    led.record("G12|L2W", true, -4.000);
    const Eigen::VectorXd x0 = b.sm.getStateVector();
    const Eigen::MatrixXd P0 = b.sm.getCovariance();

    const auto d = led.react(b.sm, "G12|L2W", "G12_L2W", /*declared=*/true,
                             true, 1.000, false, false, kEps, kResetStd);

    EXPECT_EQ(d.action, PbStepTranslateLedger::Action::TranslateDeclared);
    EXPECT_DOUBLE_EQ(d.delta_m, 5.000);
    EXPECT_DOUBLE_EQ(b.sm.getStateVector()(b.i12), x0(b.i12) - 5.000);
    EXPECT_TRUE(bitwiseEqual(P0, b.sm.getCovariance()));          // no re-inflation, unlike the reset
    EXPECT_EQ(led.counters().translate_declared, 1u);
    EXPECT_EQ(led.counters().translate_undeclared, 0u);
    EXPECT_TRUE(led.counters().closureHolds());
}

TEST(PbStepTranslateTest, TwoMillimetreChangeIsSkippedDeclaredOrNot) {
    // Undeclared: below the band, nothing is written.
    {
        TranslateBed b;
        PbStepTranslateLedger led;
        led.record("G32|L1W", true, 1.000);
        const Eigen::VectorXd x0 = b.sm.getStateVector();
        const Eigen::MatrixXd P0 = b.sm.getCovariance();
        const auto d = led.react(b.sm, "G32|L1W", "G32_L1W", false, true, 1.002,
                                 false, false, kEps, kResetStd);
        EXPECT_EQ(d.action, PbStepTranslateLedger::Action::SkippedEps);
        EXPECT_TRUE(bitwiseEqualExcept(x0, b.sm.getStateVector(), -1));   // nothing moved
        EXPECT_TRUE(bitwiseEqual(P0, b.sm.getCovariance()));
        EXPECT_EQ(led.counters().skipped_eps, 1u);
        EXPECT_EQ(led.counters().skipped_eps_declared, 0u);
        EXPECT_DOUBLE_EQ(led.counters().sum_abs_translate_m, 0.0);
        EXPECT_TRUE(led.counters().closureHolds());
    }
    // Declared and 2 mm: the empty declaration (117 of 390 in CST §7.1).  It costs
    // nothing: no translation, and not the reset that resetOnPhaseBiasDisc takes.
    {
        TranslateBed b;
        PbStepTranslateLedger led;
        led.record("G32|L1W", true, 1.000);
        const Eigen::VectorXd x0 = b.sm.getStateVector();
        const Eigen::MatrixXd P0 = b.sm.getCovariance();
        const auto d = led.react(b.sm, "G32|L1W", "G32_L1W", /*declared=*/true, true, 1.002,
                                 false, false, kEps, kResetStd);
        EXPECT_EQ(d.action, PbStepTranslateLedger::Action::SkippedEps);
        EXPECT_TRUE(bitwiseEqualExcept(x0, b.sm.getStateVector(), -1));
        EXPECT_TRUE(bitwiseEqual(P0, b.sm.getCovariance()));       // no re-inflation on an empty declaration
        EXPECT_EQ(led.counters().skipped_eps, 1u);
        EXPECT_EQ(led.counters().skipped_eps_declared, 1u);
        EXPECT_TRUE(led.counters().closureHolds());
    }
    // A change equal to the band is not a step either (the comparison is strict >).
    {
        TranslateBed b;
        PbStepTranslateLedger led;
        led.record("G32|L1W", true, 0.0);
        const auto d = led.react(b.sm, "G32|L1W", "G32_L1W", false, true, kEps,
                                 false, false, kEps, kResetStd);
        EXPECT_EQ(d.action, PbStepTranslateLedger::Action::SkippedEps);
    }
}

// ---------------------------------------------------------------------------
// Once, and only once, with both knobs armed.
// ---------------------------------------------------------------------------
TEST(PbStepTranslateTest, DeclaredStepWithBothKnobsIsTranslatedExactlyOnce) {
    // The mechanism half: one react call makes at most one shift, so a declared
    // step moves the state by −Δ and not by −2Δ, and the declared-step ledger (which
    // the engine's dispatch does not reach while the translation is armed) is
    // left with empty counters.
    TranslateBed b;
    PbStepTranslateLedger led;
    PbDiscCarryLedger     carry;        // the seam the dispatch skips
    led.record("G32|L1W", true, 0.0);
    carry.record("G32|L1W", 0.0);       // the carry's memory exists and is simply not consulted
    const Eigen::VectorXd x0 = b.sm.getStateVector();

    const auto d = led.react(b.sm, "G32|L1W", "G32_L1W", /*declared=*/true, true, 0.300,
                             false, false, kEps, kResetStd);
    EXPECT_EQ(d.action, PbStepTranslateLedger::Action::TranslateDeclared);
    EXPECT_DOUBLE_EQ(b.sm.getStateVector()(b.i32), x0(b.i32) - 0.300);   // −Δ, not −2Δ
    EXPECT_EQ(carry.counters().events, 0u);
    EXPECT_EQ(carry.counters().disc_carried, 0u);

    // The dispatch half: an engine configured with both knobs reports both armed.
    // resetOnPhaseBiasDisc (RealTimeEngineSsrBreaks.cpp) branches on the
    // translation first and returns, so carryOnPhaseBiasDisc_ is not entered and
    // the carry counters of a live run stay 0; those counters are the run-end
    // evidence.
    LogManager lm;
    GnssConfig cfg;
    const fs::path p = writeTempConfig(
        "both", "{ \"processing\": { \"ssr_phase_bias_disc_carry\": true,"
                " \"ssr_phase_bias_step_translate\": true } }\n");
    ASSERT_TRUE(cfg.loadFromJson(p, lm));
    ASSERT_TRUE(cfg.ssrPhaseBiasDiscCarry());
    ASSERT_TRUE(cfg.ssrPhaseBiasStepTranslate());
    RealTimeEngine eng(cfg, lm);
    EXPECT_TRUE(eng.pbStepTranslateArmed());
    EXPECT_TRUE(eng.pbDiscCarryArmed());
    EXPECT_EQ(eng.pbStepTranslateCounters().cells, 0u);
    EXPECT_EQ(eng.pbDiscCarryCounters().events, 0u);
    std::error_code ec; fs::remove(p, ec);
}

// ---------------------------------------------------------------------------
// The refusal ladder: declared ⇒ the existing reset; undeclared ⇒ a counted no-op.
// ---------------------------------------------------------------------------
TEST(PbStepTranslateTest, NoPairIsAResetWhenDeclaredAndANoOpWhenNot) {
    // First sighting (no record at all) + declared ⇒ the existing reset.
    {
        TranslateBed b;
        PbStepTranslateLedger led;
        const auto d = led.react(b.sm, "G32|L1W", "G32_L1W", true, true, 1.0,
                                 false, false, kEps, kResetStd);
        EXPECT_EQ(d.action, PbStepTranslateLedger::Action::ResetFirstSight);
        expectResetSignature(b.sm, b.i32, kResetStd);
        EXPECT_EQ(led.counters().reset_first_sight, 1u);
    }
    // k−1 present but not applied (the A5 freshness edge) + declared ⇒ reset.
    {
        TranslateBed b;
        PbStepTranslateLedger led;
        led.record("G32|L1W", /*applied_now=*/false, 0.0);
        const auto d = led.react(b.sm, "G32|L1W", "G32_L1W", true, true, 1.0,
                                 false, false, kEps, kResetStd);
        EXPECT_EQ(d.action, PbStepTranslateLedger::Action::ResetFirstSight);
        expectResetSignature(b.sm, b.i32, kResetStd);
    }
    // Not applied at k (the bias aged out) + declared ⇒ reset, and no translation
    // to a value the model does not apply.
    {
        TranslateBed b;
        PbStepTranslateLedger led;
        led.record("G32|L1W", true, 1.0);
        const auto d = led.react(b.sm, "G32|L1W", "G32_L1W", true, /*applied_now=*/false, 0.0,
                                 false, false, kEps, kResetStd);
        EXPECT_EQ(d.action, PbStepTranslateLedger::Action::ResetFirstSight);
        expectResetSignature(b.sm, b.i32, kResetStd);
    }
    // The same shape undeclared: nothing written, counted as no_pair.
    {
        TranslateBed b;
        PbStepTranslateLedger led;
        const Eigen::VectorXd x0 = b.sm.getStateVector();
        const Eigen::MatrixXd P0 = b.sm.getCovariance();
        const auto d = led.react(b.sm, "G32|L1W", "G32_L1W", false, true, 1.0,
                                 false, false, kEps, kResetStd);
        EXPECT_EQ(d.action, PbStepTranslateLedger::Action::None);
        EXPECT_TRUE(bitwiseEqualExcept(x0, b.sm.getStateVector(), -1));
        EXPECT_TRUE(bitwiseEqual(P0, b.sm.getCovariance()));
        EXPECT_EQ(led.counters().no_pair, 1u);
        EXPECT_TRUE(led.counters().closureHolds());
    }
}

TEST(PbStepTranslateTest, StructuralRefusalsResetOnlyWhenDeclared) {
    struct Case { bool nlwl; bool bssd; const char* amb; PbStepTranslateLedger::Action want; };
    const Case cases[] = {
        { true,  false, "G32_L1W", PbStepTranslateLedger::Action::ResetBasis },
        { false, true,  "G32_L1W", PbStepTranslateLedger::Action::ResetBssd },
        { false, false, "G99_L1W", PbStepTranslateLedger::Action::ResetUnknownKey },
    };
    for (const auto& c : cases) {
        // Declared: the existing reset, with its own counter.
        TranslateBed b;
        PbStepTranslateLedger led;
        led.record("G32|L1W", true, 0.0);
        const auto d = led.react(b.sm, "G32|L1W", c.amb, /*declared=*/true, true, 0.500,
                                 c.nlwl, c.bssd, kEps, kResetStd);
        EXPECT_EQ(d.action, c.want);
        if (c.want != PbStepTranslateLedger::Action::ResetUnknownKey)
            expectResetSignature(b.sm, b.i32, kResetStd);   // the unknown key has no column to check
        EXPECT_EQ(led.counters().resets(), 1u);
        EXPECT_TRUE(led.counters().closureHolds());

        // Undeclared: a counted no-op.  The reset path does nothing here, and a
        // reset would make a BSSD consumer reset on every sub-threshold product move.
        TranslateBed b2;
        PbStepTranslateLedger led2;
        led2.record("G32|L1W", true, 0.0);
        const Eigen::VectorXd x0 = b2.sm.getStateVector();
        const Eigen::MatrixXd P0 = b2.sm.getCovariance();
        const auto d2 = led2.react(b2.sm, "G32|L1W", c.amb, /*declared=*/false, true, 0.500,
                                   c.nlwl, c.bssd, kEps, kResetStd);
        EXPECT_EQ(d2.action, PbStepTranslateLedger::Action::RefusedUndeclared);
        EXPECT_TRUE(bitwiseEqualExcept(x0, b2.sm.getStateVector(), -1));
        EXPECT_TRUE(bitwiseEqual(P0, b2.sm.getCovariance()));
        EXPECT_EQ(led2.counters().resets(), 0u);
        EXPECT_EQ(led2.counters().refused_undeclared, 1u);
        EXPECT_TRUE(led2.counters().closureHolds());
    }
}

TEST(PbStepTranslateTest, CounterClosureHoldsOverAMixedSequence) {
    TranslateBed b;
    PbStepTranslateLedger led;
    // A first sighting (declared ⇒ reset), then a real step, a tiny one, an
    // undeclared step, an undeclared first sighting of G07 (no_pair) and a
    // declared not-applied epoch on G07 (reset).
    led.react(b.sm, "G32|L1W", "G32_L1W", true, true, 1.0, false, false, kEps, kResetStd);
    led.record("G32|L1W", true, 1.0);
    led.react(b.sm, "G32|L1W", "G32_L1W", true, true, 1.4, false, false, kEps, kResetStd);
    led.record("G32|L1W", true, 1.4);
    led.react(b.sm, "G32|L1W", "G32_L1W", true, true, 1.401, false, false, kEps, kResetStd);
    led.record("G32|L1W", true, 1.401);
    led.react(b.sm, "G32|L1W", "G32_L1W", false, true, 2.0, false, false, kEps, kResetStd);
    led.record("G32|L1W", true, 2.0);
    led.react(b.sm, "G07|L1W", "G07_L1W", false, true, 1.0, false, false, kEps, kResetStd);
    led.record("G07|L1W", true, 1.0);
    led.react(b.sm, "G07|L1W", "G07_L1W", true, false, 0.0, false, false, kEps, kResetStd);

    const auto& c = led.counters();
    EXPECT_EQ(c.cells, 6u);
    EXPECT_EQ(c.declared_seen, 4u);
    EXPECT_EQ(c.translate_declared, 1u);
    EXPECT_EQ(c.translate_undeclared, 1u);
    EXPECT_EQ(c.skipped_eps, 1u);
    EXPECT_EQ(c.skipped_eps_declared, 1u);
    EXPECT_EQ(c.reset_first_sight, 2u);   // the very first cell, and the not-applied one
    EXPECT_EQ(c.no_pair, 1u);
    EXPECT_TRUE(c.closureHolds());
    EXPECT_NEAR(c.sum_abs_translate_m, 0.400 + 0.599, 1e-12);
}

// ---------------------------------------------------------------------------
// Config plumbing.
// ---------------------------------------------------------------------------
TEST(PbStepTranslateTest, ConfigDefaultsAreOffAndTheKnobReachesOnlyTheDerivedConsumer) {
    {
        GnssConfig d;
        EXPECT_FALSE(d.ssrPhaseBiasStepTranslate());
        EXPECT_DOUBLE_EQ(d.getSsrPhaseBiasStepTranslateEpsM(), 0.005);
        EXPECT_FALSE(d.getNetworkConfig().closed_loop.pb_step_translate);
        EXPECT_DOUBLE_EQ(d.getNetworkConfig().closed_loop.pb_step_translate_eps_m, 0.005);
    }
    {
        LogManager log;
        GnssConfig cfg;
        const fs::path p = writeTempConfig(
            "harness", networkConfig(", \"pb_step_translate\": true,"
                                     " \"pb_step_translate_eps_m\": 0.02"));
        ASSERT_TRUE(cfg.loadFromJson(p, log));
        EXPECT_TRUE(cfg.getNetworkConfig().closed_loop.pb_step_translate);
        EXPECT_DOUBLE_EQ(cfg.getNetworkConfig().closed_loop.pb_step_translate_eps_m, 0.02);
        EXPECT_FALSE(cfg.ssrPhaseBiasStepTranslate());                   // the producer does not translate
        const GnssConfig consumer = cfg.deriveClosedLoopConsumerConfig();
        EXPECT_TRUE(consumer.ssrPhaseBiasStepTranslate());               // the consumer does
        EXPECT_DOUBLE_EQ(consumer.getSsrPhaseBiasStepTranslateEpsM(), 0.02);   // …with the harness eps
        std::error_code ec; fs::remove(p, ec);
    }
    {
        LogManager log;
        GnssConfig cfg;
        const fs::path p = writeTempConfig("off", networkConfig(""));
        ASSERT_TRUE(cfg.loadFromJson(p, log));
        EXPECT_FALSE(cfg.deriveClosedLoopConsumerConfig().ssrPhaseBiasStepTranslate());
        std::error_code ec; fs::remove(p, ec);
    }
    {
        LogManager log;
        GnssConfig cfg;
        const fs::path p = writeTempConfig(
            "standalone", "{ \"processing\": { \"ssr_phase_bias_step_translate\": true,"
                          " \"ssr_phase_bias_step_translate_eps_m\": 0.01 } }\n");
        ASSERT_TRUE(cfg.loadFromJson(p, log));
        EXPECT_TRUE(cfg.ssrPhaseBiasStepTranslate());
        EXPECT_DOUBLE_EQ(cfg.getSsrPhaseBiasStepTranslateEpsM(), 0.01);
        std::error_code ec; fs::remove(p, ec);
    }
}

TEST(PbStepTranslateTest, ActionNamesAreTheCstTraceKinds) {
    using A = PbStepTranslateLedger::Action;
    EXPECT_STREQ(PbStepTranslateLedger::actionName(A::TranslateDeclared), "translate_disc");
    EXPECT_STREQ(PbStepTranslateLedger::actionName(A::TranslateUndeclared), "translate_undeclared");
    EXPECT_STREQ(PbStepTranslateLedger::actionName(A::SkippedEps), "translate_skip_eps");
    EXPECT_STREQ(PbStepTranslateLedger::actionName(A::RefusedUndeclared), "translate_refused");
    EXPECT_STREQ(PbStepTranslateLedger::actionName(A::ResetBssd), "reset_bssd");
}
