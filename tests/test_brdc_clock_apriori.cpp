/**
 * @file test_brdc_clock_apriori.cpp
 * @brief Card NBP gates U1 to U3: the ORBIT-7 parse, the native broadcast clock
 *        selector and continuous a priori against the bed, and the per-IODE wire.
 *
 * Gates (docs/refound/NBP_native_broadcast_clock_posture_2026-10-07.md section 5):
 *   U1  ORBIT-7 parsed on the DOY-129 file: 465 GPS records, transmission lead p50 7182 s.
 *   U2  The selected record equals the ttom pick of scripts/clock_level/scl_brdc_clk.py on
 *       100 % of the 30 s grid (G01 excluded by health); a_s(t) equals the bed's CLK value
 *       (scl_brdc_apriori.py --continuous --rule ttom) at every node to the CLK print
 *       resolution.
 *   U3  brdc(iode) + C_wire equals a_s + C_est to 1e-9 m; the stamped record is held by the
 *       store at consume time on 100 % of records.
 *
 * The real-data tests read the DOY-129 navigation file and the bed's CLK
 * (runs/_scl_data_bc/2024-129/products/) and skip when neither is found.  Lookup order:
 * the environment (GNSS_NBP_NAV, GNSS_NBP_BED_CLK), the project root's data/ and runs/
 * trees, then the main checkout at C:/GnssProcessor.  With GNSS_NBP_DUMP set to a path,
 * U2 also writes its picks (sat, tow, toc_sow, iode) for an external comparison with the
 * Python pick.
 *
 * The ttom pick of scl_brdc_clk.py is transcribed here as `pyPick` (sort by (ttom, toc),
 * take the last record with ttom <= t + 1e-6), independently of the selector under test.
 */

#include <gtest/gtest.h>
#include <fmt/format.h>
#include <nlohmann/json.hpp>

#include "network/state/RunContext.hpp"
#include "config/GnssConfig.hpp"
#include "logging/LogManager.hpp"
#include "products/BrdcClockApriori.hpp"
#include "products/LiveEphemerisStore.hpp"
#include "products/NetworkProductEmitter.hpp"
#include "products/RinexNavParser.hpp"
#include "math/TimeUtils.hpp"
#include "common/GnssTypes.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <optional>
#include <sstream>
#include <string>
#include <vector>
#include <cstdint>

namespace fs = std::filesystem;
using namespace GnssProcessor;

