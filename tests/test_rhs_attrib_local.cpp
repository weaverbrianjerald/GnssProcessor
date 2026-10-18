/**
 * @file test_rhs_attrib_local.cpp
 * @brief The attribution's station-local readout (`LOC:<station>:<key>` labels in
 *        @c network.rhs_attrib_columns, @c networkRhsAttribLocalShares and the `loc_<tow>.csv`
 *        writer in network/qc/RhsAttrib.hpp) and the kernel's
 *        @c NetworkNormalEquations::reconstructStationParts.
 *
 * The fixture is a small two-band network solved by the estimator itself: two satellites
 * with a clock and two phase biases each on the border, three stations with code and phase
 * rows on both bands, local columns receiver clock, receiver DCB, two slant ionospheres and
 * four ambiguities, a pre-PD receiver DCB datum prior at every station (the only closure of
 * the station's geometry-free level direction), a post-PD ambiguity carry and a coupling row
 * at two stations, and a datum that pins G01's clock to a non-zero value.  The phase datum
 * is either the first station's ambiguity pins or the ensemble sum-to-zero gauges (the
 * second variant gives the reconstruction its gauge correction); a third variant adds one
 * DD-prior block to the pin datum (the DD correction and the dd_fold class).  Each station
 * has a real state manager, so the local-column names are the production ones.  The
 * contracts:
 *  - the label parser accepts `LOC:` labels and keeps the colons of the key;
 *  - a key resolves on the full column name, else on a unique bare key;
 *  - the column names come from the state manager through the assembly's h2local map, and
 *    the capture records them only when a `LOC:` label asks for them;
 *  - the shares of every named local column close on the kernel's solved increment, each
 *    row's share equals an independent dense solve, and the kind totals sum to the rows and
 *    folds;
 *  - the kernel's reconstruction parts sum to its reconstruction, the gauge and the DD
 *    corrections included;
 *  - the feedback's split sums to the feedback by shared column and, through the shared
 *    solution, over every station's parts, the folds and the datum; the target station's
 *    own part equals the shared-path shares written beside its rows; with the ensemble
 *    gauges the class closure's gap and the split's FBCLOSURE are exactly the gauges'
 *    right-hand side, formed independently;
 *  - the station nets are sorted, sum per station over satellites and with the folds to the
 *    attributed shares plus the split; one refinement step moves no part beyond rounding,
 *    and every PERTURB re-solve measures the attributed whole effect;
 *  - the whole effect of a row (direct plus through the shared solution, or its FBLOS part
 *    for another station's row) equals the change of the solved increment when the row's
 *    prefit is removed and the network re-solved; with a DD-prior block the change also
 *    holds what the row moves through the fold (the dd_fold part and DDPRIOR);
 *  - the geometry-free level direction is found and its shares close too;
 *  - an unknown label keeps its slot, is named once, and the shared labels still go to
 *    cols_ alone; a frame without the station-local capture writes no loc_ file.
 */
#include <gtest/gtest.h>

#include "config/GnssConfig.hpp"
#include "network/assembly/Contributions.hpp"
#include "network/solve/Estimator.hpp"
#include "network/solve/NormalEquations.hpp"
#include "network/qc/RhsAttrib.hpp"
#include "network/assembly/RowClassify.hpp"
#include "network/driver/RunTypes.hpp"
#include "network/state/StationParamsShadow.hpp"
#include "filter/StateKeySchema.hpp"
#include "filter/StateManager.hpp"
#include "logging/LogManager.hpp"
#include "rinex/MultiRinexSync.hpp"

#include <Eigen/Dense>
#include <spdlog/sinks/ostream_sink.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <limits>
#include <map>
#include <memory>
#include <set>
#include <sstream>
#include <string>
#include <vector>
#include "support/TestTempDir.hpp"   // per-process, per-test scratch directory

namespace fs = std::filesystem;
using namespace GnssProcessor;

