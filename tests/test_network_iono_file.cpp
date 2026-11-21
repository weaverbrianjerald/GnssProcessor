/**
 * @file test_network_iono_file.cpp
 * @brief Unit tests of the user's file ionosphere: the reader of the network run's traces
 *        (realtime/NetworkIonoFile.hpp) and the corrections, rows and per-satellite events
 *        (realtime/NetworkIonoConstraint.hpp).
 *
 * The cases:
 *   - the reader keeps the listed receivers only, marks a link integer when the ring fix holds or decides it or the
 *     chain pins it on two bands (one band or a row without a pin target is not enough), matches an epoch within half
 *     a second, reads the receivers' positions, and refuses a missing trace or a listed receiver with no row;
 *   - the sigma table: the first bin below the first edge, the bin's value above it; a constellation with its own
 *     table takes it, the others the common one;
 *   - pair: the receiver term (the median of the difference) cancels, a satellite the two receivers disagree on, a
 *     non-integer link, a link below the elevation cut at the network receiver and a missing link are named, three
 *     satellites are needed;
 *   - plane: a field linear in east and north is recovered exactly at the user (relative to the reference); links
 *     below the cut stay out of the fit, and a satellite whose links are all below it is named;
 *   - single: one receiver's integer links above the cut only; a missing epoch is named;
 *   - events: the user's pivot never enters a row (the reference is the highest non-pivot satellite), a pivot
 *     hand-over is counted and the satellite that was the pivot returns only after return_ep passing epochs; the
 *     innovation test is centred on the held satellites, so satellites whose integers are not held cannot drag the
 *     centre; a release (not integer) waits return_ep passing epochs before the row returns; an innovation release
 *     resets the slant variance (refloat); an interrupted return is counted; an active satellite the user no longer
 *     offers is released; the run-end line carries the counters.
 *
 * Suite names @c NetworkIonoFile and @c NetworkIonoConstraint are owned solely by this file.
 */

#include <gtest/gtest.h>

#include "realtime/NetworkIonoConstraint.hpp"
#include "realtime/NetworkIonoFile.hpp"

#include <array>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

using namespace GnssProcessor;
namespace fs = std::filesystem;

