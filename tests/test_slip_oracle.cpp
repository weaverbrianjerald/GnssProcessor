/**
 * @file test_slip_oracle.cpp
 * @brief SIM-ONLY ORACLE slip response (`network.cycle_slip.oracle_relevel`,
 *        include/slip/NetworkSlipOracle.hpp) — the level-preserving re-level
 *        CEILING.
 *
 * ## What is under test, and what is deliberately NOT
 *
 * The oracle is a pure decision-plus-actuation layer over ONE arc: given the
 * fixture's truth jumps, the arc's carried level and this epoch's time, it
 * either shifts @c LocalCarry::value_abs by the true sum d*lambda and tells the
 * caller to skip every freeing action, or it declines.  Every case here drives
 * that layer directly on a fabricated carry, so the four detector sites that
 * CALL it (slip/NetworkSlipQc.cpp) are out of scope — their own population logic
 * has coverage in tests/test_network_slip_qc.cpp, and the hook's contract
 * ("true ⇒ the caller frees nothing") is a caller-side property a same-build
 * byte-identity A/B certifies, not a unit test.
 *
 * ## The cases that matter most are the ones bounding what it TOUCHES
 *
 * A truth-fed repair is the strongest actuation in the slip stack: it writes a
 * converged-weight carried level directly, with no data supporting it.  So the
 * assertions are deliberately two-sided — @c var unchanged, @c last_seen_s
 * unchanged, the sibling band untouched, and the arc NOT freed — because the
 * failure that would silently corrupt the whole census is an oracle that moves
 * something else as well.  @c ExactRepairLeavesEverythingButValueAbsAlone and
 * the false-alarm KEEP half are those bounds.
 *
 * ## Window semantics are the second half of the correctness argument
 *
 * The oracle must apply exactly the jumps the carried level does NOT already
 * contain.  The window is
 * `( max(last_applied, carry.birth_s, now − window_s), now ]` — lower bound
 * EXCLUSIVE, upper INCLUSIVE — and all three lower terms have a case here
 * because each one, got wrong, produces a ceiling no run-end counter can
 * contradict:
 *
 * - the DETECTOR-LATENCY term (`now − window_s`) is what makes a late detector
 *   repairable.  ⚠ `carry.last_seen_s` is NOT the lower bound, though it reads
 *   like the natural "already absorbed" mark: it is the PREVIOUS epoch's stash,
 *   and a zero-q carried level is not re-set by the intervening solve, so a
 *   detector firing at t+1…t+3 on a slip at t is still looking at an un-absorbed
 *   jump.  @c LateDetectorRepairsTheSlipItFiredOnEvenAfterARestash is that
 *   regression, pinned against the same case with the window closed;
 * - `carry.birth_s` is the HARD floor (@c SlipBeforeTheCarrysBirthIsAlreadyAbsorbed):
 *   a T4 erase-and-rebirth re-creates the level from a solve that ALREADY
 *   contains the earlier jump, so re-applying it would inject the very error the
 *   card exists to remove;
 * - `last_applied` makes two detectors on one arc pay the jump ONCE
 *   (@c ASecondDetectorInTheSameWindowCannotDoubleApply).
 */

#include <gtest/gtest.h>

#include "common/GnssTypes.hpp"
#include "common/PppUtils.hpp"
#include "config/GnssConfig.hpp"
#include "logging/LogManager.hpp"
#include "slip/NetworkSlipOracle.hpp"

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
/// GPS week 2313 — the campaign fixture's frame; 30 s cadence, tow ~302 8xx.
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

