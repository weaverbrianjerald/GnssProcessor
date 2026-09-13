/**
 * @file test_resume_signal_canon_restore.cpp
 * @brief Contract tests for the RS-8 CANONICAL-SIGNAL continuity card
 *        (docs/SPEC_event_harness.md §21.16): the snapshot's format-forward
 *        @c CN records (one per (station roster index, constellation, band,
 *        C|L) naming the locked RINEX code), the loader's side of the write
 *        knob (@c network.state_snapshot_signal_capture) and the read knob
 *        (@c network.resume_signal_canon_restore), and @c PppModel's
 *        export / restore pair.
 *
 * The defect this pins: @c PppModel::decideConstellationCanonicals picks each
 * station's per-(band, obs-type) canonical signal from that station's FIRST
 * processed epoch's satellite counts and LOCKS it for the run.  The latch was in
 * no snapshot, so a warm resume re-decided it from the resume epoch's counts —
 * GAMB00PYF flipped L2X→L2W, CPVG00CPV L2W→L2X — which changed the L2X tracker
 * set, left the eq34 L2X component without its datum receiver, and stepped six
 * phase-bias families ~25 m at the seam.
 *
 * What is locked here:
 *   (a) the rows round-trip through the snapshot TEXT under the EXISTING header
 *       (format-forward, not a version bump), preserving per-station order;
 *   (b) a malformed @c CN row is a HARD parse failure — the @c RC / @c SD
 *       precedent — including an obs-type that is neither 'C' nor 'L', which
 *       would install an entry no lookup can ever match and so read as a cold
 *       latch for that band;
 *   (c) a snapshot with no @c CN rows restores nothing (an old file keeps
 *       today's re-decide behaviour) and the write knob OFF emits none of them;
 *   (d) @c PppModel::exportCanonicalSignals / @c restoreCanonicalSignals
 *       round-trip, and — the whole point of the card — a RESTORED latch is not
 *       re-decided by the next epoch's tally even when that tally would pick a
 *       different signal, while the same model WITHOUT the restore does flip;
 *   (e) the loader's defaults, its @c resume_frame_restore prerequisite, and the
 *       INERT-not-fatal posture with no snapshot path.
 *
 * The seam mechanics themselves (the L2X tracker set, the eq34 datum receiver
 * and the raw phase-bias level actually continuing across a warm resume) are
 * graded on the harness legs, not here.
 */
#include <gtest/gtest.h>

#include "config/GnssConfig.hpp"
#include "logging/LogManager.hpp"
#include "model/PppModel.hpp"
#include "products/NetworkStateSnapshot.hpp"
#include "products/ProductParser.hpp"
#include "rinex/RinexParser.hpp"

#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace fs = std::filesystem;
using namespace GnssProcessor;

namespace GnssProcessor {

/// Test seam (declared a friend in include/model/PppModel.hpp): reaches the
/// PRIVATE first-epoch decision and the latch members, so the restore's contract
/// can be pinned without a geometry / product chain.
struct PppModelCanonicalTestAccess {
    static void decide(PppModel& m, const std::vector<RinexObservation>& obs) {
        m.decideConstellationCanonicals(obs);
    }
    static bool decided(const PppModel& m, const std::string& cons) {
        return m.constellation_canonical_decided_.count(cons) > 0;
    }
    static bool auditPending(const PppModel& m, const std::string& cons) {
        return m.constellation_canonical_audit_pending_.count(cons) > 0;
    }
};

} // namespace GnssProcessor

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

/// A snapshot carrying the latch of two stations, including the exact L2 flip
/// §21.16 measured (station 0 locked on L2X, station 7 on L2W).
NetworkStateSnapshot canonSnapshot() {
    NetworkStateSnapshot s = baseSnapshot();
    s.canonical_signals[0] = {
        NetworkStateSnapshot::CanonSignal{"GPS", "L1", 'C', "C1W"},
        NetworkStateSnapshot::CanonSignal{"GPS", "L1", 'L', "L1W"},
        NetworkStateSnapshot::CanonSignal{"GPS", "L2", 'C', "C2X"},
        NetworkStateSnapshot::CanonSignal{"GPS", "L2", 'L', "L2X"},
    };
    s.canonical_signals[7] = {
        NetworkStateSnapshot::CanonSignal{"GPS", "L2", 'L', "L2W"},
        NetworkStateSnapshot::CanonSignal{"Galileo", "E5a", 'L', "L5Q"},
    };
    return s;
}

