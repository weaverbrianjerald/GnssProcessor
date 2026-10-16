/**
 * @file test_rhs_attrib_columns.cpp
 * @brief Card ICF instrument A': the attribution's solution shares at named columns
 *        (@c network.rhs_attrib_columns, @c networkRhsAttribColumnShares in
 *        filter/NetworkRhsAttrib.hpp) and the contribution kind in the row dump.
 *
 * The fixture is a small network solved by the estimator itself: two satellites with a
 * clock and an L1C phase bias each on the border, three stations with code and phase
 * rows, ambiguity pins at the first station, a pre-PD receiver code-bias prior at every
 * station and a post-PD ambiguity carry at the other two, a diagonal border prior and two
 * constraint rows, and a datum that pins G01's clock to a non-zero value (so the datum
 * has a share at the free columns).  The contracts:
 *  - at a free named column the solution shares of every part sum to the estimator's
 *    solution (closure at rounding), and the recomputed solution is the solve's;
 *  - each (station, kind) share equals an independent dense solve of that part's
 *    reduced right-hand side under the same datum, so the per-row route is the operator
 *    the header states;
 *  - the (station, kind, satellite, signal) parts sum to the (station, kind) parts, and
 *    the kind totals to the total;
 *  - at a pinned named column every share is zero but the datum's, which is the pin;
 *  - a label with no column keeps its slot, empty;
 *  - an empty knob writes no cols_ file, and the class-based files do not depend on it;
 *  - the row dump's last column is the row's contribution kind;
 *  - the loader keeps the string entries of an array, in order and without duplicates.
 */
#include <gtest/gtest.h>

#include "config/GnssConfig.hpp"
#include "network/assembly/NetworkContributions.hpp"
#include "network/solve/NetworkEstimator.hpp"
#include "network/solve/NetworkNormalEquations.hpp"
#include "network/qc/NetworkRhsAttrib.hpp"
#include "network/assembly/NetworkRowClassify.hpp"
#include "network/driver/NetworkRunTypes.hpp"
#include "logging/LogManager.hpp"
#include "rinex/MultiRinexSync.hpp"

#include <Eigen/Dense>
#include <spdlog/sinks/null_sink.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using namespace GnssProcessor;

