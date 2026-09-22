/**
 * @file test_network_gross_los.cpp
 * @brief Card GL — the GROSS LINE-OF-SIGHT screen's two pure rules and its
 *        config gate (docs/SPEC_event_harness.md §24.18).
 *
 * The screen exists because every post-fit tier this producer owns pools
 * CARRIED-AMBIGUITY PHASE rows — `NetworkRaim.cpp:328` admits a row by
 * `|A_r(row, amb_col)| > 0.5`, which is +1 on a phase row and exactly 0 on a
 * code row — so a gross CODE error on one line of sight is not merely
 * under-weighted there, it is unreachable.  Measured: (DJIG00DJI, G06) at
 * −75 … −142 m on three code signals for 31 epochs of `runs/_cm_pvb` with
 * `network_raim.csv` filing zero rows, and BSHM00ISR poisoned to a 54 m station
 * code rms by hour 5 of `runs/_cm_c10`.
 *
 * What is exercised here is what can be exercised without a network: the two
 * DECISION rules, which are pure functions for exactly this reason (the
 * `networkRaimEraseBandScope` precedent), and the loader.  The response's own
 * numbers — what a declaration does to a real line of sight — are graded by the
 * 6 h arm the SPEC records, not by this suite (the suite guards structure and
 * non-regression; it does not validate estimation accuracy).
 *
 * Three things this file is deliberately strict about:
 *  - the DECLARATION needs both the row count AND the persistence, and either
 *    one alone is not enough;
 *  - the CLEAR needs the release bar, not merely the absence of the declaration
 *    bar — the band between the two is the hysteresis and a line of sight
 *    sitting in it must keep its disposition;
 *  - OFF is OFF: the knob defaults to false and every companion knob keeps the
 *    pre-card value, so an unarmed run cannot allocate the state or move a row
 *    weight (standing invariant #6).
 */
#include <gtest/gtest.h>

#include "app/NetworkRunContext.hpp"     // RF-3b: the ACT half's carry + counters
#include "config/GnssConfig.hpp"
#include "filter/NetworkGrossLos.hpp"
#include "filter/NetworkRunTypes.hpp"    // RF-3b: NetStation / StationAssembly / LocalCarry
#include "logging/LogManager.hpp"

#include <filesystem>
#include <limits>
#include <fstream>
#include <string>
#include <vector>

using namespace GnssProcessor;

namespace {

namespace fs = std::filesystem;

/// One code row of a line of sight: (signal, |v|·√w, signed v in metres).
GrossLosSample row(const std::string& sig, double r, double v_m) {
    return GrossLosSample{ sig, r, v_m };
}

/// The DJIG/G06 shape §24.18 names: three code signals, all hugely over the bar.
/// A row at σ ≈ 0.9 m and v = −142 m stands at ~158σ; the numbers below are that
/// shape, not that exact arc.
std::vector<GrossLosSample> grossLos() {
    return { row("C1W", 158.0, -142.0), row("C2W", 91.0, -82.0),
             row("C5Q", 84.0, -75.0) };
}

fs::path writeTempConfig(const std::string& tag, const std::string& body) {
    const fs::path p = fs::temp_directory_path() / ("gl_cfg_" + tag + ".json");
    std::ofstream f(p);
    f << body;
    f.close();
    return p;
}

/// A minimal loadable network config, with @p extra spliced into the network block.
std::string networkConfig(const std::string& extra) {
    return std::string(
        "{\n"
        "  \"processing\": {\n"
        "    \"mode\": \"ppp-rtk-network\",\n"
        "    \"active_systems\": [ \"GPS\" ],\n"
        "    \"signal_priorities\": { \"GPS\": \"WC\" }\n"
        "  },\n"
        "  \"network\": {\n"
        "    \"clock_datum_station\": \"STA1\",\n")
        + extra +
        "    \"stations\": [\n"
        "      { \"name\": \"STA1\", \"position_ecef\": [ -2493304.99, -4655214.79, 3565497.67 ] },\n"
        "      { \"name\": \"STA2\", \"position_ecef\": [ -2281621.75, -1453585.08, 5756964.89 ] }\n"
        "    ]\n"
        "  }\n"
        "}\n";
}

bool loadWith(const std::string& extra, GnssConfig& cfg, LogManager& log,
              const std::string& tag) {
    const fs::path p = writeTempConfig(tag, networkConfig(extra));
    const bool ok = cfg.loadFromJson(p, log);
    std::error_code ec; fs::remove(p, ec);
    return ok;
}

} // namespace

// ---------------------------------------------------------------------------
// The per-epoch rule — networkGrossLosAssess
// ---------------------------------------------------------------------------

TEST(NetworkGrossLosRule, GrossLineOfSightClearsTheBarOnEveryCodeSignal) {
    const auto d = networkGrossLosAssess(grossLos(), /*k=*/8.0, /*min_rows=*/2,
                                         /*clear_sigma=*/3.0);
    EXPECT_TRUE(d.over);
    EXPECT_FALSE(d.under_clear);
    EXPECT_EQ(d.n_rows, 3);
    EXPECT_EQ(d.n_over, 3);
    EXPECT_NEAR(d.r_max, 158.0, 1e-9);
    EXPECT_NEAR(d.v_abs_max, 142.0, 1e-9);
    // The identities reach the event, sorted and de-duplicated.
    EXPECT_EQ(d.signals, "C1W;C2W;C5Q");
}

