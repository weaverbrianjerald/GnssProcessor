/**
 * @file test_handover_stransform.cpp
 * @brief Handover S-transform (network.handover_stransform, queue 1(b);
 *        docs/SPEC_pivot_visibility_gauge_defect.md §8, measured motivation in
 *        docs/SPEC_event_harness.md §19): the structural direction, the rank-1
 *        kernel, its exactness, and the loader interlocks.
 *
 * The card rests on the claim that a clock-pivot handover moves a gauge, not the
 * information: the carried shared state is valid but expressed in the datum that
 * just retired, so it is re-expressed rather than dropped for an epoch.  The
 * claim holds only if three properties hold exactly, and a failure of any of them
 * is not visible at the site where it occurs:
 *
 *   1. the transform runs along the structural direction the datum builder
 *      pinned; a hand-written or eigen-derived direction would mix families and
 *      move states the gauge does not own (§8.1 rule 1 of
 *      SPEC_pivot_visibility_gauge_defect);
 *   2. the transform lands the incoming pivot on its pinned value (0) and leaves
 *      every between-satellite difference inside the constellation unchanged.
 *      That invariance is what makes it an S-transform; without it the released
 *      carry would inject a frame error that no downstream instrument separates
 *      from a real one (§8.4 INC-3 of SPEC_pivot_visibility_gauge_defect);
 *   3. the dispersion bump is non-negative on every transformed row.  §8.1 keeps
 *      only the diagonal term because dropping the cross term overstates variance;
 *      a bump that could go negative would turn a conservative approximation into
 *      an over-confident prior.
 *
 * The remaining cases cover the no-op paths (a transform that cannot be derived
 * changes nothing, rather than half-applying) and the loader interlocks, which
 * are the only places the knob can become inert while the run still reads as
 * clean (invariant #4).
 *
 * The kernel is exercised directly: it is a pure function over a carry vector and
 * a direction, with no context, no estimator and no I/O.  The seam around it
 * handles counters, events and the fold release, which the byte-identity and
 * actuation gates cover on real runs.
 */

#include <gtest/gtest.h>

#include <cmath>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "config/GnssConfig.hpp"
#include "app/NetworkRunContext.hpp"
#include "filter/NetworkHandoverStransform.hpp"
#include "filter/NetworkRunTypes.hpp"
#include "filter/NetworkSbasis.hpp"
#include "logging/LogManager.hpp"

using namespace GnssProcessor;
namespace fs = std::filesystem;

