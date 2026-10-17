/**
 * @file test_resume_border_restore.cpp
 * @brief Contract tests for the RS-7 dense-border continuity mechanism
 *        (docs/SPEC_event_harness.md §21.14): the read knob
 *        @c network.resume_border_restore, its interlocks, and the restore
 *        convention it applies to the snapshot's @c QK / @c QP records.
 *
 * The defect.  Under @c network.shared_carry_full_cov the D2 shared carry
 * (satellite clocks, code biases, phase biases) folds as one dense information
 * block @f$\Lambda=(P_{prev}+Q)^{-1}@f$.  Without a border restore, a warm resume
 * restores that block's values and per-key variances (the @c D records) but not
 * its cross-covariance, so at the first warm fold every carried key misses
 * @c SharedCarryCov::row and falls back to the per-key diagonal; the C1 dense
 * border run-end line counts these diagonal folds.  The block is in the file (the
 * @c QK keys and @c QP lower-triangle rows written by
 * @c network.state_snapshot_coupling_capture); its other reader is
 * @c network.resume_coupling_restore, which the loader refuses on a
 * @c carry_invariant field lineage because it requires the coupling gauge projection.
 *
 * The split, which is the substance of the mechanism and the reason a test guards it.
 * One capture knob writes two independent populations:
 *   * the coupling rows (@c QL / @c QZ) are per-row regressions @f$M_r@f$ on
 *     the border.  A truncated one breaks the family identity
 *     @f$M^Tg_s=-g_r(c)@f$: every coupling row then observes the clock gauge
 *     and the W5 null-space gate withholds the epoch.  They therefore need 1f's
 *     repair and stay behind @c resume_coupling_restore;
 *   * the @c QK / @c QP block is a covariance.  It carries no regression, no
 *     gauge identity and no family sum, so it needs neither
 *     @c carry_conditional_coupling nor 1f.
 * If a later edit put the new knob under the old interlock, the field lineage
 * would lose its border again with only a run-end counter to show it; hence
 * @ref ResumeBorderRestore.RestoreDoesNotRequireCard1f.
 *
 * Tested here: the default; the @c resume_frame_restore prerequisite (the border
 * is expressed in the realization frame, the RS-6 argument); the warn-and-disarm
 * without @c shared_carry_full_cov (nothing consumes the block, so an armed knob
 * would be inert without any message, invariant #4); the independence from
 * the gauge projection and from conditional coupling; and the restore convention itself:
 * which keys are kept, which are dropped (row and column), that the rebuilt @c P
 * is symmetric from the stored lower triangle, and that a snapshot without
 * @c QK / @c QP rows restores nothing rather than something partial.
 *
 * The seam mechanics (the first fold running the dense path, the rx-CB gate
 * share, the code-bias level, the ZWD sink) are graded on the §21.14 harness
 * legs, not here.
 */
#include <gtest/gtest.h>

#include "network/state/RunContext.hpp"
#include "config/GnssConfig.hpp"
#include "network/driver/RunTypes.hpp"
#include "logging/LogManager.hpp"
#include "products/NetworkStateSnapshot.hpp"

#include <cmath>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>
#include <string>
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

/// A snapshot carrying a three-key dense border plus the matching D rows, with
/// values chosen so a shortest-round-trip decimal differs from a rounded one.
/// The third key has no live carry in @ref liveCarry below, so the
/// drop path is exercised by every restore test that uses it.
NetworkStateSnapshot borderSnapshot() {
    NetworkStateSnapshot s = baseSnapshot();
    s.carry_cov_keys = {"G01|CLK", "G01|CB|C1C", "G12|CLK"};
    s.carry_cov_rows = {{1.2345678901234e-04},
                        {2.0e-05, 3.0517578125e-04},
                        {-1.0e-06, 4.0e-06, 5.0e-04}};
    return s;
}

/// The live D2 carry the restore intersects against: the first two keys of
/// @ref borderSnapshot, plus an SH coefficient that must never reach the dense
/// block (VtecSh's carry is the diagonal pre-gate fold, and @ref sharedCarryKey
/// has no key for the (n,m,trig) family).
std::vector<CarryPrior> liveCarry() {
    std::vector<CarryPrior> v;
    CarryPrior clk;
    clk.kind = SharedParamKind::Clock;
    clk.sat = "G01";
    clk.value = -0.9718706;
    clk.var = 1.2345678901234e-04;
    v.push_back(clk);
    CarryPrior cb;
    cb.kind = SharedParamKind::CodeBias;
    cb.sat = "G01";
    cb.signal = "C1C";
    cb.value = 3.75;
    cb.var = 3.0517578125e-04;
    v.push_back(cb);
    CarryPrior sh;
    sh.kind = SharedParamKind::VtecSh;
    sh.sh_n = 0;
    sh.sh_m = 0;
    sh.sh_trig = 'C';
    sh.value = 32.0;
    sh.var = 1.0;
    v.push_back(sh);
    return v;
}