namespace {

constexpr double kC = 299792458.0;

std::string navField(double v) {
    char buf[40];
    std::snprintf(buf, sizeof(buf), "% .12E", v);
    std::string s(buf);
    if (s.size() > 19) {
        const auto e = s.find_first_of("Ee");
        if (e != std::string::npos && s.size() - e == 5 && s[e + 2] == '0') s.erase(e + 2, 1);
    }
    if (s.size() < 19) s = std::string(19 - s.size(), ' ') + s;
    return s.substr(0, 19);
}

std::string navLine(std::initializer_list<double> vals) {
    std::string s = "    ";
    for (double v : vals) s += navField(v);
    return s + "\n";
}

// A GPS LNAV record at 2024-05-08 hh:00 with chosen clock terms, IODE / IODC, health and
// ORBIT-7 (transmission time, fit interval).
std::string gpsRecord(int prn, int hh, double af0, double af1, int iode, int health,
                      double ttom, double fit, double toe) {
    char hdr[40];
    std::snprintf(hdr, sizeof(hdr), "G%02d %4d %02d %02d %02d %02d %02d", prn, 2024, 5, 8, hh, 0, 0);
    std::string s = std::string(hdr) + navField(af0) + navField(af1) + navField(0.0) + "\n";
    s += navLine({double(iode), 10.0, 4.5e-9, 1.0});
    s += navLine({1e-6, 0.01, 1e-6, 5153.7});
    s += navLine({toe, 1e-8, 0.3, 1e-8});
    s += navLine({0.95, 200.0, 1.0, -8e-9});
    s += navLine({1e-10, 1.0, 2313.0, 0.0});
    s += navLine({2.0, double(health), -1e-8, double(iode)});
    s += navLine({ttom, fit});
    return s;
}

const char* kHdrV3 =
    "     3.04           N: GNSS NAV DATA    M: MIXED            RINEX VERSION / TYPE\n"
    "                                                            END OF HEADER\n";

fs::path firstExisting(const char* env, const std::vector<fs::path>& cands) {
    if (const char* v = std::getenv(env)) {
        if (fs::exists(v)) return fs::path(v);
    }
    for (const auto& p : cands)
        if (fs::exists(p)) return p;
    return {};
}

const char* kNavName = "BRDC00IGS_R_20241290000_01D_MN.rnx";
const char* kClkName = "COD0MGXFIN_20241290000_01D_30S_CLK.CLK";

fs::path navFixture() {
    const fs::path root(GNSS_PROJECT_ROOT);
    return firstExisting("GNSS_NBP_NAV", {
        root / "runs" / "_scl_data_bc" / "2024-129" / "products" / kNavName,
        root / "data" / "2024-129" / "products" / kNavName,
        fs::path("C:/GnssProcessor/runs/_scl_data_bc/2024-129/products") / kNavName,
        fs::path("C:/GnssProcessor/data/2024-129/products") / kNavName });
}

fs::path bedClkFixture() {
    const fs::path root(GNSS_PROJECT_ROOT);
    return firstExisting("GNSS_NBP_BED_CLK", {
        root / "runs" / "_scl_data_bc" / "2024-129" / "products" / kClkName,
        fs::path("C:/GnssProcessor/runs/_scl_data_bc/2024-129/products") / kClkName });
}

/// 2024-05-08 00:00:00 GPS, continuous seconds.
double day129Origin() {
    return TimeUtils::continuousGpsSeconds(TimeUtils::gregorianGpsToGpsTime(2024, 5, 8, 0, 0, 0.0));
}

GnssTime fromContinuous(double s) {
    const double wk = std::floor(s / 604800.0);
    return GnssTime{static_cast<int>(wk), s - wk * 604800.0};
}

/// scl_brdc_clk.py's ttom pick, transcribed: sort by (ttom, toc), last record with
/// ttom <= t + 1e-6.  Health is not tested (the bed drops G01 by --drop).
std::optional<int> pyPick(const std::vector<BroadcastEphemeris>& recs, const std::vector<int>& idx,
                          double t) {
    std::optional<int> out;
    for (int i : idx)
        if (brdcTransmissionGpsSeconds(recs[i]) <= t + 1e-6) out = i;
    return out;
}

/// The bed CLK's GPS 'AS' first values: sat -> (continuous GPS s -> value s).
std::map<std::string, std::map<long long, double>> readBedClk(const fs::path& p) {
    std::map<std::string, std::map<long long, double>> out;
    std::ifstream in(p);
    std::string ln;
    while (std::getline(in, ln)) {
        if (ln.rfind("AS G", 0) != 0) continue;
        std::istringstream is(ln);
        std::string as, sat; int y, mo, d, h, mi, n; double s, v;
        is >> as >> sat >> y >> mo >> d >> h >> mi >> s >> n >> v;
        const double t = TimeUtils::continuousGpsSeconds(TimeUtils::gregorianGpsToGpsTime(y, mo, d, h, mi, s));
        out[sat][std::llround(t)] = v;
    }
    return out;
}

/// One unit in the last printed digit of a 12-digit mantissa Fortran E value (the CLK
/// print resolution; g3_walk.py's ffmt divides before rounding, so the printed value
/// itself can sit slightly beyond half a unit from the double it was made from).
double printResolution(double v) {
    if (v == 0.0) return 1e-18;
    const double e = std::floor(std::log10(std::abs(v))) + 1.0;
    return std::pow(10.0, e - 12.0);
}

/// GPS records only (the native posture is GPS-only; the setup passes the same set).
std::vector<BroadcastEphemeris> gpsOnly(std::vector<BroadcastEphemeris> v) {
    v.erase(std::remove_if(v.begin(), v.end(),
                           [](const BroadcastEphemeris& e) { return e.system != System::GPS; }),
            v.end());
    return v;
}

} // namespace

