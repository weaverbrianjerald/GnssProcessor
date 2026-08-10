/**
 * @file test_station_congruence.cpp
 * @brief The STATION CONGRUENCE screen — coordinate-fault detection in position
 *        space (docs/SPEC_station_congruence.md), INC-0..3.
 *
 * Station coordinates are HELD in the network producer, and a wrong held
 * coordinate is the one fault class no armed tier can see: median polish removes
 * the station common effect, per-row w-tests see only what the receiver clock and
 * the ambiguities did not absorb, and the (station, satellite) block test pools
 * ONE line of sight while a coordinate error is coherent across ALL of them.
 * JPLM's 6.10 cm ARP error is the measured precedent — found by hand.
 *
 * What is worth testing is therefore not "does the arithmetic run" but the six
 * claims that fail silently if they are wrong:
 *
 *   1. RECOVERY AND LOCALIZATION.  A known coordinate offset injected into one
 *      station comes back as that offset, with the right SIGN, at that station and
 *      not at its neighbours.  The recovery is EXACT, not approximate — with a
 *      noise-free injection @f$\hat\delta = M^{+}Md = d@f$ whenever @f$M@f$ has
 *      full rank — so a tolerance of 1e-9 on a 5 cm offset is a real test of the
 *      whole chain, not a fitted one.
 *   2. ABSORPTION IS MEASURED, NOT WORKED AROUND.  On a constellation whose Up
 *      partial is exactly proportional to the receiver-clock column, that
 *      direction is absorbed EXACTLY: the third eigenvalue of @f$M@f$ is zero and
 *      the screen must report the direction UNOBSERVABLE with MDB = +infinity
 *      rather than invert through it.  A card that regularized here would print a
 *      finite vertical MDB that no consumer could act on.
 *   3. SEPARABILITY (INC-3).  A coordinate offset excites THIS tier and not the
 *      per-LOS block tier; a single-LOS fault does the converse.  The two tiers
 *      partition the fault classes — that is the claim the 2x2 contingency table
 *      exists to measure in the field, and it has to hold on a hand-built network
 *      or it holds nowhere.
 *   4. The kernel contraction IS the dense GLS quantity — against an independent
 *      oracle, not against the arrowhead kernel's own Schur reduction.  A wrong
 *      marginalization still returns a plausible symmetric 3x3 and nobody can tell.
 *   5. The NULL LADDER is chi-square_q, and the sigma0 rescale that makes it so.
 *      NetworkCarryVce inverts a chi-square_1 geometric mean, so a chi-square_3
 *      statistic fed raw biases the scale by ~1.5x in sigma — the correction the
 *      block card paid for, re-verified here at q = 3 because a sibling card
 *      getting it wrong produces a plausible, silently mis-scaled ladder.  The
 *      Student-t arm preserves D5's negative: the Gaussian tail is the wrong
 *      critical value on real residuals, which is why the ladder is empirical.
 *   6. The bookkeeping contracts: CLEAN/FRAME split (invariant #7), median/MAD
 *      accumulation with no sqrt(N) claim (D1's K_eff ~ 2), the retention cap
 *      COUNTED rather than silent, and OFF inertness (structural byte-identity).
 */

#include <gtest/gtest.h>
#include <Eigen/Dense>

#include <cmath>
#include <limits>
#include <random>
#include <vector>

#include "filter/NetworkCongruence.hpp"
#include "filter/NetworkNormalEquations.hpp"

namespace GnssProcessor {
namespace {

// ===========================================================================
//  Dense GLS oracle — deliberately independent of the arrowhead kernel.
// ===========================================================================

struct DenseGls {
    Eigen::MatrixXd A;          ///< rows x params
    Eigen::VectorXd w;          ///< per-row information weight (1/sigma^2)
    Eigen::VectorXd prior_diag; ///< diagonal prior INFORMATION over the parameters
    Eigen::MatrixXd cov;        ///< (A'WA + Prior)^-1
    Eigen::MatrixXd Q;          ///< residual cofactor over ALL rows

    /// Residuals with the prior pseudo-observations at their means (zero).  This
    /// is the right operator for a DETERMINISTIC injection: it answers "what does
    /// the adjustment leave behind when the data is perturbed by exactly this".
    Eigen::VectorXd residuals(const Eigen::VectorXd& y) const {
        const Eigen::VectorXd x = cov * (A.transpose() * (w.asDiagonal() * y));
        return y - A * x;
    }

