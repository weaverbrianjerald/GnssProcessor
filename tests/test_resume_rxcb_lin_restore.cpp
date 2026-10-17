/**
 * @file test_resume_rxcb_lin_restore.cpp
 * @brief Contract tests for the rx-CB linearization-point restore (
 *        docs/SPEC_event_harness.md §21.18): the snapshot's format-forward
 *        @c RL record (one code-bias @c x_lin per (station roster index, CB
 *        state key), written beside the RS-6 @c RC row), the loader's side of
 *        the write knob (@c network.state_snapshot_rxcb_lin_capture) and the
 *        read knob (@c network.resume_rxcb_lin_restore), and the seed-override
 *        rule through which the station initializer consumes it.
 *
 * Purpose.  RS-6 restores the rx-CB carry's numerator (@c value_abs / @c var /
 * @c last_seen_s).  The gate that decides whether that prior is folded tests it
 * against the station's code-bias linearization point:
 * @c cb_innov = @c value_abs − @c x_lin(cb), against a fixed 5.13 m bar.
 * @c x_lin is a one-shot seed that @c StateInitializer::initializeCodeBiases
 * writes on the station's first processed epoch and never revises.  On a warm
 * resume with the restore off, that reference is re-derived from the resume
 * epoch's code while the numerator comes from the snapshot, so more stations sit
 * outside the bar at every epoch of a leg than in the continuous run, and lose
 * their receiver-DCB memory for the whole leg (§21.18(a)/(b)).  In terms of
 * invariant #7: the re-coupling edge re-validates the value it reconnects, but
 * not the frame that value is judged against.
 *
 * Checked here:
 *   (a) both knobs default off, and the write knob is disarmed with a warning
 *       without its RS-6 station capture (an RL row with no RC row would restore
 *       a reference for a carry that is not there);
 *   (b) the three loader refusals: no @c resume_station_restore, its @c _rxcb
 *       half false, and @c network.rx_code_bias_datum armed alongside (that
 *       datum's RHS is η = λ(0 − x_lin), so moving @c x_lin moves the solve and
 *       the restore is no longer the exact no-op that justifies it);
 *   (c) @c RL rows round-trip through the snapshot text at full precision, and a
 *       malformed or non-finite one is a hard parse failure;
 *   (d) the seed-override rule (a carried key takes the writer's value, any
 *       other key falls through to the one-epoch median seed) and the scope of
 *       the @c uncovered counter: only a key the map carries a reference for
 *       (one with an rx-CB carry, whose @c x_lin the gate reads) can be a miss;
 *   (e) a snapshot with no @c RL rows restores nothing (an old file keeps the
 *       earlier behaviour, and the container stays empty).
 *
 * The seam mechanics (the withheld share falling to the continuous run's, and
 * the code-bias / clock / ZWD consequences at the withholding stations) are
 * graded on the harness legs, not here.
 */
#include <gtest/gtest.h>

#include "config/GnssConfig.hpp"
#include "logging/LogManager.hpp"
#include "measurement/StateInitializer.hpp"
#include "products/NetworkStateSnapshot.hpp"

#include <cmath>
#include <filesystem>
#include <fstream>
#include <limits>
#include <map>
#include <sstream>
#include <string>
#include <utility>
#include <vector>
#include "support/TestTempDir.hpp"   // per-process, per-test scratch directory

namespace fs = std::filesystem;
using namespace GnssProcessor;