// ===========================================================================
// U1: ORBIT-7 parse
// ===========================================================================

// Synthetic: the transmission time, fit interval and IODC are read; a transmission time
// referred to the previous week (negative) is reduced into it; the RINEX unknown value
// leaves the record without one.
TEST(NbpBrdcParse, Orbit7FieldsAndWeekReduction) {
    const double toc_sow = 3.0 * 86400.0 + 2.0 * 3600.0;   // Wed 02:00
    std::string body = std::string(kHdrV3);
    body += gpsRecord(5, 2, 1e-4, 1e-12, 37, 0, toc_sow - 7182.0, 4.0, toc_sow);
    body += gpsRecord(6, 2, 1e-4, 1e-12, 38, 0, -600.0, 4.0, toc_sow);       // previous week, signed
    body += gpsRecord(7, 2, 1e-4, 1e-12, 39, 0, 0.9999e9, 0.0, toc_sow);     // unknown
    const auto v = RinexNavParser::parseString(body);
    ASSERT_EQ(v.size(), 3u);
    EXPECT_TRUE(v[0].has_transmission_time);
    EXPECT_EQ(v[0].transmission_time.week, 2313);
    EXPECT_DOUBLE_EQ(v[0].transmission_time.tow, toc_sow - 7182.0);
    EXPECT_DOUBLE_EQ(v[0].fit_interval, 4.0);
    EXPECT_EQ(v[0].iodc, 37);
    EXPECT_TRUE(v[1].has_transmission_time);
    EXPECT_EQ(v[1].transmission_time.week, 2312);
    EXPECT_DOUBLE_EQ(v[1].transmission_time.tow, 604800.0 - 600.0);
    EXPECT_FALSE(v[2].has_transmission_time);
}

// The DOY-129 file: 465 GPS records, every one with a transmission time, lead toc - ttom
// p50 7182 s (the card's facts).
TEST(NbpBrdcParse, Doy129Orbit7) {
    const fs::path nav = navFixture();
    if (nav.empty()) GTEST_SKIP() << "DOY-129 navigation file not found";
    const auto v = RinexNavParser::parse(nav.string());
    std::vector<double> lead;
    int n_gps = 0, n_tx = 0;
    for (const auto& e : v) {
        if (e.system != System::GPS) continue;
        ++n_gps;
        if (!e.has_transmission_time) continue;
        ++n_tx;
        lead.push_back(brdcTocGpsSeconds(e) - brdcTransmissionGpsSeconds(e));
    }
    EXPECT_EQ(n_gps, 465);
    EXPECT_EQ(n_tx, 465);
    ASSERT_FALSE(lead.empty());
    std::sort(lead.begin(), lead.end());
    EXPECT_DOUBLE_EQ(lead[lead.size() / 2], 7182.0);   // the script's sorted[n // 2]
    std::printf("[NBP U1] %d GPS records, %d with ORBIT-7 ttom; lead p50 %.0f s min %.0f max %.0f\n",
                n_gps, n_tx, lead[lead.size() / 2], lead.front(), lead.back());
}

// ===========================================================================
// U2: selector and continuous a priori against the bed
// ===========================================================================