namespace {

constexpr double kT0 = 2420.0 * 604800.0 + 97200.0;   // week 2420, 03:00 on day 1 of the week

/// Write the four traces of a small network run into a fresh folder.
fs::path writeTraces(const std::string& tag) {
    const fs::path dir = fs::temp_directory_path() / ("iono_file_test_" + tag);
    fs::remove_all(dir);
    fs::create_directories(dir);
    std::ofstream(dir / "network_ring_fix.csv")
        << "week,tow,station,sat,sig1,sig2,ring,candidate,state,refused\n"
        << "2420,97200.000,AAAA00XXX,G01,L1C,L2W,2,1,held,\n"
        << "2420,97200.000,AAAA00XXX,G02,L1C,L2W,2,1,refused,window\n"
        << "2420,97200.000,BBBB00XXX,G01,L1C,L2W,2,1,decided,\n"
        << "2420,97200.000,CCCC00XXX,G01,L1C,L2W,2,1,held,\n";
    std::ofstream(dir / "network_chain_edges.csv")
        << "week,tow,system,family,band,comp,r1,edge_sta,edge_sat,child,parent,depth,target_m,target_cyc\n"
        << "2420,97200.000,G,pair,L1C,0,R,AAAA00XXX,G03,G03,AAAA00XXX,1,0.0,0.0\n"
        << "2420,97200.000,G,pair,L2W,0,R,AAAA00XXX,G03,G03,AAAA00XXX,1,0.0,0.0\n"
        << "2420,97200.000,G,pair,L1C,0,R,AAAA00XXX,G04,G04,AAAA00XXX,1,0.0,0.0\n"
        << "2420,97200.000,G,pair,L1C,0,R,AAAA00XXX,G05,G05,AAAA00XXX,1,0.0,\n"
        << "2420,97200.000,G,pair,L2W,0,R,AAAA00XXX,G05,G05,AAAA00XXX,1,0.0,\n";
    std::ofstream iono(dir / "network_ionosphere.csv");
    iono << "week,tow,station,sat,iono_m,elevation_deg,azimuth_deg,x_lin_m,iono_total_m\n";
    for (int s = 1; s <= 5; ++s)
        iono << "2420,97200.000,AAAA00XXX,G0" << s << ",0.1,40.0,90.0,1.0," << (1.0 + 0.1 * s) << "\n";
    iono << "2420,97200.000,BBBB00XXX,G01,0.1,41.0,90.0,1.0,0.75\n";
    iono << "2420,97200.000,CCCC00XXX,G01,0.1,42.0,90.0,1.0,0.25\n";
    std::ofstream(dir / "network_stations.csv")
        << "station,role,ecef_x,ecef_y,ecef_z,phase_tracking\n"
        << "AAAA00XXX,network,4075580.0,931854.0,4801568.0,G:L1C L2W\n"
        << "BBBB00XXX,network,4075535.0,931823.0,4801609.0,G:L1C L2W\n";
    return dir;
}

IonoFileSettings settingsOf(IonoFileCombine combine, std::vector<std::string> stations) {
    IonoFileSettings s;
    s.combine = combine;
    s.stations = std::move(stations);
    s.sigma_elev_deg = { 15.0, 30.0 };
    s.sigma_m = { 0.01, 0.005 };
    s.test_k = 4.0;
    s.return_ep = 3;
    return s;
}

NetworkIonoLink link(double iono, bool integer = true) {
    NetworkIonoLink l;
    l.iono_m = iono;
    l.elevation_deg = 45.0;
    l.integer = integer;
    return l;
}

const IonoFileCorrection& corrOf(const std::vector<IonoFileCorrection>& c, const std::string& sat) {
    for (const auto& x : c)
        if (x.sat == sat) return x;
    throw std::runtime_error("no correction for " + sat);
}

/// One epoch of the event owner with every offered satellite valid at value v and state x (variance var).
IonoFileDecision decideAll(NetworkIonoConstraint& owner, const std::map<std::string, double>& value,
                           const std::map<std::string, double>& x, double var, const std::string& pivot,
                           const std::set<std::string>& invalid = {}) {
    std::vector<IonoFileCorrection> corr;
    std::map<std::string, IonoFileUserState> state;
    std::map<std::string, double> elev;
    for (const auto& [sat, v] : value) {
        IonoFileCorrection c;
        c.sat = sat;
        c.valid = !invalid.count(sat);
        c.cause = c.valid ? IonoFileCause::Missing : IonoFileCause::NotInteger;
        c.value_m = v;
        c.sigma_m = 0.005;
        corr.push_back(c);
        IonoFileUserState st;
        st.x_m = x.at(sat);
        st.var_m2 = (sat == pivot) ? 0.0 : var;
        st.pivot = sat == pivot;
        state[sat] = st;
        elev[sat] = 45.0;
    }
    return owner.decide(corr, state, elev);
}

}  // namespace

