/**
 * @file test_slip_oracle.cpp
 * @brief Simulation-only oracle slip response (`network.cycle_slip.oracle_relevel`,
 *        include/network/slip/NetworkSlipOracle.hpp): the ceiling for a level-preserving
 *        re-level.
 *
 * ## Scope
 *
 * The oracle is a decision-plus-actuation layer over one arc: given the fixture's
 * truth jumps, the arc's carried level and this epoch's time, it either shifts
 * @c LocalCarry::value_abs by the true sum d*lambda and tells the caller to skip
 * every freeing action, or it declines.  Every case here drives that layer
 * directly on a fabricated carry, so the four detector sites that call it
 * (network/slip/NetworkSlipQc.cpp) are out of scope.  Their population logic is covered in
 * tests/test_network_slip_qc.cpp, and the hook's contract ("true ⇒ the caller
 * frees nothing") is a caller-side property certified by a same-build
 * byte-identity A/B, not by a unit test.
 *
 * ## Bounds on what the oracle modifies
 *
 * A truth-fed repair is the strongest actuation in the slip stack: it writes a
 * converged-weight carried level directly, with no data supporting it.  The
 * assertions are therefore two-sided (@c var unchanged, @c last_seen_s unchanged,
 * the sibling band untouched, and the arc not freed), because an oracle that also
 * moved something else would corrupt the whole census without any visible sign.
 * @c ExactRepairLeavesEverythingButValueAbsAlone and the false-alarm keep half
 * check those bounds.
 *
 * ## Window semantics
 *
 * The oracle applies exactly the jumps the carried level does not already contain.
 * The window is `( max(last_applied, carry.birth_s, now − window_s), now ]`, with
 * the lower bound exclusive and the upper bound inclusive.  Each of the three
 * lower terms has a case here, because an error in any one of them produces a
 * ceiling that no run-end counter can contradict:
 *
 * - The detector-latency term (`now − window_s`) makes a late detector
 *   repairable.  `carry.last_seen_s` is not the lower bound, although it reads
 *   like an "already absorbed" mark: it is the previous epoch's stash, and a
 *   zero-q carried level is not re-set by the intervening solve, so a detector
 *   firing at t+1…t+3 on a slip at t still sees an un-absorbed jump.
 *   @c LateDetectorRepairsTheSlipItFiredOnEvenAfterARestash covers this and
 *   contrasts it with the same case under a window that ends at last_seen_s.
 * - `carry.birth_s` is the hard floor (@c SlipBeforeTheCarrysBirthIsAlreadyAbsorbed):
 *   a T4 erase-and-rebirth re-creates the level from a solve that already
 *   contains the earlier jump, so re-applying it would inject the error the card
 *   is meant to remove.
 * - `last_applied` makes two detectors on one arc apply the jump once
 *   (@c ASecondDetectorInTheSameWindowCannotDoubleApply).
 */

#include <gtest/gtest.h>

#include "common/GnssTypes.hpp"
#include "common/PppUtils.hpp"
#include "config/GnssConfig.hpp"
#include "logging/LogManager.hpp"
#include "network/slip/NetworkSlipOracle.hpp"

#include <spdlog/spdlog.h>
#include <spdlog/sinks/null_sink.h>

#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace GnssProcessor {
namespace {

constexpr const char* kSat  = "G22";
constexpr const char* kSig1 = "L1C";
constexpr const char* kSig2 = "L2W";
/// GPS week 2313, the campaign fixture's frame; 30 s cadence, tow ~302 8xx.
constexpr double kW = 2313.0 * 604800.0;

std::shared_ptr<spdlog::logger> nullLogger() {
    return std::make_shared<spdlog::logger>(
        "test_slip_oracle", std::make_shared<spdlog::sinks::null_sink_mt>());
}

/// A scratch fixture directory that cleans up after itself.
struct TempTree {
    std::filesystem::path root;
    explicit TempTree(const std::string& tag)
        : root(std::filesystem::temp_directory_path()
               / ("gnss_slip_oracle_" + tag)) {
        std::error_code ec;
        std::filesystem::remove_all(root, ec);
        std::filesystem::create_directories(root);
    }
    ~TempTree() { std::error_code ec; std::filesystem::remove_all(root, ec); }
    TempTree(const TempTree&) = delete;
    TempTree& operator=(const TempTree&) = delete;

    /// Write one station's truth_slips.csv with the fixture's exact header.
    std::filesystem::path writeStation(const std::string& sta,
                                       const std::string& rows) {
        const auto dir = root / sta;
        std::filesystem::create_directories(dir);
        const auto f = dir / "truth_slips.csv";
        std::ofstream out(f);
        out << "week,tow_s,satellite,signal,delta_cycles,flagged,elev_deg\n" << rows;
        return f;
    }
};

/// Forward slashes only: the JSON body below does not escape backslashes.
std::string jsonPath(const std::filesystem::path& p) {
    std::string s = p.string();
    for (auto& ch : s) if (ch == '\\') ch = '/';
    return s;
}

/// One oracle wired to a hand-built carry.  The wavelength provider is the
/// production @ref PppUtils, so the cycles->metres conversion under test is the
/// one the run uses.
struct Harness {
    LogManager        log_manager;
    NetworkSlipOracle oracle;
    net::ArcTable carry;

    Harness() {
        oracle.carry = &carry;
        oracle.utils = std::make_unique<PppUtils>(log_manager);
        addCarry(kSig1);
        addCarry(kSig2);
    }