// Synthetic: the switch happens at the first node at or after ttom, the step is removed
// (a priori continuous at the switch node), an unhealthy record is never selected.
TEST(NbpBrdcSelect, SyntheticSwitchAndHealth) {
    const double day = day129Origin();
    const double wk  = std::floor(day / 604800.0) * 604800.0;
    const double sow0 = day - wk;
    std::string body = std::string(kHdrV3);
    // G05: record A toc 00:00 (ttom 22:00 previous day), record B toc 02:00 ttom 00:59:50.
    body += gpsRecord(5, 0, 1.0e-4, 1.0e-11, 10, 0, sow0 - 7200.0, 4.0, sow0);
    body += gpsRecord(5, 2, 1.0e-4 + 5e-10, 2.0e-11, 11, 0, sow0 + 3590.0, 4.0, sow0 + 7200.0);
    // G06: one healthy record and a later unhealthy one.
    body += gpsRecord(6, 0, 2.0e-4, 0.0, 20, 0, sow0 - 7200.0, 4.0, sow0);
    body += gpsRecord(6, 2, 2.0e-4, 0.0, 21, 63, sow0 + 600.0, 4.0, sow0 + 7200.0);
    const BrdcClockApriori ap(RinexNavParser::parseString(body), day, day + 3.0 * 3600.0);
    EXPECT_EQ(ap.stats().n_unhealthy, 1u);
    EXPECT_EQ(ap.stats().n_switches, 1u);
    const auto* segs = ap.segments("G05");
    ASSERT_NE(segs, nullptr);
    ASSERT_EQ(segs->size(), 2u);
    EXPECT_DOUBLE_EQ((*segs)[1].start_s, day + 3600.0);   // first node at or after 00:59:50
    // Continuity at the switch node: a_k(t_sw) == a_k-1(t_sw).
    const double t_sw = day + 3600.0;
    const double a_old = ap.clockSeconds((*segs)[0].record, t_sw) + (*segs)[0].offset_s;
    const double a_new = ap.clockSeconds((*segs)[1].record, t_sw) + (*segs)[1].offset_s;
    EXPECT_NEAR(a_new, a_old, 1e-18);
    // Between nodes the node-at-or-before record holds.
    const auto e_before = ap.evaluate("G05", fromContinuous(t_sw - 0.07));
    const auto e_after  = ap.evaluate("G05", fromContinuous(t_sw + 0.07));
    ASSERT_TRUE(e_before && e_after);
    EXPECT_EQ(e_before->iode, 10);
    EXPECT_EQ(e_after->iode, 11);
    // G06's unhealthy record is never selected.
    for (double t = day; t < day + 3.0 * 3600.0; t += 30.0) {
        const auto e = ap.evaluate("G06", fromContinuous(t));
        ASSERT_TRUE(e.has_value());
        EXPECT_EQ(e->iode, 20);
    }
}

