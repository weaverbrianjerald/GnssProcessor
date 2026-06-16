/**
 * @file ImuAidingSource.cpp
 * @brief IMU first-client adapter — wraps ImuIntegrator behind IAidingSource.
 *
 * Implements the state descriptor and the predict-time contribution (the
 * composable generalization of KalmanFilter::predictImu).  These are not yet
 * wired into the live epoch loop — predictImu / the existing TC branch remain
 * the live paths until Step 3 — so the IMU stays numerically inert.
 *
 * @see docs/aiding_source_abstraction_design.md
 */

#include "aiding/ImuAidingSource.hpp"

#include "filter/StateManager.hpp"
#include "filter/KalmanFilter.hpp"
#include "measurement/MeasurementModel.hpp"

#include <limits>

namespace GnssProcessor {

namespace {
/// Sentinel for an absent ins_map slot (matches predictImu's skip convention).
constexpr std::size_t kNoIndex = std::numeric_limits<std::size_t>::max();
} // namespace

ImuAidingSource::ImuAidingSource(const GnssConfig& config, LogManager& log_manager)
    : config_(config),
      log_manager_(log_manager),
      tightly_coupled_(config.tightlyCoupledImu()) {
    // Wrap (do not replace) ImuIntegrator.  Construction + data load mirror the
    // legacy Application::run wiring; a load failure leaves hasData()==false,
    // the existing graceful GNSS-only fallback.
    integrator_ = std::make_unique<ImuIntegrator>(config_, log_manager_);
    const auto& path = config_.getImuConfig().imu_file_path;
    if (!path.empty() && !integrator_->loadData(path)) {
        log_manager_.getLogger("aiding")->warn(
            "ImuAidingSource: failed to load IMU data from '{}'; "
            "IMU aiding inert (GNSS-only fallback).", path.string());
    }
}

std::optional<StateBlockSpec> ImuAidingSource::reserveStates(StateManager& sm) {
    // Stash for live per-epoch index resolution (indices shift under prunes —
    // never cache the returned start_index).
    state_manager_ = &sm;

    if (!sm.hasImuBiases())
        return std::nullopt;  // IMU error states not allocated in this config.

    // Describe the existing contiguous attitude/accel-bias/gyro-bias block
    // (StateManager.cpp lays them out as attitude_idx_, +3, +6).  Allocate
    // nothing — bit-for-bit identical state layout.
    StateBlockSpec spec;
    spec.owner = AidingSourceKind::Imu;
    spec.key_prefix.clear();  // tier-1: core offsets, no keyed-tail block.
    spec.start_index = sm.getAttitudeIndex();
    spec.size = 9;
    spec.layout = { "dpsi_x", "dpsi_y", "dpsi_z",
                    "dba_x",  "dba_y",  "dba_z",
                    "dbg_x",  "dbg_y",  "dbg_z" };
    return spec;
}

std::vector<std::size_t> ImuAidingSource::buildInsMap(const StateManager& sm) {
    // INS error-state order: pos(0-2), vel(3-5), att(6-8), ba(9-11), bg(12-14).
    // Replicates KalmanFilter::predictImu's ins_map construction verbatim.
    std::vector<std::size_t> m(15, kNoIndex);
    const std::size_t p = sm.getPositionIndex();
    m[0] = p; m[1] = p + 1; m[2] = p + 2;
    if (sm.hasVelocity()) {
        const std::size_t v = sm.getVelocityIndex();
        m[3] = v; m[4] = v + 1; m[5] = v + 2;
    }
    if (sm.hasImuBiases()) {
        const std::size_t a  = sm.getAttitudeIndex();
        const std::size_t ba = sm.getAccelBiasIndex();
        const std::size_t bg = sm.getGyroBiasIndex();
        m[6]  = a;  m[7]  = a + 1;  m[8]  = a + 2;
        m[9]  = ba; m[10] = ba + 1; m[11] = ba + 2;
        m[12] = bg; m[13] = bg + 1; m[14] = bg + 2;
    }
    return m;
}

PredictContribution ImuAidingSource::mapPredictionToContribution(
    const ImuPrediction& pred, const std::vector<std::size_t>& ins_map) const {
    PredictContribution c;
    c.source = AidingSourceKind::Imu;
    c.valid = pred.valid;
    if (!pred.valid || ins_map.size() != 15)
        { c.valid = false; return c; }

    c.dt_total = pred.dt_total;
    c.num_samples = pred.num_samples;
    c.requires_naive_relinearization = tightly_coupled_;
    c.indices = ins_map;       // size-15, sentinels preserved (skipped downstream)
    c.Phi = pred.Phi;          // 15x15 INS error-state transition
    c.Q   = pred.Q_ins;        // 15x15 INS process noise

    // Nominal write-backs replicate predictImu's x updates exactly:
    //   pos <- pred.position (always), vel <- pred.velocity (if present),
    //   attitude-error <- 0 (if present; the correction folds into C_b_e_).
    std::vector<std::size_t> nidx;
    std::vector<double> nval;
    nidx.reserve(9);
    nval.reserve(9);
    for (int k = 0; k < 3; ++k) { nidx.push_back(ins_map[static_cast<std::size_t>(k)]);     nval.push_back(pred.position[k]); }
    if (ins_map[3] != kNoIndex)
        for (int k = 0; k < 3; ++k) { nidx.push_back(ins_map[static_cast<std::size_t>(3 + k)]); nval.push_back(pred.velocity[k]); }
    if (ins_map[6] != kNoIndex)
        for (int k = 0; k < 3; ++k) { nidx.push_back(ins_map[static_cast<std::size_t>(6 + k)]); nval.push_back(0.0); }

    c.nominal_indices = std::move(nidx);
    c.nominal_values = Eigen::Map<const Eigen::VectorXd>(
        nval.data(), static_cast<Eigen::Index>(nval.size()));
    return c;
}

PredictContribution ImuAidingSource::contributePredict(const AidingEpochContext& ctx) {
    // First epoch forces the GNSS-only predict (no interval to integrate);
    // also bail if not yet wired or no data.
    if (ctx.first_epoch || !state_manager_ || !filter_ || !hasData())
        return PredictContribution{};  // .valid == false

    const StateManager& sm = *state_manager_;
    const Eigen::VectorXd& x = ctx.state;

    const Eigen::Vector3d pos =
        x.segment<3>(static_cast<Eigen::Index>(sm.getPositionIndex()));
    Eigen::Vector3d vel = Eigen::Vector3d::Zero();
    if (sm.hasVelocity())
        vel = x.segment<3>(static_cast<Eigen::Index>(sm.getVelocityIndex()));

    Eigen::Vector3d accel_bias = Eigen::Vector3d::Zero();
    Eigen::Vector3d gyro_bias  = Eigen::Vector3d::Zero();
    if (sm.hasImuBiases()) {
        accel_bias = x.segment<3>(static_cast<Eigen::Index>(sm.getAccelBiasIndex()));
        gyro_bias  = x.segment<3>(static_cast<Eigen::Index>(sm.getGyroBiasIndex()));
    }

    // Current nominal attitude read THROUGH the owner (the single C_b_e_ owner).
    const Eigen::Matrix3d C_b_e = filter_->getAttitude();

    const ImuPrediction pred = integrator_->integrate(
        ctx.t_prev, ctx.t_now, pos, vel, C_b_e, accel_bias, gyro_bias);
    if (!pred.valid)
        return PredictContribution{};  // caller falls back to time-only predict

    // Write the new nominal attitude back through the owner (predictImu sets
    // C_b_e_ = pred.attitude mid-body; here the source does it via the handle).
    filter_->setAttitude(pred.attitude);

    return mapPredictionToContribution(pred, buildInsMap(sm));
}

void ImuAidingSource::ingestSample(const AidingSample& sample) {
    if (sample.kind != AidingSourceKind::Imu || !integrator_) return;
    ImuSample s;
    s.time  = sample.time;
    s.accel = sample.accel;
    s.gyro  = sample.gyro;
    integrator_->addSample(s);
}

void ImuAidingSource::applyTcJacobianInputs(MeasurementModel& model) const {
    if (!tightly_coupled_ || !hasData() || !state_manager_ || !filter_)
        return;

    // Relocated verbatim from Application::run's two TC sites: a
    // gravity-subtracted body specific force (f_b ~ C^T*(-g) at rest; the live
    // state is a good approximation over the GNSS epoch interval) plus the
    // current attitude, fed to the unchanged setImuState partials.
    const StateManager& sm = *state_manager_;
    const Eigen::VectorXd x = sm.getStateVector();
    Eigen::Vector3d accel_bias = Eigen::Vector3d::Zero();
    if (sm.hasImuBiases())
        accel_bias = x.segment<3>(static_cast<Eigen::Index>(sm.getAccelBiasIndex()));
    const Eigen::Vector3d g_e = ImuIntegrator::computeGravityEcef(
        x.segment<3>(static_cast<Eigen::Index>(sm.getPositionIndex())));
    const Eigen::Matrix3d C = filter_->getAttitude();
    const Eigen::Vector3d f_b = C.transpose() * (-g_e) - accel_bias;
    model.setImuState(C, f_b);
}

} // namespace GnssProcessor