/// Forward slashes only — the JSON body below does not escape backslashes.
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
    std::map<std::tuple<int, std::string, std::string>, LocalCarry> carry;

    Harness() {
        oracle.carry = &carry;
        oracle.utils = std::make_unique<PppUtils>(log_manager);
        addCarry(kSig1);
        addCarry(kSig2);
    }

    /// A carried level last stashed at @p last_seen_s (default: one 30 s epoch
    /// before the reference epoch, the normal warm posture) and CREATED at
    /// @p birth_s — the window's hard floor.  The default birth is a long-
    /// established arc (~47 min before every case's epoch), so a case that does
    /// not name it is exercising the latency bound alone.
    void addCarry(const std::string& sig, double last_seen_s = kW + 302820.0,
                  double value_abs = 100.0, double var = 1.0e-5,
                  double birth_s = kW + 300000.0) {
        LocalCarry lc{};
        lc.value_abs   = value_abs;
        lc.var         = var;
        lc.last_seen_s = last_seen_s;
        lc.birth_s     = birth_s;
        carry[std::make_tuple(0, std::string(kSat), sig)] = lc;
    }
    void addTruth(const std::string& sig, double tow_s, int delta) {
        oracle.truth[std::make_tuple(0, std::string(kSat), sig)]
            .push_back({kW + tow_s, delta});
    }
    LocalCarry& c(const std::string& sig) {
        return carry.at(std::make_tuple(0, std::string(kSat), sig));
    }
    bool run(const std::string& sig, double tow_s, std::string& detail,
             OracleOutcome& oc) {
        return networkSlipOracleHandle(oracle, 0, kSat, sig, kW + tow_s, detail, oc);
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

// The fixture writes one row per APPLIED jump, so one arc can carry several —
// the parser must keep them all, in ascending time, keyed per (sat, signal).
TEST(SlipOracleCsv, ParsesMultipleSlipsPerArcInAscendingTime) {
    TempTree t("csv_multi");
    const auto f = t.writeStation("S01000SIM",
        "2313,304860.000,G11,L5Q,-88,1,13.90\n"
        "2313,302820.000,G19,L1C,48,1,11.50\n"     // deliberately OUT of order
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

// The header is pinned by NAME, not position: a fixture that grows or reorders a
// column must fail loudly here rather than silently read the wrong field as
// delta_cycles, which would produce a plausible-looking but wrong ceiling.
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

// Both edges of the LATENCY window in one case, because they are one decision.
// A slip stamped AT this epoch is un-absorbed (the detector is firing on it right
// now) and must be applied; the lower edge is EXCLUSIVE, so a slip stamped
// exactly window_s back is out.  The carry's last stash (302820) sits in the
// middle of the window on purpose: it must NOT move either edge.
TEST(SlipOracleWindow, LatencyWindowIsExclusiveBelowAndInclusiveAtNow) {
    Harness h;
    ASSERT_DOUBLE_EQ(h.oracle.window_s, 90.0);   // three 30 s epochs
    h.addTruth(kSig1, 302760.0, 11);   // == now - window_s -> exclusive, out
    h.addTruth(kSig1, 302790.0, 3);    // inside, and BEFORE carry.last_seen_s
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
    // The binding-term census: the latency edge is what held here.  It is the only
    // instrument that separates "the window is too narrow" from "nothing jumped",
    // which have opposite fixes and identical repaired/false-alarm counts.
    EXPECT_EQ(h.oracle.bound_window, 1);
    EXPECT_EQ(h.oracle.bound_birth, 0);
    EXPECT_EQ(h.oracle.bound_applied, 0);
}

// WARNING - THE REGRESSION.  Detectors fire LATE - MW must fill its window, IF-TD
// needs a triple difference - so the normal population is a slip at t caught at
// t+1..t+3, by which time the harvest has re-stashed the carry at t+1.  The stash
// does NOT absorb the jump: the level is zero-q, so one epoch of phase barely
// moves the stiff prior and the integer shift still restores it.  Bounding the
// window at carry.last_seen_s instead of at the detector latency turned this
// whole population into "false alarms" - 701 kept against 88 repaired on a
// 60-epoch smoke - and the ceiling that arm reported was a badly biased lower
// bound.  The window_s = 0 half pins the old behaviour so a re-introduction fails
// here rather than in six hours of run time.
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
        // IS the pre-fix rule, evaluated by the post-fix code.
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
        // WARNING - 0 is NOT "same-epoch only".  The lower bound is exclusive, so
        // window 0 collapses to (now, now] and admits nothing, including this
        // epoch's own slip.  Pinned because a reader reaching for a pessimistic
        // rung would otherwise arm a silently dead one.
        Harness h;
        h.oracle.window_s = 0.0;
        h.addTruth(kSig1, 302850.0, 5);                     // stamped AT this epoch
        std::string detail; OracleOutcome oc{};
        ASSERT_TRUE(h.run(kSig1, 302850.0, detail, oc));
        EXPECT_EQ(oc, OracleOutcome::Kept);
        EXPECT_EQ(h.oracle.applied, 0);
    }
}

// The HARD floor.  A T4 attribution erase deletes the carry entry; the next
// harvest re-CREATES it from a solve whose level already contains every earlier
// jump.  So a truth slip stamped before birth_s is absorbed by construction and
// re-applying it would inject exactly the error this card exists to remove - even
// though it sits well inside the latency window.  Two-sided: the same window's
// POST-birth slip must still be repaired, so the floor TRIMS the window rather
// than closing it.
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
        EXPECT_EQ(h.oracle.multi, 0);       // ONE jump in the trimmed window
        EXPECT_NE(detail.find("n=1"), std::string::npos) << detail;
    }
}

