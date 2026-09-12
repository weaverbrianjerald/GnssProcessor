/**
 * @file test_restored_arc_age.cpp
 * @brief The RESTORED-ARC AGE convention (SPEC_event_harness §15.4): a
 *        warm-resume-restored @ref GnssProcessor::LocalCarry reads MATURE for
 *        its whole restored life, and the first post-resume harvest must NOT
 *        re-brand it as newborn.
 *
 * ## The defect this file pins
 *
 * `LocalCarry::birth_ep` / `birth_s` are deliberately NOT snapshotted: a restored
 * entry's true birth predates the snapshot, so the documented contract is that it
 * reads MATURE (refusing it would refuse the whole network for `min_age` epochs
 * after every resume).  Leaving the fields at their 0 default did not deliver that
 * contract, because 0 ALSO means "never stashed" — which is exactly what the
 * phase-leveling harvest's first-stash-wins rule read it as, so ONE post-resume
 * harvest stamped every restored arc with the CURRENT loop epoch.  Measured on a
 * 60-epoch warm-resume leg: 390 of 414 cold-anchor-fallback candidacy skips read
 * `younger than 60 epochs`, i.e. the maturity bar was structurally inert on every
 * warm-resume leg and every age-gated knob was measuring nothing.
 *
 * The fix is a SENTINEL, not a format bump: the restore stamps
 * @ref LocalCarry::kBirthEpRestored / @ref LocalCarry::kBirthSRestored, the
 * first-stash-wins rule tests `!= 0` instead of `> 0`, and every age reader (all
 * of which already tested `> 0`) therefore reads the sentinel as mature.  Nothing
 * is added to the snapshot record or to the warm-resume fingerprint, so the fix
 * applies to snapshots ALREADY on disk — which is also why a format bump would not
 * have been testable on the existing harness seeds.
 *
 * ## What the cases are, and why each one is here
 *
 * 1. **The stamp rule** (@ref GnssProcessor::localCarryStampBirth) — the sentinel
 *    survives a harvest and a re-stash; a genuinely new entry is stamped at
 *    creation; an erased-and-reborn entry is re-stamped.  The regression is
 *    one-sided in a way no run-end counter can see (a re-stamped restored arc
 *    looks exactly like a fresh one), so it is asserted directly.
 * 2. **Every age reader, on the sentinel.**  There are three, and they are listed
 *    explicitly here because "audit the readers" is the part of this card that
 *    rots: the F3 block admission gate and the candidacy maturity bar (both
 *    @ref GnssProcessor::localCarryYoung — one definition, two readers), and the
 *    sim-only slip oracle's window floor (`birth_s`).  Each gets the sentinel and
 *    a stamped entry, so a reader that starts treating −1 as "born at epoch −1"
 *    fails here rather than in a field leg.
 * 3. **The snapshot round trip.**  A real write → read through
 *    @ref GnssProcessor::NetworkStateSnapshotIO, then the exact conversion the
 *    restore site performs (@ref GnssProcessor::makeRestoredLocalCarry), so the
 *    file-format half of the contract — the snapshot carries no birth field, and
 *    the reader must therefore sentinel-stamp — is pinned end to end.
 *
 * Out of scope: the per-epoch harvest LOOP itself (it lives inside `runNetwork`
 * and is certified by the same-build byte-identity A/B and the harness leg pair
 * in SPEC_event_harness §15.4), and the election's own non-age branches (those
 * are tests/test_candidacy_usable.cpp's).
 */

#include <gtest/gtest.h>

#include "filter/NetworkAnchorHygiene.hpp"
#include "filter/NetworkInject.hpp"
#include "filter/NetworkPhaseDatum.hpp"
#include "filter/NetworkRunTypes.hpp"
#include "filter/StateKeySchema.hpp"
#include "logging/LogManager.hpp"
#include "products/NetworkStateSnapshot.hpp"
#include "slip/NetworkSlipOracle.hpp"

#include <filesystem>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