namespace {

NetworkStateSnapshot baseSnapshot() {
    NetworkStateSnapshot s;
    s.snap_week = 2313;
    s.snap_tow  = 277290.0;
    s.wall_utc  = "2026-09-13T00:00:00Z";
    s.generation_id = 1399161629u;
    s.config_fingerprint = 0x545b8bdd2b638231ull;
    s.n_emitted = 604;
    s.prev_week = 2313;
    s.prev_tow  = 277290.0;
    return s;
}

/// A snapshot carrying the RS-6 carry rows together with their RS-9 references,
/// the only pairing the loader permits.  The values are chosen so a
/// shortest-round-trip decimal is a different string from a rounded one.
NetworkStateSnapshot linSnapshot() {
    NetworkStateSnapshot s = baseSnapshot();
    s.rx_cb_carry[{0, "GPS_DCB"}] =
        NetworkStateSnapshot::Local{-3.8391234567890123, 1.2345678901234e-04, 1400277290.0};
    s.rx_cb_carry[{7, "GPS_IFB5"}] =
        NetworkStateSnapshot::Local{0.0625, 3.0517578125e-05, 1400277260.0};
    // The reference each of those is differenced against, deliberately not equal
    // to value_abs.  Seeding x_lin := value_abs is the variant §21.18 rejects: it
    // drives the leg's withheld share far below the seed run's and breaks the
    // leg↔seed comparability on which every RS gate is read.
    s.rx_cb_lin[{0, "GPS_DCB"}]  = -3.7654321098765432;
    s.rx_cb_lin[{7, "GPS_IFB5"}] = 0.109375;
    return s;
}

fs::path writeTempConfig(const std::string& tag, const std::string& body) {
    const fs::path p = gnss_test::tempDir() / ("resumerxcblin_cfg_" + tag + ".json");
    std::ofstream f(p);
    f << body;
    f.close();
    return p;
}

std::string networkConfig(const std::string& extra = "") {
    return std::string("{\n")
        + "  \"processing\": {\n"
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
          "}\n";
}

bool loadInto(const std::string& tag, const std::string& body, GnssConfig& cfg) {
    LogManager log;
    const fs::path p = writeTempConfig(tag, body);
    const bool ok = cfg.loadFromJson(p, log);
    std::error_code ec; fs::remove(p, ec);
    return ok;
}

std::string serialized(const NetworkStateSnapshot& s) {
    std::ostringstream os;
    NetworkStateSnapshotIO::serialize(os, s);
    return os.str();
}

/// The armed read posture: the three prerequisites the loader enforces, plus the
/// capture side, so an A/B config is one literal.
const char* kArmedRead =
    "    \"state_snapshot_path\": \"state_snapshot.snap\",\n"
    "    \"state_snapshot_frame_capture\": true,\n"
    "    \"state_snapshot_station_capture\": true,\n"
    "    \"state_snapshot_rxcb_lin_capture\": true,\n"
    "    \"resume_frame_restore\": true,\n"
    "    \"resume_station_restore\": true,\n"
    "    \"resume_rxcb_lin_restore\": true,\n";

} // namespace

// ---------------------------------------------------------------------------
// (a) defaults + the capture knob's prerequisite
// ---------------------------------------------------------------------------

TEST(ResumeRxCbLinRestore, BothKnobsDefaultOff) {
    GnssConfig cfg;
    ASSERT_TRUE(loadInto("defaults", networkConfig(), cfg));
    EXPECT_FALSE(cfg.getNetworkConfig().state_snapshot_rxcb_lin_capture);
    EXPECT_FALSE(cfg.networkResumeRxCbLinRestore());
}

/// The RL row is the reference the RC row's gate differences against, so writing
/// it without the carry it judges would restore a frame for a carry that is not
/// there.  The loader warns and disarms (the RS-7 shared_carry_full_cov
/// precedent) rather than throwing, and the file stays as it was.
TEST(ResumeRxCbLinRestore, CaptureWithoutTheStationCaptureIsDisarmed) {
    GnssConfig cfg;
    ASSERT_TRUE(loadInto("capnostation",
        networkConfig("    \"state_snapshot_path\": \"state_snapshot.snap\",\n"
                      "    \"state_snapshot_rxcb_lin_capture\": true,\n"), cfg));
    EXPECT_FALSE(cfg.getNetworkConfig().state_snapshot_rxcb_lin_capture);
}

TEST(ResumeRxCbLinRestore, CaptureWithTheStationCaptureParsesAndDoesNotArmTheReadSide) {
    GnssConfig cfg;
    ASSERT_TRUE(loadInto("capture",
        networkConfig("    \"state_snapshot_path\": \"state_snapshot.snap\",\n"
                      "    \"state_snapshot_station_capture\": true,\n"
                      "    \"state_snapshot_rxcb_lin_capture\": true,\n"), cfg));
    EXPECT_TRUE(cfg.getNetworkConfig().state_snapshot_rxcb_lin_capture);
    EXPECT_FALSE(cfg.networkResumeRxCbLinRestore());
}

// ---------------------------------------------------------------------------
// (b) the three loader refusals
// ---------------------------------------------------------------------------

/// Restoring the reference without the carry is the §21.18 defect inverted: the
/// gate would judge a freshly harvested value against the writer's frame.
TEST(ResumeRxCbLinRestore, RestoreWithoutTheStationRestoreIsRejected) {
    // The loader turns its own throw into a failed load (loadFromJson catches and
    // returns false), the form in which every other restore prerequisite is asserted.
    GnssConfig cfg;
    EXPECT_FALSE(loadInto("nostation",
        networkConfig("    \"state_snapshot_path\": \"state_snapshot.snap\",\n"
                      "    \"state_snapshot_frame_capture\": true,\n"
                      "    \"resume_frame_restore\": true,\n"
                      "    \"resume_rxcb_lin_restore\": true,\n"), cfg));
}

