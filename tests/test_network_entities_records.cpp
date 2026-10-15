/**
 * @file test_network_entities_records.cpp
 * @brief Card NOM step N4: the snapshot records of the entity tables, one round trip per
 *        Record row (scripts/nom3_designs/N4_design.md section 5, identity check 3).
 *
 * Each test fills a table with two present rows and one row where the field is absent,
 * calls the row's to_snapshot (through recordToSnapshot, the writer's call), serializes the
 * snapshot, parses it, calls from_snapshot (recordFromSnapshot, the reader's call) on a
 * second context, and compares field by field.  Then the declarations: every Record row
 * has both fill functions, its tag is unique, and kRestoreOrder names each Record tag once.
 */
#include <gtest/gtest.h>

#include <limits>
#include <set>
#include <sstream>
#include <string>

#include "app/NetworkEntities.hpp"
#include "app/NetworkRunContext.hpp"
#include "config/GnssConfig.hpp"
#include "products/NetworkStateSnapshot.hpp"

using namespace GnssProcessor;

namespace {

using S  = net::StationState;
using SS = net::StationSignalState;
using C  = net::ConstellationState;
using Ch = net::ChannelState;
using L  = net::LosState;
using G  = net::SatSignalState;
using A  = net::SatelliteState;

/// A run context on the default configuration (the record functions read its knobs).
struct Ctx {
    GnssConfig cfg;
    NetworkRunContext ctx{cfg, cfg.getNetworkConfig()};
};

/// The record @p tag of @p from, written, serialized, parsed and restored into @p to.
void roundTrip(const char* tag, const NetworkRunContext& from, NetworkRunContext& to) {
    NetworkStateSnapshot snap;
    snap.snap_week = 2313;
    snap.snap_tow  = 279150.0;
    snap.n_emitted = 665;
    snap.config_fingerprint = 0xc5b5df834d185ac8ULL;
    net::recordToSnapshot(tag, from, snap);
    std::ostringstream os;
    NetworkStateSnapshotIO::serialize(os, snap);
    std::istringstream is(os.str());
    NetworkStateSnapshot back;
    std::string err;
    ASSERT_TRUE(NetworkStateSnapshotIO::parse(is, back, &err)) << err;
    net::recordFromSnapshot(tag, back, to);
}

void expectSameCarry(const LocalCarry* a, const LocalCarry* b) {
    ASSERT_NE(a, nullptr);
    ASSERT_NE(b, nullptr);
    EXPECT_EQ(a->value_abs, b->value_abs);
    EXPECT_EQ(a->var, b->var);
    EXPECT_EQ(a->last_seen_s, b->last_seen_s);
}

}  // namespace

TEST(NetworkEntitiesRecords, RecordRoundTripZ) {
    Ctx a, b;
    net::StationTable& t = a.ctx.entities.stations;
    net::put(t, StationId{0}, &S::zwd) = LocalCarry{0.12345, 1e-5, 1399161660.0};
    net::put(t, StationId{4}, &S::zwd) = LocalCarry{0.2468, 2e-5, 1399161690.0};
    net::mark(t, StationId{2}, &S::staf_departed);   // a row without the field
    // A stale value on the reader's side is replaced, not merged ("Z" clears first).
    net::put(b.ctx.entities.stations, StationId{3}, &S::zwd) = LocalCarry{9.0, 1.0, 1.0};
    roundTrip("Z", a.ctx, b.ctx);
    const net::StationTable& r = b.ctx.entities.stations;
    EXPECT_EQ(net::present(r, &S::zwd), 2u);
    expectSameCarry(net::get(t, StationId{0}, &S::zwd), net::get(r, StationId{0}, &S::zwd));
    expectSameCarry(net::get(t, StationId{4}, &S::zwd), net::get(r, StationId{4}, &S::zwd));
    EXPECT_EQ(net::get(r, StationId{2}, &S::zwd), nullptr);
    EXPECT_EQ(net::get(r, StationId{3}, &S::zwd), nullptr);
}

