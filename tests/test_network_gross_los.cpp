/**
 * @file test_network_gross_los.cpp
 * @brief The gross line-of-sight screen's two pure rules, its act half and its
 *        config gate (docs/SPEC_event_harness.md §24.18) (card GL).
 *
 * The screen exists because every post-fit tier of this producer pools
 * carried-ambiguity phase rows: networkPostFitRaim (NetworkRaim.cpp) admits a
 * row by `|A_r(row, amb_col)| > 0.5`, which is +1 on a phase row and exactly 0
 * on a code row.  A gross code error on one line of sight is therefore not
 * under-weighted there but unreachable.  §24.18 records the field cases: one
 * line of sight 75 to 142 m off on three code signals for 31 epochs while
 * `network_raim.csv` filed zero rows, and a station driven to a 54 m code rms.
 *
 * This suite exercises what can be exercised without a network: the two
 * decision rules, which are pure functions for this reason, the act half on
 * hand-built station assemblies, and the loader.  What a declaration does to a real line of sight
 * is graded by the 6 h arm the SPEC records, not here (the suite guards
 * structure and non-regression; it does not validate estimation accuracy).
 *
 * The file is strict about three points:
 *  - a declaration needs both the row count and the persistence; either one
 *    alone is not enough;
 *  - a clear needs the release bar, not merely the absence of the declaration
 *    bar.  The band between the two is the hysteresis, and a line of sight in
 *    it keeps its disposition;
 *  - with the knob off nothing changes: the knob defaults to false and every
 *    companion knob keeps its legacy value, so an unarmed run cannot allocate
 *    the state or move a row weight (invariant #6).
 */
#include <gtest/gtest.h>

#include "app/NetworkRunContext.hpp"     // the act half's carry + counters (RF-3b)
#include "config/GnssConfig.hpp"
#include "filter/NetworkGrossLos.hpp"
#include "filter/NetworkRunTypes.hpp"    // NetStation / StationAssembly / LocalCarry (RF-3b)
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

/// The DJIG00DJI/G06 shape §24.18 names: three code signals, all far over the bar.
/// A row at σ ≈ 0.9 m and v = −142 m stands at ~158σ; the numbers below follow
/// that shape, not the exact arc.
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
// The per-epoch rule: networkGrossLosAssess
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
    // range error and is wrong on every code band at once.  min_rows separates
    // the two, and a single row cannot satisfy it.
    const std::vector<GrossLosSample> cell = {
        row("C1W", 40.0, -36.0), row("C2W", 1.1, -1.0), row("C5Q", 0.7, -0.6)};
    const auto d = networkGrossLosAssess(cell, 8.0, 2, 3.0);
    EXPECT_FALSE(d.over);
    EXPECT_EQ(d.n_over, 1);
    // Not clean either: the large row is above the release bar, so the line of
    // sight is in neither camp and any declared disposition survives the epoch.
    EXPECT_FALSE(d.under_clear);
}

TEST(NetworkGrossLosRule, EmptySignalLabelsStillCount) {
    // A code row of a posture whose satellite code biases are supplied externally,
    // and the W6 datum signal's rows even when they are estimated, carry no CB:
    // column and therefore no signal label.  Counting distinct labels instead of
    // rows would leave the screen inert, without a symptom, on those postures (the
    // failure mode the card addresses), so the rows count and the empty labels are
    // left out of the event detail.
    const std::vector<GrossLosSample> cell = {
        row("", 30.0, -27.0), row("", 25.0, -22.0)};
    const auto d = networkGrossLosAssess(cell, 8.0, 2, 3.0);
    EXPECT_TRUE(d.over);
    EXPECT_EQ(d.n_over, 2);
    EXPECT_EQ(d.signals, "");
}

TEST(NetworkGrossLosRule, HysteresisBandIsNeitherDeclareNorClear) {
    // 5σ sits above the 3σ release bar and below the 8σ declaration bar.  The
    // verdict refuses both edges: this gap stops a line of sight near the bar
    // from alternating declare/clear every other epoch.
    const std::vector<GrossLosSample> cell = {row("C1W", 5.0, 4.5), row("C2W", 4.2, 3.8)};
    const auto d = networkGrossLosAssess(cell, 8.0, 2, 3.0);
    EXPECT_FALSE(d.over);
    EXPECT_FALSE(d.under_clear);
}