fs::path writeTempConfig(const std::string& tag, const std::string& body) {
    const fs::path p = fs::temp_directory_path() / ("resumecanon_cfg_" + tag + ".json");
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
          "    \"signal_priorities\": { \"GPS\": \"WXC\", \"Galileo\": \"CB\" }\n"
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

bool loadInto(const std::string& tag, const std::string& body, GnssConfig& cfg, LogManager& log) {
    const fs::path p = writeTempConfig(tag, body);
    const bool ok = cfg.loadFromJson(p, log);
    std::error_code ec; fs::remove(p, ec);
    return ok;
}

bool loadInto(const std::string& tag, const std::string& body, GnssConfig& cfg) {
    LogManager log;
    return loadInto(tag, body, cfg, log);
}

std::string serialized(const NetworkStateSnapshot& s) {
    std::ostringstream os;
    NetworkStateSnapshotIO::serialize(os, s);
    return os.str();
}

std::vector<std::string> lines(const std::string& t) {
    std::vector<std::string> v;
    std::istringstream is(t);
    for (std::string ln; std::getline(is, ln); ) v.push_back(ln);
    return v;
}

/// The stage-then-apply conversion the driver performs (networkTryWarmResume
/// stages, networkRestoreCanonicalSignals applies), reproduced for one station.
std::vector<PppModel::CanonicalSignal> staged(const NetworkStateSnapshot& s, int idx) {
    std::vector<PppModel::CanonicalSignal> out;
    const auto it = s.canonical_signals.find(idx);
    if (it == s.canonical_signals.end()) return out;
    for (const auto& c : it->second)
        out.push_back(PppModel::CanonicalSignal{c.constellation, c.band, c.obs_type, c.signal});
    return out;
}

/// One GPS epoch's observations with a chosen L2W / L2X satellite split — the
/// tally decideConstellationCanonicals runs on.  L1W is present on every
/// satellite so the L1 band has an unambiguous winner either way.
std::vector<RinexObservation> gpsEpoch(int n_l2w, int n_l2x) {
    std::vector<RinexObservation> obs;
    const int n = n_l2w + n_l2x;
    for (int i = 0; i < n; ++i) {
        RinexObservation o;
        o.satellite = "G" + std::string(i + 1 < 10 ? "0" : "") + std::to_string(i + 1);
        o.system = System::GPS;
        o.measurements["C1W"] = 2.0e7 + i;
        o.measurements["L1W"] = 1.0e8 + i;
        if (i < n_l2w) o.measurements["L2W"] = 1.0e8 + i;
        else           o.measurements["L2X"] = 1.0e8 + i;
        obs.push_back(std::move(o));
    }
    return obs;
}

/// The PppModel construction chain, owned so each test is independent.
struct ModelFixture {
    LogManager   log;
    GnssConfig   cfg;
    std::unique_ptr<ProductParser> pp;
    std::unique_ptr<RinexParser>   rp;
    std::unique_ptr<StateManager>  sm;
    std::unique_ptr<PppModel>      model;

    explicit ModelFixture(const std::string& tag) {
        EXPECT_TRUE(loadInto(tag, networkConfig(), cfg, log));
        pp    = std::make_unique<ProductParser>(cfg, log);
        rp    = std::make_unique<RinexParser>(cfg, log, *pp);
        sm    = std::make_unique<StateManager>(5, log, cfg);
        model = std::make_unique<PppModel>(cfg, *pp, log, *rp, *sm);
        model->setCanonicalTraceLabel("AAAA");
    }
};

} // namespace

// ---------------------------------------------------------------------------
// (a) the snapshot TEXT round-trip
// ---------------------------------------------------------------------------