TEST(NetworkEntitiesRecords, RecordRoundTripEQP) {
    Ctx a, b;
    const std::vector<std::string> roster{"WTZR00DEU", "GOPE00CZE", "ONSA00SWE"};
    a.ctx.entities.names.assign(roster);
    b.ctx.entities.names.assign(roster);
    net::StationTable& t = a.ctx.entities.stations;
    net::put(t, StationId{0}, &S::signals, std::string("G|L2W"), &SS::eq34_rx_parent) = "G07";
    net::put(t, StationId{1}, &S::signals, std::string("G|L1C"), &SS::eq34_rx_parent) = "G24";
    // A cell of the same row without the field (another member's key space).
    net::put(t, StationId{2}, &S::signals, std::string("GPS_DCB"), &SS::rx_cb) = LocalCarry{1.0, 1e-4, 1.0};
    NetworkStateSnapshot snap;
    net::recordToSnapshot("EQP", a.ctx, snap);
    ASSERT_EQ(snap.eq34_rx_parent.size(), 2u);
    EXPECT_EQ(snap.eq34_rx_parent.at("GOPE00CZE|G|L1C"), "G24");   // the pre-N4a key shape
    EXPECT_EQ(snap.eq34_rx_parent.at("WTZR00DEU|G|L2W"), "G07");
    roundTrip("EQP", a.ctx, b.ctx);
    const net::StationTable& r = b.ctx.entities.stations;
    EXPECT_EQ(net::present(r, &S::signals, &SS::eq34_rx_parent), 2u);
    EXPECT_EQ(*net::get(r, StationId{0}, &S::signals, std::string("G|L2W"), &SS::eq34_rx_parent), "G07");
    EXPECT_EQ(*net::get(r, StationId{1}, &S::signals, std::string("G|L1C"), &SS::eq34_rx_parent), "G24");
    EXPECT_EQ(net::get(r, StationId{2}, &S::signals, std::string("GPS_DCB"), &SS::eq34_rx_parent), nullptr);
    EXPECT_EQ(b.ctx.entities.reach.failures[static_cast<std::size_t>(net::Converter::StationName)], 0);
    // A name that is not on the roster is dropped and counted (the converter's round trip).
    snap.eq34_rx_parent["XXXX00XXX|G|L1C"] = "G01";
    net::recordFromSnapshot("EQP", snap, b.ctx);
    EXPECT_EQ(b.ctx.entities.reach.failures[static_cast<std::size_t>(net::Converter::StationName)], 1);
    EXPECT_EQ(net::present(r, &S::signals, &SS::eq34_rx_parent), 2u);
}

TEST(NetworkEntitiesRecords, RecordRoundTripRC) {
    Ctx a, b;
    net::StationTable& t = a.ctx.entities.stations;
    net::put(t, StationId{0}, &S::signals, std::string("GPS_DCB"), &SS::rx_cb) =
        LocalCarry{-3.8391234567890123, 2.5e-3, 1399161660.0};
    net::put(t, StationId{7}, &S::signals, std::string("GPS_IFB5"), &SS::rx_cb) =
        LocalCarry{0.0625, 1e-4, 1399161690.0};
    net::put(t, StationId{7}, &S::signals, std::string(""), &SS::rx_clk) = LocalCarry{1.0, 1.0, 1.0};
    // The non-finite screen at the source: this entry is not written.
    net::put(t, StationId{3}, &S::signals, std::string("GPS_DCB"), &SS::rx_cb) =
        LocalCarry{std::numeric_limits<double>::quiet_NaN(), 1e-4, 1.0};
    roundTrip("RC", a.ctx, b.ctx);
    const net::StationTable& r = b.ctx.entities.stations;
    EXPECT_EQ(net::present(r, &S::signals, &SS::rx_cb), 2u);
    expectSameCarry(net::get(t, StationId{0}, &S::signals, std::string("GPS_DCB"), &SS::rx_cb),
                    net::get(r, StationId{0}, &S::signals, std::string("GPS_DCB"), &SS::rx_cb));
    expectSameCarry(net::get(t, StationId{7}, &S::signals, std::string("GPS_IFB5"), &SS::rx_cb),
                    net::get(r, StationId{7}, &S::signals, std::string("GPS_IFB5"), &SS::rx_cb));
    EXPECT_EQ(net::get(r, StationId{3}, &S::signals, std::string("GPS_DCB"), &SS::rx_cb), nullptr);
    EXPECT_EQ(net::get(r, StationId{7}, &S::signals, std::string(""), &SS::rx_clk), nullptr);
}