namespace {

/// A carried clock row.
CarryPrior clk(const std::string& sat, double value, double var) {
    CarryPrior c;
    c.kind  = SharedParamKind::Clock;
    c.sat   = sat;
    c.value = value;
    c.var   = var;
    return c;
}

/// A carried bias row (code or phase); the transform must not touch it.
CarryPrior bias(SharedParamKind kind, const std::string& sat, const std::string& sig,
                double value, double var) {
    CarryPrior c;
    c.kind   = kind;
    c.sat    = sat;
    c.signal = sig;
    c.value  = value;
    c.var    = var;
    return c;
}

/// The carry used by most cases: two constellations, distinct levels and variances.
std::vector<CarryPrior> makeCarry() {
    return {
        clk("G01", 0.00, 1.0e-6),    // the outgoing pivot: pinned last epoch, so level 0
        clk("G02", -2.0742503730608925, 4.0e-4),   // incoming pivot, value from SPEC_event_harness §19.1
        clk("G05", -0.75, 2.5e-4),
        clk("G32", +1.25, 9.0e-4),
        clk("E02", +0.1443824781885379, 3.0e-4),   // Galileo: a different gauge entirely
        clk("E03", -0.50, 5.0e-4),
        bias(SharedParamKind::CodeBias,  "G02", "C1C", +0.031, 1.0e-4),
        bias(SharedParamKind::PhaseBias, "G05", "L1C", -0.480, 4.0e-3),
        bias(SharedParamKind::CodeBias,  "E03", "C1C", +0.012, 1.0e-4),
    };
}

/// The kernel on a carried clock list: it walks the run's carried state (card PRM phase 2),
/// so the list is moved into a NetworkCarryState's prev_carry and back.
ClockStransformResult applyTo(std::vector<CarryPrior>& c,
                              const std::vector<std::pair<std::string, double>>& v_c,
                              const std::string& q) {
    NetworkCarryState state;
    state.prev_carry = std::move(c);
    const ClockStransformResult r = applyClockStransform(state, v_c, q);
    c = std::move(state.prev_carry);
    return r;
}

double levelOf(const std::vector<CarryPrior>& c, const std::string& sat) {
    for (const auto& e : c)
        if (e.kind == SharedParamKind::Clock && e.sat == sat) return e.value;
    ADD_FAILURE() << "no carried clock for " << sat;
    return std::nan("");
}

double varOf(const std::vector<CarryPrior>& c, const std::string& sat) {
    for (const auto& e : c)
        if (e.kind == SharedParamKind::Clock && e.sat == sat) return e.var;
    ADD_FAILURE() << "no carried clock for " << sat;
    return std::nan("");
}

/// Minimal ppp-rtk-network config that parses, with an injected network block.
struct Loaded {
    LogManager log;
    GnssConfig cfg;
    fs::path   path;
    Loaded(const std::string& tag, const std::string& network_extra) {
        const std::string body = std::string("{\n") +
            "  \"processing\": {\n"
            "    \"mode\": \"ppp-rtk-network\",\n"
            "    \"active_systems\": [ \"GPS\" ],\n"
            "    \"signal_priorities\": { \"GPS\": \"WC\" }\n"
            "  },\n"
            "  \"network\": {\n"
            "    \"clock_datum_station\": \"AAAA\",\n"
            + network_extra +
            "    \"stations\": [\n"
            "      { \"name\": \"AAAA\", \"position_ecef\": [ -2493304.99, -4655214.79, 3565497.67 ] },\n"
            "      { \"name\": \"BBBB\", \"position_ecef\": [ -2281621.75, -1453585.08, 5756964.89 ] }\n"
            "    ]\n"
            "  }\n"
            "}\n";
        path = fs::temp_directory_path() / ("sts_cfg_" + tag + ".json");
        std::ofstream f(path);
        f << body;
        f.close();
        EXPECT_TRUE(cfg.loadFromJson(path, log));
    }
    ~Loaded() { std::error_code ec; fs::remove(path, ec); }
};

const std::vector<std::string> kRegistered{"G01", "G02", "G05", "G32", "E02", "E03"};

}  // namespace

// ===========================================================================
// 1: the structural direction: membership, coefficients, order
// ===========================================================================

TEST(HandoverStransform, NullDirectionIsTheConstellationsClocksAtUnitWeight) {
    const auto g = NetworkSbasis::clockNullDirection(kRegistered, "G");
    ASSERT_EQ(g.size(), 4u);
    // Lexical order, the order buildSharedDatum groups and sorts by, so the
    // direction spans exactly the columns that builder pinned.
    EXPECT_EQ(g[0].first, "G01");
    EXPECT_EQ(g[1].first, "G02");
    EXPECT_EQ(g[2].first, "G05");
    EXPECT_EQ(g[3].first, "G32");
    for (const auto& e : g) EXPECT_DOUBLE_EQ(e.second, 1.0);

    // The gauge is per constellation: Galileo's direction contains no GPS column.
    const auto e = NetworkSbasis::clockNullDirection(kRegistered, "E");
    ASSERT_EQ(e.size(), 2u);
    EXPECT_EQ(e[0].first, "E02");
    EXPECT_EQ(e[1].first, "E03");

    // A constellation with nothing registered yields an empty direction, which the
    // caller must treat as "no transform", not as a zero direction.
    EXPECT_TRUE(NetworkSbasis::clockNullDirection(kRegistered, "C").empty());
}

// ===========================================================================
// 2: the kernel: the incoming pivot lands on 0, every clock moves by the same c0
// ===========================================================================