// Past the latency the detectors are capable of, an actuation is a genuine FALSE
// ALARM: nothing in this stack looks 5 epochs back, so a jump that old is either
// already absorbed or was never this arc's.  Keeping the level is the ceiling's
// answer, and the un-repaired population must land in fa_kept rather than quietly
// widening the window.
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

// Two detectors, one arc, one jump - on DIFFERENT epochs, both inside the 90 s
// window of the slip.  Only last_applied separates them (the latency bound still
// admits the slip at the second firing), so this is the case that would DOUBLE the
// shift if the ledger were dropped - and the run-end applied count would read
// better for it.
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

// Several jumps inside one window SUM - the level is a single scalar and the
// repair must land the arc where truth put it, not where the first jump did.
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

// `last_applied` advances so a SECOND detector firing on the same arc in the
// same (or the next) epoch cannot re-apply a jump the first already paid.
// Without it the two-detector case (MW and IF-TD both flagging one arc) would
// double the shift — and the run-end `applied` count would look BETTER for it.
TEST(SlipOracleWindow, LastAppliedAdvancesSoASecondActuationCannotDoubleApply) {
    Harness h;
    h.addTruth(kSig1, 302850.0, 5);
    std::string d1, d2; OracleOutcome o1{}, o2{};

    ASSERT_TRUE(h.run(kSig1, 302850.0, d1, o1));
    ASSERT_EQ(o1, OracleOutcome::Repaired);
    const double after_first = h.c(kSig1).value_abs;

    // Same epoch, second detector: the window is now empty -> false alarm KEEP.
    ASSERT_TRUE(h.run(kSig1, 302850.0, d2, o2));
    EXPECT_EQ(o2, OracleOutcome::Kept);
    EXPECT_DOUBLE_EQ(h.c(kSig1).value_abs, after_first);
    EXPECT_EQ(h.oracle.applied, 1);
    EXPECT_EQ(h.oracle.fa_kept, 1);
    EXPECT_EQ(h.oracle.seen, 2);
}

// ===========================================================================
// Actuation bounds — what it must NOT touch
// ===========================================================================

// The ceiling's premise is that a PERFECT repair costs no information, so the
// level keeps its converged weight: `var` is unchanged by construction.  The
// stash stamp is unchanged too (the harvest owns it), the sibling band is
// untouched (a slip on L1C is a slip on L1C), and — the load-bearing one — the
// arc is NOT freed: the hook returning true is the caller's instruction to skip
// every freeing action, and a repair that ALSO freed would measure the current
// response, not the ceiling.
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
    // The exact ceiling: the shift is an EXACT integer number of cycles.
    const double cyc = (h.c(kSig1).value_abs - 100.0) / h.lambda(kSig1);
    EXPECT_NEAR(cyc, std::round(cyc), 1e-9);
}