namespace {

using CK = ContributionKind;

constexpr double kMu = 1.6469;   ///< (f1/f2)^2 for GPS L1/L2.

/// Local column layout of every station.
enum Col : Eigen::Index { kClk = 0, kDcb, kIon1, kIon2, kN1a, kN2a, kN1b, kN2b, kNLocal };

/// The fixture network and its solve.
struct Fixture {
    LogManager                   lm;
    GnssConfig                   cfg;
    NetworkEstimator             est;
    std::vector<NetStation>      stations;
    std::vector<StationAssembly> asm_res;
    std::vector<int>             est_index_of;
    NetworkEstimator::Datum      datum;
    Eigen::VectorXd              x_s;
    Eigen::Index clk[2]{-1, -1};
    Eigen::Index pb1[2]{-1, -1};
    Eigen::Index pb2[2]{-1, -1};
};

/// A state manager whose states are the station's local columns, and the h2local map
/// that puts each state on its column.
void buildStateManager(Fixture& f, NetStation& st, StationAssembly& R) {
    st.sm = std::make_unique<StateManager>(5, f.lm, f.cfg);
    StateManager& sm = *st.sm;
    ASSERT_TRUE(sm.hasClock());
    const auto put = [&](std::size_t sidx, Eigen::Index col) {
        R.h2local[static_cast<int>(sidx)] = static_cast<int>(col);
    };
    put(sm.getClockIndex(), kClk);
    put(sm.addCodeBiasState(StateKeys::makeDcbKey("GPS"), 0.0), kDcb);
    put(sm.addIonosphereState("G01", 0.0), kIon1);
    put(sm.addIonosphereState("G02", 0.0), kIon2);
    put(sm.addAmbiguity(StateKeys::makeAmbiguityKey("G01", "L1C"), 0.0), kN1a);
    put(sm.addAmbiguity(StateKeys::makeAmbiguityKey("G01", "L2W"), 0.0), kN2a);
    put(sm.addAmbiguity(StateKeys::makeAmbiguityKey("G02", "L1C"), 0.0), kN1b);
    put(sm.addAmbiguity(StateKeys::makeAmbiguityKey("G02", "L2W"), 0.0), kN2b);
    // The station's parameter set, as the split's seam builds it.
    buildStationParams(sm, std::vector<ParamDisposition>(sm.getStateSize(), ParamDisposition::EstimatedLocal),
                       StationAbsorbed{}, st.params);
}

/// One station's rows: per satellite P1, P2, L1, L2; the first station pins its four
/// ambiguities when @p pins; stations 1 and 2 carry a coupling row on G02 L1C.
void buildRows(Fixture& f, int r, bool pins, StationAssembly& R) {
    const bool pivot = pins && r == 0;
    const bool coupled = r > 0;
    const Eigen::Index m = 8 + (pivot ? 4 : 0) + (coupled ? 1 : 0);
    const Eigen::Index ns = f.est.numShared();
    R.ok = true;
    R.A_s = Eigen::MatrixXd::Zero(m, ns);
    R.A_r = Eigen::MatrixXd::Zero(m, kNLocal);
    R.w = Eigen::VectorXd::Ones(m);
    R.y = Eigen::VectorXd::Zero(m);
    const char* sats[2] = {"G01", "G02"};
    const Eigen::Index ion[2] = {kIon1, kIon2};
    const Eigen::Index n1[2] = {kN1a, kN1b};
    const Eigen::Index n2[2] = {kN2a, kN2b};
    Eigen::Index row = 0;
    for (int s = 0; s < 2; ++s) {
        const double dy = 0.37 * r - 0.21 * s;
        // P1, P2: code at 0.3 m.
        R.A_s(row, f.clk[s]) = -1.0;
        R.A_r(row, kClk) = 1.0;
        R.A_r(row, ion[s]) = 1.0;
        R.w(row) = 1.0 / 0.09;
        R.y(row) = 3.1 + dy;
        R.row_tags.push_back(makeRowTag(CK::DataCode, sats[s], "C1C"));
        ++row;
        R.A_s(row, f.clk[s]) = -1.0;
        R.A_r(row, kClk) = 1.0;
        R.A_r(row, ion[s]) = kMu;
        R.A_r(row, kDcb) = 1.0;
        R.w(row) = 1.0 / 0.09;
        R.y(row) = 4.6 - 0.8 * dy;
        R.row_tags.push_back(makeRowTag(CK::DataCode, sats[s], "C2W"));
        ++row;
        // L1, L2: phase at 0.01 m, weighted per station (stations 1 and 2 would otherwise
        // share their normals, and a DD block over them would have no shared projection).
        R.A_s(row, f.clk[s]) = -1.0;
        R.A_s(row, f.pb1[s]) = 1.0;
        R.A_r(row, kClk) = 1.0;
        R.A_r(row, ion[s]) = -1.0;
        R.A_r(row, n1[s]) = 1.0;
        R.w(row) = 1.0e4 * (1.0 + 0.25 * r);
        R.y(row) = 1.9 + 0.5 * dy;
        R.row_tags.push_back(makeRowTag(CK::DataPhase, sats[s], "L1C"));
        ++row;
        R.A_s(row, f.clk[s]) = -1.0;
        R.A_s(row, f.pb2[s]) = 1.0;
        R.A_r(row, kClk) = 1.0;
        R.A_r(row, ion[s]) = -kMu;
        R.A_r(row, n2[s]) = 1.0;
        R.w(row) = 1.0e4 * (1.0 + 0.25 * r);
        R.y(row) = 2.7 - 0.3 * dy;
        R.row_tags.push_back(makeRowTag(CK::DataPhase, sats[s], "L2W"));
        ++row;
    }
    R.n_data_rows = row;
    if (pivot) {
        const Eigen::Index amb[4] = {kN1a, kN2a, kN1b, kN2b};
        const char* sig[4] = {"L1C", "L2W", "L1C", "L2W"};
        for (int a = 0; a < 4; ++a) {
            R.A_r(row, amb[a]) = 1.0;
            R.w(row) = 1.0e10;
            R.y(row) = 0.01 * (a + 1);
            R.row_tags.push_back(makeRowTag(CK::AmbiguityPin, sats[a / 2], sig[a]));
            ++row;
        }
        R.np = 4;
    }
    if (coupled) {
        R.A_r(row, kN1b) = 1.0;
        R.w(row) = 50.0;
        R.y(row) = 0.2 * r;
        R.row_tags.push_back(makeRowTag(CK::CouplingScalar, "G02", "L1C"));
        R.cpl_row_start = row;
        ++row;
    }
    R.prior_cols = {kDcb};
    R.prior_dNrr = Eigen::VectorXd::Constant(1, 0.01);
    R.prior_dur = Eigen::VectorXd::Constant(1, 0.01 * (0.0 - (0.92 + 0.5 * r)));
    R.prior_tags = {RowTag{CK::PriorRxCbDatumPrePd, {}, {}}};
    if (coupled) {
        R.carry_cols = {kN2a};
        R.carry_dNrr = Eigen::VectorXd::Constant(1, 40.0);
        R.carry_dur = Eigen::VectorXd::Constant(1, 40.0 * (0.125 * r));
        R.carry_tags = {makeRowTag(CK::PriorAmbiguityCarry, "G01", "L2W")};
    }
}

/// Build, fold and solve the fixture.  @p pins: the first station's ambiguity pins are
/// the phase datum; otherwise the ensemble sum-to-zero gauges are.  @p zero_sta /
/// @p zero_row: one row whose prefit is set to zero (a perturbation of the fixture).
/// @p dd: also fold one DD-prior block on the G01/G02 L1C double difference of stations 1
/// and 2 (the invariant DD carry; it needs the pin datum, the two folds being exclusive).
void solveFixture(Fixture& f, bool pins, int zero_sta = -1, Eigen::Index zero_row = -1,
                  bool dd = false) {
    const char* sats[2] = {"G01", "G02"};
    for (int s = 0; s < 2; ++s) {
        f.clk[s] = f.est.registerClock(sats[s]);
        f.pb1[s] = f.est.registerPhaseBias(sats[s], "L1C");
        f.pb2[s] = f.est.registerPhaseBias(sats[s], "L2W");
    }
    f.est.attribArm(true);
    f.est.beginEpoch();
    f.stations.resize(3);
    f.asm_res.resize(3);
    f.est_index_of.assign(3, -1);
    std::vector<std::size_t> slots;
    for (int r = 0; r < 3; ++r) {
        NetStation& st = f.stations[static_cast<std::size_t>(r)];
        st.name = "STA" + std::to_string(r);
        StationAssembly& R = f.asm_res[static_cast<std::size_t>(r)];
        buildStateManager(f, st, R);
        buildRows(f, r, pins, R);
        if (r == zero_sta) R.y(zero_row) = 0.0;
        const NetworkNormalEquations::StationPrior prior{R.prior_cols, R.prior_dNrr, R.prior_dur};
        const std::size_t slot = f.est.addStation(R.A_s, R.A_r, R.w, R.y, prior);
        f.est_index_of[static_cast<std::size_t>(r)] = static_cast<int>(slot);
        slots.push_back(slot);
        if (!R.carry_cols.empty()) {
            f.est.addStationPrior(slot, R.carry_cols, R.carry_dNrr, R.carry_dur);
            R.carry_prior_folded = true;
        }
    }
    if (dd) {
        ASSERT_TRUE(pins) << "the DD priors and the ensemble gauges are exclusive";
        NetworkNormalEquations::DdPriorBlock blk;
        blk.rows.push_back({{slots[1], kN1a, 1.0}, {slots[1], kN1b, -1.0},
                            {slots[2], kN1a, -1.0}, {slots[2], kN1b, 1.0}});
        blk.Pz = Eigen::MatrixXd::Constant(1, 1, 1.0e-4);
        blk.zhat = Eigen::VectorXd::Constant(1, 0.3);
        f.est.contributionKind(CK::DdPriorBlock);
        f.est.setDdPriors({blk});
    }
    if (!pins) {
        std::vector<NetworkNormalEquations::SumZeroGauge> gauges;
        const Eigen::Index amb[4] = {kN1a, kN2a, kN1b, kN2b};
        const Eigen::Index pbc[4] = {f.pb1[0], f.pb2[0], f.pb1[1], f.pb2[1]};
        for (int a = 0; a < 4; ++a) {
            NetworkNormalEquations::SumZeroGauge g;
            for (const std::size_t slot : slots) g.terms.emplace_back(slot, amb[a]);
            g.shared_col = pbc[a];
            gauges.push_back(std::move(g));
        }
        f.est.setPhaseSumZeroGauges(std::move(gauges));
    }
    NetworkEstimator::SharedPrior p;
    p.kind = SharedParamKind::Clock;
    p.sat = "G02";
    p.value = 0.1;
    p.info = 4.0;
    f.est.contributionKind(CK::BorderCarryDiagonal);
    f.est.addSharedPrior({p});
    f.datum = {{f.clk[0], 0.3}};
    f.x_s = f.est.solve(f.datum);
}

/// Capture the fixture's frame; @p local asks for the station-local readout's inputs, as
/// the driver does when a LOC: label is named.
NetworkAttribFrame captureFrame(Fixture& f, bool with_xs = true, bool local = true) {
    NetworkAttribFrame frame;
    SyncedEpoch se;
    se.time.week = 2313;
    se.time.tow = 260040.0;
    networkAttribCaptureFrame(frame, se, f.stations, f.est, f.datum, f.asm_res,
                              f.est_index_of, /*own_assemblies=*/false,
                              with_xs ? &f.x_s : nullptr, local);
    return frame;
}

/// The station's local normals and cross block formed densely from its assembly (the
/// independent route): rows, the pre-PD prior and the folded post-PD prior.
void denseBlocks(const StationAssembly& R, Eigen::MatrixXd& Nrr, Eigen::MatrixXd& Nsr,
                 Eigen::VectorXd& ur) {
    const Eigen::MatrixXd W = R.w.asDiagonal();
    Nrr = R.A_r.transpose() * W * R.A_r;
    Nsr = R.A_s.transpose() * W * R.A_r;
    ur = R.A_r.transpose() * W * R.y;
    for (std::size_t e = 0; e < R.prior_cols.size(); ++e) {
        Nrr(R.prior_cols[e], R.prior_cols[e]) += R.prior_dNrr(static_cast<Eigen::Index>(e));
        ur(R.prior_cols[e]) += R.prior_dur(static_cast<Eigen::Index>(e));
    }
    if (R.carry_prior_folded)
        for (std::size_t e = 0; e < R.carry_cols.size(); ++e) {
            Nrr(R.carry_cols[e], R.carry_cols[e]) += R.carry_dNrr(static_cast<Eigen::Index>(e));
            ur(R.carry_cols[e]) += R.carry_dur(static_cast<Eigen::Index>(e));
        }
}

const NetworkRhsLocalShares::Part* findPart(const NetworkRhsLocalShares::Target& t,
                                            const std::string& record, const std::string& kind,
                                            Eigen::Index row = -1) {
    for (const auto& q : t.parts)
        if (q.record == record && q.kind == kind && (row < 0 || q.row == row)) return &q;
    return nullptr;
}

std::string readFile(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

std::size_t countOf(const std::string& hay, const std::string& needle) {
    std::size_t n = 0;
    for (std::size_t pos = hay.find(needle); pos != std::string::npos;
         pos = hay.find(needle, pos + needle.size()))
        ++n;
    return n;
}

/// The magnitude the reconstruction parts cancel from (their tolerance scale).
double partsScale(const NetworkNormalEquations::StationReconstructionParts& p) {
    return std::max({1.0, p.free.cwiseAbs().maxCoeff(), p.feedback.cwiseAbs().maxCoeff(),
                     p.sumzero.cwiseAbs().maxCoeff(), p.ddprior.cwiseAbs().maxCoeff()});
}

/// The relative rounding a solve with @p N can show: a small multiple of the machine
/// epsilon times its condition number (two routes through the same normals agree to it).
double solveRounding(const Eigen::MatrixXd& N) {
    const Eigen::SelfAdjointEigenSolver<Eigen::MatrixXd> es(N);
    const Eigen::VectorXd ev = es.eigenvalues().cwiseAbs();
    const double cond = ev.maxCoeff() / std::max(ev.minCoeff(), 1e-300);
    return std::max(1e-12, 64.0 * std::numeric_limits<double>::epsilon() * cond);
}

/// Check every resolved target of @p ls against the kernel and the dense route.
/// @p fb_complete: the class decomposition attributes every fold of the reduced system, so
/// the feedback's network split must close on the feedback.
void checkClosure(const Fixture& f, const NetworkAttribFrame& frame,
                  const NetworkRhsLocalShares& ls, bool fb_complete = true) {
    for (const auto& t : ls.targets) {
        SCOPED_TRACE(t.label);
        ASSERT_TRUE(t.resolved) << t.note;
        const std::size_t k = static_cast<std::size_t>(
            std::find(frame.sta_name.begin(), frame.sta_name.end(), t.station)
            - frame.sta_name.begin());
        ASSERT_LT(k, frame.sta_name.size());
        // The shares account for the solve's increment, to the rounding of the station's
        // normals.
        const double tol = solveRounding(frame.Nrr[k]) * std::max(1.0, t.scale);
        EXPECT_LE(t.closure_abs, tol);
        EXPECT_LE(std::abs(t.unattributed), tol) << "every channel of u_r is attributed";
        // The solved value is the estimator's own reconstruction.
        const std::size_t slot = frame.slot[k];
        const Eigen::VectorXd xr = f.est.reconstructStation(slot, f.x_s);
        if (t.local_col >= 0) EXPECT_EQ(t.solved, xr(t.local_col));
        // The kind totals sum to the rows and folds.
        double kinds = 0.0;
        for (const auto& q : t.parts) if (q.record == "KIND") kinds += q.share;
        EXPECT_NEAR(kinds, t.attributed, 1e-12 * std::max(1.0, t.scale));
        // FOLDLOS splits FOLD exactly.
        std::map<std::pair<std::string, std::string>, double> los;
        for (const auto& q : t.parts)
            if (q.record == "FOLDLOS") los[{q.channel, q.kind}] += q.share;
        for (const auto& q : t.parts) {
            if (q.record != "FOLD") continue;
            const double los_sum = los[std::make_pair(q.channel, q.kind)];
            EXPECT_NEAR(los_sum, q.share, 1e-13 * std::max(1.0, t.scale))
                << q.channel << " " << q.kind;
        }
        // The feedback's split: by shared column it sums to the feedback; through the shared
        // solution it sums to h^T x_c, which is the feedback at the solve's x_s; the target
        // station's own part equals the shared-path shares written beside its rows.
        ASSERT_TRUE(t.fb_split);
        double cols = 0.0, sta = 0.0, folds = 0.0, own = 0.0, own_rows = 0.0, fb_scale = 1.0;
        for (const auto& q : t.parts) {
            if (q.record == "FBCOL") cols += q.share;
            if (q.record == "FBSTA") {
                sta += q.share;
                if (q.from == t.station) own += q.share;
                fb_scale = std::max(fb_scale, std::abs(q.share));
            }
            if (q.record == "FBFOLD") {
                folds += q.share;
                fb_scale = std::max(fb_scale, std::abs(q.share));
            }
            if ((q.record == "ROW" || q.record == "FOLD") && !std::isnan(q.share_shared))
                own_rows += q.share_shared;
        }
        EXPECT_NEAR(cols, t.feedback, 1e-12 * std::max(1.0, t.scale));
        EXPECT_NEAR(sta + folds, t.fb_network, 1e-12 * fb_scale);
        if (fb_complete) EXPECT_LE(t.fb_closure, 1e-7 * fb_scale);
        EXPECT_NEAR(own, own_rows, 1e-7 * fb_scale);

        // The station nets: sorted by |net|; the target station's net is its direct shares
        // plus its shared-path part, another station's its shared-path part; each station's
        // satellite nets sum to its net; the nets and the folds sum to the attributed shares
        // plus the split.
        std::map<std::string, double> net, net_shared, sat_sum, kind_sum;
        double nets = 0.0, prev = std::numeric_limits<double>::infinity();
        for (const auto& q : t.parts) {
            if (q.record == "FBSTA") kind_sum[q.from] += q.share;
            if (q.record == "NETSAT") sat_sum[q.from] += q.share;
            if (q.record != "NETSTA") continue;
            EXPECT_LE(std::abs(q.share), prev) << "NETSTA sorted by |net|";
            prev = std::abs(q.share);
            net[q.from] = q.share;
            net_shared[q.from] = q.share_shared;
            nets += q.share;
        }
        ASSERT_EQ(net.size(), frame.sta_name.size());
        for (const auto& [s, v] : net) {
            const double direct = (s == t.station) ? t.attributed : 0.0;
            EXPECT_NEAR(v, direct + kind_sum[s], 1e-12 * fb_scale) << s;
            EXPECT_NEAR(net_shared[s], kind_sum[s], 1e-12 * fb_scale) << s;
            EXPECT_NEAR(sat_sum[s], v, 1e-12 * fb_scale) << s;
        }
        EXPECT_NEAR(nets + folds, t.attributed + t.fb_network, 1e-12 * fb_scale);

        // The checks of single parts: one refinement step moves no part beyond the solves'
        // rounding, and every re-solve without a group's prefits measures the attributed
        // whole effect (the re-solve holds the folds, so this holds with them too).
        const double part_tol = 1e-7 * std::max({1.0, fb_scale, t.scale});
        EXPECT_LE(t.refine_max, part_tol) << t.refine_part;
        EXPECT_FALSE(t.refine_part.empty());
        EXPECT_EQ(t.n_perturb, 4) << "own row, other station's LOS group, own and other station";
        int n_checks = 0;
        for (const auto& q : t.parts) {
            if (q.record != "PERTURB") continue;
            ++n_checks;
            EXPECT_GT(q.n, 0) << q.channel << " " << q.from;
            EXPECT_NEAR(q.check, q.share, part_tol) << q.channel << " " << q.from;
        }
        EXPECT_EQ(n_checks, t.n_perturb);
        EXPECT_LE(t.perturb_max_err, part_tol);
    }
}

}  // namespace

TEST(RhsAttribLocalTest, LabelParserAcceptsLocAndKeepsTheKeysColons) {
    EXPECT_TRUE(isRhsAttribLocalLabel("LOC:MAJU00MHL:GPS_DCB"));
    EXPECT_FALSE(isRhsAttribLocalLabel("CLK:G08"));
    EXPECT_FALSE(isRhsAttribLocalLabel("loc:MAJU00MHL:GPS_DCB"));
    const RhsAttribLocalLabel a = parseRhsAttribLocalLabel("LOC:MAJU00MHL:GPS_IFB5");
    EXPECT_EQ(a.label, "LOC:MAJU00MHL:GPS_IFB5");
    EXPECT_EQ(a.station, "MAJU00MHL");
    EXPECT_EQ(a.key, "GPS_IFB5");
    const RhsAttribLocalLabel b = parseRhsAttribLocalLabel("LOC:MAJU00MHL:IONO:G08");
    EXPECT_EQ(b.station, "MAJU00MHL");
    EXPECT_EQ(b.key, "IONO:G08");
    for (const char* bad : {"LOC:", "LOC:MAJU00MHL", "LOC:MAJU00MHL:", "LOC::GPS_DCB"}) {
        const RhsAttribLocalLabel m = parseRhsAttribLocalLabel(bad);
        EXPECT_TRUE(m.station.empty() || m.key.empty()) << bad;
    }
}

TEST(RhsAttribLocalTest, KeyResolvesOnTheFullNameThenOnAUniqueBareKey) {
    const std::vector<std::string> names{"CLK",      "CBIAS:GPS_DCB", "CBIAS:GPS_IFB5",
                                         "IONO:G03", "PCOL:G03",      "AMB:G03_L1C",
                                         "",         "CBIAS:GPS_ISC_C1W"};
    EXPECT_EQ(rhsAttribResolveLocalKey(names, "CLK"), 0);
    EXPECT_EQ(rhsAttribResolveLocalKey(names, "GPS_DCB"), 1);
    EXPECT_EQ(rhsAttribResolveLocalKey(names, "CBIAS:GPS_DCB"), 1);
    EXPECT_EQ(rhsAttribResolveLocalKey(names, "GPS_IFB5"), 2);
    EXPECT_EQ(rhsAttribResolveLocalKey(names, "GPS_ISC_C1W"), 7);
    EXPECT_EQ(rhsAttribResolveLocalKey(names, "G03_L1C"), 5);
    EXPECT_EQ(rhsAttribResolveLocalKey(names, "IONO:G03"), 3);
    EXPECT_EQ(rhsAttribResolveLocalKey(names, "G03"), -2) << "two columns share the bare key";
    EXPECT_EQ(rhsAttribResolveLocalKey(names, "GPS_IFB2"), -1);
    EXPECT_EQ(rhsAttribResolveLocalKey(names, ""), -1);
}

TEST(RhsAttribLocalTest, SharesCloseOnTheSolvedIncrementAndMatchADenseSolve) {
    Fixture f;
    solveFixture(f, /*pins=*/true);
    const NetworkAttribFrame frame = captureFrame(f);
    ASSERT_TRUE(frame.valid);
    ASSERT_EQ(frame.local_names.size(), 3u);
    ASSERT_EQ(frame.local_parts.size(), 3u);

    // The names are the state manager's labels, read from the station's parameter set through
    // h2local.
    const std::vector<std::string>& nm = frame.local_names[1];
    ASSERT_EQ(nm.size(), static_cast<std::size_t>(kNLocal));
    EXPECT_EQ(nm[kClk], "CLK");
    EXPECT_EQ(nm[kDcb], "CBIAS:GPS_DCB");
    EXPECT_EQ(nm[kIon2], "IONO:G02");
    EXPECT_EQ(nm[kN1a], "AMB:" + StateKeys::makeAmbiguityKey("G01", "L1C"));
    EXPECT_EQ(rhsAttribLocalColumnNames(f.asm_res[1], f.stations[1].params), nm);
    // ... and are exactly the labels the state manager gives the same columns.
    const std::vector<std::string> sm_labels = f.stations[1].sm->getStateLabels();
    for (const auto& [sidx, col] : f.asm_res[1].h2local)
        EXPECT_EQ(nm[static_cast<std::size_t>(col)], sm_labels[static_cast<std::size_t>(sidx)]) << sidx;

    // The kernel's parts sum to its reconstruction.
    for (std::size_t k = 0; k < 3; ++k) {
        const auto& p = frame.local_parts[k];
        const Eigen::VectorXd sum = p.free + p.feedback + p.sumzero + p.ddprior;
        EXPECT_LE((sum - p.x_r).cwiseAbs().maxCoeff(),
                  solveRounding(frame.Nrr[k]) * partsScale(p)) << k;
        EXPECT_EQ(p.x_r, f.est.reconstructStation(frame.slot[k], f.x_s));
        EXPECT_EQ(p.sumzero.cwiseAbs().maxCoeff(), 0.0) << "no gauge on the pin datum";
    }

    const std::vector<std::string> labels{"LOC:STA1:GPS_DCB", "LOC:STA1:CLK",
                                          "LOC:STA1:IONO:G02",
                                          "LOC:STA1:" + StateKeys::makeAmbiguityKey("G02", "L1C"),
                                          "LOC:STA0:GPS_DCB"};
    const NetworkRhsAttribDecomposition dec = networkRhsAttribDecompose(frame);
    const NetworkRhsLocalShares ls = networkRhsAttribLocalShares(frame, dec, labels);
    ASSERT_TRUE(ls.valid);
    ASSERT_EQ(ls.targets.size(), labels.size() + 2) << "one GFDIR per named station";
    EXPECT_EQ(ls.targets[5].label, "GFDIR:STA1");
    EXPECT_EQ(ls.targets[6].label, "GFDIR:STA0");
    checkClosure(f, frame, ls);

    // The independent route at STA1: each row's share is w y a_i^T N_rr^-1 e_k, each prior
    // entry's eta (N_rr^-1)_{l,k}, the feedback -e_k^T N_rr^-1 N_sr^T x_s.
    const StationAssembly& R = f.asm_res[1];
    Eigen::MatrixXd Nrr, Nsr;
    Eigen::VectorXd ur;
    denseBlocks(R, Nrr, Nsr, ur);
    const Eigen::MatrixXd Ninv = Nrr.inverse();
    for (std::size_t j = 0; j < 4; ++j) {
        const auto& t = ls.targets[j];
        SCOPED_TRACE(t.label);
        const Eigen::Index c = t.local_col;
        ASSERT_GE(c, 0);
        const double tol = 1e-8 * std::max(1.0, t.scale);
        int n_rows = 0;
        for (Eigen::Index row = 0; row < R.n_data_rows; ++row) {
            const CK kind = R.row_tags[static_cast<std::size_t>(row)].kind;
            const auto* q = findPart(t, "ROW", contributionKindName(kind), row);
            ASSERT_NE(q, nullptr) << row;
            EXPECT_NEAR(q->share, R.w(row) * R.y(row) * R.A_r.row(row).dot(Ninv.col(c)), tol);
            EXPECT_EQ(q->a, R.A_r(row, c));
            EXPECT_EQ(q->w, R.w(row));
            EXPECT_EQ(q->y, R.y(row));
            ++n_rows;
        }
        EXPECT_EQ(n_rows, 8);
        const auto* cpl = findPart(t, "FOLD", "coupling_scalar");
        ASSERT_NE(cpl, nullptr);
        EXPECT_EQ(cpl->channel, "row");
        EXPECT_NEAR(cpl->share, R.w(8) * R.y(8) * R.A_r.row(8).dot(Ninv.col(c)), tol);
        const auto* dcb = findPart(t, "FOLD", "prior_rx_cb_datum_pre_pd");
        ASSERT_NE(dcb, nullptr);
        EXPECT_EQ(dcb->channel, "pre_pd");
        EXPECT_NEAR(dcb->share, R.prior_dur(0) * Ninv(kDcb, c), tol);
        const auto* carry = findPart(t, "FOLD", "prior_ambiguity_carry");
        ASSERT_NE(carry, nullptr);
        EXPECT_EQ(carry->channel, "post_pd");
        EXPECT_NEAR(carry->share, R.carry_dur(0) * Ninv(kN2a, c), tol);
        const auto* fb = findPart(t, "FEEDBACK", "");
        ASSERT_NE(fb, nullptr);
        EXPECT_NEAR(fb->share, -Ninv.col(c).dot(Nsr.transpose() * f.x_s), tol);
        EXPECT_NEAR(t.solved, (Ninv * (ur - Nsr.transpose() * f.x_s))(c), tol);
    }

    // The level direction: data rows leave it free, and its loadings are the model's
    // (ionosphere 1, receiver clock -1, DCB 1 - mu, L1 ambiguities 2, L2 ambiguities 1 + mu).
    const auto& gf = ls.targets[5];
    ASSERT_TRUE(gf.resolved) << gf.note;
    const auto loading = [&](const std::string& key) {
        const std::size_t at = gf.name.find(key + "=");
        EXPECT_NE(at, std::string::npos) << key << " in " << gf.name;
        return at == std::string::npos ? 0.0 : std::stod(gf.name.substr(at + key.size() + 1));
    };
    EXPECT_NEAR(loading("CLK"), -1.0, 1e-4);
    EXPECT_NEAR(loading("CBIAS:GPS_DCB"), 1.0 - kMu, 1e-4);
    EXPECT_NEAR(loading("IONO_mean"), 1.0, 1e-4);
    EXPECT_NEAR(loading("AMB_mean"), (2.0 + 1.0 + kMu) / 2.0, 1e-4);
    for (const auto& q : gf.parts)
        if (q.record == "ROW") EXPECT_NEAR(q.a, 0.0, 1e-9) << "a data row has no component";
}

// The whole effect of one row on a station-local increment, measured by removing the row's
// prefit and re-solving: for a row of the target station it is the direct share plus the
// share through the shared solution, for a row of another station its FBLOS part.
TEST(RhsAttribLocalTest, WholeEffectOfARowEqualsTheChangeWhenItsPrefitIsRemoved) {
    const std::string label = "LOC:STA1:GPS_DCB";
    Fixture f;
    solveFixture(f, /*pins=*/true);
    const NetworkAttribFrame frame = captureFrame(f);
    const NetworkRhsAttribDecomposition dec = networkRhsAttribDecompose(frame);
    const NetworkRhsLocalShares ls = networkRhsAttribLocalShares(frame, dec, {label}, false);
    ASSERT_TRUE(ls.valid);
    ASSERT_EQ(ls.targets.size(), 1u);
    const auto& t = ls.targets[0];
    ASSERT_TRUE(t.resolved && t.fb_split) << t.note;
    const double x_full = t.solved;
    const double tol = 1e-7 * std::max(1.0, t.scale);

    // Row 1 of STA1 (its own G01 C2W code row).
    {
        Fixture g;
        solveFixture(g, /*pins=*/true, /*zero_sta=*/1, /*zero_row=*/1);
        const Eigen::VectorXd xr =
            g.est.reconstructStation(static_cast<std::size_t>(g.est_index_of[1]), g.x_s);
        const auto* q = findPart(t, "ROW", "data_code", 1);
        ASSERT_NE(q, nullptr);
        EXPECT_NEAR(q->share + q->share_shared, x_full - xr(kDcb), tol);
        EXPECT_GT(std::abs(q->share_shared), 1e-9) << "an own row also acts through x_s";
    }
    // Row 4 of STA2 (its G02 C1C code row): no direct share, the whole effect through x_s.
    {
        Fixture g;
        solveFixture(g, /*pins=*/true, /*zero_sta=*/2, /*zero_row=*/4);
        const Eigen::VectorXd xr =
            g.est.reconstructStation(static_cast<std::size_t>(g.est_index_of[1]), g.x_s);
        const NetworkRhsLocalShares::Part* los = nullptr;
        for (const auto& q : t.parts)
            if (q.record == "FBLOS" && q.from == "STA2" && q.kind == "data_code"
                && q.sat == "G02" && q.signal == "1C")
                los = &q;
        ASSERT_NE(los, nullptr);
        EXPECT_EQ(los->n, 1);
        EXPECT_NEAR(los->share, x_full - xr(kDcb), tol);
        EXPECT_GT(std::abs(los->share), 1e-9);
    }
}

TEST(RhsAttribLocalTest, GaugeCorrectionIsItsOwnShareAndTheSharesStillClose) {
    Fixture f;
    solveFixture(f, /*pins=*/false);
    const NetworkAttribFrame frame = captureFrame(f);
    ASSERT_TRUE(frame.valid);
    double sz = 0.0;
    for (std::size_t k = 0; k < frame.local_parts.size(); ++k) {
        const auto& p = frame.local_parts[k];
        sz = std::max(sz, p.sumzero.cwiseAbs().maxCoeff());
        const Eigen::VectorXd sum = p.free + p.feedback + p.sumzero + p.ddprior;
        EXPECT_LE((sum - p.x_r).cwiseAbs().maxCoeff(),
                  solveRounding(frame.Nrr[k]) * partsScale(p)) << k;
    }
    EXPECT_GT(sz, 1e-6) << "the ensemble gauges correct the reconstruction";

    const std::vector<std::string> labels{
        "LOC:STA2:" + StateKeys::makeAmbiguityKey("G01", "L1C"), "LOC:STA2:GPS_DCB"};
    const NetworkRhsAttribDecomposition dec = networkRhsAttribDecompose(frame);
    const NetworkRhsLocalShares ls = networkRhsAttribLocalShares(frame, dec, labels);
    ASSERT_TRUE(ls.valid);
    checkClosure(f, frame, ls, /*fb_complete=*/false);
    const auto* sz_part = findPart(ls.targets[0], "SUMZERO", "");
    ASSERT_NE(sz_part, nullptr);
    EXPECT_GT(std::abs(sz_part->share), 1e-6);

    // The class decomposition does not attribute the right-hand side the ensemble gauges
    // fold into the reduced system.  Formed independently from the dense station blocks
    // (per gauge n_r = N_rr^-1 e_col, beta = sum (n_r)_col, g = sum N_sr n_r,
    // gamma = sum (N_rr^-1 u_r)_col; the fold adds g gamma / beta), that right-hand side is
    // exactly the class closure's gap, and its share through the shared solution is
    // exactly the gap of the feedback's split (FBCLOSURE), so both gaps are that one term.
    const Eigen::Index n = frame.n;
    std::vector<Eigen::MatrixXd> Nrr(3), Nsr(3);
    std::vector<Eigen::VectorXd> ur(3);
    for (std::size_t r = 0; r < 3; ++r) denseBlocks(f.asm_res[r], Nrr[r], Nsr[r], ur[r]);
    Eigen::VectorXd gauge_rhs = Eigen::VectorXd::Zero(n);
    for (const Eigen::Index col : {Eigen::Index{kN1a}, Eigen::Index{kN2a}, Eigen::Index{kN1b},
                                   Eigen::Index{kN2b}}) {
        double beta = 0.0, gamma = 0.0;
        Eigen::VectorXd g = Eigen::VectorXd::Zero(n);
        for (std::size_t r = 0; r < 3; ++r) {
            const Eigen::VectorXd n_r = Nrr[r].ldlt().solve(Eigen::VectorXd::Unit(kNLocal, col));
            beta += n_r(col);
            g += Nsr[r] * n_r;
            gamma += Nrr[r].ldlt().solve(ur[r])(col);
        }
        gauge_rhs += g * (gamma / beta);
    }
    ASSERT_GT(gauge_rhs.cwiseAbs().maxCoeff(), 1e-3) << "the gauges fold a right-hand side";
    Eigen::VectorXd cls_sum = Eigen::VectorXd::Zero(n);
    for (const auto& [cls, v] : dec.class_total) cls_sum += v;
    const double rhs_tol = 1e-9 * std::max(1.0, frame.b_red.cwiseAbs().maxCoeff());
    EXPECT_LE((cls_sum + gauge_rhs - frame.b_red).cwiseAbs().maxCoeff(), rhs_tol)
        << "the class closure's gap is the gauges' right-hand side";

    // The solve's constrained operator, formed densely (the datum pins G01's clock), and
    // each target's feedback gain g_s = N_c^-1 h, h = -N_sr N_rr^-1 e_k, pinned entry zero.
    Eigen::MatrixXd Nc = frame.N_red;
    const Eigen::Index pc = f.clk[0];
    const double s = std::max(1.0, frame.N_red.diagonal().cwiseAbs().maxCoeff());
    Nc.row(pc).setZero();
    Nc.col(pc).setZero();
    Nc(pc, pc) = s;
    for (const auto& t : ls.targets) {
        SCOPED_TRACE(t.label);
        ASSERT_TRUE(t.resolved && t.fb_split);
        if (t.local_col < 0) continue;   // the level direction: covered by checkClosure
        const Eigen::VectorXd h =
            -(Nsr[2] * Nrr[2].ldlt().solve(Eigen::VectorXd::Unit(kNLocal, t.local_col)));
        Eigen::VectorXd gs = Nc.ldlt().solve(h);
        gs(pc) = 0.0;
        const double gap = -gs.dot(gauge_rhs);
        EXPECT_GT(std::abs(gap), 1e-6);
        double fb_scale = 1.0;
        for (const auto& q : t.parts)
            if (q.record == "FBSTA" || q.record == "FBFOLD")
                fb_scale = std::max(fb_scale, std::abs(q.share));
        EXPECT_NEAR(t.fb_network - t.feedback, gap, 1e-7 * fb_scale)
            << "FBCLOSURE is the gauges' right-hand side, through the shared solution";
    }
}

// With a DD-prior block the class decomposition holds the fold (its dd_fold class), so both
// closures hold; the reconstruction carries the DD correction (DDPRIOR), the feedback's
// split a dd_fold part, and a row's whole effect is its two shares plus what it moves
// through the fold: the change of the dd_fold part and of DDPRIOR when its prefit is
// removed and the network re-solved.
TEST(RhsAttribLocalTest, DdPriorFoldIsAttributedWholeAndTheSharesClose) {
    const std::string label = "LOC:STA1:" + StateKeys::makeAmbiguityKey("G01", "L1C");
    Fixture f;
    solveFixture(f, /*pins=*/true, -1, -1, /*dd=*/true);
    const NetworkAttribFrame frame = captureFrame(f);
    ASSERT_TRUE(frame.valid);
    ASSERT_EQ(frame.dd_blocks.size(), 1u);
    for (std::size_t k = 0; k < frame.local_parts.size(); ++k) {
        const auto& p = frame.local_parts[k];
        const Eigen::VectorXd sum = p.free + p.feedback + p.sumzero + p.ddprior;
        EXPECT_LE((sum - p.x_r).cwiseAbs().maxCoeff(),
                  solveRounding(frame.Nrr[k]) * partsScale(p)) << k;
        EXPECT_EQ(p.x_r, f.est.reconstructStation(frame.slot[k], f.x_s)) << k;
        if (frame.sta_name[k] == "STA0")
            EXPECT_EQ(p.ddprior.cwiseAbs().maxCoeff(), 0.0) << "STA0 is in no DD term";
        else
            EXPECT_GT(p.ddprior.cwiseAbs().maxCoeff(), 1e-9) << frame.sta_name[k];
    }
    const NetworkRhsAttribDecomposition dec = networkRhsAttribDecompose(frame);
    ASSERT_EQ(dec.dd_parts.size(), 1u);
    EXPECT_LE(dec.closure_rel, 1e-9) << "the class decomposition holds the DD fold";
    const NetworkRhsLocalShares ls =
        networkRhsAttribLocalShares(frame, dec, {label, "LOC:STA1:GPS_DCB"});
    ASSERT_TRUE(ls.valid);
    checkClosure(f, frame, ls, /*fb_complete=*/true);
    const auto ddFold = [](const NetworkRhsLocalShares::Target& t) {
        for (const auto& q : t.parts)
            if (q.record == "FBFOLD" && q.kind == "dd_fold") return q.share;
        return std::numeric_limits<double>::quiet_NaN();
    };
    for (const auto& t : ls.targets) {
        SCOPED_TRACE(t.label);
        EXPECT_GT(std::abs(t.ddprior), 1e-9);
        EXPECT_GT(std::abs(ddFold(t)), 1e-9);
    }

    // Row 1 of STA1 (its G01 C2W code row), removed and re-solved.
    const auto& t = ls.targets[0];
    Fixture g;
    solveFixture(g, /*pins=*/true, /*zero_sta=*/1, /*zero_row=*/1, /*dd=*/true);
    const NetworkAttribFrame frame_g = captureFrame(g);
    const NetworkRhsAttribDecomposition dec_g = networkRhsAttribDecompose(frame_g);
    const NetworkRhsLocalShares ls_g = networkRhsAttribLocalShares(frame_g, dec_g, {label}, false);
    ASSERT_TRUE(ls_g.valid);
    const auto& tg = ls_g.targets[0];
    ASSERT_TRUE(tg.resolved && tg.fb_split) << tg.note;
    const auto* q = findPart(t, "ROW", "data_code", 1);
    ASSERT_NE(q, nullptr);
    const double through_fold = (ddFold(t) - ddFold(tg)) + (t.ddprior - tg.ddprior);
    EXPECT_GT(std::abs(through_fold), 1e-9) << "the row also acts through the DD fold";
    EXPECT_NEAR(t.solved - tg.solved, q->share + q->share_shared + through_fold,
                1e-7 * std::max(1.0, t.scale));
}

TEST(RhsAttribLocalTest, CaptureRecordsTheLocalInputsOnlyWhenALocLabelAsksForThem) {
    EXPECT_TRUE(rhsAttribNamesLocalColumns({"CLK:G01", "LOC:STA1:CLK"}));
    EXPECT_FALSE(rhsAttribNamesLocalColumns({"CLK:G01", "CB:G01:C1W"}));
    EXPECT_FALSE(rhsAttribNamesLocalColumns({}));
    Fixture f;
    solveFixture(f, /*pins=*/true);
    const NetworkAttribFrame plain = captureFrame(f, /*with_xs=*/true, /*local=*/false);
    ASSERT_TRUE(plain.valid);
    EXPECT_EQ(plain.x_s.size(), f.x_s.size()) << "x_s is captured for the cols_ readout";
    EXPECT_TRUE(plain.local_names.empty());
    EXPECT_TRUE(plain.local_parts.empty());
    const NetworkAttribFrame loc = captureFrame(f);
    EXPECT_EQ(loc.local_names.size(), 3u);
    EXPECT_EQ(loc.local_parts.size(), 3u);
    // Without the local inputs the readout keeps every slot, unresolved.
    const NetworkRhsAttribDecomposition dec = networkRhsAttribDecompose(plain);
    const NetworkRhsLocalShares ls = networkRhsAttribLocalShares(plain, dec, {"LOC:STA1:CLK"});
    EXPECT_FALSE(ls.valid);
    ASSERT_EQ(ls.targets.size(), 1u);
    EXPECT_FALSE(ls.targets[0].resolved);
}

TEST(RhsAttribLocalTest, UnknownLabelsKeepTheirSlotAndAreNamedOnce) {
    Fixture f;
    solveFixture(f, /*pins=*/true);
    const NetworkAttribFrame frame = captureFrame(f);
    ASSERT_TRUE(frame.valid);

    std::ostringstream sink_text;
    auto sink = std::make_shared<spdlog::sinks::ostream_sink_mt>(sink_text);
    auto log = std::make_shared<spdlog::logger>("rhs_attrib_local_test", sink);
    log->set_level(spdlog::level::info);

    const fs::path dir = gnss_test::tempDir() / "rhs_attrib_local_test";
    std::error_code ec;
    fs::remove_all(dir, ec);
    const std::vector<std::string> columns{"LOC:STA1:GPS_DCB", "LOC:NOSTA:GPS_DCB",
                                           "CLK:G02",          "LOC:STA1:GPS_IFB5",
                                           "LOC:STA1"};
    std::set<std::string> warned;
    networkRhsAttribDump(frame, log, dir, "armed", columns, &warned);
    networkRhsAttribDump(frame, log, dir, "armed", columns, &warned);
    log->flush();

    // The local file: a TARGET line per label in order, the unresolved ones with a note.
    const std::string loc = readFile(dir / "loc_260040.csv");
    ASSERT_FALSE(loc.empty());
    std::istringstream ls(loc);
    std::string header;
    std::getline(ls, header);
    if (!header.empty() && header.back() == '\r') header.pop_back();
    EXPECT_EQ(header, "week,tow,label,station,name,local_col,record,channel,from_station,row,"
                      "kind,sat,signal,obs,n,w,y,v_post,a_col,share,share_shared,check");
    std::vector<std::string> targets;
    std::string line;
    while (std::getline(ls, line))
        if (line.find(",TARGET,") != std::string::npos) targets.push_back(line);
    ASSERT_EQ(targets.size(), 5u) << "four LOC labels and one GFDIR";
    EXPECT_NE(targets[0].find("LOC:STA1:GPS_DCB,STA1,CBIAS:GPS_DCB,1,TARGET"), std::string::npos);
    EXPECT_NE(targets[1].find("LOC:NOSTA:GPS_DCB,NOSTA,,,TARGET,,,,station not in the solve"),
              std::string::npos) << targets[1];
    EXPECT_NE(targets[2].find("no local column of that key"), std::string::npos) << targets[2];
    EXPECT_NE(targets[3].find("malformed label"), std::string::npos) << targets[3];
    EXPECT_NE(targets[4].find("GFDIR:STA1,STA1,"), std::string::npos) << targets[4];
    for (const char* rec : {",ROW,", ",FOLD,", ",FOLDLOS,", ",KIND,", ",FEEDBACK,", ",SUMZERO,",
                            ",DDPRIOR,", ",UNATTRIBUTED,", ",TOTAL,", ",SOLVED,",
                            ",CLOSURE_ABS,", ",CLOSURE_REL,", ",FBCOL,", ",FBSTA,",
                            ",FBLOS,", ",FBFOLD,", ",FBTOTAL,", ",FBCLOSURE,", ",NETSTA,",
                            ",NETSAT,", ",REFINE,", ",PERTURB,row,", ",PERTURB,los,",
                            ",PERTURB,station,"})
        EXPECT_NE(loc.find(rec), std::string::npos) << rec;

    // Each unresolved label is named once over the two dumps.
    const std::string text = sink_text.str();
    EXPECT_EQ(countOf(text, "'LOC:NOSTA:GPS_DCB' not decomposed"), 1u) << text;
    EXPECT_EQ(countOf(text, "'LOC:STA1:GPS_IFB5' not decomposed"), 1u);
    EXPECT_EQ(countOf(text, "'LOC:STA1' not decomposed"), 1u);
    EXPECT_EQ(warned.size(), 3u);

    // The shared label goes to cols_ alone.
    const std::string cols = readFile(dir / "cols_260040.csv");
    std::istringstream cs(cols);
    std::string cheader;
    std::getline(cs, cheader);
    if (!cheader.empty() && cheader.back() == '\r') cheader.pop_back();
    EXPECT_EQ(cheader, "week,tow,record,station,kind,sat,signal,n,rhs[CLK:G02],sol[CLK:G02]");

    // A frame without the station-local capture (a retained previous frame, which holds
    // no solve x_s) writes no loc_ file and says so once.
    const fs::path dir2 = dir / "trip";
    const NetworkAttribFrame trip = captureFrame(f, /*with_xs=*/false);
    ASSERT_TRUE(trip.valid);
    EXPECT_TRUE(trip.local_parts.empty());
    networkRhsAttribDump(trip, log, dir2, "trip:sigma0", columns, &warned);
    networkRhsAttribDump(trip, log, dir2, "trip:sigma0", columns, &warned);
    log->flush();
    EXPECT_FALSE(fs::exists(dir2 / "loc_260040.csv"));
    EXPECT_TRUE(fs::exists(dir2 / "cols_260040.csv"));
    EXPECT_EQ(countOf(sink_text.str(), "the frame holds no station-local capture"), 1u);
    fs::remove_all(dir, ec);
}