TEST(HandoverStransform, LandsIncomingPivotOnZeroAndShiftsTheConstellationRigidly) {
    auto carry = makeCarry();
    const double x_q_pre = levelOf(carry, "G02");
    const double g01_pre = levelOf(carry, "G01");
    const double g05_pre = levelOf(carry, "G05");
    const double g32_pre = levelOf(carry, "G32");

    const auto v_c = NetworkSbasis::clockNullDirection(kRegistered, "G");
    const auto r   = applyTo(carry, v_c, "G02");

    ASSERT_TRUE(r.applied);
    EXPECT_EQ(r.rows, 4);                       // exactly the four GPS clocks
    EXPECT_DOUBLE_EQ(r.c0, x_q_pre);            // v_c[q] == 1 ⇒ c0 is the carried level
    EXPECT_TRUE(r.selfcheck_ok);

    // The incoming pivot is exactly its pinned value, not close to it; the released
    // carry is valid only if this holds (§8.4 INC-3 of SPEC_pivot_visibility_gauge_defect).
    EXPECT_DOUBLE_EQ(levelOf(carry, "G02"), 0.0);
    EXPECT_DOUBLE_EQ(r.x_q_post, 0.0);

    // Every other clock of the constellation moved by the same c0 (v_c[i] = 1).
    EXPECT_DOUBLE_EQ(levelOf(carry, "G01"), g01_pre - x_q_pre);
    EXPECT_DOUBLE_EQ(levelOf(carry, "G05"), g05_pre - x_q_pre);
    EXPECT_DOUBLE_EQ(levelOf(carry, "G32"), g32_pre - x_q_pre);

    // The retired pivot now sits at -c0, which is where a fresh solve under the new
    // pin would have put it.
    EXPECT_DOUBLE_EQ(levelOf(carry, "G01"), -x_q_pre);
}

// ===========================================================================
// 3: the S-transform property: between-satellite differences are invariant
// ===========================================================================

TEST(HandoverStransform, BetweenSatelliteDifferencesAreUnchanged) {
    auto before = makeCarry();
    auto carry  = makeCarry();
    const auto v_c = NetworkSbasis::clockNullDirection(kRegistered, "G");
    ASSERT_TRUE(applyTo(carry, v_c, "G02").applied);

    const std::vector<std::pair<std::string, std::string>> pairs{
        {"G01", "G02"}, {"G01", "G05"}, {"G02", "G32"}, {"G05", "G32"}};
    for (const auto& p : pairs) {
        const double d_before = levelOf(before, p.first) - levelOf(before, p.second);
        const double d_after  = levelOf(carry,  p.first) - levelOf(carry,  p.second);
        EXPECT_NEAR(d_after, d_before, 1e-12)
            << "between-satellite difference " << p.first << "-" << p.second
            << " moved; the update is not an S-transform";
    }
}

// ===========================================================================
// 4: scope: the other constellation and every non-clock family are untouched
// ===========================================================================

TEST(HandoverStransform, OtherConstellationAndBiasFamiliesAreUntouched) {
    auto before = makeCarry();
    auto carry  = makeCarry();
    const auto v_c = NetworkSbasis::clockNullDirection(kRegistered, "G");
    ASSERT_TRUE(applyTo(carry, v_c, "G02").applied);

    // Galileo has its own clock gauge; a GPS handover does not move it.
    EXPECT_DOUBLE_EQ(levelOf(carry, "E02"), levelOf(before, "E02"));
    EXPECT_DOUBLE_EQ(levelOf(carry, "E03"), levelOf(before, "E03"));
    EXPECT_DOUBLE_EQ(varOf(carry, "E02"), varOf(before, "E02"));

    // Code and phase bias have zero component along the clock null direction (their
    // gauges are realized independently: the W6 IF pin + sum-to-zero rows, and the
    // anchor-receiver ambiguity pins).  The seam releases them to fold and does not
    // transform them; a nonzero shift here would be a double-counted datum.
    for (std::size_t i = 0; i < before.size(); ++i) {
        if (before[i].kind == SharedParamKind::Clock) continue;
        EXPECT_DOUBLE_EQ(carry[i].value, before[i].value) << "bias row " << i << " moved";
        EXPECT_DOUBLE_EQ(carry[i].var,   before[i].var)   << "bias row " << i << " var moved";
    }
}

// ===========================================================================
// 5: the dispersion bump is non-negative, and is var_q on every transformed row
// ===========================================================================

TEST(HandoverStransform, VarianceBumpIsNonNegativeAndEqualsVarQ) {
    auto before = makeCarry();
    auto carry  = makeCarry();
    const double var_q = varOf(before, "G02");

    const auto v_c = NetworkSbasis::clockNullDirection(kRegistered, "G");
    const auto r   = applyTo(carry, v_c, "G02");
    ASSERT_TRUE(r.applied);
    EXPECT_DOUBLE_EQ(r.var_q, var_q);

    for (const std::string& s : {"G01", "G02", "G05", "G32"}) {
        const double d = varOf(carry, s) - varOf(before, s);
        EXPECT_GE(d, 0.0) << s << " lost variance — the bump must never be negative";
        // v_c[i] = 1 ⇒ the bump is exactly var_q (the diagonal-compatible term of
        // §8.1 of SPEC_pivot_visibility_gauge_defect).
        EXPECT_DOUBLE_EQ(d, var_q) << s;
    }
}

