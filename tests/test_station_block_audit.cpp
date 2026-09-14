// Card ZP (SPEC_event_harness §24.10) — the per-station LOCAL-BLOCK audit.
//
// §24.9 measured a station whose own post-fit code residuals are 0.3-0.55 m while
// its reconstructed ZWD reads -18 +/- 4.5 m and 47 of its 130 rows (36 %) carry
// zero redundancy.  The instrument that explains both is the station's LOCAL
// normal block N_rr = A_r^T W A_r + folded station priors, the matrix
// NetworkEstimator::reconstructStation inverts with the shared states held at
// their solved values.  This file pins the algebra that reads it.
//
// The fixture is the uncombined posture the field vehicle actually runs, at the
// smallest size that carries every structural feature §24.9 names: one station,
// four satellites, two code signals and two phase signals per satellite, and
// per-station locals {clock, ZWD, a gamma-scaled receiver code bias, one slant
// ionosphere per LOS, one ambiguity per (LOS, band)}.  Nothing about it is
// tuned: the block's defects are consequences of that parameterization, and each
// one is asserted against an ANALYTIC value derived in the comments, not against
// a recorded number.
//
//   Gate 1 (rank + span) - the block is rank-deficient by exactly one, and the
//                          null direction is the clock / receiver-bias / iono /
//                          ambiguity combination, with the ZWD carrying ZERO
//                          loading in it.  That matters for the card's reading:
//                          the ZWD is not IN the null space, so a rank report
//                          alone would exonerate it.
//   Gate 2 (redundancy)   - every PHASE row is uncheckable (r_i == 0) because its
//                           own free ambiguity absorbs it, the sum of the
//                           redundancies equals rows - rank exactly, and all of
//                           it sits on the code rows.  This is the §24.9 census.
//   Gate 3 (the vehicle)  - a diagonal prior on the ambiguity columns (what the
//                           phase_leveling carry injects and what carry_invariant
//                           does NOT) restores full rank AND makes the phase rows
//                           checkable.  The two vehicles differ by this and the
//                           trace is built to see it.
//   Gate 4 (ZWD info)     - effectiveInformation is the Schur complement of the
//                           column against every other local state, to the digit.
//                           The implementation uses the identity 1/(N^+)_zz; this
//                           gate holds it to the definition in the card.
//   Gate 5 (span format)  - the span field is a deterministic function of the
//                           eigenvector, truncates at the 90 % cover, and cannot
//                           emit a CSV delimiter.
//   Gate 6 (knob OFF)     - the structural no-op: an unarmed run opens no stream
//                           and creates no file; an armed one writes the
//                           provenance line and the byte-exact column header the
//                           reader parses.

#include "filter/NetworkStationBlock.hpp"

#include "config/GnssConfig.hpp"
#include "config/GnssConfigTypes.hpp"
#include "logging/NetworkRunTraces.hpp"

#include <gtest/gtest.h>
#include <spdlog/spdlog.h>
#include <spdlog/sinks/null_sink.h>
#include <fmt/format.h>

#include <Eigen/Dense>

