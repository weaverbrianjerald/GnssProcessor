/**
 * @file test_stream_flag_roundtrip.cpp
 * @brief Round trips of the two integer flags of a phase-bias record (signal_integer and
 *        wide_lane_integer) from each rule that writes them, through the correction-stream
 *        file, into the correction store a user reads.
 *
 * A user decides whether an ambiguity may be fixed from the signal integer flag it finds in its
 * correction store, so a flag must arrive there with the value the network meant.  Each rule
 * that sets or changes a flag on the network side runs here on a small epoch: the estimator's
 * default (every CDMA bias flagged, GLONASS not), the settled-flags stamp of the ring fix (held
 * families only, the cascade's third signal only outside the log mode), the per-signal wire
 * fill (a copy carries its source's flags until the stamp rewrites them), the phase-family
 * member copy, and the stream's continuity guard (a lowered flag held for its hold).  The
 * epoch is then written by the real writer, parsed by the real reader and replayed into a fresh
 * store, as a file consumer loads the stream, and the store's entry is compared with what the
 * rule wrote.  The decoder of a third-party RTCM stream is the other way a flag reaches the
 * store; its frame is built bit by bit and pushed as the real-time ingest pushes it.
 *
 * Every expected flag follows from the code of the rule under test, not from a document.
 */

// The support header comes first so that its own includes are proven sufficient here too.
#include "../support/SolutionEpochBuilder.hpp"

#include <gtest/gtest.h>
#include <Eigen/Dense>

#include "common/GnssIdentity.hpp"
#include "network/ar/RingFix.hpp"
#include "network/assembly/PhaseFamily.hpp"
#include "network/assembly/SignalRoster.hpp"
#include "network/emit/EmitStore.hpp"
#include "network/emit/SolPrep.hpp"
#include "network/solve/Estimator.hpp"
#include "products/NetworkProductEmitter.hpp"
#include "products/NetworkProductIO.hpp"
#include "products/SsrCorrectionStore.hpp"
#include "rtcm/RtcmParser.hpp"
#include "rtcm/RtcmTypes.hpp"

#include "../support/TestTempDir.hpp"

