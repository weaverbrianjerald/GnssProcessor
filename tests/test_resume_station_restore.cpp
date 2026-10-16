/**
 * @file test_resume_station_restore.cpp
 * @brief Contract tests for the RS-6 station-layer continuity card
 *        (docs/SPEC_event_harness.md §21.13): the snapshot's format-forward
 *        station-layer records (@c RC: the rx-CB carry, one receiver code-bias
 *        posterior per (station roster index, CB state key); @c SD: the
 *        T5 / Iono-2 per-LOS SH representation-error deviation carry, one
 *        @c ShDevCarry per "STATION|SAT"), and the loader's side of the write
 *        knob (@c network.state_snapshot_station_capture), the read knob
 *        (@c network.resume_station_restore) and its diagnostic rx-CB
 *        sub-switch (@c ..._rxcb).
 *
 * Covered here:
 *   (a) both maps round-trip through the snapshot text exactly, with a
 *       '|'-bearing key (the SD key is "STATION|SAT") and full-precision
 *       doubles, since the restored rx-CB value is the prior the next fold's
 *       innovation differences against and the restored deviation is the GM
 *       level the next harvest ages;
 *   (b) the restored rx-CB entries carry the restored-arc sentinel of §15.4.1
 *       (the @c makeRestoredLocalCarry convention the @c L / @c Z rows use), and
 *       a restored deviation entry reads warm because @c ShDevCarry's own cold
 *       test is @c t_s <= 0 and the harvest stamp travels;
 *   (c) a snapshot with no @c RC / @c SD rows restores nothing (an old file
 *       keeps the cold-start behaviour, and the containers stay empty);
 *   (d) with the write knob off none of the rows is emitted, and the file is
 *       byte-identical to the pre-RS-6 format for the same snapshot;
 *   (e) the rx-CB sub-switch withholds only its half.
 *
 * The seam mechanics (the estimator's own level continuing across a warm resume:
 * the raw phase bias, the receiver clocks, the SH a₀₀) are graded on the harness
 * legs, not here.
 */
#include <gtest/gtest.h>

#include "config/GnssConfig.hpp"
#include "network/driver/NetworkRunTypes.hpp"
#include "logging/LogManager.hpp"
#include "products/NetworkStateSnapshot.hpp"

#include <cmath>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace fs = std::filesystem;
using namespace GnssProcessor;