/// The restore the driver performs (WarmResume.cpp, the dense D2 covariance
/// side-car block of SPEC_synthetic_minimum_network §18.12 (2a), which
/// @c resume_border_restore also reaches), reproduced here so the convention is
/// fixed by a test rather than by a comment: intersect the snapshot's key list
/// with the keys that have a live non-VtecSh carry, keep the snapshot's row
/// order, drop the rest (row and column), and mirror the stored lower triangle
/// into a symmetric @c P.  @c ok is the driver's `nk > 0 && finite` screen;
/// false means the driver leaves @c prev_carry_cov alone and the fold degrades
/// per key as without the restore.
SharedCarryCov restoreBorder(const NetworkStateSnapshot& snap,
                             const std::vector<CarryPrior>& prev_carry,
                             std::size_t* dropped, bool* ok) {
    SharedCarryCov cov;
    *dropped = 0;
    *ok = false;
    if (snap.carry_cov_keys.empty()
        || snap.carry_cov_rows.size() != snap.carry_cov_keys.size())
        return cov;
    std::set<std::string> live_keys;
    for (const CarryPrior& cp : prev_carry)
        if (cp.kind != SharedParamKind::VtecSh)
            live_keys.insert(sharedCarryKey(cp.kind, cp.sat, cp.signal));
    std::vector<std::size_t> keep;
    for (std::size_t i = 0; i < snap.carry_cov_keys.size(); ++i) {
        if (live_keys.count(snap.carry_cov_keys[i])) keep.push_back(i);
        else ++(*dropped);
    }
    const Eigen::Index nk = static_cast<Eigen::Index>(keep.size());
    cov.P = Eigen::MatrixXd::Zero(nk, nk);
    bool finite = true;
    for (Eigen::Index a = 0; a < nk; ++a) {
        const std::size_t ia = keep[static_cast<std::size_t>(a)];
        cov.row.emplace(snap.carry_cov_keys[ia], static_cast<std::size_t>(a));
        for (Eigen::Index b = 0; b <= a; ++b) {
            const std::size_t ib = keep[static_cast<std::size_t>(b)];
            const double v = snap.carry_cov_rows[ia][ib];
            if (!std::isfinite(v)) finite = false;
            cov.P(a, b) = v;
            cov.P(b, a) = v;
        }
    }
    *ok = (nk > 0 && finite);
    if (!*ok) cov.clear();
    return cov;
}

fs::path writeTempConfig(const std::string& tag, const std::string& body) {
    const fs::path p = gnss_test::tempDir() / ("resumeborder_cfg_" + tag + ".json");
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

/// The minimum a border restore needs: a snapshot to resume from, the dense
/// carry that consumes the block, and the realization frame it is expressed in.
const char* kBorderBase =
    "    \"state_snapshot_path\": \"state_snapshot.snap\",\n"
    "    \"shared_carry_full_cov\": true,\n"
    "    \"resume_frame_restore\": true,\n";

}  // namespace

// =============================================================================
// The loader contract.
//
// Note: GnssConfig::loadFromJson catches std::runtime_error, logs the message and
// returns false, so a refusal is asserted as a failed load and never by message
// text, as in every sibling resume suite.
// =============================================================================

/// The byte-identity floor: a stock network run does not restore a border.
TEST(ResumeBorderRestore, DefaultIsOff) {
    GnssConfig cfg;
    ASSERT_TRUE(loadInto("defaults", networkConfig(), cfg));
    EXPECT_FALSE(cfg.networkResumeBorderRestore());
}

/// The border's keys are absolute satellite clocks and code/phase biases against
/// the datum the same solve realizes, so the block is only meaningful once the
/// pre-seam realization continues.  Without the frame restore the first election
/// re-realizes that frame and the restored cross-covariance would describe a
/// level the writer never held (invariant #1: a stiff prior carried stale across
/// a jump).  The loader refuses this configuration.
TEST(ResumeBorderRestore, RestoreRequiresFrameRestore) {
    GnssConfig cfg;
    EXPECT_FALSE(loadInto("noframe",
        networkConfig("    \"state_snapshot_path\": \"state_snapshot.snap\",\n"
                      "    \"shared_carry_full_cov\": true,\n"
                      "    \"resume_border_restore\": true,\n"), cfg));
}

