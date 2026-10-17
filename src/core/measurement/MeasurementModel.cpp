/**
 * @file MeasurementModel.cpp
 * @brief MeasurementModel's driver: construction and setters, the single-receiver
 *        computeMeasurementUpdate with its geometry gate, validation, and the
 *        BSSD forwarders.
 *
 * MeasurementModel turns one epoch's modeled observations plus the Kalman state
 * into an EpochMeasurement (innovations, H, R).  The class is split by concern
 * over six files:
 *  - MeasurementModel.cpp (this file): construction, computeMeasurementUpdate
 *    (the order of the epoch's steps and the row loop), hasValidGeometry,
 *    validateMeasurementUpdate, and the forwarders to PivotManager.
 *  - MeasurementModelStates.cpp: the state pre-pass ensureRequiredStates, one
 *    member per state category, called in a fixed order.
 *  - MeasurementModelRow.cpp: the row's Jacobian and predicted observable
 *    (buildJacobianRow, over the term list of measurement/ObservationEquation.hpp
 *    and the lookup of measurement/UserParamView.hpp), and the definitions of the
 *    usability rule and the product coverage checks (free functions,
 *    measurement/RowRules.hpp).
 *  - MeasurementModelRowReference.cpp: the row builder as it was before the term
 *    list, and the comparison with it (processing.obs_equation_check).  Temporary.
 *  - MeasurementModelVariance.cpp: the definition of rowVariance, the variance
 *    of one row (a free function, measurement/RowRules.hpp).
 *  - MeasurementModelDd.cpp: the baseline double-difference build, in stages.
 *  - MeasurementModelNetwork.cpp: the network producer's surface.
 *
 * Heavy sub-tasks are delegated:
 *  - StateInitializer:  first-epoch ISB, phase clock, and code bias seeding.
 *  - PivotManager:      pivot selection, handover, and state re-referencing.
 *  - BssdTransform:     between-satellite single-difference S-transformation.
 */

#include "model/PppModel.hpp"
#include "common/PppTypes.hpp"
#include "measurement/MeasurementModel.hpp"
#include "products/SsrCorrectionStore.hpp"
#include <stdexcept>
#include <cmath>

namespace GnssProcessor {

    // ===================================================================
    // Helpers (file-scope)
    // ===================================================================

    namespace {

    /// Human-readable mode label used in log messages.
    const char* modeLabel(const GnssConfig& cfg) {
        return cfg.getProcessingMode() == ProcessingMode::PPP ? "PPP" : "PPP-RTK";
    }

    } // anonymous namespace

    // ===================================================================
    // Construction
    // ===================================================================

    MeasurementModel::MeasurementModel(StateManager& state_manager, const PppModel& ppp_model,
        const GnssConfig& config, LogManager& log_manager)
        : state_manager_(state_manager), ppp_model_(ppp_model), config_(config), log_manager_(log_manager),
          state_initializer_(config, ppp_model, state_manager, log_manager),
          pivot_manager_(config, log_manager),
          bssd_transform_(config, state_manager, pivot_manager_, log_manager),
          dd_pivot_manager_(config, log_manager),
          ppp_utils_(log_manager) {
        // Select the measurement-differencing strategy once from config.
        differencing_ = MeasurementBuilderFactory::create(
            differencingModeFor(config_), log_manager_);
        log_manager_.getLogger("measurement_model")->info("MeasurementModel initialized with {} signal priority sets, mode={}",
            config_.getSignalPriorities().size(), modeLabel(config_));
    }

    void MeasurementModel::setGlonassSlots(const std::map<std::string, int>& slots) {
        // Seeds this station's frequency helper with the FCN slots from its RINEX
        // header, so the GLONASS frequency-dependent Jacobian terms (the iono
        // coefficient alpha = (f_L1/f)^2 and the per-slot carrier wavelength) use
        // the actual channel instead of the compiled-in default table.  PppModel
        // seeds its own helper the same way, once; the network driver calls this
        // for each station.
        ppp_utils_.setGlonassSlots(slots);
    }

    const SsrCorrectionStore& MeasurementModel::ssrStore() const {
        return ssr_store_override_ ? *ssr_store_override_
                                   : SsrCorrectionStore::getInstance();
    }

    // ===================================================================
    // IMU state setter
    // ===================================================================

    void MeasurementModel::setImuState(const Eigen::Matrix3d& C_b_e,
                                        const Eigen::Vector3d& f_b_corrected) {
        tc_C_b_e_ = C_b_e;
        tc_f_b_ = f_b_corrected;
    }