TEST(NetworkGrossLosRule, AbsenceIsNotEvidenceOfHealth) {
    // No code rows this epoch.  `under_clear` stays false: a response is never
    // lifted on the absence of evidence (invariant #1).  The seam handles a
    // vanished line of sight through the expiry edge instead.
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
// The state machine: networkGrossLosStep
// ---------------------------------------------------------------------------

TEST(NetworkGrossLosState, DeclarationNeedsTwoConsecutiveEpochs) {
    GrossLosState s;
    const auto hot = networkGrossLosAssess(grossLos(), 8.0, 2, 3.0);
    // Epoch 1: evidence, no edge yet.  The persistence gate keeps a heavy-tailed
    // single-epoch spike from declaring.
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

    // An epoch in the hysteresis band does not advance the clear.
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
    // intervening gross epoch would re-couple a line of sight that is still
    // faulty, i.e. invariant #7's re-coupling edge failing open.
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
    // A zero or negative persistence count does not mean "declare on nothing": the
    // rule clamps it to 1, i.e. act on the first qualifying epoch.
    GrossLosState s;
    const auto hot = networkGrossLosAssess(grossLos(), 8.0, 2, 3.0);
    EXPECT_EQ(networkGrossLosStep(s, hot, 0, 0, 100.0), GrossLosEdge::Declare);
}

// ---------------------------------------------------------------------------
// The deweight undo: the response must not satisfy its own release test
// ---------------------------------------------------------------------------

TEST(NetworkGrossLosRule, DeweightedRowsAreRestoredToTheirNominalSigma) {
    // A declared line of sight's code rows are folded at sigma x 100.  Graded at
    // that weight, a line of sight with |v| still ~144 m reads ~1.28 sigma, under
    // the 3 sigma release bar: the screen would clear it, the deweight would lift,
    // and it would re-declare two epochs later.  That limit cycle makes the
    // release true by construction and erases the line of sight's carried level
    // on each pass (DJIG00DJI/G06 in the field, where it read 125.6 sigma at
    // nominal weight when declared).
    //
    // 1.2789 x 100 = 127.89, inside the 125-130 sigma band the arc sat in, so the
    // undo recovers the number the data supports, not the one the response
    // produced.
    EXPECT_NEAR(networkGrossLosUndoDeweight(1.27894, 100.0), 127.894, 1e-9);
    // No fold (absent key, or a scale of exactly 1) is an exact no-op: the undo
    // never touches an undeclared line of sight's statistic.
    EXPECT_DOUBLE_EQ(networkGrossLosUndoDeweight(4.25, 1.0), 4.25);
    EXPECT_DOUBLE_EQ(networkGrossLosUndoDeweight(4.25, 0.5), 4.25);   // never inflates a row
    EXPECT_DOUBLE_EQ(networkGrossLosUndoDeweight(4.25, std::numeric_limits<double>::quiet_NaN()),
                     4.25);
}

TEST(NetworkGrossLosRule, TheUndoIsWhatKeepsADeclaredLineOfSightDeclared) {
    // The same three rows, folded and unfolded, through the production rule.
    // Folded, every row sits under the release bar and the verdict is release;
    // unfolded, the verdict matches the declaration epoch.  This guards the
    // limit cycle described above.
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
// The two guards: both measured, both on the same state machine
// ---------------------------------------------------------------------------

TEST(NetworkGrossLosState, AGuardHoldsTheDeclarationAndSaysSo) {
    // The maturity horizon and the whole-station stand-down both reach the machine
    // through `allow_declare`.  A held declaration is Suppressed, never None: the
    // evidence was complete and a guard refused it, and that population has its own
    // run-end counter so that it is always visible (invariant #4).
    GrossLosState s;
    const auto hot = networkGrossLosAssess(grossLos(), 8.0, 2, 3.0);
    EXPECT_EQ(networkGrossLosStep(s, hot, 2, 2, 100.0, /*allow_declare=*/false),
              GrossLosEdge::None);                       // streak 1 of 2 - not yet complete
    EXPECT_EQ(networkGrossLosStep(s, hot, 2, 2, 130.0, false), GrossLosEdge::Suppressed);
    EXPECT_FALSE(s.gross);
    EXPECT_EQ(s.n_declared, 0);
}

TEST(NetworkGrossLosState, HeldEvidenceIsNeitherDiscardedNorAllowedToBurst) {
    // The streak is held at the requirement, neither zeroed nor growing.  Zeroing
    // would discard the evidence the guard held back; growing would turn the gate's
    // opening into a burst of simultaneous declarations across every line of sight
    // that waited.
    GrossLosState s;
    const auto hot = networkGrossLosAssess(grossLos(), 8.0, 2, 3.0);
    for (int i = 0; i < 40; ++i)
        networkGrossLosStep(s, hot, 2, 2, 100.0 + 30.0 * i, false);
    EXPECT_EQ(s.over_streak, 2);          // held AT the requirement, not 40
    EXPECT_FALSE(s.gross);
    // The gate opens: one qualifying epoch declares.
    EXPECT_EQ(networkGrossLosStep(s, hot, 2, 2, 1400.0, true), GrossLosEdge::Declare);
    EXPECT_TRUE(s.gross);
}

TEST(NetworkGrossLosState, AGuardNeverBlocksTheReleaseEdge) {
    // Invariant #7's re-coupling edge must not fail closed: a guard must not be
    // able to strand a declared response with no way to release it.
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
// The whole-satellite stand-down (GL-2; SPEC_event_harness §24.19)
// ---------------------------------------------------------------------------

TEST(NetworkGrossLosGl2, TheSatelliteRuleIsTheSameStepWithTheGuardClosed) {
    // The satellite rule (GL-2) reaches the machine through the same `allow_declare`
    // the maturity horizon and the whole-station stand-down use, so there is one
    // place a declaration can be refused and one population (Suppressed) that
    // records it.  A satellite over the bar at many stations is a satellite fault:
    // its lines of sight are not declared, and their evidence is kept.
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
    // The seam releases a standing per-LOS declaration when its satellite is stood
    // down: it clears `gross` so the deweight lifts at the next assembly.  This is
    // invariant #7's re-coupling edge at the satellite's scale: the fault is not this
    // line of sight's, so holding it decoupled would keep removing code the solution
    // needs.  The release is not the per-LOS clear edge, which asserts that the data
    // recovered; here it did not.  The test asserts the state transition the seam
    // performs.
    GrossLosState s;
    const auto hot = networkGrossLosAssess(grossLos(), 8.0, 2, 3.0);
    ASSERT_EQ(networkGrossLosStep(s, hot, 1, 1, 100.0, true), GrossLosEdge::Declare);
    ASSERT_TRUE(s.gross);
    // --- the seam's release, copied verbatim ---
    s.gross = false; s.over_streak = 0; s.clear_streak = 0;
    EXPECT_FALSE(s.gross);
    // With the satellite still down the line of sight cannot re-declare.
    EXPECT_EQ(networkGrossLosStep(s, hot, 1, 1, 130.0, /*allow_declare=*/false),
              GrossLosEdge::Suppressed);
    EXPECT_FALSE(s.gross);
}

TEST(NetworkGrossLosGl2, TheTriggerIsTheOverBarCountNotTheDeclaredCount) {
    // Per §24.19, "(or its over-bar station count reaches it)".  The satellite rule tallies
    // the epoch's over-bar verdicts, not the declarations, so a satellite is caught on
    // its first bad epoch rather than after the per-LOS persistence gate has spent two.
    // Waiting for the declarations would let several of its lines of sight be declared
    // and deweighted first (four, for G17 in the field).
    const auto hot = networkGrossLosAssess(grossLos(), 8.0, 2, 3.0);
    EXPECT_TRUE(hot.over);          // over-bar on the first epoch
    GrossLosState s;
    // ... while the declaration still needs two.
    EXPECT_EQ(networkGrossLosStep(s, hot, 2, 2, 100.0, true), GrossLosEdge::None);
    EXPECT_FALSE(s.gross);
}

// ---------------------------------------------------------------------------
// The config gate: off changes nothing
// ---------------------------------------------------------------------------

TEST(NetworkGrossLosConfig, DefaultsAreOffAndPreCard) {
    LogManager log; GnssConfig cfg;
    ASSERT_TRUE(loadWith("", cfg, log, "off"));
    const auto& nc = cfg.getNetworkConfig();
    // The master is off, so the seam is never entered, no LOS state is allocated,
    // NetStation::gl_code_sigma_scale is never populated and every row keeps its
    // exact cwiseInverse weight (invariant #6).
    EXPECT_FALSE(nc.gross_los_screen);
    // The companions keep their documented values, so a later arm is a one-knob
    // change rather than a posture change.
    EXPECT_DOUBLE_EQ(nc.gross_los_k, 8.0);
    EXPECT_EQ(nc.gross_los_min_rows, 2);
    EXPECT_EQ(nc.gross_los_epochs, 2);
    EXPECT_DOUBLE_EQ(nc.gross_los_clear_sigma, 3.0);
    EXPECT_EQ(nc.gross_los_clear_epochs, 2);
    EXPECT_DOUBLE_EQ(nc.gross_los_deweight, 100.0);
    EXPECT_DOUBLE_EQ(nc.gross_los_max_gap_s, 900.0);
    EXPECT_EQ(nc.gross_los_event_holdoff_ep, 0);
    // The two guards, both set from measurements (the ignition transient of the
    // certification smoke, and the per-station over-bar fraction on the 6 h field bed).
    EXPECT_EQ(nc.gross_los_warmup_epochs, 120);
    EXPECT_DOUBLE_EQ(nc.gross_los_max_station_frac, 0.5);
    // The satellite rule (GL-2) has its own master, off by default: no satellite
    // tally is built, no satellite state is allocated, nothing is pruned from an
    // emission.
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
    // declares everything, or responds with nothing, and either reads in the run-end
    // counters like a clean negative result.  The loader clamps such values.
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
    // A zero fraction would stand the screen down at every station, i.e. disarm it
    // while it reads as armed.  It is clamped to 1.0 (guard off) with a warning,
    // not accepted as "a very strict guard".
    EXPECT_DOUBLE_EQ(nc.gross_los_max_station_frac, 1.0);
    // One station cannot express a cross-station agreement: at 1 the satellite
    // rule would stand a satellite down on a single bad line of sight, the
    // opposite of its purpose.  Clamped to 2 with a warning.
    EXPECT_EQ(nc.gross_los_sat_min_stations, 2);
}

// ===========================================================================
// Completing the screen: four sub-rules, each defaulting to the legacy behaviour
// (RF-3b; docs/refound/RF-3_code_row_qc_2026-09-22.md §4).
// ===========================================================================

// The fixed min_rows rationale holds for a line of sight that has several code rows and
// fails for one that has lost every signal but a single band-1 code, where the bar is
// unsatisfiable by construction.  The available rule covers both cases (B1, F1).
TEST(NetworkGrossLosRf3b, AvailableRuleReachesAOneRowLosAndKeepsTwoForAThreeRowOne) {
    const std::vector<GrossLosSample> one = { row("C1C", 201.8, 112.5) };
    // Fixed rule: a one-row line of sight can never clear a bar of 2, which is why
    // four of the six largest episodes of the RF-3 read were unreachable.
    const GrossLosVerdict f = networkGrossLosAssess(one, 8.0, 2, 3.0, /*available=*/false);
    EXPECT_FALSE(f.over);
    EXPECT_EQ(f.min_rows_eff, 2);
    EXPECT_TRUE(f.short_over) << "the population must be COUNTED under the fixed rule "
                                 "too, or an unarmed run cannot report what it missed";
    // Available rule: the bar is min(2, 1) = 1 and the line of sight declares.
    const GrossLosVerdict a = networkGrossLosAssess(one, 8.0, 2, 3.0, /*available=*/true);
    EXPECT_TRUE(a.over);
    EXPECT_EQ(a.min_rows_eff, 1);
    EXPECT_TRUE(a.short_over);

    // A three-row line of sight keeps the multi-band corroboration under both rules:
    // one large row out of three is one band's multipath, and is refused.
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

/// One station carrying one line of sight with one signal: enough for the response
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

// Invariant #7's decoupling edge.  With the bound armed, a declaration freezes the line
// of sight's carried level and iono at their pre-declaration values rather than erasing
// them, and re-applies them over whatever the harvest re-stashed, so the folded code
// row's residual stays a measure of the fault (B2, F2).
TEST(NetworkGrossLosRf3b, BoundHoldsTheFreedStatesInsteadOfErasingThem) {
    LogManager log; GnssConfig cfg;
    ASSERT_TRUE(loadWith("    \"gross_los_screen\": true,\n"
                         "    \"gross_los_bound\": true,\n", cfg, log, "bound"));
    EXPECT_TRUE(cfg.getNetworkConfig().gross_los_bound);
    // The context's constructor resolves the knobs from the config, so the arm above
    // is the arm under test; a test that set ctx.knobs directly could pass with a
    // loader that never sets them.
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

    // The entry is still present, at its pre-declaration value and variance, with the
    // border regression cleared (clearing it is the decoupling).
    ASSERT_EQ(ctx.carry.local_carry.count(lk), 1u);
    EXPECT_DOUBLE_EQ(ctx.carry.local_carry[lk].value_abs, 42.5);
    EXPECT_DOUBLE_EQ(ctx.carry.local_carry[lk].var, 1.0e-6);
    EXPECT_TRUE(ctx.carry.local_carry[lk].coupling.empty());
    ASSERT_EQ(ctx.carry.gl_hold_amb.count(lk), 1u);
    EXPECT_EQ(ctx.carry.iono_carry.count(ik), 1u);
    EXPECT_EQ(ctx.counters.gross_los.bound_amb_held, 1);
    EXPECT_EQ(ctx.counters.gross_los.bound_iono_held, 1);

    // A harvest moves the carried value; the next act pass puts it back, otherwise the
    // "fixed prior with zero process noise" would be nominal only.
    ctx.carry.local_carry[lk].value_abs = 999.0;
    ctx.carry.iono_carry[ik].value_abs = -7.0;
    ctx.carry.gl_act_declare.clear();
    networkGrossLosAct(ctx, nullptr, stations, asm_res);
    EXPECT_DOUBLE_EQ(ctx.carry.local_carry[lk].value_abs, 42.5);
    EXPECT_DOUBLE_EQ(ctx.carry.iono_carry[ik].value_abs, 3.25);
    EXPECT_GE(ctx.counters.gross_los.bound_los_epochs, 1);

    // The release edge drops the hold, and the states continue from where they were
    // held (invariant #7's re-coupling edge).
    ctx.carry.gl_act_release.push_back({ 0, "G13" });
    networkGrossLosAct(ctx, nullptr, stations, asm_res);
    EXPECT_TRUE(ctx.carry.gl_hold_amb.empty());
    EXPECT_TRUE(ctx.carry.gl_hold_iono.empty());
    ctx.carry.local_carry[lk].value_abs = 11.0;
    ctx.carry.gl_act_release.clear();
    networkGrossLosAct(ctx, nullptr, stations, asm_res);
    EXPECT_DOUBLE_EQ(ctx.carry.local_carry[lk].value_abs, 11.0);
}

// With the bound unarmed the response is the legacy erase, unchanged: the same call
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

// The station tier is one verdict: the deferred station declaration reaches every
// satellite the station was last seen screening, once, and its scale publish folds
// every one of that station's code rows (B4, F4).
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
    // One verdict answers both lines of sight, not two sequential declarations.
    EXPECT_EQ(ctx.carry.local_carry.size(), 0u);
    EXPECT_EQ(ctx.counters.gross_los.amb_refloated, 2);
    // The publish half folds every satellite the station tracks.
    networkGrossLosPublishScales(ctx, stations);
    EXPECT_EQ(stations[0].gl_code_sigma_scale.size(), 2u);
    EXPECT_DOUBLE_EQ(stations[0].gl_code_sigma_scale["G13"], 100.0);
    EXPECT_DOUBLE_EQ(stations[0].gl_code_sigma_scale["G22"], 100.0);
    // Released, the station stops folding anything.
    sst.gross = false;
    networkGrossLosPublishScales(ctx, stations);
    EXPECT_TRUE(stations[0].gl_code_sigma_scale.empty());
}

// All four sub-rules parse, and all four default to the legacy behaviour, so arming
// one is a one-knob change rather than a posture change.
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

    // A misspelled value falls back to the fixed rule rather than to an invented one,
    // and the loader logs an error, because the fixed rule leaves one-row lines of
    // sight unreachable.
    LogManager log3; GnssConfig cfg3;
    ASSERT_TRUE(loadWith("    \"gross_los_screen\": true,\n"
                         "    \"gross_los_min_rows_rule\": \"availble\",\n",
                         cfg3, log3, "rf3b_typo"));
    EXPECT_EQ(cfg3.getNetworkConfig().gross_los_min_rows_rule,
              NetworkGrossLosMinRowsRule::Fixed);
}