TEST(NetworkEntitiesRecords, RecordRoundTripV) {
    Ctx a, b;
    net::ConstellationTable& t = a.ctx.entities.constellations;
    net::put(t, System::GPS, &C::prev_pivot) = "G05";
    net::put(t, System::Galileo, &C::prev_pivot) = "E11";
    net::put(t, System::GPS, &C::last_pivot) = "G05";
    net::put(t, System::Galileo, &C::last_pivot) = "E11";
    net::put(t, System::GLONASS, &C::pfq_pivot) = "R08";   // a row without the pivots
    net::put(b.ctx.entities.constellations, System::BeiDou, &C::prev_pivot) = "C19";
    NetworkStateSnapshot snap;
    net::recordToSnapshot("V", a.ctx, snap);   // one tag, both pivot rows
    EXPECT_EQ(snap.prev_pivot, (std::map<std::string, std::string>{{"E", "E11"}, {"G", "G05"}}));
    EXPECT_EQ(snap.last_pivot, snap.prev_pivot);
    roundTrip("V", a.ctx, b.ctx);
    const net::ConstellationTable& r = b.ctx.entities.constellations;
    EXPECT_EQ(net::present(r, &C::prev_pivot), 2u);
    EXPECT_EQ(net::present(r, &C::last_pivot), 2u);
    EXPECT_EQ(*net::get(r, System::GPS, &C::prev_pivot), "G05");
    EXPECT_EQ(*net::get(r, System::Galileo, &C::last_pivot), "E11");
    EXPECT_EQ(net::get(r, System::GLONASS, &C::prev_pivot), nullptr);
    EXPECT_EQ(net::get(r, System::BeiDou, &C::prev_pivot), nullptr);
    // Letter order (O5): the System enum puts GLONASS before Galileo, the table does not.
    std::string order;
    net::forEach(r, &C::prev_pivot, [&](System s, const std::string&) { order += rinexLetter(s); });
    EXPECT_EQ(order, "EG");
}

TEST(NetworkEntitiesRecords, RecordRoundTripGG) {
    Ctx a, b;
    net::ConstellationTable& t = a.ctx.entities.constellations;
    net::put(t, System::GPS, &C::channels, std::string("L2W"), &Ch::group_id) = 1;
    net::put(t, System::Galileo, &C::channels, std::string("L5Q"), &Ch::group_id) = 3;
    net::put(t, System::GPS, &C::channels, std::string("L1C"), &Ch::group_id) = 2;
    net::put(t, System::GPS, &C::channels, std::string("L1C"), &Ch::datum_rx) = "WTZR00DEU";
    net::put(t, System::GPS, &C::channels, std::string("L5Q"), &Ch::datum_rx) = "WTZR00DEU";
    NetworkStateSnapshot snap;
    net::recordToSnapshot("GG", a.ctx, snap);
    // The fill order of the vector record is (letter, signal) (O6), as the old map's was.
    ASSERT_EQ(snap.carry_group_id.size(), 3u);
    EXPECT_EQ(snap.carry_group_id[0], std::make_tuple('E', std::string("L5Q"), 3));
    EXPECT_EQ(snap.carry_group_id[1], std::make_tuple('G', std::string("L1C"), 2));
    EXPECT_EQ(snap.carry_group_id[2], std::make_tuple('G', std::string("L2W"), 1));
    // The GG rows serialize only beside the invariant-carry blob (network.state_snapshot_carry),
    // so the record goes straight from the writer's DTO to the reader.
    net::recordFromSnapshot("GG", snap, b.ctx);
    const net::ConstellationTable& r = b.ctx.entities.constellations;
    EXPECT_EQ(net::present(r, &C::channels, &Ch::group_id), 3u);
    EXPECT_EQ(*net::get(r, System::GPS, &C::channels, std::string("L2W"), &Ch::group_id), 1);
    EXPECT_EQ(*net::get(r, System::Galileo, &C::channels, std::string("L5Q"), &Ch::group_id), 3);
    EXPECT_EQ(net::get(r, System::GPS, &C::channels, std::string("L5Q"), &Ch::group_id), nullptr);
}

