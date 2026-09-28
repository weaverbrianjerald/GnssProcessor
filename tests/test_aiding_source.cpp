/**
 * @file test_aiding_source.cpp
 * @brief Unit tests for the aiding-source abstraction (nav-readiness item 1).
 *
 * Scope (Steps 0-2 of docs/aiding_source_abstraction_design.md; the later
 * sections cover streaming IMU ingest (Step 5), appendAidingRow and the ZUPT
 * source (Step 6)):
 *   - AidingSourceRegistry: kind<->token mapping, availability (IMU and ZUPT
 *     built), create()/createEnabled() selection, staged-source warn-skip.
 *   - AidingSourceManager: default-config (IMU disabled) yields the empty
 *     GNSS-only fast path.
 *   - ImuAidingSource::mapPredictionToContribution (private projection kernel,
 *     via ImuAidingSourceTestAccess): a synthetic ImuPrediction projects onto
 *     the ins_map with the same nominal write-backs predictImu applies.
 *   - KalmanFilter::spliceAndPropagate (private assembly kernel, via
 *     KalmanFilterTestAccess): the aided-predict matrix assembly is exercised
 *     directly.  The full KalmanFilter cannot be unit-constructed (it needs a
 *     PppModel + parser chain; see test_filter_core.cpp), so end-to-end
 *     predictImu-vs-predictAided bit-identity is the Step-4 trace-diff gate.
 *     The kernel test pins the contract: Identity seed, assignment (not +=) of
 *     the contribution Q, nominal write-backs, sentinel skipping, symmetrization.
 */

#include <gtest/gtest.h>

#include "aiding/AidingSourceRegistry.hpp"
#include "aiding/AidingSourceManager.hpp"
#include "aiding/ImuAidingSource.hpp"
#include "aiding/ZuptAidingSource.hpp"
#include "filter/KalmanFilter.hpp"
#include "filter/StateManager.hpp"
#include "measurement/MeasurementModel.hpp"
#include "common/EpochMeasurement.hpp"
#include "config/GnssConfig.hpp"
#include "logging/LogManager.hpp"

#include <Eigen/Dense>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <limits>
#include <vector>