// The DOY-129 file: the selection equals the transcribed Python ttom pick on every 30 s
// node for every healthy satellite (G01, health 63 all day, is never selected), and the
// continuous a priori equals the bed CLK at every node to the print resolution.
TEST(NbpBrdcSelect, Doy129MatchesBedPickAndClk) {
    const fs::path nav = navFixture();
    const fs::path clk = bedClkFixture();
    if (nav.empty() || clk.empty()) GTEST_SKIP() << "DOY-129 navigation file or bed CLK not found";
    const std::vector<BroadcastEphemeris> recs = gpsOnly(RinexNavParser::parse(nav.string()));
    const double day = day129Origin();
    const BrdcClockApriori ap(recs, day, day + 86370.0);

    std::map<std::string, std::vector<int>> py;   // all GPS records with ttom, (ttom, toc) order
    for (int i = 0; i < static_cast<int>(recs.size()); ++i)
        if (recs[i].system == System::GPS && recs[i].has_transmission_time)
            py[recs[i].satellite].push_back(i);
    for (auto& [s, idx] : py)
        std::stable_sort(idx.begin(), idx.end(), [&](int a, int b) {
            const double ta = brdcTransmissionGpsSeconds(recs[a]), tb = brdcTransmissionGpsSeconds(recs[b]);
            if (ta != tb) return ta < tb;
            return brdcTocGpsSeconds(recs[a]) < brdcTocGpsSeconds(recs[b]);
        });

    std::ofstream dump;
    if (const char* d = std::getenv("GNSS_NBP_DUMP")) {
        dump.open(d);
        dump << "sat,tow,toc_sow,iode\n";
    }
    long long n_nodes = 0, n_pick_eq = 0, n_unhealthy_nodes = 0, n_unhealthy_sel = 0;
    for (const auto& [sat, idx] : py) {
        const bool unhealthy = std::any_of(idx.begin(), idx.end(),
                                           [&](int i) { return recs[i].health != 0; });
        for (int n = 0; n < 2880; ++n) {
            const double t = day + 30.0 * n;
            const auto mine = ap.select(sat, t);
            const auto ev   = ap.evaluate(sat, fromContinuous(t));
            if (unhealthy) {
                ++n_unhealthy_nodes;
                if (mine || ev) ++n_unhealthy_sel;
                continue;
            }
            ++n_nodes;
            const auto ref = pyPick(recs, idx, t);
            if (mine && ref && *mine == *ref && ev && ev->record == *ref) ++n_pick_eq;
            if (dump && ev)
                dump << sat << ',' << fmt::format("{:.1f}", t - std::floor(t / 604800.0) * 604800.0) << ','
                     << fmt::format("{:.1f}", recs[ev->record].kepler.toc.tow) << ',' << ev->iode << '\n';
        }
    }
    std::printf("[NBP U2] pick: %lld / %lld healthy satellite-nodes equal the ttom pick; "
                "unhealthy satellite-nodes %lld, selected %lld\n",
                n_pick_eq, n_nodes, n_unhealthy_nodes, n_unhealthy_sel);
    EXPECT_GT(n_nodes, 0);
    EXPECT_EQ(n_pick_eq, n_nodes);
    EXPECT_GT(n_unhealthy_nodes, 0);   // G01 is in the file
    EXPECT_EQ(n_unhealthy_sel, 0);

    // a_s(t) against the bed CLK.
    const auto bed = readBedClk(clk);
    long long n_cmp = 0, n_ok = 0, n_half = 0, n_missing = 0;
    double max_abs_s = 0.0, max_over_res = 0.0;
    for (const auto& [sat, by_t] : bed) {
        for (const auto& [t, v] : by_t) {
            const auto ev = ap.evaluate(sat, fromContinuous(static_cast<double>(t)));
            if (!ev) { ++n_missing; continue; }
            ++n_cmp;
            const double d = std::abs(ev->clock_s - v);
            max_abs_s = std::max(max_abs_s, d);
            max_over_res = std::max(max_over_res, d / printResolution(v));
            if (d <= printResolution(v)) ++n_ok;
            if (d <= 0.5 * printResolution(v)) ++n_half;
        }
    }
    std::printf("[NBP U2] a priori: %lld bed nodes compared (%zu satellites), %lld within the print "
                "resolution (%lld within half of it), %lld without a native value; max |a - bed| "
                "%.3e s (%.3e m), max |a - bed| / print resolution %.3f\n",
                n_cmp, bed.size(), n_ok, n_half, n_missing, max_abs_s, max_abs_s * kC, max_over_res);
    EXPECT_GT(n_cmp, 0);
    EXPECT_EQ(n_missing, 0);
    EXPECT_EQ(n_ok, n_cmp);
    std::printf("[NBP U2] table: %zu staged records, %zu unhealthy, %zu satellites, %zu switches, "
                "O range [%.4f, %.4f] m\n",
                ap.stats().n_staged, ap.stats().n_unhealthy, ap.stats().n_sats,
                ap.stats().n_switches, ap.stats().offset_min_m, ap.stats().offset_max_m);
}

// ===========================================================================
// U3: the wire
// ===========================================================================

class NbpBrdcWire : public ::testing::Test {
protected:
    void SetUp() override { LiveEphemerisStore::getInstance().clear(); }
    void TearDown() override { LiveEphemerisStore::getInstance().clear(); }
};