/// The rxcb sub-switch is what restores the carry itself; with it false the
/// station restore is armed and the rx-CB half is not, so the same inversion
/// applies one level down.
TEST(ResumeRxCbLinRestore, RestoreWithTheRxCbSubSwitchOffIsRejected) {
    GnssConfig cfg;
    EXPECT_FALSE(loadInto("norxcb",
        networkConfig(std::string(kArmedRead)
                      + "    \"resume_station_restore_rxcb\": false,\n"), cfg));
}

/// The restore is permitted only because an x_lin shift is an exact solve no-op
/// for a state that enters linearly, so the gate is its only consumer.  The
/// receiver code-bias datum breaks that: its prior's RHS is η = λ(0 − x_lin), so
/// moving x_lin moves the target and therefore the solve.
TEST(ResumeRxCbLinRestore, RestoreWithTheReceiverCodeBiasDatumIsRejected) {
    GnssConfig cfg;
    EXPECT_FALSE(loadInto("withdatum",
        networkConfig(std::string(kArmedRead)
                      + "    \"rx_code_bias_datum\": true,\n"), cfg));
}

TEST(ResumeRxCbLinRestore, ArmedPostureLoads) {
    GnssConfig cfg;
    ASSERT_TRUE(loadInto("armed", networkConfig(kArmedRead), cfg));
    EXPECT_TRUE(cfg.getNetworkConfig().state_snapshot_rxcb_lin_capture);
    EXPECT_TRUE(cfg.networkResumeRxCbLinRestore());
    EXPECT_TRUE(cfg.networkResumeStationRestore());
    EXPECT_TRUE(cfg.networkResumeStationRestoreRxCb());
    EXPECT_FALSE(cfg.networkRxCodeBiasDatum());
}

// ---------------------------------------------------------------------------
// (c) the snapshot text round-trip
// ---------------------------------------------------------------------------

TEST(ResumeRxCbLinRestore, RlRowsRoundTripThroughTheSnapshotText) {
    const NetworkStateSnapshot s = linSnapshot();
    const std::string text = serialized(s);
    // The rows use the existing header: format-forward, not a new version.
    EXPECT_EQ(text.rfind("# gnss_network_state_snapshot v1\n", 0), 0u) << text.substr(0, 60);
    EXPECT_NE(text.find("RL,0,GPS_DCB,"), std::string::npos) << text;
    EXPECT_NE(text.find("RL,7,GPS_IFB5,"), std::string::npos) << text;

    NetworkStateSnapshot r;
    std::string err;
    std::istringstream is(text);
    ASSERT_TRUE(NetworkStateSnapshotIO::parse(is, r, &err)) << err;
    ASSERT_EQ(r.rx_cb_lin.size(), 2u);
    // Full precision (EXPECT_DOUBLE_EQ, within 4 ULP): this value becomes the
    // station's linearization point for the whole run, and the gate's statistic is
    // a difference from it.
    EXPECT_DOUBLE_EQ(r.rx_cb_lin.at({0, "GPS_DCB"}), -3.7654321098765432);
    EXPECT_DOUBLE_EQ(r.rx_cb_lin.at({7, "GPS_IFB5"}), 0.109375);
    // The RS-6 half is unchanged by the addition; the two travel together.
    ASSERT_EQ(r.rx_cb_carry.size(), 2u);
    EXPECT_DOUBLE_EQ(r.rx_cb_carry.at({0, "GPS_DCB"}).value_abs, -3.8391234567890123);
    // RL is not part of hasStationState: an absent RL block makes only the RL
    // restore inert, in the same way the emit / datum halves keep their own flags.
    NetworkStateSnapshot lin_only = baseSnapshot();
    lin_only.rx_cb_lin[{3, "GPS_DCB"}] = 1.0;
    EXPECT_FALSE(lin_only.hasStationState());
}

