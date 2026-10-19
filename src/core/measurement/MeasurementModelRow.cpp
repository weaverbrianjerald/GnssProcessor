/**
 * @file MeasurementModelRow.cpp
 * @brief The row of the single-receiver model: whether an observation enters the update
 *        (isUsableMeasurement), the product coverage checks, and the Jacobian row and
 *        predicted observable (MeasurementModel::buildJacobianRow).
 *
 * The usability rule and the coverage checks are free functions (measurement/RowRules.hpp):
 * they read configuration, the model's product tables and the SSR store, never a state.
 *
 * buildJacobianRow builds the observation equation of a code or phase row as a list of terms
 * (measurement/ObservationEquation.hpp) over this model's state vector
 * (measurement/UserParamView.hpp), sums it for the predicted observable and scatters the
 * partials into the row.  A Doppler row (velocity, clock rate, inter-system rate and the
 * tightly coupled IMU partials) has its own function, since those columns exist only in a
 * single-receiver configuration.  The coverage checks decide whether a .BIA product or the SSR
 * stream already corrects a signal.
 */

#include "measurement/MeasurementModel.hpp"
#include "measurement/RowRules.hpp"
#include "common/PppTypes.hpp"
#include "common/PppUtils.hpp"
#include "common/SignalBand.hpp"
#include "measurement/ObservationEquation.hpp"
#include "measurement/UserParamView.hpp"
#include "products/SsrCorrectionStore.hpp"
#include <cmath>

namespace GnssProcessor {

    // ===================================================================
    // Product coverage checks
    // ===================================================================

    bool ifCombHasFullBia(const RowProducts& products, const std::string& satellite,
                          const std::string& if_key) {
        const size_t us = if_key.find('_');
        if (us == std::string::npos || us == 0 || us + 1 >= if_key.size())
            return true; // not an IF key: treated as covered
        const std::string sig1 = if_key.substr(0, us);
        const std::string sig2 = if_key.substr(us + 1);
        return products.ppp_model.hasBiasEntry(satellite, sig1)
            && products.ppp_model.hasBiasEntry(satellite, sig2);
    }

    bool ssrCoversCodeSignal(const RowProducts& products, const std::string& satellite,
                             const std::string& signal, const GnssTime& obs_time) {
        // Matches the ssr_bias_term gate in buildJacobianRow.  The network producer
        // estimates satellite biases as states (importing them would make those
        // states unobservable), so it never imports SSR code biases and this
        // returns false.  In every other mode this is a presence check against the
        // live store: true iff a non-trivial ssr_bias_term will be applied to this
        // (sat, raw signal) row, in which case an estimated sat-bias state for the
        // same row would be redundant.  With an empty store (post-processing), false.
        const GnssConfig& config = products.config;
        if (config.getProcessingMode() == ProcessingMode::PPP_RTK_Network)
            return false;
        // With the staleness gate on, coverage uses the same limit as the apply gate
        // (max age × stale-hold factor).  Once the gate drops a bias the row is
        // uncorrected, so the sat-bias state and the no-BIA downweight apply again
        // (A5).
        if (config.enableSsrBiasMaxAge())
            return products.ssr.hasCodeBias(satellite, signal, obs_time,
                                            config.getSsrBiasMaxAge()
                                                * config.getSsrBiasStaleHoldFactor());
        return products.ssr.hasCodeBias(satellite, signal);
    }

    bool biaEntryApplies(const GnssConfig& config) {
        if (!config.getModelCorrections().bias_coverage_requires_apply) return true;
        // Producer exemption: the network's code rows are corrected by the
        // network-estimated shared code-bias states, not by the .BIA, so its
        // coverage semantics stay legacy under either flag value.
        if (config.getProcessingMode() == ProcessingMode::PPP_RTK_Network) return true;
        return config.getModelCorrections().enable_code_phase_bias;
    }

    bool biaCoversSignal(const RowProducts& products, const std::string& satellite,
                         const std::string& signal) {
        if (!products.ppp_model.hasBiasEntry(satellite, signal)) return false;
        return biaEntryApplies(products.config);
    }

    bool ssrCoversPhaseSignal(const RowProducts& products, const std::string& satellite,
                              const std::string& signal, const GnssTime& obs_time) {
        // Phase-channel counterpart of ssrCoversCodeSignal.  The network producer
        // estimates satellite phase biases as states and never imports them, so it
        // returns false.  In every other mode this is a presence check against the
        // live store, age-gated with the same limit as the apply site, so coverage
        // matches what buildJacobianRow applies to the row.
        const GnssConfig& config = products.config;
        if (config.getProcessingMode() == ProcessingMode::PPP_RTK_Network)
            return false;
        if (config.enableSsrBiasMaxAge())
            return products.ssr.getPhaseBias(satellite, signal, obs_time,
                                             config.getSsrBiasMaxAge()
                                                 * config.getSsrBiasStaleHoldFactor())
                .has_value();
        return products.ssr.getPhaseBias(satellite, signal).has_value();
    }