#include <algorithm>
#include <cstdint>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace GnssProcessor {
namespace {

using gnss_test::SolutionEpochBuilder;

// ---- The file and the store ---------------------------------------------------------------

/// Writes one epoch through the real writer (no counters), parses the file and replays the
/// record into @p store through the replay emitter the file's contract names, as a file
/// consumer loads the stream.  Returns the parsed record.
NetworkProductRecord roundTripInto(const NetworkSolutionEpoch& sol, SsrCorrectionStore& store,
                                   const std::string& file) {
    const std::string path = gnss_test::tempPath(file).string();
    {
        NetworkProductWriter w(path);
        EXPECT_TRUE(w.good());
        w.write(sol, false, {}, {}, nullptr);
    }
    bool ok = false;
    SbasisContract contract;
    const std::vector<NetworkProductRecord> recs = NetworkProductReader::read(path, &contract, &ok);
    EXPECT_TRUE(ok);
    if (recs.size() != 1u) {
        ADD_FAILURE() << "expected one record in " << file << ", read " << recs.size();
        return NetworkProductRecord{};
    }
    NetworkProductEmitter emitter = NetworkProductReader::makeReplayEmitter(contract);
    NetworkProductReader::replay(recs[0], store, emitter);
    return recs[0];
}

/// The parsed P record of (@p sat, @p sig), or null when the file has none.
const NetworkPhaseBias* filed(const NetworkProductRecord& rec, const std::string& sat, const std::string& sig) {
    for (const NetworkSatSolution& s : rec.epoch.sats) {
        if (s.sat != sat) continue;
        const auto it = s.phase_bias.find(sig);
        return it == s.phase_bias.end() ? nullptr : &it->second;
    }
    return nullptr;
}

/// Expects (@p sat, @p sig) to carry the two flags both in the parsed file and in the store.
void expectArrived(const NetworkProductRecord& rec, const SsrCorrectionStore& store, const std::string& sat,
                   const std::string& sig, bool signal_integer, int wide_lane_integer) {
    SCOPED_TRACE(sat + " " + sig);
    const NetworkPhaseBias* f = filed(rec, sat, sig);
    ASSERT_NE(f, nullptr) << "no P record in the file";
    EXPECT_EQ(f->signal_integer, signal_integer) << "file";
    EXPECT_EQ(f->wide_lane_integer, wide_lane_integer) << "file";
    const auto e = store.getPhaseBias(sat, sig);
    ASSERT_TRUE(e.has_value()) << "no entry in the store";
    EXPECT_EQ(e->signal_integer, signal_integer) << "store";
    EXPECT_EQ(e->wide_lane_integer, wide_lane_integer) << "store";
}

/// The Galileo phase families: L1X shares L1C, L5X shares L5Q (member first, identity second).
PhaseFamilyTable galileoFamilies() {
    return {{*SignalId::fromObsCode(System::Galileo, "L1X"), *SignalId::fromObsCode(System::Galileo, "L1C")},
            {*SignalId::fromObsCode(System::Galileo, "L5X"), *SignalId::fromObsCode(System::Galileo, "L5Q")}};
}

// ---- The continuity guard's stream ----------------------------------------------------------

/// Emission @p k of a five-satellite Galileo stream on the phase signals @p p1 / @p p2 and the
/// code signals of the same names (C for L), every flag up.  E05's @p p1 phase bias moves by
/// 1 mm per emission from emission 5 to emission 12 and then stays; the other four hold the
/// family's median step at zero.  One millimetre on E1 moves the narrow-lane level by about
/// 0.021 cycle, so the level passes the 0.15 cycle threshold at emission 12 (0.166 cycle) and
/// not before (0.145 cycle at emission 11), while each step stays under the 5 cm value bar.
NetworkSolutionEpoch guardEmission(int k, const std::string& p1, const std::string& p2) {
    const double e05 = k < 5 ? 0.0 : 0.001 * (std::min(k, 12) - 4);
    NetworkSolutionEpoch sol;
    sol.epoch_time = GnssTime{2313, 30.0 * k};
    sol.iod_ssr = 1;
    sol.provider_id = 1;
    sol.solution_id = 1;
    for (const char* id : {"E01", "E02", "E03", "E04", "E05"}) {
        NetworkSatSolution ns;
        ns.sat = id;
        ns.has_clock = true;
        ns.clock_c0_m = 10.0;
        const bool moving = std::string(id) == "E05";
        ns.phase_bias[p1] = NetworkPhaseBias{1.0 + (moving ? e05 : 0.0), true, 1};
        ns.phase_bias[p2] = NetworkPhaseBias{2.0, true, 1};
        ns.code_bias_m["C" + p1.substr(1)] = 0.3;
        ns.code_bias_m["C" + p2.substr(1)] = 0.5;
        sol.sats.push_back(ns);
    }
    return sol;
}

constexpr int kGuardEmissions = 20;
constexpr int kGuardTrip      = 12;   ///< The emission at which the guard trips.
constexpr int kGuardHold      = 5;    ///< Emissions the flags stay lowered, the trip included.

/// The live store's entry of each watched "sat|signal" key at one emission.
using LiveEntries = std::map<std::string, SsrPhaseBiasEntry>;

/**
 * Runs the guarded stream as the network's emit seam does: the member records (when @p members
 * is not empty), the push into the live store, the guard's lowered flags mirrored into the
 * solution, then the file written from that solution with the live emitter's counters.
 * Returns the live store's watched entries at each emission.
 */
std::vector<LiveEntries> runGuardedStream(NetworkProductEmitter& em, const PhaseFamilyTable& members,
                                          const std::string& p1, const std::string& p2,
                                          const std::vector<std::string>& watched, const std::string& path) {
    std::vector<LiveEntries> live;
    SsrCorrectionStore store;
    NetworkProductWriter w(path);
    EXPECT_TRUE(w.good());
    for (int k = 0; k < kGuardEmissions; ++k) {
        NetworkSolutionEpoch sol = guardEmission(k, p1, p2);
        if (!members.empty()) networkPhaseFamilyMemberRecords(members, sol);
        em.emitToStore(sol, store);
        if (em.continuityGuardArmed()) em.applyContinuityGuardFlags(sol);
        w.write(sol, false, {}, {}, &em);
        LiveEntries row;
        for (const std::string& key : watched) {
            const std::size_t bar = key.find('|');
            const auto e = store.getPhaseBias(key.substr(0, bar), key.substr(bar + 1));
            if (e.has_value()) row[key] = *e;
            else ADD_FAILURE() << "live store has no entry for " << key << " at emission " << k;
        }
        live.push_back(row);
    }
    return live;
}

/**
 * Replays the guarded stream's file into a fresh store and expects every watched key to carry,
 * at every emission, the live store's flags and counter; the keys in @p lowered_keys carry a
 * lowered signal integer flag exactly from the trip for the hold, both in the file and in the
 * store, and every watched key keeps wide_lane_integer 1.
 */
void expectReplayMatchesLive(const std::string& path, const std::vector<LiveEntries>& live,
                             const std::vector<std::string>& lowered_keys) {
    bool ok = false;
    SbasisContract contract;
    const std::vector<NetworkProductRecord> recs = NetworkProductReader::read(path, &contract, &ok);
    EXPECT_TRUE(ok);
    ASSERT_EQ(recs.size(), live.size());
    NetworkProductEmitter replay_emitter = NetworkProductReader::makeReplayEmitter(contract);
    SsrCorrectionStore replay_store;
    for (std::size_t k = 0; k < recs.size(); ++k) {
        SCOPED_TRACE("emission " + std::to_string(k));
        NetworkProductReader::replay(recs[k], replay_store, replay_emitter);
        const bool in_hold = static_cast<int>(k) >= kGuardTrip && static_cast<int>(k) < kGuardTrip + kGuardHold;
        for (const auto& [key, want] : live[k]) {
            SCOPED_TRACE(key);
            const std::size_t bar = key.find('|');
            const std::string sat = key.substr(0, bar), sig = key.substr(bar + 1);
            const bool lowered_here =
                in_hold && std::find(lowered_keys.begin(), lowered_keys.end(), key) != lowered_keys.end();
            EXPECT_EQ(want.signal_integer, !lowered_here) << "live store";
            EXPECT_EQ(want.wide_lane_integer, 1) << "live store";
            const NetworkPhaseBias* f = filed(recs[k], sat, sig);
            ASSERT_NE(f, nullptr) << "no P record in the file";
            EXPECT_EQ(f->signal_integer, !lowered_here) << "file";
            EXPECT_EQ(f->wide_lane_integer, 1) << "file";
            const auto got = replay_store.getPhaseBias(sat, sig);
            ASSERT_TRUE(got.has_value()) << "no entry in the replayed store";
            EXPECT_EQ(got->signal_integer, want.signal_integer) << "replayed store";
            EXPECT_EQ(got->wide_lane_integer, want.wide_lane_integer) << "replayed store";
            EXPECT_EQ(got->discontinuity_counter, want.discontinuity_counter) << "replayed store";
        }
    }
}

// ---- An RTCM frame built bit by bit -----------------------------------------------------------

/// MSB-first bit writer for hand-built RTCM payloads.
class BitWriter {
public:
    void put(int n, std::uint64_t value) {
        for (int i = n - 1; i >= 0; --i) {
            const bool bit = ((value >> i) & 0x1ull) != 0;
            if ((bit_count_ & 7u) == 0) bytes_.push_back(0u);
            if (bit) bytes_.back() |= static_cast<std::uint8_t>(1u << (7 - (bit_count_ & 7u)));
            ++bit_count_;
        }
    }
    void putSigned(int n, std::int64_t value) {
        const std::uint64_t mask = (n == 64) ? ~0ull : ((1ull << n) - 1ull);
        put(n, static_cast<std::uint64_t>(value) & mask);
    }
    std::vector<std::uint8_t> finish() {
        while (bit_count_ % 8) put(1, 0);
        return bytes_;
    }

private:
    std::vector<std::uint8_t> bytes_;
    std::size_t               bit_count_{0};
};

/// 0xD3 | length(10) | payload | CRC (zero: the parser assumes the framer checked it).
std::vector<std::uint8_t> buildFrame(const std::vector<std::uint8_t>& payload) {
    std::vector<std::uint8_t> frame;
    const auto pl = static_cast<std::uint16_t>(payload.size());
    frame.push_back(0xD3);
    frame.push_back(static_cast<std::uint8_t>((pl >> 8) & 0x03u));
    frame.push_back(static_cast<std::uint8_t>(pl & 0xFFu));
    frame.insert(frame.end(), payload.begin(), payload.end());
    frame.push_back(0);
    frame.push_back(0);
    frame.push_back(0);
    return frame;
}

}  // namespace