TEST(NetworkEntitiesRecords, RecordRoundTripCBP) {
    Ctx a, b;
    net::ConstellationTable& t = a.ctx.entities.constellations;
    net::put(t, System::GPS, &C::channels, std::string("C1C"), &Ch::code_bias_prev) =
        {{"G01", -1.2345678901234567}, {"G07", std::numeric_limits<double>::quiet_NaN()}};
    net::put(t, System::Galileo, &C::channels, std::string("C5Q"), &Ch::code_bias_prev) = {{"E03", 0.5}};
    net::put(t, System::GPS, &C::channels, std::string("C2W"), &Ch::group_id) = 4;
    NetworkStateSnapshot snap;
    net::recordToSnapshot("CBP", a.ctx, snap);
    ASSERT_EQ(snap.code_bias_prev.size(), 2u);
    EXPECT_EQ(snap.code_bias_prev.at("G|C1C").count("G07"), 0u);   // non-finite dropped at the source
    roundTrip("CBP", a.ctx, b.ctx);
    const net::ConstellationTable& r = b.ctx.entities.constellations;
    EXPECT_EQ(net::present(r, &C::channels, &Ch::code_bias_prev), 2u);
    EXPECT_EQ(net::get(r, System::GPS, &C::channels, std::string("C1C"), &Ch::code_bias_prev)->at("G01"),
              -1.2345678901234567);
    EXPECT_EQ(net::get(r, System::Galileo, &C::channels, std::string("C5Q"), &Ch::code_bias_prev)->at("E03"), 0.5);
    EXPECT_EQ(net::get(r, System::GPS, &C::channels, std::string("C2W"), &Ch::code_bias_prev), nullptr);
}

TEST(NetworkEntitiesRecords, RecordRoundTripEQD) {
    Ctx a, b;
    net::ConstellationTable& t = a.ctx.entities.constellations;
    net::put(t, System::GPS, &C::channels, std::string("L1C"), &Ch::datum_rx) = "GOPE00CZE";
    net::put(t, System::GPS, &C::channels, std::string("L2W"), &Ch::datum_rx) = "WTZR00DEU";
    net::put(t, System::Galileo, &C::channels, std::string("L1C"), &Ch::group_id) = 1;
    NetworkStateSnapshot snap;
    net::recordToSnapshot("EQD", a.ctx, snap);
    EXPECT_EQ(snap.eq34_datum_rx.at("G|L1C"), "GOPE00CZE");   // the pre-N4b key shape
    roundTrip("EQD", a.ctx, b.ctx);
    const net::ConstellationTable& r = b.ctx.entities.constellations;
    EXPECT_EQ(net::present(r, &C::channels, &Ch::datum_rx), 2u);
    EXPECT_EQ(*net::get(r, System::GPS, &C::channels, std::string("L2W"), &Ch::datum_rx), "WTZR00DEU");
    EXPECT_EQ(net::get(r, System::Galileo, &C::channels, std::string("L1C"), &Ch::datum_rx), nullptr);
    EXPECT_EQ(*net::first(r, &C::channels, &Ch::datum_rx), "GOPE00CZE");   // the old begin()
    snap.eq34_datum_rx["GPS|L1C"] = "X";   // not a one-letter key: counted, dropped
    net::recordFromSnapshot("EQD", snap, b.ctx);
    EXPECT_EQ(b.ctx.entities.reach.failures[static_cast<std::size_t>(net::Converter::ConstellationLetter)], 1);
    EXPECT_EQ(net::present(r, &C::channels, &Ch::datum_rx), 2u);
}

TEST(NetworkEntitiesRecords, RecordRoundTripEQE) {
    Ctx a, b;
    net::put(a.ctx.entities.los, LosId{StationId{3}, SatId{System::GPS, 24}}, &L::eq34_prev_el) =
        61.284999999999997;
    net::put(a.ctx.entities.los, LosId{StationId{17}, SatId{System::GPS, 7}}, &L::eq34_prev_el) = 12.5;
    net::put(a.ctx.entities.los, LosId{StationId{3}, SatId{System::Galileo, 11}}, &L::pc) =
        LocalCarry{1.0, 1.0, 1.0};   // a row without the field
    NetworkStateSnapshot snap;
    net::recordToSnapshot("EQE", a.ctx, snap);
    EXPECT_EQ(snap.eq34_prev_el.at({3, "G24"}), 61.284999999999997);   // the pre-N4c key shape
    roundTrip("EQE", a.ctx, b.ctx);
    const net::LosTable& r = b.ctx.entities.los;
    EXPECT_EQ(net::present(r, &L::eq34_prev_el), 2u);
    EXPECT_EQ(*net::getLos(r, 17, "G07", &L::eq34_prev_el), 12.5);
    EXPECT_EQ(*net::getLos(r, 3, "G24", &L::eq34_prev_el), 61.284999999999997);
    EXPECT_EQ(net::getLos(r, 3, "E11", &L::eq34_prev_el), nullptr);
}