TEST(NetworkIonoFile, ReadsTracesAndFlagsIntegerLinks) {
    const fs::path dir = writeTraces("read");
    std::string error;
    const NetworkIonoFile f = NetworkIonoFile::read(dir.string(), { "AAAA00XXX", "BBBB00XXX" }, error);
    ASSERT_TRUE(error.empty()) << error;
    EXPECT_EQ(f.counts().rows, 6u);           // CCCC00XXX is not listed
    EXPECT_EQ(f.counts().epochs, 1u);
    EXPECT_EQ(f.counts().ring_fix_rows, 2u);  // held and decided rows of the listed receivers
    EXPECT_EQ(f.counts().pin_rows, 3u);       // rows with a pin target
    const NetworkIonoEpoch* e = f.epochAt(kT0 + 0.2);
    ASSERT_NE(e, nullptr);
    EXPECT_EQ(f.epochAt(kT0 + 1.0), nullptr);
    const auto& a = e->at("AAAA00XXX");
    EXPECT_TRUE(a.at("G01").integer);    // held
    EXPECT_FALSE(a.at("G02").integer);   // refused
    EXPECT_TRUE(a.at("G03").integer);    // pinned on two bands
    EXPECT_FALSE(a.at("G04").integer);   // pinned on one band
    EXPECT_FALSE(a.at("G05").integer);   // rows without a pin target
    EXPECT_TRUE(e->at("BBBB00XXX").at("G01").integer);   // decided
    EXPECT_DOUBLE_EQ(a.at("G02").iono_m, 1.2);
    EXPECT_EQ(f.counts().rows_integer, 3u);
    std::array<double, 3> p{};
    EXPECT_TRUE(f.position("AAAA00XXX", p));
    EXPECT_DOUBLE_EQ(p[2], 4801568.0);
    EXPECT_NE(f.runStartLine().find("3 with network integers"), std::string::npos);
}

TEST(NetworkIonoFile, RefusesMissingTraceOrReceiver) {
    const fs::path dir = writeTraces("refuse");
    std::string error;
    NetworkIonoFile::read(dir.string(), { "DDDD00XXX" }, error);
    EXPECT_NE(error.find("DDDD00XXX"), std::string::npos);
    fs::remove(dir / "network_chain_edges.csv");
    NetworkIonoFile::read(dir.string(), { "AAAA00XXX" }, error);
    EXPECT_NE(error.find("network_chain_edges.csv"), std::string::npos);
}

TEST(NetworkIonoConstraint, SigmaTableLookup) {
    const IonoFileSettings s = settingsOf(IonoFileCombine::Single, { "A" });
    EXPECT_DOUBLE_EQ(s.sigma(14.9), 0.01);   // below the first edge: the first bin (the cut is at the network)
    EXPECT_DOUBLE_EQ(s.sigma(15.0), 0.01);
    EXPECT_DOUBLE_EQ(s.sigma(29.9), 0.01);
    EXPECT_DOUBLE_EQ(s.sigma(30.0), 0.005);
    EXPECT_DOUBLE_EQ(s.sigma(85.0), 0.005);
    IonoFileSettings t = s;
    t.sigma_by_system['E'] = { 0.002, 0.0013 };
    EXPECT_DOUBLE_EQ(t.sigma(35.0, 'E'), 0.0013);
    EXPECT_DOUBLE_EQ(t.sigma(20.0, 'E'), 0.002);
    EXPECT_DOUBLE_EQ(t.sigma(35.0, 'G'), 0.005);   // no own table: the common one
}

TEST(NetworkIonoConstraint, PairCancelsReceiverTermAndNamesFailures) {
    const IonoFileSettings s = settingsOf(IonoFileCombine::Pair, { "A", "B" });
    NetworkIonoEpoch e;
    const std::vector<double> truth = { 2.0, 2.3, 1.7, 2.9, 3.1, 2.5, 2.2 };
    for (int i = 0; i < 7; ++i) {
        const std::string sat = "G0" + std::to_string(i + 1);
        e["A"][sat] = link(truth[i] + 0.8);    // receiver term +0.8
        e["B"][sat] = link(truth[i] - 0.3);    // receiver term -0.3
    }
    e["B"]["G04"].iono_m += 0.10;              // disagreement, 20 sigma at 45 degrees
    e["B"]["G05"].integer = false;             // not integer at B
    e["B"].erase("G06");                       // missing at B
    e["A"]["G08"] = link(2.0);                 // below the 30 degree cut at B
    e["B"]["G08"] = link(0.9);
    e["B"]["G08"].elevation_deg = 29.5;
    std::vector<IonoFileUserSat> sats;
    for (int i = 0; i < 8; ++i) sats.push_back({ "G0" + std::to_string(i + 1), 45.0 });
    const auto c = ionoFileCorrections(s, &e, sats, {});
    EXPECT_EQ(corrOf(c, "G08").cause, IonoFileCause::LowElevation);
    EXPECT_NEAR(corrOf(c, "G01").elevation_net_deg, 45.0, 1e-12);
    EXPECT_EQ(corrOf(c, "G04").cause, IonoFileCause::Disagree);
    EXPECT_EQ(corrOf(c, "G05").cause, IonoFileCause::NotInteger);
    EXPECT_EQ(corrOf(c, "G06").cause, IonoFileCause::Missing);
    for (const char* sat : { "G01", "G02", "G03", "G07" }) EXPECT_TRUE(corrOf(c, sat).valid) << sat;
    // Between-satellite differences equal the truth's: the receiver terms cancel.
    EXPECT_NEAR(corrOf(c, "G02").value_m - corrOf(c, "G01").value_m, 0.3, 1e-12);
    EXPECT_NEAR(corrOf(c, "G07").value_m - corrOf(c, "G03").value_m, 0.5, 1e-12);
    EXPECT_DOUBLE_EQ(corrOf(c, "G01").sigma_m, 0.005);
}