// ---- The file itself: every value of the two flags ---------------------------------------------

// Every pair of the two flags (signal integer 0 or 1, wide-lane indicator 0 to 3, its 2-bit
// range) written on a record reaches the file and the store unchanged.  Fails if the writer
// drops or reorders the two P-record fields, the reader parses them from the wrong position,
// or the emitter's store record stops copying either flag.
TEST(StreamFlagRoundTrip, EveryFlagPairReachesTheStoreUnchanged) {
    for (const bool si : {false, true}) {
        for (int wl = 0; wl <= 3; ++wl) {
            SCOPED_TRACE("signal_integer " + std::to_string(si) + ", wide_lane_integer " + std::to_string(wl));
            SolutionEpochBuilder b;
            b.signalInteger("G05", "L1C", si).wideLaneInteger("G05", "L1C", wl);
            SsrCorrectionStore store;
            const NetworkProductRecord rec =
                roundTripInto(b.epoch(), store, "pair_" + std::to_string(si) + std::to_string(wl) + ".txt");
            expectArrived(rec, store, "G05", "L1C", si, wl);
            // A neighbour record of the same satellite keeps its own flags.
            expectArrived(rec, store, "G05", "L2W", true, 1);
        }
    }
}

// ---- The estimator's default -----------------------------------------------------------------