/// A non-finite x_lin would corrupt more than one fold: through
/// computeRawResidual it also reaches the first epoch's iono and ambiguity seeds.
/// It is therefore a hard parse failure (the caller's documented cold-start
/// signal), never a silent partial restore (the RC / ER / CBP precedent).
TEST(ResumeRxCbLinRestore, MalformedOrNonFiniteRlRowIsAHardParseFailure) {
    const auto replaceRlLine = [](const std::string& repl) {
        std::string text = serialized(linSnapshot());
        const std::size_t pos = text.find("RL,0,GPS_DCB,");
        EXPECT_NE(pos, std::string::npos) << text;
        const std::size_t eol = text.find('\n', pos);
        text.replace(pos, eol - pos, repl);
        NetworkStateSnapshot r;
        std::string err;
        std::istringstream is(text);
        // The END line-count terminator still matches: the replacements below keep
        // the line count, so every failure asserted here comes from the RL field
        // screen and not from the truncation guard.
        return NetworkStateSnapshotIO::parse(is, r, &err);
    };
    EXPECT_FALSE(replaceRlLine("RL,0,GPS_DCB,nan"));
    EXPECT_FALSE(replaceRlLine("RL,0,GPS_DCB,inf"));
    EXPECT_FALSE(replaceRlLine("RL,0,GPS_DCB,-inf"));
    EXPECT_FALSE(replaceRlLine("RL,0,GPS_DCB"));          // too few fields
    EXPECT_FALSE(replaceRlLine("RL,0,GPS_DCB,1.0,2.0"));  // too many
    EXPECT_FALSE(replaceRlLine("RL,0,,1.0"));             // empty CB key
    EXPECT_FALSE(replaceRlLine("RL,x,GPS_DCB,1.0"));      // non-integer station index
    EXPECT_FALSE(replaceRlLine("RL,0,GPS_DCB,notanumber"));
    // ...and the well-formed control still parses, so the failures above are not
    // caused by the line replacement itself.
    EXPECT_TRUE(replaceRlLine("RL,0,GPS_DCB,-3.7654321098765432"));
    // Either sign and any magnitude is legal: a receiver DCB is signed, and the
    // seed path's own |est| < 200 m bound belongs to the median estimator, not to
    // a value the writer already solved for.
    EXPECT_TRUE(replaceRlLine("RL,0,GPS_DCB,412.5"));
    EXPECT_TRUE(replaceRlLine("RL,0,GPS_DCB,-412.5"));
}

// ---------------------------------------------------------------------------
// (d) the seed-override rule
// ---------------------------------------------------------------------------

/// The decision @c StateInitializer::initializeCodeBiases makes per key inside
/// its `samples` loop: a key the map carries a finite reference for is seeded to
/// the writer's x_lin, and the one-epoch code median is not computed.
TEST(ResumeRxCbLinRestore, SeedOverrideResolvesOnlyFiniteCarriedKeys) {
    const std::map<std::string, double> ovr{{"GPS_DCB", -3.7654321098765432},
                                            {"GPS_IFB5", 0.109375}};
    const auto covered = StateInitializer::resolveCodeBiasSeedOverride(ovr, "GPS_DCB");
    ASSERT_TRUE(covered.has_value());
    EXPECT_DOUBLE_EQ(*covered, -3.7654321098765432);
    EXPECT_DOUBLE_EQ(*StateInitializer::resolveCodeBiasSeedOverride(ovr, "GPS_IFB5"), 0.109375);
    // Absent ⇒ nullopt ⇒ the median seed.  A different constellation's family
    // is the ordinary case (a field lineage carries GPS only).
    EXPECT_FALSE(StateInitializer::resolveCodeBiasSeedOverride(ovr, "GAL_DCB").has_value());
    EXPECT_FALSE(StateInitializer::resolveCodeBiasSeedOverride(ovr, "gps_dcb").has_value());
    // An empty map is the earlier path: a run that leaves the knob off cannot
    // take a different branch here.
    EXPECT_FALSE(StateInitializer::resolveCodeBiasSeedOverride({}, "GPS_DCB").has_value());
    // A second guard, for any future producer of the map: the parser already
    // refuses a non-finite RL row, and a non-finite value that reaches this point
    // is refused rather than installed as a linearization point.
    const std::map<std::string, double> bad{
        {"GPS_DCB", std::numeric_limits<double>::quiet_NaN()},
        {"GPS_IFB5", std::numeric_limits<double>::infinity()}};
    EXPECT_FALSE(StateInitializer::resolveCodeBiasSeedOverride(bad, "GPS_DCB").has_value());
    EXPECT_FALSE(StateInitializer::resolveCodeBiasSeedOverride(bad, "GPS_IFB5").has_value());
}