TEST(NetworkIonoConstraint, PairNeedsThreeSatellites) {
    const IonoFileSettings s = settingsOf(IonoFileCombine::Pair, { "A", "B" });
    NetworkIonoEpoch e;
    e["A"]["G01"] = link(1.0);
    e["B"]["G01"] = link(0.5);
    e["A"]["G02"] = link(1.2);
    e["B"]["G02"] = link(0.7);
    const auto c = ionoFileCorrections(s, &e, { { "G01", 50.0 }, { "G02", 50.0 } }, {});
    EXPECT_EQ(corrOf(c, "G01").cause, IonoFileCause::TooFew);
    EXPECT_EQ(corrOf(c, "G02").cause, IonoFileCause::TooFew);
}

TEST(NetworkIonoConstraint, PlaneRecoversLinearField) {
    const std::vector<std::string> rx = { "R1", "R2", "R3", "R4", "R5" };
    const IonoFileSettings s = settingsOf(IonoFileCombine::Plane, rx);
    const IonoFileOffsets off = { { "R1", { 160.0, 60.0 } },  { "R2", { -40.0, 250.0 } }, { "R3", { 90.0, -280.0 } },
                                  { "R4", { -300.0, 30.0 } }, { "R5", { 200.0, 300.0 } } };
    const std::map<std::string, std::array<double, 3>> field = {   // a + b east + c north per satellite
        { "E01", { 1.5, 0.002, -0.001 } }, { "E02", { 2.5, -0.001, 0.003 } }, { "E03", { 0.9, 0.0005, 0.0 } } };
    NetworkIonoEpoch e;
    double term = 0.3;
    for (const auto& r : rx) {
        term = -term + 0.11;   // a different receiver term at each receiver
        for (const auto& [sat, f] : field)
            e[r][sat] = link(term + f[0] + f[1] * off.at(r)[0] + f[2] * off.at(r)[1]);
    }
    const auto c = ionoFileCorrections(s, &e, { { "E01", 60.0 }, { "E02", 35.0 }, { "E03", 25.0 } }, off);
    for (const char* sat : { "E01", "E02", "E03" }) ASSERT_TRUE(corrOf(c, sat).valid) << sat;
    EXPECT_NEAR(corrOf(c, "E02").value_m - corrOf(c, "E01").value_m, 1.0, 1e-9);
    EXPECT_NEAR(corrOf(c, "E03").value_m - corrOf(c, "E01").value_m, -0.6, 1e-9);
    EXPECT_DOUBLE_EQ(corrOf(c, "E01").value_m, 0.0);   // the reference (all receivers, highest)
    // Links below the cut stay out of the fit: with two receivers low for E02 it still has three, too few.
    NetworkIonoEpoch low = e;
    low["R1"]["E02"].elevation_deg = 25.0;
    low["R2"]["E02"].elevation_deg = 28.0;
    const auto c3 = ionoFileCorrections(s, &low, { { "E01", 60.0 }, { "E02", 35.0 }, { "E03", 25.0 } }, off);
    EXPECT_EQ(corrOf(c3, "E02").cause, IonoFileCause::LowElevation);
    EXPECT_TRUE(corrOf(c3, "E03").valid);
    EXPECT_NEAR(corrOf(c3, "E03").value_m - corrOf(c3, "E01").value_m, -0.6, 1e-9);
    // Three receivers are not enough.
    NetworkIonoEpoch few = e;
    few.erase("R4");
    few.erase("R5");
    const auto c2 = ionoFileCorrections(s, &few, { { "E01", 60.0 }, { "E02", 35.0 } }, off);
    EXPECT_EQ(corrOf(c2, "E02").cause, IonoFileCause::TooFew);
}