TEST(NetworkGrossLosRule, OneLoudRowIsNotAGrossLineOfSight) {
    // One band over the bar is one band's multipath; a gross line of sight is a
    // RANGE error and is wrong on every code band at once.  min_rows is what
    // separates them, and it must not be satisfiable by a single row.
    const std::vector<GrossLosSample> cell = {
        row("C1W", 40.0, -36.0), row("C2W", 1.1, -1.0), row("C5Q", 0.7, -0.6)};
    const auto d = networkGrossLosAssess(cell, 8.0, 2, 3.0);
    EXPECT_FALSE(d.over);
    EXPECT_EQ(d.n_over, 1);
    // Not clean either — the loud row is above the release bar, so the line of
    // sight is in neither camp and any declared disposition survives the epoch.
    EXPECT_FALSE(d.under_clear);
}

TEST(NetworkGrossLosRule, EmptySignalLabelsStillCount) {
    // A code row of a posture whose satellite code biases are supplied EXTERNALLY,
    // and the W6 datum signal's rows even when they are estimated, carry no CB:
    // column and therefore no signal label.  Counting DISTINCT LABELS instead of
    // ROWS would make the screen silently inert on exactly those postures — which
    // is the failure mode this whole card is about — so the rows count and the
    // (empty) labels simply do not reach the event detail.
    const std::vector<GrossLosSample> cell = {
        row("", 30.0, -27.0), row("", 25.0, -22.0)};
    const auto d = networkGrossLosAssess(cell, 8.0, 2, 3.0);
    EXPECT_TRUE(d.over);
    EXPECT_EQ(d.n_over, 2);
    EXPECT_EQ(d.signals, "");
}

TEST(NetworkGrossLosRule, HysteresisBandIsNeitherDeclareNorClear) {
    // 5σ sits above the 3σ release bar and below the 8σ declaration bar.  The
    // verdict must refuse BOTH edges: this gap is what stops a line of sight
    // hovering at the bar from flapping declare/clear every other epoch.
    const std::vector<GrossLosSample> cell = {row("C1W", 5.0, 4.5), row("C2W", 4.2, 3.8)};
    const auto d = networkGrossLosAssess(cell, 8.0, 2, 3.0);
    EXPECT_FALSE(d.over);
    EXPECT_FALSE(d.under_clear);
}

TEST(NetworkGrossLosRule, AbsenceIsNotEvidenceOfHealth) {
    // No code rows this epoch.  `under_clear` must stay FALSE: standing invariant
    // #1's rule that a response is never lifted on the ABSENCE of evidence.  The
    // seam handles a vanished line of sight through the expiry edge instead.
    const auto d = networkGrossLosAssess({}, 8.0, 2, 3.0);
    EXPECT_FALSE(d.over);
    EXPECT_FALSE(d.under_clear);
    EXPECT_EQ(d.n_rows, 0);
}

TEST(NetworkGrossLosRule, NonFiniteRowsAreIgnoredNotCounted) {
    const std::vector<GrossLosSample> cell = {
        row("C1W", std::numeric_limits<double>::quiet_NaN(), 0.0),
        row("C2W", 30.0, -27.0)};
    const auto d = networkGrossLosAssess(cell, 8.0, 2, 3.0);
    EXPECT_EQ(d.n_over, 1);
    EXPECT_FALSE(d.over);        // one countable row < min_rows
    EXPECT_NEAR(d.r_max, 30.0, 1e-9);
}

// ---------------------------------------------------------------------------
// The state machine — networkGrossLosStep
// ---------------------------------------------------------------------------

TEST(NetworkGrossLosState, DeclarationNeedsTwoConsecutiveEpochs) {
    GrossLosState s;
    const auto hot = networkGrossLosAssess(grossLos(), 8.0, 2, 3.0);
    // Epoch 1: evidence, no edge yet — the persistence gate is what starves a
    // heavy-tailed single-epoch spike.
    EXPECT_EQ(networkGrossLosStep(s, hot, 2, 2, 100.0), GrossLosEdge::None);
    EXPECT_FALSE(s.gross);
    EXPECT_EQ(s.over_streak, 1);
    // Epoch 2: the decoupling edge.
    EXPECT_EQ(networkGrossLosStep(s, hot, 2, 2, 130.0), GrossLosEdge::Declare);
    EXPECT_TRUE(s.gross);
    EXPECT_EQ(s.n_declared, 1);
    EXPECT_NEAR(s.declared_tow, 130.0, 1e-9);
    EXPECT_EQ(s.over_streak, 0);   // the evidence is spent
}

TEST(NetworkGrossLosState, NonConsecutiveEvidenceNeverDeclares) {
    GrossLosState s;
    const auto hot   = networkGrossLosAssess(grossLos(), 8.0, 2, 3.0);
    const auto quiet = networkGrossLosAssess({row("C1W", 0.8, 0.7), row("C2W", 0.6, 0.5)},
                                             8.0, 2, 3.0);
    for (int i = 0; i < 6; ++i) {
        EXPECT_EQ(networkGrossLosStep(s, hot, 2, 2, 100.0 + 30.0 * i), GrossLosEdge::None);
        EXPECT_EQ(networkGrossLosStep(s, quiet, 2, 2, 115.0 + 30.0 * i), GrossLosEdge::None);
    }
    EXPECT_FALSE(s.gross);
    EXPECT_EQ(s.n_declared, 0);
}