#include <cmath>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace GnssProcessor {
namespace {

/// GPS L1/L2 ionospheric scale factor (f1/f2)^2 — the ratio §24.9 measured in the
/// receiver "code-bias" seed and named as the tell that the column is an
/// ionosphere-like offset rather than a hardware DCB.
constexpr double kGamma = (1575.42 / 1227.60) * (1575.42 / 1227.60);

constexpr int kSats = 4;
constexpr Eigen::Index kColClk  = 0;
constexpr Eigen::Index kColZwd  = 1;
constexpr Eigen::Index kColRxcb = 2;
constexpr Eigen::Index kColIono = 3;               // + sat
constexpr Eigen::Index kColAmb  = kColIono + kSats;  // + 2*sat (+0 = L1, +1 = L2)
constexpr Eigen::Index kNLocal  = kColAmb + 2 * kSats;   // 15
constexpr Eigen::Index kNRows   = 4 * kSats;             // 16

/// Wet mapping function per satellite (1/sin(elevation) at 15 / 30 / 50 / 75 deg).
/// They must be DISTINCT — the clock and the ZWD separate only through the
/// elevation spread, which is the §24.9 sentence this fixture encodes.
double mapWet(int sat) {
    static const double el_deg[kSats] = {15.0, 30.0, 50.0, 75.0};
    return 1.0 / std::sin(el_deg[sat] * 3.14159265358979323846 / 180.0);
}

/// The station's local design rows, in the uncombined posture:
///   C1_j: clk + m_j*zwd + iono_j
///   C2_j: clk + m_j*zwd + gamma*iono_j + rxcb
///   L1_j: clk + m_j*zwd - iono_j + amb_{j,L1}
///   L2_j: clk + m_j*zwd - gamma*iono_j + amb_{j,L2}
Eigen::MatrixXd makeAr() {
    Eigen::MatrixXd A = Eigen::MatrixXd::Zero(kNRows, kNLocal);
    for (int j = 0; j < kSats; ++j) {
        const double m = mapWet(j);
        const Eigen::Index r0 = 4 * j;
        for (int k = 0; k < 4; ++k) {
            A(r0 + k, kColClk) = 1.0;
            A(r0 + k, kColZwd) = m;
        }
        A(r0 + 0, kColIono + j) = 1.0;
        A(r0 + 1, kColIono + j) = kGamma;
        A(r0 + 1, kColRxcb)     = 1.0;
        A(r0 + 2, kColIono + j) = -1.0;
        A(r0 + 3, kColIono + j) = -kGamma;
        A(r0 + 2, kColAmb + 2 * j + 0) = 1.0;
        A(r0 + 3, kColAmb + 2 * j + 1) = 1.0;
    }
    return A;
}

/// Row information weights: code sigma 0.4 m, phase sigma 3 mm (the §24.9 run's
/// measured row weights).
Eigen::VectorXd makeW() {
    Eigen::VectorXd w(kNRows);
    for (int j = 0; j < kSats; ++j) {
        const Eigen::Index r0 = 4 * j;
        w(r0 + 0) = w(r0 + 1) = 1.0 / (0.4 * 0.4);
        w(r0 + 2) = w(r0 + 3) = 1.0 / (0.003 * 0.003);
    }
    return w;
}

Eigen::MatrixXd makeNrr(const Eigen::MatrixXd& A, const Eigen::VectorXd& w) {
    return A.transpose() * w.asDiagonal() * A;
}

/// True iff row @p r is a phase row of the fixture.
bool isPhaseRow(Eigen::Index r) { return (r % 4) >= 2; }

std::vector<std::string> makeLabels() {
    std::vector<std::string> L(static_cast<std::size_t>(kNLocal));
    L[kColClk]  = "clk";
    L[kColZwd]  = "zwd";
    L[kColRxcb] = "rxcb:C2W";
    for (int j = 0; j < kSats; ++j) {
        const std::string sat = "G0" + std::to_string(j + 1);
        L[static_cast<std::size_t>(kColIono + j)] = "iono:" + sat;
        L[static_cast<std::size_t>(kColAmb + 2 * j + 0)] = "amb:" + sat + ":L1C";
        L[static_cast<std::size_t>(kColAmb + 2 * j + 1)] = "amb:" + sat + ":L2W";
    }
    return L;
}

std::shared_ptr<spdlog::logger> nullLogger() {
    return std::make_shared<spdlog::logger>(
        "test_station_block_audit", std::make_shared<spdlog::sinks::null_sink_mt>());
}

}  // namespace