// Over the whole DOY-129 day on the 30 s grid: the store is fed by transmission time, the
// wire is stamped, and the consumer's reconstruction brdc(iode) + C_wire (the store's
// pinned evaluation) equals a_s + C_est to 1e-9 m with the stamped record held on 100 %
// of records.  The emission-time reconstruction (t - 0.075 s, where the producer's a
// priori uses the node-before record) is reported, not gated.
TEST_F(NbpBrdcWire, Doy129ReconstructionAndHeld) {
    const fs::path nav = navFixture();
    if (nav.empty()) GTEST_SKIP() << "DOY-129 navigation file not found";
    const double day = day129Origin();
    BrdcClockApriori ap(gpsOnly(RinexNavParser::parse(nav.string())), day, day + 86370.0);
    auto& store = LiveEphemerisStore::getInstance();
    std::map<std::string, int> prev;
    long long n_rec = 0, n_held = 0, n_unstamped = 0, n_switch = 0, n_bad = 0;
    double max_dev = 0.0, max_dev_em = 0.0;
    std::vector<std::string> sats;
    for (int p = 1; p <= 32; ++p) sats.push_back(fmt::format("G{:02d}", p));
    for (int n = 0; n < 2880; ++n) {
        const double t_s = day + 30.0 * n;
        const GnssTime t = fromContinuous(t_s);
        ap.feedStore(store, t_s);
        NetworkSolutionEpoch sol;
        sol.epoch_time = t;
        for (std::size_t i = 0; i < sats.size(); ++i) {
            NetworkSatSolution s;
            s.sat = sats[i];
            s.has_clock = true;
            s.clock_c0_m = 0.37 * static_cast<double>(i) - 4.2 + 1e-3 * n;   // an arbitrary C_est
            sol.sats.push_back(s);
        }
        const std::vector<NetworkSatSolution> before = sol.sats;
        const auto rows = ap.stampWire(sol, t, &store, prev);
        ASSERT_EQ(rows.size(), sats.size());
        for (std::size_t i = 0; i < rows.size(); ++i) {
            const auto& r = rows[i];
            if (r.iode < 0) { ++n_unstamped; continue; }
            ++n_rec;
            if (r.held) ++n_held;
            if (r.switched) ++n_switch;
            const auto ev = ap.evaluate(r.sat, t);
            const auto bc = store.computeSatelliteClock(r.sat, t, sol.sats[i].clock_iode);
            if (!ev || !bc || bc->iode != r.iode) { ++n_bad; continue; }
            const double lhs = bc->bias * kC + sol.sats[i].clock_c0_m;
            const double rhs = ev->clock_s * kC + before[i].clock_c0_m;
            max_dev = std::max(max_dev, std::abs(lhs - rhs));
            // Emission time: the producer's a priori there against the consumer's pinned record.
            const GnssTime te = fromContinuous(t_s - 0.075);
            const auto ev_e = ap.evaluate(r.sat, te);
            const auto bc_e = store.computeSatelliteClock(r.sat, te, r.iode);
            if (ev_e && bc_e)
                max_dev_em = std::max(max_dev_em, std::abs((bc_e->bias * kC + sol.sats[i].clock_c0_m)
                                                         - (ev_e->clock_s * kC + before[i].clock_c0_m)));
        }
    }
    std::printf("[NBP U3] %lld stamped records (%lld unstamped: no eligible record), %lld held by "
                "the store, %lld switches, %lld unevaluable; max |brdc(iode) + C_wire - (a_s + "
                "C_est)| %.3e m at the epoch, %.3e m at emission time t - 0.075 s\n",
                n_rec, n_unstamped, n_held, n_switch, n_bad, max_dev, max_dev_em);
    EXPECT_GT(n_rec, 0);
    EXPECT_EQ(n_held, n_rec);
    EXPECT_EQ(n_bad, 0);
    EXPECT_LE(max_dev, 1e-9);
    EXPECT_EQ(n_unstamped, 2880);   // G01 only: unhealthy all day
}

// ===========================================================================
// Loader refusals and the fingerprint
// ===========================================================================