// The estimator flags every estimated CDMA phase bias (GPS, Galileo, BeiDou, QZSS) as integer
// and never a GLONASS one, and never sets the wide-lane indicator; the file and the store carry
// exactly that.  Fails if the estimator's rule changes (a GLONASS bias flagged, a CDMA bias left
// unflagged, a wide-lane indicator written) or the round trip loses the flag.
TEST(StreamFlagRoundTrip, TheEstimatorDefaultFlagsEveryCdmaBiasAndNoGlonassBias) {
    NetworkEstimator est;
    const std::vector<std::pair<std::string, std::string>> cols = {
        {"G05", "L1C"}, {"G05", "L2W"}, {"E11", "L1C"}, {"C08", "L2I"}, {"J02", "L1C"}, {"R05", "L1C"}};
    std::vector<Eigen::Index> idx;
    for (const auto& [sat, sig] : cols) idx.push_back(est.registerPhaseBias(sat, sig));
    Eigen::VectorXd x(est.numShared());
    x.setZero();
    for (std::size_t i = 0; i < idx.size(); ++i) x(idx[i]) = 0.1 * static_cast<double>(i + 1);
    const NetworkSolutionEpoch sol = est.toSolutionEpoch(x, GnssTime{2313, 259230.5}, 7, 42, 3);

    SsrCorrectionStore store;
    const NetworkProductRecord rec = roundTripInto(sol, store, "default.txt");
    for (const auto& [sat, sig] : cols) expectArrived(rec, store, sat, sig, sat[0] != 'R', 0);
}

// ---- The settled-flags stamp -------------------------------------------------------------------

// The stamp writes signal_integer = held family and settled, wide_lane_integer = held family,
// and the record of no held family 0 / 0, overriding the flags any earlier writer left; the
// store carries the stamp's values and nothing after the stamp raises a flag it lowered.  The
// held family here is GPS L1C / L2W: G05 settled on both, G12 settled on L1C only, G20 with no
// settled entry this epoch; Galileo is held by no family, so E11's earlier flags (1 / 2 on
// L1C) are lowered to 0 / 0.  Fails if the stamp's rule changes or any later writer (the
// emitter's store record, the file) raises a lowered flag.
TEST(StreamFlagRoundTrip, TheSettledStampArrivesAsStamped) {
    NetworkRingFix rf;
    rf.pairs.insert({"G", "L1C", "L2W"});
    rf.settled[{"G05", "L1C"}] = true;
    rf.settled[{"G05", "L2W"}] = true;
    rf.settled[{"G12", "L1C"}] = true;
    rf.settled[{"G12", "L2W"}] = false;
    SolutionEpochBuilder b;
    // G20 as the estimator leaves it: flagged, no wide-lane indicator.
    b.clock("G20", 1.5).phaseBias("G20", "L1C", 0.02).signalInteger("G20", "L1C", true)
        .phaseBias("G20", "L2W", 0.03).signalInteger("G20", "L2W", true);
    NetworkSolutionEpoch sol = b.epoch();
    ringFixStampSettledFlags(sol, rf);

    SsrCorrectionStore store;
    const NetworkProductRecord rec = roundTripInto(sol, store, "stamp.txt");
    expectArrived(rec, store, "G05", "L1C", true, 1);
    expectArrived(rec, store, "G05", "L2W", true, 1);
    expectArrived(rec, store, "G12", "L1C", true, 1);
    expectArrived(rec, store, "G12", "L2W", false, 1);    // held family, not settled
    expectArrived(rec, store, "G20", "L1C", false, 1);    // no settled entry: not settled
    expectArrived(rec, store, "G20", "L2W", false, 1);
    expectArrived(rec, store, "E11", "L1C", false, 0);    // no held family: the earlier 1 / 2 lowered
    expectArrived(rec, store, "E11", "L5Q", false, 0);
}