// ---------------------------------------------------------------------------
// Gate 1: rank deficiency, and WHICH local direction is unresolved.
// ---------------------------------------------------------------------------
//
// Solving A_r * n = 0 by hand:
//   C1_j - C2_j  =>  (1 - gamma) iono_j - rxcb = 0   =>  iono_j = rxcb/(gamma-1)
//                                                        (the SAME for every j)
//   C1_j         =>  clk + m_j*zwd = -iono_j, for all j with DISTINCT m_j
//                =>  zwd = 0  and  clk = -iono_j
//   L1_j, L2_j   =>  amb_{j,L1} = -(clk - iono_j),  amb_{j,L2} = -(clk - gamma*iono_j)
// With rxcb = gamma - 1: iono_j = -1, clk = 1, zwd = 0, amb_L1 = -2,
// amb_L2 = -(1+gamma).  One dimension, and the ZWD is EXACTLY absent from it.
TEST(StationBlockAuditTest, TheUncombinedStationBlockIsRankDeficientByOneAndTheZwdIsNotInIt) {
    const Eigen::MatrixXd A = makeAr();
    const Eigen::VectorXd w = makeW();
    const Eigen::MatrixXd N = makeNrr(A, w);

    // The analytic null vector, verified against the design itself so the gate
    // cannot pass on a fixture that drifted from the comment above.
    Eigen::VectorXd n = Eigen::VectorXd::Zero(kNLocal);
    n(kColClk)  = 1.0;
    n(kColZwd)  = 0.0;
    n(kColRxcb) = kGamma - 1.0;
    for (int j = 0; j < kSats; ++j) {
        n(kColIono + j) = -1.0;
        n(kColAmb + 2 * j + 0) = -2.0;
        n(kColAmb + 2 * j + 1) = -(1.0 + kGamma);
    }
    EXPECT_LT((A * n).norm(), 1e-10) << "the fixture no longer has the derived null direction";

    const auto sp = StationBlockAudit::spectrum(N, makeLabels());
    ASSERT_TRUE(sp.ok);
    EXPECT_EQ(sp.n_local, kNLocal);
    // 16 rows, 15 locals, ONE exact null direction.
    EXPECT_EQ(sp.rank, kNLocal - 1);
    EXPECT_GT(sp.cond, 0.0);

    // The smallest eigenvalue is the null one: structurally zero against the
    // largest, and orders below the second-smallest (so the deficiency is EXACTLY
    // one, not "one that happens to sort first").
    const Eigen::Index nn = sp.eigenvalues.size();
    const double lmax = sp.eigenvalues(nn - 1);
    EXPECT_LE(std::abs(sp.ev_val[0]), StationBlockAudit::kRankTolRel * lmax);
    EXPECT_GT(sp.ev_val[1], StationBlockAudit::kRankTolRel * lmax);

    // The span names ambiguity columns and NEVER the ZWD.  This is the reading
    // the card needs: a rank report alone would say "the block has a hole" and a
    // reader would assume the absurd ZWD is in it.  It is not -- the ZWD's defect
    // is weak information (gate 4), not rank.
    const std::string& span = sp.ev_span[0];
    EXPECT_EQ(span.find("zwd"), std::string::npos) << "span=" << span;
    EXPECT_EQ(span.rfind("amb:", 0), 0u) << "span=" << span;
    // Analytic top share: amb_L2^2 / |n|^2 = (1+gamma)^2 / |n|^2.
    const double top = (1.0 + kGamma) * (1.0 + kGamma) / n.squaredNorm();
    EXPECT_NEAR(top, 0.1417, 5e-4);
    EXPECT_NE(span.find(fmt::format("{:.3f}", top)), std::string::npos) << "span=" << span;
}