    void MeasurementModel::appendAidingRow(EpochMeasurement& update,
                                           const AidingMeasurementRow& row) {
        // Guard: a diagonal append after BSSD/DD populated R_full would silently
        // drop the SD cross-covariance (the KF falls back to R_v.asDiagonal()
        // only when R_full's dims don't match innovations).  Fail loudly instead.
        if (update.R_full.size() != 0) {
            throw std::runtime_error(
                "appendAidingRow: R_full is populated (BSSD/DD active); a diagonal "
                "aiding row would drop SD cross-covariance. Row-contributing aiding "
                "sources are restricted to undifferenced PPP / loosely-coupled GNSS.");
        }

        const Eigen::Index n    = update.innovations.size();
        const Eigen::Index ncol = (update.H.cols() > 0)
                                ? update.H.cols()
                                : static_cast<Eigen::Index>(row.jacobian.size());
        if (row.jacobian.size() != ncol) {
            throw std::invalid_argument(
                "appendAidingRow: jacobian size does not match the state dimension "
                "(H columns).");
        }

        update.innovations.conservativeResize(n + 1);
        update.innovations(n) = row.innovation;

        update.R_v.conservativeResize(n + 1);
        update.R_v(n) = row.variance;

        update.H.conservativeResize(n + 1, ncol);
        update.H.row(n) = row.jacobian.transpose();

        update.satellite_ids.push_back(row.label);
        update.signal_types.push_back(row.signal);
    }

    // ===================================================================
    // Row geometry gate
    // ===================================================================

    bool MeasurementModel::hasValidGeometry(const PppObservation& obs,
                                            const std::string& epoch_prefix) const {
        // Validate stored geometry
        if (obs.sat_pos.norm() < 1.9e7 || !obs.sat_pos.allFinite()) {
            log_manager_.getLogger("measurement_model")->warn(
                "{}Invalid stored satellite position for {} at TOW {:.3f}: norm={:.3e} m, mode={}",
                epoch_prefix, obs.satellite, obs.time.tow, obs.sat_pos.norm(), modeLabel(config_));
            return false;
        }
        if (obs.range < 1.5e7 || !std::isfinite(obs.range)) {
            log_manager_.getLogger("measurement_model")->warn(
                "{}Invalid stored range for {} at TOW {:.3f}: {:.3f} m, mode={}",
                epoch_prefix, obs.satellite, obs.time.tow, obs.range, modeLabel(config_));
            return false;
        }
        if (obs.elevation < config_.getElevationMaskDeg()) {
            log_manager_.getLogger("measurement_model")->debug(
                "{}Satellite {} below elevation mask at TOW {:.3f}, elevation={:.3f} deg, mode={}",
                epoch_prefix, obs.satellite, obs.time.tow, obs.elevation, modeLabel(config_));
            return false;
        }
        if (obs.los.norm() < 0.5 || !obs.los.allFinite()) {
            log_manager_.getLogger("measurement_model")->warn(
                "{}Invalid stored LOS for {} at TOW {:.3f}, mode={}",
                epoch_prefix, obs.satellite, obs.time.tow, modeLabel(config_));
            return false;
        }
        if (obs.mf_w <= 0.0 || !std::isfinite(obs.mf_w)) {
            log_manager_.getLogger("measurement_model")->warn(
                "{}Invalid wet mapping function for {} at TOW {:.3f}: {:.3f}, mode={}",
                epoch_prefix, obs.satellite, obs.time.tow, obs.mf_w, modeLabel(config_));
            return false;
        }
        return true;
    }

    // --- computeMeasurementUpdate: top-level orchestrator ---