    bool phaseBiasCoveredForAr(const RowProducts& products, const std::string& satellite,
                               const std::string& signal, const GnssTime& obs_time) {
        const GnssConfig& config = products.config;
        const size_t us = signal.find('_');
        const bool is_if = (us != std::string::npos && us > 0 && us + 1 < signal.size());
        // Veto AR eligibility when the covering network phase bias declares
        // signal_integer=false (a product of the retired anchor-free ensemble datum carries
        // non-integer biases).
        // Only a bias present in the store (age-gated when the staleness gate is on) can
        // veto, and only with the toggle on, so an absent bias, a .BIA-only path and the
        // toggle off are byte-identical.  For an IF combination, both component biases
        // must be integer-grade (T3a).
        if (config.networkRequireSignalIntegerForAr()) {
            const bool   gate    = config.enableSsrBiasMaxAge();
            const double max_age = config.getSsrBiasMaxAge() * config.getSsrBiasStaleHoldFactor();
            auto nonInteger = [&](const std::string& sig) -> bool {
                auto pb = gate ? products.ssr.getPhaseBias(satellite, sig, obs_time, max_age)
                               : products.ssr.getPhaseBias(satellite, sig);
                return pb.has_value() && !pb->signal_integer;
            };
            if (is_if) {
                if (nonInteger(signal.substr(0, us)) || nonInteger(signal.substr(us + 1)))
                    return false;
            } else if (nonInteger(signal)) {
                return false;
            }
        }
        if (!config.getModelCorrections().bias_coverage_requires_apply) {
            // Legacy: raw .BIA presence, exactly the historical AR gate.
            return is_if ? ifCombHasFullBia(products, satellite, signal)
                         : products.ppp_model.hasBiasEntry(satellite, signal);
        }
        if (is_if) {
            const std::string sig1 = signal.substr(0, us);
            const std::string sig2 = signal.substr(us + 1);
            return (biaCoversSignal(products, satellite, sig1)
                    && biaCoversSignal(products, satellite, sig2))
                || (ssrCoversPhaseSignal(products, satellite, sig1, obs_time)
                    && ssrCoversPhaseSignal(products, satellite, sig2, obs_time));
        }
        return biaCoversSignal(products, satellite, signal)
            || ssrCoversPhaseSignal(products, satellite, signal, obs_time);
    }

    bool ifCombHasFullRxAtx(const RowProducts& products, const std::string& if_key,
                            System system) {
        const size_t us = if_key.find('_');
        if (us == std::string::npos || us == 0 || us + 1 >= if_key.size())
            return true;
        const std::string sig1 = if_key.substr(0, us);
        const std::string sig2 = if_key.substr(us + 1);
        return products.ppp_model.hasReceiverAtxEntry(sig1, system)
            && products.ppp_model.hasReceiverAtxEntry(sig2, system);
    }

    // ===================================================================
    // Measurement filtering
    // ===================================================================

    bool isUsableMeasurement(const GnssConfig& config,
        LogManager& log_manager,
        const PivotSignalMap& pivot_signals,
        const PppObservation& obs,
        const std::string& ref_type,
        const std::set<std::string>& excluded,
        const std::string& epoch_prefix) {
        if (ref_type.empty()) return false;

        if (obs.system == System::GLONASS && config.enableBetweenSatelliteSd()
            && !config.enableGlonassBssd()) {
            return false;
        }

        // When BSSD is active, exclude code/phase/Doppler measurements on
        // signals not tracked by the pivot satellite.  Such measurements
        // cannot be same-signal differenced and would be rejected by
        // BssdTransform's purity rule anyway; filtering here is an upstream
        // optimization.  Doppler is included: under BSSD it is SD-transformed
        // alongside code/phase, which cancels the receiver clock rate and the
        // ISBR in same-system pairs.
        if (!pivot_signals.empty() && ref_type.size() >= 2
            && (ref_type[0] == 'C' || ref_type[0] == 'L' || ref_type[0] == 'D')) {
            const std::string cons = PppUtils::getSystemString(obs.satellite);
            auto cit = pivot_signals.find(cons);
            if (cit != pivot_signals.end()
                && !cit->second.count(ref_type)) return false;
        }

        const MeasurementType mt = classifyMeasurement(ref_type);

        if (mt != MeasurementType::Code && mt != MeasurementType::Phase &&
            !(mt == MeasurementType::Doppler && config.estimateVelocity()))
            return false;

        const bool is_uncombined = config.isUncombinedEnabled();
        const bool has_underscore = (ref_type.find('_') != std::string::npos);

        if (mt == MeasurementType::Doppler) {
            if (has_underscore) return false;
        } else if (is_uncombined) {
            if (has_underscore) return false;
        } else {
            if (!has_underscore) return false;
        }

        if (!obs.observed.count(ref_type) || !obs.variances.count(ref_type) || !obs.modeled.count(ref_type))
            return false;

        const std::string excl_key = obs.satellite + "_" + ref_type;
        if (excluded.count(excl_key)) {
            log_manager.getLogger("measurement_model")->debug(
                "{}Excluding previously rejected measurement: {}", epoch_prefix, excl_key);
            return false;
        }

        return true;
    }