// An arc with no carried level has nothing to preserve, so today's response is
// already the right one and the hook must DECLINE (return false) rather than
// swallow the actuation.  Counted separately: a large `no_carry` inside a run
// with many actuations would say the vehicle is not what the card assumed.
TEST(SlipOracleActuate, NoCarryEntryFallsThroughToTodaysResponse) {
    Harness h;
    h.carry.clear();
    h.addTruth(kSig1, 302850.0, 5);
    std::string detail; OracleOutcome oc{};
    EXPECT_FALSE(h.run(kSig1, 302850.0, detail, oc));
    EXPECT_EQ(oc, OracleOutcome::NotHandled);
    EXPECT_EQ(h.oracle.no_carry, 1);
    EXPECT_EQ(h.oracle.applied, 0);
    EXPECT_EQ(h.oracle.fa_kept, 0);      // ⚠ NOT a false alarm — a different fact
    EXPECT_EQ(h.oracle.seen, 1);
}

// An unknown signal has no wavelength, so the shift cannot be formed.  It falls
// through to today's response and is counted in its OWN bucket: rolling it into
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
    EXPECT_EQ(h.oracle.seen, h.oracle.applied + h.oracle.fa_kept + h.oracle.fa_freed
                             + h.oracle.no_carry + h.oracle.no_lambda);
    // …and the binding census's own: every actuation that got past the carry
    // lookup is attributed to exactly one lower-bound term, including this one
    // (the wavelength check is downstream of the window scan).
    EXPECT_EQ(h.oracle.seen - h.oracle.no_carry,
              h.oracle.bound_window + h.oracle.bound_birth + h.oracle.bound_applied);
}

// ===========================================================================
// The false-alarm policy — the two halves of the ceiling
// ===========================================================================

// `keep` IS the ceiling: an oracle knows the detector was wrong and leaves the
// converged level exactly where it was, so the arm measures repair quality with
// the false-alarm cost REMOVED.  `free` keeps that cost, which is what separates
// "the repair is the lever" from "the false alarms are the lever".
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
// Noise (the precision ladder's knob)
// ===========================================================================

// The ladder answers "how good does an integer estimate have to be?", so its
// draws must be REPRODUCIBLE — a re-executed run, or two detectors hitting one
// arc in one epoch, must see the same perturbation.  Determinism is the whole
// contract; the distribution is not under test.
TEST(SlipOracleNoise, IsDeterministicPerKeyAndVariesAcrossKeys) {
    const double a1 = networkSlipOracleNoise(0.05, 0, kSat, kSig1, kW + 302850.0);
    const double a2 = networkSlipOracleNoise(0.05, 0, kSat, kSig1, kW + 302850.0);
    EXPECT_DOUBLE_EQ(a1, a2);                                    // same key ⇒ same draw
    EXPECT_NE(a1, networkSlipOracleNoise(0.05, 1, kSat, kSig1, kW + 302850.0));
    EXPECT_NE(a1, networkSlipOracleNoise(0.05, 0, kSat, kSig2, kW + 302850.0));
    EXPECT_NE(a1, networkSlipOracleNoise(0.05, 0, "G23", kSig1, kW + 302850.0));
    EXPECT_NE(a1, networkSlipOracleNoise(0.05, 0, kSat, kSig1, kW + 302880.0));
    // sigma 0 is the EXACT ceiling and must be exactly 0, not a tiny draw.
    EXPECT_DOUBLE_EQ(networkSlipOracleNoise(0.0, 0, kSat, kSig1, kW + 302850.0), 0.0);
}

// …and the applied shift carries it, so a laddered arm is a different but
// reproducible number rather than a different RUN.
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
        // sum_abs_cyc records the TRUTH magnitude, not the perturbed one — the
        // counter must describe the population, not the injection.
        EXPECT_NEAR(h.oracle.sum_abs_cyc, 5.0, 1e-12);
        if (pass == 0) first = h.c(kSig1).value_abs;
        else EXPECT_DOUBLE_EQ(h.c(kSig1).value_abs, first);
    }
}

// ===========================================================================
// Load-time contract — the field-inertness mechanism
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