namespace {

/// Load the sim golden with @p edit applied; a refusal (false or a throw) returns false.
bool loadSimGoldenWith(const std::function<void(nlohmann::json&)>& edit, GnssConfig& cfg,
                       const std::string& tag) {
    const fs::path src = fs::path(GNSS_PROJECT_ROOT) / "config" / "golden" / "golden_sim_refound.json";
    std::ifstream in(src);
    nlohmann::json j = nlohmann::json::parse(in, nullptr, true, /*ignore_comments=*/true);
    edit(j);
    const fs::path p = fs::temp_directory_path() / ("gnss_nbp_" + tag + ".json");
    { std::ofstream out(p); out << j.dump(2); }
    LogManager log;
    bool ok = false;
    try { ok = cfg.loadFromJson(p, log); } catch (const std::exception&) { ok = false; }
    std::error_code ec;
    fs::remove(p, ec);
    return ok;
}

/// The SCL2B hold posture (card SMH's smoothed hold, no phase-clock hold).
void smhHold(nlohmann::json& j) {
    j["network"]["emit_pb_hold_phase_clock"] = false;
    j["network"]["emit_pb_hold_smoothed"]    = true;
}

std::uint64_t fingerprintOf(const GnssConfig& cfg) {
    NetworkRunContext ctx(cfg, cfg.getNetworkConfig());
    ctx.carry.snap_path = "state_snapshot.snap";
    std::vector<NetStation> st(2);
    st[0].name = "AAAA"; st[0].is_pivot = true;
    st[1].name = "BBBB"; st[1].is_pivot = false;
    networkSnapshotFingerprint(ctx, cfg, st);
    return ctx.carry.live_fingerprint;
}

} // namespace

TEST(NbpBrdcConfig, ArmsWithTheSmoothedHoldAndKeepsTheSp3AnchorSigma) {
    GnssConfig off, on;
    ASSERT_TRUE(loadSimGoldenWith(smhHold, off, "off"));
    ASSERT_TRUE(loadSimGoldenWith([](nlohmann::json& j) {
        smhHold(j);
        j["network"]["apriori_source"] = "sp3_brdc_clock";
        j["network"]["brdc_select"]    = "ttom";
    }, on, "on"));
    EXPECT_FALSE(off.networkAprioriBrdcClock());
    EXPECT_TRUE(on.networkAprioriBrdcClock());
    EXPECT_EQ(on.networkBrdcSelect(), NetworkBrdcSelect::Ttom);
    // sp3's anchor-sigma auto, not brdc_rts's 3 m (bed equivalence).
    EXPECT_DOUBLE_EQ(on.resolveNetworkAprioriClockAnchorSigmaM(),
                     off.resolveNetworkAprioriClockAnchorSigmaM());
    // Fingerprinted: fresh convergence only.
    EXPECT_NE(fingerprintOf(on), fingerprintOf(off));
}

TEST(NbpBrdcConfig, RefusesThePhaseClockHoldAndRequiresTheSmoothedHold) {
    GnssConfig a, b, c;
    // The golden's own hold is card LAH's phase-clock hold: refused under the new value.
    EXPECT_FALSE(loadSimGoldenWith([](nlohmann::json& j) {
        j["network"]["apriori_source"] = "sp3_brdc_clock";
    }, a, "lah"));
    // No smoothed hold: refused.
    EXPECT_FALSE(loadSimGoldenWith([](nlohmann::json& j) {
        j["network"]["emit_pb_hold_phase_clock"] = false;
        j["network"]["apriori_source"] = "sp3_brdc_clock";
    }, b, "plain"));
    // Not a rule: refused.
    EXPECT_FALSE(loadSimGoldenWith([](nlohmann::json& j) {
        smhHold(j);
        j["network"]["apriori_source"] = "sp3_brdc_clock";
        j["network"]["brdc_select"]    = "toc";
    }, c, "toc"));
}

TEST(NbpBrdcConfig, KnobOffFingerprintUnchangedByTheSelectKnob) {
    GnssConfig a, b;
    ASSERT_TRUE(loadSimGoldenWith([](nlohmann::json&) {}, a, "fp_a"));
    ASSERT_TRUE(loadSimGoldenWith([](nlohmann::json& j) {
        j["network"]["brdc_select"] = "ttom";
    }, b, "fp_b"));
    EXPECT_EQ(fingerprintOf(a), fingerprintOf(b));
}