TEST(NetworkEntitiesRecords, RecordRoundTripSD) {
    Ctx a, b;
    const std::vector<std::string> roster{"WTZZ00DEU", "MAJU00MHL", "ABMF00GLP"};
    a.ctx.entities.names.assign(roster);
    b.ctx.entities.names.assign(roster);
    net::LosTable& t = a.ctx.entities.los;
    net::putLos(a.ctx.entities, 0, "G24", &L::sh_dev) = ShDevCarry{0.75, 0.04, 0.09, 1399161660.0};
    net::putLos(a.ctx.entities, 1, "E11", &L::sh_dev) = ShDevCarry{-0.5, 0.02, 0.09, 1399161690.0};
    net::putLos(a.ctx.entities, 2, "G05", &L::sh_dev) =
        ShDevCarry{std::numeric_limits<double>::quiet_NaN(), 0.02, 0.09, 1.0};   // dropped
    NetworkStateSnapshot snap;
    net::recordToSnapshot("SD", a.ctx, snap);
    ASSERT_EQ(snap.sh_dev_carry.size(), 2u);
    EXPECT_EQ(snap.sh_dev_carry.begin()->first, "MAJU00MHL|E11");   // name order (O3)
    roundTrip("SD", a.ctx, b.ctx);
    const net::LosTable& r = b.ctx.entities.los;
    EXPECT_EQ(net::present(r, &L::sh_dev), 2u);
    const ShDevCarry* d = net::getLos(r, 0, "G24", &L::sh_dev);
    ASSERT_NE(d, nullptr);
    EXPECT_EQ(d->dev, 0.75);
    EXPECT_EQ(d->var, 0.04);
    EXPECT_EQ(d->var_ss, 0.09);
    EXPECT_EQ(d->t_s, 1399161660.0);
    EXPECT_EQ(net::getLos(r, 2, "G05", &L::sh_dev), nullptr);
    EXPECT_NE(net::getLos(t, 2, "G05", &L::sh_dev), nullptr);
}

TEST(NetworkEntitiesRecords, LosKeysRoundTripInTheOldOrder) {
    // O1 and O2: a (station index, satellite) key converts and back, and the table iterates
    // in the old pair order; "" is the no-satellite identity, and a non-canonical satellite
    // string does not round-trip (counted by the writer's converter).
    net::NetworkEntities e;
    const std::vector<std::pair<int, std::string>> keys{
        {0, ""}, {0, "C19"}, {0, "E05"}, {0, "G05"}, {0, "G10"}, {1, "G01"}, {12, "R08"}};
    for (auto it = keys.rbegin(); it != keys.rend(); ++it)
        net::putLos(e, it->first, it->second, &L::arc_spike_streak) = 1;
    std::vector<std::pair<int, std::string>> walked;
    net::forEach(e.los, &L::arc_spike_streak,
                 [&](const LosId& id, int) { walked.push_back(net::losPair(id)); });
    EXPECT_EQ(walked, keys);
    EXPECT_FALSE(net::losKey(0, "G5").has_value());
    EXPECT_FALSE(net::losOf(e, 0, "XYZ").has_value());
    EXPECT_EQ(e.reach.failures[static_cast<std::size_t>(net::Converter::Satellite)], 1);
    EXPECT_EQ(net::present(e.los, &L::arc_spike_streak), keys.size());
}

TEST(NetworkEntitiesRecords, RecordRoundTripPA) {
    Ctx a, b;
    net::putSig(a.ctx.entities, "G24_L1C", &G::anchor_incumbent) = "WTZZ00DEU";
    net::putSig(a.ctx.entities, "E11_L5Q", &G::anchor_incumbent) = "MAJU00MHL";
    net::putSig(a.ctx.entities, "G05_L2W", &G::pb_level_prev) = 0.25;   // a row without the field
    NetworkStateSnapshot snap;
    net::recordToSnapshot("PA", a.ctx, snap);
    ASSERT_EQ(snap.anchor_incumbent.size(), 2u);
    EXPECT_EQ(snap.anchor_incumbent.begin()->first, "E11_L5Q");   // the pre-N4d key shape
    roundTrip("PA", a.ctx, b.ctx);
    const net::SatSignalTable& r = b.ctx.entities.sat_signals;
    EXPECT_EQ(net::present(r, &G::anchor_incumbent), 2u);
    EXPECT_EQ(*net::getSig(r, "G24_L1C", &G::anchor_incumbent), "WTZZ00DEU");
    EXPECT_EQ(*net::getSig(r, "E11_L5Q", &G::anchor_incumbent), "MAJU00MHL");
    EXPECT_EQ(net::getSig(r, "G05_L2W", &G::anchor_incumbent), nullptr);
}