/// Invariant #4.  With shared_carry_full_cov off, every key folds through the
/// per-key diagonal path by construction, so nothing would read the restored
/// block.  The loader warns and disarms the knob rather than loading it armed
/// and inert.
TEST(ResumeBorderRestore, RestoreWithoutFullCovWarnsAndDisarms) {
    GnssConfig cfg;
    ASSERT_TRUE(loadInto("nofullcov",
        networkConfig("    \"state_snapshot_path\": \"state_snapshot.snap\",\n"
                      "    \"resume_frame_restore\": true,\n"
                      "    \"resume_border_restore\": true,\n"), cfg));
    EXPECT_FALSE(cfg.networkResumeBorderRestore())
        << "an armed-but-unconsumed border restore must be disarmed, not carried";
}

/// The central contract of the knob.  network.resume_coupling_restore requires
/// carry_coupling_gauge_project because it also restores the coupling
/// rows, whose truncation breaks Mᵀg_s = −g_r(c).  A covariance block carries no
/// regression and no gauge identity, so the border restore loads without 1f.
/// That makes it reachable on the carry_invariant field lineage, where 1f is out
/// of regime and resume_coupling_restore is refused.
TEST(ResumeBorderRestore, RestoreDoesNotRequireCard1f) {
    GnssConfig cfg;
    ASSERT_TRUE(loadInto("no1f",
        networkConfig(std::string(kBorderBase)
                      + "    \"carry_coupling_gauge_project\": false,\n"
                        "    \"resume_border_restore\": true,\n"), cfg));
    EXPECT_TRUE(cfg.networkResumeBorderRestore());
    EXPECT_FALSE(cfg.networkResumeCouplingRestore())
        << "the two knobs are independent; arming one must not arm the other";
}

/// The same split on the other interlock: buildCplRow is what needs
/// carry_conditional_coupling, and the border restore does not build rows.
TEST(ResumeBorderRestore, RestoreDoesNotRequireConditionalCoupling) {
    GnssConfig cfg;
    ASSERT_TRUE(loadInto("nocond",
        networkConfig(std::string(kBorderBase)
                      + "    \"carry_conditional_coupling\": false,\n"
                        "    \"resume_border_restore\": true,\n"), cfg));
    EXPECT_TRUE(cfg.networkResumeBorderRestore());
}

/// The §21.14 leg posture: the field lineage's own carry, the frame restore, the
/// border restore, and resume_coupling_restore explicitly off (the loader would
/// refuse it here).  This loads clean.
TEST(ResumeBorderRestore, ArmedFieldPostureLoads) {
    GnssConfig cfg;
    ASSERT_TRUE(loadInto("armed",
        networkConfig(std::string(kBorderBase)
                      + "    \"phase_datum_eq34\": true,\n"     // carry_invariant's own
                        "    \"carry_invariant\": true,\n"      //   loader prerequisite
                        "    \"state_snapshot_frame_capture\": true,\n"
                        "    \"state_snapshot_coupling_capture\": true,\n"
                        "    \"resume_border_restore\": true,\n"
                        "    \"resume_coupling_restore\": false,\n"), cfg));
    EXPECT_TRUE(cfg.networkResumeBorderRestore());
    EXPECT_TRUE(cfg.networkResumeFrameRestore());
    EXPECT_FALSE(cfg.networkResumeCouplingRestore());
    EXPECT_TRUE(cfg.getNetworkConfig().state_snapshot_coupling_capture);
}

// =============================================================================
// The restore convention.
// =============================================================================