    /// A carried level last stashed at @p last_seen_s (default: one 30 s epoch
    /// before the reference epoch, the normal warm state) and created at
    /// @p birth_s, the window's hard floor.  The default birth is a long-
    /// established arc (~47 min before every case's epoch), so a case that does
    /// not set it exercises the latency bound alone.
    void addCarry(const std::string& sig, double last_seen_s = kW + 302820.0,
                  double value_abs = 100.0, double var = 1.0e-5,
                  double birth_s = kW + 300000.0) {
        LocalCarry lc{};
        lc.value_abs   = value_abs;
        lc.var         = var;
        lc.last_seen_s = last_seen_s;
        lc.birth_s     = birth_s;
        net::putArcIn(carry, std::make_tuple(0, std::string(kSat), sig), &net::ArcState::amb) = lc;
    }
    void addTruth(const std::string& sig, double tow_s, int delta) {
        oracle.truth[std::make_tuple(0, std::string(kSat), sig)]
            .push_back({kW + tow_s, delta});
    }
    LocalCarry& c(const std::string& sig) {
        return (*net::getArc(carry, std::make_tuple(0, std::string(kSat), sig), &net::ArcState::amb));
    }
    /// @p cause is the calling detector's tier bit.  It steers only the
    /// false-alarm keep attribution, never a decision, so cases that are not
    /// about the tier split leave it at the MW drain's value.
    bool run(const std::string& sig, double tow_s, std::string& detail,
             OracleOutcome& oc,
             std::uint8_t cause = RelevelEpochState::kCauseMw) {
        return networkSlipOracleHandle(oracle, 0, kSat, sig, kW + tow_s, cause,
                                       detail, oc);
    }
    /// The production wavelength, so a case can predict the shift without
    /// hard-coding a constant a signal-table change would silently invalidate.
    double lambda(const std::string& sig) {
        return oracle.utils->getWavelength(System::GPS, sig, kSat);
    }
};

// ===========================================================================
// CSV parsing
// ===========================================================================

// The fixture writes one row per applied jump, so one arc can carry several; the
// parser keeps them all, in ascending time, keyed per (sat, signal).
TEST(SlipOracleCsv, ParsesMultipleSlipsPerArcInAscendingTime) {
    TempTree t("csv_multi");
    const auto f = t.writeStation("S01000SIM",
        "2313,304860.000,G11,L5Q,-88,1,13.90\n"
        "2313,302820.000,G19,L1C,48,1,11.50\n"     // deliberately out of order
        "2313,303540.000,G19,L1C,-7,0,14.02\n"
        "2313,302850.000,G04,L2W,-65,0,18.00\n");
    const auto m = networkSlipOracleParseCsv(f.string());

    ASSERT_EQ(m.size(), 3u);
    const auto& g19 = m.at({"G19", "L1C"});
    ASSERT_EQ(g19.size(), 2u);
    EXPECT_DOUBLE_EQ(g19[0].s, kW + 302820.0);   // sorted, not file order
    EXPECT_EQ(g19[0].delta_cyc, 48);
    EXPECT_DOUBLE_EQ(g19[1].s, kW + 303540.0);
    EXPECT_EQ(g19[1].delta_cyc, -7);
    EXPECT_EQ(m.at({"G04", "L2W"})[0].delta_cyc, -65);
    EXPECT_EQ(m.at({"G11", "L5Q"})[0].delta_cyc, -88);
}

// The header is checked by name, not position: a fixture that adds or reorders a
// column fails here instead of reading the wrong field as delta_cycles, which
// would produce a plausible-looking but wrong ceiling.
TEST(SlipOracleCsv, RejectsAnUnexpectedHeaderAMalformedRowAndAMissingFile) {
    {
        TempTree t("csv_hdr");
        const auto dir = t.root / "S01000SIM";
        std::filesystem::create_directories(dir);
        { std::ofstream f(dir / "truth_slips.csv");
          f << "week,tow_s,sat,sig,cycles\n2313,302820.000,G19,L1C,48\n"; }
        EXPECT_THROW(networkSlipOracleParseCsv((dir / "truth_slips.csv").string()),
                     std::runtime_error);
    }
    {
        TempTree t("csv_row");
        const auto f = t.writeStation("S01000SIM", "2313,302820.000,G19\n");
        EXPECT_THROW(networkSlipOracleParseCsv(f.string()), std::runtime_error);
    }
    {
        TempTree t("csv_missing");
        EXPECT_THROW(networkSlipOracleParseCsv((t.root / "nope.csv").string()),
                     std::runtime_error);
    }
}

// ===========================================================================
// Window semantics
// ===========================================================================

// Both edges of the latency window in one case, because they are one decision.
// A slip stamped at this epoch is un-absorbed (the detector is firing on it now)
// and is applied; the lower edge is exclusive, so a slip stamped exactly window_s
// back is out.  The carry's last stash (302820) sits in the middle of the window
// on purpose: it moves neither edge.
TEST(SlipOracleWindow, LatencyWindowIsExclusiveBelowAndInclusiveAtNow) {
    Harness h;
    ASSERT_DOUBLE_EQ(h.oracle.window_s, 90.0);   // three 30 s epochs
    h.addTruth(kSig1, 302760.0, 11);   // == now - window_s -> exclusive, out
    h.addTruth(kSig1, 302790.0, 3);    // inside, and before carry.last_seen_s
    h.addTruth(kSig1, 302850.0, 5);    // == now            -> inclusive, in
    h.addTruth(kSig1, 302880.0, 99);   // future            -> out

    std::string detail; OracleOutcome oc{};
    ASSERT_TRUE(h.run(kSig1, 302850.0, detail, oc));
    EXPECT_EQ(oc, OracleOutcome::Repaired);
    EXPECT_NEAR(h.c(kSig1).value_abs, 100.0 + 8.0 * h.lambda(kSig1), 1e-9);
    EXPECT_EQ(h.oracle.applied, 1);
    EXPECT_EQ(h.oracle.multi, 1);
    EXPECT_NEAR(h.oracle.sum_abs_cyc, 8.0, 1e-12);
    EXPECT_EQ(h.oracle.seen, 1);
    // The binding-term census: the latency edge bound the window here.  It is the
    // only instrument that separates "the window is too narrow" from "nothing
    // jumped", which have opposite fixes and identical repaired/false-alarm counts.
    EXPECT_EQ(h.oracle.bound_window, 1);
    EXPECT_EQ(h.oracle.bound_birth, 0);
    EXPECT_EQ(h.oracle.bound_applied, 0);
}

// Regression case.  Detectors fire late (MW has to fill its window, IF-TD needs a
// triple difference), so the normal population is a slip at t caught at t+1..t+3,
// by which time the harvest has re-stashed the carry at t+1.  The stash does not
// absorb the jump: the level is zero-q, so one epoch of phase barely moves the
// stiff prior and the integer shift still restores it.  Bounding the window at
// carry.last_seen_s instead of at the detector latency turns this whole
// population into false alarms and biases the reported ceiling low.  The
// window_s = 30 half reproduces that rule so a re-introduction fails here.
TEST(SlipOracleWindow, LateDetectorRepairsTheSlipItFiredOnEvenAfterARestash) {
    {
        Harness h;                                          // default 90 s window
        h.addCarry(kSig1, /*last_seen_s=*/kW + 302820.0);   // re-stashed at t+1
        h.addTruth(kSig1, 302790.0, -12);                   // the slip, at t
        std::string detail; OracleOutcome oc{};
        ASSERT_TRUE(h.run(kSig1, 302850.0, detail, oc));    // detected at t+2
        EXPECT_EQ(oc, OracleOutcome::Repaired);
        EXPECT_NEAR(h.c(kSig1).value_abs, 100.0 - 12.0 * h.lambda(kSig1), 1e-9);
        EXPECT_EQ(h.oracle.applied, 1);
        EXPECT_EQ(h.oracle.fa_kept, 0);
        EXPECT_NE(detail.find("delta=-12"), std::string::npos) << detail;
    }
    {
        // window_s = 30 s puts the lower edge exactly at last_seen_s, so this half
        // evaluates the pre-fix rule with the current code.
        Harness h;
        h.oracle.window_s = 30.0;
        h.addCarry(kSig1, /*last_seen_s=*/kW + 302820.0);
        h.addTruth(kSig1, 302790.0, -12);
        const double v0 = h.c(kSig1).value_abs;
        std::string detail; OracleOutcome oc{};
        ASSERT_TRUE(h.run(kSig1, 302850.0, detail, oc));
        EXPECT_EQ(oc, OracleOutcome::Kept);                 // the defect, on purpose
        EXPECT_DOUBLE_EQ(h.c(kSig1).value_abs, v0);
        EXPECT_EQ(h.oracle.applied, 0);
        EXPECT_EQ(h.oracle.fa_kept, 1);
    }
    {
        // window_s = 0 does not mean "same epoch only".  The lower bound is
        // exclusive, so window 0 collapses to (now, now] and admits nothing,
        // including this epoch's own slip.  Checked here because a pessimistic
        // setting of 0 would otherwise disable the oracle without any sign.
        Harness h;
        h.oracle.window_s = 0.0;
        h.addTruth(kSig1, 302850.0, 5);                     // stamped at this epoch
        std::string detail; OracleOutcome oc{};
        ASSERT_TRUE(h.run(kSig1, 302850.0, detail, oc));
        EXPECT_EQ(oc, OracleOutcome::Kept);
        EXPECT_EQ(h.oracle.applied, 0);
    }
}

// The hard floor.  A T4 attribution erase deletes the carry entry; the next
// harvest re-creates it from a solve whose level already contains every earlier
// jump.  A truth slip stamped before birth_s is therefore absorbed by construction,
// and re-applying it would inject the error this card is meant to remove, even
// though it sits well inside the latency window.  Two-sided: a post-birth slip in
// the same window is still repaired, so the floor trims the window instead of
// closing it.
TEST(SlipOracleWindow, SlipBeforeTheCarrysBirthIsAlreadyAbsorbed) {
    {
        Harness h;
        // Entry re-created at t+1 (the post-erase rebirth), slip at t.
        h.addCarry(kSig1, /*last_seen_s=*/kW + 302820.0, /*value_abs=*/100.0,
                   /*var=*/1.0e-5, /*birth_s=*/kW + 302820.0);
        h.addTruth(kSig1, 302790.0, -12);
        const double v0 = h.c(kSig1).value_abs;
        std::string detail; OracleOutcome oc{};
        ASSERT_TRUE(h.run(kSig1, 302850.0, detail, oc));
        EXPECT_EQ(oc, OracleOutcome::Kept);
        EXPECT_DOUBLE_EQ(h.c(kSig1).value_abs, v0);
        EXPECT_EQ(h.oracle.applied, 0);
        EXPECT_EQ(h.oracle.fa_kept, 1);
        EXPECT_EQ(h.oracle.bound_birth, 1);      // …because the arc is younger
        EXPECT_EQ(h.oracle.bound_window, 0);
    }
    {
        Harness h;
        h.addCarry(kSig1, /*last_seen_s=*/kW + 302820.0, /*value_abs=*/100.0,
                   /*var=*/1.0e-5, /*birth_s=*/kW + 302820.0);
        h.addTruth(kSig1, 302790.0, -12);   // pre-birth  -> absorbed
        h.addTruth(kSig1, 302840.0, 4);     // post-birth -> must apply
        std::string detail; OracleOutcome oc{};
        ASSERT_TRUE(h.run(kSig1, 302850.0, detail, oc));
        EXPECT_EQ(oc, OracleOutcome::Repaired);
        EXPECT_NEAR(h.c(kSig1).value_abs, 100.0 + 4.0 * h.lambda(kSig1), 1e-9);
        EXPECT_EQ(h.oracle.multi, 0);       // one jump in the trimmed window
        EXPECT_NE(detail.find("n=1"), std::string::npos) << detail;
    }
}

// Beyond the detectors' latency, an actuation is a false alarm: nothing in this
// stack looks 5 epochs back, so a jump that old is either already absorbed or not
// this arc's.  Keeping the level is the ceiling's answer, and the un-repaired
// population is counted in fa_kept instead of widening the window.
TEST(SlipOracleWindow, SlipOlderThanTheLatencyWindowIsAFalseAlarm) {
    Harness h;
    h.addTruth(kSig1, 302700.0, 25);   // 5 epochs back; the window is 3
    const double v0 = h.c(kSig1).value_abs;
    std::string detail; OracleOutcome oc{};
    ASSERT_TRUE(h.run(kSig1, 302850.0, detail, oc));
    EXPECT_EQ(oc, OracleOutcome::Kept);
    EXPECT_DOUBLE_EQ(h.c(kSig1).value_abs, v0);
    EXPECT_EQ(h.oracle.applied, 0);
    EXPECT_EQ(h.oracle.fa_kept, 1);
    EXPECT_EQ(detail, "delta=0;n=0;noise=0");
}

// Two detectors, one arc, one jump, on different epochs, both inside the 90 s
// window of the slip.  Only last_applied separates them (the latency bound still
// admits the slip at the second firing), so without it the shift would be applied
// twice, and the run-end applied count would look better for it.
TEST(SlipOracleWindow, ASecondDetectorInTheSameWindowCannotDoubleApply) {
    Harness h;
    h.addTruth(kSig1, 302820.0, 9);
    std::string d1, d2; OracleOutcome o1{}, o2{};

    ASSERT_TRUE(h.run(kSig1, 302850.0, d1, o1));        // t+1: MW fires
    ASSERT_EQ(o1, OracleOutcome::Repaired);
    const double after_first = h.c(kSig1).value_abs;
    EXPECT_NEAR(after_first, 100.0 + 9.0 * h.lambda(kSig1), 1e-9);

    ASSERT_TRUE(h.run(kSig1, 302880.0, d2, o2));        // t+2: IF-TD fires
    EXPECT_EQ(o2, OracleOutcome::Kept);
    EXPECT_DOUBLE_EQ(h.c(kSig1).value_abs, after_first);
    EXPECT_EQ(h.oracle.applied, 1);
    EXPECT_EQ(h.oracle.fa_kept, 1);
    EXPECT_EQ(h.oracle.seen, 2);
    EXPECT_EQ(h.oracle.bound_applied, 1);        // the second firing, attributed
    EXPECT_EQ(h.oracle.bound_window, 1);         // the first
}

// Several jumps inside one window are summed: the level is a single scalar and the
// repair puts the arc where truth put it, not where the first jump did.
TEST(SlipOracleWindow, MultipleSlipsInsideTheWindowSumAndAreCountedAsMulti) {
    Harness h;
    h.addTruth(kSig1, 302790.0, 7);
    h.addTruth(kSig1, 302820.0, -3);
    h.addTruth(kSig1, 302850.0, 10);

    std::string detail; OracleOutcome oc{};
    ASSERT_TRUE(h.run(kSig1, 302850.0, detail, oc));
    EXPECT_EQ(oc, OracleOutcome::Repaired);
    EXPECT_NEAR(h.c(kSig1).value_abs, 100.0 + 14.0 * h.lambda(kSig1), 1e-9);
    EXPECT_EQ(h.oracle.applied, 1);
    EXPECT_EQ(h.oracle.multi, 1);
    EXPECT_NEAR(h.oracle.max_abs_cyc, 14.0, 1e-12);
    EXPECT_NE(detail.find("n=3"), std::string::npos) << detail;
    EXPECT_NE(detail.find("delta=14"), std::string::npos) << detail;
}

// `last_applied` advances so a second detector firing on the same arc in the
// same (or the next) epoch cannot re-apply a jump the first already applied.
// Without it the two-detector case (MW and IF-TD both flagging one arc) would
// double the shift, and the run-end `applied` count would look better for it.
// This case covers the same epoch.
TEST(SlipOracleWindow, LastAppliedAdvancesSoASecondActuationCannotDoubleApply) {
    Harness h;
    h.addTruth(kSig1, 302850.0, 5);
    std::string d1, d2; OracleOutcome o1{}, o2{};

    ASSERT_TRUE(h.run(kSig1, 302850.0, d1, o1));
    ASSERT_EQ(o1, OracleOutcome::Repaired);
    const double after_first = h.c(kSig1).value_abs;

    // Same epoch, second detector: the window is now empty -> false-alarm keep.
    ASSERT_TRUE(h.run(kSig1, 302850.0, d2, o2));
    EXPECT_EQ(o2, OracleOutcome::Kept);
    EXPECT_DOUBLE_EQ(h.c(kSig1).value_abs, after_first);
    EXPECT_EQ(h.oracle.applied, 1);
    EXPECT_EQ(h.oracle.fa_kept, 1);
    EXPECT_EQ(h.oracle.seen, 2);
}

// ===========================================================================
// Actuation bounds: what the oracle leaves alone
// ===========================================================================

// The ceiling assumes a perfect repair costs no information, so the level keeps
// its converged weight: `var` is unchanged by construction.  The stash stamp is
// unchanged too (the harvest owns it), the sibling band is untouched (a slip on
// L1C is a slip on L1C only), and, most important, the arc is not freed: the hook
// returning true tells the caller to skip every freeing action, and a repair that
// also freed would measure the current response, not the ceiling.
TEST(SlipOracleActuate, ExactRepairLeavesEverythingButValueAbsAlone) {
    Harness h;
    h.addTruth(kSig1, 302850.0, -12);
    const double var0  = h.c(kSig1).var;
    const double seen0 = h.c(kSig1).last_seen_s;
    const double sib0  = h.c(kSig2).value_abs;

    std::string detail; OracleOutcome oc{};
    ASSERT_TRUE(h.run(kSig1, 302850.0, detail, oc));
    EXPECT_EQ(oc, OracleOutcome::Repaired);
    EXPECT_NEAR(h.c(kSig1).value_abs, 100.0 - 12.0 * h.lambda(kSig1), 1e-9);
    EXPECT_DOUBLE_EQ(h.c(kSig1).var, var0);              // converged weight kept
    EXPECT_DOUBLE_EQ(h.c(kSig1).last_seen_s, seen0);     // the harvest owns this
    EXPECT_DOUBLE_EQ(h.c(kSig2).value_abs, sib0);        // per-signal, not per-sat
    EXPECT_NE(detail.find("noise=0.0000"), std::string::npos) << detail;
    // The exact ceiling: the shift is an exact integer number of cycles.
    const double cyc = (h.c(kSig1).value_abs - 100.0) / h.lambda(kSig1);
    EXPECT_NEAR(cyc, std::round(cyc), 1e-9);
}

// An arc with no carried level has nothing to preserve, so today's response is
// already the right one and the hook declines (returns false) instead of
// consuming the actuation.  Counted separately: a large `no_carry` in a run with
// many actuations would mean the vehicle is not what the card assumed.
TEST(SlipOracleActuate, NoCarryEntryFallsThroughToTodaysResponse) {
    Harness h;
    h.carry.clear();
    h.addTruth(kSig1, 302850.0, 5);
    std::string detail; OracleOutcome oc{};
    EXPECT_FALSE(h.run(kSig1, 302850.0, detail, oc));
    EXPECT_EQ(oc, OracleOutcome::NotHandled);
    EXPECT_EQ(h.oracle.no_carry, 1);
    EXPECT_EQ(h.oracle.applied, 0);
    EXPECT_EQ(h.oracle.fa_kept, 0);      // not a false alarm; counted as no_carry
    EXPECT_EQ(h.oracle.seen, 1);
}

// An unknown signal has no wavelength, so the shift cannot be formed.  It falls
// through to today's response and is counted in its own bucket: rolling it into
// `fa_kept` would report "truth says nothing slipped" for an arc the oracle
// simply could not measure, and the closure would still balance.
TEST(SlipOracleActuate, UnknownSignalIsCountedSeparatelyAndFallsThrough) {
    Harness h;
    h.addCarry("LZZ");
    h.oracle.truth[std::make_tuple(0, std::string(kSat), std::string("LZZ"))]
        .push_back({kW + 302850.0, 5});
    const double v0 = h.c("LZZ").value_abs;

    std::string detail; OracleOutcome oc{};
    EXPECT_FALSE(h.run("LZZ", 302850.0, detail, oc));
    EXPECT_EQ(oc, OracleOutcome::NotHandled);
    EXPECT_DOUBLE_EQ(h.c("LZZ").value_abs, v0);
    EXPECT_EQ(h.oracle.no_lambda, 1);
    EXPECT_EQ(h.oracle.applied, 0);
    EXPECT_EQ(h.oracle.fa_kept, 0);
    // Closure (the run-end line's identity).
    EXPECT_EQ(h.oracle.seen, h.oracle.applied + h.oracle.true_slip_freed
                             + h.oracle.fa_kept + h.oracle.fa_freed
                             + h.oracle.no_carry + h.oracle.no_lambda);
    // …and the binding census's own: every actuation that got past the carry
    // lookup is attributed to exactly one lower-bound term, including this one
    // (the wavelength check is downstream of the window scan).
    EXPECT_EQ(h.oracle.seen - h.oracle.no_carry,
              h.oracle.bound_window + h.oracle.bound_birth + h.oracle.bound_applied);
}

// ===========================================================================
// The false-alarm policy: the two halves of the ceiling
// ===========================================================================

// `keep` is the ceiling: the oracle knows the detector was wrong and leaves the
// converged level where it was, so the arm measures repair quality with the
// false-alarm cost removed.  `free` keeps that cost, which separates "the repair
// is the lever" from "the false alarms are the lever".
TEST(SlipOracleFalseAlarm, KeepTouchesNothingAndFreeFallsThrough) {
    {
        Harness keep;                       // default policy
        ASSERT_EQ(keep.oracle.false_alarm, NetworkSlipOracle::FalseAlarm::Keep);
        const double v0 = keep.c(kSig1).value_abs;
        const double s0 = keep.c(kSig1).var;
        std::string detail; OracleOutcome oc{};
        EXPECT_TRUE(keep.run(kSig1, 302850.0, detail, oc));   // handled ⇒ not freed
        EXPECT_EQ(oc, OracleOutcome::Kept);
        EXPECT_DOUBLE_EQ(keep.c(kSig1).value_abs, v0);
        EXPECT_DOUBLE_EQ(keep.c(kSig1).var, s0);
        EXPECT_EQ(keep.oracle.fa_kept, 1);
        EXPECT_EQ(keep.oracle.fa_freed, 0);
        EXPECT_EQ(detail, "delta=0;n=0;noise=0");
    }
    {
        Harness fr;
        fr.oracle.false_alarm = NetworkSlipOracle::FalseAlarm::Free;
        const double v0 = fr.c(kSig1).value_abs;
        std::string detail; OracleOutcome oc{};
        EXPECT_FALSE(fr.run(kSig1, 302850.0, detail, oc));    // caller frees as today
        EXPECT_EQ(oc, OracleOutcome::NotHandled);
        EXPECT_DOUBLE_EQ(fr.c(kSig1).value_abs, v0);          // the oracle wrote nothing
        EXPECT_EQ(fr.oracle.fa_freed, 1);
        EXPECT_EQ(fr.oracle.fa_kept, 0);
    }
}

// ===========================================================================
// The true-slip policy: the false-alarm isolation arm
// ===========================================================================

// The isolation arm's contract, both halves in one case because they are one
// decision: with `true_slip = Free` a real slip is declined (the caller runs
// today's ordinary free/re-level for it, as on the base arm) while a false alarm
// is still kept.  The only difference from the base arm is therefore the false
// alarms, which makes their cost separable from the true-slip re-level's.
// `applied` is 0 by construction here, so a run-end inertness check keyed on it
// would fire on a correctly working arm; the keep count is this policy's
// actuation counter.
TEST(SlipOracleTrueSlip, FreeDeclinesARealSlipAndStillKeepsTheFalseAlarm) {
    Harness h;
    h.oracle.true_slip = NetworkSlipOracle::TrueSlip::Free;
    ASSERT_EQ(h.oracle.false_alarm, NetworkSlipOracle::FalseAlarm::Keep);
    h.addTruth(kSig1, 302850.0, 7);          // a real slip on band 1
    const double v1 = h.c(kSig1).value_abs;
    const double v2 = h.c(kSig2).value_abs;  // band 2 never jumped

    {   // true slip -> declined: the caller frees, the oracle wrote nothing
        std::string detail; OracleOutcome oc{};
        EXPECT_FALSE(h.run(kSig1, 302850.0, detail, oc));
        EXPECT_EQ(oc, OracleOutcome::NotHandled);
        EXPECT_DOUBLE_EQ(h.c(kSig1).value_abs, v1);
        EXPECT_EQ(h.oracle.true_slip_freed, 1);
        EXPECT_EQ(h.oracle.applied, 0);
        EXPECT_EQ(h.oracle.fa_kept, 0);      // not a false alarm: truth says it jumped
        // The ledger does not advance: nothing was applied, so a second detector
        // on this arc sees the same window (and declines again).
        EXPECT_DOUBLE_EQ(h.oracle.arcs.at(std::make_tuple(0, std::string(kSat), kSig1))
                             .last_applied_s, -1.0e18);
    }
    {   // false alarm -> kept, as on the ceiling arm
        std::string detail; OracleOutcome oc{};
        EXPECT_TRUE(h.run(kSig2, 302850.0, detail, oc));
        EXPECT_EQ(oc, OracleOutcome::Kept);
        EXPECT_DOUBLE_EQ(h.c(kSig2).value_abs, v2);
        EXPECT_EQ(h.oracle.fa_kept, 1);
        EXPECT_EQ(h.oracle.fa_freed, 0);
    }
    EXPECT_EQ(h.oracle.seen, h.oracle.applied + h.oracle.true_slip_freed
                             + h.oracle.fa_kept + h.oracle.fa_freed
                             + h.oracle.no_carry + h.oracle.no_lambda);
}

// `repair` is the default and reproduces the pre-knob behaviour bit for bit: the
// knob adds a second arm and leaves the one already measured unchanged.  Checked on
// the same slip the isolation case declines, so the two read as a pair.
TEST(SlipOracleTrueSlip, RepairIsTheDefaultAndKeepsTodaysBehaviour) {
    Harness h;
    ASSERT_EQ(h.oracle.true_slip, NetworkSlipOracle::TrueSlip::Repair);
    h.addTruth(kSig1, 302850.0, 7);
    std::string detail; OracleOutcome oc{};
    ASSERT_TRUE(h.run(kSig1, 302850.0, detail, oc));
    EXPECT_EQ(oc, OracleOutcome::Repaired);
    EXPECT_NEAR(h.c(kSig1).value_abs, 100.0 + 7.0 * h.lambda(kSig1), 1e-9);
    EXPECT_EQ(h.oracle.applied, 1);
    EXPECT_EQ(h.oracle.true_slip_freed, 0);
}

// The fourth corner of the 2x2: both halves decline, so the oracle actuates on
// nothing.  It serves as a byte-identity control for the hook itself, and it is a
// clean no-op, not a partial response.
TEST(SlipOracleTrueSlip, BothPoliciesFreeIsAPureNoOp) {
    Harness h;
    h.oracle.true_slip   = NetworkSlipOracle::TrueSlip::Free;
    h.oracle.false_alarm = NetworkSlipOracle::FalseAlarm::Free;
    h.addTruth(kSig1, 302850.0, 7);
    const double v1 = h.c(kSig1).value_abs, v2 = h.c(kSig2).value_abs;

    std::string d1, d2; OracleOutcome o1{}, o2{};
    EXPECT_FALSE(h.run(kSig1, 302850.0, d1, o1));   // true slip
    EXPECT_FALSE(h.run(kSig2, 302850.0, d2, o2));   // false alarm
    EXPECT_DOUBLE_EQ(h.c(kSig1).value_abs, v1);
    EXPECT_DOUBLE_EQ(h.c(kSig2).value_abs, v2);
    EXPECT_EQ(h.oracle.true_slip_freed, 1);
    EXPECT_EQ(h.oracle.fa_freed, 1);
    EXPECT_EQ(h.oracle.applied, 0);
    EXPECT_EQ(h.oracle.fa_kept, 0);
}

// A signal with no wavelength and a truth slip lands in `true_slip_freed`, not
// `no_lambda`: on this policy no shift is ever formed, so the wavelength is never
// consulted, and counting it as an unmeasurable arc would book the same decline
// twice and break the closure.  The decision is a decline either way; only the
// census bucket depends on the order.
TEST(SlipOracleTrueSlip, FreeDeclinesBeforeTheWavelengthLookup) {
    Harness h;
    h.oracle.true_slip = NetworkSlipOracle::TrueSlip::Free;
    h.addCarry("LZZ");
    h.oracle.truth[std::make_tuple(0, std::string(kSat), std::string("LZZ"))]
        .push_back({kW + 302850.0, 5});

    std::string detail; OracleOutcome oc{};
    EXPECT_FALSE(h.run("LZZ", 302850.0, detail, oc));
    EXPECT_EQ(h.oracle.true_slip_freed, 1);
    EXPECT_EQ(h.oracle.no_lambda, 0);
    EXPECT_EQ(h.oracle.seen, h.oracle.applied + h.oracle.true_slip_freed
                             + h.oracle.fa_kept + h.oracle.fa_freed
                             + h.oracle.no_carry + h.oracle.no_lambda);
}

// ===========================================================================
// The false-alarm tier split
// ===========================================================================

// A false-alarm isolation arm that reports one total cannot say which detector's
// thresholds to examine, and the four sites use four unrelated statistics.  The
// tier bit is the site's own `kCause*`; it steers only the attribution (never a
// decision), and the split closes on `fa_kept` with an empty `other`.  A nonzero
// `other` means a call site passes a bit this split does not name, i.e. a detector
// whose false alarms are attributed to nothing.
TEST(SlipOracleTier, FalseAlarmKeepsAttributeToTheCallingDetectorAndClose) {
    Harness h;
    std::string detail; OracleOutcome oc{};
    // Four actuations on four distinct arcs (one per tier), none with truth.
    h.addCarry("L5Q");
    h.addCarry("L1W");
    ASSERT_TRUE(h.run(kSig1, 302850.0, detail, oc, RelevelEpochState::kCauseMw));
    ASSERT_TRUE(h.run(kSig2, 302850.0, detail, oc, RelevelEpochState::kCauseLli));
    ASSERT_TRUE(h.run("L5Q",  302850.0, detail, oc, RelevelEpochState::kCauseIfTd));
    ASSERT_TRUE(h.run("L1W",  302850.0, detail, oc, RelevelEpochState::kCauseWindup));

    EXPECT_EQ(h.oracle.fa_kept, 4);
    EXPECT_EQ(h.oracle.fa_kept_mw, 1);
    EXPECT_EQ(h.oracle.fa_kept_lli, 1);
    EXPECT_EQ(h.oracle.fa_kept_iftd, 1);
    EXPECT_EQ(h.oracle.fa_kept_wusat, 1);
    EXPECT_EQ(h.oracle.fa_kept_other, 0);
    EXPECT_EQ(h.oracle.fa_kept, h.oracle.fa_kept_mw + h.oracle.fa_kept_lli
                                + h.oracle.fa_kept_iftd + h.oracle.fa_kept_wusat
                                + h.oracle.fa_kept_other);
}

// The tier does not reach any decision: a repaired arc and a `free`-policy
// decline are attributed to nothing at all, so the split counts keeps only.  (An
// unrecognised bit is the mis-wiring case; it is counted in `other`, not folded
// into one of the four.)
TEST(SlipOracleTier, OnlyKeepsAreAttributedAndAnUnknownBitIsVisible) {
    {
        Harness h;
        h.addTruth(kSig1, 302850.0, 3);
        std::string detail; OracleOutcome oc{};
        ASSERT_TRUE(h.run(kSig1, 302850.0, detail, oc, RelevelEpochState::kCauseIfTd));
        EXPECT_EQ(oc, OracleOutcome::Repaired);
        EXPECT_EQ(h.oracle.fa_kept_iftd, 0);       // a repair is not a keep
    }
    {
        Harness h;
        h.oracle.false_alarm = NetworkSlipOracle::FalseAlarm::Free;
        std::string detail; OracleOutcome oc{};
        EXPECT_FALSE(h.run(kSig1, 302850.0, detail, oc, RelevelEpochState::kCauseMw));
        EXPECT_EQ(h.oracle.fa_freed, 1);
        EXPECT_EQ(h.oracle.fa_kept_mw, 0);         // a freed false alarm is not a keep
    }
    {
        Harness h;
        std::string detail; OracleOutcome oc{};
        ASSERT_TRUE(h.run(kSig1, 302850.0, detail, oc,
                          RelevelEpochState::kCauseT4Attrib));   // no call site passes this
        EXPECT_EQ(h.oracle.fa_kept, 1);
        EXPECT_EQ(h.oracle.fa_kept_other, 1);
        EXPECT_EQ(h.oracle.fa_kept_mw + h.oracle.fa_kept_lli
                  + h.oracle.fa_kept_iftd + h.oracle.fa_kept_wusat, 0);
    }
}

// ===========================================================================
// Noise (the precision ladder's knob)
// ===========================================================================

// The ladder answers "how good does an integer estimate have to be?", so its
// draws are reproducible: a re-executed run, or two detectors hitting one arc in
// one epoch, sees the same perturbation.  Determinism is the contract under test;
// the distribution is not.
TEST(SlipOracleNoise, IsDeterministicPerKeyAndVariesAcrossKeys) {
    const double a1 = networkSlipOracleNoise(0.05, 0, kSat, kSig1, kW + 302850.0);
    const double a2 = networkSlipOracleNoise(0.05, 0, kSat, kSig1, kW + 302850.0);
    EXPECT_DOUBLE_EQ(a1, a2);                                    // same key ⇒ same draw
    EXPECT_NE(a1, networkSlipOracleNoise(0.05, 1, kSat, kSig1, kW + 302850.0));
    EXPECT_NE(a1, networkSlipOracleNoise(0.05, 0, kSat, kSig2, kW + 302850.0));
    EXPECT_NE(a1, networkSlipOracleNoise(0.05, 0, "G23", kSig1, kW + 302850.0));
    EXPECT_NE(a1, networkSlipOracleNoise(0.05, 0, kSat, kSig1, kW + 302880.0));
    // sigma 0 is the exact ceiling and returns exactly 0, not a tiny draw.
    EXPECT_DOUBLE_EQ(networkSlipOracleNoise(0.0, 0, kSat, kSig1, kW + 302850.0), 0.0);
}

// …and the applied shift carries it, so a laddered arm is a different but
// reproducible number, not a different run.
TEST(SlipOracleNoise, PerturbsTheAppliedShiftReproducibly) {
    const double expect_noise =
        networkSlipOracleNoise(0.05, 0, kSat, kSig1, kW + 302850.0);
    ASSERT_NE(expect_noise, 0.0);

    double first = 0.0;
    for (int pass = 0; pass < 2; ++pass) {
        Harness h;
        h.oracle.noise_cyc = 0.05;
        h.addTruth(kSig1, 302850.0, 5);
        std::string detail; OracleOutcome oc{};
        ASSERT_TRUE(h.run(kSig1, 302850.0, detail, oc));
        EXPECT_NEAR(h.c(kSig1).value_abs,
                    100.0 + (5.0 + expect_noise) * h.lambda(kSig1), 1e-9);
        // sum_abs_cyc records the truth magnitude, not the perturbed one: the
        // counter describes the population, not the injection.
        EXPECT_NEAR(h.oracle.sum_abs_cyc, 5.0, 1e-12);
        if (pass == 0) first = h.c(kSig1).value_abs;
        else EXPECT_DOUBLE_EQ(h.c(kSig1).value_abs, first);
    }
}

// ===========================================================================
// Load-time contract: the field-inertness mechanism
// ===========================================================================

/// A minimal loadable PPP-RTK network config; @p extra splices into the network
/// block (the test_carry_zwd_in_block.cpp loader-fixture pattern).
std::string networkConfig(const std::string& extra) {
    return std::string("{\n")
        + "  \"processing\": {\n"
          "    \"mode\": \"ppp-rtk-network\",\n"
          "    \"active_systems\": [ \"GPS\" ],\n"
          "    \"signal_priorities\": { \"GPS\": \"WC\" }\n"
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

bool loadInto(const std::string& tag, const std::string& extra, GnssConfig& cfg) {
    LogManager lm;
    const auto p = std::filesystem::temp_directory_path()
                   / ("slip_oracle_cfg_" + tag + ".json");
    { std::ofstream f(p); f << networkConfig(extra); }
    const bool ok = cfg.loadFromJson(p, lm);
    std::error_code ec; std::filesystem::remove(p, ec);
    return ok;
}

/// The oracle's own arm, spliced into the network block.
std::string oracleArm(const std::string& root, const std::string& extra = {}) {
    return std::string("    \"phase_leveling\": true,\n")
         + "    \"cycle_slip\": {\n"
           "      \"oracle_relevel\": true,\n"
           "      \"oracle_truth_root\": \"" + root + "\""
         + (extra.empty() ? "\n" : ",\n") + extra
         + "    },\n";
}

// A sim fixture writes one truth file per station, so a station without one
// means the root is mis-pointed (or the roster is not this fixture's).  A partial
// repair would produce a ceiling that is neither the current response nor the
// perfect one, which cannot be interpreted, so this is a hard error.
TEST(SlipOracleLoad, MissingStationTruthFileIsAHardErrorAndTheRosterIndexesTheTable) {
    TempTree t("load");
    t.writeStation("S01000SIM", "2313,302820.000,G19,L1C,48,1,11.50\n");
    // S02000SIM deliberately absent.

    GnssConfig cfg;
    ASSERT_TRUE(loadInto("load", oracleArm(jsonPath(t.root)), cfg));
    ASSERT_TRUE(cfg.getNetworkConfig().slip_qc.oracle_relevel);

    LogManager lm;
    auto log = nullLogger();
    std::vector<NetStation> stations(2);
    stations[0].name = "S01000SIM";
    stations[1].name = "S02000SIM";
    EXPECT_THROW(networkSlipOracleLoad(cfg, stations, lm, log), std::runtime_error);

    // …and with the roster trimmed to what the fixture actually has, it loads and
    // indexes by station index (the local_carry key's first element).
    stations.pop_back();
    auto oracle = networkSlipOracleLoad(cfg, stations, lm, log);
    ASSERT_TRUE(oracle);
    ASSERT_EQ(oracle->truth.size(), 1u);
    const auto& v = oracle->truth.at(std::make_tuple(0, std::string("G19"),
                                                     std::string("L1C")));
    ASSERT_EQ(v.size(), 1u);
    EXPECT_EQ(v[0].delta_cyc, 48);
    EXPECT_DOUBLE_EQ(v[0].s, kW + 302820.0);
    EXPECT_EQ(oracle->false_alarm, NetworkSlipOracle::FalseAlarm::Keep);
    EXPECT_EQ(oracle->true_slip, NetworkSlipOracle::TrueSlip::Repair);
    EXPECT_DOUBLE_EQ(oracle->noise_cyc, 0.0);
    EXPECT_DOUBLE_EQ(oracle->window_s, 90.0);   // the knob reaches the run object
}

// ===========================================================================
// Interlocks (loader-side)
// ===========================================================================

// The oracle writes LocalCarry::value_abs, so it is inert without the leveling
// carry and structurally empty under carry_invariant.  Both cases disarm it (not
// warn-only): a ceiling measured on a vehicle the mechanism cannot actuate would
// read as a falsification of level preservation, a wrong verdict rather than a
// partial one.
TEST(SlipOracleInterlock, DisarmedWithoutPhaseLevelingAndUnderCarryInvariant) {
    TempTree t("interlock");
    t.writeStation("S01000SIM", "2313,302820.000,G19,L1C,48,1,11.50\n");
    const std::string root = jsonPath(t.root);

    {   // the validated posture: leveling on, invariant off
        GnssConfig cfg;
        ASSERT_TRUE(loadInto("il_on", oracleArm(root), cfg));
        EXPECT_TRUE(cfg.getNetworkConfig().slip_qc.oracle_relevel);
    }
    {   // no leveling carry to repair
        GnssConfig cfg;
        std::string arm = oracleArm(root);
        const auto pos = arm.find("\"phase_leveling\": true");
        ASSERT_NE(pos, std::string::npos);
        arm.replace(pos, std::string("\"phase_leveling\": true").size(),
                    "\"phase_leveling\": false");
        ASSERT_TRUE(loadInto("il_nopl", arm, cfg));
        EXPECT_FALSE(cfg.getNetworkConfig().slip_qc.oracle_relevel);
    }
    {   // The DD invariant vehicle: the leveling carry is superseded and
        // structurally empty there.  Configured as a carry_invariant lineage is
        // (phase_leveling off and the eq34 chain datum on; the loader refuses
        // carry_invariant without it), because the disarm has to hold on the real
        // configuration, not a synthetic one with both vehicles on.
        GnssConfig cfg;
        std::string arm = oracleArm(root);
        const auto pos = arm.find("\"phase_leveling\": true");
        ASSERT_NE(pos, std::string::npos);
        arm.replace(pos, std::string("\"phase_leveling\": true").size(),
                    "\"phase_leveling\": false");
        ASSERT_TRUE(loadInto("il_ci",
                             arm + "    \"carry_invariant\": true,\n"
                                   "    \"phase_datum_eq34\": true,\n", cfg));
        EXPECT_FALSE(cfg.getNetworkConfig().slip_qc.oracle_relevel);
    }
}

// The loader's hard errors.  A root that does not exist is the field-inertness
// mechanism (a field lineage has no truth file); an unrecognised false-alarm
// policy is a typo that would otherwise fall back to `keep` and mislabel a whole
// arm; a negative noise sigma is refused too.  Each surfaces as a refused load:
// GnssConfig::loadFromJson catches and returns false, and Application treats that
// as fatal.
TEST(SlipOracleInterlock, LoaderRefusesAMissingRootABadPolicyAndNegativeNoise) {
    TempTree t("loader_err");
    t.writeStation("S01000SIM", "2313,302820.000,G19,L1C,48,1,11.50\n");
    const std::string root = jsonPath(t.root);
    {
        GnssConfig cfg;
        EXPECT_FALSE(loadInto("err_root", oracleArm(jsonPath(t.root / "nope")), cfg));
    }
    {
        GnssConfig cfg;
        EXPECT_FALSE(loadInto("err_fa",
                              oracleArm(root, "      \"oracle_false_alarm\": \"reset\"\n"),
                              cfg));
    }
    {
        GnssConfig cfg;
        EXPECT_FALSE(loadInto("err_noise",
                              oracleArm(root, "      \"oracle_noise_cyc\": -1.0\n"), cfg));
    }
    // `free` is the other legal policy and loads.
    {
        GnssConfig cfg;
        ASSERT_TRUE(loadInto("ok_free",
                             oracleArm(root, "      \"oracle_false_alarm\": \"free\"\n"),
                             cfg));
        EXPECT_EQ(cfg.getNetworkConfig().slip_qc.oracle_false_alarm, "free");
    }
    // …and the same contract on the other half of the 2x2.  A typo here would
    // fall back to `repair`, and the arm would report the ceiling while being read
    // as the false-alarm isolation; the two verdicts point at different cards.
    {
        GnssConfig cfg;
        EXPECT_FALSE(loadInto("err_ts",
                              oracleArm(root, "      \"oracle_true_slip\": \"keep\"\n"),
                              cfg));
    }
    {
        GnssConfig cfg;
        ASSERT_TRUE(loadInto("ok_ts_free",
                             oracleArm(root, "      \"oracle_true_slip\": \"free\"\n"),
                             cfg));
        EXPECT_EQ(cfg.getNetworkConfig().slip_qc.oracle_true_slip, "free");
    }
    {   // the isolation arm's exact spelling, and the no-op corner: both legal
        GnssConfig cfg;
        ASSERT_TRUE(loadInto("ok_noop",
                             oracleArm(root, "      \"oracle_true_slip\": \"free\",\n"
                                             "      \"oracle_false_alarm\": \"free\"\n"),
                             cfg));
        EXPECT_EQ(cfg.getNetworkConfig().slip_qc.oracle_true_slip, "free");
        EXPECT_EQ(cfg.getNetworkConfig().slip_qc.oracle_false_alarm, "free");
    }
    // Default off: with the knob absent it stays off and no root is required,
    // which is the byte-identity precondition (standing invariant #6).
    {
        GnssConfig cfg;
        ASSERT_TRUE(loadInto("off", "    \"phase_leveling\": true,\n", cfg));
        const auto& sq = cfg.getNetworkConfig().slip_qc;
        EXPECT_FALSE(sq.oracle_relevel);
        EXPECT_TRUE(sq.oracle_truth_root.empty());
        EXPECT_EQ(sq.oracle_false_alarm, "keep");
        EXPECT_EQ(sq.oracle_true_slip, "repair");
        EXPECT_DOUBLE_EQ(sq.oracle_noise_cyc, 0.0);
        EXPECT_DOUBLE_EQ(sq.oracle_window_s, 90.0);
    }
}

// …and the knob reaches the run object, not just the loaded config: the policy
// is read once at construction, so a loader that parsed it into the config and
// never handed it over would run the ceiling while the manifest, the summary
// line and the run-end clause all said `free`.
TEST(SlipOracleTrueSlip, TheKnobReachesTheRunObject) {
    TempTree t("ts_run");
    t.writeStation("S01000SIM", "2313,302820.000,G19,L1C,48,1,11.50\n");
    const std::string root = jsonPath(t.root);

    LogManager lm;
    auto log = nullLogger();
    std::vector<NetStation> stations(1);
    stations[0].name = "S01000SIM";
    {
        GnssConfig cfg;
        ASSERT_TRUE(loadInto("ts_run_free",
                             oracleArm(root, "      \"oracle_true_slip\": \"free\"\n"), cfg));
        auto oracle = networkSlipOracleLoad(cfg, stations, lm, log);
        ASSERT_TRUE(oracle);
        EXPECT_EQ(oracle->true_slip, NetworkSlipOracle::TrueSlip::Free);
        EXPECT_EQ(oracle->false_alarm, NetworkSlipOracle::FalseAlarm::Keep);  // the isolation pair
    }
    {   // the knob absent -> the shipped ceiling, unchanged
        GnssConfig cfg;
        ASSERT_TRUE(loadInto("ts_run_def", oracleArm(root), cfg));
        auto oracle = networkSlipOracleLoad(cfg, stations, lm, log);
        ASSERT_TRUE(oracle);
        EXPECT_EQ(oracle->true_slip, NetworkSlipOracle::TrueSlip::Repair);
    }
}

// The detector-latency window is the knob the repaired/false-alarm split is most
// sensitive to, so its default is checked here instead of being left to the
// struct: 90 s is three 30 s epochs, the latency the truth census counts as
// detected.  Unlike the noise sigma it is clamped, not refused: 0 is a legal
// setting (because the lower bound is exclusive, it admits no truth slip at all;
// see LateDetectorRepairsTheSlipItFiredOnEvenAfterARestash), so a negative value
// has one sensible reading and refusing the load would cost an arm for a typo.
TEST(SlipOracleInterlock, WindowKnobDefaultsToNinetySecondsAndClampsNegative) {
    TempTree t("window");
    t.writeStation("S01000SIM", "2313,302820.000,G19,L1C,48,1,11.50\n");
    const std::string root = jsonPath(t.root);
    {   // armed, knob absent -> the default
        GnssConfig cfg;
        ASSERT_TRUE(loadInto("w_def", oracleArm(root), cfg));
        EXPECT_DOUBLE_EQ(cfg.getNetworkConfig().slip_qc.oracle_window_s, 90.0);
    }
    {   // an explicit value survives
        GnssConfig cfg;
        ASSERT_TRUE(loadInto("w_set",
                             oracleArm(root, "      \"oracle_window_s\": 150.0\n"), cfg));
        EXPECT_DOUBLE_EQ(cfg.getNetworkConfig().slip_qc.oracle_window_s, 150.0);
    }
    {   // 0 is legal (it admits no slip), not an error
        GnssConfig cfg;
        ASSERT_TRUE(loadInto("w_zero",
                             oracleArm(root, "      \"oracle_window_s\": 0.0\n"), cfg));
        EXPECT_DOUBLE_EQ(cfg.getNetworkConfig().slip_qc.oracle_window_s, 0.0);
    }
    {   // negative -> warn and clamp to 0; the load still succeeds
        GnssConfig cfg;
        ASSERT_TRUE(loadInto("w_neg",
                             oracleArm(root, "      \"oracle_window_s\": -30.0\n"), cfg));
        EXPECT_TRUE(cfg.getNetworkConfig().slip_qc.oracle_relevel);
        EXPECT_DOUBLE_EQ(cfg.getNetworkConfig().slip_qc.oracle_window_s, 0.0);
    }
}

} // namespace
} // namespace GnssProcessor