TEST(ResumeSignalCanonRestore, CanonicalRowsRoundTripThroughTheSnapshotText) {
    const NetworkStateSnapshot s = canonSnapshot();
    ASSERT_TRUE(s.hasCanonicalSignalState());
    const std::string text = serialized(s);
    // The rows ride the EXISTING header — format-forward, not a version bump.
    EXPECT_EQ(text.rfind("# gnss_network_state_snapshot v1\n", 0), 0u) << text.substr(0, 60);
    EXPECT_NE(text.find("CN,0,GPS,L2,L,L2X"), std::string::npos) << text;
    EXPECT_NE(text.find("CN,7,GPS,L2,L,L2W"), std::string::npos) << text;
    EXPECT_NE(text.find("CN,7,Galileo,E5a,L,L5Q"), std::string::npos) << text;

    NetworkStateSnapshot r;
    std::string err;
    std::istringstream is(text);
    ASSERT_TRUE(NetworkStateSnapshotIO::parse(is, r, &err)) << err;
    ASSERT_TRUE(r.hasCanonicalSignalState());
    ASSERT_EQ(r.canonical_signals.size(), 2u);
    ASSERT_EQ(r.canonical_signals.at(0).size(), 4u);
    ASSERT_EQ(r.canonical_signals.at(7).size(), 2u);
    // Per-station ORDER survives: the writer exports in the model's own map order
    // and the reader appends, so a diff of two snapshots is row-stable.
    EXPECT_EQ(r.canonical_signals.at(0)[3].signal, "L2X");
    EXPECT_EQ(r.canonical_signals.at(0)[3].band, "L2");
    EXPECT_EQ(r.canonical_signals.at(0)[3].obs_type, 'L');
    EXPECT_EQ(r.canonical_signals.at(0)[3].constellation, "GPS");
    EXPECT_EQ(r.canonical_signals.at(7)[0].signal, "L2W");
    EXPECT_EQ(r.canonical_signals.at(7)[1].constellation, "Galileo");
}

/// A malformed row is a HARD parse failure (the caller's documented cold-start
/// signal), never a silent partial restore — the RC / SD precedent.  The obs-type
/// case is the one worth naming: anything but 'C' or 'L' installs an entry no
/// lookup can match, which reads as a COLD latch for that band and re-opens the
/// exact defect the rows exist to close.
TEST(ResumeSignalCanonRestore, MalformedCanonicalRowIsAHardParseFailure) {
    const auto mangle = [](const std::string& repl) {
        std::string text = serialized(canonSnapshot());
        const std::size_t pos = text.find("CN,0,GPS,L2,L,L2X");
        EXPECT_NE(pos, std::string::npos) << text;
        const std::size_t eol = text.find('\n', pos);
        text.replace(pos, eol - pos, repl);
        NetworkStateSnapshot r;
        std::string err;
        std::istringstream is(text);
        const bool ok = NetworkStateSnapshotIO::parse(is, r, &err);
        return std::pair<bool, std::string>{ok, err};
    };
    for (const std::string& bad : {
             std::string("CN,0,GPS,L2,L"),           // too few fields
             std::string("CN,0,GPS,L2,L,L2X,extra"), // too many
             std::string("CN,0,,L2,L,L2X"),          // empty constellation
             std::string("CN,0,GPS,,L,L2X"),         // empty band
             std::string("CN,0,GPS,L2,L,"),          // empty signal
             std::string("CN,x,GPS,L2,L,L2X"),       // unparseable index
             std::string("CN,-1,GPS,L2,L,L2X"),      // negative index
             std::string("CN,0,GPS,L2,D,L2X"),       // obs-type neither C nor L
             std::string("CN,0,GPS,L2,LL,L2X") }) {  // obs-type not one char
        const auto res = mangle(bad);
        EXPECT_FALSE(res.first) << "accepted: " << bad;
        EXPECT_NE(res.second.find("CN"), std::string::npos) << bad << " -> " << res.second;
    }
}

// ---------------------------------------------------------------------------
// (c) absence and the write knob
// ---------------------------------------------------------------------------