    EpochMeasurement MeasurementModel::computeMeasurementUpdate(const std::vector<PppObservation>& observations,
        const Eigen::VectorXd& current_state,
        const std::set<std::string>& excluded_measurements) {
        EpochMeasurement update;
        update.time = observations.empty() ? GnssTime{ 0, 0.0 } : observations[0].time;
        update.observations = observations;
        update.excluded_measurements = excluded_measurements;
        const std::string epoch_prefix;  // %E in spdlog pattern carries the timestamp

        if (observations.empty()) {
            log_manager_.getLogger("measurement_model")->error("{}No observations provided for measurement update, mode={}",
                epoch_prefix, modeLabel(config_));
            return update;
        }

        // --- First-epoch initialization (delegated to StateInitializer) ---
        // Latch "ISB initialized" only when the seed succeeded.  If GPS (the ISB
        // reference) is absent this epoch, the call returns false; latching anyway
        // would freeze every non-GPS ISB at 0 for the whole session once GPS joins
        // later.  The seed is retried each epoch until GPS is present.
        if (!state_initializer_.isIsbInitialized()) {
            if (state_initializer_.initializeInterSystemBiases(observations, epoch_prefix))
                state_initializer_.markIsbInitialized();
        }
        if (!state_initializer_.isPhaseClockInitialized() && config_.estimatePhaseClock()) {
            state_initializer_.initializePhaseClockOffset(observations, epoch_prefix);
            state_initializer_.markPhaseClockInitialized();
        }

        // --- Differencing pre-pass: pivot handover + signal-coverage map ---
        // Delegated to the differencing strategy's preparePivots hook (no-op
        // for undifferenced PPP; BssdMeasurementBuilder runs the pivot handover
        // and builds the pivot signal-coverage map).  Invoked after the
        // bssd_pivot_signals_ reset and before ensureRequiredStates so that
        // ambiguity states for pivot-uncovered signals are never created.  The
        // handover's first-pass-vs-rebuild and forced-only semantics now live
        // in BssdMeasurementBuilder::preparePivots.
        bssd_pivot_signals_.clear();
        differencing_->preparePivots(*this, observations, epoch_prefix, update.time);

        // --- State pre-pass ---
        ensureRequiredStates(observations, epoch_prefix);

        // --- First-epoch code bias seeding ---
        if (!state_initializer_.isCodeBiasInitialized() && config_.estimateIfb()) {
            state_initializer_.initializeCodeBiases(observations, epoch_prefix);
            state_initializer_.markCodeBiasInitialized();
        }

        // --- Freeze pivot covariances (differencing strategy) ---
        differencing_->freezePivotCovariances(*this);

        // --- Validate state vector ---
        const Eigen::VectorXd x = state_manager_.getStateVector();
        if (x.size() < 5) {
            log_manager_.getLogger("measurement_model")->error("{}Invalid state vector size: {}, expected at least 5, mode={}",
                epoch_prefix, x.size(), modeLabel(config_));
            return update;
        }

        const size_t state_size = state_manager_.getStateSize();
        if (static_cast<size_t>(x.size()) != state_size) {
            log_manager_.getLogger("measurement_model")->error("{}State vector size {} does not match StateManager size {}, mode={}",
                epoch_prefix, x.size(), state_size, modeLabel(config_));
            return update;
        }

        // --- Count usable measurements ---
        size_t num_obs = 0;
        for (const auto& obs : observations)
            for (const auto& [ref_type, observed] : obs.observed)
                if (isUsableMeasurement(config_, log_manager_, bssd_pivot_signals_, obs, ref_type,
                                        excluded_measurements, epoch_prefix))
                    ++num_obs;

        if (num_obs == 0) {
            log_manager_.getLogger("measurement_model")->error("{}No valid observations found in residuals, mode={}",
                epoch_prefix, modeLabel(config_));
            return update;
        }

        // --- Allocate output ---
        update.innovations.resize(num_obs);
        update.R_v.resize(num_obs);
        update.H.resize(num_obs, state_size);
        update.H.setZero();
        update.satellite_ids.reserve(num_obs);
        update.signal_types.reserve(num_obs);

        // --- Assemble rows ---
        size_t row = 0;
        for (const auto& obs : observations) {
            if (!hasValidGeometry(obs, epoch_prefix)) continue;

            for (const auto& [ref_type, observed] : obs.observed) {
                if (!isUsableMeasurement(config_, log_manager_, bssd_pivot_signals_, obs, ref_type,
                                        excluded_measurements, epoch_prefix)) continue;

                try {
                    Eigen::VectorXd jacobian;
                    double predicted = 0.0;
                    if (!buildJacobianRow(obs, ref_type, x, state_size, jacobian, predicted))
                        continue;

                    update.innovations(row) = observed - predicted;
                    update.R_v(row) = rowVariance(rowProducts(), obs, ref_type);

                    update.H.row(row) = jacobian;
                    update.satellite_ids.push_back(obs.satellite);
                    update.signal_types.push_back(ref_type);

                    if (!std::isfinite(update.innovations(row)) || !std::isfinite(update.R_v(row))) {
                        update.innovations(row) = 0.0;
                        update.R_v(row) = 1e6;
                    }

                    log_manager_.getLogger("measurement_model")->trace(
                        "{}Meas {}/{}: obs={:.3f}, mod={:.3f}, pred={:.3f}, innov={:.3f}",
                        epoch_prefix, obs.satellite, ref_type,
                        observed, obs.modeled.at(ref_type), predicted, update.innovations(row));

                    row++;
                }
                catch (const std::exception&) {
                    continue;
                }
            }
        }

        // --- Finalize ---
        if (row == 0) {
            log_manager_.getLogger("measurement_model")->error("{}No valid measurement updates computed, mode={}",
                epoch_prefix, modeLabel(config_));
            return update;
        }
        if (row < num_obs) {
            log_manager_.getLogger("measurement_model")->warn("{}Processed {} observations out of {} expected, mode={}",
                epoch_prefix, row, num_obs, modeLabel(config_));
            update.innovations.conservativeResize(row);
            update.R_v.conservativeResize(row);
            update.H.conservativeResize(row, Eigen::NoChange);
            update.satellite_ids.resize(row);
            update.signal_types.resize(row);
        }
        update.bad_residual_count = 0;
        log_manager_.getLogger("measurement_model")->info("{}Computed measurement update: {} valid innovations, H ({}x{}), mode={}",
            epoch_prefix, row, update.H.rows(), update.H.cols(), modeLabel(config_));

        // --- Differencing S-transformation (strategy) ---
        differencing_->applyDifferencing(*this, update, epoch_prefix);

        return update;
    }