namespace GnssProcessor {
namespace {

constexpr const char* kSat = "G10";
constexpr const char* kSig = "L1C";
/// GPS week 2313 — the harness fixture's frame; 30 s cadence.
constexpr double kW = 2313.0 * 604800.0;
constexpr double kMaxVar = 1e-2;   // the golden's phase_datum_handover_max_var_m2

std::string ambKey() { return StateKeys::makeAmbiguityKey(kSat, kSig); }

/// A carried entry as the warm resume produces it, via the production factory.
LocalCarry restored(double value_abs = 12.5, double var = 1e-6,
                    double last_seen_s = kW + 279150.0) {
    return makeRestoredLocalCarry(value_abs, var, last_seen_s);
}

/// A carried entry as a HARVEST produces it: stamped at @p birth_ep / @p birth_s.
LocalCarry stamped(long long birth_ep, double birth_s,
                   double value_abs = 12.5, double var = 1e-6) {
    LocalCarry lc{value_abs, var, birth_s};
    lc.birth_ep = birth_ep;
    lc.birth_s  = birth_s;
    return lc;
}

// ===========================================================================
// 1: the stamp rule — localCarryStampBirth, the harvest's first-stash-wins test
// ===========================================================================

// THE defect, in one assertion pair.  The harvest builds a fresh LocalCarry and
// asks the stamp rule what its birth is; with the old `> 0` test the restored
// entry's −1 lost to "unstamped" and the arc was re-branded as born NOW.
TEST(RestoredArcAge, TheSentinelSurvivesAHarvestAndARestash) {
    const LocalCarry restored_entry = restored();
    ASSERT_EQ(restored_entry.birth_ep, LocalCarry::kBirthEpRestored);
    ASSERT_DOUBLE_EQ(restored_entry.birth_s, LocalCarry::kBirthSRestored);

    // Harvest #1, at the leg's first epoch after the resume.
    LocalCarry h1{99.0, 2e-6, kW + 279180.0};
    localCarryStampBirth(h1, &restored_entry, /*now_ep=*/1, kW + 279180.0);
    EXPECT_EQ(h1.birth_ep, LocalCarry::kBirthEpRestored)
        << "one post-resume harvest must not re-brand a restored arc as newborn";
    EXPECT_DOUBLE_EQ(h1.birth_s, LocalCarry::kBirthSRestored);

    // Harvest #2, 59 epochs later — still the sentinel, so the arc reads mature
    // for its WHOLE restored life, not just for the first epoch.
    LocalCarry h60{98.0, 2e-6, kW + 280950.0};
    localCarryStampBirth(h60, &h1, /*now_ep=*/60, kW + 280950.0);
    EXPECT_EQ(h60.birth_ep, LocalCarry::kBirthEpRestored);
    EXPECT_DOUBLE_EQ(h60.birth_s, LocalCarry::kBirthSRestored);
}

// The other half of the contract: nothing about a genuinely NEW arc changes.  A
// first stash is stamped at today's epoch, and an ordinary re-stash keeps it.
TEST(RestoredArcAge, ANewEntryIsStampedAtCreationAndKeptAcrossRestashes) {
    LocalCarry born{50.0, 1e-4, kW + 279180.0};
    localCarryStampBirth(born, /*prev=*/nullptr, /*now_ep=*/7, kW + 279180.0);
    EXPECT_EQ(born.birth_ep, 7);
    EXPECT_DOUBLE_EQ(born.birth_s, kW + 279180.0);

    LocalCarry restash{50.1, 9e-5, kW + 279210.0};
    localCarryStampBirth(restash, &born, /*now_ep=*/8, kW + 279210.0);
    EXPECT_EQ(restash.birth_ep, 7) << "first stash wins across a re-stash";
    EXPECT_DOUBLE_EQ(restash.birth_s, kW + 279180.0);
}

// An erase-and-rebirth (a T4 attribution erase, a gap prune, a re-admission)
// RESTARTS the clock — including for an entry that was restored.  It needs no
// clause of its own precisely because the erase DELETES the entry, so the next
// harvest sees no previous entry at all; the case exists so that a future "keep
// the sentinel forever" shortcut cannot pass.
TEST(RestoredArcAge, AnErasedAndRebornEntryIsRestamped) {
    std::map<std::tuple<int, std::string, std::string>, LocalCarry> carry;
    const auto key = std::make_tuple(0, std::string(kSat), std::string(kSig));
    carry[key] = restored();

    ASSERT_EQ(carry.erase(key), 1u);          // the T4 attribution erase
    const auto it = carry.find(key);
    LocalCarry reborn{7.5, 3e-3, kW + 280000.0};
    localCarryStampBirth(reborn, (it == carry.end() ? nullptr : &it->second),
                         /*now_ep=*/42, kW + 280000.0);
    EXPECT_EQ(reborn.birth_ep, 42)
        << "a re-born arc carries its code-level birth error and must read YOUNG";
    EXPECT_DOUBLE_EQ(reborn.birth_s, kW + 280000.0);
    EXPECT_TRUE(localCarryYoung(reborn, /*now_ep=*/60, /*min_age_ep=*/60));
}

// ===========================================================================
// 2a: age reader 1+2 — localCarryYoung (the F3 block gate and the candidacy bar
//     share this ONE definition)
// ===========================================================================

TEST(RestoredArcAge, TheSharedAgeReaderCallsTheSentinelMature) {
    const LocalCarry r = restored();
    // now_ep 1 on a 60-epoch bar: a re-stamped restored arc would read as 0
    // epochs old, which is the inert-bar regime the defect produced.
    EXPECT_FALSE(localCarryYoung(r, /*now_ep=*/1,  /*min_age_ep=*/60));
    EXPECT_FALSE(localCarryYoung(r, /*now_ep=*/60, /*min_age_ep=*/60));
    EXPECT_FALSE(localCarryYoung(r, /*now_ep=*/1,  /*min_age_ep=*/1));

    // An unstamped (0) entry is the same case — never stashed, age unknown.
    LocalCarry never{};
    EXPECT_FALSE(localCarryYoung(never, /*now_ep=*/1000, /*min_age_ep=*/60));
}

TEST(RestoredArcAge, TheSharedAgeReaderStillMeasuresAStampedEntry) {
    const LocalCarry young  = stamped(/*birth_ep=*/980, kW + 279150.0);
    const LocalCarry mature = stamped(/*birth_ep=*/100, kW + 270000.0);

    EXPECT_TRUE (localCarryYoung(young,  /*now_ep=*/1000, /*min_age_ep=*/60));
    EXPECT_FALSE(localCarryYoung(mature, /*now_ep=*/1000, /*min_age_ep=*/60));
    // The bar is a >= test on the age: exactly at the bar is MATURE.
    EXPECT_FALSE(localCarryYoung(stamped(940, kW), /*now_ep=*/1000, 60));
    EXPECT_TRUE (localCarryYoung(stamped(941, kW), /*now_ep=*/1000, 60));
    // 0 disables the test outright.
    EXPECT_FALSE(localCarryYoung(young, /*now_ep=*/1000, /*min_age_ep=*/0));
}

// ===========================================================================
// 2b: age reader 2, END TO END — the cold anchor-fallback candidacy bar
// ===========================================================================

/// Station roster + this epoch's candidate census, exactly what the election
/// reads.  Four stations, all tracking G10/L1C, ranked A > B > C > D by coverage
/// so the election's sweep walks them in that order.
struct Roster {
    std::vector<NetStation> stations;
    std::vector<bool>       present;
    PhaseDatumCandidates    cnd;
};

Roster makeRoster(const std::vector<std::string>& names,
                  const std::vector<int>& coverage) {
    Roster r;
    r.stations.resize(names.size());
    r.present.assign(names.size(), true);
    r.cnd.sat_el.resize(names.size());
    r.cnd.usable_keys.resize(names.size());
    for (std::size_t i = 0; i < names.size(); ++i) {
        r.stations[i].name = names[i];
        r.cnd.cover['G'][i] = coverage[i];
        r.cnd.cand_set['G'].insert(i);
        r.cnd.sat_el[i][kSat] = 45.0;
        r.cnd.usable_keys[i][ambKey()] = StateKeys::makeSatPhaseBiasKey(kSat, kSig);
        r.cnd.union_keys[ambKey()]     = StateKeys::makeSatPhaseBiasKey(kSat, kSig);
    }
    return r;
}

struct Outcome {
    std::string          elected;   ///< this_epoch_anchor for the family ("-" = none)
    NetworkAnchorTallies tally;
};

/// One election with the incumbent retired this epoch (the handover the
/// candidacy pass screens for), against the given carry view.
Outcome elect(Roster& r, int min_age_ep, long long now_ep,
              const std::map<std::tuple<int, std::string, std::string>,
                             LocalCarry>* lc) {
    AnchorHandoverLedger ledger;
    // BBBB (rank 1, the top non-incumbent) and CCCC (rank 2) both have immaculate
    // ledger values, so the ONLY thing that can move the election off BBBB is the
    // maturity bar — and CCCC exists so that a refused BBBB produces a RANK CHANGE
    // rather than a fall-through, which is the observable the bar is there for.
    ledger.value_abs[{1, ambKey()}] = {7.0,  1e-6, 1e-3};
    ledger.value_abs[{2, ambKey()}] = {12.5, 1e-6, 1e-3};

    NetworkInjectState inject;
    InjectAnchorDirective d;
    d.sat = kSat; d.signal = kSig; d.cold = false;
    inject.anchor_now_slip.push_back(std::move(d));

    AnchorCandidacyUsable usable;
    usable.on          = true;
    usable.min_age_ep  = min_age_ep;
    usable.now_ep      = now_ep;
    usable.local_carry = lc;

    AnchorHygiene hyg;
    Outcome o;
    std::map<std::string, std::string> incumbent{{ambKey(), "AAAA"}};
    std::map<std::string, double>      pin_target;
    std::map<std::string, std::string> this_epoch_anchor;
    std::set<std::string>              shared;
    std::vector<AnchorHandoverEvent>   events;
    networkElectPhaseAnchors(/*sticky=*/true, r.stations, r.present, r.cnd, hyg,
                             o.tally, incumbent, pin_target, this_epoch_anchor,
                             shared, &ledger, &events, kMaxVar, nullptr,
                             /*max_var_ratio=*/0.0, &inject, &usable);
    const auto it = this_epoch_anchor.find(ambKey());
    o.elected = it == this_epoch_anchor.end() ? "-" : it->second;
    return o;
}

// The production reading of the defect: on a warm-resume leg shorter than the
// bar, EVERY candidate is restored, so with the re-stamp the pass skips them all
// as `young` and falls through to today's sweep.  With the sentinel the same
// candidate is screened on its ledger value, which is what the bar is for.
TEST(RestoredArcAge, ARestoredCandidateIsNotRefusedByTheMaturityBar) {
    std::map<std::tuple<int, std::string, std::string>, LocalCarry> lc;
    lc[{1, kSat, kSig}] = restored();
    lc[{2, kSat, kSig}] = stamped(/*birth_ep=*/1, kW + 270000.0);

    auto r = makeRoster({"AAAA", "BBBB", "CCCC", "DDDD"}, {9, 8, 7, 6});
    const Outcome o = elect(r, /*min_age_ep=*/60, /*now_ep=*/1, &lc);

    EXPECT_EQ(o.elected, "BBBB") << "the top-ranked candidate keeps the family";
    EXPECT_EQ(o.tally.candidacy_skip_young, 0)
        << "a restored entry has no usable age and must not be counted young";
    EXPECT_EQ(o.tally.candidacy_usable_elected, 1);
    EXPECT_EQ(o.tally.candidacy_rank_changed, 0);
    EXPECT_EQ(o.tally.handover_cold, 0);
}

// The bar must still BITE on a genuinely young arc — the sentinel is not a way
// of switching the screen off.
TEST(RestoredArcAge, AGenuinelyYoungCandidateIsStillRefused) {
    std::map<std::tuple<int, std::string, std::string>, LocalCarry> lc;
    lc[{1, kSat, kSig}] = stamped(/*birth_ep=*/980, kW + 279150.0);   // 20 ep old
    lc[{2, kSat, kSig}] = stamped(/*birth_ep=*/100, kW + 270000.0);   // 900 ep old

    auto r = makeRoster({"AAAA", "BBBB", "CCCC", "DDDD"}, {9, 8, 7, 6});
    const Outcome o = elect(r, /*min_age_ep=*/60, /*now_ep=*/1000, &lc);

    EXPECT_EQ(o.elected, "CCCC") << "the bar must still move the election off a "
                                    "genuinely young top-ranked candidate";
    EXPECT_EQ(o.tally.candidacy_skip_young, 1);
    EXPECT_EQ(o.tally.candidacy_rank_changed, 1);
}

// ===========================================================================
// 2c: age reader 3 — the sim-only slip oracle's `birth_s` window floor
// ===========================================================================

/// One oracle wired to a hand-built carry (same construction as
/// tests/test_slip_oracle.cpp's Harness, trimmed to this card's question).
struct OracleHarness {
    LogManager        log_manager;
    NetworkSlipOracle oracle;
    std::map<std::tuple<int, std::string, std::string>, LocalCarry> carry;