TEST(NetworkGrossLosState, ClearNeedsTheReleaseBarTwiceAndIsItsOwnEdge) {
    GrossLosState s;
    const auto hot = networkGrossLosAssess(grossLos(), 8.0, 2, 3.0);
    ASSERT_EQ(networkGrossLosStep(s, hot, 2, 2, 100.0), GrossLosEdge::None);
    ASSERT_EQ(networkGrossLosStep(s, hot, 2, 2, 130.0), GrossLosEdge::Declare);

    // Sitting in the hysteresis band does NOT progress the clear.
    const auto band = networkGrossLosAssess({row("C1W", 5.0, 4.5), row("C2W", 4.0, 3.6)},
                                            8.0, 2, 3.0);
    EXPECT_EQ(networkGrossLosStep(s, band, 2, 2, 160.0), GrossLosEdge::None);
    EXPECT_TRUE(s.gross);
    EXPECT_EQ(s.clear_streak, 0);

    const auto quiet = networkGrossLosAssess({row("C1W", 1.4, 1.2), row("C2W", 0.9, 0.8)},
                                             8.0, 2, 3.0);
    EXPECT_EQ(networkGrossLosStep(s, quiet, 2, 2, 190.0), GrossLosEdge::None);
    EXPECT_TRUE(s.gross);
    EXPECT_EQ(s.clear_streak, 1);
    EXPECT_EQ(networkGrossLosStep(s, quiet, 2, 2, 220.0), GrossLosEdge::Clear);
    EXPECT_FALSE(s.gross);
    EXPECT_EQ(s.clear_streak, 0);
}

TEST(NetworkGrossLosState, ReturningEvidenceResetsTheClearProgress) {
    GrossLosState s;
    const auto hot = networkGrossLosAssess(grossLos(), 8.0, 2, 3.0);
    const auto quiet = networkGrossLosAssess({row("C1W", 1.0, 0.9), row("C2W", 0.8, 0.7)},
                                             8.0, 2, 3.0);
    ASSERT_EQ(networkGrossLosStep(s, hot, 2, 3, 100.0), GrossLosEdge::None);
    ASSERT_EQ(networkGrossLosStep(s, hot, 2, 3, 130.0), GrossLosEdge::Declare);
    EXPECT_EQ(networkGrossLosStep(s, quiet, 2, 3, 160.0), GrossLosEdge::None);
    EXPECT_EQ(networkGrossLosStep(s, quiet, 2, 3, 190.0), GrossLosEdge::None);
    EXPECT_EQ(s.clear_streak, 2);
    // The fault comes back before the third clean epoch: the release progress is
    // lost and the line of sight stays decoupled.  A clear that survived an
    // intervening gross epoch would re-couple a still-broken line of sight, which
    // is standing invariant #7's re-coupling edge failing open.
    EXPECT_EQ(networkGrossLosStep(s, hot, 2, 3, 220.0), GrossLosEdge::None);
    EXPECT_TRUE(s.gross);
    EXPECT_EQ(s.clear_streak, 0);
}

TEST(NetworkGrossLosState, ReDeclarationIsANewEpisode) {
    GrossLosState s;
    const auto hot = networkGrossLosAssess(grossLos(), 8.0, 2, 3.0);
    const auto quiet = networkGrossLosAssess({row("C1W", 1.0, 0.9), row("C2W", 0.8, 0.7)},
                                             8.0, 2, 3.0);
    ASSERT_EQ(networkGrossLosStep(s, hot, 1, 1, 100.0), GrossLosEdge::Declare);
    ASSERT_EQ(networkGrossLosStep(s, quiet, 1, 1, 130.0), GrossLosEdge::Clear);
    ASSERT_EQ(networkGrossLosStep(s, hot, 1, 1, 160.0), GrossLosEdge::Declare);
    EXPECT_EQ(s.n_declared, 2);
    EXPECT_NEAR(s.declared_tow, 160.0, 1e-9);
}

TEST(NetworkGrossLosState, DegenerateEpochCountsAreClampedToOne) {
    // A zero/negative persistence count must not mean "declare on nothing" — the
    // rule clamps to 1, i.e. act immediately, which is the only defensible reading.
    GrossLosState s;
    const auto hot = networkGrossLosAssess(grossLos(), 8.0, 2, 3.0);
    EXPECT_EQ(networkGrossLosStep(s, hot, 0, 0, 100.0), GrossLosEdge::Declare);
}

// ---------------------------------------------------------------------------
// The DEWEIGHT UNDO - the response must not launder its own release test
// ---------------------------------------------------------------------------

TEST(NetworkGrossLosRule, DeweightedRowsAreRestoredToTheirNominalSigma) {
    // MEASURED on the first runs/_gl_on6 (DJIG00DJI/G06): declared at TOW 265500
    // with |v| 143.8 m reading 125.6 sigma at nominal weight; one epoch later the
    // same line of sight read 1.28 sigma with |v| STILL 144.7 m, because its code
    // rows were now folded at sigma x 100 - under the 3 sigma release bar.  The
    // screen cleared it, the deweight lifted, and it re-declared two epochs later:
    // a limit cycle in which the response was true by construction and the line of
    // sight's carried level was erased for ever.
    //
    // 1.2789 x 100 = 127.89, which is the 125-130 sigma band the arc actually sat
    // in - so the undo recovers the number the DATA supports, not the one the
    // response produced.
    EXPECT_NEAR(networkGrossLosUndoDeweight(1.27894, 100.0), 127.894, 1e-9);
    // No fold (absent key, or a scale of exactly 1) must be an exact no-op - the
    // undo may never touch an undeclared line of sight's statistic.
    EXPECT_DOUBLE_EQ(networkGrossLosUndoDeweight(4.25, 1.0), 4.25);
    EXPECT_DOUBLE_EQ(networkGrossLosUndoDeweight(4.25, 0.5), 4.25);   // never INFLATES a row
    EXPECT_DOUBLE_EQ(networkGrossLosUndoDeweight(4.25, std::numeric_limits<double>::quiet_NaN()),
                     4.25);
}