/// A file with no CN rows restores nothing: an old snapshot (or a writer without
/// the capture knob) leaves every station re-deciding its latch as today.
TEST(ResumeSignalCanonRestore, SnapshotWithoutCanonicalRowsRestoresNothing) {
    const NetworkStateSnapshot s = baseSnapshot();
    ASSERT_FALSE(s.hasCanonicalSignalState());
    const std::string text = serialized(s);
    EXPECT_EQ(text.find("\nCN,"), std::string::npos) << text;

    NetworkStateSnapshot r;
    std::string err;
    std::istringstream is(text);
    ASSERT_TRUE(NetworkStateSnapshotIO::parse(is, r, &err)) << err;
    EXPECT_TRUE(r.canonical_signals.empty());
    EXPECT_FALSE(r.hasCanonicalSignalState());
    EXPECT_TRUE(staged(r, 0).empty());

    // ...and the restore applied to an empty stage is a no-op on the model: the
    // latch stays undecided, so the first epoch decides it exactly as today.
    ModelFixture f("inertmodel");
    EXPECT_EQ(f.model->restoreCanonicalSignals(staged(r, 0)), 0u);
    EXPECT_FALSE(PppModelCanonicalTestAccess::decided(*f.model, "GPS"));
}

/// The WRITE knob is what the driver gates the capture on, so with it off the
/// container stays empty and the file the same snapshot produces is byte-identical
/// to the pre-card one (standing invariant #6, at the format level).
TEST(ResumeSignalCanonRestore, WriteKnobOffEmitsNoneOfTheRows) {
    GnssConfig cfg;
    ASSERT_TRUE(loadInto("nocapture",
        networkConfig("    \"state_snapshot_path\": \"state_snapshot.snap\",\n"), cfg));
    EXPECT_FALSE(cfg.getNetworkConfig().state_snapshot_signal_capture);

    const std::string off = serialized(baseSnapshot());
    NetworkStateSnapshot on_snap = baseSnapshot();
    on_snap.canonical_signals[0] = {
        NetworkStateSnapshot::CanonSignal{"GPS", "L2", 'L', "L2X"} };
    const std::string on = serialized(on_snap);
    ASSERT_NE(off, on);

    const auto la = lines(off), lb = lines(on);
    ASSERT_EQ(la.size() + 1u, lb.size());
    std::vector<std::string> diff;
    std::size_t ia = 0;
    for (const auto& ln : lb) {
        if (ia < la.size() && la[ia] == ln) { ++ia; continue; }
        diff.push_back(ln);
    }
    EXPECT_EQ(ia + 1u, la.size()) << "only the END line may also differ";
    ASSERT_EQ(diff.size(), 2u);
    EXPECT_EQ(diff[0], "CN,0,GPS,L2,L,L2X");
    EXPECT_EQ(diff[1].rfind("END,", 0), 0u) << diff[1];
}

// ---------------------------------------------------------------------------
// (d) the model's export / restore pair and the first-epoch guard
// ---------------------------------------------------------------------------

/// The baseline the card is about: with NO restore, the station's latch is
/// decided by whatever epoch it happens to see first — so the same model fed a
/// leg-shaped epoch first locks L2W where a seed-shaped epoch locks L2X.  This is
/// the flip §21.16 measured (GAMB00PYF L2X→L2W), reproduced in a unit test.
TEST(ResumeSignalCanonRestore, WithoutARestoreTheFirstEpochsTallyDecidesTheLatch) {
    {   // seed-shaped first epoch: L2X leads 10 to 9
        ModelFixture f("seedshape");
        PppModelCanonicalTestAccess::decide(*f.model, gpsEpoch(/*l2w=*/9, /*l2x=*/10));
        const auto rows = f.model->exportCanonicalSignals();
        bool found = false;
        for (const auto& c : rows)
            if (c.band == "L2" && c.obs_type == 'L') { EXPECT_EQ(c.signal, "L2X"); found = true; }
        EXPECT_TRUE(found) << "no L2 phase canonical was decided";
    }
    {   // leg-shaped first epoch: L2W leads 10 to 8 — the latch flips
        ModelFixture f("legshape");
        PppModelCanonicalTestAccess::decide(*f.model, gpsEpoch(/*l2w=*/10, /*l2x=*/8));
        const auto rows = f.model->exportCanonicalSignals();
        for (const auto& c : rows)
            if (c.band == "L2" && c.obs_type == 'L') EXPECT_EQ(c.signal, "L2W");
    }
}