TEST(NetworkIonoConstraint, SingleMissingEpochAndLowSatellite) {
    const IonoFileSettings s = settingsOf(IonoFileCombine::Single, { "A" });
    NetworkIonoEpoch e;
    e["A"]["G01"] = link(1.0);
    e["A"]["G02"] = link(1.1, false);
    e["A"]["G04"] = link(1.3);
    e["A"]["G04"].elevation_deg = 22.0;   // below the cut at the receiver, although the user sees it at 31 degrees
    const std::vector<IonoFileUserSat> sats = { { "G01", 50.0 }, { "G02", 50.0 }, { "G03", 50.0 }, { "G04", 31.0 } };
    const auto c = ionoFileCorrections(s, &e, sats, {});
    EXPECT_TRUE(corrOf(c, "G01").valid);
    EXPECT_DOUBLE_EQ(corrOf(c, "G01").value_m, 1.0);
    EXPECT_EQ(corrOf(c, "G02").cause, IonoFileCause::NotInteger);
    EXPECT_EQ(corrOf(c, "G03").cause, IonoFileCause::Missing);
    EXPECT_EQ(corrOf(c, "G04").cause, IonoFileCause::LowElevation);
    const auto none = ionoFileCorrections(s, nullptr, sats, {});
    EXPECT_EQ(corrOf(none, "G01").cause, IonoFileCause::Missing);
    EXPECT_EQ(corrOf(none, "G04").cause, IonoFileCause::Missing);
}

TEST(NetworkIonoConstraint, PivotNeverEntersARowAndHandOverIsCounted) {
    NetworkIonoConstraint owner;
    owner.setSettings(settingsOf(IonoFileCombine::Pair, { "A", "B" }));   // return after 3 passing epochs
    const std::map<std::string, double> v = { { "G01", 5.0 }, { "G02", 5.2 }, { "G03", 4.9 }, { "G04", 5.4 } };
    // G02 the pivot: its frozen state is a leftover (here 0.17 m off), the others are slant minus the pivot's.
    std::map<std::string, double> x = { { "G01", -0.2 }, { "G02", 0.17 }, { "G03", -0.3 }, { "G04", 0.2 } };
    IonoFileDecision d = decideAll(owner, v, x, 1e-6, "G02");
    ASSERT_EQ(d.rows.size(), 1u);
    EXPECT_NE(d.rows[0].reference, "G02");
    for (const auto& s : d.rows[0].sats) EXPECT_NE(s, "G02");
    EXPECT_EQ(d.rows[0].sats.size(), 2u);                 // three non-pivot satellites, two rows
    EXPECT_TRUE(d.refloat.empty());                        // the pivot's leftover is not tested
    EXPECT_EQ(owner.counters().at('G').first_uses, 3);
    // Hand-over to G01: G01 (active) is released as the pivot, G02 (re-based) returns only after 3 passing epochs.
    x = { { "G01", 0.0 }, { "G02", 0.2 }, { "G03", -0.1 }, { "G04", 0.4 } };
    d = decideAll(owner, v, x, 1e-6, "G01");
    EXPECT_EQ(owner.counters().at('G').pivot_handovers, 1);
    EXPECT_EQ(owner.counters().at('G').releases[static_cast<std::size_t>(IonoFileCause::Pivot)], 1);
    ASSERT_EQ(d.rows.size(), 1u);
    EXPECT_EQ(d.rows[0].sats.size(), 1u);                 // G03 and G04 only
    d = decideAll(owner, v, x, 1e-6, "G01");              // G02's second passing epoch (the hand-over was the first)
    EXPECT_EQ(d.rows[0].sats.size(), 1u);
    d = decideAll(owner, v, x, 1e-6, "G01");              // the third: G02 is back after its validated return
    EXPECT_EQ(d.rows[0].sats.size(), 2u);
    EXPECT_NE(owner.runEndLine().find("pivot hand-overs 1"), std::string::npos);
}