    OracleHarness() {
        oracle.carry = &carry;
        oracle.utils = std::make_unique<PppUtils>(log_manager);
        oracle.window_s = 90.0;          // the default three-epoch latency bound
    }
    void put(const LocalCarry& lc) {
        carry[std::make_tuple(0, std::string(kSat), std::string(kSig))] = lc;
    }
    void addTruth(double tow_s, int delta) {
        oracle.truth[std::make_tuple(0, std::string(kSat), std::string(kSig))]
            .push_back({kW + tow_s, delta});
    }
    bool run(double tow_s, std::string& detail, OracleOutcome& oc) {
        return networkSlipOracleHandle(oracle, 0, kSat, kSig, kW + tow_s,
                                       RelevelEpochState::kCauseMw, detail, oc);
    }
};

// The oracle's window is ( max(last_applied, birth_s, now − window_s), now ].
// A restored arc's TRUE birth is un-snapshotted, so the documented behaviour is
// "no floor — fall back to the latency bound".  Before the sentinel the first
// post-resume harvest stamped birth_s with the RESUME second, which floored the
// window there and reported the arc as `bound_birth`; the sentinel keeps losing
// the max for the arc's whole restored life.
TEST(RestoredArcAge, TheOracleWindowIsNotFlooredByARestoredBirth) {
    OracleHarness h;
    LocalCarry lc = restored(/*value_abs=*/100.0, /*var=*/1e-5,
                             /*last_seen_s=*/kW + 302820.0);
    h.put(lc);
    h.addTruth(302820.0, 3);            // inside the 90 s latency window

    std::string detail; OracleOutcome oc{};
    ASSERT_TRUE(h.run(302850.0, detail, oc));
    EXPECT_EQ(oc, OracleOutcome::Repaired);
    EXPECT_EQ(h.oracle.bound_window, 1) << "the latency bound must be the binding "
                                           "term for a restored arc";
    EXPECT_EQ(h.oracle.bound_birth, 0);
}

// …and the floor still works when the birth IS known, so the sentinel has not
// disarmed the "already absorbed" guard.
TEST(RestoredArcAge, TheOracleWindowIsStillFlooredByARealBirth) {
    OracleHarness h;
    h.put(stamped(/*birth_ep=*/5, /*birth_s=*/kW + 302820.0,
                  /*value_abs=*/100.0, /*var=*/1e-5));
    h.addTruth(302790.0, -12);          // inside the window, but PRE-birth

    std::string detail; OracleOutcome oc{};
    ASSERT_TRUE(h.run(302850.0, detail, oc));
    EXPECT_EQ(oc, OracleOutcome::Kept);
    EXPECT_EQ(h.oracle.bound_birth, 1);
    EXPECT_EQ(h.oracle.applied, 0);
}

// ===========================================================================
// 3: the snapshot round trip — the file carries no birth field, so the READER
//    is where the convention has to be applied
// ===========================================================================

TEST(RestoredArcAge, ASnapshotRoundTripYieldsSentinelAgesOnEveryRestoredEntry) {
    const auto path = std::filesystem::temp_directory_path()
                    / "gnss_restored_arc_age_snapshot.txt";
    std::error_code ec;
    std::filesystem::remove(path, ec);

    NetworkStateSnapshot snap;
    snap.snap_week = 2313;
    snap.snap_tow  = 279150.0;
    snap.n_emitted = 665;
    snap.config_fingerprint = 0xc5b5df834d185ac8ULL;
    // Two L rows (one with the optional var_first field, one without) and a Z row
    // — the three shapes the restore loop actually meets on the harness seeds.
    snap.local_carry[{0, "G10", "L1C"}] = {-13.371942948457217, 0.016046053590845624,
                                           kW + 279150.0, 0.0};
    snap.local_carry[{3, "E03", "L5Q"}] = {-1.4035410604255958, 0.050056665667344,
                                           kW + 279150.0, 0.25};
    snap.zwd_carry[0] = {0.12345, 1e-5, kW + 279150.0, 0.0};

    std::string err;
    ASSERT_TRUE(NetworkStateSnapshotIO::write(path, snap, &err)) << err;
    NetworkStateSnapshot back;
    ASSERT_TRUE(NetworkStateSnapshotIO::read(path, back, &err)) << err;
    ASSERT_EQ(back.local_carry.size(), 2u);
    ASSERT_EQ(back.zwd_carry.size(), 1u);

    // The conversion the restore site performs, on every row that came back.
    for (const auto& [key, l] : back.local_carry) {
        const LocalCarry lc = makeRestoredLocalCarry(l.value_abs, l.var,
                                                     l.last_seen_s, l.var_first);
        EXPECT_EQ(lc.birth_ep, LocalCarry::kBirthEpRestored) << std::get<1>(key);
        EXPECT_DOUBLE_EQ(lc.birth_s, LocalCarry::kBirthSRestored);
        EXPECT_FALSE(localCarryYoung(lc, /*now_ep=*/1, /*min_age_ep=*/60));
        // Everything the row DOES carry must survive untouched.
        EXPECT_DOUBLE_EQ(lc.value_abs, l.value_abs);
        EXPECT_DOUBLE_EQ(lc.var, l.var);
        EXPECT_DOUBLE_EQ(lc.last_seen_s, l.last_seen_s);
        EXPECT_DOUBLE_EQ(lc.var_first, l.var_first);
    }
    for (const auto& [idx, l] : back.zwd_carry) {
        const LocalCarry lc = makeRestoredLocalCarry(l.value_abs, l.var, l.last_seen_s);
        EXPECT_EQ(lc.birth_ep, LocalCarry::kBirthEpRestored) << idx;
        EXPECT_DOUBLE_EQ(lc.birth_s, LocalCarry::kBirthSRestored);
    }
    // The record itself carries NO birth field — that is why the reader has to
    // apply the convention.  A future format bump that adds one must delete this
    // assertion deliberately, not by accident.
    EXPECT_DOUBLE_EQ(back.local_carry.at({0, "G10", "L1C"}).var_first, 0.0);

    std::filesystem::remove(path, ec);
}

}  // namespace
}  // namespace GnssProcessor