// ---------------------------------------------------------------------------
// Gate 2: the redundancy census -- the §24.9 "36 % of rows are UNCHECKABLE".
// ---------------------------------------------------------------------------
TEST(StationBlockAuditTest, PhaseRowsWithFreeAmbiguitiesCarryZeroRedundancy) {
    const Eigen::MatrixXd A = makeAr();
    const Eigen::VectorXd w = makeW();
    const Eigen::MatrixXd N = makeNrr(A, w);
    const auto sp = StationBlockAudit::spectrum(N, makeLabels());
    ASSERT_TRUE(sp.ok);

    const Eigen::VectorXd r = StationBlockAudit::redundancy(sp, A, w);
    ASSERT_EQ(r.size(), kNRows);

    // Every phase row carries its own free ambiguity, which appears in NO other
    // row: the row is absorbed exactly, so no residual of it is checkable and no
    // QC screen, however armed, can see a fault on it.
    int zero_phase = 0, zero_code = 0;
    for (Eigen::Index i = 0; i < r.size(); ++i) {
        if (r(i) < StationBlockAudit::kZeroRedundancy)
            (isPhaseRow(i) ? zero_phase : zero_code)++;
        if (isPhaseRow(i)) EXPECT_NEAR(r(i), 0.0, 1e-9) << "phase row " << i;
        else               EXPECT_GT(r(i), 1e-3)        << "code row " << i;
    }
    EXPECT_EQ(zero_phase, 2 * kSats);   // all 8 phase rows
    EXPECT_EQ(zero_code, 0);            // and none of the code rows
    // 8 of 16 rows = 50 % uncheckable on this fixture; §24.9 measured 36 % on a
    // real station, where some satellites contribute code without phase.

    // The global identity: with no priors folded in, sum(r_i) == rows - rank.
    // It ties the per-row numbers to the spectrum and would catch a
    // pseudo-inverse taken on the wrong subspace.
    EXPECT_NEAR(r.sum(), static_cast<double>(kNRows - sp.rank), 1e-8);
}

// ---------------------------------------------------------------------------
// Gate 3: the VEHICLE difference, in one assertion.
// ---------------------------------------------------------------------------
//
// phase_leveling folds a diagonal prior onto each carried ambiguity column
// (NetworkEstimator::addStationPrior, which rank-1-updates the cached LDL^T, so
// stationLocalNormals returns a block that HAS them).  carry_invariant folds no
// such prior: its ambiguities are free every epoch.  That single structural
// difference is what the trace exists to see, so it is pinned here.
TEST(StationBlockAuditTest, AnAmbiguityPriorRestoresRankAndMakesThePhaseRowsCheckable) {
    const Eigen::MatrixXd A = makeAr();
    const Eigen::VectorXd w = makeW();
    Eigen::MatrixXd N = makeNrr(A, w);

    const double lambda = 1.0 / (0.005 * 0.005);   // a 5 mm carried ambiguity
    for (int j = 0; j < kSats; ++j) {
        N(kColAmb + 2 * j + 0, kColAmb + 2 * j + 0) += lambda;
        N(kColAmb + 2 * j + 1, kColAmb + 2 * j + 1) += lambda;
    }

    const auto sp = StationBlockAudit::spectrum(N, makeLabels());
    ASSERT_TRUE(sp.ok);
    // The null direction had non-zero loading on every ambiguity column, so the
    // prior closes it: the block is FULL RANK.
    EXPECT_EQ(sp.rank, kNLocal);

    const Eigen::VectorXd r = StationBlockAudit::redundancy(sp, A, w);
    ASSERT_EQ(r.size(), kNRows);
    for (Eigen::Index i = 0; i < r.size(); ++i)
        if (isPhaseRow(i)) {
            EXPECT_GT(r(i), StationBlockAudit::kZeroRedundancy)
                << "phase row " << i << " is still uncheckable";
            EXPECT_GT(r(i), 1e-3) << "phase row " << i << " is only marginally checkable";
        }
}