TEST(NetworkIonoConstraint, TestIsCentredOnHeldSatellites) {
    NetworkIonoConstraint owner;
    owner.setSettings(settingsOf(IonoFileCombine::Pair, { "A", "B" }));
    // Two held satellites agree (u = 5.0); three whose integers are not held sit 30 to 40 cm off with loose states.
    const std::map<std::string, std::pair<double, double>> sat = {   // sat -> (u = c - x, state variance)
        { "G01", { 5.00, 1e-6 } }, { "G02", { 5.00, 1e-6 } }, { "G03", { 5.30, 0.04 } }, { "G04", { 5.35, 0.04 } },
        { "G05", { 5.40, 0.04 } } };
    std::vector<IonoFileCorrection> corr;
    std::map<std::string, IonoFileUserState> state;
    std::map<std::string, double> elev;
    for (const auto& [s, uv] : sat) {
        IonoFileCorrection c;
        c.sat = s;
        c.valid = true;
        c.value_m = uv.first;
        c.sigma_m = 0.003;
        corr.push_back(c);
        IonoFileUserState st;
        st.var_m2 = uv.second;
        st.held = uv.second < 1e-3;
        state[s] = st;
        elev[s] = 45.0;
    }
    const IonoFileDecision d = owner.decide(corr, state, elev);
    EXPECT_TRUE(d.refloat.empty());          // the median of all five (5.30) would have failed both held satellites
    ASSERT_EQ(d.rows.size(), 1u);
    EXPECT_EQ(d.rows[0].sats.size(), 4u);
}

TEST(NetworkIonoConstraint, ReleaseWaitsForValidatedReturn) {
    NetworkIonoConstraint owner;
    owner.setSettings(settingsOf(IonoFileCombine::Pair, { "A", "B" }));   // return after 3 passing epochs
    const std::map<std::string, double> v = { { "G01", 5.0 }, { "G02", 5.2 }, { "G03", 4.9 }, { "G04", 5.5 } };
    const std::map<std::string, double> x = { { "G01", 0.0 }, { "G02", 0.2 }, { "G03", -0.1 }, { "G04", 0.5 } };
    decideAll(owner, v, x, 1e-6, "G01");
    IonoFileDecision d = decideAll(owner, v, x, 1e-6, "G01", { "G04" });   // G04 not integer
    ASSERT_EQ(d.events.size(), 1u);
    EXPECT_NE(d.events[0].find("release G04 not integer"), std::string::npos);
    EXPECT_TRUE(d.refloat.empty());
    EXPECT_EQ(d.rows[0].sats.size(), 1u);                  // G02 and G03 (G01 is the pivot)
    for (int k = 1; k <= 2; ++k) {
        d = decideAll(owner, v, x, 1e-6, "G01");
        EXPECT_EQ(d.rows[0].sats.size(), 1u) << "returning, epoch " << k;
    }
    d = decideAll(owner, v, x, 1e-6, "G01");
    EXPECT_EQ(d.rows[0].sats.size(), 2u);
    const IonoFileCounters& c = owner.counters().at('G');
    EXPECT_EQ(c.returns_validated, 1);
    EXPECT_EQ(c.releases[static_cast<std::size_t>(IonoFileCause::NotInteger)], 1);
}