    // ===================================================================
    // Jacobian + innovation assembly
    // ===================================================================

    ObsInput MeasurementModel::obsInput() const {
        return ObsInput{rowProducts(), ppp_utils_, &state_initializer_.resolver(), nlwl_ref_band_,
                        clockRefs()};
    }

    bool MeasurementModel::buildJacobianRow(const PppObservation& obs,
        const std::string& ref_type,
        const Eigen::VectorXd& x,
        size_t state_size,
        Eigen::VectorXd& jacobian,
        double& predicted) const {

        jacobian = Eigen::VectorXd::Zero(state_size);

        if (classifyMeasurement(ref_type) == MeasurementType::Doppler)
            return buildDopplerRow(obs, ref_type, x, jacobian, predicted);

        // Code and phase rows: the term list of the observation equation over this model's
        // state vector, summed for the predicted observable and scattered into the row.
        const UserParamView view(state_manager_, x, state_size);
        const ObservationEquation eq =
            observationEquation(obsInput(), obs, ref_type, ParamLookup(view));
        if (eq.usable) {
            ObsTermValues terms;
            const bool decomposed = wantsRowDecomposition(obs);
            predicted = evaluate(eq, decomposed ? &terms : nullptr);
            scatterDense(eq, jacobian);
            if (decomposed) traceRowDecomposition(obs, ref_type, predicted, terms);
        }
        return eq.usable;
    }

    bool MeasurementModel::buildDopplerRow(const PppObservation& obs,
        const std::string& ref_type,
        const Eigen::VectorXd& x,
        Eigen::VectorXd& jacobian,
        double& predicted) const {

        if (!state_manager_.hasVelocity()) return false;

        jacobian.segment<3>(state_manager_.getVelocityIndex()) = -obs.los;

        const Eigen::Vector3d rx_vel = x.segment<3>(state_manager_.getVelocityIndex());
        predicted = obs.modeled.at(ref_type) - obs.los.dot(rx_vel);

        // Receiver clock-rate partial: Doppler observes the GPS-referenced
        // rate (c · dδt_r/dt) with unit coefficient.  Without this term the
        // receiver oscillator drift can only enter the velocity state, and
        // from there the position through the F[pos, vel] = dt coupling.
        if (state_manager_.hasClockRate()) {
            const size_t cr_idx = state_manager_.getClockRateIndex();
            jacobian(cr_idx) = 1.0;
            predicted += x(cr_idx);
        }

        // Inter-system clock-rate offset (ISBR) for non-GPS observations, the
        // rate counterpart of the ISB partial in the code/phase equation: the
        // total observed rate on a non-GPS Doppler is (global clock rate +
        // ISBR_sys).
        const std::string sys_name = StateInitializer::systemToIsbName(obs.system);
        if (!sys_name.empty()) {
            const size_t isbr_idx = state_manager_.getSystemTimeRateOffsetIndex(sys_name);
            if (isbr_idx < static_cast<size_t>(jacobian.size())) {
                jacobian(isbr_idx) = 1.0;
                predicted += x(isbr_idx);
            }
        }

        if (config_.tightlyCoupledImu() && state_manager_.hasImuBiases()) {
            const Eigen::Vector3d f_e = tc_C_b_e_ * tc_f_b_;
            Eigen::Matrix3d skew_fe;
            skew_fe <<       0.0, -f_e(2),  f_e(1),
                        f_e(2),      0.0, -f_e(0),
                       -f_e(1),  f_e(0),      0.0;

            const Eigen::Vector3d att_partial = -skew_fe.transpose() * obs.los;
            jacobian.segment<3>(state_manager_.getAttitudeIndex()) = att_partial;

            const Eigen::Vector3d ba_partial = -tc_C_b_e_.transpose() * obs.los;
            jacobian.segment<3>(state_manager_.getAccelBiasIndex()) = ba_partial;
        }

        return true;
    }