TEST(NetworkEntitiesRecords, RecordRoundTripPT) {
    Ctx a, b;
    net::putSig(a.ctx.entities, "E02_L5Q", &G::anchor_pin_target) = -28.981234567;
    net::putSig(a.ctx.entities, "G24_L1C", &G::anchor_pin_target) = 0.0625;
    net::putSig(a.ctx.entities, "G05_L2W", &G::anchor_incumbent) = "ABMF00GLP";   // without the field
    roundTrip("PT", a.ctx, b.ctx);
    const net::SatSignalTable& r = b.ctx.entities.sat_signals;
    EXPECT_EQ(net::present(r, &G::anchor_pin_target), 2u);
    EXPECT_EQ(*net::getSig(r, "E02_L5Q", &G::anchor_pin_target), -28.981234567);
    EXPECT_EQ(*net::getSig(r, "G24_L1C", &G::anchor_pin_target), 0.0625);
    EXPECT_EQ(net::getSig(r, "G05_L2W", &G::anchor_pin_target), nullptr);
}

TEST(NetworkEntitiesRecords, SatSignalKeysRoundTripInTheOldOrder) {
    // O1 and O2: an ambiguity-form key and a hold-form key name the same satellite signal; the
    // table iterates in the string order of either form; "" is the satellite-wide wildcard,
    // and a code-type or non-canonical key does not round-trip (counted by the writer).
    net::NetworkEntities e;
    const std::vector<std::string> keys{"C19_L2I", "E05_L1C", "E05_L1X", "E05_L5Q", "G05_",
                                        "G05_L1C", "G05_L2W", "G10_L1C", "R08_L1C"};
    for (auto it = keys.rbegin(); it != keys.rend(); ++it)
        net::putSig(e, *it, &G::pb_level_prev) = 1.0;
    std::vector<std::string> walked;
    net::forEach(e.sat_signals, &G::pb_level_prev,
                 [&](const SatSignalId& id, double) { walked.push_back(net::ambKey(id)); });
    EXPECT_EQ(walked, keys);
    EXPECT_EQ(net::satSignalKey("G05|L1C"), net::satSignalKey("G05_L1C"));
    EXPECT_EQ(net::holdKey(*net::satSignalKey("G05_L1C")), "G05|L1C");
    EXPECT_FALSE(net::satSignalKey("G05_C1C").has_value());
    EXPECT_FALSE(net::satSignalKey("G5_L1C").has_value());
    EXPECT_FALSE(net::satSignalOf(e, "G05").has_value());
    EXPECT_EQ(e.reach.failures[static_cast<std::size_t>(net::Converter::SatSignal)], 1);
    EXPECT_EQ(net::present(e.sat_signals, &G::pb_level_prev), keys.size());
    EXPECT_TRUE(net::emplaceSig(e, "G24_L1C", &G::pb_level_prev, 2.0));
    EXPECT_FALSE(net::emplaceSig(e, "G24_L1C", &G::pb_level_prev, 3.0));
    EXPECT_EQ(*net::getSig(e.sat_signals, "G24_L1C", &G::pb_level_prev), 2.0);
}