    /// Residuals with the prior pseudo-observations DRAWN.
    ///
    /// ⚠ This distinction is not cosmetic and it is the trap this toy walked into
    /// first.  @c Q = P^-1 - A N^-1 A' is the residual cofactor of the AUGMENTED
    /// system (data rows + prior pseudo-observations), so E[e~ e~'] = sigma^2 Q
    /// holds only when the priors carry their own noise.  Hold them exactly at
    /// zero and the residual operator R = QP is no longer idempotent: one gets
    /// E[e~ e~'] = sigma^2 QPQ = sigma^2 (Q - A N^-1 Pi N^-1 A'), which on this
    /// toy's stiff iono/ambiguity priors is HALF of Q — and the null statistic
    /// would read E[T] = 0.52 instead of 1 while every other test still passed.
    Eigen::VectorXd residualsDrawnPrior(const Eigen::VectorXd& y,
                                        const Eigen::VectorXd& b) const {
        const Eigen::VectorXd x =
            cov * (A.transpose() * (w.asDiagonal() * y)
                   + prior_diag.cwiseProduct(b));
        return y - A * x;
    }
};

DenseGls finishDense(const Eigen::MatrixXd& A, const Eigen::VectorXd& w,
                     const Eigen::VectorXd& prior_diag) {
    DenseGls d;
    d.A = A;
    d.w = w;
    d.prior_diag = (prior_diag.size() == A.cols())
                       ? prior_diag
                       : Eigen::VectorXd::Zero(A.cols());
    Eigen::MatrixXd N = A.transpose() * w.asDiagonal() * A;
    N.diagonal() += d.prior_diag;
    d.cov = N.inverse();
    d.Q = Eigen::MatrixXd(
        Eigen::MatrixXd(Eigen::VectorXd(w.array().inverse()).asDiagonal()) -
        A * d.cov * A.transpose());
    return d;
}

// ===========================================================================
//  The toy network — a physically-shaped producer block WITH geometry.
//
//  kR stations x kS satellites, four rows per LOS (code/phase x two bands).
//  Shared = per-satellite clock; local = receiver clock, per-satellite slant
//  iono, per-(satellite, band) ambiguity.  The iono and ambiguity priors stand
//  in for the SH tie and the phase-leveling carry.
//
//  The piece that does not exist in the block card's toy, and is the whole
//  subject here: each row carries a POSITION PARTIAL.  The network drops those
//  three columns (coordinates are held), which is precisely why they have to be
//  retained by StationAssembly::row_pos_enu for this screen to exist at all.
// ===========================================================================

/// ⚠ TEN stations, not three, and the reason is physics rather than coverage.
/// A coordinate offset at one station projects onto the SHARED satellite clocks,
/// and with a handful of stations those clocks absorb it almost for free — the
/// offset re-emerges as a compensating clock at every OTHER station, so the screen
/// localizes nothing.  Measured on the 3-station version of this same toy: a 5 cm
/// East offset at station 1 produced |delta| = 1.9 / 2.5 cm at its two neighbours
/// (37 % / 50 % leakage) with a LARGER T than the perturbed station itself.  That
/// is a true property of the instrument, not a defect: over-determined satellite
/// clocks are a PRECONDITION for coordinate-fault localization, and the field
/// networks this card runs on carry 60-305 stations.
constexpr int kR = 10;                         ///< stations
/// ⚠ TEN satellites, and this one is quantitative.  The two tiers' separability
/// scales as kS^2, because the 3-D coordinate subspace occupies 3 of the station's
/// kS line-of-sight directions: a coordinate fault's share of any ONE block falls
/// as 1/kS, while a single-LOS fault's projection onto the coordinate subspace
/// falls as 3/kS.  Measured on this toy at kS = 6: a single-LOS slip put 59 % of
/// its residual energy in the coordinate subspace and the ratio-of-ratios was only
/// 4.3.  That is not a defect of either tier — with six satellites a
/// per-satellite fault genuinely IS half a coordinate error — but it means the
/// partition can only be demonstrated on a sky the size of a real one.
constexpr int kS = 10;                         ///< satellites
constexpr Eigen::Index kRowsPerSta = 4 * kS;   ///< four rows per LOS
constexpr Eigen::Index kLocal = 1 + 3 * kS;    ///< dt, I_k, (N_k0, N_k1)
constexpr Eigen::Index kNTot = kS + kR * kLocal;

constexpr double kMu2 = (154.0 / 120.0) * (154.0 / 120.0);
constexpr double kLam1 = 0.190293672798365;
constexpr double kLam2 = 0.244210213424568;
constexpr double kSigCode = 0.3, kSigPhase = 0.003;
constexpr double kSigIono = 0.01, kSigAmb = 0.005;
constexpr double kPin = 1e14;
constexpr double kDeg = 3.14159265358979323846 / 180.0;

enum Kind { kC1 = 0, kC2 = 1, kL1 = 2, kL2 = 3 };

Eigen::Index ionoCol(int k) { return 1 + k; }
Eigen::Index ambCol(int k, int band) { return 1 + kS + 2 * k + band; }
Eigen::Index rowOf(int k, int kind) { return 4 * k + kind; }
Eigen::Index globalRow(int r, int k, int kind) { return r * kRowsPerSta + rowOf(k, kind); }

double sigmaOf(int r, int k, int kind) {
    const double scale = 1.0 + 0.35 * static_cast<double>((r + 2 * k) % 4);
    const double base = (kind == kC1 || kind == kC2) ? kSigCode : kSigPhase;
    const double band = (kind == kC2 || kind == kL2) ? 1.1 : 1.0;
    return base * scale * band;
}

double muOf(int kind) {
    switch (kind) {
        case kC1: return 1.0;
        case kC2: return kMu2;
        case kL1: return -1.0;
        default:  return -kMu2;
    }
}

/// Unit line of sight, receiver -> satellite, in local ENU.
Eigen::Vector3d losEnu(double az_deg, double el_deg) {
    const double az = az_deg * kDeg, el = el_deg * kDeg;
    return Eigen::Vector3d(std::cos(el) * std::sin(az),
                           std::cos(el) * std::cos(az),
                           std::sin(el));
}

/// FLAT: every satellite at one elevation, so the Up partial sin(el) is CONSTANT
/// across rows and therefore exactly proportional to the receiver-clock column —
/// the absorption case of SPEC §6.2, degenerate by construction rather than by
/// numerical accident.  VARIED: elevations differ per (station, satellite), so
/// all three ENU directions carry information.
enum class Sky { Varied, Flat };

Eigen::Vector3d satLos(Sky sky, int r, int k) {
    if (sky == Sky::Flat)
        return losEnu(360.0 * static_cast<double>(k) / static_cast<double>(kS), 45.0);
    // Azimuths spread around the compass, elevations strictly inside [15, 75] deg.
    // The per-station offsets keep the stations' skies DIFFERENT — a network of
    // identical geometries would make the localization test vacuous — while the
    // fmod keeps every one of them in the hemisphere.  An "elevation" past 90 deg
    // is still a unit vector, so it type-checks and silently wrecks M; that is how
    // the first version of this toy leaked 46 % of a 5 cm offset onto its
    // neighbours at ten stations.
    const double az = std::fmod(30.0 + 61.0 * static_cast<double>(k)
                                     + 23.0 * static_cast<double>(r), 360.0);
    const double el = 15.0 + std::fmod(11.0 * static_cast<double>(k)
                                     + 7.0 * static_cast<double>(r), 60.0);
    return losEnu(az, el);
}

struct CongruenceNet {
    Eigen::MatrixXd A_s[kR], A_r[kR];
    Eigen::VectorXd w[kR];
    /// dy/dd per row (kRowsPerSta x 3, ENU) in the REPORTED sign convention:
    /// d = p_held - p_true, so this is +u (the assembly's row_pos_enu is -u, and
    /// the sweep negates it).  Injecting y += Hd * d therefore simulates a held
    /// coordinate that is `d` too far East/North/Up.
    Eigen::MatrixXd Hd[kR];
    std::size_t slot[kR]{};
    Eigen::MatrixXd cov_free;
    DenseGls dense;
    Eigen::Index pin_col{0};

