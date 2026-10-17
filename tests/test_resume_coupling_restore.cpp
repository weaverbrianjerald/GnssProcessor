/**
 * @file test_resume_coupling_restore.cpp
 * @brief Contract tests for the coupling-regressor resume (see
 *        SPEC_synthetic_minimum_network §18.12): the snapshot format-v3 coupling
 *        records (@c QS shared-column dictionary, @c QL / @c QZ packed per-row
 *        regressions, @c QR write-epoch clock-refloat set), their backward
 *        compatibility with v1/v2 files, and the loader's side of the two knobs
 *        (@c network.state_snapshot_coupling_capture,
 *        @c network.resume_coupling_restore).
 *
 * Why the format is pinned this closely.  Under
 * @c network.carry_conditional_coupling a carried leveling column is not a diagonal
 * prior: it is a row @f$A_r=+1,\;A_s=M_r,\;
 * y=(value\_abs-x_{lin})+\sum_j M_{rj}\hat s_{prev,j}@f$ asserted at the pure
 * conditional variance.  @f$A_s=M_r@f$ is what makes the row neutral to the shared
 * states: its contribution to the reduced shared normals is
 * @f$\alpha(M-M_d)(M-M_d)^T@f$, which vanishes when the restored regression equals
 * this epoch's own data regression.  The v1/v2 formats (RTB-7) serialize only
 * @c (value_abs, var, last_seen_s), so a warm-resumed row is injected with an empty
 * regression: the @f$M=0@f$ case, a plain diagonal prior at the conditional
 * variance, a combination no continuous epoch produces.  §18.11 measures the
 * effect: such rows drive the border at @f$\alpha\approx10^6\,\mathrm{m^{-2}}@f$
 * against the Q3 anchored clock prior's @f$1.1\times10^4@f$, and restored clock
 * anchors trip at the first fold.
 *
 * What is pinned here:
 * (1) every regressor round-trips bit-exactly and in harvest order.  The order is
 *     observable at the ulp: @c CouplingRowBuilder::buildCplRow accumulates
 *     @c rhs += coeff*s_prev in vector order (Coupling.cpp), and the gauge-projection
 *     family deficit sums in the same order;
 * (2) @c serialize is idempotent through a parse;
 * (3) the three cases the format exists to handle: a datum-pinned column
 *     (@c p_prev == 0 is a meaningful value, not "missing"); an SH @c sh_layer
 *     carried as its own field (not the @c D record's "L1"-in-the-sat tag, which
 *     would bind a layer-1 coefficient to the live layer-0 column); and two
 *     vintages of one identity side by side (a station whose harvest failed keeps
 *     an older coupling, so the dictionary key is (identity, ŝ_prev, P_prev), not
 *     the identity alone);
 * (4) @c hasCouplingState() is independent of @c hasFrameState().  Widening the
 *     latter would let a coupling-only snapshot claim frame state it lacks; the
 *     restore would then run with an empty @c anchor_incumbent, the QUEUE-1(d)
 *     erase would fire, and the feature would undo its own restore without any sign;
 * (5) the off path is byte-identical: a frame-only snapshot still writes v2 and a
 *     bare one v1;
 * (6) every structural defect is a hard parse failure, because restoring a subset
 *     of a row is the row amputation (@f$M^Tg_s\neq-g_r(c)@f$ ⇒ every coupling
 *     row observes the clock gauge ⇒ the W5 null-space gate withholds the epoch),
 *     which is strictly worse than no coupling at all;
 * (7) the loader interlocks.  This feature changes the direction only and never
 *     modifies @c var: a weight change (the retired seam variance re-inflation)
 *     walked the consumer by metres (invariant #1, the slip-relevel-walk family).
 *
 * The seam mechanics (the first PASS-B inject building a conditional row, the
 * present/departed split, the first-harvest M-vs-M_d census) are graded on the
 * .ep1440 branch legs, not here.
 */
#include <gtest/gtest.h>
#include "config/GnssConfig.hpp"
#include "logging/LogManager.hpp"
#include "products/NetworkStateSnapshot.hpp"
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>
#include "support/TestTempDir.hpp"   // per-process, per-test scratch directory

namespace fs = std::filesystem;
using namespace GnssProcessor;