    // ===================================================================
    // Validation
    // ===================================================================

    bool MeasurementModel::validateMeasurementUpdate(const EpochMeasurement& measurement) const {
        const std::string epoch_prefix;  // %E in spdlog pattern carries the timestamp

        if (measurement.innovations.size() != measurement.R_v.size() ||
            measurement.innovations.size() != measurement.H.rows() ||
            measurement.H.cols() != state_manager_.getStateSize() ||
            measurement.innovations.size() != measurement.satellite_ids.size() ||
            measurement.innovations.size() != measurement.signal_types.size()) {
            log_manager_.getLogger("measurement_model")->error(
                "{}Measurement update dimension mismatch: innovations={}, R_v={}, H={}x{}, "
                "sat_ids={}, signal_types={}, state={}, mode={}",
                epoch_prefix,
                measurement.innovations.size(), measurement.R_v.size(),
                measurement.H.rows(), measurement.H.cols(),
                measurement.satellite_ids.size(), measurement.signal_types.size(),
                state_manager_.getStateSize(), modeLabel(config_));
            return false;
        }

        for (size_t i = 0; i < measurement.innovations.size(); ++i) {
            if (!std::isfinite(measurement.innovations(i)) || !std::isfinite(measurement.R_v(i))) {
                log_manager_.getLogger("measurement_model")->warn(
                    "{}Invalid measurement values for {} {}: innovation={:.3f}, variance={:.3f}, mode={}",
                    epoch_prefix,
                    measurement.satellite_ids[i], measurement.signal_types[i],
                    measurement.innovations(i), measurement.R_v(i), modeLabel(config_));
                return false;
            }
            if (measurement.R_v(i) <= 0.0) {
                log_manager_.getLogger("measurement_model")->warn(
                    "{}Non-positive variance for {} {}: variance={:.3f}, mode={}",
                    epoch_prefix,
                    measurement.satellite_ids[i], measurement.signal_types[i],
                    measurement.R_v(i), modeLabel(config_));
                return false;
            }
            if (std::abs(measurement.innovations(i)) > 100.0) {
                log_manager_.getLogger("measurement_model")->warn(
                    "{}Large innovation for {} {}: innovation={:.3f} m, mode={}",
                    epoch_prefix,
                    measurement.satellite_ids[i], measurement.signal_types[i],
                    measurement.innovations(i), modeLabel(config_));
            }
        }

        for (int i = 0; i < measurement.H.rows(); ++i) {
            if (!measurement.H.row(i).allFinite()) {
                log_manager_.getLogger("measurement_model")->warn(
                    "{}Non-finite Jacobian row for {} {}, mode={}",
                    epoch_prefix,
                    measurement.satellite_ids[i], measurement.signal_types[i], modeLabel(config_));
                return false;
            }
        }

        log_manager_.getLogger("measurement_model")->trace(
            "{}Validated measurement update: {} measurements, mode={}",
            epoch_prefix, measurement.innovations.size(), modeLabel(config_));
        return true;
    }

    // ===================================================================
    // BSSD delegation
    // ===================================================================

    void MeasurementModel::handleBssdPivotHandover(
        const std::vector<PppObservation>& observations,
        const std::string& epoch_prefix,
        bool forced_only)
    {
        pivot_manager_.handlePivotHandover(observations, state_manager_, epoch_prefix, forced_only);
    }

    // -------------------------------------------------------------------
    // The DD pivot handover (S-transform) lives in DdPivotManager:
    // @c dd_pivot_manager_.selectPivots applies the handover internally
    // when the chosen pivot differs from the sticky one.
    // -------------------------------------------------------------------

    void MeasurementModel::freezePivotAmbiguityCovariance()
    {
        pivot_manager_.freezePivotAmbiguityCovariance(state_manager_);
    }

    void MeasurementModel::freezePivotIonoAndBiasCovariance()
    {
        pivot_manager_.freezePivotIonoAndBiasCovariance(state_manager_);
    }

} // namespace GnssProcessor