namespace {

NetworkStateSnapshot baseSnapshot() {
    NetworkStateSnapshot s;
    s.snap_week = 2313;
    s.snap_tow  = 277290.0;
    s.wall_utc  = "2026-09-12T00:00:00Z";
    s.generation_id = 1399161629u;
    s.config_fingerprint = 0x545b8bdd2b638231ull;
    s.n_emitted = 604;
    s.prev_week = 2313;
    s.prev_tow  = 277290.0;
    return s;
}

/// A snapshot carrying both station-layer record kinds, with values chosen so a
/// shortest-round-trip decimal is not the same string as a rounded one.
NetworkStateSnapshot stationSnapshot() {
    NetworkStateSnapshot s = baseSnapshot();
    // rx-CB: the two families that coexist on the field lineage, at two stations.
    s.rx_cb_carry[{0, "GPS_DCB"}] =
        NetworkStateSnapshot::Local{-3.8391234567890123, 1.2345678901234e-04, 1400277290.0};
    s.rx_cb_carry[{7, "GPS_IFB5"}] =
        NetworkStateSnapshot::Local{0.0625, 3.0517578125e-05, 1400277260.0};
    // Deviation carry (T5): the key contains a '|'; the format splits on commas only.
    s.sh_dev_carry["WTZZ00DEU|G24"] =
        NetworkStateSnapshot::ShDev{-1.2345678901234567, 0.87654321098765, 1.5625, 1400277290.0};
    s.sh_dev_carry["MAJU00MHL|E11"] =
        NetworkStateSnapshot::ShDev{0.5, 1.5625, 1.5625, 1400277260.0};
    return s;
}

fs::path writeTempConfig(const std::string& tag, const std::string& body) {
    const fs::path p = fs::temp_directory_path() / ("resumestation_cfg_" + tag + ".json");
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

std::vector<std::string> lines(const std::string& t) {
    std::vector<std::string> v;
    std::istringstream is(t);
    for (std::string ln; std::getline(is, ln); ) v.push_back(ln);
    return v;
}

/// The restore the driver performs for the rx-CB half (networkTryWarmResume in
/// NetworkWarmResume.cpp), reproduced here so the sentinel convention is locked by
/// a test rather than by a comment; the driver's own block is one `if` there.
std::map<std::pair<int, std::string>, LocalCarry>
restoreRxCb(const NetworkStateSnapshot& s) {
    std::map<std::pair<int, std::string>, LocalCarry> out;
    for (const auto& [key, l] : s.rx_cb_carry)
        out[{key.first, key.second}] =
            makeRestoredLocalCarry(l.value_abs, l.var, l.last_seen_s);
    return out;
}

/// The deviation half's restore: as written, deliberately without a sentinel.
std::map<std::string, ShDevCarry> restoreShDev(const NetworkStateSnapshot& s) {
    std::map<std::string, ShDevCarry> out;
    for (const auto& [key, d] : s.sh_dev_carry)
        out[key] = ShDevCarry{d.dev, d.var, d.var_ss, d.t_s};
    return out;
}

} // namespace

// ---------------------------------------------------------------------------
// (a) the snapshot text round-trip
// ---------------------------------------------------------------------------

TEST(ResumeStationRestore, StationLayerRoundTripsThroughTheSnapshotText) {
    const NetworkStateSnapshot s = stationSnapshot();
    ASSERT_TRUE(s.hasStationState());
    const std::string text = serialized(s);
    // The rows use the existing header: they are format-forward, not a new version.
    EXPECT_EQ(text.rfind("# gnss_network_state_snapshot v1\n", 0), 0u) << text.substr(0, 60);
    EXPECT_NE(text.find("RC,0,GPS_DCB,"), std::string::npos) << text;
    EXPECT_NE(text.find("RC,7,GPS_IFB5,"), std::string::npos) << text;
    // The '|' in the SD key survives the comma-split format verbatim.
    EXPECT_NE(text.find("SD,WTZZ00DEU|G24,"), std::string::npos) << text;

    NetworkStateSnapshot r;
    std::string err;
    std::istringstream is(text);
    ASSERT_TRUE(NetworkStateSnapshotIO::parse(is, r, &err)) << err;
    ASSERT_EQ(r.rx_cb_carry.size(), 2u);
    // Full precision, bitwise: the restored value is the prior the next fold's
    // innovation differences against, and its var is that fold's weight.
    const auto& a = r.rx_cb_carry.at({0, "GPS_DCB"});
    EXPECT_DOUBLE_EQ(a.value_abs, -3.8391234567890123);
    EXPECT_DOUBLE_EQ(a.var, 1.2345678901234e-04);
    EXPECT_DOUBLE_EQ(a.last_seen_s, 1400277290.0);
    EXPECT_DOUBLE_EQ(r.rx_cb_carry.at({7, "GPS_IFB5"}).value_abs, 0.0625);
    ASSERT_EQ(r.sh_dev_carry.size(), 2u);
    const auto& d = r.sh_dev_carry.at("WTZZ00DEU|G24");
    EXPECT_DOUBLE_EQ(d.dev, -1.2345678901234567);
    EXPECT_DOUBLE_EQ(d.var, 0.87654321098765);
    EXPECT_DOUBLE_EQ(d.var_ss, 1.5625);
    EXPECT_DOUBLE_EQ(d.t_s, 1400277290.0);
    EXPECT_TRUE(r.hasStationState());
}

/// strtod accepts a non-finite value, which would then corrupt every subsequent
/// fold of that (station, family).  As for the ER / CBP rows, it is a hard parse
/// failure (the caller's documented cold-start signal), never a silent partial
/// restore.  A non-positive rx-CB variance is refused for the same reason: the
/// harvest only stashes var > 0.  The SD variance is checked the same way.
TEST(ResumeStationRestore, NonFiniteOrNonPositiveStationFieldIsAHardParseFailure) {
    const auto mangle = [](const std::string& tag_prefix, const std::string& repl) {
        NetworkStateSnapshot s = stationSnapshot();
        std::string text = serialized(s);
        const std::size_t pos = text.find(tag_prefix);
        EXPECT_NE(pos, std::string::npos) << text;
        const std::size_t eol = text.find('\n', pos);
        text.replace(pos, eol - pos, repl);
        NetworkStateSnapshot r;
        std::string err;
        std::istringstream is(text);
        const bool ok = NetworkStateSnapshotIO::parse(is, r, &err);
        return std::pair<bool, std::string>{ok, err};
    };
    auto rc_nan = mangle("RC,0,GPS_DCB,", "RC,0,GPS_DCB,nan,1e-4,1400277290");
    EXPECT_FALSE(rc_nan.first);
    EXPECT_NE(rc_nan.second.find("RC"), std::string::npos) << rc_nan.second;
    auto rc_var = mangle("RC,0,GPS_DCB,", "RC,0,GPS_DCB,1.0,0,1400277290");
    EXPECT_FALSE(rc_var.first);
    EXPECT_NE(rc_var.second.find("RC"), std::string::npos) << rc_var.second;
    auto sd_inf = mangle("SD,MAJU00MHL|E11,", "SD,MAJU00MHL|E11,inf,1,1,1400277260");
    EXPECT_FALSE(sd_inf.first);
    EXPECT_NE(sd_inf.second.find("SD"), std::string::npos) << sd_inf.second;
    auto sd_neg = mangle("SD,MAJU00MHL|E11,", "SD,MAJU00MHL|E11,0.5,-1,1,1400277260");
    EXPECT_FALSE(sd_neg.first);
    EXPECT_NE(sd_neg.second.find("SD"), std::string::npos) << sd_neg.second;
}

// ---------------------------------------------------------------------------
// (b) the restored-entry conventions
// ---------------------------------------------------------------------------

/// The rx-CB half goes back through @c makeRestoredLocalCarry, so every restored
/// entry carries the §15.4.1 restored-arc sentinel rather than a 0 that also
/// means "never stashed", so a later age reader can rely on it.  The rx-CB inject
/// reads only value_abs / var / last_seen_s at present, so no run exercises the
/// convention and this test locks it.
TEST(ResumeStationRestore, RestoredRxCbEntriesCarryTheRestoredArcSentinel) {
    NetworkStateSnapshot r;
    std::string err;
    std::istringstream is(serialized(stationSnapshot()));
    ASSERT_TRUE(NetworkStateSnapshotIO::parse(is, r, &err)) << err;

    const auto carry = restoreRxCb(r);
    ASSERT_EQ(carry.size(), 2u);
    for (const auto& [key, lc] : carry) {
        EXPECT_EQ(lc.birth_ep, LocalCarry::kBirthEpRestored) << key.second;
        EXPECT_DOUBLE_EQ(lc.birth_s, LocalCarry::kBirthSRestored);
        // Restored ⇒ mature at every age reader (all of which test `> 0`), and
        // not re-brandable by a first-stash-wins rule (which tests `!= 0`).
        EXPECT_FALSE(localCarryYoung(lc, /*now_ep=*/3, /*min_age_ep=*/10));
        EXPECT_NE(lc.birth_ep, 0);
    }
    const auto& dcb = carry.at({0, "GPS_DCB"});
    EXPECT_DOUBLE_EQ(dcb.value_abs, -3.8391234567890123);
    EXPECT_DOUBLE_EQ(dcb.var, 1.2345678901234e-04);
    EXPECT_DOUBLE_EQ(dcb.last_seen_s, 1400277290.0);
    // The harvest-side fields a restore cannot know stay at their defaults.
    EXPECT_TRUE(dcb.coupling.empty());
    EXPECT_EQ(dcb.blk, nullptr);
    EXPECT_DOUBLE_EQ(dcb.var_first, 0.0);
}

/// The deviation half is restored as written, with no sentinel, on purpose:
/// @c ShDevCarry's own cold test is @c t_s <= 0, so the writer's harvest stamp is
/// what makes the entry read warm instead of cold-seeding at the steady state
/// (dev 0, var = representation_sigma²).
TEST(ResumeStationRestore, RestoredDeviationEntriesReadWarmThroughTheirHarvestStamp) {
    NetworkStateSnapshot r;
    std::string err;
    std::istringstream is(serialized(stationSnapshot()));
    ASSERT_TRUE(NetworkStateSnapshotIO::parse(is, r, &err)) << err;

    const auto dev = restoreShDev(r);
    ASSERT_EQ(dev.size(), 2u);
    for (const auto& [key, dc] : dev)
        EXPECT_GT(dc.t_s, 0.0) << key << " must read WARM (the cold test is t_s <= 0)";
    const auto& g24 = dev.at("WTZZ00DEU|G24");
    EXPECT_DOUBLE_EQ(g24.dev, -1.2345678901234567);
    EXPECT_DOUBLE_EQ(g24.var, 0.87654321098765);
    EXPECT_DOUBLE_EQ(g24.var_ss, 1.5625);
    EXPECT_DOUBLE_EQ(g24.t_s, 1400277290.0);
    // A cold-started map is the control: every entry would seed at steady state.
    const std::map<std::string, ShDevCarry> cold;
    EXPECT_TRUE(cold.empty());
}

// ---------------------------------------------------------------------------
// (c) / (d) absence and the write knob
// ---------------------------------------------------------------------------

/// A file with no station-layer rows restores nothing: an old snapshot (or a
/// writer without the capture knob) leaves both carries cold-starting.
TEST(ResumeStationRestore, SnapshotWithoutStationRowsRestoresNothing) {
    const NetworkStateSnapshot s = baseSnapshot();
    ASSERT_FALSE(s.hasStationState());
    const std::string text = serialized(s);
    EXPECT_EQ(text.find("\nRC,"), std::string::npos) << text;
    EXPECT_EQ(text.find("\nSD,"), std::string::npos) << text;

    NetworkStateSnapshot r;
    std::string err;
    std::istringstream is(text);
    ASSERT_TRUE(NetworkStateSnapshotIO::parse(is, r, &err)) << err;
    EXPECT_TRUE(r.rx_cb_carry.empty());
    EXPECT_TRUE(r.sh_dev_carry.empty());
    EXPECT_FALSE(r.hasStationState());
    EXPECT_TRUE(restoreRxCb(r).empty());
    EXPECT_TRUE(restoreShDev(r).empty());
}

/// The driver gates the capture on the write knob, so with it off the containers
/// stay empty and the file the same snapshot produces is byte-identical to the
/// pre-RS-6 one (standing invariant #6, at the format level).
TEST(ResumeStationRestore, WriteKnobOffEmitsNoneOfTheRows) {
    GnssConfig cfg;
    ASSERT_TRUE(loadInto("nocapture",
        networkConfig("    \"state_snapshot_path\": \"state_snapshot.snap\",\n"), cfg));
    EXPECT_FALSE(cfg.getNetworkConfig().state_snapshot_station_capture);

    // What the driver writes with the knob off: a snapshot whose station containers
    // were never filled.  Line-wise, the only differences against an armed one are
    // the added rows and the END line count they advance.
    const std::string off = serialized(baseSnapshot());
    NetworkStateSnapshot on_snap = baseSnapshot();
    on_snap.rx_cb_carry[{0, "GPS_DCB"}] =
        NetworkStateSnapshot::Local{1.0, 0.25, 1400277290.0};
    on_snap.sh_dev_carry["WTZZ00DEU|G24"] =
        NetworkStateSnapshot::ShDev{0.5, 1.0, 1.5625, 1400277290.0};
    const std::string on = serialized(on_snap);
    ASSERT_NE(off, on);

    const auto la = lines(off), lb = lines(on);
    ASSERT_EQ(la.size() + 2u, lb.size());
    std::vector<std::string> diff;
    std::size_t ia = 0;
    for (const auto& ln : lb) {
        if (ia < la.size() && la[ia] == ln) { ++ia; continue; }
        diff.push_back(ln);
    }
    EXPECT_EQ(ia + 1u, la.size()) << "only the END line may also differ";
    ASSERT_EQ(diff.size(), 3u);
    EXPECT_EQ(diff[0], "RC,0,GPS_DCB,1,0.25,1400277290");
    EXPECT_EQ(diff[1], "SD,WTZZ00DEU|G24,0.5,1,1.5625,1400277290");
    EXPECT_EQ(diff[2].rfind("END,", 0), 0u) << diff[2];
}

// ---------------------------------------------------------------------------
// (e) the diagnostic rx-CB sub-switch withholds its half
// ---------------------------------------------------------------------------

/// The rx-CB sub-switch restores only its half; the T5 deviation half restores
/// whenever its rows are present.  The attribution matrix of §21.13 of
/// SPEC_event_harness rests on this: the half whose restore moves a channel owns
/// it.  A withheld half leaves its map exactly as a cold start does.
TEST(ResumeStationRestore, RxCbSubSwitchRestoresOnlyItsHalf) {
    NetworkStateSnapshot r;
    std::string err;
    std::istringstream is(serialized(stationSnapshot()));
    ASSERT_TRUE(NetworkStateSnapshotIO::parse(is, r, &err)) << err;

    // The driver's gates are `have_rc && rxcb` and `have_sd`; mirror them.
    const auto restore = [&](bool rxcb) {
        std::map<std::pair<int, std::string>, LocalCarry> cb;
        std::map<std::string, ShDevCarry> dev;
        if (!r.rx_cb_carry.empty() && rxcb) cb  = restoreRxCb(r);
        if (!r.sh_dev_carry.empty())        dev = restoreShDev(r);
        return std::pair<std::size_t, std::size_t>{cb.size(), dev.size()};
    };
    EXPECT_EQ(restore(true),  (std::pair<std::size_t, std::size_t>{2u, 2u}));
    EXPECT_EQ(restore(false), (std::pair<std::size_t, std::size_t>{0u, 2u}));
}

TEST(ResumeStationRestoreConfig, DefaultsAreOffAndTheRxCbSubSwitchOn) {
    GnssConfig cfg;
    ASSERT_TRUE(loadInto("defaults", networkConfig(), cfg));
    EXPECT_FALSE(cfg.getNetworkConfig().state_snapshot_station_capture);
    EXPECT_FALSE(cfg.networkResumeStationRestore());
    // The sub-switch defaults to true: an ordinary armed resume restores both
    // halves, and only an attribution leg sets it.
    EXPECT_TRUE(cfg.networkResumeStationRestoreRxCb());
}

TEST(ResumeStationRestoreConfig, CaptureKnobParsesAndDoesNotArmTheReadSide) {
    GnssConfig cfg;
    ASSERT_TRUE(loadInto("capture",
        networkConfig("    \"state_snapshot_path\": \"state_snapshot.snap\",\n"
                      "    \"state_snapshot_station_capture\": true,\n"), cfg));
    EXPECT_TRUE(cfg.getNetworkConfig().state_snapshot_station_capture);
    EXPECT_FALSE(cfg.networkResumeStationRestore());
}

/// Both memories are expressed in the realization frame underneath them, so the
/// read knob is meaningful only when that frame also continues, as for
/// @c resume_datum_restore / @c resume_emit_restore.
TEST(ResumeStationRestoreConfig, RestoreWithoutFrameRestoreIsRejected) {
    // The loader turns its own throw into a failed load (loadFromJson catches and
    // returns false), the form in which every other restore prerequisite is asserted.
    GnssConfig cfg;
    EXPECT_FALSE(loadInto("noframe",
        networkConfig("    \"state_snapshot_path\": \"state_snapshot.snap\",\n"
                      "    \"resume_station_restore\": true,\n"), cfg));
}

TEST(ResumeStationRestoreConfig, ArmedPostureLoadsAndTheSubSwitchParses) {
    GnssConfig cfg;
    ASSERT_TRUE(loadInto("armed",
        networkConfig("    \"state_snapshot_path\": \"state_snapshot.snap\",\n"
                      "    \"state_snapshot_frame_capture\": true,\n"
                      "    \"state_snapshot_station_capture\": true,\n"
                      "    \"resume_frame_restore\": true,\n"
                      "    \"resume_station_restore\": true,\n"
                      "    \"resume_station_restore_rxcb\": false,\n"), cfg));
    EXPECT_TRUE(cfg.getNetworkConfig().state_snapshot_station_capture);
    EXPECT_TRUE(cfg.networkResumeStationRestore());
    EXPECT_FALSE(cfg.networkResumeStationRestoreRxCb());
    EXPECT_TRUE(cfg.networkResumeFrameRestore());
}

/// Armed with no snapshot path, the loader warns and still loads (inert, not
/// fatal), as for every other resume-response knob.
TEST(ResumeStationRestoreConfig, ArmedWithoutASnapshotPathStillLoads) {
    GnssConfig cfg;
    ASSERT_TRUE(loadInto("inert",
        networkConfig("    \"state_snapshot_station_capture\": true,\n"
                      "    \"resume_frame_restore\": true,\n"
                      "    \"resume_station_restore\": true,\n"), cfg));
    EXPECT_TRUE(cfg.networkResumeStationRestore());
}