// ===========================================================================
// 6: the no-op paths change nothing; a half-applied transform is the worst outcome
// ===========================================================================

TEST(HandoverStransform, NoOpPathsLeaveTheCarryBitIdentical) {
    const auto v_c = NetworkSbasis::clockNullDirection(kRegistered, "G");
    const auto before = makeCarry();

    // (a) the incoming pivot has no carried clock level (a first sighting, or a
    //     vanish-return whose carry aged out), so there is nothing to re-reference from.
    {
        auto carry = makeCarry();
        const auto r = applyTo(carry, v_c, "G99");
        EXPECT_FALSE(r.applied);
        EXPECT_EQ(r.rows, 0);
        for (std::size_t i = 0; i < before.size(); ++i) {
            EXPECT_DOUBLE_EQ(carry[i].value, before[i].value);
            EXPECT_DOUBLE_EQ(carry[i].var,   before[i].var);
        }
    }
    // (b) an empty direction (constellation not registered this epoch).
    {
        auto carry = makeCarry();
        EXPECT_FALSE(applyTo(carry, {}, "G02").applied);
        for (std::size_t i = 0; i < before.size(); ++i)
            EXPECT_DOUBLE_EQ(carry[i].value, before[i].value);
    }
    // (c) a carry holding a stale row for a satellite the direction does not contain
    //     is left alone: membership is read from v_c, never inferred from the id.
    {
        auto carry = makeCarry();
        carry.push_back(clk("G77", +7.0, 1.0e-3));   // not in kRegistered
        ASSERT_TRUE(applyTo(carry, v_c, "G02").applied);
        EXPECT_DOUBLE_EQ(levelOf(carry, "G77"), +7.0);
        EXPECT_DOUBLE_EQ(varOf(carry, "G77"), 1.0e-3);
    }
}

// ===========================================================================
// 7: idempotence: re-deriving the coefficient after a transform returns 0
// ===========================================================================

TEST(HandoverStransform, SecondApplicationIsANoOp) {
    auto carry = makeCarry();
    const auto v_c = NetworkSbasis::clockNullDirection(kRegistered, "G");
    ASSERT_TRUE(applyTo(carry, v_c, "G02").applied);
    const auto snapshot = carry;

    const auto again = applyTo(carry, v_c, "G02");
    EXPECT_DOUBLE_EQ(again.c0, 0.0);        // the pivot already sits at its pinned value
    for (std::size_t i = 0; i < snapshot.size(); ++i)
        EXPECT_DOUBLE_EQ(carry[i].value, snapshot[i].value)
            << "row " << i << " moved on a second application";
}

// ===========================================================================
// 8: the loader: default off, and both interlocks disarm rather than run inert
// ===========================================================================

TEST(HandoverStransformConfig, DefaultsOffArmsFromJsonAndInterlocksDisarm) {
    {
        Loaded off("off", "");
        EXPECT_FALSE(off.cfg.networkHandoverStransform());
    }
    {
        // The transform releases the skip for the changed constellation only; the
        // collateral half belongs to increment 0 (scoped_handover_response).  Without
        // it the edge is not fixed, so the loader disarms the knob rather than let a
        // partial fix be graded (invariant #4).
        Loaded no_scope("noscope", "    \"handover_stransform\": true,\n");
        EXPECT_FALSE(no_scope.cfg.networkHandoverStransform());
    }
    {
        Loaded on("on",
                  "    \"scoped_handover_response\": true,\n"
                  "    \"handover_stransform\": true,\n");
        EXPECT_TRUE(on.cfg.networkHandoverStransform());
        EXPECT_TRUE(on.cfg.networkScopedHandoverResponse());
    }
    {
        // An anchor-free clock datum realizes the gauge with no pivot satellite, so
        // there is no handover seam to transform across; armed here, the knob would
        // be permanently inert.
        Loaded s2z("s2z",
                   "    \"scoped_handover_response\": true,\n"
                   "    \"clock_datum_sum_to_zero\": true,\n"
                   "    \"handover_stransform\": true,\n");
        EXPECT_FALSE(s2z.cfg.networkHandoverStransform());
    }
}