TEST(NetworkGrossLosRule, TheUndoIsWhatKeepsADeclaredLineOfSightDeclared) {
    // The same three rows, folded and unfolded, through the actual rule.  Folded,
    // every row sits under the release bar and the verdict says RELEASE; unfolded,
    // the verdict is unchanged from the declaration epoch.  This is the assertion
    // that the bug cannot come back.
    const std::vector<GrossLosSample> folded = {
        row("C1W", 1.279, -144.7), row("C2W", 0.913, -103.3), row("C5Q", 0.845, -95.6)};
    const auto bad = networkGrossLosAssess(folded, 8.0, 2, 3.0);
    EXPECT_TRUE(bad.under_clear);      // the response has made its own release true
    EXPECT_FALSE(bad.over);

    std::vector<GrossLosSample> restored = folded;
    for (auto& s2 : restored) s2.r = networkGrossLosUndoDeweight(s2.r, 100.0);
    const auto good = networkGrossLosAssess(restored, 8.0, 2, 3.0);
    EXPECT_TRUE(good.over);
    EXPECT_FALSE(good.under_clear);
    EXPECT_EQ(good.n_over, 3);
    EXPECT_NEAR(good.r_max, 127.9, 0.05);
}

// ---------------------------------------------------------------------------
// The two GUARDS - both measured, both on the same state machine
// ---------------------------------------------------------------------------

TEST(NetworkGrossLosState, AGuardHoldsTheDeclarationAndSaysSo) {
    // The maturity horizon and the whole-station stand-down both reach the machine
    // through `allow_declare`.  A held declaration is SUPPRESSED, never None: the
    // evidence was complete and a guard refused it, and that population has its own
    // run-end counter precisely so it is never silent (standing invariant #4).
    GrossLosState s;
    const auto hot = networkGrossLosAssess(grossLos(), 8.0, 2, 3.0);
    EXPECT_EQ(networkGrossLosStep(s, hot, 2, 2, 100.0, /*allow_declare=*/false),
              GrossLosEdge::None);                       // streak 1 of 2 - not yet complete
    EXPECT_EQ(networkGrossLosStep(s, hot, 2, 2, 130.0, false), GrossLosEdge::Suppressed);
    EXPECT_FALSE(s.gross);
    EXPECT_EQ(s.n_declared, 0);
}

TEST(NetworkGrossLosState, HeldEvidenceIsNeitherDiscardedNorAllowedToBurst) {
    // Held at the requirement, not zeroed and not growing.  Zeroed would make the
    // guard a memory hole; growing would turn the gate's opening into a burst of
    // simultaneous declarations across every line of sight that waited.
    GrossLosState s;
    const auto hot = networkGrossLosAssess(grossLos(), 8.0, 2, 3.0);
    for (int i = 0; i < 40; ++i)
        networkGrossLosStep(s, hot, 2, 2, 100.0 + 30.0 * i, false);
    EXPECT_EQ(s.over_streak, 2);          // held AT the requirement, not 40
    EXPECT_FALSE(s.gross);
    // The gate opens: one qualifying epoch declares, and no earlier one would have.
    EXPECT_EQ(networkGrossLosStep(s, hot, 2, 2, 1400.0, true), GrossLosEdge::Declare);
    EXPECT_TRUE(s.gross);
}

TEST(NetworkGrossLosState, AGuardNeverBlocksTheReleaseEdge) {
    // Standing invariant #7's re-coupling edge must not fail CLOSED.  A response
    // that a guard could strand with no way out is worse than no response.
    GrossLosState s;
    const auto hot = networkGrossLosAssess(grossLos(), 8.0, 2, 3.0);
    const auto quiet = networkGrossLosAssess({row("C1W", 1.0, 0.9), row("C2W", 0.8, 0.7)},
                                             8.0, 2, 3.0);
    ASSERT_EQ(networkGrossLosStep(s, hot, 1, 1, 100.0, true), GrossLosEdge::Declare);
    // The station now stands down (or the horizon closed behind a warm resume):
    // the release still works.
    EXPECT_EQ(networkGrossLosStep(s, quiet, 1, 1, 130.0, /*allow_declare=*/false),
              GrossLosEdge::Clear);
    EXPECT_FALSE(s.gross);
}

// ---------------------------------------------------------------------------
// GL-2 - the whole-SATELLITE stand-down (SPEC_event_harness s24.19)
// ---------------------------------------------------------------------------

TEST(NetworkGrossLosGl2, TheSatelliteRuleIsTheSameStepWithTheGuardClosed) {
    // GL-2 reaches the machine through the same `allow_declare` the maturity horizon
    // and the whole-station stand-down use - deliberately, so there is ONE place a
    // declaration can be refused and one population (Suppressed) that records it.
    // A satellite over the bar at many stations is a SATELLITE fault; its lines of
    // sight must not be declared, and the evidence must not be thrown away either.
    GrossLosState s;
    const auto hot = networkGrossLosAssess(grossLos(), 8.0, 2, 3.0);
    ASSERT_EQ(networkGrossLosStep(s, hot, 2, 2, 100.0, /*allow_declare=*/false),
              GrossLosEdge::None);
    EXPECT_EQ(networkGrossLosStep(s, hot, 2, 2, 130.0, false), GrossLosEdge::Suppressed);
    EXPECT_FALSE(s.gross);
    // The satellite is released: the very next qualifying epoch declares, because the
    // line of sight's own evidence was held, not discarded.
    EXPECT_EQ(networkGrossLosStep(s, hot, 2, 2, 160.0, true), GrossLosEdge::Declare);
}

