/**
 * @file test_consumer_disc_carry.cpp
 * @brief Card CC — the closed-loop consumer's continuity: (A) the declared
 *        phase-bias-step ambiguity CARRY (StateManager::shiftAmbiguity +
 *        PbDiscCarryLedger, the knob-ON twin of the reset reaction) and (B) the
 *        harness-side snapshot contract (the consumer file beside the network
 *        history copy; the refusals of RealTimeEngine::writeStateSnapshotTo).
 *
 * What is locked here (SPEC_event_harness §24.4, gate G0):
 *   - the shift is VALUE-ONLY: P bitwise unchanged, every other state bitwise
 *     unchanged, the key not marked pending re-seed; unknown keys refused
 *     exactly like resetAmbiguity;
 *   - a declared step is an exact TRANSLATION of the measurement update: with
 *     the applied bias moved by Δ and the ambiguity by −Δ the innovation is
 *     identical, so the posterior position and every other state agree with
 *     the no-step run to 1e-9 and the ambiguity posterior differs by exactly −Δ
 *     (the reset alternative, by contrast, moves the position);
 *   - every refusal branch of the ledger (first sighting, NL/WL basis, BSSD,
 *     unknown key) takes the EXISTING reset — covariance row/column zeroed,
 *     diagonal re-inflated, key pending re-seed — and increments its own
 *     counter; the closure carried + resets == events holds over a mixed
 *     sequence;
 *   - the config plumbing: processing.ssr_phase_bias_disc_carry loads directly,
 *     network.closed_loop.pb_disc_carry reaches the DERIVED consumer config and
 *     never the base, network.closed_loop.state_snapshot parses; defaults false;
 *   - (B) the consumer history file sits beside the network copy with the same
 *     .epNNNN suffix, and writeStateSnapshotTo refuses (writes nothing) on an
 *     engine that has processed no epoch.
 *
 * The engine-level halves (the seam's per-key sweep, the RESTORED rider, the
 * beside-the-copy write on a live producer) need a full engine + product
 * stream and are validated on the field harness (the v9 seed + G2/G3 legs),
 * exactly as the RT snapshot's own engine halves were (test_rt_state_snapshot).
 */

#include <gtest/gtest.h>

#include "config/GnssConfig.hpp"
#include "config/GnssConfigTypes.hpp"
#include "filter/StateManager.hpp"
#include "logging/LogManager.hpp"
#include "products/NetworkStateSnapshot.hpp"
#include "realtime/PbDiscCarryLedger.hpp"
#include "realtime/RealTimeEngine.hpp"
#include "realtime/RtStateSnapshot.hpp"

#include <Eigen/Dense>

#include <cmath>
#include <filesystem>
#include <fstream>
#include <limits>
#include <string>

namespace fs = std::filesystem;
using namespace GnssProcessor;

namespace {

constexpr double kResetStd = 100.0;   // the reaction's ambiguity_std stand-in

/// A StateManager with a core block + two undifferenced ambiguities and a dense
/// (fully correlated) covariance — the shape a converged consumer carries.
struct Bed {
    LogManager  lm;
    GnssConfig  cfg;
    StateManager sm{ 5, lm, cfg };
    std::size_t core{ 0 };
    Eigen::Index i24{ -1 }, i06{ -1 };