TEST(NetworkEntitiesRecords, RecordRoundTripK) {
    Ctx a, b;
    ClockOsc o;
    o.x0 = 1.25; o.x1 = -3.5e-4; o.p00 = 0.04; o.p01 = 1.0e-6; o.p11 = 2.0e-9;
    o.t_last = 1399161660.0; o.init = true;
    net::putSat(a.ctx.entities, "G24", &A::clock_osc) = o;
    o.x0 = -0.5; o.init = false;
    net::putSat(a.ctx.entities, "E11", &A::clock_osc) = o;
    net::putSat(a.ctx.entities, "G05", &A::clm_info) = 2.0;   // a row without the field
    roundTrip("K", a.ctx, b.ctx);
    const net::SatTable& r = b.ctx.entities.sats;
    EXPECT_EQ(net::present(r, &A::clock_osc), 2u);
    const ClockOsc* g = net::getSat(r, "G24", &A::clock_osc);
    ASSERT_NE(g, nullptr);
    EXPECT_EQ(g->x0, 1.25);
    EXPECT_EQ(g->x1, -3.5e-4);
    EXPECT_EQ(g->p00, 0.04);
    EXPECT_EQ(g->p01, 1.0e-6);
    EXPECT_EQ(g->p11, 2.0e-9);
    EXPECT_EQ(g->t_last, 1399161660.0);
    EXPECT_TRUE(g->init);
    EXPECT_FALSE(net::getSat(r, "E11", &A::clock_osc)->init);
    EXPECT_EQ(net::getSat(r, "G05", &A::clock_osc), nullptr);
}

TEST(NetworkEntitiesRecords, RecordRoundTripJ) {
    Ctx a, b;
    net::putSat(a.ctx.entities, "G24", &A::prev_clk) = {0.125, 1399161660.0};
    net::putSat(a.ctx.entities, "C19", &A::prev_clk) = {-7.5, 1399161690.0};
    net::putSat(a.ctx.entities, "G05", &A::clm_n) = 3;   // a row without the field
    NetworkStateSnapshot snap;
    net::recordToSnapshot("J", a.ctx, snap);
    EXPECT_EQ(snap.prev_clk.begin()->first, "C19");   // the pre-N4e1 key shape and order
    roundTrip("J", a.ctx, b.ctx);
    const net::SatTable& r = b.ctx.entities.sats;
    EXPECT_EQ(net::present(r, &A::prev_clk), 2u);
    EXPECT_EQ(*net::getSat(r, "G24", &A::prev_clk), (std::pair<double, double>{0.125, 1399161660.0}));
    EXPECT_EQ(*net::getSat(r, "C19", &A::prev_clk), (std::pair<double, double>{-7.5, 1399161690.0}));
    EXPECT_EQ(net::getSat(r, "G05", &A::prev_clk), nullptr);
}

TEST(NetworkEntitiesRecords, RecordRoundTripY) {
    Ctx a, b;
    net::putSat(a.ctx.entities, "G24", &A::ura_refloat_horizon) = 7;
    net::putSat(a.ctx.entities, "E11", &A::ura_refloat_horizon) = 1;
    net::markSat(a.ctx.entities, "G05", &A::retro_active);   // a row without the field
    roundTrip("Y", a.ctx, b.ctx);
    const net::SatTable& r = b.ctx.entities.sats;
    EXPECT_EQ(net::present(r, &A::ura_refloat_horizon), 2u);
    EXPECT_EQ(*net::getSat(r, "G24", &A::ura_refloat_horizon), 7);
    EXPECT_EQ(*net::getSat(r, "E11", &A::ura_refloat_horizon), 1);
    EXPECT_EQ(net::getSat(r, "G05", &A::ura_refloat_horizon), nullptr);
}

/// Records C, R and A: a satellite set as a flag, written and restored wholesale.
void flagRoundTrip(const char* tag, bool net::SatelliteState::*f) {
    Ctx a, b;
    net::markSat(a.ctx.entities, "G24", f);
    net::markSat(a.ctx.entities, "E11", f);
    net::putSat(a.ctx.entities, "G05", &A::clm_info) = 2.0;   // a row without the flag
    net::markSat(b.ctx.entities, "G10", f);                    // replaced by the restore
    roundTrip(tag, a.ctx, b.ctx);
    const net::SatTable& r = b.ctx.entities.sats;
    EXPECT_EQ(net::satSet(r, f), (std::set<std::string>{"E11", "G24"})) << tag;
    EXPECT_FALSE(net::hasSat(r, "G05", f)) << tag;
}

TEST(NetworkEntitiesRecords, RecordRoundTripC) {
    flagRoundTrip("C", &A::c1_zoh_active);
}

TEST(NetworkEntitiesRecords, RecordRoundTripR) {
    flagRoundTrip("R", &A::ura_retro_sats);
}

TEST(NetworkEntitiesRecords, RecordRoundTripA) {
    flagRoundTrip("A", &A::retro_active);
}


