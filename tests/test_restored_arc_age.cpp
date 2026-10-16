/**
 * @file test_restored_arc_age.cpp
 * @brief Restored-arc age convention (SPEC_event_harness §15.4): a
 *        warm-resume-restored @ref GnssProcessor::LocalCarry reads mature for its
 *        whole restored life, and the first post-resume harvest does not re-stamp
 *        it as newborn.
 *
 * ## Behavior under test
 *
 * `LocalCarry::birth_ep` / `birth_s` are not snapshotted: a restored entry's true
 * birth predates the snapshot, so the contract is that it reads mature (refusing it
 * would refuse the whole network for `min_age` epochs after every resume).  The
 * value 0 cannot express that, because 0 also means "never stashed", and the
 * phase-leveling harvest's first-stash-wins rule treats an unstamped predecessor as
 * absent.  A single post-resume harvest would then stamp every restored arc with
 * the current loop epoch, and on a warm-resume leg shorter than the bar every
 * age-gated knob would see only young arcs.
 *
 * The restore therefore stamps a sentinel, @ref LocalCarry::kBirthEpRestored /
 * @ref LocalCarry::kBirthSRestored (both −1).  The first-stash-wins rule tests
 * `!= 0` rather than `> 0`, so the sentinel is kept, and every age reader tests
 * `> 0`, so it reads the sentinel as mature.  Nothing is added to the snapshot
 * record or to the warm-resume fingerprint, so the convention also applies to
 * snapshots already on disk.
 *
 * ## Cases
 *
 * 1. The stamp rule (@ref GnssProcessor::localCarryStampBirth): the sentinel
 *    survives a harvest and a re-stash; a new entry is stamped at creation; an
 *    erased-and-reborn entry is re-stamped.  A re-stamped restored arc looks the
 *    same as a fresh one to every run-end counter, so the rule is asserted directly.
 * 2. Every age reader, on the sentinel.  There are three, listed here so that a new
 *    reader is added to the list: the F3 block admission gate and the candidacy
 *    maturity bar (both through @ref GnssProcessor::localCarryYoung, one definition
 *    with two readers), and the sim-only slip oracle's window floor (`birth_s`).
 *    Each is given the sentinel and a stamped entry, so a reader that treats −1 as
 *    "born at epoch −1" fails here.
 * 3. The snapshot round trip: a real write and read through
 *    @ref GnssProcessor::NetworkStateSnapshotIO, then the conversion the restore
 *    site performs (@ref GnssProcessor::makeRestoredLocalCarry).  This pins the
 *    file-format half of the contract end to end: the snapshot carries no birth
 *    field, so the reader applies the sentinel.
 * 4. The re-level stamp (@ref GnssProcessor::localCarryStampRelevel), the rule of
 *    @ref LocalCarry::relevel_s: set when the entry is created and when the arc is
 *    freed, kept on every other re-stash.  It needs no restore sentinel: a restored
 *    entry's 0 reads as unstamped, and an ordinary re-stash keeps it (suite
 *    @c LocalCarryRelevelStamp).
 *
 * Out of scope: the per-epoch harvest loop itself (inside `runNetwork`, certified by
 * the same-build byte-identity A/B and the harness leg pair in SPEC_event_harness
 * §15.4), and the election's non-age branches (covered by
 * tests/test_candidacy_usable.cpp).
 */

#include <gtest/gtest.h>

#include "network/solve/AnchorHygiene.hpp"
#include "network/carry/Inject.hpp"
#include "network/solve/PhaseDatum.hpp"
#include "network/driver/RunTypes.hpp"
#include "filter/StateKeySchema.hpp"
#include "logging/LogManager.hpp"
#include "products/NetworkStateSnapshot.hpp"
#include "network/slip/SlipOracle.hpp"

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
/// GPS week 2313 (the harness fixture's frame); 30 s cadence.
constexpr double kW = 2313.0 * 604800.0;
constexpr double kMaxVar = 1e-2;   // the golden's phase_datum_handover_max_var_m2

std::string ambKey() { return StateKeys::makeAmbiguityKey(kSat, kSig); }