namespace {

// -----------------------------------------------------------------------------
// Snapshot / config fixtures.  Duplicated from tests/test_resume_frame_restore.cpp
// on purpose rather than moved into a shared header: they are anonymous-namespace
// file-local helpers, and duplication is the convention here (under CLAUDE.md's
// one-suite-per-file rule a shared test header is the more expensive coupling).
// -----------------------------------------------------------------------------

NetworkStateSnapshot baseSnapshot() {
    NetworkStateSnapshot s;
    s.snap_week = 2313;
    s.snap_tow  = 302400.0;
    s.wall_utc  = "2026-09-02T00:00:00Z";
    s.generation_id = 1399161629u;
    s.config_fingerprint = 0x35860c5f45b8a004ull;
    s.n_emitted = 1440;
    s.prev_week = 2313;
    s.prev_tow  = 302400.0;
    return s;
}

NetworkStateSnapshot frameSnapshot() {
    NetworkStateSnapshot s = baseSnapshot();
    s.anchor_incumbent["G01_L1C"] = "S30000SIM";
    s.anchor_incumbent["E02_L5Q"] = "S07000SIM";
    s.anchor_pin_target["E02_L5Q"] = -28.981234567;   // a continuous-handover target
    NetworkStateSnapshot::ClockAnchorSat g01;
    g01.t_last = 1400000000.0 + 302400.0; g01.last_value = -0.9718706; g01.last_var_eff = 1.7e-5;
    g01.in_band = 0; g01.anchored = true;
    NetworkStateSnapshot::ClockAnchorSat g12;
    g12.t_last = 1400000000.0 + 302370.0; g12.last_value = 5.04; g12.last_var_eff = 0.08;
    g12.in_band = 2; g12.anchored = false;
    s.clock_anchor["G01"] = g01;
    s.clock_anchor["G12"] = g12;
    s.clk_frame_acc['G'] = 1.2178912315598864;
    s.clk_frame_acc['E'] = 0.2675;
    s.clk_frame_prev["G01"] = {0.0, 302400.0};
    s.clk_frame_prev["G12"] = {-5.04, 302400.0};
    s.clk_frame_epochs = 1440;
    return s;
}

/// A v3 snapshot exercising every case the §18.12 format exists to handle, in one
/// object: an empty @c signal (Clock column) and an empty @c sat (VtecSh), a
/// datum-pinned column (@c p_prev == 0), an SH entry on layer 1 with the full
/// (n, m, trig, layer) quadruple, and a second dictionary entry for an identity
/// already present at a different @c p_prev (the older-vintage row).  It also
/// carries the frame records, because the loader interlock makes the coupling
/// restore reachable only behind @c network.resume_frame_restore.
NetworkStateSnapshot couplingSnapshot() {
    NetworkStateSnapshot s = frameSnapshot();          // loader interlock: frame rows too
    s.local_carry[{7, "G01", "L1C"}] = {-3.4312345678901234, 8.128093975162085e-07, 1399204800.0};
    s.local_carry[{7, "E02", "L5Q"}] = {12.5,                9.0e-07,               1399204740.0};
    s.zwd_carry[7]                   = {2.331,               4.0e-06,               1399204800.0};
    s.coupling_dict = {
        {0, "G01", "",    0, 0, 'C', 0, -0.9718706123456789, 1.7312e-05},  // Clock, empty signal
        {2, "G01", "L1C", 0, 0, 'C', 0, -1.2178912315598864, 8.128093975162085e-07},
        {1, "E02", "C1C", 0, 0, 'C', 0,  0.2675481109223,    0.0},         // pinned
        {3, "",    "",    2, 1, 'S', 1,  0.4471821,          2.05e-05},    // VtecSh layer 1
        {0, "G01", "",    0, 0, 'C', 0, -0.9718706123456789, 4.4e-05},     // same identity,
    };                                                 // older vintage (different p_prev)
    s.coupling_rows[{7, "G01", "L1C"}] = {{1, -0.99876543210987}, {0, 0.48312}, {3, -1.21e-04}};
    s.coupling_rows[{7, "E02", "L5Q"}] = {{4,  1.0}, {2, -0.5}};   // the stale-vintage row
    s.coupling_zrows[7]                = {{0, 0.331}, {1, -0.0042}};
    s.clock_refloat_sats = {"G14", "G17"};
    return s;
}

// -----------------------------------------------------------------------------
// Text surgery.  Every corruption below rewrites a whole line rather than patching
// a substring, so no case depends on fmt's exact shortest-round-trip rendering of a
// coefficient.  @p needle always begins with '\n', so it anchors on a line start
// and cannot match mid-line (e.g. "\nL," does not match inside a "QL," line).
// -----------------------------------------------------------------------------

/// Erase the whole line anchored at @p needle (leading '\n' included, so the
/// previous line keeps its terminator).
bool eraseLine(std::string& text, const std::string& needle) {
    const std::size_t p = text.find(needle);
    if (p == std::string::npos) return false;
    const std::size_t e = text.find('\n', p + 1);
    if (e == std::string::npos) return false;
    text.erase(p, e - p);
    return true;
}

/// Replace the content of the line anchored at @p needle with @p line.
bool replaceLine(std::string& text, const std::string& needle, const std::string& line) {
    const std::size_t p = text.find(needle);
    if (p == std::string::npos) return false;
    const std::size_t e = text.find('\n', p + 1);
    if (e == std::string::npos) return false;
    text.replace(p + 1, e - (p + 1), line);
    return true;
}

/// Insert a verbatim copy of the line anchored at @p needle immediately after it.
bool duplicateLine(std::string& text, const std::string& needle) {
    const std::size_t p = text.find(needle);
    if (p == std::string::npos) return false;
    const std::size_t e = text.find('\n', p + 1);
    if (e == std::string::npos) return false;
    text.insert(e, text.substr(p, e - p));
    return true;
}

/// Add @p delta to the trailing @c END,<n> truncation guard, so a test that
/// removes or adds a record tests the record rule and not the line count.
bool bumpEndCount(std::string& text, long long delta) {
    const std::size_t p = text.rfind("\nEND,");
    if (p == std::string::npos) return false;
    const std::size_t b = p + 5;
    const std::size_t e = text.find('\n', b);
    if (e == std::string::npos) return false;
    const long long n = std::stoll(text.substr(b, e - b));
    text.replace(b, e - b, std::to_string(n + delta));
    return true;
}

std::string serializeToString(const NetworkStateSnapshot& s) {
    std::ostringstream os;
    NetworkStateSnapshotIO::serialize(os, s);
    return os.str();
}

/// Parse @p text; on failure @p err carries the reason.
bool parseFromString(const std::string& text, NetworkStateSnapshot& out, std::string& err) {
    std::istringstream is(text);
    return NetworkStateSnapshotIO::parse(is, out, &err);
}

// -----------------------------------------------------------------------------
// Loader fixtures (same shape as the §18.10 suite; a distinct temp-file prefix
// keeps two concurrently running suites off each other's scratch paths).
// -----------------------------------------------------------------------------

fs::path writeTempConfig(const std::string& tag, const std::string& body) {
    const fs::path p = gnss_test::tempDir() / ("resumecoupling_cfg_" + tag + ".json");
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

/// The conditional-coupling knobs every coupling posture needs: the conditional local carry is
/// exact only against a dense border carry, so the loader requires
/// @c shared_carry_full_cov for @c carry_conditional_coupling
/// (GnssConfigLoadNetwork.cpp, network section loader).
const char* kCard1 =
    "    \"shared_carry_full_cov\": true,\n"
    "    \"carry_conditional_coupling\": true,\n"
    "    \"carry_coupling_gauge_project\": true,\n";

}  // namespace

// =============================================================================
// ResumeCouplingSnapshotTest: the format-v3 contract.
// =============================================================================

// 1. The main requirement: every regressor comes back bit-exact and in
//    harvest order.  Vector equality, not a set compare: the order is observable
//    at the ulp (rhs accumulates in vector order), so a restore with the same
//    contents in a different order is a different row.
TEST(ResumeCouplingSnapshotTest, RegressorsRoundTripBitExactInHarvestOrder) {
    const NetworkStateSnapshot s = couplingSnapshot();
    ASSERT_TRUE(s.hasCouplingState());
    const std::string text = serializeToString(s);
    EXPECT_EQ(text.rfind("# gnss_network_state_snapshot v3\n", 0), 0u) << text.substr(0, 60);
    EXPECT_NE(text.find("\nQS,0,0,G01,,0,0,67,0,"), std::string::npos);   // empty signal
    EXPECT_NE(text.find("\nQL,7,G01,L1C,3,"), std::string::npos);         // k = 3 declared
    EXPECT_NE(text.find("\nQZ,7,2,"), std::string::npos);                 // k = 2 declared
    EXPECT_NE(text.find("\nQR,G14\n"), std::string::npos);

    NetworkStateSnapshot r;
    std::string err;
    ASSERT_TRUE(parseFromString(text, r, err)) << err;
    ASSERT_EQ(r.coupling_dict.size(), s.coupling_dict.size());
    for (std::size_t i = 0; i < s.coupling_dict.size(); ++i) {
        SCOPED_TRACE(i);
        EXPECT_EQ(r.coupling_dict[i].kind,     s.coupling_dict[i].kind);
        EXPECT_EQ(r.coupling_dict[i].sat,      s.coupling_dict[i].sat);
        EXPECT_EQ(r.coupling_dict[i].signal,   s.coupling_dict[i].signal);
        EXPECT_EQ(r.coupling_dict[i].sh_n,     s.coupling_dict[i].sh_n);
        EXPECT_EQ(r.coupling_dict[i].sh_m,     s.coupling_dict[i].sh_m);
        EXPECT_EQ(r.coupling_dict[i].sh_trig,  s.coupling_dict[i].sh_trig);
        EXPECT_EQ(r.coupling_dict[i].sh_layer, s.coupling_dict[i].sh_layer);
        EXPECT_DOUBLE_EQ(r.coupling_dict[i].s_prev, s.coupling_dict[i].s_prev);
        EXPECT_DOUBLE_EQ(r.coupling_dict[i].p_prev, s.coupling_dict[i].p_prev);
    }
    // Whole-container equality: std::vector<std::pair<int,double>> compares
    // element-wise in order, which is the property under test.
    EXPECT_EQ(r.coupling_rows, s.coupling_rows);
    EXPECT_EQ(r.coupling_zrows, s.coupling_zrows);
    ASSERT_EQ(r.coupling_rows.count({7, "G01", "L1C"}), 1u);
    {
        const auto& v = r.coupling_rows.at({7, "G01", "L1C"});
        ASSERT_EQ(v.size(), 3u);
        EXPECT_EQ(v[0].first, 1);            // harvest order, not ascending id
        EXPECT_EQ(v[1].first, 0);
        EXPECT_EQ(v[2].first, 3);
        EXPECT_DOUBLE_EQ(v[0].second, -0.99876543210987);
        EXPECT_DOUBLE_EQ(v[1].second, 0.48312);
        EXPECT_DOUBLE_EQ(v[2].second, -1.21e-04);
    }
    EXPECT_EQ(r.clock_refloat_sats, s.clock_refloat_sats);
    EXPECT_TRUE(r.hasCouplingState());
}

// 2. The strongest single statement of exactness available without a reference
//    file: serialize(parse(serialize(s))) is byte-for-byte serialize(s).  It
//    covers every field, every container's emit order, and the END line count in
//    one comparison.
TEST(ResumeCouplingSnapshotTest, SerializeIsIdempotent) {
    const std::string once = serializeToString(couplingSnapshot());
    NetworkStateSnapshot r;
    std::string err;
    ASSERT_TRUE(parseFromString(once, r, err)) << err;
    EXPECT_EQ(serializeToString(r), once);
}

// 3. p_prev == 0 is the meaningful "datum-pinned / ensemble-gauge column,
//    exactly known" value, not a missing one.  A reader that dropped or defaulted
//    it would activate the κ shrink (guarded on p_prev > 0 in
//    CouplingRowBuilder::buildCplRow, Coupling.cpp) on a column the datum
//    re-applies exactly, changing the row's gauge sum.
TEST(ResumeCouplingSnapshotTest, PinnedColumnZeroPPrevSurvives) {
    NetworkStateSnapshot r;
    std::string err;
    ASSERT_TRUE(parseFromString(serializeToString(couplingSnapshot()), r, err)) << err;
    ASSERT_EQ(r.coupling_dict.size(), 5u);          // not dropped
    EXPECT_EQ(r.coupling_dict[2].sat, "E02");
    EXPECT_EQ(r.coupling_dict[2].signal, "C1C");
    EXPECT_DOUBLE_EQ(r.coupling_dict[2].p_prev, 0.0);
    EXPECT_DOUBLE_EQ(r.coupling_dict[2].s_prev, 0.2675481109223);
}

// 4. sh_layer is its own field.  The D record has no room for it and tags layer 1
//    in the sat string as the literal "L1" (networkWriteStateSnapshot,
//    StateSnapshotWrite.cpp).  Reusing that tag here would make resolveSharedCol
//    (vtecCoeffColumn(n, m, trig, sh_layer)) bind a layer-1 coefficient to the
//    live layer-0 column, which is strictly worse than departing.
TEST(ResumeCouplingSnapshotTest, ShLayerRoundTripsAsItsOwnField) {
    const NetworkStateSnapshot s = couplingSnapshot();
    const std::string text = serializeToString(s);
    // ...,sh_n=2,sh_m=1,sh_trig='S'(83),sh_layer=1, with the sat field empty.
    EXPECT_NE(text.find("\nQS,3,3,,,2,1,83,1,"), std::string::npos);
    NetworkStateSnapshot r;
    std::string err;
    ASSERT_TRUE(parseFromString(text, r, err)) << err;
    ASSERT_EQ(r.coupling_dict.size(), 5u);
    EXPECT_EQ(r.coupling_dict[3].kind, 3);          // VtecSh
    EXPECT_TRUE(r.coupling_dict[3].sat.empty());    // never "L1"
    EXPECT_TRUE(r.coupling_dict[3].signal.empty());
    EXPECT_EQ(r.coupling_dict[3].sh_n, 2);
    EXPECT_EQ(r.coupling_dict[3].sh_m, 1);
    EXPECT_EQ(r.coupling_dict[3].sh_trig, static_cast<int>('S'));
    EXPECT_EQ(r.coupling_dict[3].sh_layer, 1);
}

// 5. The dictionary key is (identity, ŝ_prev, P_prev), not the identity alone.
//    networkHarvestColumn assigns both values from the shared column, so the rows of one
//    harvest epoch agree, but a station whose harvest failed `continue`s before the
//    stash and keeps an older coupling (observed on the branch-point snapshot file).
//    An identity-only key would restore those rows against the wrong frame.
TEST(ResumeCouplingSnapshotTest, DistinctVintagesOfOneIdentityBothSurvive) {
    NetworkStateSnapshot r;
    std::string err;
    ASSERT_TRUE(parseFromString(serializeToString(couplingSnapshot()), r, err)) << err;
    ASSERT_EQ(r.coupling_dict.size(), 5u);
    // Ids 0 and 4 are the same identity at two vintages.
    EXPECT_EQ(r.coupling_dict[0].kind, r.coupling_dict[4].kind);
    EXPECT_EQ(r.coupling_dict[0].sat,  r.coupling_dict[4].sat);
    EXPECT_EQ(r.coupling_dict[0].signal, r.coupling_dict[4].signal);
    EXPECT_DOUBLE_EQ(r.coupling_dict[0].s_prev, r.coupling_dict[4].s_prev);
    EXPECT_DOUBLE_EQ(r.coupling_dict[0].p_prev, 1.7312e-05);
    EXPECT_DOUBLE_EQ(r.coupling_dict[4].p_prev, 4.4e-05);
    EXPECT_NE(r.coupling_dict[0].p_prev, r.coupling_dict[4].p_prev);
    // ...and each row still points at its own vintage.
    ASSERT_EQ(r.coupling_rows.count({7, "G01", "L1C"}), 1u);
    ASSERT_EQ(r.coupling_rows.count({7, "E02", "L5Q"}), 1u);
    EXPECT_EQ(r.coupling_rows.at({7, "G01", "L1C"})[1].first, 0);   // fresh vintage
    EXPECT_EQ(r.coupling_rows.at({7, "E02", "L5Q"})[0].first, 4);   // stale vintage
}

// 6. hasCouplingState() is deliberately separate from hasFrameState().  The latter
//    also gates the resume-side frame restore (networkTryWarmResume,
//    WarmResume.cpp), so widening it would let a coupling-bearing snapshot
//    with no PA/PT/CA rows claim frame state it does not have: the restore would
//    proceed with an empty anchor_incumbent, the QUEUE-1(d) erase would fire
//    anyway, and the carries restored here would be erased without a warning.
TEST(ResumeCouplingSnapshotTest, HasCouplingStateIsIndependentOfHasFrameState) {
    NetworkStateSnapshot only_cpl = baseSnapshot();
    only_cpl.local_carry[{7, "G01", "L1C"}] = {-3.43, 8.1e-07, 1399204800.0};
    only_cpl.coupling_dict = {{0, "G01", "", 0, 0, 'C', 0, -0.97, 1.7e-05}};
    only_cpl.coupling_rows[{7, "G01", "L1C"}] = {{0, 0.5}};
    EXPECT_TRUE(only_cpl.hasCouplingState());
    EXPECT_FALSE(only_cpl.hasFrameState());
    // The QR set alone is coupling state too (a refloated sat is skip-stashed,
    // so no other record in the file can carry the flag).
    NetworkStateSnapshot only_qr = baseSnapshot();
    only_qr.clock_refloat_sats = {"G14"};
    EXPECT_TRUE(only_qr.hasCouplingState());
    EXPECT_FALSE(only_qr.hasFrameState());
    // ...and the converse: frame state alone is not coupling state.
    const NetworkStateSnapshot fr = frameSnapshot();
    EXPECT_TRUE(fr.hasFrameState());
    EXPECT_FALSE(fr.hasCouplingState());
}

// 7. Off-path byte identity, write side (a): a frame-only run still writes the
//    v2 header and none of the four coupling tags.
TEST(ResumeCouplingSnapshotTest, FrameOnlySnapshotStillWritesV2Header) {
    const std::string text = serializeToString(frameSnapshot());
    EXPECT_EQ(text.rfind("# gnss_network_state_snapshot v2\n", 0), 0u) << text.substr(0, 60);
    EXPECT_EQ(text.find("\nQS,"), std::string::npos);
    EXPECT_EQ(text.find("\nQL,"), std::string::npos);
    EXPECT_EQ(text.find("\nQZ,"), std::string::npos);
    EXPECT_EQ(text.find("\nQR,"), std::string::npos);
}

// 8. Off-path byte identity, write side (b): a bare run still writes v1, which
//    shows that hasFrameState() was not widened into the v3 selector.
TEST(ResumeCouplingSnapshotTest, BareSnapshotStillWritesV1Header) {
    const NetworkStateSnapshot s = baseSnapshot();
    ASSERT_FALSE(s.hasFrameState());
    ASSERT_FALSE(s.hasCouplingState());
    const std::string text = serializeToString(s);
    EXPECT_EQ(text.rfind("# gnss_network_state_snapshot v1\n", 0), 0u) << text.substr(0, 60);
}

// 9. The inert path (tier 2): v2 files, such as the existing branch library
//    (runs/camp60wu_quiet12h_frwriter/*.ep1440), stay legal input.  A v2 file parses,
//    all four coupling containers come back empty, and hasCouplingState() is false,
//    so the armed restore warns that it is inert instead of restoring half a row.
TEST(ResumeCouplingSnapshotTest, V2FileParsesWithEmptyCouplingContainers) {
    NetworkStateSnapshot r;
    std::string err;
    ASSERT_TRUE(parseFromString(serializeToString(frameSnapshot()), r, err)) << err;
    EXPECT_TRUE(r.coupling_dict.empty());
    EXPECT_TRUE(r.coupling_rows.empty());
    EXPECT_TRUE(r.coupling_zrows.empty());
    EXPECT_TRUE(r.clock_refloat_sats.empty());
    EXPECT_FALSE(r.hasCouplingState());
    EXPECT_TRUE(r.hasFrameState());          // the v2 half is untouched
}

// 10. The parser enforces row completeness.  The explicit k makes it checkable: a
//     line that lost exactly one whole (id, M) pair still has even arity and would
//     pass unnoticed, and a partial coefficient vector breaks the gauge
//     identity (strictly worse than an empty one).
TEST(ResumeCouplingSnapshotTest, EntryCountMismatchFailsParse) {
    std::string text = serializeToString(couplingSnapshot());
    ASSERT_TRUE(replaceLine(text, "\nQL,7,G01,L1C,",
                            "QL,7,G01,L1C,4,1,-0.5,0,0.48312,3,-0.000121"));   // says 4, has 3
    NetworkStateSnapshot r;
    std::string err;
    EXPECT_FALSE(parseFromString(text, r, err));
    EXPECT_NE(err.find("QL"), std::string::npos) << err;
}

// 11. A dictionary id outside the parsed dictionary is a hard failure, checked
//     in the post-loop splice (record order in the file stays semantically
//     irrelevant, so the check cannot live in the dispatch arm).
TEST(ResumeCouplingSnapshotTest, OutOfRangeDictionaryIdFailsParse) {
    std::string text = serializeToString(couplingSnapshot());
    ASSERT_TRUE(replaceLine(text, "\nQL,7,G01,L1C,",
                            "QL,7,G01,L1C,3,999,-0.5,0,0.48312,3,-0.000121"));
    NetworkStateSnapshot r;
    std::string err;
    EXPECT_FALSE(parseFromString(text, r, err));
    EXPECT_NE(err.find("out of range"), std::string::npos) << err;
}

// 12. Ids must be dense and ascending: the id is the vector index, so a gap would
//     re-map every later row's regression onto the wrong columns without an error.
TEST(ResumeCouplingSnapshotTest, NonDenseDictionaryIdFailsParse) {
    std::string text = serializeToString(couplingSnapshot());
    ASSERT_TRUE(eraseLine(text, "\nQS,1,"));
    ASSERT_TRUE(bumpEndCount(text, -1));      // isolate the id rule from the END guard
    NetworkStateSnapshot r;
    std::string err;
    EXPECT_FALSE(parseFromString(text, r, err));
    EXPECT_NE(err.find("dense"), std::string::npos) << err;
}

// 13. A QL with no L row to attach to is a hard failure, not an ignored orphan:
//     the regression is meaningless without the (value_abs, var) it conditions.
TEST(ResumeCouplingSnapshotTest, OrphanQlWithoutLRowFailsParse) {
    std::string text = serializeToString(couplingSnapshot());
    ASSERT_TRUE(eraseLine(text, "\nL,7,G01,L1C,"));   // anchored at line start, so not a QL
    ASSERT_TRUE(bumpEndCount(text, -1));
    NetworkStateSnapshot r;
    std::string err;
    EXPECT_FALSE(parseFromString(text, r, err));
    EXPECT_NE(err.find("no matching L row"), std::string::npos) << err;
}

// 14. Two QL rows on one key would make the restored regression depend on file
//     order, so the parser refuses them.
TEST(ResumeCouplingSnapshotTest, DuplicateQlRowFailsParse) {
    std::string text = serializeToString(couplingSnapshot());
    ASSERT_TRUE(duplicateLine(text, "\nQL,7,E02,L5Q,"));
    ASSERT_TRUE(bumpEndCount(text, +1));
    NetworkStateSnapshot r;
    std::string err;
    EXPECT_FALSE(parseFromString(text, r, err));
    EXPECT_NE(err.find("duplicate"), std::string::npos) << err;
}

// 15. The one path in this format where a bad value passes without an error: fmt
//     writes "nan"/"inf" and parseD's strtod accepts and fully consumes them.
//     Without an explicit finite screen, a non-finite coefficient round-trips and
//     then hits buildCplRow's own non-finite-RHS guard (Coupling.cpp), which
//     drops the whole row: the arc loses its level, not just its coupling.
TEST(ResumeCouplingSnapshotTest, NonFiniteCoefficientFailsParse) {
    std::string text = serializeToString(couplingSnapshot());
    ASSERT_TRUE(replaceLine(text, "\nQZ,7,", "QZ,7,2,0,nan,1,-0.0042"));
    NetworkStateSnapshot r;
    std::string err;
    EXPECT_FALSE(parseFromString(text, r, err));
    EXPECT_NE(err.find("finite"), std::string::npos) << err;
}

// 16. p_prev == 0 is legal (case 3); a negative one is corrupt: it is a variance,
//     and the departed route marginalizes M²·P_prev into σ².
TEST(ResumeCouplingSnapshotTest, NegativePPrevFailsParse) {
    std::string text = serializeToString(couplingSnapshot());
    ASSERT_TRUE(replaceLine(text, "\nQS,2,", "QS,2,1,E02,C1C,0,0,67,0,0.5,-1"));
    NetworkStateSnapshot r;
    std::string err;
    EXPECT_FALSE(parseFromString(text, r, err));
}

// 17. Every coupling line goes through the `put` lambda, so the END,<n> truncation
//     guard counts them: deleting a QS without fixing END is caught by the line
//     count.  The deleted entry is the last dictionary entry, so the remaining ids
//     are still dense and the id rule cannot fire first and mask the case under
//     test (the dict-id range failure lives in the post-loop splice, which the
//     in-loop END check precedes).
TEST(ResumeCouplingSnapshotTest, EndLineCountCoversCouplingRecords) {
    std::string text = serializeToString(couplingSnapshot());
    ASSERT_TRUE(eraseLine(text, "\nQS,4,"));          // END deliberately untouched
    NetworkStateSnapshot r;
    std::string err;
    EXPECT_FALSE(parseFromString(text, r, err));
    EXPECT_NE(err.find("line-count mismatch"), std::string::npos) << err;
}

// 18. File position matters: the parser's blob state machines consume the CARRY
//     and EMITTER blobs verbatim, so any record emitted after either would be
//     lost.  The coupling records must precede both.
TEST(ResumeCouplingSnapshotTest, CouplingRecordsPrecedeTheBlobs) {
    NetworkStateSnapshot s = couplingSnapshot();
    s.carry_state   = "CARRY,v1,0\nCEND\n";
    s.emitter_state = "EMITTER,v1\nEEND\n";
    const std::string text = serializeToString(s);
    const std::size_t qs = text.find("\nQS,");
    const std::size_t qr = text.find("\nQR,");
    const std::size_t cy = text.find("\nCARRY,");
    const std::size_t em = text.find("\nEMITTER,");
    ASSERT_NE(qs, std::string::npos);
    ASSERT_NE(qr, std::string::npos);
    ASSERT_NE(cy, std::string::npos);
    ASSERT_NE(em, std::string::npos);
    EXPECT_LT(qs, cy);
    EXPECT_LT(qs, em);
    EXPECT_LT(qr, cy);
    EXPECT_LT(qr, em);
}

// 19. Non-collision, statically.  "END," / "EMITTER," / "CARRY," are matched
//     with rfind(p, 0) == 0 prefix tests, not tag equality, so a coupling tag that
//     was a prefix of (or prefixed by) one of them would be mis-dispatched.
TEST(ResumeCouplingSnapshotTest, NoNewTagCollidesWithStructuralPrefixes) {
    const std::vector<std::string> tags{"QS,", "QL,", "QZ,", "QR,"};
    const std::vector<std::string> structural{"END,", "EMITTER,", "CARRY,"};
    for (const std::string& t : tags) {
        SCOPED_TRACE(t);
        for (const std::string& p : structural) {
            SCOPED_TRACE(p);
            EXPECT_NE(t.rfind(p, 0), 0u);   // structural is not a prefix of the tag
            EXPECT_NE(p.rfind(t, 0), 0u);   // ...nor the tag of structural
        }
    }
}

// =============================================================================
// ResumeCouplingConfig: the loader interlocks.
//
// GnssConfig::loadFromJson catches the std::runtime_error (its handler catches
// std::exception, GnssConfigLoad.cpp), logs the message and returns false, so
// these cases can assert the refusal but not the message text.  The message
// content is reviewed at the throw sites in loadNetworkObservabilityAndSnapshot_ /
// loadNetworkCarries_.
// =============================================================================

// 20. Both knobs default to off: a stock network run writes v1/v2 and restores
//     nothing (the byte-identity baseline).
TEST(ResumeCouplingConfig, DefaultsAreOff) {
    GnssConfig cfg;
    ASSERT_TRUE(loadInto("cpl_defaults", networkConfig(), cfg));
    EXPECT_FALSE(cfg.getNetworkConfig().state_snapshot_coupling_capture);
    EXPECT_FALSE(cfg.networkResumeCouplingRestore());
}

// 21. The capture has its own knob but is unusable without the frame capture: the
//     coupling restore is reachable only behind resume_frame_restore, which is
//     inert on a snapshot with no frame rows.
TEST(ResumeCouplingConfig, CaptureRequiresFrameCapture) {
    GnssConfig cfg;
    EXPECT_FALSE(loadInto("cpl_capture_nofr",
        networkConfig("    \"state_snapshot_path\": \"state_snapshot.snap\",\n"
                      "    \"state_snapshot_coupling_capture\": true,\n"), cfg));
}

// 22. Warn-inert, not illegal (the same rule as state_snapshot_frame_capture in
//     GnssConfigLoadNetwork.cpp): armed with no snapshot path, nothing is written
//     and the run proceeds.
TEST(ResumeCouplingConfig, CaptureWithoutSnapshotPathIsWarnInert) {
    GnssConfig cfg;
    ASSERT_TRUE(loadInto("cpl_capture_nopath",
        networkConfig("    \"state_snapshot_frame_capture\": true,\n"
                      "    \"state_snapshot_coupling_capture\": true,\n"), cfg));
    EXPECT_TRUE(cfg.getNetworkConfig().state_snapshot_coupling_capture);
}

// 23. The restored ŝ_prev are frame-valid only once the pre-seam realization is
//     restored; without resume_frame_restore the first incumbent-less election's
//     QUEUE-1(d) clause erases the leveling carries, coupling included, before
//     PASS B reads them.
TEST(ResumeCouplingConfig, RestoreRequiresFrameRestore) {
    GnssConfig cfg;
    EXPECT_FALSE(loadInto("cpl_restore_nofr",
        networkConfig(std::string("    \"phase_leveling\": true,\n"
                                  "    \"state_snapshot_path\": \"state_snapshot.snap\",\n")
                      + kCard1 +
                      "    \"resume_coupling_restore\": true,\n"), cfg));
}

// 24. buildCplRow is never called without carry_conditional_coupling, so the
//     restored regressors would be dead weight.
TEST(ResumeCouplingConfig, RestoreRequiresConditionalCoupling) {
    GnssConfig cfg;
    EXPECT_FALSE(loadInto("cpl_restore_nocond",
        networkConfig("    \"phase_leveling\": true,\n"
                      "    \"state_snapshot_path\": \"state_snapshot.snap\",\n"
                      "    \"resume_frame_restore\": true,\n"
                      "    \"resume_coupling_restore\": true,\n"), cfg));
}

// 25. carry_coupling_gauge_project repairs the family-sum defect that a
//     truncated row leaves (it restores Mᵀg_s = −g_r(c)), and the resume seam is
//     the largest-scale truncation regime.  Without it the W5 null-space gate can
//     withhold every epoch that carries coupling rows.
TEST(ResumeCouplingConfig, RestoreRequiresGaugeProject) {
    GnssConfig cfg;
    EXPECT_FALSE(loadInto("cpl_restore_nogauge",
        networkConfig("    \"phase_leveling\": true,\n"
                      "    \"state_snapshot_path\": \"state_snapshot.snap\",\n"
                      "    \"shared_carry_full_cov\": true,\n"
                      "    \"carry_conditional_coupling\": true,\n"
                      "    \"carry_coupling_gauge_project\": false,\n"
                      "    \"resume_frame_restore\": true,\n"
                      "    \"resume_coupling_restore\": true,\n"), cfg));
}

// 27. The full arm-C validation posture loads clean and both knobs read true.
TEST(ResumeCouplingConfig, ArmedPostureLoads) {
    GnssConfig cfg;
    ASSERT_TRUE(loadInto("cpl_armed",
        networkConfig(std::string("    \"phase_leveling\": true,\n"
                                  "    \"state_snapshot_path\": \"state_snapshot.snap\",\n"
                                  "    \"state_snapshot_frame_capture\": true,\n"
                                  "    \"state_snapshot_coupling_capture\": true,\n")
                      + kCard1 +
                      "    \"carry_coupling_reinit_sources\": true,\n"
                      "    \"resume_frame_restore\": true,\n"
                      "    \"resume_coupling_restore\": true,\n"
                      "    \"resume_slip_warmup_ep\": 5,\n"), cfg));
    EXPECT_TRUE(cfg.getNetworkConfig().state_snapshot_frame_capture);
    EXPECT_TRUE(cfg.getNetworkConfig().state_snapshot_coupling_capture);
    EXPECT_TRUE(cfg.networkResumeFrameRestore());
    EXPECT_TRUE(cfg.networkResumeCouplingRestore());
    EXPECT_EQ(cfg.networkResumeSlipWarmupEp(), 5);
}