    Bed() {
        core = sm.getStateSize();
        i24 = static_cast<Eigen::Index>(sm.addAmbiguity("G24_L5X", 3.25, 10.0));
        i06 = static_cast<Eigen::Index>(sm.addAmbiguity("G06_L2W", -1.5, 10.0));
        const Eigen::Index n = static_cast<Eigen::Index>(sm.getStateSize());
        // Dense SPD covariance with every cross term populated.
        Eigen::MatrixXd A = Eigen::MatrixXd::Zero(n, n);
        for (Eigen::Index r = 0; r < n; ++r)
            for (Eigen::Index c = 0; c < n; ++c)
                A(r, c) = 0.05 * std::sin(1.0 + 0.7 * r + 1.3 * c) + (r == c ? 1.0 : 0.0);
        Eigen::MatrixXd P = A * A.transpose() + 0.01 * Eigen::MatrixXd::Identity(n, n);
        sm.setCovariance(P);
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

std::error_code& ec_ignore() {
    static std::error_code ec;
    return ec;
}

fs::path writeTempConfig(const std::string& tag, const std::string& body) {
    const fs::path p = fs::temp_directory_path() / ("cc_disc_carry_" + tag + ".json");
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
        "    \"active_systems\": [ \"GPS\", \"Galileo\" ],\n"
        "    \"signal_priorities\": { \"GPS\": \"WC\", \"Galileo\": \"CB\" }\n"
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
// (A) StateManager::shiftAmbiguity — value-only, exact, guarded.
// ---------------------------------------------------------------------------
TEST(ConsumerDiscCarryTest, ShiftAmbiguityIsValueOnlyAndExact) {
    Bed b;
    const Eigen::VectorXd x0 = b.sm.getStateVector();
    const Eigen::MatrixXd P0 = b.sm.getCovariance();
    const double delta = -0.735;

    ASSERT_TRUE(b.sm.shiftAmbiguity("G24_L5X", delta));

    const Eigen::VectorXd x1 = b.sm.getStateVector();
    EXPECT_EQ(x1(b.i24), x0(b.i24) + delta);                     // the one state, the exact sum
    EXPECT_TRUE(bitwiseEqualExcept(x0, x1, b.i24));               // nothing else moved
    EXPECT_TRUE(bitwiseEqual(P0, b.sm.getCovariance()));          // P untouched, bit for bit
    EXPECT_FALSE(b.sm.isAmbiguityPendingReseed("G24_L5X"));       // no re-seed
    EXPECT_EQ(b.sm.getAmbiguities().size(), 2u);                  // no state created / removed
}

TEST(ConsumerDiscCarryTest, ShiftAmbiguityRefusesUnknownEmptyAndNonFinite) {
    Bed b;
    const Eigen::VectorXd x0 = b.sm.getStateVector();
    const Eigen::MatrixXd P0 = b.sm.getCovariance();
    const std::size_t n0 = b.sm.getStateSize();

    EXPECT_FALSE(b.sm.shiftAmbiguity("E11_L1C", 1.0));   // unknown: never auto-created (resetAmbiguity's guard)
    EXPECT_FALSE(b.sm.shiftAmbiguity("", 1.0));
    EXPECT_FALSE(b.sm.shiftAmbiguity("G24_L5X", std::numeric_limits<double>::quiet_NaN()));
    EXPECT_FALSE(b.sm.shiftAmbiguity("G24_L5X", std::numeric_limits<double>::infinity()));

    EXPECT_EQ(b.sm.getStateSize(), n0);
    EXPECT_TRUE(bitwiseEqualExcept(x0, b.sm.getStateVector(), -1));
    EXPECT_TRUE(bitwiseEqual(P0, b.sm.getCovariance()));
    EXPECT_EQ(b.sm.getAmbiguityIndex("E11_L1C"), -1);
}

// ---------------------------------------------------------------------------
// (A) The algebra the carry rests on: a declared step is an exact translation
// of the measurement update.  Phase row: predicted = geometry + ssr_bias + amb
// (both +1, MeasurementModel.cpp); code row: geometry only.  Two satellites,
// state [pos(3), clk, zwd, N1, N2], a dense prior, one update.
// ---------------------------------------------------------------------------
TEST(ConsumerDiscCarryTest, DeclaredStepIsAnExactTranslationOfTheUpdate) {
    constexpr int n = 7;
    constexpr Eigen::Index N1 = 5;
    // Geometry (unit LOS + wet mapping) for two satellites.
    const Eigen::Vector3d e1(0.30, -0.40, 0.866), e2(-0.60, 0.20, 0.775);
    const double m1 = 1.15, m2 = 1.29;
    auto row = [&](const Eigen::Vector3d& e, double m, int amb) {
        Eigen::RowVectorXd h = Eigen::RowVectorXd::Zero(n);
        h.segment<3>(0) = -e.transpose(); h(3) = 1.0; h(4) = m;
        if (amb >= 0) h(amb) = 1.0;
        return h;
    };
    Eigen::MatrixXd H(4, n);
    H.row(0) = row(e1, m1, -1);   // code  sat 1
    H.row(1) = row(e1, m1, 5);    // phase sat 1
    H.row(2) = row(e2, m2, -1);   // code  sat 2
    H.row(3) = row(e2, m2, 6);    // phase sat 2
    Eigen::VectorXd Rv(4);
    Rv << 0.3 * 0.3, 0.003 * 0.003, 0.3 * 0.3, 0.003 * 0.003;
    const Eigen::MatrixXd R = Rv.asDiagonal();

    // Dense SPD prior.
    Eigen::MatrixXd A = Eigen::MatrixXd::Zero(n, n);
    for (int r = 0; r < n; ++r)
        for (int c = 0; c < n; ++c)
            A(r, c) = 0.1 * std::cos(0.5 + 0.9 * r + 1.7 * c) + (r == c ? 0.5 : 0.0);
    const Eigen::MatrixXd P = A * A.transpose() + 1e-4 * Eigen::MatrixXd::Identity(n, n);

    Eigen::VectorXd x(n);
    x << 0.12, -0.05, 0.31, 2.4, 0.11, 3.25, -1.5;   // prior (geometry-free residual frame)
    const double PB1 = 1.250;                        // applied phase bias, sat 1
    const double delta = 0.735;                      // the declared step: PB1 -> PB1 + delta

    // Observations: a fixed "truth" observable vector, independent of the datum.
    Eigen::VectorXd z(4);
    z << 0.9, 4.60, -0.2, -0.7;

    auto predicted = [&](const Eigen::VectorXd& xs, double pb1) {
        Eigen::VectorXd p = H * xs;
        p(1) += pb1;     // ssr_bias_term on sat 1's phase row (+1)
        return p;
    };
    auto update = [&](const Eigen::VectorXd& xs, const Eigen::MatrixXd& Ps, double pb1,
                      Eigen::VectorXd& xo, Eigen::MatrixXd& Po, Eigen::VectorXd& innov) {
        innov = z - predicted(xs, pb1);
        const Eigen::MatrixXd S = H * Ps * H.transpose() + R;
        const Eigen::MatrixXd K = Ps * H.transpose() * S.inverse();
        xo = xs + K * innov;
        Po = (Eigen::MatrixXd::Identity(n, n) - K * H) * Ps;
    };

    // Run A: no step.
    Eigen::VectorXd xa, va; Eigen::MatrixXd Pa;
    update(x, P, PB1, xa, Pa, va);

    // Run B: the bias moved by +delta AND the ambiguity carried by -delta
    // (the shift), covariance untouched.
    Eigen::VectorXd xb0 = x; xb0(N1) -= delta;
    Eigen::VectorXd xb, vb; Eigen::MatrixXd Pb;
    update(xb0, P, PB1 + delta, xb, Pb, vb);

    for (int i = 0; i < 4; ++i) EXPECT_NEAR(va(i), vb(i), 1e-12) << "innovation row " << i;
    for (int i = 0; i < n; ++i) {
        if (i == N1) continue;
        EXPECT_NEAR(xa(i), xb(i), 1e-9) << "state " << i;      // position, clock, ZWD, N2 unchanged
    }
    EXPECT_NEAR(xb(N1), xa(N1) - delta, 1e-9);                // the ambiguity moved by exactly -delta
    EXPECT_TRUE(bitwiseEqual(Pa, Pb));                          // the SAME covariance path, bit for bit

    // The reset alternative (what the carry replaces): P row/column zeroed,
    // diagonal at std², value kept — the innovation is no longer the no-step
    // innovation and the posterior position MOVES.
    Eigen::MatrixXd Pr = P;
    Pr.row(N1).setZero(); Pr.col(N1).setZero(); Pr(N1, N1) = kResetStd * kResetStd;
    Eigen::VectorXd xr, vr; Eigen::MatrixXd PR;
    update(x, Pr, PB1 + delta, xr, PR, vr);
    EXPECT_GT((xr.head<3>() - xa.head<3>()).norm(), 1e-4);
}

// ---------------------------------------------------------------------------
// (A) The ledger: carry on a recorded key, flips carried separately, one event
// for a bump+flip, no event => nothing.
// ---------------------------------------------------------------------------
TEST(ConsumerDiscCarryTest, LedgerCarriesADeclaredStepValueOnly) {
    Bed b;
    PbDiscCarryLedger led;
    using A = PbDiscCarryLedger::Action;

    led.record("G24|L5X", 1.250);           // the value the model applied at the last update
    const Eigen::VectorXd x0 = b.sm.getStateVector();
    const Eigen::MatrixXd P0 = b.sm.getCovariance();

    // A disc-counter bump, the new applied value 1.985 => delta +0.735, amb -= 0.735.
    auto d = led.react(b.sm, "G24|L5X", "G24_L5X", /*bump=*/true, /*flip=*/false, 1.985,
                       /*nlwl=*/false, /*bssd=*/false, kResetStd);
    EXPECT_EQ(d.action, A::CarriedDisc);
    EXPECT_NEAR(d.delta_m, 0.735, 1e-12);
    EXPECT_NEAR(b.sm.getStateVector()(b.i24), x0(b.i24) - 0.735, 1e-12);
    EXPECT_TRUE(bitwiseEqualExcept(x0, b.sm.getStateVector(), b.i24));
    EXPECT_TRUE(bitwiseEqual(P0, b.sm.getCovariance()));
    EXPECT_FALSE(b.sm.isAmbiguityPendingReseed("G24_L5X"));
    led.record("G24|L5X", 1.985);           // the update site

    // A freshness flip to "not applied" (0): delta = 0 - 1.985 => amb += 1.985.
    const double before_flip = b.sm.getStateVector()(b.i24);
    d = led.react(b.sm, "G24|L5X", "G24_L5X", false, true, 0.0, false, false, kResetStd);
    EXPECT_EQ(d.action, A::CarriedFlip);
    EXPECT_NEAR(d.delta_m, -1.985, 1e-12);
    EXPECT_NEAR(b.sm.getStateVector()(b.i24), before_flip + 1.985, 1e-12);
    EXPECT_TRUE(bitwiseEqual(P0, b.sm.getCovariance()));
    led.record("G24|L5X", 0.0);

    // A bump AND a flip in the same epoch (the bias returns re-realized): ONE
    // event, one translation spanning both, counted as a disc carry.
    const double before_both = b.sm.getStateVector()(b.i24);
    d = led.react(b.sm, "G24|L5X", "G24_L5X", true, true, 2.100, false, false, kResetStd);
    EXPECT_EQ(d.action, A::CarriedDisc);
    EXPECT_NEAR(d.delta_m, 2.100, 1e-12);
    EXPECT_NEAR(b.sm.getStateVector()(b.i24), before_both - 2.100, 1e-12);
    led.record("G24|L5X", 2.100);

    // No bump, no flip: nothing happens and nothing is counted as an event.
    const Eigen::VectorXd xq = b.sm.getStateVector();
    d = led.react(b.sm, "G24|L5X", "G24_L5X", false, false, 2.104, false, false, kResetStd);
    EXPECT_EQ(d.action, A::None);
    EXPECT_TRUE(bitwiseEqualExcept(xq, b.sm.getStateVector(), -1));

    const auto& c = led.counters();
    EXPECT_EQ(c.events, 3u);
    EXPECT_EQ(c.disc_bumps, 2u);
    EXPECT_EQ(c.flips, 2u);
    EXPECT_EQ(c.disc_carried, 2u);
    EXPECT_EQ(c.flip_carried, 1u);
    EXPECT_EQ(c.resets(), 0u);
    EXPECT_TRUE(c.closureHolds());
    ASSERT_TRUE(led.recorded("G24|L5X").has_value());
    EXPECT_DOUBLE_EQ(*led.recorded("G24|L5X"), 2.100);
    EXPECT_FALSE(led.recorded("G06|L2W").has_value());
}

// ---------------------------------------------------------------------------
// (A) Every refusal branch takes the EXISTING reset and increments its counter.
// ---------------------------------------------------------------------------
TEST(ConsumerDiscCarryTest, RefusalBranchesTakeTheResetAndCount) {
    using A = PbDiscCarryLedger::Action;

    // 1. First sighting: no recorded value for the key.
    {
        Bed b;
        PbDiscCarryLedger led;
        const double v = b.sm.getStateVector()(b.i24);
        const auto d = led.react(b.sm, "G24|L5X", "G24_L5X", true, false, 1.985,
                                 false, false, kResetStd);
        EXPECT_EQ(d.action, A::ResetFirstSight);
        expectResetSignature(b.sm, b.i24, kResetStd);
        EXPECT_TRUE(b.sm.isAmbiguityPendingReseed("G24_L5X"));
        EXPECT_EQ(b.sm.getStateVector()(b.i24), v);   // the reset keeps the value (re-seeded later)
        EXPECT_EQ(led.counters().reset_first_sight, 1u);
    }
    // 2. NL/WL state basis.
    {
        Bed b;
        PbDiscCarryLedger led;
        led.record("G24|L5X", 1.250);
        const auto d = led.react(b.sm, "G24|L5X", "G24_L5X", true, false, 1.985,
                                 /*nlwl=*/true, false, kResetStd);
        EXPECT_EQ(d.action, A::ResetBasis);
        expectResetSignature(b.sm, b.i24, kResetStd);
        EXPECT_TRUE(b.sm.isAmbiguityPendingReseed("G24_L5X"));
        EXPECT_EQ(led.counters().reset_basis, 1u);
    }
    // 3. BSSD consumer.
    {
        Bed b;
        PbDiscCarryLedger led;
        led.record("G24|L5X", 1.250);
        const auto d = led.react(b.sm, "G24|L5X", "G24_L5X", false, true, 0.0,
                                 false, /*bssd=*/true, kResetStd);
        EXPECT_EQ(d.action, A::ResetBssd);
        expectResetSignature(b.sm, b.i24, kResetStd);
        EXPECT_TRUE(b.sm.isAmbiguityPendingReseed("G24_L5X"));
        EXPECT_EQ(led.counters().reset_bssd, 1u);
        EXPECT_EQ(led.counters().flips, 1u);
    }
    // 4. Unknown ambiguity key: the reset is taken and is itself a no-op there
    //    (resetAmbiguity's guard) — no state is created, nothing is pending.
    {
        Bed b;
        PbDiscCarryLedger led;
        led.record("E11|L1C", 0.4);
        const std::size_t n0 = b.sm.getStateSize();
        const Eigen::MatrixXd P0 = b.sm.getCovariance();
        const auto d = led.react(b.sm, "E11|L1C", "E11_L1C", true, false, 0.9,
                                 false, false, kResetStd);
        EXPECT_EQ(d.action, A::ResetUnknownKey);
        EXPECT_EQ(b.sm.getStateSize(), n0);
        EXPECT_TRUE(bitwiseEqual(P0, b.sm.getCovariance()));
        EXPECT_FALSE(b.sm.isAmbiguityPendingReseed("E11_L1C"));
        EXPECT_EQ(led.counters().reset_unknown_key, 1u);
    }
}

TEST(ConsumerDiscCarryTest, CounterClosureHoldsOverAMixedSequence) {
    Bed b;
    PbDiscCarryLedger led;
    // Two keys, a mixed sequence: a first-sight reset, then carries, a basis
    // refusal, an unknown-key refusal, quiet epochs in between.
    led.react(b.sm, "G24|L5X", "G24_L5X", true, false, 1.0, false, false, kResetStd);   // first sight
    led.record("G24|L5X", 1.0);
    led.react(b.sm, "G24|L5X", "G24_L5X", false, false, 1.001, false, false, kResetStd); // quiet
    led.record("G24|L5X", 1.001);
    led.react(b.sm, "G24|L5X", "G24_L5X", true, false, 1.8, false, false, kResetStd);    // carried
    led.record("G24|L5X", 1.8);
    led.record("G06|L2W", -0.3);
    led.react(b.sm, "G06|L2W", "G06_L2W", false, true, 0.0, false, false, kResetStd);    // flip carried
    led.record("G06|L2W", 0.0);
    led.react(b.sm, "G06|L2W", "G06_L2W", true, true, -0.25, true, false, kResetStd);    // basis refusal
    led.record("G06|L2W", -0.25);
    led.record("E11|L1C", 0.5);
    led.react(b.sm, "E11|L1C", "E11_L1C", true, false, 0.7, false, false, kResetStd);    // unknown key
    led.record("E11|L1C", 0.7);

    const auto& c = led.counters();
    EXPECT_EQ(c.events, 5u);
    EXPECT_EQ(c.disc_bumps, 4u);
    EXPECT_EQ(c.flips, 2u);
    EXPECT_EQ(c.disc_carried, 1u);
    EXPECT_EQ(c.flip_carried, 1u);
    EXPECT_EQ(c.reset_first_sight, 1u);
    EXPECT_EQ(c.reset_basis, 1u);
    EXPECT_EQ(c.reset_bssd, 0u);
    EXPECT_EQ(c.reset_unknown_key, 1u);
    EXPECT_EQ(c.resets(), 3u);
    EXPECT_TRUE(c.closureHolds());
    EXPECT_EQ(led.size(), 3u);
    EXPECT_STREQ(PbDiscCarryLedger::actionName(PbDiscCarryLedger::Action::CarriedDisc), "carried_disc");
}

// ---------------------------------------------------------------------------
// Config plumbing: the knob lands on the DERIVED consumer config only.
// ---------------------------------------------------------------------------
TEST(ConsumerDiscCarryTest, ConfigDefaultsAreOffAndTheKnobReachesOnlyTheDerivedConsumer) {
    {
        GnssConfig d;
        EXPECT_FALSE(d.ssrPhaseBiasDiscCarry());
        EXPECT_FALSE(d.getNetworkConfig().closed_loop.pb_disc_carry);
        EXPECT_FALSE(d.getNetworkConfig().closed_loop.state_snapshot);
    }
    // The harness knob: parsed, derived into the consumer, absent on the base.
    {
        LogManager log;
        GnssConfig cfg;
        const fs::path p = writeTempConfig(
            "harness", networkConfig(", \"pb_disc_carry\": true, \"state_snapshot\": true"));
        ASSERT_TRUE(cfg.loadFromJson(p, log));
        EXPECT_TRUE(cfg.getNetworkConfig().closed_loop.pb_disc_carry);
        EXPECT_TRUE(cfg.getNetworkConfig().closed_loop.state_snapshot);
        EXPECT_FALSE(cfg.ssrPhaseBiasDiscCarry());                       // the PRODUCER never carries
        const GnssConfig consumer = cfg.deriveClosedLoopConsumerConfig();
        EXPECT_TRUE(consumer.ssrPhaseBiasDiscCarry());                   // the consumer does
        std::error_code ec; fs::remove(p, ec);
    }
    // Without the harness knob the derived consumer stays on the reset path.
    {
        LogManager log;
        GnssConfig cfg;
        const fs::path p = writeTempConfig("off", networkConfig(""));
        ASSERT_TRUE(cfg.loadFromJson(p, log));
        EXPECT_FALSE(cfg.deriveClosedLoopConsumerConfig().ssrPhaseBiasDiscCarry());
        std::error_code ec; fs::remove(p, ec);
    }
    // A standalone consumer sets the processing key directly.
    {
        LogManager log;
        GnssConfig cfg;
        const fs::path p = writeTempConfig(
            "standalone", "{ \"processing\": { \"ssr_phase_bias_disc_carry\": true } }\n");
        ASSERT_TRUE(cfg.loadFromJson(p, log));
        EXPECT_TRUE(cfg.ssrPhaseBiasDiscCarry());
        std::error_code ec; fs::remove(p, ec);
    }
}

// ---------------------------------------------------------------------------
// (B) The consumer history file beside the network copy, and the write refusal
// on an engine that has processed nothing.
// ---------------------------------------------------------------------------
TEST(ConsumerDiscCarryTest, ConsumerHistoryFileSitsBesideTheNetworkCopy) {
    const fs::path net = fs::path("C:/runs/evseed_x") / "state_snapshot.txt";
    fs::path net_copy = net; net_copy += ".ep0604";                     // NetworkRunContext.cpp's suffix
    const fs::path cons = RealTimeEngine::consumerSnapshotHistoryPath(net, 604);
    EXPECT_EQ(cons.filename().string(), "state_snapshot.txt.consumer.ep0604");
    EXPECT_EQ(cons.parent_path(), net_copy.parent_path());
    // Same zero-padded n_emitted suffix as the network copy.
    const std::string cs = cons.string(), ns = net_copy.string();
    EXPECT_EQ(cs.substr(cs.size() - 7), ns.substr(ns.size() - 7));
    EXPECT_EQ(RealTimeEngine::consumerSnapshotHistoryPath(net, 1).filename().string(),
              "state_snapshot.txt.consumer.ep0001");
}

// The on-disk invariant the harness relies on: the consumer file written beside
// a network history copy carries the SAME epoch (week, tow) in its M line.  The
// two writers stamp the tow differently (the network copy with fmt's default
// double format, the RT file with "{:.17g}"), so the invariant is on the parsed
// value — which is what event_leg.py's read_snapshot_header / read_rt_snapshot_
// header compare, and what the engine's age gate consumes.
TEST(ConsumerDiscCarryTest, ConsumerFileCarriesTheNetworkCopysTow) {
    const int    week = 2313;
    const double tow  = 277290.00000810623;   // the field seed's ep0604 stamp (double residue and all)
    const fs::path dir = fs::temp_directory_path();
    const fs::path net = dir / "cc_disc_carry_state_snapshot.txt";
    fs::path net_copy = net; net_copy += ".ep0604";
    const fs::path cons = RealTimeEngine::consumerSnapshotHistoryPath(net, 604);

    NetworkStateSnapshot ns;
    ns.snap_week = week; ns.snap_tow = tow; ns.n_emitted = 604;
    std::string err;
    ASSERT_TRUE(NetworkStateSnapshotIO::write(net_copy, ns, &err)) << err;

    RtStateSnapshot rs;
    rs.snap_week = week; rs.snap_tow = tow;
    rs.core_size = 2; rs.state_size = 2;
    rs.x = Eigen::VectorXd::Zero(2);
    rs.P = Eigen::MatrixXd::Identity(2, 2);
    ASSERT_TRUE(RtStateSnapshotIO::write(cons, rs, &err)) << err;

    auto mline = [](const fs::path& p) {
        std::ifstream f(p);
        std::string line;
        while (std::getline(f, line))
            if (line.rfind("M,", 0) == 0) return line;
        return std::string();
    };
    auto field = [](const std::string& line, int k) {
        std::string s = line; int i = 0; std::size_t pos;
        while (i < k && (pos = s.find(',')) != std::string::npos) { s = s.substr(pos + 1); ++i; }
        return s.substr(0, s.find(','));
    };
    const std::string mn = mline(net_copy), mc = mline(cons);
    ASSERT_FALSE(mn.empty()); ASSERT_FALSE(mc.empty());
    EXPECT_EQ(std::stoi(field(mn, 1)), week);
    EXPECT_EQ(std::stoi(field(mc, 1)), week);
    EXPECT_EQ(std::stod(field(mn, 2)), tow);          // both round-trip the exact double
    EXPECT_EQ(std::stod(field(mc, 2)), tow);
    EXPECT_EQ(std::stod(field(mn, 2)), std::stod(field(mc, 2)));

    // And the RT reader recovers the same epoch the engine's age gate will see.
    RtStateSnapshot back;
    ASSERT_TRUE(RtStateSnapshotIO::read(cons, back, &err)) << err;
    EXPECT_EQ(back.snap_week, week);
    EXPECT_EQ(back.snap_tow, tow);

    fs::remove(net_copy, ec_ignore()); fs::remove(cons, ec_ignore());
}

TEST(ConsumerDiscCarryTest, WriteStateSnapshotToRefusesBeforeAnyEpoch) {
    LogManager lm;
    GnssConfig cfg;
    RealTimeEngine eng(cfg, lm);
    const fs::path p = fs::temp_directory_path() / "cc_disc_carry_never_written.txt";
    std::error_code ec; fs::remove(p, ec);
    std::string err;
    const GnssTime t{ 2313, 277290.0 };
    EXPECT_FALSE(eng.writeStateSnapshotTo(p, &t, &err));
    EXPECT_NE(err.find("no epoch"), std::string::npos) << err;
    EXPECT_FALSE(fs::exists(p));                                       // nothing written
    EXPECT_FALSE(eng.lastProcessedEpochTime().has_value());
    EXPECT_EQ(eng.stateSnapshotRestoreStatus().outcome,
              RealTimeEngine::RtSnapshotRestoreStatus::Outcome::NotStaged);
    EXPECT_FALSE(eng.pbDiscCarryArmed());
    EXPECT_EQ(eng.pbDiscCarryCounters().events, 0u);
}
