/**
 * @file test_consumer_sat_trace.cpp
 * @brief Card CST: pins the schema of `logs/closed_loop_sat_trace.csv`.
 *
 * The formatter is the one kernel of the card that is pure — no engine, no
 * store, no file — so it is the piece a unit test can hold still.  What is
 * guarded here is exactly what a downstream reader breaks on: the column ORDER
 * and COUNT, the "absent is BLANK, never 0" rule (an absent phase bias and a
 * zero phase bias are different wires and the whole instrument exists to tell
 * them apart), and the precision of each column group.
 *
 * Suite name owned by this file alone (suite-hygiene rule in CLAUDE.md).
 */

#include "realtime/ConsumerSatTrace.hpp"

#include <gtest/gtest.h>

#include <sstream>
#include <string>
#include <vector>

using namespace GnssProcessor;

namespace {

/// Split a CSV line on ',' KEEPING empty fields (the blanks are the point).
std::vector<std::string> splitCsv(const std::string& line) {
    std::vector<std::string> out;
    std::string cur;
    for (const char c : line) {
        if (c == ',') { out.push_back(cur); cur.clear(); }
        else if (c != '\n') { cur.push_back(c); }
    }
    out.push_back(cur);
    return out;
}

/// A fully-populated row — every optional present, so the column COUNT of a
/// data line can be compared against the header's.
ConsumerSatRow fullRow() {
    ConsumerSatRow r;
    r.sat = "G23"; r.sig = "L1W"; r.code_sig = "C1W";
    r.elev_deg = 41.25; r.n_sig = 2;
    r.have_clk = true; r.clk_m = -0.110000;
    r.have_cb  = true; r.cb_m  = 1.234567;
    r.have_pb  = true; r.pb_m  = -3.638000;
    r.have_clk_disc = true; r.clk_disc = 7;
    r.have_cb_disc  = true; r.cb_disc  = 3;
    r.have_pb_disc  = true; r.pb_disc  = 11;
    r.ev_pb = "reset_disc"; r.ev_cb = "reinflate_disc"; r.ev_clk = "reinflate_isb";
    r.have_innov_code = true;  r.innov_code_m  = 0.500000;
    r.have_innov_phase = true; r.innov_phase_m = -0.250000;
    r.have_res_code = true;    r.res_code_m    = 0.030000;
    r.have_res_phase = true;   r.res_phase_m   = -0.001000;
    r.have_amb  = true; r.amb_m  = 12.345678; r.amb_var  = 1.5e-4;
    r.have_iono = true; r.iono_m = 3.210000;  r.iono_var = 2.25e+02;
    return r;
}

} // namespace

TEST(ConsumerSatTraceSchemaTest, HeaderNamesEveryColumnInOrder) {
    const auto cols = splitCsv(std::string(kConsumerSatTraceHeader));
    const std::vector<std::string> expect = {
        "week", "tow", "sat", "sig", "code_sig", "elev_deg", "n_sig",
        "clk_m", "cb_m", "pb_m",
        "clk_disc", "cb_disc", "pb_disc",
        "ev_pb", "ev_cb", "ev_clk",
        "innov_code_m", "innov_phase_m",
        "res_code_m", "res_phase_m",
        "amb_m", "amb_var", "iono_m", "iono_var" };
    EXPECT_EQ(cols, expect);
}

TEST(ConsumerSatTraceSchemaTest, RowFieldCountMatchesHeader) {
    const auto n_hdr = splitCsv(std::string(kConsumerSatTraceHeader)).size();
    EXPECT_EQ(splitCsv(formatConsumerSatRow(2313, 276810.0, fullRow())).size(), n_hdr);
    // An empty row (every optional absent) must still carry the same field count
    // — a short line is what makes a trace unparsable half-way through a run.
    ConsumerSatRow bare;
    bare.sat = "G01"; bare.sig = "L2W";
    EXPECT_EQ(splitCsv(formatConsumerSatRow(2313, 276810.0, bare)).size(), n_hdr);
}

TEST(ConsumerSatTraceSchemaTest, PopulatedRowFormatsExactly) {
    const std::string line = formatConsumerSatRow(2313, 276810.0, fullRow());
    ASSERT_FALSE(line.empty());
    EXPECT_EQ(line.back(), '\n');
    EXPECT_EQ(line,
              "2313,276810.000,G23,L1W,C1W,41.250,2,"
              "-0.110000,1.234567,-3.638000,"
              "7,3,11,"
              "reset_disc,reinflate_disc,reinflate_isb,"
              "0.500000,-0.250000,"
              "0.030000,-0.001000,"
              "12.345678,0.00015,3.210000,225\n");
}

TEST(ConsumerSatTraceSchemaTest, AbsentQuantitiesAreBlankNeverZero) {
    // The distinction this instrument exists for: "the consumer applied nothing"
    // must not render as "the consumer applied 0.000000".
    ConsumerSatRow r;
    r.sat = "G12"; r.sig = "L1C";          // no paired code signal this epoch
    r.elev_deg = 8.0; r.n_sig = 1;
    r.have_pb = true; r.pb_m = 0.0;        // a real, applied, ZERO phase bias
    const auto f = splitCsv(formatConsumerSatRow(2313, 279300.5, r));
    EXPECT_EQ(f[4], "");                   // code_sig absent
    EXPECT_EQ(f[7], "");                   // clk_m   absent  (blank)
    EXPECT_EQ(f[8], "");                   // cb_m    absent  (blank)
    EXPECT_EQ(f[9], "0.000000");           // pb_m    PRESENT and zero
    EXPECT_EQ(f[10], "");                  // clk_disc absent
    EXPECT_EQ(f[12], "");                  // pb_disc  absent (no counter on this channel)
    EXPECT_EQ(f[18], "");                  // res_code_m  absent
    EXPECT_EQ(f[20], "");                  // amb_m   absent
    EXPECT_EQ(f[21], "");                  // amb_var absent
    EXPECT_EQ(f[22], "");                  // iono_m  absent
    EXPECT_EQ(f[23], "");                  // iono_var absent — trailing field still emitted
}

TEST(ConsumerSatTraceSchemaTest, MetreColumnsResolveSubMillimetreSteps) {
    // EV6 measured a +0.0005 m emitted phase-bias move at a +14.36 m S-transform
    // and called the frame algebra exonerated on that number; a trace that
    // rounded it to 0.000 could not have said so.
    ConsumerSatRow r;
    r.sat = "G22"; r.sig = "L2W";
    r.have_pb = true; r.pb_m = 0.0005;
    const auto f = splitCsv(formatConsumerSatRow(2313, 279240.0, r));
    EXPECT_EQ(f[9], "0.000500");
}

TEST(ConsumerSatTraceSchemaTest, DefaultEventsAreNoneNotEmpty) {
    // "none" is a measurement (the consumer did nothing with the wire this
    // epoch); an empty field would read as "not instrumented".
    ConsumerSatRow r;
    r.sat = "G05"; r.sig = "L1W";
    const auto f = splitCsv(formatConsumerSatRow(2313, 276810.0, r));
    EXPECT_EQ(f[13], "none");
    EXPECT_EQ(f[14], "none");
    EXPECT_EQ(f[15], "none");
}