TEST(NetworkEntitiesRecords, RecordRoundTripLB) {
    Ctx a, b;
    net::putSat(a.ctx.entities, "G24", &A::clm_info_c) = 400.0;
    net::putSat(a.ctx.entities, "G24", &A::clm_n) = 12;
    net::putSat(a.ctx.entities, "E11", &A::clm_info_c) = 25.0;   // no epoch count: written as 0
    net::putSat(a.ctx.entities, "G05", &A::clm_info_c) = 0.0;    // not positive: not written
    net::putSat(a.ctx.entities, "G07", &A::clm_n) = 4;           // no information: not written
    NetworkStateSnapshot snap;
    net::recordToSnapshot("LB", a.ctx, snap);
    ASSERT_EQ(snap.level_info.size(), 2u);
    EXPECT_EQ(snap.level_info.at("G24"), (std::pair<double, int>{400.0, 12}));
    EXPECT_EQ(snap.level_info.at("E11"), (std::pair<double, int>{25.0, 0}));
    roundTrip("LB", a.ctx, b.ctx);
    const net::SatTable& r = b.ctx.entities.sats;
    EXPECT_EQ(net::present(r, &A::clm_info_c), 2u);
    EXPECT_EQ(*net::getSat(r, "G24", &A::clm_info_c), 400.0);
    EXPECT_EQ(*net::getSat(r, "G24", &A::clm_n), 12);
    EXPECT_EQ(*net::getSat(r, "E11", &A::clm_n), 0);
    EXPECT_EQ(net::getSat(r, "G07", &A::clm_n), nullptr);
}

TEST(NetworkEntitiesRecords, SatelliteKeysRoundTripInTheOldOrder) {
    // O1 and O2: a satellite string converts and back, and the table iterates in the string
    // order; "" is the no-satellite SatId{} (its row is kept), and a non-canonical string
    // does not round-trip (counted by the writer's converter).
    net::NetworkEntities e;
    const std::vector<std::string> keys{"", "C19", "E05", "G05", "G10", "R08"};
    for (auto it = keys.rbegin(); it != keys.rend(); ++it)
        net::putSat(e, *it, &A::readmit_marg) = 1;
    std::vector<std::string> walked;
    net::forEach(e.sats, &A::readmit_marg,
                 [&](const SatId& id, long long) { walked.push_back(net::satKey(id)); });
    EXPECT_EQ(walked, keys);
    EXPECT_FALSE(net::satIdKey("G5").has_value());
    EXPECT_FALSE(net::satIdOf(e, "XYZ").has_value());
    EXPECT_EQ(e.reach.failures[static_cast<std::size_t>(net::Converter::Satellite)], 1);
    EXPECT_TRUE(net::markSat(e, "G05", &A::ura_retro_sats));
    EXPECT_FALSE(net::markSat(e, "G05", &A::ura_retro_sats));
    EXPECT_EQ(net::unmarkSat(e.sats, "G05", &A::ura_retro_sats), 1u);
    EXPECT_EQ(net::unmarkSat(e.sats, "G05", &A::ura_retro_sats), 0u);
}

TEST(NetworkEntitiesRecords, EveryRecordRowIsDeclaredOnceAndRestoredInOrder) {
    std::set<std::string> tags;
    std::string prev_tag;   // a tag spans adjacent rows of one table only (V: the two pivots)
    for (const net::EntityFields& t : net::kEntityFieldTables) {
        prev_tag.clear();
        for (const net::FieldDecl& d : *t.fields) {
            ASSERT_NE(d.rows, nullptr) << d.name;
            if (d.persist != net::Persist::Record) {
                EXPECT_EQ(d.to_snapshot, nullptr) << d.name;
                EXPECT_EQ(d.from_snapshot, nullptr) << d.name;
                prev_tag.clear();
                continue;
            }
            EXPECT_NE(d.to_snapshot, nullptr) << d.name;
            EXPECT_NE(d.from_snapshot, nullptr) << d.name;
            EXPECT_TRUE(tags.insert(d.record).second || prev_tag == d.record)
                << "tag declared twice: " << d.record;
            prev_tag = d.record;
        }
    }
    std::set<std::string> restored(net::kRestoreOrder.begin(), net::kRestoreOrder.end());
    EXPECT_EQ(restored, tags);
    EXPECT_EQ(restored.size(), net::kRestoreOrder.size());
    EXPECT_EQ(net::recordRow("NOPE"), nullptr);
}