/// The restore is driven by the knob alone: a snapshot's QK/QP rows rebuild the
/// dense block for every key that also has a live carry, at the snapshot's own
/// row order, symmetric from the stored lower triangle, bit-exact.
TEST(ResumeBorderRestore, QkQpRowsRebuildTheDenseBlockBitExact) {
    NetworkStateSnapshot r;
    std::string err;
    std::ostringstream os;
    NetworkStateSnapshotIO::serialize(os, borderSnapshot());
    std::istringstream is(os.str());
    ASSERT_TRUE(NetworkStateSnapshotIO::parse(is, r, &err)) << err;

    std::size_t dropped = 0;
    bool ok = false;
    const SharedCarryCov cov = restoreBorder(r, liveCarry(), &dropped, &ok);
    ASSERT_TRUE(ok);
    // G12|CLK has no live carry: dropped (row and column), never zero-filled.
    EXPECT_EQ(dropped, 1u);
    ASSERT_EQ(cov.row.size(), 2u);
    EXPECT_EQ(cov.row.count("G12|CLK"), 0u);
    const std::size_t a = cov.row.at("G01|CLK");
    const std::size_t b = cov.row.at("G01|CB|C1C");
    ASSERT_EQ(cov.P.rows(), 2);
    ASSERT_EQ(cov.P.cols(), 2);
    EXPECT_DOUBLE_EQ(cov.P(static_cast<Eigen::Index>(a), static_cast<Eigen::Index>(a)),
                     1.2345678901234e-04);
    EXPECT_DOUBLE_EQ(cov.P(static_cast<Eigen::Index>(b), static_cast<Eigen::Index>(b)),
                     3.0517578125e-04);
    // Only the lower triangle is stored; the restore mirrors it.
    EXPECT_DOUBLE_EQ(cov.P(static_cast<Eigen::Index>(b), static_cast<Eigen::Index>(a)),
                     2.0e-05);
    EXPECT_DOUBLE_EQ(cov.P(static_cast<Eigen::Index>(a), static_cast<Eigen::Index>(b)),
                     2.0e-05);
    EXPECT_TRUE(cov.P.isApprox(cov.P.transpose()));
}

/// A snapshot with no QK/QP rows (the file a writer without
/// network.state_snapshot_coupling_capture produces) restores nothing.  The block
/// is left empty (the fold then degrades per key, as without the restore), never
/// partially populated.  The driver's restore-time warning calls the knob INERT
/// for the run, and the run-end line flags 0 keys restored.
TEST(ResumeBorderRestore, SnapshotWithoutQkQpRestoresNothing) {
    const NetworkStateSnapshot s = baseSnapshot();
    std::ostringstream os;
    NetworkStateSnapshotIO::serialize(os, s);
    const std::string text = os.str();
    EXPECT_EQ(text.find("\nQK,"), std::string::npos) << text;
    EXPECT_EQ(text.find("\nQP,"), std::string::npos) << text;

    NetworkStateSnapshot r;
    std::string err;
    std::istringstream is(text);
    ASSERT_TRUE(NetworkStateSnapshotIO::parse(is, r, &err)) << err;
    std::size_t dropped = 0;
    bool ok = false;
    const SharedCarryCov cov = restoreBorder(r, liveCarry(), &dropped, &ok);
    EXPECT_FALSE(ok);
    EXPECT_TRUE(cov.row.empty());
    EXPECT_EQ(cov.P.size(), 0);
}

/// Every key gone (a resume whose D rows all departed) is the other inert case:
/// nk == 0 fails the driver's screen, so prev_carry_cov is left untouched rather
/// than replaced by an empty block that would claim a dense fold it cannot do.
TEST(ResumeBorderRestore, NoLiveKeyLeavesTheBlockUntouched) {
    NetworkStateSnapshot r;
    std::string err;
    std::ostringstream os;
    NetworkStateSnapshotIO::serialize(os, borderSnapshot());
    std::istringstream is(os.str());
    ASSERT_TRUE(NetworkStateSnapshotIO::parse(is, r, &err)) << err;

    std::size_t dropped = 0;
    bool ok = false;
    const SharedCarryCov cov =
        restoreBorder(r, std::vector<CarryPrior>{}, &dropped, &ok);
    EXPECT_FALSE(ok);
    EXPECT_EQ(dropped, 3u);
    EXPECT_TRUE(cov.row.empty());
}

/// SH-VTEC never enters the dense block: its carry is the diagonal pre-gate
/// fold, and @ref sharedCarryKey has no key for the (n,m,trig) family, so a
/// VtecSh entry contributes no live key (it would alias onto the bare `sat`
/// fallback, which for an SH coefficient is the empty string).
TEST(ResumeBorderRestore, VtecShCarryNeverEntersTheBorder) {
    NetworkStateSnapshot s = borderSnapshot();
    s.carry_cov_keys.push_back("");        // what a VtecSh key would collapse to
    s.carry_cov_rows.push_back({0.0, 0.0, 0.0, 1.0});
    std::size_t dropped = 0;
    bool ok = false;
    const SharedCarryCov cov = restoreBorder(s, liveCarry(), &dropped, &ok);
    ASSERT_TRUE(ok);
    EXPECT_EQ(cov.row.count(""), 0u) << "the SH entry of liveCarry() must not match";
    EXPECT_EQ(cov.row.size(), 2u);
    EXPECT_EQ(dropped, 2u);
}