/// The population against which @c n_resume_rxcb_lin_uncovered is measured.  The
/// RL rows pair 1:1 with the RC rows, so "in the map", "has an rx-CB carry" and
/// "the inject gate reads this key's x_lin" are the same set.  A field station
/// holds two code-bias states (@c GPS_DCB, carried; @c GPS_IFB5, not), so counting
/// every un-restored key the seeding loop reaches would report one miss per
/// station for a family nothing carries.  Only a carried key that could not take
/// its reference is a miss, and on the field legs that count is expected to be 0.
TEST(ResumeRxCbLinRestore, UncoveredIsScopedToTheKeysTheMapCarries) {
    // The field shape: GPS_DCB carried (an RL row beside its RC row), GPS_IFB5 not.
    const std::map<std::string, double> ovr{{"GPS_DCB", -3.7654321098765432}};
    EXPECT_TRUE(StateInitializer::codeBiasSeedOverrideCovers(ovr, "GPS_DCB"));
    EXPECT_FALSE(StateInitializer::codeBiasSeedOverrideCovers(ovr, "GPS_IFB5"));
    EXPECT_FALSE(StateInitializer::codeBiasSeedOverrideCovers(ovr, "GAL_DCB"));
    EXPECT_FALSE(StateInitializer::codeBiasSeedOverrideCovers({}, "GPS_DCB"));
    // The two @c nullopt cases differ, and only the second is counted: an
    // absent key is a family with no carry, while a present-but-refused one is a
    // carried key whose restored reference could not be applied.
    const std::map<std::string, double> bad{
        {"GPS_DCB", std::numeric_limits<double>::quiet_NaN()}};
    EXPECT_TRUE(StateInitializer::codeBiasSeedOverrideCovers(bad, "GPS_DCB"));
    EXPECT_FALSE(StateInitializer::resolveCodeBiasSeedOverride(bad, "GPS_DCB").has_value());
    EXPECT_FALSE(StateInitializer::codeBiasSeedOverrideCovers(bad, "GPS_IFB5"));
    EXPECT_FALSE(StateInitializer::resolveCodeBiasSeedOverride(bad, "GPS_IFB5").has_value());
}

// ---------------------------------------------------------------------------
// (e) an RL-less snapshot is inert
// ---------------------------------------------------------------------------

/// An RS-6 file (RC rows, no RL) is a legal input.  It keeps the earlier
/// behaviour (each station re-derives its linearization point from the resume
/// epoch's code), and the restore container stays empty, which the run's RS-9
/// line reports as INERT.
TEST(ResumeRxCbLinRestore, SnapshotWithoutRlRowsRestoresNothing) {
    NetworkStateSnapshot s = linSnapshot();
    s.rx_cb_lin.clear();
    const std::string text = serialized(s);
    EXPECT_EQ(text.find("\nRL,"), std::string::npos) << text;
    // The RS-6 rows are still there, so this is the file the write knob off produces.
    EXPECT_NE(text.find("RC,0,GPS_DCB,"), std::string::npos) << text;

    NetworkStateSnapshot r;
    std::string err;
    std::istringstream is(text);
    ASSERT_TRUE(NetworkStateSnapshotIO::parse(is, r, &err)) << err;
    EXPECT_TRUE(r.rx_cb_lin.empty());
    EXPECT_EQ(r.rx_cb_carry.size(), 2u);
    EXPECT_TRUE(r.hasStationState());
}

/// With the write knob off the file is byte-identical to the earlier bytes: the
/// driver never fills the container, so the serializer emits no RL rows.
TEST(ResumeRxCbLinRestore, WriteKnobOffLeavesTheSnapshotBytesUnchanged) {
    NetworkStateSnapshot with = linSnapshot();
    NetworkStateSnapshot without = with;
    without.rx_cb_lin.clear();                 // what an unarmed capture leaves
    const std::string a = serialized(without); // the earlier bytes
    const std::string b = serialized(with);
    EXPECT_NE(a, b);
    // ...and the only difference is the added rows: strip them and the bytes match
    // except for the END line count, which every format-forward group moves.
    std::istringstream isb(b);
    std::string stripped, ln;
    int removed = 0;
    while (std::getline(isb, ln)) {
        if (ln.rfind("RL,", 0) == 0) { ++removed; continue; }
        if (ln.rfind("END,", 0) == 0) continue;
        stripped += ln;
        stripped += '\n';
    }
    std::istringstream isa(a);
    std::string base;
    while (std::getline(isa, ln)) {
        if (ln.rfind("END,", 0) == 0) continue;
        base += ln;
        base += '\n';
    }
    EXPECT_EQ(removed, 2);
    EXPECT_EQ(stripped, base);
}