    bool MeasurementModel::wantsRowDecomposition(const PppObservation& obs) const {
        return !obs.satellite.empty()
            && log_manager_.getLogger("measurement_model")->should_log(spdlog::level::trace);
    }

    // PHASE-DECOMP diagnostic (trace, "measurement_model"): prints the
    // zero-difference predicted phase and the terms that carry a clock,
    // ionosphere, bias or ambiguity, so that a wandering float or a product
    // datum step can be attributed to one channel.  It is not a complete
    // decomposition: the a-priori model, the range correction, the zenith wet
    // delay, the troposphere gradients, the inter-system bias and the
    // phase-colour term are in predicted and are not printed.  ssr_pbias is
    // the applied network/SSR satellite phase-bias term (0 when the store has
    // no entry for this (sat, signal); the closed-loop datum attribution uses
    // this term); sat_pbias is the network producer's estimated
    // per-(sat,signal) phase-bias state, plus the receiver phase-bias split
    // state when that is armed (inert elsewhere); sat_clk is the producer's
    // residual satellite-clock state; amb is the float-ambiguity contribution
    // in metres (NL/WL basis included).
    //
    // CODE-DECOMP diagnostic: for any code signal, prints the zero-difference
    // predicted observable and the clock, ionosphere and bias terms in it, so
    // that a residual of hundreds of metres or kilometres can be attributed.
    // It is not a complete decomposition: the a-priori model, the range
    // correction, the zenith wet delay, the troposphere gradients, the
    // inter-system bias and the network satellite-clock term (on every network
    // producer code row) are in predicted and are not printed.  rx_clk is the
    // receiver-clock state (0 under BSSD, where the between-sat SD removes the
    // physical clock, so zd_innov here still contains it, equally on every
    // signal, and it cancels only after differencing); iono is the
    // per-sat slant-iono term (+alpha*I on code); ssr_cbias is the SSR code
    // bias matched by the exact RINEX obs code (0 if the provider's set lacks
    // this code); sat_bias is the estimated per-(sat,signal) state; bia_cbias
    // prints the receiver DCB/IFB state term.  A .BIA code bias
    // (enable_code_phase_bias) is applied in PppModel and is already part of
    // obs.modeled.
    void MeasurementModel::traceRowDecomposition(const PppObservation& obs,
        const std::string& ref_type,
        double predicted,
        const ObsTermValues& terms) const {

        auto lg = log_manager_.getLogger("measurement_model");
        const double observed_val =
            obs.observed.count(ref_type) ? obs.observed.at(ref_type) : 0.0;
        if (classifyMeasurement(ref_type) == MeasurementType::Phase) {
            lg->trace("PHASE-DECOMP {} {} tow={:.1f} observed={:.3f} predicted={:.3f} "
                      "zd_innov={:+.3f}m | rx_clk={:+.3f} phase_clk={:+.3f} "
                      "iono={:+.3f} ssr_pbias={:+.3f} sat_pbias={:+.3f} "
                      "sat_clk={:+.3f} amb={:+.3f}",
                      obs.satellite, ref_type, obs.time.tow, observed_val, predicted,
                      observed_val - predicted, terms[ObsEffect::ReceiverClock],
                      terms[ObsEffect::PhaseClock], terms[ObsEffect::Ionosphere],
                      terms[ObsEffect::SsrBias], terms[ObsEffect::SatellitePhaseBias],
                      terms[ObsEffect::SatelliteClock], terms[ObsEffect::Ambiguity]);
        } else {
            lg->trace("CODE-DECOMP {} {} tow={:.1f} observed={:.3f} predicted={:.3f} "
                      "zd_innov={:+.3f}m | rx_clk={:+.3f} iono={:+.3f} ssr_cbias={:+.3f} "
                      "sat_bias={:+.3f} bia_cbias={:+.3f}",
                      obs.satellite, ref_type, obs.time.tow, observed_val, predicted,
                      observed_val - predicted, terms[ObsEffect::ReceiverClock],
                      terms[ObsEffect::Ionosphere], terms[ObsEffect::SsrBias],
                      terms[ObsEffect::SatelliteCodeBias], terms[ObsEffect::ReceiverCodeBias]);
        }
    }

} // namespace GnssProcessor