// ---------------------------------------------------------------------------
// Gate 4: the ZWD column's EFFECTIVE information.
// ---------------------------------------------------------------------------
TEST(StationBlockAuditTest, EffectiveInformationEqualsTheExplicitSchurComplement) {
    // A non-singular SPD block, so the definition N_zz - N_zo N_oo^-1 N_oz can be
    // evaluated with an ORDINARY inverse and the identity 1/(N^-1)_zz has nothing
    // to hide behind.  Deterministic, no RNG.
    const Eigen::Index n = 6;
    Eigen::MatrixXd M(n, n);
    for (Eigen::Index i = 0; i < n; ++i)
        for (Eigen::Index j = 0; j < n; ++j)
            M(i, j) = std::sin(1.0 + 0.7 * static_cast<double>(i)
                                   + 1.3 * static_cast<double>(j));
    const Eigen::MatrixXd N =
        M.transpose() * M + Eigen::MatrixXd::Identity(n, n) * 0.25;

    const std::vector<std::string> labels(static_cast<std::size_t>(n), "x");
    const auto sp = StationBlockAudit::spectrum(N, labels);
    ASSERT_TRUE(sp.ok);
    ASSERT_EQ(sp.rank, n);

    for (Eigen::Index z = 0; z < n; ++z) {
        // Explicit elimination of every OTHER column.
        Eigen::MatrixXd Noo(n - 1, n - 1);
        Eigen::VectorXd Noz(n - 1);
        Eigen::Index a = 0;
        for (Eigen::Index i = 0; i < n; ++i) {
            if (i == z) continue;
            Eigen::Index b = 0;
            for (Eigen::Index j = 0; j < n; ++j) {
                if (j == z) continue;
                Noo(a, b++) = N(i, j);
            }
            Noz(a++) = N(i, z);
        }
        const double schur = N(z, z) - Noz.dot(Noo.ldlt().solve(Noz));
        EXPECT_NEAR(StationBlockAudit::effectiveInformation(sp, z), schur,
                    1e-9 * std::abs(schur) + 1e-12)
            << "column " << z;
    }

    // And on the fixture: the ZWD column has far LESS effective information than
    // its raw sum(w*m_wet^2), which is the §24.9 quantity ("the ZWD column's
    // effective information in the reduced station block is ~1/50 of that").
    const Eigen::MatrixXd A = makeAr();
    const Eigen::VectorXd wv = makeW();
    const auto spf = StationBlockAudit::spectrum(makeNrr(A, wv), makeLabels());
    ASSERT_TRUE(spf.ok);
    const double raw = makeNrr(A, wv)(kColZwd, kColZwd);
    const double eff = StationBlockAudit::effectiveInformation(spf, kColZwd);
    EXPECT_GT(raw, 0.0);
    EXPECT_GT(eff, 0.0);          // the ZWD is NOT in the null space (gate 1)
    EXPECT_LT(eff / raw, 1e-5);   // and keeps under 1e-5 of its raw information
}

// ---------------------------------------------------------------------------
// Gate 5: the span field's format contract.
// ---------------------------------------------------------------------------
TEST(StationBlockAuditTest, SpanIsDeterministicTruncatedAtTheCoverAndCsvSafe) {
    Eigen::VectorXd v(4);
    // Two EXACTLY equal loadings: the tie must break on column index, or two arms
    // of an A/B differ because std::sort saw the pair in another order.
    v << 0.6, 0.6, 0.4, std::sqrt(1.0 - 0.36 - 0.36 - 0.16);
    const std::vector<std::string> labels = {"clk", "zwd", "iono:G01", "amb:G01:L1C"};
    const std::string s = StationBlockAudit::spanOf(v, labels);
    EXPECT_EQ(s, "clk=0.360|zwd=0.360|iono:G01=0.160");
    EXPECT_EQ(StationBlockAudit::spanOf(v, labels), s);   // deterministic

    // The cover truncates: one column carrying 96 % is the whole span.
    Eigen::VectorXd u(3);
    u << std::sqrt(0.96), std::sqrt(0.03), std::sqrt(0.01);
    EXPECT_EQ(StationBlockAudit::spanOf(u, labels), "clk=0.960");

    // A label carrying a delimiter cannot break the CSV, and a missing label
    // falls back to the column index rather than an empty field.
    const std::vector<std::string> bad = {"a,b", "c|d", "e=f"};
    EXPECT_EQ(StationBlockAudit::spanOf(v, bad), "a;b=0.360|c;d=0.360|e;f=0.160");
    const std::vector<std::string> few = {"clk"};
    EXPECT_EQ(StationBlockAudit::spanOf(v, few), "clk=0.360|col1=0.360|col2=0.160");
    EXPECT_EQ(StationBlockAudit::spanOf(Eigen::VectorXd(), labels), std::string());
}