// A sim fixture writes one truth file PER station, so a station without one
// means the root is mis-pointed (or the roster is not this fixture's).  Silently
// running a partial repair would produce a ceiling that is neither the current
// response nor the perfect one — the one outcome that cannot be interpreted.
// Hard error.
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
    // indexes by STATION INDEX (the local_carry key's first element).
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
    EXPECT_DOUBLE_EQ(oracle->noise_cyc, 0.0);
    EXPECT_DOUBLE_EQ(oracle->window_s, 90.0);   // the knob reaches the run object
}

// ===========================================================================
// Interlocks (loader-side)
// ===========================================================================

// The oracle writes LocalCarry::value_abs, so it is INERT without the leveling
// carry and structurally empty under carry_invariant.  Both DISARM (not
// warn-only): a ceiling measured on a vehicle the mechanism cannot actuate would
// read as a falsification of level preservation, which is a wrong scientific
// verdict rather than a partial one.
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
        // structurally empty there.  Spelled the way a carry_invariant lineage
        // actually is — phase_leveling OFF and the eq34 chain datum on (the
        // loader REFUSES carry_invariant without it) — because the disarm must
        // hold on the real posture, not a synthetic both-vehicles-on one.
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

// The loader's hard errors.  A root that does not exist IS the field-inertness
// mechanism (a field lineage has no truth file); an unrecognised false-alarm
// policy is a typo that would silently fall back to `keep` and mislabel a whole
// arm.  Both surface as a REFUSED load — GnssConfig::loadFromJson catches and
// returns false, and Application treats that as fatal.
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
    // `free` is the other legal policy and must survive.
    {
        GnssConfig cfg;
        ASSERT_TRUE(loadInto("ok_free",
                             oracleArm(root, "      \"oracle_false_alarm\": \"free\"\n"),
                             cfg));
        EXPECT_EQ(cfg.getNetworkConfig().slip_qc.oracle_false_alarm, "free");
    }
    // Default OFF: the knob absent leaves it off with no root required at all —
    // the byte-identity precondition (standing invariant #6).
    {
        GnssConfig cfg;
        ASSERT_TRUE(loadInto("off", "    \"phase_leveling\": true,\n", cfg));
        const auto& sq = cfg.getNetworkConfig().slip_qc;
        EXPECT_FALSE(sq.oracle_relevel);
        EXPECT_TRUE(sq.oracle_truth_root.empty());
        EXPECT_EQ(sq.oracle_false_alarm, "keep");
        EXPECT_DOUBLE_EQ(sq.oracle_noise_cyc, 0.0);
        EXPECT_DOUBLE_EQ(sq.oracle_window_s, 90.0);
    }
}

// The detector-latency window is the knob the repaired/false-alarm SPLIT is most
// sensitive to, so its default is pinned here rather than left to the struct: 90 s
// is three 30 s epochs, the latency the truth census calls DETECTED.  Unlike the
// noise sigma it is CLAMPED rather than refused - 0 (same-epoch only) is a
// legitimate pessimistic rung of the ladder, so a negative value has exactly one
// sensible reading and refusing the load would cost an arm for a typo.
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
    {   // 0 is the same-epoch-only rung, not an error
        GnssConfig cfg;
        ASSERT_TRUE(loadInto("w_zero",
                             oracleArm(root, "      \"oracle_window_s\": 0.0\n"), cfg));
        EXPECT_DOUBLE_EQ(cfg.getNetworkConfig().slip_qc.oracle_window_s, 0.0);
    }
    {   // negative -> warn and CLAMP, load still succeeds
        GnssConfig cfg;
        ASSERT_TRUE(loadInto("w_neg",
                             oracleArm(root, "      \"oracle_window_s\": -30.0\n"), cfg));
        EXPECT_TRUE(cfg.getNetworkConfig().slip_qc.oracle_relevel);
        EXPECT_DOUBLE_EQ(cfg.getNetworkConfig().slip_qc.oracle_window_s, 0.0);
    }
}

} // namespace
} // namespace GnssProcessor