// A cascade's third signal is a held family only while the cascades hold: outside the log mode
// a settled third signal arrives as 1 / 1; in the log mode (the cascades decide and trace,
// never hold) the same record arrives as 0 / 0.  Fails if the stamp's cascade rule changes in
// either mode.
TEST(StreamFlagRoundTrip, ACascadeThirdSignalIsFlaggedOnlyOutsideTheLogMode) {
    for (const bool log_mode : {false, true}) {
        SCOPED_TRACE(log_mode ? "log mode" : "cascades hold");
        NetworkRingFix rf;
        rf.pairs.insert({"G", "L1C", "L2W"});
        RingFixCascade c;
        c.letter = 'G';
        c.ck = "G";
        c.s1 = "L1C";
        c.s2 = "L2W";
        c.s3 = "L5Q";
        c.ref = "L2W";
        c.name = "G:L1C/L2W>L5Q";
        rf.cascades.push_back(c);
        rf.cascade_log = log_mode;
        rf.settled[{"G05", "L1C"}] = true;
        rf.settled[{"G05", "L2W"}] = true;
        rf.settled[{"G05", "L5Q"}] = true;
        SolutionEpochBuilder b;
        b.phaseBias("G05", "L5Q", 0.071).signalInteger("G05", "L5Q", true);
        NetworkSolutionEpoch sol = b.epoch();
        ringFixStampSettledFlags(sol, rf);

        SsrCorrectionStore store;
        const NetworkProductRecord rec =
            roundTripInto(sol, store, std::string("cascade_") + (log_mode ? "log" : "hold") + ".txt");
        expectArrived(rec, store, "G05", "L5Q", !log_mode, log_mode ? 0 : 1);
        expectArrived(rec, store, "G05", "L1C", true, 1);   // the pair itself is held in both modes
    }
}

// ---- The per-signal wire fill and the stamp after it --------------------------------------------

// The wire fill copies a whole record onto a modelled signal that has none, flags included:
// with the settled flags off, GPS L1W filled from L1C arrives with L1C's estimator flags
// (1 / 0).  With the stamp after the fill, as the solution's preparation orders them, the
// filled L1W belongs to no held family and arrives as 0 / 0 while L1C carries its own settled
// flag.  Fails if the fill stops copying the flags, or the stamp stops overwriting a filled
// record with the flags of its own signal.
TEST(StreamFlagRoundTrip, AFilledRecordCarriesItsSourceFlagsUntilTheStampRewritesThem) {
    SignalRoster rs;
    rs.by_station.resize(1);
    rs.by_station[0]["GPS"]['1'].modelled_phase = "L1W";
    for (const bool stamp : {false, true}) {
        SCOPED_TRACE(stamp ? "settled flags on" : "settled flags off");
        SolutionEpochBuilder b;
        b.wideLaneInteger("G05", "L1C", 0).wideLaneInteger("G05", "L2W", 0);   // the estimator's flags
        NetworkSolutionEpoch sol = b.epoch();
        const NetworkWireFillCounts f = networkWireFillModelledSignals(sol, rs);
        ASSERT_GE(f.phase, 1);
        ASSERT_EQ(sol.sats.front().phase_bias.count("L1W"), 1u);
        if (stamp) {
            NetworkRingFix rf;
            rf.pairs.insert({"G", "L1C", "L2W"});
            rf.settled[{"G05", "L1C"}] = true;
            rf.settled[{"G05", "L2W"}] = true;
            ringFixStampSettledFlags(sol, rf);
        }
        SsrCorrectionStore store;
        const NetworkProductRecord rec =
            roundTripInto(sol, store, std::string("fill_") + (stamp ? "stamp" : "plain") + ".txt");
        if (stamp) {
            expectArrived(rec, store, "G05", "L1W", false, 0);
            expectArrived(rec, store, "G05", "L1C", true, 1);
        } else {
            expectArrived(rec, store, "G05", "L1W", true, 0);
            expectArrived(rec, store, "G05", "L1C", true, 0);
        }
    }
}