    Eigen::VectorXd stationRows(const Eigen::VectorXd& all, int r) const {
        return all.segment(r * kRowsPerSta, kRowsPerSta);
    }
    Eigen::MatrixXd stationQ(int r) const {
        return dense.Q.block(r * kRowsPerSta, r * kRowsPerSta, kRowsPerSta, kRowsPerSta);
    }
    /// The four rows of one LOS, in the dense stack.
    std::vector<Eigen::Index> denseRows(int r, int k) const {
        return {globalRow(r, k, kC1), globalRow(r, k, kC2),
                globalRow(r, k, kL1), globalRow(r, k, kL2)};
    }
};

CongruenceNet buildNet(NetworkNormalEquations& neq, Sky sky, Eigen::Index pin_col = 0) {
    CongruenceNet net;
    net.pin_col = pin_col;

    Eigen::MatrixXd A = Eigen::MatrixXd::Zero(kR * kRowsPerSta, kNTot);
    Eigen::VectorXd w(kR * kRowsPerSta);
    Eigen::VectorXd prior = Eigen::VectorXd::Zero(kNTot);

    for (int r = 0; r < kR; ++r) {
        net.A_s[r] = Eigen::MatrixXd::Zero(kRowsPerSta, kS);
        net.A_r[r] = Eigen::MatrixXd::Zero(kRowsPerSta, kLocal);
        net.Hd[r]  = Eigen::MatrixXd::Zero(kRowsPerSta, 3);
        net.w[r].resize(kRowsPerSta);
        for (int k = 0; k < kS; ++k) {
            const Eigen::Vector3d u = satLos(sky, r, k);
            for (int kind = 0; kind < 4; ++kind) {
                const Eigen::Index row = rowOf(k, kind);
                net.A_s[r](row, k) = -1.0;                  // satellite clock
                net.A_r[r](row, 0) = 1.0;                   // receiver clock
                net.A_r[r](row, ionoCol(k)) = muOf(kind);   // slant iono
                if (kind == kL1) net.A_r[r](row, ambCol(k, 0)) = kLam1;
                if (kind == kL2) net.A_r[r](row, ambCol(k, 1)) = kLam2;
                net.Hd[r].row(row) = u.transpose();         // dy/dd (see the member doc)
                const double s = sigmaOf(r, k, kind);
                net.w[r](row) = 1.0 / (s * s);
            }
        }
        std::vector<Eigen::Index> cols;
        std::vector<double> info;
        for (int k = 0; k < kS; ++k) {
            cols.push_back(ionoCol(k));
            info.push_back(1.0 / (kSigIono * kSigIono));
            for (int band = 0; band < 2; ++band) {
                cols.push_back(ambCol(k, band));
                info.push_back(1.0 / (kSigAmb * kSigAmb));
            }
        }
        const Eigen::VectorXd y = Eigen::VectorXd::Zero(kRowsPerSta);
        net.slot[r] = neq.addStationObservations(net.A_s[r], net.A_r[r], net.w[r], y);
        Eigen::VectorXd dN = Eigen::Map<Eigen::VectorXd>(
            info.data(), static_cast<Eigen::Index>(info.size()));
        neq.addStationPrior(net.slot[r], cols, dN, Eigen::VectorXd::Zero(dN.size()));

        const Eigen::Index base = kS + r * kLocal;
        A.block(r * kRowsPerSta, 0, kRowsPerSta, kS) = net.A_s[r];
        A.block(r * kRowsPerSta, base, kRowsPerSta, kLocal) = net.A_r[r];
        w.segment(r * kRowsPerSta, kRowsPerSta) = net.w[r];
        for (std::size_t i = 0; i < cols.size(); ++i) prior(base + cols[i]) = info[i];
    }

    // The datum: a Dirichlet pin on one satellite clock, exactly as the driver
    // realizes it.  The kernel zeroes that column's row/col in the shared
    // covariance; the dense oracle DROPS the column outright.  The two agree in
    // the pin -> infinity limit.
    Eigen::MatrixXd pin_info = Eigen::MatrixXd::Zero(kS, kS);
    pin_info(pin_col, pin_col) = kPin;
    neq.addSharedInfo(pin_info, Eigen::VectorXd::Zero(kS));
    neq.reduce();
    net.cov_free = neq.reducedSharedNormals().inverse();
    net.cov_free.row(pin_col).setZero();
    net.cov_free.col(pin_col).setZero();

    std::vector<Eigen::Index> keep;
    for (Eigen::Index c = 0; c < kNTot; ++c)
        if (c != pin_col) keep.push_back(c);
    Eigen::MatrixXd Ad(A.rows(), static_cast<Eigen::Index>(keep.size()));
    Eigen::VectorXd pd(static_cast<Eigen::Index>(keep.size()));
    for (std::size_t c = 0; c < keep.size(); ++c) {
        Ad.col(static_cast<Eigen::Index>(c)) = A.col(keep[c]);
        pd(static_cast<Eigen::Index>(c)) = prior(keep[c]);
    }
    net.dense = finishDense(Ad, w, pd);
    return net;
}

/// The whole network's observation vector with a held-coordinate error @p d
/// injected at station @p r and nothing else.  y = Hd * d is the linearized
/// footprint of that error (see CongruenceNet::Hd).
Eigen::VectorXd injectCoordinate(const CongruenceNet& net, int r,
                                 const Eigen::Vector3d& d) {
    Eigen::VectorXd y = Eigen::VectorXd::Zero(kR * kRowsPerSta);
    y.segment(r * kRowsPerSta, kRowsPerSta) = net.Hd[r] * d;
    return y;
}

/// The whole network's observation vector with a same-sign bias on BOTH PHASE
/// rows of ONE line of sight and nothing else — an unflagged half-cycle-class
/// slip.  This is the block card's own discriminating fault (it measured a 6x
/// block-over-row gain on exactly this shape, where the row tier is blind), and it
/// is this card's negative control.
///
/// ⚠ Deliberately NOT a bias on all four rows.  A same-sign bias on the code rows
/// too is a per-satellite RANGE bias, and a range bias is genuinely CONFOUNDED
/// with a coordinate error at small satellite counts — that confounding is real
/// physics, not a weakness of either tier, and it is what M's eigen structure and
/// the MDB exist to report.  Using it as the negative control would test the
/// constellation size, not the partition.
Eigen::VectorXd injectLosPhase(int r, int k, double bias) {
    Eigen::VectorXd y = Eigen::VectorXd::Zero(kR * kRowsPerSta);
    y(globalRow(r, k, kL1)) = bias;
    y(globalRow(r, k, kL2)) = bias;
    return y;
}

/// This card's statistic for one station, off the DENSE oracle.
CongruenceSolve solveStation(const CongruenceNet& net, int r,
                             const Eigen::VectorXd& resid_all,
                             double delta0 = baardaDelta0(3.29)) {
    return congruenceStatistic(net.Hd[r], net.w[r], net.stationQ(r),
                               net.stationRows(resid_all, r), delta0);
}

/// The BLOCK card's statistic on the loudest LOS of one station: T_B = e' Q^-1 e / b.
double maxBlockT(const CongruenceNet& net, int r, const Eigen::VectorXd& resid_all) {
    double worst = 0.0;
    for (int k = 0; k < kS; ++k) {
        const std::vector<Eigen::Index> rows = net.denseRows(r, k);
        const Eigen::Index b = static_cast<Eigen::Index>(rows.size());
        Eigen::MatrixXd Qb(b, b);
        Eigen::VectorXd eb(b);
        for (Eigen::Index i = 0; i < b; ++i) {
            eb(i) = resid_all(rows[static_cast<std::size_t>(i)]);
            for (Eigen::Index j = 0; j < b; ++j)
                Qb(i, j) = net.dense.Q(rows[static_cast<std::size_t>(i)],
                                       rows[static_cast<std::size_t>(j)]);
        }
        const double q = eb.dot(Qb.ldlt().solve(eb)) / static_cast<double>(b);
        if (std::isfinite(q) && q > worst) worst = q;
    }
    return worst;
}

/// A network-wide observation draw y ~ N(0, sigma^2 P^-1).
Eigen::VectorXd drawGaussian(const CongruenceNet& net, std::mt19937& rng,
                             double sigma) {
    std::normal_distribution<double> g(0.0, 1.0);
    Eigen::VectorXd y(kR * kRowsPerSta);
    for (int r = 0; r < kR; ++r)
        for (Eigen::Index i = 0; i < kRowsPerSta; ++i)
            y(r * kRowsPerSta + i) = sigma * g(rng) / std::sqrt(net.w[r](i));
    return y;
}

/// The same, with Student-t(4) errors normalized to unit variance — heavier tails
/// at the same second moment, which is the shape D5 measured on real residuals.
Eigen::VectorXd drawStudentT(const CongruenceNet& net, std::mt19937& rng) {
    std::student_t_distribution<double> t(4.0);
    const double norm = std::sqrt(4.0 / (4.0 - 2.0));   // Var[t_nu] = nu/(nu-2)
    Eigen::VectorXd y(kR * kRowsPerSta);
    for (int r = 0; r < kR; ++r)
        for (Eigen::Index i = 0; i < kRowsPerSta; ++i)
            y(r * kRowsPerSta + i) = (t(rng) / norm) / std::sqrt(net.w[r](i));
    return y;
}

/// The prior pseudo-observations, drawn at their own stated variances (1/info).
/// Required for a valid H0 — see DenseGls::residualsDrawnPrior.  Parameters with
/// no prior get zero, where the zero information makes the value irrelevant.
Eigen::VectorXd drawPrior(const CongruenceNet& net, std::mt19937& rng,
                          double sigma) {
    std::normal_distribution<double> g(0.0, 1.0);
    const Eigen::VectorXd& pi = net.dense.prior_diag;
    Eigen::VectorXd b = Eigen::VectorXd::Zero(pi.size());
    for (Eigen::Index j = 0; j < pi.size(); ++j)
        if (pi(j) > 0.0) b(j) = sigma * g(rng) / std::sqrt(pi(j));
    return b;
}

/// The station assembly as the producer hands it to the sweep — including
/// `row_pos_enu` in the ASSEMBLY's convention (dy/dDelta = -u, straight out of
/// em.H), NOT the statistic's.  Building it here is the whole point of the
/// end-to-end test below: the negation lives in the sweep, and a sign error there
/// is invisible to every test that calls congruenceStatistic directly.
StationAssembly assemblyOf(const CongruenceNet& net, int r,
                           std::vector<NetRowClass>& cls) {
    StationAssembly R;
    R.ok = true;
    R.A_s = net.A_s[r];
    R.A_r = net.A_r[r];
    R.w   = net.w[r];
    R.y   = Eigen::VectorXd::Zero(kRowsPerSta);
    R.row_pos_enu = -net.Hd[r];          // the assembly stores dy/dDelta
    cls.assign(static_cast<std::size_t>(kRowsPerSta), NetRowClass::ObsCode);
    for (int k = 0; k < kS; ++k) {
        cls[static_cast<std::size_t>(rowOf(k, kL1))] = NetRowClass::ObsPhase;
        cls[static_cast<std::size_t>(rowOf(k, kL2))] = NetRowClass::ObsPhase;
    }
    return R;
}

}  // namespace

// ===========================================================================
//  0. THE SEAM — the assembly's convention through the sweep, sign included
// ===========================================================================

TEST(StationCongruenceTest, SweepSeamCarriesTheAssemblySignConventionThrough) {
    // Everything else in this file drives congruenceStatistic directly, which
    // means the ONE negation in the whole card — StationAssembly::row_pos_enu is
    // dy/dDelta, the reported quantity is d = -Delta — is exercised nowhere.  That
    // is exactly where a sign error survives a green suite and then names a real
    // 5 cm offset in the wrong direction.  This test drives the seam.
    NetworkNormalEquations neq(kS);
    const CongruenceNet net = buildNet(neq, Sky::Varied);

    const Eigen::Vector3d d(0.05, 0.0, 0.0);      // held mark 5 cm too far EAST
    const Eigen::VectorXd resid_all =
        net.dense.residuals(injectCoordinate(net, 1, d));

    NetworkCongruence acc(/*armed=*/true, /*window_epochs=*/0.0,
                          /*min_samples=*/1e12, /*report_sigma=*/4.0);
    NetworkCongruenceTallies tallies;
    std::vector<CongruenceHit> hits;
    acc.beginEpoch(0.0, 100.0);

    std::vector<NetRowClass> cls;
    const StationAssembly R = assemblyOf(net, 1, cls);
    networkCongruenceSweepStation(acc, neq, net.slot[1], /*station_index=*/1, R, cls,
                                  net.stationRows(resid_all, 1), net.cov_free,
                                  baardaDelta0(3.29), /*datum_changed=*/false,
                                  /*block_known=*/true, /*block_flagged=*/false,
                                  tallies, hits);

    ASSERT_EQ(tallies.station_sweeps, 1);
    ASSERT_EQ(hits.size(), 1u);
    EXPECT_EQ(tallies.rows, static_cast<long long>(kRowsPerSta));
    EXPECT_EQ(tallies.no_geom_rows, 0);
    EXPECT_EQ(tallies.no_rows, 0);
    EXPECT_EQ(tallies.solve_failed, 0);
    EXPECT_EQ(hits[0].station, 1);
    EXPECT_EQ(hits[0].n_code, kS * 2);
    EXPECT_EQ(hits[0].n_phase, kS * 2);
    EXPECT_EQ(hits[0].label, CongruenceLabel::Clean);
    ASSERT_EQ(hits[0].solve.dof, 3);
    // THE ASSERTION THIS TEST EXISTS FOR.
    EXPECT_NEAR(hits[0].solve.delta(0), 0.05, 1e-9);
    EXPECT_NEAR(hits[0].solve.delta(1), 0.0, 1e-9);
    EXPECT_NEAR(hits[0].solve.delta(2), 0.0, 1e-9);

    // A datum-changed epoch lands in the FRAME ladder, and rows with no geometry
    // are counted rather than quietly skipped.
    StationAssembly R2 = R;
    R2.row_pos_enu = Eigen::MatrixXd();          // INC-0 retention absent
    acc.beginEpoch(30.0, 130.0);
    networkCongruenceSweepStation(acc, neq, net.slot[1], 1, R2, cls,
                                  net.stationRows(resid_all, 1), net.cov_free,
                                  baardaDelta0(3.29), /*datum_changed=*/true,
                                  true, false, tallies, hits);
    EXPECT_EQ(tallies.station_sweeps, 2);
    EXPECT_EQ(tallies.no_rows, 1);
    EXPECT_EQ(tallies.no_geom_rows, static_cast<long long>(kRowsPerSta));
    EXPECT_EQ(hits.size(), 1u) << "a station with no geometry produces no record";
}

// ===========================================================================
//  1. RECOVERY AND LOCALIZATION — SPEC §6.1
// ===========================================================================

TEST(StationCongruenceTest, RecoversAndLocalizesAnInjectedCoordinateOffset) {
    NetworkNormalEquations neq(kS);
    const CongruenceNet net = buildNet(neq, Sky::Varied);

    // +5 cm EAST injected into station 1's HELD coordinate.
    const Eigen::Vector3d d(0.05, 0.0, 0.0);
    const Eigen::VectorXd resid = net.dense.residuals(injectCoordinate(net, 1, d));

    const CongruenceSolve hit = solveStation(net, 1, resid);
    ASSERT_TRUE(hit.ok);
    ASSERT_EQ(hit.dof, 3) << "the varied-elevation sky must leave all three ENU "
                             "directions observable, or this is not a recovery test";
    EXPECT_EQ(hit.b, static_cast<int>(kRowsPerSta));

    // THE SIGN CONVENTION, PINNED.  delta is the HELD COORDINATE'S ERROR, so a
    // +5 cm East perturbation of the held mark reads +0.05, not -0.05.  The
    // recovery is EXACT (delta = M^+ M d = d for full-rank M), so the tolerance
    // tests the whole chain rather than being fitted to it.
    EXPECT_NEAR(hit.delta(0), 0.05, 1e-9);
    EXPECT_NEAR(hit.delta(1), 0.0, 1e-9);
    EXPECT_NEAR(hit.delta(2), 0.0, 1e-9);
    EXPECT_GT(hit.T_raw, 1.0);

    // LOCALIZATION.  The offset propagates to the other stations through the
    // shared satellite clocks: the adjustment puts roughly 1/kR of each
    // per-satellite range bias into the satellite clock, and every other station
    // then reads that back as a small coordinate offset of its own.  That floor is
    // REAL — it is why the screen needs an over-determined clock block, and why
    // the SPEC grades a perturbed arm against an unperturbed control rather than
    // against zero.  What must hold is that the screen names the RIGHT monument.
    for (int r = 0; r < kR; ++r) {
        if (r == 1) continue;
        const CongruenceSolve other = solveStation(net, r, resid);
        ASSERT_TRUE(other.ok);
        EXPECT_LT(other.delta.norm(), 0.3 * d.norm())
            << "station " << r << " absorbed too much of station 1's offset";
        EXPECT_GT(hit.delta.norm(), 3.0 * other.delta.norm())
            << "station " << r << " is not clearly separated from the perturbed one";
        EXPECT_LT(other.T_raw, 0.1 * hit.T_raw) << "station " << r;
    }

    // …and the same offset injected NORTH lands in the north component, with no
    // east leakage: the estimate is a vector, not a magnitude.
    const Eigen::Vector3d dn(0.0, -0.03, 0.0);
    const CongruenceSolve north =
        solveStation(net, 1, net.dense.residuals(injectCoordinate(net, 1, dn)));
    ASSERT_TRUE(north.ok);
    EXPECT_NEAR(north.delta(0), 0.0, 1e-9);
    EXPECT_NEAR(north.delta(1), -0.03, 1e-9);
}

// ===========================================================================
//  2. ABSORPTION IS MEASURED, NOT WORKED AROUND — SPEC §6.2
// ===========================================================================

TEST(StationCongruenceTest, AbsorbedVerticalIsReportedUnobservableNotInverted) {
    // Every satellite at one elevation ⇒ the Up partial sin(el) is CONSTANT over
    // the station's rows, i.e. exactly proportional to the receiver-clock column.
    // The clock absorbs it perfectly and M's third eigenvalue is zero.
    NetworkNormalEquations neq(kS);
    const CongruenceNet net = buildNet(neq, Sky::Flat);

    const Eigen::VectorXd resid =
        net.dense.residuals(injectCoordinate(net, 0, Eigen::Vector3d(0.05, 0.0, 0.0)));
    const CongruenceSolve hit = solveStation(net, 0, resid);
    ASSERT_TRUE(hit.ok);

    // The collapse is STRUCTURAL, not numerical: assert it against the largest
    // eigenvalue rather than against an absolute floor.
    EXPECT_LT(hit.eval(0), 1e-10 * hit.eval(2));
    EXPECT_EQ(hit.dof, 2) << "the absorbed direction must be DROPPED from the "
                             "observable subspace, not regularized into it";

    // The weakest direction is Up, and it is named — a reader must be able to see
    // WHICH direction the clock took.
    EXPECT_NEAR(std::abs(hit.null_dir(2)), 1.0, 1e-6);

    // MDB: East and North stay finite and useful; Up is +infinity — a CATEGORY,
    // never a large finite number a consumer could act on.
    EXPECT_TRUE(std::isfinite(hit.mdb(0)));
    EXPECT_TRUE(std::isfinite(hit.mdb(1)));
    EXPECT_GT(hit.mdb(0), 0.0);
    EXPECT_FALSE(std::isfinite(hit.mdb(2)));
    EXPECT_GT(hit.mdb(2), hit.mdb(0));   // trivially true of +inf, and the point

    // The horizontal recovery still works in the observable subspace, so the
    // degeneracy costs the vertical and nothing else.
    EXPECT_NEAR(hit.delta(0), 0.05, 1e-8);
    EXPECT_NEAR(hit.delta(2), 0.0, 1e-8);

    // …and the varied sky, for contrast: every direction estimable, every MDB
    // finite.  Without this half, "Up is infinite" could be a property of the
    // implementation rather than of the geometry.
    NetworkNormalEquations neq2(kS);
    const CongruenceNet varied = buildNet(neq2, Sky::Varied);
    const CongruenceSolve v = solveStation(
        varied, 0,
        varied.dense.residuals(injectCoordinate(varied, 0, Eigen::Vector3d(0.05, 0, 0))));
    ASSERT_TRUE(v.ok);
    EXPECT_EQ(v.dof, 3);
    for (int k = 0; k < 3; ++k) {
        EXPECT_TRUE(std::isfinite(v.mdb(k))) << "component " << k;
        EXPECT_GT(v.mdb(k), 0.0) << "component " << k;
    }
}

// ===========================================================================
//  3. SEPARABILITY — the two tiers partition the fault classes (INC-3, SPEC §6.3)
// ===========================================================================

TEST(StationCongruenceTest, CoordinateAndSingleLosFaultsExciteDifferentTiers) {
    NetworkNormalEquations neq(kS);
    const CongruenceNet net = buildNet(neq, Sky::Varied);

    // (a) a coordinate offset: coherent across every line of sight.
    const Eigen::VectorXd r_coord = net.dense.residuals(
        injectCoordinate(net, 0, Eigen::Vector3d(0.05, 0.02, 0.0)));
    const CongruenceSolve coord = solveStation(net, 0, r_coord);
    const double coord_block = maxBlockT(net, 0, r_coord);
    ASSERT_TRUE(coord.ok);

    // (b) an unflagged slip on ONE line of sight — the block card's fault class.
    const Eigen::VectorXd r_los = net.dense.residuals(injectLosPhase(0, 2, 0.05));
    const CongruenceSolve los = solveStation(net, 0, r_los);
    const double los_block = maxBlockT(net, 0, r_los);
    ASSERT_TRUE(los.ok);

    ASSERT_GT(coord_block, 0.0);
    ASSERT_GT(los.T_raw, 0.0);

    // The claim is a RATIO claim, so it is invariant to how loud each injection
    // was made: the congruence tier's response RELATIVE to the block tier's is
    // orders of magnitude larger for a coordinate fault than for a single-LOS
    // fault.  Absolute thresholds here would only measure the injection sizes.
    const double coord_ratio = coord.T_raw / coord_block;
    const double los_ratio   = los.T_raw / los_block;
    EXPECT_GT(coord_ratio, 10.0 * los_ratio)
        << "coordinate ratio " << coord_ratio << " vs LOS ratio " << los_ratio;
    EXPECT_GT(coord_ratio, 1.0)  << "a coordinate fault must be louder HERE";
    EXPECT_LT(los_ratio, 1.0)    << "a single-LOS fault must be louder THERE";
}

// ===========================================================================
//  4. The kernel contraction IS the dense GLS quantity — SPEC §6.4
// ===========================================================================

TEST(StationCongruenceTest, KernelContractionMatchesDenseGls) {
    NetworkNormalEquations neq(kS);
    const CongruenceNet net = buildNet(neq, Sky::Varied);

    const Eigen::VectorXd resid = net.dense.residuals(
        injectCoordinate(net, 2, Eigen::Vector3d(0.04, -0.02, 0.01)));

    for (int r = 0; r < kR; ++r) {
        // The kernel's b x b cofactor over the station's rows — the same call the
        // sweep makes, over the same rows.
        const Eigen::MatrixXd Qk = neq.blockResidualCofactor(
            net.slot[r], net.A_s[r], net.A_r[r], net.w[r], net.cov_free);
        ASSERT_EQ(Qk.rows(), kRowsPerSta);
        const Eigen::MatrixXd Qd = net.stationQ(r);
        EXPECT_LT((Qk - Qd).cwiseAbs().maxCoeff()
                      / std::max(Qd.cwiseAbs().maxCoeff(), 1e-300), 1e-10)
            << "station " << r;

        // …and, more to the point, the 3x3 contraction and everything derived
        // from it.  A wrong marginalization returns a plausible symmetric 3x3;
        // only an independent oracle can tell.
        const Eigen::VectorXd e = net.stationRows(resid, r);
        const CongruenceSolve ck =
            congruenceStatistic(net.Hd[r], net.w[r], Qk, e, baardaDelta0(3.29));
        const CongruenceSolve cd =
            congruenceStatistic(net.Hd[r], net.w[r], Qd, e, baardaDelta0(3.29));
        ASSERT_TRUE(ck.ok);
        ASSERT_TRUE(cd.ok);
        EXPECT_EQ(ck.dof, cd.dof);
        EXPECT_LT((ck.delta - cd.delta).cwiseAbs().maxCoeff(), 1e-10) << "station " << r;
        EXPECT_NEAR(ck.T_raw, cd.T_raw, 1e-10 * std::max(1.0, cd.T_raw)) << "station " << r;
        for (int k = 0; k < 3; ++k)
            EXPECT_NEAR(ck.mdb(k), cd.mdb(k), 1e-9 * std::max(1.0, cd.mdb(k)))
                << "station " << r << " component " << k;
    }
}

// ===========================================================================
//  4b. The MDB formula, and that it is the pseudo-inverse's diagonal
// ===========================================================================

TEST(StationCongruenceTest, MdbIsDelta0TimesTheInformationPseudoInverseDiagonal) {
    NetworkNormalEquations neq(kS);
    const CongruenceNet net = buildNet(neq, Sky::Varied);
    const double delta0 = baardaDelta0(3.29);

    const Eigen::VectorXd resid =
        net.dense.residuals(injectCoordinate(net, 0, Eigen::Vector3d(0.01, 0, 0)));
    const CongruenceSolve hit = solveStation(net, 0, resid, delta0);
    ASSERT_TRUE(hit.ok);
    ASSERT_EQ(hit.dof, 3);

    // Rebuild M independently and invert it (full rank here), so the check does
    // not reuse the implementation's own pseudo-inverse.
    const Eigen::MatrixXd G = net.w[0].asDiagonal() * net.Hd[0];
    Eigen::Matrix3d M = G.transpose() * net.stationQ(0) * G;
    M = (0.5 * (M + M.transpose())).eval();
    const Eigen::Matrix3d Minv = M.inverse();
    for (int k = 0; k < 3; ++k)
        EXPECT_NEAR(hit.mdb(k), delta0 * std::sqrt(Minv(k, k)),
                    1e-9 * delta0 * std::sqrt(Minv(k, k)))
            << "component " << k;

    // delta_0 = k + Phi^-1(0.8) is the project's ONE reliability convention, and
    // the MDB is linear in it — a card that quietly used its own power would be
    // incommensurable with every PPP and network MDB in the tree.
    const CongruenceSolve twice = solveStation(net, 0, resid, 2.0 * delta0);
    for (int k = 0; k < 3; ++k)
        EXPECT_NEAR(twice.mdb(k), 2.0 * hit.mdb(k), 1e-9 * hit.mdb(k));
}

// ===========================================================================
//  5. The null ladder is chi-square_q, and the sigma0 rescale that makes it so
//     — SPEC §6.5
// ===========================================================================

TEST(StationCongruenceTest, NullDistributionIsChiSquareQAndStudentTOverOccupies) {
    NetworkNormalEquations neq(kS);
    const CongruenceNet net = buildNet(neq, Sky::Varied);

    constexpr int kDraws = 500;   // x kR stations = 5000 samples per arm
    std::mt19937 rng(20260804u);

    // Under H0 the numerator g = H'P e~ has covariance sigma0^2 M exactly (Q P Q
    // = Q), so T_raw * q ~ sigma0^2 chi-square_q.  With sigma0 = 1 that puts
    // E[T_raw] = 1 for every q — the property the ladder's "E = 1 under H0"
    // reading depends on, and the reason a rung means "k times the variance the
    // model allows" rather than "k sigma".
    double sum = 0.0;
    long long n = 0, over4 = 0;
    for (int i = 0; i < kDraws; ++i) {
        const Eigen::VectorXd resid = net.dense.residualsDrawnPrior(
            drawGaussian(net, rng, 1.0), drawPrior(net, rng, 1.0));
        for (int r = 0; r < kR; ++r) {
            const CongruenceSolve s = solveStation(net, r, resid);
            ASSERT_TRUE(s.ok);
            ASSERT_EQ(s.dof, 3);
            sum += s.T_raw;
            ++n;
            if (s.T_raw > 4.0) ++over4;
        }
    }
    const double mean = sum / static_cast<double>(n);
    EXPECT_NEAR(mean, 1.0, 0.08) << "E[T_raw] must be 1 under H0 for every q";

    // P(chi-square_3 > 12) = 0.00738.  A generous band: this is a distributional
    // sanity check, not a goodness-of-fit test, and SPEC 2.1's rule is that the
    // FIELD critical values are empirical precisely because this tail is wrong on
    // real residuals.
    const double gauss_frac = static_cast<double>(over4) / static_cast<double>(n);
    EXPECT_GT(gauss_frac, 0.002);
    EXPECT_LT(gauss_frac, 0.020);

    // THE D5 NEGATIVE, PRESERVED.  Same second moment, heavier tails: the upper
    // rung over-occupies.  A card that shipped the Gaussian critical value would
    // read this as a network full of coordinate faults.
    long long t_over4 = 0, t_n = 0;
    double t_sum = 0.0;
    std::mt19937 rng_t(20260805u);
    for (int i = 0; i < kDraws; ++i) {
        const Eigen::VectorXd resid = net.dense.residualsDrawnPrior(
            drawStudentT(net, rng_t), drawPrior(net, rng_t, 1.0));
        for (int r = 0; r < kR; ++r) {
            const CongruenceSolve s = solveStation(net, r, resid);
            ASSERT_TRUE(s.ok);
            t_sum += s.T_raw;
            ++t_n;
            if (s.T_raw > 4.0) ++t_over4;
        }
    }
    EXPECT_NEAR(t_sum / static_cast<double>(t_n), 1.0, 0.20)
        << "the t draw is normalized to unit variance, so the MEAN must not move "
           "— only the tail";
    EXPECT_GT(static_cast<double>(t_over4) / static_cast<double>(t_n),
              1.5 * gauss_frac)
        << "Student-t over-occupancy at rung 4 is the empirical-critical-values "
           "rule's evidence; losing it means the draw is no longer heavy-tailed";
}

TEST(StationCongruenceTest, Sigma0RecoversTheTrueScaleThroughTheChiSquareQRescale) {
    // NetworkCarryVce inverts the geometric mean of a chi-square_1 variate.  A
    // chi-square_3 statistic fed as sqrt(T) would read 2 exp(psi(3/2))/3 = 0.5611
    // instead of 0.2807 — sigma0 low by sqrt(0.2807/0.5611) = 0.707, i.e. every t
    // inflated by 2x.  This is the block card's correction, re-verified at q = 3
    // because a sibling card getting it wrong produces a plausible ladder.
    constexpr double kTrueSigma = 2.0;
    NetworkCongruence acc(/*armed=*/true, /*window_epochs=*/0.0,
                          /*min_samples=*/50.0, /*report_sigma=*/4.0);

    std::mt19937 rng(4242u);
    std::chi_squared_distribution<double> chi3(3.0);
    for (int ep = 0; ep < 400; ++ep) {
        acc.beginEpoch(static_cast<double>(ep) * 30.0, static_cast<double>(ep) * 30.0);
        NetworkCongruence::Input in;
        in.station = 0;
        in.solve.ok = true;
        in.solve.dof = 3;
        in.solve.b = 24;
        // T_raw = sigma^2 * chi-square_3 / 3, the null this card's feed produces.
        in.solve.T_raw = kTrueSigma * kTrueSigma * chi3(rng) / 3.0;
        acc.addStation(in);
    }
    acc.beginEpoch(12000.0, 12000.0);   // one more predict to publish the estimate
    EXPECT_TRUE(acc.sigma0Warm());
    EXPECT_NEAR(acc.sigma0(), kTrueSigma, 0.15 * kTrueSigma);

    // …and the calibration indicator says so: geoT() -> 1 when the scale has
    // converged, for any q.  A raw geometric mean of t would sit at
    // 2 exp(psi(q/2))/q and drift with how many stations were rank-2.
    const NetworkCongruence::Ladder& L =
        acc.runLadder(0, CongruenceLabel::Clean);
    EXPECT_GT(L.n_geo, 300);
    EXPECT_NEAR(L.geoT(), 1.0, 0.25);
}

// ===========================================================================
//  6. The bookkeeping contracts — SPEC §6.6
// ===========================================================================

TEST(StationCongruenceTest, CleanAndFrameLaddersAreSeparateAndBothCounted) {
    NetworkCongruence acc(/*armed=*/true, /*window_epochs=*/0.0,
                          /*min_samples=*/0.0, /*report_sigma=*/4.0);
    for (int ep = 0; ep < 10; ++ep) {
        acc.beginEpoch(ep * 30.0, ep * 30.0);
        NetworkCongruence::Input in;
        in.station = 7;
        in.solve.ok = true;
        in.solve.dof = 3;
        in.solve.b = 24;
        in.solve.T_raw = 1.0;
        in.label = (ep % 5 == 0) ? CongruenceLabel::Frame : CongruenceLabel::Clean;
        acc.addStation(in);
    }
    // A datum re-realization is a population to MEASURE, not to drop (invariant
    // #7's both-edges rule); the split is what keeps it from contaminating the
    // CLEAN ladder while still being counted.
    EXPECT_EQ(acc.runLadder(7, CongruenceLabel::Clean).n_epochs, 8);
    EXPECT_EQ(acc.runLadder(7, CongruenceLabel::Frame).n_epochs, 2);
    EXPECT_EQ(acc.stationEpochs(7), 10);
    EXPECT_EQ(acc.deltaStats(7, CongruenceLabel::Clean).n, 8);
    EXPECT_EQ(acc.deltaStats(7, CongruenceLabel::Frame).n, 2);
}

TEST(StationCongruenceTest, MedianAndMadAccumulateWithoutASqrtNClaim) {
    NetworkCongruence acc(/*armed=*/true, /*window_epochs=*/0.0,
                          /*min_samples=*/0.0, /*report_sigma=*/4.0);
    // Nine epochs of a persistent +5 cm East offset with a deliberate outlier:
    // the MEDIAN must ignore it, which is exactly why the card reports median/MAD
    // rather than a mean +- sigma/sqrt(N) (D1 measured K_eff(30) ~ 2, so per-epoch
    // samples do NOT average as white and a sqrt(N) claim would be a fiction).
    const double east[9] = {0.048, 0.052, 0.050, 0.049, 0.051,
                            0.050, 0.500, 0.049, 0.051};
    for (int ep = 0; ep < 9; ++ep) {
        acc.beginEpoch(ep * 30.0, ep * 30.0);
        NetworkCongruence::Input in;
        in.station = 3;
        in.solve.ok = true;
        in.solve.dof = 3;
        in.solve.b = 24;
        in.solve.T_raw = 1.0;
        in.solve.delta = Eigen::Vector3d(east[ep], 0.0, 0.0);
        in.solve.mdb = Eigen::Vector3d(0.10, 0.12,
                                       std::numeric_limits<double>::infinity());
        acc.addStation(in);
    }
    const NetworkCongruence::DeltaStats d = acc.deltaStats(3, CongruenceLabel::Clean);
    EXPECT_EQ(d.n, 9);
    EXPECT_NEAR(d.med(0), 0.050, 1e-9);
    EXPECT_LT(d.mad(0), 0.01) << "the MAD must not be dragged by the outlier";
    EXPECT_NEAR(d.med(1), 0.0, 1e-12);

    // The MDB readout keeps the unobservable epochs OUT of the median and reports
    // them as their own COUNT — a median over a pool containing infinity is either
    // meaningless or silently censored depending on the sort.
    const NetworkCongruence::MdbStats q = acc.mdbStats(3, CongruenceLabel::Clean);
    EXPECT_NEAR(q.med(0), 0.10, 1e-12);
    EXPECT_NEAR(q.med(1), 0.12, 1e-12);
    EXPECT_EQ(q.n_inf[0], 0);
    EXPECT_EQ(q.n_inf[2], 9);
    EXPECT_FALSE(std::isfinite(q.med(2)));   // no finite sample to take a median of
}

TEST(StationCongruenceTest, NullGeometryIsCountedNeverInvertedThrough) {
    NetworkCongruence acc(/*armed=*/true, /*window_epochs=*/0.0,
                          /*min_samples=*/0.0, /*report_sigma=*/4.0);
    acc.beginEpoch(0.0, 0.0);
    NetworkCongruence::Input in;
    in.station = 1;
    in.solve.ok = false;        // no observable direction at all
    in.solve.dof = 0;
    in.solve.b = 24;
    const NetworkCongruence::StationStat st = acc.addStation(in);
    // Counted with its identity — "swept and quiet" and "no geometry to sweep"
    // are different findings (invariant #4) and must not share a cell.
    EXPECT_TRUE(st.accepted);
    EXPECT_TRUE(st.null_geom);
    EXPECT_TRUE(st.degenerate);
    EXPECT_FALSE(st.reported);
    const NetworkCongruence::Ladder& L = acc.runLadder(1, CongruenceLabel::Clean);
    EXPECT_EQ(L.n_epochs, 1);
    EXPECT_EQ(L.n_null, 1);
    EXPECT_EQ(L.n_degenerate, 1);
    EXPECT_EQ(L.n_geo, 0) << "a null-geometry epoch must not touch the calibration";
    EXPECT_EQ(acc.deltaStats(1, CongruenceLabel::Clean).n, 0);
}

TEST(StationCongruenceTest, SeparabilityTableIsUnpopulatedWithoutABlockVerdict) {
    // min_samples is deliberately unreachable so sigma0 stays RAW FORMAL (== 1)
    // and t == T_raw: this test is about which contingency cell a verdict lands
    // in, and with a live calibration the four hand-picked T values would each
    // re-scale the next one's threshold.  (That is not a defect of the
    // accumulator — it is the never-censor contract doing its job — but it makes
    // a four-sample contingency fixture untestable without freezing the scale.)
    NetworkCongruence acc(/*armed=*/true, /*window_epochs=*/0.0,
                          /*min_samples=*/1e12, /*report_sigma=*/4.0);
    auto feed = [&acc](int ep, bool known, bool flagged, double T) {
        acc.beginEpoch(ep * 30.0, ep * 30.0);
        NetworkCongruence::Input in;
        in.station = 5;
        in.solve.ok = true;
        in.solve.dof = 3;
        in.solve.b = 24;
        in.solve.T_raw = T;
        in.block_known = known;
        in.block_flagged = flagged;
        return acc.addStation(in);
    };
    // Loud here, quiet there — the a-cell, the population only this card sees.
    feed(0, true, false, 100.0);
    // Loud in both — the two tiers firing on one event.
    feed(1, true, true, 100.0);
    // Quiet here, loud there — the block tier's own class.
    feed(2, true, true, 0.5);
    // No block census armed: NO verdict exists, which is not the same as a
    // negative one, so nothing may be counted.
    feed(3, false, false, 100.0);

    const auto c = acc.separability();
    EXPECT_EQ(c[0][0], 1);   // congruence loud, block quiet
    EXPECT_EQ(c[0][1], 1);   // both loud
    EXPECT_EQ(c[1][1], 1);   // congruence quiet, block loud
    EXPECT_EQ(c[1][0], 0);
    EXPECT_EQ(acc.separabilityKnown(), 3)
        << "the unarmed-block epoch must not enter the table at all";
}

TEST(StationCongruenceTest, UnarmedInstanceIsCompletelyInert) {
    // OFF byte-identity, in the form a unit test can assert: an unarmed
    // accumulator accepts nothing, allocates nothing, and answers every readout
    // with an empty result — so no ladder, no sample and no event can exist on a
    // run whose master knob is off.
    NetworkCongruence off(/*armed=*/false);
    EXPECT_FALSE(off.enabled());
    off.beginEpoch(0.0, 0.0);
    NetworkCongruence::Input in;
    in.station = 0;
    in.solve.ok = true;
    in.solve.dof = 3;
    in.solve.T_raw = 1e6;
    const NetworkCongruence::StationStat st = off.addStation(in);
    EXPECT_FALSE(st.accepted);
    EXPECT_FALSE(st.reported);
    EXPECT_TRUE(off.stations().empty());
    EXPECT_EQ(off.epochs(), 0);
    EXPECT_EQ(off.stationEpochs(0), 0);
    EXPECT_EQ(off.droppedSamples(), 0);

    // …and the INC-0 retention it gates: a default StationAssembly carries no
    // geometry, which is what makes the sweep's `row < row_pos_enu.rows()` test
    // the structural OFF guard rather than a convenience.
    const StationAssembly bare;
    EXPECT_EQ(bare.row_pos_enu.rows(), 0);
    EXPECT_EQ(bare.row_pos_enu.cols(), 0);
}

TEST(StationCongruenceTest, DegenerateInputsAreRejectedNotPapedOver) {
    const double delta0 = baardaDelta0(3.29);
    const Eigen::MatrixXd H = Eigen::MatrixXd::Identity(3, 3);
    const Eigen::VectorXd w = Eigen::VectorXd::Ones(3);
    const Eigen::VectorXd e = Eigen::VectorXd::Ones(3);
    const Eigen::MatrixXd Q = Eigen::MatrixXd::Identity(3, 3);

    EXPECT_TRUE(congruenceStatistic(H, w, Q, e, delta0).ok);
    // Shape mismatches, a non-finite entry and an all-zero information matrix are
    // each a REJECT, never a plausible number: this statistic names a monument.
    EXPECT_FALSE(congruenceStatistic(H, w, Q, Eigen::VectorXd::Ones(2), delta0).ok);
    EXPECT_FALSE(congruenceStatistic(H, Eigen::VectorXd::Ones(2), Q, e, delta0).ok);
    Eigen::MatrixXd Hbad = H;
    Hbad(1, 1) = std::numeric_limits<double>::quiet_NaN();
    EXPECT_FALSE(congruenceStatistic(Hbad, w, Q, e, delta0).ok);
    const CongruenceSolve zero = congruenceStatistic(
        H, w, Eigen::MatrixXd::Zero(3, 3), e, delta0);
    EXPECT_FALSE(zero.ok);
    EXPECT_EQ(zero.dof, 0);
    for (int k = 0; k < 3; ++k) EXPECT_FALSE(std::isfinite(zero.mdb(k)));
}

TEST(StationCongruenceTest, RetentionCapIsCountedNeverSilent) {
    NetworkCongruence acc(/*armed=*/true, /*window_epochs=*/0.0,
                          /*min_samples=*/0.0, /*report_sigma=*/4.0);
    const std::size_t n = kCongruenceMaxSamples + 37;
    for (std::size_t ep = 0; ep < n; ++ep) {
        acc.beginEpoch(static_cast<double>(ep), static_cast<double>(ep));
        NetworkCongruence::Input in;
        in.station = 2;
        in.solve.ok = true;
        in.solve.dof = 3;
        in.solve.b = 24;
        in.solve.T_raw = 1.0;
        acc.addStation(in);
    }
    // Every epoch is on the LADDER (that is O(1) and never truncates); only the
    // retained SAMPLES are capped, and the overflow is a reported number so a
    // truncated median never reads as a whole-run one.
    EXPECT_EQ(acc.runLadder(2, CongruenceLabel::Clean).n_epochs,
              static_cast<long long>(n));
    EXPECT_EQ(acc.deltaStats(2, CongruenceLabel::Clean).n,
              static_cast<long long>(kCongruenceMaxSamples));
    EXPECT_EQ(acc.droppedSamples(), 37);
}

}  // namespace GnssProcessor