/// A carried entry as the warm resume produces it, via the production factory.
LocalCarry restored(double value_abs = 12.5, double var = 1e-6,
                    double last_seen_s = kW + 279150.0) {
    return makeRestoredLocalCarry(value_abs, var, last_seen_s);
}

/// A carried entry as a harvest produces it: stamped at @p birth_ep / @p birth_s.
LocalCarry stamped(long long birth_ep, double birth_s,
                   double value_abs = 12.5, double var = 1e-6) {
    LocalCarry lc{value_abs, var, birth_s};
    lc.birth_ep = birth_ep;
    lc.birth_s  = birth_s;
    return lc;
}

// ===========================================================================
// 1: the stamp rule (localCarryStampBirth, the harvest's first-stash-wins test)
// ===========================================================================

// The harvest builds a fresh LocalCarry and asks the stamp rule for its birth.  A
// `> 0` test would treat the restored entry's −1 as unstamped and stamp the arc
// with the current epoch; the `!= 0` test keeps the sentinel.
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

    // Harvest #2, 59 epochs later: still the sentinel, so the arc reads mature for
    // its whole restored life, not only at the first epoch.
    LocalCarry h60{98.0, 2e-6, kW + 280950.0};
    localCarryStampBirth(h60, &h1, /*now_ep=*/60, kW + 280950.0);
    EXPECT_EQ(h60.birth_ep, LocalCarry::kBirthEpRestored);
    EXPECT_DOUBLE_EQ(h60.birth_s, LocalCarry::kBirthSRestored);
}

// The other half of the contract: a new arc is unaffected.  A first stash is
// stamped at the current epoch, and an ordinary re-stash keeps that stamp.
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
// restarts the age clock, also for an entry that was restored.  The rule needs no
// clause for this: the erase deletes the entry, so the next harvest sees no
// previous entry.  The case fails any change that keeps the sentinel across an
// erase.
TEST(RestoredArcAge, AnErasedAndRebornEntryIsRestamped) {
    net::ArcTable carry;
    const auto key = std::make_tuple(0, std::string(kSat), std::string(kSig));
    net::putArcIn(carry, key, &net::ArcState::amb) = restored();

    ASSERT_EQ(net::dropArc(carry, key, &net::ArcState::amb), 1u);          // the T4 attribution erase
    const auto it = net::getArc(carry, key, &net::ArcState::amb);
    LocalCarry reborn{7.5, 3e-3, kW + 280000.0};
    localCarryStampBirth(reborn, (it == nullptr ? nullptr : it),
                         /*now_ep=*/42, kW + 280000.0);
    EXPECT_EQ(reborn.birth_ep, 42)
        << "a re-born arc carries its code-level birth error and must read YOUNG";
    EXPECT_DOUBLE_EQ(reborn.birth_s, kW + 280000.0);
    EXPECT_TRUE(localCarryYoung(reborn, /*now_ep=*/60, /*min_age_ep=*/60));
}

// ===========================================================================
// 2a: age readers 1 and 2, localCarryYoung (the F3 block gate and the candidacy
//     bar share this one definition)
// ===========================================================================

TEST(RestoredArcAge, TheSharedAgeReaderCallsTheSentinelMature) {
    const LocalCarry r = restored();
    // now_ep 1 on a 60-epoch bar: a re-stamped restored arc would read as 0 epochs
    // old and be refused.
    EXPECT_FALSE(localCarryYoung(r, /*now_ep=*/1,  /*min_age_ep=*/60));
    EXPECT_FALSE(localCarryYoung(r, /*now_ep=*/60, /*min_age_ep=*/60));
    EXPECT_FALSE(localCarryYoung(r, /*now_ep=*/1,  /*min_age_ep=*/1));

    // An unstamped (0) entry is treated the same way: never stashed, age unknown.
    LocalCarry never{};
    EXPECT_FALSE(localCarryYoung(never, /*now_ep=*/1000, /*min_age_ep=*/60));
}