// ---- The phase-family member copy ---------------------------------------------------------------

// A phase-family member repeats its identity's record, both flags included, in the solution
// (before the push) and again in the emitter's store record, so the live store and the replayed
// file agree: with the Galileo family L1C / L5Q held and E11 settled on L1C only, the member
// L1X arrives as 1 / 1 and L5X as 0 / 1.  The stamp runs before the member copy, as in the
// network; a member is never a held family's own signal.  Fails if either copy stops carrying
// the identity's flags.
TEST(StreamFlagRoundTrip, AFamilyMemberArrivesWithItsIdentitysFlags) {
    NetworkRingFix rf;
    rf.pairs.insert({"E", "L1C", "L5Q"});
    rf.settled[{"E11", "L1C"}] = true;
    rf.settled[{"E11", "L5Q"}] = false;
    SolutionEpochBuilder b;
    NetworkSolutionEpoch sol = b.epoch();
    ringFixStampSettledFlags(sol, rf);
    const PhaseFamilyTable families = galileoFamilies();
    EXPECT_EQ(networkPhaseFamilyMemberRecords(families, sol), 2u);

    NetworkProductEmitter emitter;
    emitter.setPhaseFamilyMembers({{{'E', "L1X"}, "L1C"}, {{'E', "L5X"}, "L5Q"}});
    SsrCorrectionStore live;
    emitter.emitToStore(sol, live);

    SsrCorrectionStore store;
    const NetworkProductRecord rec = roundTripInto(sol, store, "member.txt");
    expectArrived(rec, store, "E11", "L1C", true, 1);
    expectArrived(rec, store, "E11", "L5Q", false, 1);
    expectArrived(rec, store, "E11", "L1X", true, 1);
    expectArrived(rec, store, "E11", "L5X", false, 1);
    for (const char* sig : {"L1X", "L5X"}) {
        SCOPED_TRACE(std::string("live store ") + sig);
        const auto a = live.getPhaseBias("E11", sig);
        const auto r = store.getPhaseBias("E11", sig);
        ASSERT_TRUE(a.has_value());
        ASSERT_TRUE(r.has_value());
        EXPECT_EQ(a->signal_integer, r->signal_integer);
        EXPECT_EQ(a->wide_lane_integer, r->wide_lane_integer);
    }
}

// ---- The continuity guard -----------------------------------------------------------------------

// A flag the continuity guard lowers reaches the store as false.  The guard trips on E05 at
// emission 12 and holds both family flags lowered for 5 emissions; the emit seam mirrors the
// lowered flags into the solution before the file is written, so the file says 0 for exactly
// those emissions and a replay of the file (whose emitter has no guard) puts the same false
// flag, with the same counter, into the store at every emission.  The guard leaves the
// wide-lane indicator at 1.  Fails if the mirror into the solution is dropped (the file and the
// replayed store would say 1 during the hold), if the store record stops lowering a held key,
// or if the guard starts changing the wide-lane indicator.
TEST(StreamFlagRoundTrip, AFlagTheContinuityGuardLowersReachesTheStoreAsFalse) {
    NetworkProductEmitter em(0.05);
    em.setUnsettledSteps({"E:L1X/L5X"});
    em.setContinuityGuard(0.15, kGuardHold, 4);
    const std::vector<std::string> watched = {"E05|L1X", "E05|L5X", "E04|L1X", "E04|L5X"};
    const std::string path = gnss_test::tempPath("guard.txt").string();
    const std::vector<LiveEntries> live = runGuardedStream(em, {}, "L1X", "L5X", watched, path);
    // The guard tripped once, so the hold below is not vacuous.
    ASSERT_EQ(em.continuityGuardTrips().count("E:L1X/L5X"), 1u);
    EXPECT_EQ(em.continuityGuardTrips().at("E:L1X/L5X"), 1);
    expectReplayMatchesLive(path, live, {"E05|L1X", "E05|L5X"});
}