TEST(NetworkGrossLosGl2, AStandingDeclarationIsReleasedWhenItsSatelliteGoesDown) {
    // The seam releases a STANDING per-LOS declaration when its satellite is stood
    // down - it clears `gross` so the deweight lifts at the next assembly.  This is
    // standing invariant #7's re-coupling edge at the satellite's scale: the fault is
    // not this line of sight's, so holding it decoupled would keep removing code the
    // solution needs.  The release is deliberately NOT the per-LOS clear edge, which
    // asserts the DATA healed; here it did not.  This asserts the state transition the
    // seam performs.
    GrossLosState s;
    const auto hot = networkGrossLosAssess(grossLos(), 8.0, 2, 3.0);
    ASSERT_EQ(networkGrossLosStep(s, hot, 1, 1, 100.0, true), GrossLosEdge::Declare);
    ASSERT_TRUE(s.gross);
    // --- the seam's release, verbatim ---
    s.gross = false; s.over_streak = 0; s.clear_streak = 0;
    EXPECT_FALSE(s.gross);
    // And with the satellite still down the line of sight cannot re-declare.
    EXPECT_EQ(networkGrossLosStep(s, hot, 1, 1, 130.0, /*allow_declare=*/false),
              GrossLosEdge::Suppressed);
    EXPECT_FALSE(s.gross);
}

TEST(NetworkGrossLosGl2, TheTriggerIsTheOverBarCountNotTheDeclaredCount) {
    // s24.19: "(or its over-bar station count reaches it)".  The satellite rule tallies
    // the epoch's OVER-BAR verdicts, not the declarations, so a satellite is caught on
    // its first bad epoch rather than after the per-LOS persistence gate has spent two
    // - which is the difference between catching G17 at tow 271170 and catching it at
    // 271200, after four lines of sight have already been declared and de-weighted.
    const auto hot = networkGrossLosAssess(grossLos(), 8.0, 2, 3.0);
    EXPECT_TRUE(hot.over);          // over-bar on the FIRST epoch
    GrossLosState s;
    // ... while the declaration still needs two.
    EXPECT_EQ(networkGrossLosStep(s, hot, 2, 2, 100.0, true), GrossLosEdge::None);
    EXPECT_FALSE(s.gross);
}

// ---------------------------------------------------------------------------
// The config gate — OFF is OFF
// ---------------------------------------------------------------------------

TEST(NetworkGrossLosConfig, DefaultsAreOffAndPreCard) {
    LogManager log; GnssConfig cfg;
    ASSERT_TRUE(loadWith("", cfg, log, "off"));
    const auto& nc = cfg.getNetworkConfig();
    // The master is OFF, so the seam is never entered, no LOS state is allocated,
    // NetStation::gl_code_sigma_scale is never populated and every row keeps its
    // exact cwiseInverse weight (standing invariant #6).
    EXPECT_FALSE(nc.gross_los_screen);
    // The companions keep their documented values so a later arm is a ONE-knob
    // change rather than a posture change.
    EXPECT_DOUBLE_EQ(nc.gross_los_k, 8.0);
    EXPECT_EQ(nc.gross_los_min_rows, 2);
    EXPECT_EQ(nc.gross_los_epochs, 2);
    EXPECT_DOUBLE_EQ(nc.gross_los_clear_sigma, 3.0);
    EXPECT_EQ(nc.gross_los_clear_epochs, 2);
    EXPECT_DOUBLE_EQ(nc.gross_los_deweight, 100.0);
    EXPECT_DOUBLE_EQ(nc.gross_los_max_gap_s, 900.0);
    EXPECT_EQ(nc.gross_los_event_holdoff_ep, 0);
    // The two GUARDS, both measured (the cert smoke's ignition blow-up and the
    // per-station over-bar fraction on the 6 h field bed).
    EXPECT_EQ(nc.gross_los_warmup_epochs, 120);
    EXPECT_DOUBLE_EQ(nc.gross_los_max_station_frac, 0.5);
    // GL-2 is its own master and is OFF by default: no satellite tally is built,
    // no satellite state is allocated, nothing is pruned from an emission.
    EXPECT_FALSE(nc.gross_los_sat_standdown);
    EXPECT_EQ(nc.gross_los_sat_min_stations, 3);   // == network.raim_min_stations
    EXPECT_TRUE(nc.gross_los_sat_wire_withhold);   // inert while the master is off
}

TEST(NetworkGrossLosConfig, ArmedValuesAreParsed) {
    LogManager log; GnssConfig cfg;
    ASSERT_TRUE(loadWith("    \"gross_los_screen\": true,\n"
                         "    \"gross_los_k\": 6.5,\n"
                         "    \"gross_los_min_rows\": 3,\n"
                         "    \"gross_los_epochs\": 4,\n"
                         "    \"gross_los_clear_sigma\": 2.5,\n"
                         "    \"gross_los_clear_epochs\": 5,\n"
                         "    \"gross_los_deweight\": 50.0,\n"
                         "    \"gross_los_max_gap_s\": 600.0,\n"
                         "    \"gross_los_event_holdoff_ep\": 7,\n"
                         "    \"gross_los_warmup_epochs\": 240,\n"
                         "    \"gross_los_max_station_frac\": 0.75,\n"
                         "    \"gross_los_sat_standdown\": true,\n"
                         "    \"gross_los_sat_min_stations\": 5,\n"
                         "    \"gross_los_sat_wire_withhold\": false,\n",
                         cfg, log, "on"));
    const auto& nc = cfg.getNetworkConfig();
    EXPECT_TRUE(nc.gross_los_screen);
    EXPECT_DOUBLE_EQ(nc.gross_los_k, 6.5);
    EXPECT_EQ(nc.gross_los_min_rows, 3);
    EXPECT_EQ(nc.gross_los_epochs, 4);
    EXPECT_DOUBLE_EQ(nc.gross_los_clear_sigma, 2.5);
    EXPECT_EQ(nc.gross_los_clear_epochs, 5);
    EXPECT_DOUBLE_EQ(nc.gross_los_deweight, 50.0);
    EXPECT_DOUBLE_EQ(nc.gross_los_max_gap_s, 600.0);
    EXPECT_EQ(nc.gross_los_event_holdoff_ep, 7);
    EXPECT_EQ(nc.gross_los_warmup_epochs, 240);
    EXPECT_DOUBLE_EQ(nc.gross_los_max_station_frac, 0.75);
    EXPECT_TRUE(nc.gross_los_sat_standdown);
    EXPECT_EQ(nc.gross_los_sat_min_stations, 5);
    EXPECT_FALSE(nc.gross_los_sat_wire_withhold);
}