TEST(NetworkIonoConstraint, InnovationReleaseRefloatsAndInterruptedReturnIsCounted) {
    NetworkIonoConstraint owner;
    owner.setSettings(settingsOf(IonoFileCombine::Pair, { "A", "B" }));
    const std::map<std::string, double> v = { { "G01", 5.0 }, { "G02", 5.2 }, { "G03", 4.9 }, { "G04", 5.5 } };
    std::map<std::string, double> x = { { "G01", 0.0 }, { "G02", 0.2 }, { "G03", -0.1 }, { "G04", 0.5 } };
    decideAll(owner, v, x, 1e-6, "G01");
    x["G03"] += 0.10;                                   // the user's state and the correction now conflict
    IonoFileDecision d = decideAll(owner, v, x, 1e-6, "G01");
    ASSERT_EQ(d.refloat.size(), 1u);
    EXPECT_EQ(d.refloat[0], "G03");
    EXPECT_NE(d.events[0].find("release G03 innovation"), std::string::npos);
    x["G03"] -= 0.10;
    decideAll(owner, v, x, 1e-6, "G01");                // passes once
    d = decideAll(owner, v, x, 1e-6, "G01", { "G03" }); // then fails: the return is interrupted
    EXPECT_NE(d.events[0].find("return failed G03"), std::string::npos);
    const IonoFileCounters& c = owner.counters().at('G');
    EXPECT_EQ(c.refloats, 1);
    EXPECT_EQ(c.returns_failed, 1);
    EXPECT_EQ(c.releases[static_cast<std::size_t>(IonoFileCause::Innovation)], 1);
    EXPECT_NE(owner.runEndLine().find("innovation 1"), std::string::npos);
    EXPECT_NE(owner.runEndLine().find("refloats 1"), std::string::npos);
}

TEST(NetworkIonoConstraint, SteadyTermInsideTheTestIsCounted) {
    NetworkIonoConstraint owner;
    owner.setSettings(settingsOf(IonoFileCombine::Pair, { "A", "B" }));   // test_k 4
    const std::map<std::string, double> v = { { "G01", 5.0 }, { "G02", 5.2 }, { "G03", 4.9 }, { "G04", 5.5 } };
    // G04 carries a steady 1.6 cm term: about 2.5 test sigma (sigma_s 5 mm, state 1 mm, centre about 3.7 mm).
    std::map<std::string, double> x = { { "G01", 0.0 }, { "G02", 0.2 }, { "G03", -0.1 }, { "G04", 0.484 } };
    for (int k = 0; k < 3; ++k) {
        const IonoFileDecision d = decideAll(owner, v, x, 1e-6, "G01");
        EXPECT_TRUE(d.refloat.empty());                     // inside the 4-sigma test: never released
    }
    const IonoFileCounters& c = owner.counters().at('G');
    EXPECT_EQ(c.beyond2, 3);
    EXPECT_EQ(c.beyond3, 0);
    EXPECT_NE(owner.runEndLine().find("G04 3/0"), std::string::npos);
}

TEST(NetworkIonoConstraint, VanishedSatelliteIsReleased) {
    NetworkIonoConstraint owner;
    owner.setSettings(settingsOf(IonoFileCombine::Pair, { "A", "B" }));
    std::map<std::string, double> v = { { "G01", 5.0 }, { "G02", 5.2 }, { "G03", 4.9 } };
    std::map<std::string, double> x = { { "G01", 0.0 }, { "G02", 0.2 }, { "G03", -0.1 } };
    decideAll(owner, v, x, 1e-6, "G01");
    v.erase("G03");
    x.erase("G03");
    const IonoFileDecision d = decideAll(owner, v, x, 1e-6, "G01");
    ASSERT_EQ(d.events.size(), 1u);
    EXPECT_NE(d.events[0].find("release G03 missing"), std::string::npos);
    EXPECT_EQ(owner.counters().at('G').releases[static_cast<std::size_t>(IonoFileCause::Missing)], 1);
}