// The guard's lowering reaches a phase-family member too: with the family L1C / L5Q guarded
// and L1X / L5X its members, the emitter copies the lowered identity flag onto the member's
// store record and marks the member lowered, so the file and the replayed store say 0 for the
// members during the hold as well.  Fails if the emitter's member copy stops following a
// lowered identity, or stops passing the member to the mirror into the solution.
TEST(StreamFlagRoundTrip, AGuardLoweringReachesAFamilyMemberThroughTheFile) {
    NetworkProductEmitter em(0.05);
    em.setUnsettledSteps({"E:L1C/L5Q"});
    em.setContinuityGuard(0.15, kGuardHold, 4);
    em.setPhaseFamilyMembers({{{'E', "L1X"}, "L1C"}, {{'E', "L5X"}, "L5Q"}});
    const std::vector<std::string> lowered = {"E05|L1C", "E05|L5Q", "E05|L1X", "E05|L5X"};
    std::vector<std::string> watched = lowered;
    watched.push_back("E04|L1X");
    const std::string path = gnss_test::tempPath("guard_member.txt").string();
    const std::vector<LiveEntries> live = runGuardedStream(em, galileoFamilies(), "L1C", "L5Q", watched, path);
    ASSERT_EQ(em.continuityGuardTrips().count("E:L1C/L5Q"), 1u);
    EXPECT_EQ(em.continuityGuardTrips().at("E:L1C/L5Q"), 1);
    expectReplayMatchesLive(path, live, lowered);
}

// ---- The RTCM decoder ---------------------------------------------------------------------------

// A third-party stream's flags reach the store through the decoder and the push the real-time
// ingest makes (each decoded record pushed with the message header): an IGS-SSR GPS phase-bias
// message (4076, IM 26) with G05 L1C at 0 / 3 and L2W at 1 / 0 and G07 L1C at 1 / 2 arrives
// with those flags.  Fails if the decoder reads the signal integer bit or the 2-bit wide-lane
// field from the wrong position or the push alters an entry.
TEST(StreamFlagRoundTrip, RtcmDecodedFlagsReachTheStore) {
    struct Sig { int id; bool si; int wl; int disc; int bias_raw; };
    struct Sat { int id; std::vector<Sig> sigs; };
    // SSR signal ids: 0 is GPS L1C, 11 is GPS L2W.
    const std::vector<Sat> sats = {{5, {{0, false, 3, 1, 1200}, {11, true, 0, 2, -3400}}},
                                   {7, {{0, true, 2, 0, 560}}}};
    BitWriter pw;
    pw.put(12, 4076);     // message number
    pw.put(3, 1);         // IGS-SSR version
    pw.put(8, 26);        // IM: GPS phase bias
    pw.put(20, 3600);     // epoch (GPS time of week, s)
    pw.put(4, 0);         // update interval
    pw.put(1, 0);         // multi-message indicator
    pw.put(4, 5);         // IOD-SSR
    pw.put(16, 1234);     // provider id
    pw.put(4, 2);         // solution id
    pw.put(1, 1);         // dispersive bias consistency indicator
    pw.put(1, 0);         // Melbourne-Wubbena consistency indicator
    pw.put(6, sats.size());
    for (const Sat& s : sats) {
        pw.put(6, s.id);
        pw.put(5, s.sigs.size());
        pw.put(9, 0);           // yaw
        pw.putSigned(8, 0);     // yaw rate
        for (const Sig& g : s.sigs) {
            pw.put(5, g.id);
            pw.put(1, g.si ? 1 : 0);
            pw.put(2, g.wl);
            pw.put(4, g.disc);
            pw.putSigned(20, g.bias_raw);
        }
    }
    RtcmParser parser;
    ASSERT_TRUE(parser.parseFrame(buildFrame(pw.finish())));
    ASSERT_EQ(parser.getSsrPhaseBiases().size(), 2u);
    SsrCorrectionStore store;
    for (const SsrPhaseBias& r : parser.getSsrPhaseBiases()) store.pushPhaseBias(r, parser.getLastSsrHeader());

    const auto expectEntry = [&](const std::string& sat, const std::string& sig, bool si, int wl, int disc) {
        SCOPED_TRACE(sat + " " + sig);
        const auto e = store.getPhaseBias(sat, sig);
        ASSERT_TRUE(e.has_value());
        EXPECT_EQ(e->signal_integer, si);
        EXPECT_EQ(e->wide_lane_integer, wl);
        EXPECT_EQ(e->discontinuity_counter, disc);
    };
    expectEntry("G05", "L1C", false, 3, 1);
    expectEntry("G05", "L2W", true, 0, 2);
    expectEntry("G07", "L1C", true, 2, 0);
}

}  // namespace GnssProcessor