/// The card's contract, in one test: a RESTORED latch marks the constellation
/// DECIDED, so the next epoch's tally — even one that would pick the other
/// channel — leaves it alone.  Export after the epoch returns the writer's
/// signals, and the audit reports the disagreement rather than acting on it.
TEST(ResumeSignalCanonRestore, ARestoredLatchIsNotReDecidedByTheNextEpochsTally) {
    ModelFixture f("restored");
    // The writer's latch: L2X (what a 00:00 epoch decided).
    NetworkStateSnapshot r;
    std::string err;
    std::istringstream is(serialized(canonSnapshot()));
    ASSERT_TRUE(NetworkStateSnapshotIO::parse(is, r, &err)) << err;
    const auto rows = staged(r, 0);
    ASSERT_EQ(rows.size(), 4u);
    EXPECT_EQ(f.model->restoreCanonicalSignals(rows), 4u);
    EXPECT_TRUE(PppModelCanonicalTestAccess::decided(*f.model, "GPS"));
    EXPECT_TRUE(PppModelCanonicalTestAccess::auditPending(*f.model, "GPS"));
    EXPECT_EQ(f.model->canonicalAuditEntries(), 0);
    EXPECT_EQ(f.model->canonicalAuditDisagreements(), 0);

    // The resume epoch's tally prefers L2W — exactly the leg-shaped counts.
    PppModelCanonicalTestAccess::decide(*f.model, gpsEpoch(/*l2w=*/10, /*l2x=*/8));

    const auto after = f.model->exportCanonicalSignals();
    ASSERT_EQ(after.size(), 4u);
    for (const auto& c : after)
        if (c.band == "L2" && c.obs_type == 'L')
            EXPECT_EQ(c.signal, "L2X") << "the restored latch was RE-DECIDED";
    // The audit ran exactly once, compared the bands the epoch also tallied, and
    // found the L2 phase channel disagreeing — a nonzero count is the defect being
    // PREVENTED, and it is the run-end instrument for that.
    EXPECT_FALSE(PppModelCanonicalTestAccess::auditPending(*f.model, "GPS"));
    EXPECT_GE(f.model->canonicalAuditEntries(), 2);
    EXPECT_EQ(f.model->canonicalAuditDisagreements(), 1);

    // A second epoch neither re-decides nor re-audits (the audit is one-shot).
    const long long entries = f.model->canonicalAuditEntries();
    PppModelCanonicalTestAccess::decide(*f.model, gpsEpoch(/*l2w=*/12, /*l2x=*/2));
    EXPECT_EQ(f.model->canonicalAuditEntries(), entries);
    for (const auto& c : f.model->exportCanonicalSignals())
        if (c.band == "L2" && c.obs_type == 'L') EXPECT_EQ(c.signal, "L2X");
}

/// A restore whose latch AGREES with the resume epoch reports zero
/// disagreements — the other direction of the same instrument, so a 0 at run end
/// reads as "the seam epoch agreed", never as "the wiring did nothing" (the
/// entries / stations counters carry that).
TEST(ResumeSignalCanonRestore, AnAgreeingRestoreAuditsZeroDisagreements) {
    ModelFixture f("agree");
    EXPECT_EQ(f.model->restoreCanonicalSignals(staged(canonSnapshot(), 0)), 4u);
    PppModelCanonicalTestAccess::decide(*f.model, gpsEpoch(/*l2w=*/8, /*l2x=*/11));
    EXPECT_GE(f.model->canonicalAuditEntries(), 2);
    EXPECT_EQ(f.model->canonicalAuditDisagreements(), 0);
}