TEST(NetworkGrossLosConfig, NonsenseKnobsAreClampedLoudly) {
    // A screen configured with k ≤ 0 or a deweight ≤ 1 is not a gentler screen: it
    // declares everything, or responds with nothing, and either reads in the
    // run-end counters exactly like a clean negative result.  Clamp, never accept.
    LogManager log; GnssConfig cfg;
    ASSERT_TRUE(loadWith("    \"gross_los_screen\": true,\n"
                         "    \"gross_los_k\": -1.0,\n"
                         "    \"gross_los_min_rows\": 0,\n"
                         "    \"gross_los_epochs\": -3,\n"
                         "    \"gross_los_clear_epochs\": 0,\n"
                         "    \"gross_los_deweight\": 0.5,\n"
                         "    \"gross_los_warmup_epochs\": -5,\n"
                         "    \"gross_los_max_station_frac\": 0.0,\n"
                         "    \"gross_los_sat_standdown\": true,\n"
                         "    \"gross_los_sat_min_stations\": 1,\n",
                         cfg, log, "clamp"));
    const auto& nc = cfg.getNetworkConfig();
    EXPECT_DOUBLE_EQ(nc.gross_los_k, 1.0);
    EXPECT_EQ(nc.gross_los_min_rows, 1);
    EXPECT_EQ(nc.gross_los_epochs, 1);
    EXPECT_EQ(nc.gross_los_clear_epochs, 1);
    EXPECT_DOUBLE_EQ(nc.gross_los_deweight, 1.0);
    EXPECT_EQ(nc.gross_los_warmup_epochs, 0);
    // A zero fraction would stand the screen down at EVERY station, i.e. disarm it
    // while reading as armed - clamped to 1.0 (guard off) with a warning, never
    // accepted as "a very strict guard".
    EXPECT_DOUBLE_EQ(nc.gross_los_max_station_frac, 1.0);
    // One station cannot express a CROSS-STATION agreement: at 1 the satellite
    // rule would stand a satellite down on a single bad line of sight, which is
    // the opposite of what it is for.  Clamped to 2, loudly.
    EXPECT_EQ(nc.gross_los_sat_min_stations, 2);
}

// ===========================================================================
// RF-3b (docs/refound/RF-3_code_row_qc_2026-09-22.md §4) — the screen made
// complete.  Four sub-rules, each default = today's behaviour.
// ===========================================================================

// B1 (F1).  The constant's rationale holds for a line of sight that HAS several code
// rows and is false for one that has lost every signal but a single band-1 code, where
// the bar is unsatisfiable by construction.  The rule keeps both halves.
TEST(NetworkGrossLosRf3b, AvailableRuleReachesAOneRowLosAndKeepsTwoForAThreeRowOne) {
    const std::vector<GrossLosSample> one = { row("C1C", 201.8, 112.5) };
    // FIXED: a one-row line of sight can never clear a bar of 2 — this is exactly why
    // four of the six largest episodes of the RF-3 read were unreachable.
    const GrossLosVerdict f = networkGrossLosAssess(one, 8.0, 2, 3.0, /*available=*/false);
    EXPECT_FALSE(f.over);
    EXPECT_EQ(f.min_rows_eff, 2);
    EXPECT_TRUE(f.short_over) << "the population must be COUNTED under the fixed rule "
                                 "too, or an unarmed run cannot report what it missed";
    // AVAILABLE: the bar is min(2, 1) = 1 and the line of sight declares.
    const GrossLosVerdict a = networkGrossLosAssess(one, 8.0, 2, 3.0, /*available=*/true);
    EXPECT_TRUE(a.over);
    EXPECT_EQ(a.min_rows_eff, 1);
    EXPECT_TRUE(a.short_over);

    // A three-row line of sight keeps the multi-band corroboration under BOTH rules:
    // one loud row out of three is one band's multipath, and is refused.
    const std::vector<GrossLosSample> three = { row("C1C", 158.0, -142.0),
                                               row("C1W", 1.2, -1.0),
                                               row("C2W", 0.9, -0.8) };
    for (bool avail : { false, true }) {
        const GrossLosVerdict d = networkGrossLosAssess(three, 8.0, 2, 3.0, avail);
        EXPECT_FALSE(d.over) << "available must not become min_rows = 1 on a line of "
                                "sight that HAS the rows to corroborate with";
        EXPECT_EQ(d.min_rows_eff, 2);
        EXPECT_FALSE(d.short_over);
    }
    // Two of the three over the bar declares under both rules, as it always did.
    const std::vector<GrossLosSample> two_over = { row("C1C", 158.0, -142.0),
                                                  row("C1W", 91.0, -82.0),
                                                  row("C2W", 0.9, -0.8) };
    EXPECT_TRUE(networkGrossLosAssess(two_over, 8.0, 2, 3.0, false).over);
    EXPECT_TRUE(networkGrossLosAssess(two_over, 8.0, 2, 3.0, true).over);
}

