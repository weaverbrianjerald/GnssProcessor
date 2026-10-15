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

TEST(NetworkEntitiesRecords, EveryRecordRowIsDeclaredOnceAndRestoredInOrder) {
    std::set<std::string> tags;
    for (const net::EntityFields& t : net::kEntityFieldTables)
        for (const net::FieldDecl& d : *t.fields) {
            ASSERT_NE(d.rows, nullptr) << d.name;
            if (d.persist != net::Persist::Record) {
                EXPECT_EQ(d.to_snapshot, nullptr) << d.name;
                EXPECT_EQ(d.from_snapshot, nullptr) << d.name;
                continue;
            }
            EXPECT_NE(d.to_snapshot, nullptr) << d.name;
            EXPECT_NE(d.from_snapshot, nullptr) << d.name;
            EXPECT_TRUE(tags.insert(d.record).second) << "tag declared twice: " << d.record;
        }
    std::set<std::string> restored(net::kRestoreOrder.begin(), net::kRestoreOrder.end());
    EXPECT_EQ(restored, tags);
    EXPECT_EQ(restored.size(), net::kRestoreOrder.size());
    EXPECT_EQ(net::recordRow("NOPE"), nullptr);
}