/// Rows the restore cannot mean anything by are ignored rather than installed —
/// an entry with a bogus obs-type would sit in the map matching no lookup, which
/// reads as a cold latch for that band.  (The parser refuses such a row outright;
/// this is the model's own belt-and-braces for a non-snapshot caller.)
TEST(ResumeSignalCanonRestore, RestoreIgnoresRowsItCannotMean) {
    ModelFixture f("ignore");
    const std::vector<PppModel::CanonicalSignal> rows = {
        PppModel::CanonicalSignal{"", "L2", 'L', "L2X"},
        PppModel::CanonicalSignal{"GPS", "", 'L', "L2X"},
        PppModel::CanonicalSignal{"GPS", "L2", 'L', ""},
        PppModel::CanonicalSignal{"GPS", "L2", 'D', "L2X"},
        PppModel::CanonicalSignal{"GPS", "L2", 'L', "L2X"},   // the only good one
    };
    EXPECT_EQ(f.model->restoreCanonicalSignals(rows), 1u);
    const auto out = f.model->exportCanonicalSignals();
    ASSERT_EQ(out.size(), 1u);
    EXPECT_EQ(out[0].signal, "L2X");
    EXPECT_TRUE(PppModelCanonicalTestAccess::decided(*f.model, "GPS"));
}

// ---------------------------------------------------------------------------
// (e) the loader
// ---------------------------------------------------------------------------

TEST(ResumeSignalCanonRestoreConfig, DefaultsAreOff) {
    GnssConfig cfg;
    ASSERT_TRUE(loadInto("defaults", networkConfig(), cfg));
    EXPECT_FALSE(cfg.getNetworkConfig().state_snapshot_signal_capture);
    EXPECT_FALSE(cfg.networkResumeSignalCanonRestore());
}

TEST(ResumeSignalCanonRestoreConfig, CaptureKnobParsesAndDoesNotArmTheReadSide) {
    GnssConfig cfg;
    ASSERT_TRUE(loadInto("capture",
        networkConfig("    \"state_snapshot_path\": \"state_snapshot.snap\",\n"
                      "    \"state_snapshot_signal_capture\": true,\n"), cfg));
    EXPECT_TRUE(cfg.getNetworkConfig().state_snapshot_signal_capture);
    EXPECT_FALSE(cfg.networkResumeSignalCanonRestore());
}

/// The arcs the restored realization frame names ARE the writer's canonical
/// signals', so the read knob is only meaningful when that frame also continues —
/// mirroring resume_datum_restore / resume_emit_restore / resume_station_restore.
TEST(ResumeSignalCanonRestoreConfig, RestoreWithoutFrameRestoreIsRejected) {
    // The loader turns its own throw into a FAILED LOAD (loadFromJson catches and
    // returns false) — the shape every other restore prerequisite is asserted in.
    GnssConfig cfg;
    EXPECT_FALSE(loadInto("noframe",
        networkConfig("    \"state_snapshot_path\": \"state_snapshot.snap\",\n"
                      "    \"resume_signal_canon_restore\": true,\n"), cfg));
}

TEST(ResumeSignalCanonRestoreConfig, ArmedPostureLoads) {
    GnssConfig cfg;
    ASSERT_TRUE(loadInto("armed",
        networkConfig("    \"state_snapshot_path\": \"state_snapshot.snap\",\n"
                      "    \"state_snapshot_frame_capture\": true,\n"
                      "    \"state_snapshot_signal_capture\": true,\n"
                      "    \"resume_frame_restore\": true,\n"
                      "    \"resume_signal_canon_restore\": true,\n"), cfg));
    EXPECT_TRUE(cfg.getNetworkConfig().state_snapshot_signal_capture);
    EXPECT_TRUE(cfg.networkResumeSignalCanonRestore());
    EXPECT_TRUE(cfg.networkResumeFrameRestore());
}

/// Armed with no snapshot path the loader warns and still loads (INERT, not
/// fatal) — the posture every other resume-response knob takes.
TEST(ResumeSignalCanonRestoreConfig, ArmedWithoutASnapshotPathStillLoads) {
    GnssConfig cfg;
    ASSERT_TRUE(loadInto("inert",
        networkConfig("    \"state_snapshot_signal_capture\": true,\n"
                      "    \"resume_frame_restore\": true,\n"
                      "    \"resume_signal_canon_restore\": true,\n"), cfg));
    EXPECT_TRUE(cfg.networkResumeSignalCanonRestore());
}