namespace {

/// One station carrying one line of sight with one signal — enough for the RESPONSE
/// half of the screen to have something to reach.
StationAssembly oneLosAssembly(const std::string& sat, const std::string& sig) {
    StationAssembly R;
    R.ok = true;
    PlAmb pa;
    pa.sat      = sat;
    pa.signal   = sig;
    pa.amb_key  = sat + "_" + sig;
    pa.col      = 0;
    pa.elev_deg = 30.0;
    R.pl_amb.push_back(pa);
    return R;
}

} // namespace

// B2 (F2), standing invariant #7's decoupling edge.  With the bound armed a declaration
// FREEZES the line of sight's carried level and iono at their pre-declaration values
// rather than erasing them, and re-applies them over whatever the harvest re-stashed —
// which is what keeps the folded code row's residual a measure of the fault.
TEST(NetworkGrossLosRf3b, BoundHoldsTheFreedStatesInsteadOfErasingThem) {
    LogManager log; GnssConfig cfg;
    ASSERT_TRUE(loadWith("    \"gross_los_screen\": true,\n"
                         "    \"gross_los_bound\": true,\n", cfg, log, "bound"));
    EXPECT_TRUE(cfg.getNetworkConfig().gross_los_bound);
    // The knobs are resolved from the config by the context's constructor, so the
    // arm above IS the arm under test — a test that poked ctx.knobs directly could
    // pass with a loader that never sets them.
    NetworkRunContext ctx(cfg, cfg.getNetworkConfig());
    ASSERT_TRUE(ctx.knobs.gl_on);
    ASSERT_TRUE(ctx.knobs.gl_bound);

    std::vector<NetStation> stations(1);
    stations[0].name = "STA1";
    std::vector<StationAssembly> asm_res{ oneLosAssembly("G13", "L1C") };

    const auto lk = std::make_tuple(0, std::string("G13"), std::string("L1C"));
    const std::pair<int, std::string> ik{ 0, "G13" };
    LocalCarry lc{ 42.5, 1.0e-6, 100.0 };
    lc.coupling = { CoupledShared{ SharedParamKind::Clock, "G13", "", 0, 0, 'C',
                                   0, 0.8, 1.5, 0.2 } };
    ctx.carry.local_carry[lk] = lc;
    ctx.carry.iono_carry[ik] = LocalCarry{ 3.25, 1.0e-4, 100.0 };

    ctx.carry.gl_act_declare.push_back({ 0, "G13" });
    networkGrossLosAct(ctx, nullptr, stations, asm_res);

    // The entry is STILL THERE, at its pre-declaration value and variance, with the
    // border regression cleared (that half IS the decoupling).
    ASSERT_EQ(ctx.carry.local_carry.count(lk), 1u);
    EXPECT_DOUBLE_EQ(ctx.carry.local_carry[lk].value_abs, 42.5);
    EXPECT_DOUBLE_EQ(ctx.carry.local_carry[lk].var, 1.0e-6);
    EXPECT_TRUE(ctx.carry.local_carry[lk].coupling.empty());
    ASSERT_EQ(ctx.carry.gl_hold_amb.count(lk), 1u);
    EXPECT_EQ(ctx.carry.iono_carry.count(ik), 1u);
    EXPECT_EQ(ctx.counters.gross_los.bound_amb_held, 1);
    EXPECT_EQ(ctx.counters.gross_los.bound_iono_held, 1);

    // A harvest moves the carried value; the next ACT pass must put it back, or the
    // "fixed prior with zero process noise" is nominal only.
    ctx.carry.local_carry[lk].value_abs = 999.0;
    ctx.carry.iono_carry[ik].value_abs = -7.0;
    ctx.carry.gl_act_declare.clear();
    networkGrossLosAct(ctx, nullptr, stations, asm_res);
    EXPECT_DOUBLE_EQ(ctx.carry.local_carry[lk].value_abs, 42.5);
    EXPECT_DOUBLE_EQ(ctx.carry.iono_carry[ik].value_abs, 3.25);
    EXPECT_GE(ctx.counters.gross_los.bound_los_epochs, 1);

    // The RELEASE edge drops the hold, and the states continue from where they were
    // held (standing invariant #7's re-coupling edge).
    ctx.carry.gl_act_release.push_back({ 0, "G13" });
    networkGrossLosAct(ctx, nullptr, stations, asm_res);
    EXPECT_TRUE(ctx.carry.gl_hold_amb.empty());
    EXPECT_TRUE(ctx.carry.gl_hold_iono.empty());
    ctx.carry.local_carry[lk].value_abs = 11.0;
    ctx.carry.gl_act_release.clear();
    networkGrossLosAct(ctx, nullptr, stations, asm_res);
    EXPECT_DOUBLE_EQ(ctx.carry.local_carry[lk].value_abs, 11.0);
}

// The OFF path is today's ERASE, unchanged: the same call with the bound unarmed
// removes the entry and its iono carry.  The two forms are exclusive by construction.
TEST(NetworkGrossLosRf3b, WithoutTheBoundTheResponseIsStillTheErase) {
    LogManager log; GnssConfig cfg;
    ASSERT_TRUE(loadWith("    \"gross_los_screen\": true,\n", cfg, log, "nobound"));
    EXPECT_FALSE(cfg.getNetworkConfig().gross_los_bound);
    NetworkRunContext ctx(cfg, cfg.getNetworkConfig());
    ASSERT_TRUE(ctx.knobs.gl_on);
    ASSERT_FALSE(ctx.knobs.gl_bound);
    std::vector<NetStation> stations(1);
    stations[0].name = "STA1";
    std::vector<StationAssembly> asm_res{ oneLosAssembly("G13", "L1C") };
    const auto lk = std::make_tuple(0, std::string("G13"), std::string("L1C"));
    const std::pair<int, std::string> ik{ 0, "G13" };
    ctx.carry.local_carry[lk] = LocalCarry{ 42.5, 1.0e-6, 100.0 };
    ctx.carry.iono_carry[ik] = LocalCarry{ 3.25, 1.0e-4, 100.0 };
    ctx.carry.gl_act_declare.push_back({ 0, "G13" });
    networkGrossLosAct(ctx, nullptr, stations, asm_res);
    EXPECT_EQ(ctx.carry.local_carry.count(lk), 0u);
    EXPECT_EQ(ctx.carry.iono_carry.count(ik), 0u);
    EXPECT_TRUE(ctx.carry.gl_hold_amb.empty());
    EXPECT_EQ(ctx.counters.gross_los.iono_erased, 1);
}