// ---------------------------------------------------------------------------
// Gate 6: the knob-OFF structural no-op + the byte-exact header.
// ---------------------------------------------------------------------------
TEST(StationBlockAuditTest, UnarmedOpensNothingAndArmedWritesTheDocumentedHeader) {
    namespace fs = std::filesystem;
    const auto log = nullLogger();
    const fs::path dir = fs::temp_directory_path() / "gnss_station_block_audit_trace_test";
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir);

    const GnssConfig cfg;
    const GlobalVtecShConfig shc;

    {   // OFF: no stream, no file.  The seam's own gate is `is_open()`, so this
        // IS the structural no-op -- an unarmed run cannot reach the audit.
        NetworkConfig nc;
        EXPECT_FALSE(nc.trace_station_block) << "the knob must default OFF";
        NetworkRunTraces t(dir);
        t.openAll(cfg, nc, shc, /*sh_model=*/nullptr, /*cl_trace_armed=*/false, log);
        EXPECT_FALSE(t.station_block_trace.is_open());
        t.closeAll();
    }
    EXPECT_FALSE(fs::exists(dir / "network_station_block.csv"));

    {   // ON: the stream opens and the file carries provenance + columns.
        NetworkConfig nc;
        nc.trace_station_block = true;
        NetworkRunTraces t(dir);
        t.openAll(cfg, nc, shc, /*sh_model=*/nullptr, /*cl_trace_armed=*/false, log);
        EXPECT_TRUE(t.station_block_trace.is_open());
        t.closeAll();
    }
    ASSERT_TRUE(fs::exists(dir / "network_station_block.csv"));

    std::ifstream in(dir / "network_station_block.csv");
    std::string provenance, header;
    ASSERT_TRUE(static_cast<bool>(std::getline(in, provenance)));
    ASSERT_TRUE(static_cast<bool>(std::getline(in, header)));
    if (!provenance.empty() && provenance.back() == '\r') provenance.pop_back();
    if (!header.empty() && header.back() == '\r') header.pop_back();

    // The provenance line must say WHICH matrix the numbers describe; without it
    // the trace reads as the station's stand-alone normals, which it is not.
    EXPECT_EQ(provenance.rfind('#', 0), 0u);
    EXPECT_NE(provenance.find("N_rr"), std::string::npos);
    EXPECT_NE(provenance.find("reconstructStation"), std::string::npos);

    // Byte-pinned so a column edit cannot land without updating
    // scripts/zp_station_block_read.py.
    EXPECT_EQ(header,
              "week,tow,station,n_rows_code,n_rows_phase,n_local,rank,cond,"
              "ev1_val,ev1_span,ev2_val,ev2_span,ev3_val,ev3_span,"
              "zwd_info_eff,zwd_info_raw,zwd_info_ratio,x_r_norm,x_r_zwd,"
              "zero_red_code,zero_red_phase,n_rows");
    // Armed and silent is a header-only file -- the honest "ran, audited none".
    std::string extra;
    EXPECT_FALSE(static_cast<bool>(std::getline(in, extra)));
    in.close();
    fs::remove_all(dir, ec);
}

}  // namespace GnssProcessor