namespace GnssProcessor {

// ---- Test seams (house XxxTestAccess friend-shim idiom) --------------------

/// Reaches ImuAidingSource's private projection kernel.
struct ImuAidingSourceTestAccess {
    static PredictContribution mapPrediction(const ImuAidingSource& src,
                                             const ImuPrediction& pred,
                                             const std::vector<std::size_t>& ins_map) {
        return src.mapPredictionToContribution(pred, ins_map);
    }
};

/// Reaches KalmanFilter's private static aided-predict assembly kernel.
struct KalmanFilterTestAccess {
    static void spliceAndPropagate(Eigen::VectorXd& x, Eigen::MatrixXd& P,
                                   Eigen::MatrixXd& Q_full, std::size_t state_size,
                                   const std::vector<PredictContribution>& contributions) {
        KalmanFilter::spliceAndPropagate(x, P, Q_full, state_size, contributions);
    }
};

/// Reaches ZuptAidingSource's private row-building kernel.
struct ZuptAidingSourceTestAccess {
    static MeasurementRows buildRows(const Eigen::VectorXd& state, std::size_t vel_idx,
                                     std::size_t state_size, double sigma_mps) {
        return ZuptAidingSource::buildZuptRows(state, vel_idx, state_size, sigma_mps);
    }
};

namespace {

constexpr std::size_t kNoIndex = std::numeric_limits<std::size_t>::max();

// ============================================================================
// AidingSourceKind <-> token
// ============================================================================

TEST(AidingKind, ToStringFromStringRoundTrip) {
    const AidingSourceKind kinds[] = {
        AidingSourceKind::Imu, AidingSourceKind::Zupt, AidingSourceKind::Nhc,
        AidingSourceKind::WheelOdom, AidingSourceKind::Baro, AidingSourceKind::Vision };
    for (auto k : kinds) {
        auto parsed = aidingKindFromString(toString(k));
        ASSERT_TRUE(parsed.has_value()) << toString(k);
        EXPECT_EQ(*parsed, k);
    }
    EXPECT_FALSE(aidingKindFromString("not_a_source").has_value());
    // Case-insensitive + synonyms.
    EXPECT_EQ(aidingKindFromString("IMU"), AidingSourceKind::Imu);
    EXPECT_EQ(aidingKindFromString("inertial"), AidingSourceKind::Imu);
}

// ============================================================================
// AidingSourceRegistry
// ============================================================================

TEST(AidingRegistry, BuiltKindsAreImuAndZupt) {
    EXPECT_TRUE(AidingSourceRegistry::isAvailable(AidingSourceKind::Imu));
    EXPECT_TRUE(AidingSourceRegistry::isAvailable(AidingSourceKind::Zupt));
    // Not yet implemented / staged.
    EXPECT_FALSE(AidingSourceRegistry::isAvailable(AidingSourceKind::Nhc));
    EXPECT_FALSE(AidingSourceRegistry::isAvailable(AidingSourceKind::WheelOdom));
    EXPECT_FALSE(AidingSourceRegistry::isAvailable(AidingSourceKind::Baro));
    EXPECT_FALSE(AidingSourceRegistry::isAvailable(AidingSourceKind::Vision));

    EXPECT_EQ(AidingSourceRegistry::availableKinds().size(), 2u);
}

TEST(AidingRegistry, CreateImuYieldsTypedImuSource) {
    LogManager log;
    GnssConfig config;  // default: IMU disabled, empty file -> inert but constructible.
    auto s = AidingSourceRegistry::create(AidingSourceKind::Imu, config, log);
    ASSERT_NE(s, nullptr);
    EXPECT_EQ(s->kind(), AidingSourceKind::Imu);
    EXPECT_NE(s->asImu(), nullptr);     // typed down-cast recovers the IMU.
    EXPECT_FALSE(s->name().empty());
    EXPECT_TRUE(s->bridgesGaps());      // IMU bridges GNSS outages.
    EXPECT_FALSE(s->hasData());         // no file configured.
}

TEST(AidingRegistry, CreateUnbuiltKindsReturnNull) {
    LogManager log;
    GnssConfig config;
    EXPECT_EQ(AidingSourceRegistry::create(AidingSourceKind::Vision, config, log), nullptr);
    EXPECT_EQ(AidingSourceRegistry::create(AidingSourceKind::Nhc, config, log), nullptr);
    EXPECT_EQ(AidingSourceRegistry::create(AidingSourceKind::WheelOdom, config, log), nullptr);
}

TEST(AidingRegistry, CreateEnabledDefaultConfigIsEmpty) {
    LogManager log;
    GnssConfig config;  // IMU disabled by default -> aiding.imu defaults false.
    auto sources = AidingSourceRegistry::createEnabled(config, log);
    EXPECT_TRUE(sources.empty());
}

// ============================================================================
// AidingSourceManager
// ============================================================================

TEST(AidingManager, DefaultConfigIsEmptyGnssOnlyFastPath) {
    LogManager log;
    GnssConfig config;
    AidingSourceManager mgr(config, log);
    EXPECT_TRUE(mgr.empty());
    EXPECT_EQ(mgr.size(), 0u);
    EXPECT_EQ(mgr.imu(), nullptr);
    EXPECT_FALSE(mgr.anySourceBridgesGaps());
}

// ============================================================================
// ImuAidingSource::mapPredictionToContribution (projection kernel)
// ============================================================================

// A fully-populated synthetic prediction with deterministic, distinct entries.
static ImuPrediction makeSyntheticPrediction() {
    ImuPrediction pred;
    pred.position = Eigen::Vector3d(1.0, 2.0, 3.0);
    pred.velocity = Eigen::Vector3d(4.0, 5.0, 6.0);
    pred.attitude = Eigen::Matrix3d::Identity();
    pred.Phi = Eigen::MatrixXd(15, 15);
    pred.Q_ins = Eigen::MatrixXd(15, 15);
    for (int i = 0; i < 15; ++i)
        for (int j = 0; j < 15; ++j) {
            pred.Phi(i, j)   = 1.0 + i * 15 + j;
            pred.Q_ins(i, j) = 100.0 + i * 15 + j;
        }
    pred.dt_total = 0.5;
    pred.num_samples = 7;
    pred.valid = true;
    return pred;
}

TEST(ImuProjection, FullInsMapPlacesPhiQAndNominalWriteBacks) {
    LogManager log;
    GnssConfig config;  // not tightly-coupled -> requires_naive_relinearization false.
    ImuAidingSource src(config, log);

    const ImuPrediction pred = makeSyntheticPrediction();
    std::vector<std::size_t> ins_map(15);
    for (std::size_t k = 0; k < 15; ++k) ins_map[k] = 10 + k;  // distinct, in-range

    const PredictContribution c =
        ImuAidingSourceTestAccess::mapPrediction(src, pred, ins_map);

    EXPECT_TRUE(c.valid);
    EXPECT_EQ(c.source, AidingSourceKind::Imu);
    EXPECT_EQ(c.indices, ins_map);
    EXPECT_TRUE(c.Phi.isApprox(pred.Phi));
    EXPECT_TRUE(c.Q.isApprox(pred.Q_ins));
    EXPECT_DOUBLE_EQ(c.dt_total, 0.5);
    EXPECT_EQ(c.num_samples, 7u);
    EXPECT_FALSE(c.requires_naive_relinearization);

    // Nominal write-backs: pos(3) <- position, vel(3) <- velocity, att(3) <- 0.
    ASSERT_EQ(c.nominal_indices.size(), 9u);
    ASSERT_EQ(static_cast<std::size_t>(c.nominal_values.size()), 9u);
    const std::vector<std::size_t> expect_idx = { 10, 11, 12, 13, 14, 15, 16, 17, 18 };
    const double expect_val[9] = { 1, 2, 3, 4, 5, 6, 0, 0, 0 };
    EXPECT_EQ(c.nominal_indices, expect_idx);
    for (int k = 0; k < 9; ++k)
        EXPECT_DOUBLE_EQ(c.nominal_values(k), expect_val[k]);
}

TEST(ImuProjection, AbsentVelocitySlotOmitsVelocityNominalWrites) {
    LogManager log;
    GnssConfig config;
    ImuAidingSource src(config, log);

    const ImuPrediction pred = makeSyntheticPrediction();
    std::vector<std::size_t> ins_map(15);
    for (std::size_t k = 0; k < 15; ++k) ins_map[k] = 10 + k;
    ins_map[3] = ins_map[4] = ins_map[5] = kNoIndex;  // velocity absent

    const PredictContribution c =
        ImuAidingSourceTestAccess::mapPrediction(src, pred, ins_map);

    // pos(3) + att(3); velocity dropped.
    ASSERT_EQ(c.nominal_indices.size(), 6u);
    const std::vector<std::size_t> expect_idx = { 10, 11, 12, 16, 17, 18 };
    const double expect_val[6] = { 1, 2, 3, 0, 0, 0 };
    EXPECT_EQ(c.nominal_indices, expect_idx);
    for (int k = 0; k < 6; ++k)
        EXPECT_DOUBLE_EQ(c.nominal_values(k), expect_val[k]);
}

TEST(ImuProjection, InvalidPredictionYieldsInvalidContribution) {
    LogManager log;
    GnssConfig config;
    ImuAidingSource src(config, log);

    ImuPrediction pred = makeSyntheticPrediction();
    pred.valid = false;
    std::vector<std::size_t> ins_map(15, 0);

    const PredictContribution c =
        ImuAidingSourceTestAccess::mapPrediction(src, pred, ins_map);
    EXPECT_FALSE(c.valid);
}

// ============================================================================
// KalmanFilter::spliceAndPropagate (aided-predict assembly kernel)
// ============================================================================

// Build the canonical IMU-style contribution used by the kernel tests:
// owns full-state indices {2,3}, Phi=[[1,0.5],[0,1]], Q=[[0.1,0],[0,0.2]],
// writes x(2)=5.0.
static PredictContribution makeBlockContribution() {
    PredictContribution c;
    c.valid = true;
    c.dt_total = 1.0;
    c.indices = { 2, 3 };
    c.Phi = (Eigen::MatrixXd(2, 2) << 1.0, 0.5, 0.0, 1.0).finished();
    c.Q   = (Eigen::MatrixXd(2, 2) << 0.1, 0.0, 0.0, 0.2).finished();
    c.nominal_indices = { 2 };
    c.nominal_values = (Eigen::VectorXd(1) << 5.0).finished();
    return c;
}

TEST(SpliceAndPropagate, AssignsPhiQNominalAndPropagates) {
    constexpr std::size_t N = 6;
    Eigen::VectorXd x = Eigen::VectorXd::Zero(N);
    Eigen::MatrixXd P = Eigen::MatrixXd::Identity(N, N) * 2.0;
    Eigen::MatrixXd Q = Eigen::MatrixXd::Identity(N, N) * 0.001;

    KalmanFilterTestAccess::spliceAndPropagate(x, P, Q, N, { makeBlockContribution() });

    // Nominal write-back.
    EXPECT_DOUBLE_EQ(x(2), 5.0);
    EXPECT_DOUBLE_EQ(x(0), 0.0);
    EXPECT_DOUBLE_EQ(x(3), 0.0);

    // P = F P0 Fᵀ + Q_full, with P0 = 2I.  Hand-computed:
    //   F(2,2)=1, F(2,3)=0.5, F(3,3)=1 ; Q assigned: Q(2,2)=0.1, Q(3,3)=0.2.
    //   P(2,2) = 2*(1+0.25) + 0.1 = 2.6   <-- 2.601 would mean Q was accumulated (+=).
    EXPECT_DOUBLE_EQ(P(2, 2), 2.6);
    EXPECT_DOUBLE_EQ(P(2, 3), 1.0);
    EXPECT_DOUBLE_EQ(P(3, 2), 1.0);          // symmetric
    EXPECT_DOUBLE_EQ(P(3, 3), 2.2);
    EXPECT_DOUBLE_EQ(P(0, 0), 2.001);        // untouched state keeps baseline Q
    EXPECT_DOUBLE_EQ(P(4, 4), 2.001);
}

TEST(SpliceAndPropagate, AllInvalidContributionsLeaveIdentityPropagation) {
    constexpr std::size_t N = 4;
    Eigen::VectorXd x = Eigen::VectorXd::Zero(N);
    Eigen::MatrixXd P = Eigen::MatrixXd::Identity(N, N) * 2.0;
    Eigen::MatrixXd Q = Eigen::MatrixXd::Identity(N, N) * 0.001;

    PredictContribution invalid;  // .valid == false
    KalmanFilterTestAccess::spliceAndPropagate(x, P, Q, N, { invalid });

    // F stays Identity, no nominal writes: P = P0 + Q_full, x unchanged.
    EXPECT_DOUBLE_EQ(x(2), 0.0);
    EXPECT_DOUBLE_EQ(P(2, 2), 2.001);
    EXPECT_DOUBLE_EQ(P(0, 0), 2.001);
}

TEST(SpliceAndPropagate, SentinelOwnedIndexIsSkipped) {
    constexpr std::size_t N = 6;
    Eigen::VectorXd x = Eigen::VectorXd::Zero(N);
    Eigen::MatrixXd P = Eigen::MatrixXd::Identity(N, N) * 2.0;
    Eigen::MatrixXd Q = Eigen::MatrixXd::Identity(N, N) * 0.001;

    PredictContribution c;
    c.valid = true;
    c.indices = { 2, kNoIndex };   // second slot is a sentinel -> skipped
    c.Phi = (Eigen::MatrixXd(2, 2) << 1.5, 9.0, 9.0, 9.0).finished();
    c.Q   = (Eigen::MatrixXd(2, 2) << 0.3, 9.0, 9.0, 9.0).finished();

    KalmanFilterTestAccess::spliceAndPropagate(x, P, Q, N, { c });

    // Only (2,2) is written: F(2,2)=1.5, Q(2,2)=0.3 -> P(2,2)=2*(1.5^2)+0.3=4.8.
    EXPECT_DOUBLE_EQ(P(2, 2), 4.8);
    // The sentinel slot must not have touched any other entry.
    EXPECT_DOUBLE_EQ(P(3, 3), 2.001);
    EXPECT_DOUBLE_EQ(P(2, 3), 0.0);
}

// ============================================================================
// Streaming IMU ingest (Step 5: RT-IMU plumbing)
// ============================================================================

TEST(ImuStreamingIngest, AddSampleMatchesBatchLoad) {
    LogManager log;
    GnssConfig config;

    struct Row { int week; double tow, ax, ay, az, wx, wy, wz; };
    const std::vector<Row> rows = {
        { 2313, 100.25,  0.013, -0.024, 9.806,  0.0011,  0.0022, -0.0033 },
        { 2313, 100.50,  0.021, -0.011, 9.799,  0.0021,  0.0011, -0.0021 },
        { 2313, 100.75, -0.004,  0.013, 9.791,  0.0009, -0.0004, -0.0011 },
        { 2313, 101.00,  0.008,  0.019, 9.815, -0.0002,  0.0013,  0.0004 },
    };

    // Batch: write a temp CSV (full precision so the text round-trips exactly)
    // and loadData.
    namespace fs = std::filesystem;
    const fs::path tmp = fs::temp_directory_path() / "gnss_aiding_imu_stream_test.csv";
    {
        std::ofstream f(tmp);
        f << std::setprecision(17);
        f << "# week,tow,ax,ay,az,wx,wy,wz\n";
        for (const auto& r : rows)
            f << r.week << ',' << r.tow << ',' << r.ax << ',' << r.ay << ',' << r.az
              << ',' << r.wx << ',' << r.wy << ',' << r.wz << '\n';
    }
    ImuIntegrator batch(config, log);
    ASSERT_TRUE(batch.loadData(tmp));
    fs::remove(tmp);

    // Streaming: addSample row-by-row (the feedImuSample path).
    ImuIntegrator stream(config, log);
    for (const auto& r : rows) {
        ImuSample s;
        s.time.week = r.week;
        s.time.tow  = r.tow;
        s.accel = Eigen::Vector3d(r.ax, r.ay, r.az);
        s.gyro  = Eigen::Vector3d(r.wx, r.wy, r.wz);
        stream.addSample(s);
    }
    ASSERT_EQ(stream.sampleCount(), batch.sampleCount());

    // Identical integration interval + initial state -> identical prediction.
    const GnssTime t0{ 2313, 100.0 }, t1{ 2313, 101.0 };
    const Eigen::Vector3d pos(-2281621.77, -1453585.12, 5756964.83);
    const Eigen::Vector3d vel = Eigen::Vector3d::Zero();
    const Eigen::Matrix3d C = Eigen::Matrix3d::Identity();
    const Eigen::Vector3d ba = Eigen::Vector3d::Zero(), bg = Eigen::Vector3d::Zero();
    const ImuPrediction pb = batch.integrate(t0, t1, pos, vel, C, ba, bg);
    const ImuPrediction ps = stream.integrate(t0, t1, pos, vel, C, ba, bg);

    ASSERT_TRUE(pb.valid);
    ASSERT_TRUE(ps.valid);
    EXPECT_EQ(ps.num_samples, pb.num_samples);
    EXPECT_DOUBLE_EQ(ps.dt_total, pb.dt_total);
    EXPECT_TRUE(ps.position.isApprox(pb.position));
    EXPECT_TRUE(ps.velocity.isApprox(pb.velocity));
    EXPECT_TRUE(ps.attitude.isApprox(pb.attitude));
    EXPECT_TRUE(ps.Phi.isApprox(pb.Phi));
    EXPECT_TRUE(ps.Q_ins.isApprox(pb.Q_ins));
}

TEST(ImuStreamingIngest, IngestSampleRoutesImuKindOnly) {
    LogManager log;
    GnssConfig config;
    ImuAidingSource src(config, log);
    ASSERT_NE(src.integrator(), nullptr);
    EXPECT_FALSE(src.hasData());

    AidingSample imu;
    imu.kind = AidingSourceKind::Imu;
    imu.time = GnssTime{ 2313, 200.0 };
    imu.accel = Eigen::Vector3d(0.1, 0.2, 9.8);
    imu.gyro  = Eigen::Vector3d(0.01, 0.0, -0.01);
    src.ingestSample(imu);
    src.ingestSample(imu);
    EXPECT_TRUE(src.hasData());
    EXPECT_EQ(src.integrator()->sampleCount(), 2u);

    // A non-IMU sample is ignored by the IMU source.
    AidingSample baro;
    baro.kind = AidingSourceKind::Baro;
    baro.time = GnssTime{ 2313, 200.1 };
    src.ingestSample(baro);
    EXPECT_EQ(src.integrator()->sampleCount(), 2u);
}

// ============================================================================
// MeasurementModel::appendAidingRow (Step 6: measurement-row seam)
// ============================================================================

// Build a minimal finalized EpochMeasurement with `gnss_rows` GNSS rows over a
// state of size `n` (diagonal R_v, no R_full).
static EpochMeasurement makeGnssMeasurement(int gnss_rows, int n) {
    EpochMeasurement m;
    m.innovations = Eigen::VectorXd::Ones(gnss_rows);
    m.R_v = Eigen::VectorXd::Constant(gnss_rows, 0.01);
    m.H = Eigen::MatrixXd::Zero(gnss_rows, n);
    for (int r = 0; r < gnss_rows; ++r) {
        m.satellite_ids.push_back("G0" + std::to_string(r + 1));
        m.signal_types.push_back("L1C");
    }
    return m;
}

static AidingMeasurementRow makeRow(int n, int col, double innov, double var,
                                    const std::string& label) {
    AidingMeasurementRow row;
    row.jacobian = Eigen::VectorXd::Zero(n);
    row.jacobian(col) = 1.0;
    row.innovation = innov;
    row.variance = var;
    row.label = label;
    row.signal = "AID";
    return row;
}

TEST(AppendAidingRow, GrowsSystemAndSetsRow) {
    constexpr int N = 8;
    EpochMeasurement m = makeGnssMeasurement(/*gnss_rows=*/3, N);
    const AidingMeasurementRow row = makeRow(N, /*col=*/5, /*innov=*/-0.25, /*var=*/0.0025, "ZUPT_vx");

    MeasurementModel::appendAidingRow(m, row);

    ASSERT_EQ(m.innovations.size(), 4);
    ASSERT_EQ(m.R_v.size(), 4);
    ASSERT_EQ(m.H.rows(), 4);
    ASSERT_EQ(m.H.cols(), N);
    ASSERT_EQ(m.satellite_ids.size(), 4u);
    ASSERT_EQ(m.signal_types.size(), 4u);

    EXPECT_DOUBLE_EQ(m.innovations(3), -0.25);
    EXPECT_DOUBLE_EQ(m.R_v(3), 0.0025);
    EXPECT_DOUBLE_EQ(m.H(3, 5), 1.0);
    EXPECT_DOUBLE_EQ(m.H(3, 0), 0.0);
    EXPECT_EQ(m.satellite_ids[3], "ZUPT_vx");
    EXPECT_EQ(m.signal_types[3], "AID");
    // Existing GNSS rows untouched.
    EXPECT_DOUBLE_EQ(m.innovations(0), 1.0);
}

TEST(AppendAidingRow, ThrowsWhenRFullPopulated) {
    constexpr int N = 6;
    EpochMeasurement m = makeGnssMeasurement(2, N);
    m.R_full = Eigen::MatrixXd::Identity(2, 2);  // BSSD/DD active
    const AidingMeasurementRow row = makeRow(N, 4, 0.0, 0.0025, "ZUPT_vx");
    EXPECT_THROW(MeasurementModel::appendAidingRow(m, row), std::runtime_error);
}

TEST(AppendAidingRow, ThrowsOnJacobianSizeMismatch) {
    constexpr int N = 6;
    EpochMeasurement m = makeGnssMeasurement(2, N);
    const AidingMeasurementRow row = makeRow(/*n=*/N + 1, 4, 0.0, 0.0025, "ZUPT_vx");
    EXPECT_THROW(MeasurementModel::appendAidingRow(m, row), std::invalid_argument);
}

// ============================================================================
// ZuptAidingSource (Step 6: first row-contributing client)
// ============================================================================

TEST(AidingRegistry, ZuptIsBuiltAndRowContributing) {
    EXPECT_TRUE(AidingSourceRegistry::isAvailable(AidingSourceKind::Zupt));
    LogManager log;
    GnssConfig config;
    auto s = AidingSourceRegistry::create(AidingSourceKind::Zupt, config, log);
    ASSERT_NE(s, nullptr);
    EXPECT_EQ(s->kind(), AidingSourceKind::Zupt);
    EXPECT_EQ(s->asImu(), nullptr);
    EXPECT_FALSE(s->reparameterizesGnssRows());  // row source, not column
    EXPECT_FALSE(s->bridgesGaps());
    EXPECT_TRUE(s->hasData());
}

TEST(ZuptSource, BuildRowsPinsVelocityToZero) {
    constexpr std::size_t N = 9;
    constexpr std::size_t vel_idx = 3;  // [pos(0-2), vel(3-5), ...]
    Eigen::VectorXd state = Eigen::VectorXd::Zero(N);
    state(3) = 0.40;   // vx
    state(4) = -0.25;  // vy
    state(5) = 0.10;   // vz
    const double sigma = 0.05;

    const MeasurementRows rows =
        ZuptAidingSourceTestAccess::buildRows(state, vel_idx, N, sigma);

    ASSERT_EQ(rows.size(), 3u);
    const char* labels[3] = { "ZUPT_vx", "ZUPT_vy", "ZUPT_vz" };
    for (int axis = 0; axis < 3; ++axis) {
        const auto& r = rows[static_cast<std::size_t>(axis)];
        EXPECT_EQ(r.label, labels[axis]);
        EXPECT_EQ(r.signal, "AID");
        EXPECT_DOUBLE_EQ(r.variance, sigma * sigma);
        EXPECT_EQ(r.jacobian.size(), static_cast<Eigen::Index>(N));
        // Unit jacobian at the velocity component, zero elsewhere.
        EXPECT_DOUBLE_EQ(r.jacobian(vel_idx + axis), 1.0);
        EXPECT_DOUBLE_EQ(r.jacobian.sum(), 1.0);
        // Innovation drives the velocity component toward zero: z - h = -v.
        EXPECT_DOUBLE_EQ(r.innovation, -state(static_cast<Eigen::Index>(vel_idx + axis)));
    }
}

TEST(ZuptSource, NoVelocityStatesYieldsNoRows) {
    // A default-config StateManager has no velocity states, so ZUPT contributes
    // nothing (the guard path).
    LogManager log;
    GnssConfig config;
    StateManager sm(/*core_state_size=*/5, log, config);
    ASSERT_FALSE(sm.hasVelocity());

    ZuptAidingSource src(config, log);
    src.reserveStates(sm);

    const Eigen::VectorXd x = sm.getStateVector();
    const AidingEpochContext ctx{ x, GnssTime{ 2313, 100.0 }, GnssTime{ 2313, 101.0 }, false };
    EXPECT_TRUE(src.contributeMeasurement(ctx).empty());
}

}  // namespace
}  // namespace GnssProcessor