namespace {

using CK = ContributionKind;

/// The fixture network and its solve.
struct Fixture {
    NetworkEstimator             est;
    std::vector<NetStation>      stations;
    std::vector<StationAssembly> asm_res;
    std::vector<int>             est_index_of;
    NetworkEstimator::Datum      datum;
    Eigen::VectorXd              x_s;
    Eigen::Index clk[2]{-1, -1};
    Eigen::Index pb[2]{-1, -1};
};

/// One station's rows: per satellite a code row and a phase row; the first station pins
/// its two ambiguities.  Innovations are arbitrary but distinct.
void buildRows(Fixture& f, int r, StationAssembly& R) {
    const bool pivot = (r == 0);
    const Eigen::Index m = 4 + (pivot ? 2 : 0);
    const Eigen::Index ns = f.est.numShared();
    R = StationAssembly{};
    R.ok = true;
    R.A_s = Eigen::MatrixXd::Zero(m, ns);
    R.A_r = Eigen::MatrixXd::Zero(m, 4);
    R.w = Eigen::VectorXd::Ones(m);
    R.y = Eigen::VectorXd::Zero(m);
    const char* sats[2] = {"G01", "G02"};
    Eigen::Index row = 0;
    for (int s = 0; s < 2; ++s) {
        R.A_s(row, f.clk[s]) = -1.0;
        R.A_r(row, 0) = 1.0;
        R.A_r(row, 3) = 1.0;
        R.w(row) = 4.0;
        R.y(row) = 1.5 + 0.7 * r - 0.3 * s;
        R.row_tags.push_back(makeRowTag(CK::DataCode, sats[s], "C1C"));
        ++row;
        R.A_s(row, f.clk[s]) = -1.0;
        R.A_s(row, f.pb[s]) = 1.0;
        R.A_r(row, 0) = 1.0;
        R.A_r(row, 1 + s) = 1.0;
        R.w(row) = 1.0e4;
        R.y(row) = 2.25 - 0.4 * r + 0.9 * s;
        R.row_tags.push_back(makeRowTag(CK::DataPhase, sats[s], "L1C"));
        ++row;
    }
    R.n_data_rows = row;
    if (pivot) {
        for (int s = 0; s < 2; ++s) {
            R.A_r(row, 1 + s) = 1.0;
            R.w(row) = 1.0e10;
            R.y(row) = 0.01 * (s + 1);
            R.row_tags.push_back(makeRowTag(CK::AmbiguityPin, sats[s], "L1C"));
            ++row;
        }
        R.np = 2;
    }
    R.prior_cols = {3};
    R.prior_dNrr = Eigen::VectorXd::Constant(1, 0.01);
    R.prior_dur = Eigen::VectorXd::Constant(1, 0.01 * (0.0 - (0.92 + 0.5 * r)));
    R.prior_tags = {RowTag{CK::PriorRxCbDatumPrePd, {}, {}}};
    if (!pivot) {
        R.carry_cols = {2};
        R.carry_dNrr = Eigen::VectorXd::Constant(1, 50.0);
        R.carry_dur = Eigen::VectorXd::Constant(1, 50.0 * (0.125 * r));
        R.carry_tags = {makeRowTag(CK::PriorAmbiguityCarry, "G02", "L1C")};
    }
}

/// Build, fold and solve the fixture with G01's clock pinned to @p pin_value.
void solveFixture(Fixture& f, double pin_value) {
    f.clk[0] = f.est.registerClock("G01");
    f.clk[1] = f.est.registerClock("G02");
    f.pb[0] = f.est.registerPhaseBias("G01", "L1C");
    f.pb[1] = f.est.registerPhaseBias("G02", "L1C");
    f.est.attribArm(true);
    f.est.beginEpoch();
    f.stations.resize(3);
    f.asm_res.resize(3);
    f.est_index_of.assign(3, -1);
    for (int r = 0; r < 3; ++r) {
        f.stations[static_cast<std::size_t>(r)].name = "STA" + std::to_string(r);
        StationAssembly& R = f.asm_res[static_cast<std::size_t>(r)];
        buildRows(f, r, R);
        const NetworkNormalEquations::StationPrior prior{R.prior_cols, R.prior_dNrr, R.prior_dur};
        const std::size_t slot = f.est.addStation(R.A_s, R.A_r, R.w, R.y, prior);
        f.est_index_of[static_cast<std::size_t>(r)] = static_cast<int>(slot);
        if (!R.carry_cols.empty()) {
            f.est.addStationPrior(slot, R.carry_cols, R.carry_dNrr, R.carry_dur);
            R.carry_prior_folded = true;
        }
    }
    NetworkEstimator::SharedPrior p;
    p.kind = SharedParamKind::Clock;
    p.sat = "G02";
    p.value = 0.1;
    p.info = 4.0;
    f.est.contributionKind(CK::BorderCarryDiagonal);
    f.est.addSharedPrior({p});
    Eigen::MatrixXd H = Eigen::MatrixXd::Zero(2, f.est.numShared());
    H(0, f.pb[0]) = 1.0;
    H(0, f.pb[1]) = 1.0;
    H(1, f.pb[1]) = 1.0;
    const Eigen::Vector2d z(0.05, -0.02);
    const std::vector<CK> kinds{CK::CbSumToZero, CK::CbIfPin};
    f.est.contributionRowKinds(&kinds);
    f.est.addSharedConstraintRows(H, z, 1.0e3);
    f.datum = {{f.clk[0], pin_value}};
    f.x_s = f.est.solve(f.datum);
}

NetworkAttribFrame captureFrame(Fixture& f) {
    NetworkAttribFrame frame;
    SyncedEpoch se;
    se.time.week = 2313;
    se.time.tow = 259200.0;
    networkAttribCaptureFrame(frame, se, f.stations, f.est, f.datum, f.asm_res,
                              f.est_index_of, /*own_assemblies=*/false, &f.x_s);
    return frame;
}

const std::vector<std::string> kLabels{"CLK:G02", "PB:G01:L1C", "CLK:G01", "CLK:G99"};

/// The datum-constrained normals, built independently (a dense LU solve below).
Eigen::MatrixXd constrainedNormals(const NetworkAttribFrame& fr, std::vector<char>& pinned) {
    Eigen::MatrixXd N = fr.N_red;
    pinned.assign(static_cast<std::size_t>(fr.n), 0);
    for (const auto& p : fr.datum) {
        N.row(p.column).setZero();
        N.col(p.column).setZero();
        N(p.column, p.column) = 1.0;
        pinned[static_cast<std::size_t>(p.column)] = 1;
    }
    return N;
}

std::string readFile(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

/// One line of a dump: std::getline without the carriage return a text-mode write adds
/// on Windows.
bool readLine(std::istream& in, std::string& line) {
    if (!std::getline(in, line)) return false;
    if (!line.empty() && line.back() == '\r') line.pop_back();
    return true;
}

std::shared_ptr<spdlog::logger> nullLogger() {
    return std::make_shared<spdlog::logger>("rhs_attrib_columns_test",
                                            std::make_shared<spdlog::sinks::null_sink_mt>());
}

}  // namespace

TEST(RhsAttribColumnsTest, SolutionSharesSumToTheSolveAtTheNamedColumns) {
    Fixture f;
    solveFixture(f, /*pin_value=*/0.3);
    const NetworkAttribFrame frame = captureFrame(f);
    ASSERT_TRUE(frame.valid);
    const NetworkRhsAttribDecomposition dec = networkRhsAttribDecompose(frame);
    ASSERT_TRUE(dec.valid);
    const NetworkRhsColumnShares sh = networkRhsAttribColumnShares(frame, dec, kLabels);
    ASSERT_TRUE(sh.valid);
    ASSERT_EQ(sh.col.size(), kLabels.size());
    EXPECT_EQ(sh.col[0], f.clk[1]);
    EXPECT_EQ(sh.col[1], f.pb[0]);
    EXPECT_EQ(sh.col[2], f.clk[0]);
    EXPECT_EQ(sh.col[3], -1) << "a label with no column keeps its slot";
    EXPECT_EQ(sh.pinned[2], 1);
    EXPECT_EQ(sh.pinned[0], 0);

    for (Eigen::Index j = 0; j < 3; ++j) {
        SCOPED_TRACE(kLabels[static_cast<std::size_t>(j)]);
        const Eigen::Index c = sh.col[static_cast<std::size_t>(j)];
        // The recomputed solution is the solve's, and the shares close on it.
        EXPECT_NEAR(sh.x_full(j), f.x_s(c), 1e-12 * std::max(1.0, std::abs(f.x_s(c))));
        EXPECT_EQ(sh.x_solve(j), f.x_s(c));
        double scale = 0.0;
        for (const auto& p : sh.station_kind) scale = std::max(scale, std::abs(p.sol(j)));
        for (const auto& p : sh.folds)        scale = std::max(scale, std::abs(p.sol(j)));
        EXPECT_LE(sh.sol_closure_abs(j), 1e-12 * std::max(1.0, scale));
        EXPECT_LE(sh.sol_closure_rel(j), 1e-10);
        EXPECT_LE(sh.rhs_closure_abs(j), 1e-9 * std::max(1.0, std::abs(sh.b_red(j))));
        EXPECT_NEAR(sh.sol_sum(j), f.x_s(c), 1e-12 * std::max(1.0, scale));
    }
    // The absent column reads zero everywhere.
    EXPECT_EQ(sh.x_full(3), 0.0);
    EXPECT_EQ(sh.sol_sum(3), 0.0);

    // At the pinned column every share but the datum's is zero; the datum's is the pin.
    for (const auto& p : sh.station_kind) EXPECT_EQ(p.sol(2), 0.0) << p.sta << " " << p.kind;
    bool saw_pin = false;
    for (const auto& p : sh.folds) {
        if (p.record == "DATUMPIN") {
            saw_pin = true;
            EXPECT_EQ(p.n, 1);
            EXPECT_EQ(p.sol(2), 0.3);
            EXPECT_NE(p.sol(0), 0.0) << "a non-zero pin moves the free columns";
        } else {
            EXPECT_EQ(p.sol(2), 0.0) << p.kind;
        }
    }
    EXPECT_TRUE(saw_pin);
    EXPECT_NEAR(sh.x_full(2), 0.3, 1e-15);

    // Every station channel has its (station, kind) part, the shared folds their class.
    std::map<std::string, int> kinds_seen;
    for (const auto& p : sh.station_kind) ++kinds_seen[p.kind];
    EXPECT_EQ(kinds_seen["data_code"], 3);
    EXPECT_EQ(kinds_seen["data_phase"], 3);
    EXPECT_EQ(kinds_seen["ambiguity_pin"], 1);
    EXPECT_EQ(kinds_seen["prior_rx_cb_datum_pre_pd"], 3);
    EXPECT_EQ(kinds_seen["prior_ambiguity_carry"], 2);
    std::vector<std::string> shared;
    for (const auto& p : sh.folds) if (p.record == "SHARED") shared.push_back(p.kind);
    EXPECT_EQ(shared, (std::vector<std::string>{"shared:border_carry_diagonal",
                                                "shared:cb_if_pin", "shared:cb_sum_to_zero"}));

    // The line-of-sight parts sum to the (station, kind) parts; the kind totals to the total.
    std::map<std::pair<std::string, std::string>, Eigen::VectorXd> los_sum;
    for (const auto& p : sh.station_los) {
        auto key = std::make_pair(p.sta, p.kind);
        if (!los_sum.count(key)) los_sum[key] = Eigen::VectorXd::Zero(p.sol.size());
        los_sum[key] += p.sol;
    }
    for (const auto& p : sh.station_kind)
        EXPECT_LT((los_sum[{p.sta, p.kind}] - p.sol).cwiseAbs().maxCoeff(), 1e-13)
            << p.sta << " " << p.kind;
    Eigen::VectorXd kind_sum = Eigen::VectorXd::Zero(static_cast<Eigen::Index>(kLabels.size()));
    for (const auto& p : sh.kind_total) kind_sum += p.sol;
    EXPECT_LT((kind_sum - sh.sol_sum).cwiseAbs().maxCoeff(), 1e-12);

    // An independent route: each (station, kind) part's reduced vector, formed densely as
    // u_s - N_sr N_rr^-1 u_r and solved by LU under the same pins.
    std::vector<char> pinned;
    const Eigen::MatrixXd Nc = constrainedNormals(frame, pinned);
    const Eigen::FullPivLU<Eigen::MatrixXd> lu(Nc);
    for (std::size_t k = 0; k < frame.asm_ptr.size(); ++k) {
        const StationAssembly& R = *frame.asm_ptr[k];
        std::map<std::string, std::pair<Eigen::VectorXd, Eigen::VectorXd>> per;
        auto acc = [&](const std::string& kn) -> auto& {
            if (!per.count(kn))
                per[kn] = {Eigen::VectorXd::Zero(frame.n), Eigen::VectorXd::Zero(R.A_r.cols())};
            return per[kn];
        };
        for (Eigen::Index row = 0; row < R.y.size(); ++row) {
            auto& a = acc(contributionKindName(rowTagAt(R, row).kind));
            a.first  += R.A_s.row(row).transpose() * (R.w(row) * R.y(row));
            a.second += R.A_r.row(row).transpose() * (R.w(row) * R.y(row));
        }
        for (std::size_t e = 0; e < R.prior_cols.size(); ++e)
            acc(contributionKindName(R.prior_tags[e].kind)).second(R.prior_cols[e]) +=
                R.prior_dur(static_cast<Eigen::Index>(e));
        for (std::size_t e = 0; e < R.carry_cols.size(); ++e)
            acc(contributionKindName(R.carry_tags[e].kind)).second(R.carry_cols[e]) +=
                R.carry_dur(static_cast<Eigen::Index>(e));
        for (auto& [kn, uu] : per) {
            Eigen::VectorXd v = uu.first - frame.Nsr[k] * frame.Nrr[k].ldlt().solve(uu.second);
            const Eigen::VectorXd rhs_at = v;
            for (std::size_t q = 0; q < pinned.size(); ++q)
                if (pinned[q]) v(static_cast<Eigen::Index>(q)) = 0.0;
            const Eigen::VectorXd y = lu.solve(v);
            const auto it = std::find_if(sh.station_kind.begin(), sh.station_kind.end(),
                                         [&](const auto& p) {
                                             return p.sta == frame.sta_name[k] && p.kind == kn;
                                         });
            ASSERT_NE(it, sh.station_kind.end()) << frame.sta_name[k] << " " << kn;
            for (Eigen::Index j = 0; j < 2; ++j) {
                const Eigen::Index c = sh.col[static_cast<std::size_t>(j)];
                EXPECT_NEAR(it->sol(j), y(c), 1e-10 * std::max(1.0, std::abs(y(c))))
                    << frame.sta_name[k] << " " << kn << " " << kLabels[static_cast<std::size_t>(j)];
                EXPECT_NEAR(it->rhs(j), rhs_at(c), 1e-9 * std::max(1.0, std::abs(rhs_at(c))))
                    << frame.sta_name[k] << " " << kn;
            }
        }
    }
}

TEST(RhsAttribColumnsTest, EmptyKnobWritesNothingAndTheRowDumpNamesKinds) {
    Fixture f;
    solveFixture(f, /*pin_value=*/0.0);
    const NetworkAttribFrame frame = captureFrame(f);
    ASSERT_TRUE(frame.valid);
    const fs::path root = fs::temp_directory_path() / "rhs_attrib_columns_test";
    std::error_code ec;
    fs::remove_all(root, ec);
    const fs::path off = root / "off";
    const fs::path on = root / "on";
    const auto log = nullLogger();

    networkRhsAttribDump(frame, log, off, "armed", {});
    EXPECT_TRUE(fs::exists(off / "attrib_259200.csv"));
    EXPECT_TRUE(fs::exists(off / "rows_259200.csv"));
    EXPECT_TRUE(fs::exists(off / "vec_259200.csv"));
    EXPECT_FALSE(fs::exists(off / "cols_259200.csv")) << "an empty knob writes no cols_ file";

    networkRhsAttribDump(frame, log, on, "armed", kLabels);
    ASSERT_TRUE(fs::exists(on / "cols_259200.csv"));
    for (const char* name : {"attrib_259200.csv", "rows_259200.csv", "vec_259200.csv"})
        EXPECT_EQ(readFile(off / name), readFile(on / name))
            << name << " must not depend on the knob";

    // The row dump's last column is the row's kind.
    std::istringstream rows(readFile(off / "rows_259200.csv"));
    std::string header, first;
    readLine(rows, header);
    readLine(rows, first);
    EXPECT_EQ(header, "station,row,class,sat,w,y,h_c00_l1,h_c00_l2,w_y_h1,kind");
    EXPECT_EQ(first.substr(first.rfind(',') + 1), "data_code");

    // The cols_ file: header, one COLUMN line, and the closure lines.
    const std::string cols = readFile(on / "cols_259200.csv");
    std::istringstream cs(cols);
    std::string cheader;
    readLine(cs, cheader);
    EXPECT_EQ(cheader,
              "week,tow,record,station,kind,sat,signal,n,rhs[CLK:G02],sol[CLK:G02],"
              "rhs[PB:G01:L1C],sol[PB:G01:L1C],rhs[CLK:G01],sol[CLK:G01],"
              "rhs[CLK:G99],sol[CLK:G99]");
    for (const char* rec : {",COLUMN,", ",STAKIND,", ",STALOS,", ",SHARED,", ",DATUMPIN,",
                            ",KIND,", ",TOTAL,", ",FULL,", ",SOLVE,", ",CLOSURE_ABS,",
                            ",CLOSURE_REL,"})
        EXPECT_NE(cols.find(rec), std::string::npos) << rec;
    fs::remove_all(root, ec);
}

namespace {

/// A minimal loadable PPP-RTK network config with @p network_extra spliced into the
/// network block (each entry ends with a comma).
std::string networkConfig(const std::string& network_extra) {
    return std::string("{\n") +
        "  \"processing\": {\n"
        "    \"mode\": \"ppp-rtk-network\",\n"
        "    \"active_systems\": [ \"GPS\" ],\n"
        "    \"signal_priorities\": { \"GPS\": \"WC\" }\n"
        "  },\n"
        "  \"network\": {\n"
        "    \"clock_datum_station\": \"AAAA\",\n"
        + network_extra +
        "    \"stations\": [\n"
        "      { \"name\": \"AAAA\", \"position_ecef\": [ -2493304.99, -4655214.79, 3565497.67 ] },\n"
        "      { \"name\": \"BBBB\", \"position_ecef\": [ -2281621.75, -1453585.08, 5756964.89 ] }\n"
        "    ]\n"
        "  }\n"
        "}\n";
}

std::vector<std::string> loadColumns(const std::string& tag, const std::string& extra) {
    const fs::path p = fs::temp_directory_path() / ("rhs_attrib_columns_" + tag + ".json");
    {
        std::ofstream o(p);
        o << networkConfig(extra);
    }
    LogManager log;
    GnssConfig cfg;
    EXPECT_TRUE(cfg.loadFromJson(p, log));
    std::error_code ec;
    fs::remove(p, ec);
    return cfg.networkRhsAttribColumns();
}

}  // namespace

TEST(RhsAttribColumnsTest, LoaderKeepsStringEntriesInOrder) {
    EXPECT_TRUE(loadColumns("default", "").empty());
    EXPECT_EQ(loadColumns("list",
                          "    \"rhs_attrib_columns\": [\"CLK:G07\", \"CB:G07:C1W\", "
                          "\"CLK:G07\", 3, \"\", \"PB:G07:L1C\"],\n"),
              (std::vector<std::string>{"CLK:G07", "CB:G07:C1W", "PB:G07:L1C"}));
    EXPECT_TRUE(loadColumns("scalar", "    \"rhs_attrib_columns\": \"CLK:G07\",\n").empty())
        << "a non-array is refused and the knob stays inert";
}