// B4 (F4).  The station tier is ONE verdict: the deferred station declaration reaches
// every satellite the station was last seen screening, once, and its scale publish
// folds every one of that station's code rows.
TEST(NetworkGrossLosRf3b, StationTierRespondsOnceForTheWholeStation) {
    LogManager log; GnssConfig cfg;
    ASSERT_TRUE(loadWith("    \"gross_los_screen\": true,\n"
                         "    \"gross_los_station_tier\": true,\n", cfg, log, "statier"));
    EXPECT_TRUE(cfg.getNetworkConfig().gross_los_station_tier);
    NetworkRunContext ctx(cfg, cfg.getNetworkConfig());
    ASSERT_TRUE(ctx.knobs.gl_on);
    ASSERT_TRUE(ctx.knobs.gl_station_tier);
    ASSERT_DOUBLE_EQ(ctx.knobs.gl_deweight, 100.0);

    std::vector<NetStation> stations(1);
    stations[0].name = "STA1";
    StationAssembly R = oneLosAssembly("G13", "L1C");
    PlAmb pa2; pa2.sat = "G22"; pa2.signal = "L1C"; pa2.amb_key = "G22_L1C";
    pa2.col = 1; pa2.elev_deg = 40.0;
    R.pl_amb.push_back(pa2);
    std::vector<StationAssembly> asm_res{ R };

    ctx.carry.local_carry[std::make_tuple(0, std::string("G13"), std::string("L1C"))] =
        LocalCarry{ 1.0, 1e-6, 0.0 };
    ctx.carry.local_carry[std::make_tuple(0, std::string("G22"), std::string("L1C"))] =
        LocalCarry{ 2.0, 1e-6, 0.0 };
    auto& sst = ctx.carry.gl_sta_state[0];
    sst.gross = true;
    sst.sats = { "G13", "G22" };
    ctx.carry.gl_act_station.push_back({ 0, true });
    networkGrossLosAct(ctx, nullptr, stations, asm_res);
    // One verdict, both lines of sight answered — not two sequential declarations.
    EXPECT_EQ(ctx.carry.local_carry.size(), 0u);
    EXPECT_EQ(ctx.counters.gross_los.amb_refloated, 2);
    // The publish half folds EVERY satellite the station tracks.
    networkGrossLosPublishScales(ctx, stations);
    EXPECT_EQ(stations[0].gl_code_sigma_scale.size(), 2u);
    EXPECT_DOUBLE_EQ(stations[0].gl_code_sigma_scale["G13"], 100.0);
    EXPECT_DOUBLE_EQ(stations[0].gl_code_sigma_scale["G22"], 100.0);
    // Released, the station stops folding anything.
    sst.gross = false;
    networkGrossLosPublishScales(ctx, stations);
    EXPECT_TRUE(stations[0].gl_code_sigma_scale.empty());
}

// All four sub-rules parse, and all four default to today's behaviour so an existing
// arm is a ONE-knob change rather than a posture change.
TEST(NetworkGrossLosRf3b, SubRulesParseAndDefaultToTodaysBehaviour) {
    LogManager log; GnssConfig cfg;
    ASSERT_TRUE(loadWith("", cfg, log, "rf3b_def"));
    const auto& d = cfg.getNetworkConfig();
    EXPECT_EQ(d.gross_los_min_rows_rule, NetworkGrossLosMinRowsRule::Fixed);
    EXPECT_FALSE(d.gross_los_bound);
    EXPECT_FALSE(d.gross_los_emit_exclude);
    EXPECT_FALSE(d.gross_los_station_tier);

    LogManager log2; GnssConfig cfg2;
    ASSERT_TRUE(loadWith("    \"gross_los_screen\": true,\n"
                         "    \"gross_los_min_rows_rule\": \"available\",\n"
                         "    \"gross_los_bound\": true,\n"
                         "    \"gross_los_emit_exclude\": true,\n"
                         "    \"gross_los_station_tier\": true,\n",
                         cfg2, log2, "rf3b_on"));
    const auto& a = cfg2.getNetworkConfig();
    EXPECT_EQ(a.gross_los_min_rows_rule, NetworkGrossLosMinRowsRule::Available);
    EXPECT_TRUE(a.gross_los_bound);
    EXPECT_TRUE(a.gross_los_emit_exclude);
    EXPECT_TRUE(a.gross_los_station_tier);

    // A typo falls back to the pre-card rule rather than to an invented one: shipping
    // the unreachable half of the population silently is the failure this guards.
    LogManager log3; GnssConfig cfg3;
    ASSERT_TRUE(loadWith("    \"gross_los_screen\": true,\n"
                         "    \"gross_los_min_rows_rule\": \"availble\",\n",
                         cfg3, log3, "rf3b_typo"));
    EXPECT_EQ(cfg3.getNetworkConfig().gross_los_min_rows_rule,
              NetworkGrossLosMinRowsRule::Fixed);
}