TEST(RestoredArcAge, TheSharedAgeReaderStillMeasuresAStampedEntry) {
    const LocalCarry young  = stamped(/*birth_ep=*/980, kW + 279150.0);
    const LocalCarry mature = stamped(/*birth_ep=*/100, kW + 270000.0);

    EXPECT_TRUE (localCarryYoung(young,  /*now_ep=*/1000, /*min_age_ep=*/60));
    EXPECT_FALSE(localCarryYoung(mature, /*now_ep=*/1000, /*min_age_ep=*/60));
    // The bar is a >= test on the age: an age equal to the bar is mature.
    EXPECT_FALSE(localCarryYoung(stamped(940, kW), /*now_ep=*/1000, 60));
    EXPECT_TRUE (localCarryYoung(stamped(941, kW), /*now_ep=*/1000, 60));
    // 0 disables the test outright.
    EXPECT_FALSE(localCarryYoung(young, /*now_ep=*/1000, /*min_age_ep=*/0));
}

// ===========================================================================
// 2b: age reader 2 end to end (the cold anchor-fallback candidacy bar)
// ===========================================================================

/// Station roster and this epoch's candidate census, the inputs the election
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
              const net::ArcTable* lc) {
    AnchorHandoverLedger ledger;
    // BBBB (rank 1, the top non-incumbent) and CCCC (rank 2) both have clean ledger
    // values, so only the maturity bar can move the election off BBBB.  CCCC makes a
    // refused BBBB produce a rank change rather than a fall-through, and the rank
    // change is the observable the assertions read.
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

// On a warm-resume leg shorter than the bar every candidate is restored; a
// re-stamp would make the pass skip them all as `young` and fall through to the
// current epoch's sweep.  With the sentinel the restored candidate is screened on
// its ledger value.
TEST(RestoredArcAge, ARestoredCandidateIsNotRefusedByTheMaturityBar) {
    net::ArcTable lc;
    net::putArcIn(lc, {1, kSat, kSig}, &net::ArcState::amb) = restored();
    net::putArcIn(lc, {2, kSat, kSig}, &net::ArcState::amb) = stamped(/*birth_ep=*/1, kW + 270000.0);

    auto r = makeRoster({"AAAA", "BBBB", "CCCC", "DDDD"}, {9, 8, 7, 6});
    const Outcome o = elect(r, /*min_age_ep=*/60, /*now_ep=*/1, &lc);

    EXPECT_EQ(o.elected, "BBBB") << "the top-ranked candidate keeps the family";
    EXPECT_EQ(o.tally.candidacy_skip_young, 0)
        << "a restored entry has no usable age and must not be counted young";
    EXPECT_EQ(o.tally.candidacy_usable_elected, 1);
    EXPECT_EQ(o.tally.candidacy_rank_changed, 0);
    EXPECT_EQ(o.tally.handover_cold, 0);
}

// The bar still refuses a young arc: the sentinel does not switch the screen off.
TEST(RestoredArcAge, AGenuinelyYoungCandidateIsStillRefused) {
    net::ArcTable lc;
    net::putArcIn(lc, {1, kSat, kSig}, &net::ArcState::amb) = stamped(/*birth_ep=*/980, kW + 279150.0);   // 20 ep old
    net::putArcIn(lc, {2, kSat, kSig}, &net::ArcState::amb) = stamped(/*birth_ep=*/100, kW + 270000.0);   // 900 ep old

    auto r = makeRoster({"AAAA", "BBBB", "CCCC", "DDDD"}, {9, 8, 7, 6});
    const Outcome o = elect(r, /*min_age_ep=*/60, /*now_ep=*/1000, &lc);

    EXPECT_EQ(o.elected, "CCCC") << "the bar must still move the election off a "
                                    "genuinely young top-ranked candidate";
    EXPECT_EQ(o.tally.candidacy_skip_young, 1);
    EXPECT_EQ(o.tally.candidacy_rank_changed, 1);
}

// ===========================================================================
// 2c: age reader 3, the sim-only slip oracle's `birth_s` window floor
// ===========================================================================

/// One oracle wired to a hand-built carry (same construction as
/// tests/network/test_slip_oracle.cpp's Harness, reduced to what these cases need).
struct OracleHarness {
    LogManager        log_manager;
    NetworkSlipOracle oracle;
    net::ArcTable carry;

    OracleHarness() {
        oracle.carry = &carry;
        oracle.utils = std::make_unique<PppUtils>(log_manager);
        oracle.window_s = 90.0;          // the default three-epoch latency bound
    }
    void put(const LocalCarry& lc) {
        net::putArcIn(carry, std::make_tuple(0, std::string(kSat), std::string(kSig)), &net::ArcState::amb) = lc;
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
// A restored arc's true birth is not snapshotted, so the documented behaviour is no
// birth floor: the latency bound applies.  A re-stamp would set birth_s to the
// resume second, floor the window there and count the arc as `bound_birth`; the
// sentinel (−1) never wins the max, for the arc's whole restored life.
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

// The floor still applies when the birth is known, so the sentinel leaves the
// "already absorbed" guard in place.
TEST(RestoredArcAge, TheOracleWindowIsStillFlooredByARealBirth) {
    OracleHarness h;
    h.put(stamped(/*birth_ep=*/5, /*birth_s=*/kW + 302820.0,
                  /*value_abs=*/100.0, /*var=*/1e-5));
    h.addTruth(302790.0, -12);          // inside the window, but before the birth

    std::string detail; OracleOutcome oc{};
    ASSERT_TRUE(h.run(302850.0, detail, oc));
    EXPECT_EQ(oc, OracleOutcome::Kept);
    EXPECT_EQ(h.oracle.bound_birth, 1);
    EXPECT_EQ(h.oracle.applied, 0);
}

// ===========================================================================
// 3: the snapshot round trip (the file carries no birth field, so the reader
//    applies the convention)
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
    // Two L rows (one with the optional var_first field, one without) and a Z row:
    // the three row shapes the restore loop meets on the harness seeds.
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
        // Every field the row carries survives unchanged.
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
    // The record carries no birth field, which is why the reader applies the
    // convention.  A format change that adds one has to remove this assertion
    // explicitly.
    EXPECT_DOUBLE_EQ(back.local_carry.at({0, "G10", "L1C"}).var_first, 0.0);

    std::filesystem::remove(path, ec);
}

// ===========================================================================
// 4: the re-level stamp (localCarryStampRelevel): LocalCarry::relevel_s moves on
//    two events only
// ===========================================================================

// A created entry: a first stash, or the re-stash after a T4 attribution erase or gap
// prune deleted the entry (the erase needs no clause of its own because it deletes, so
// the next harvest creates).  Its level came from one epoch of code.
TEST(LocalCarryRelevelStamp, StampIsSetWhenTheEntryIsCreated) {
    EXPECT_DOUBLE_EQ(localCarryStampRelevel(/*prev=*/0.0, /*created=*/true,
                                            /*freed=*/false, /*now_s=*/1'234'567.0),
                     1'234'567.0);
}

// A freed arc: it was in this epoch's latched relevel.free_ambs set, so this harvest
// re-levelled it from one epoch of code although the entry already existed.
TEST(LocalCarryRelevelStamp, StampIsSetWhenTheArcWasFreedThisEpoch) {
    EXPECT_DOUBLE_EQ(localCarryStampRelevel(/*prev=*/1'000'000.0, /*created=*/false,
                                            /*freed=*/true, /*now_s=*/1'234'567.0),
                     1'234'567.0);
}

// Every other re-stash preserves the stamp; otherwise the age would reset every epoch
// and no arc could mature out of the inflation.
TEST(LocalCarryRelevelStamp, StampIsPreservedOnAnOrdinaryRestash) {
    EXPECT_DOUBLE_EQ(localCarryStampRelevel(/*prev=*/1'000'000.0, /*created=*/false,
                                            /*freed=*/false, /*now_s=*/1'234'567.0),
                     1'000'000.0);
}

TEST(LocalCarryRelevelStamp, StampIsSetWhenBothEventsCoincide) {
    EXPECT_DOUBLE_EQ(localCarryStampRelevel(0.0, /*created=*/true, /*freed=*/true, 500.0),
                     500.0);
}

}  // namespace
}  // namespace GnssProcessor
