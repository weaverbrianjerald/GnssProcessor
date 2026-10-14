/**
 * @file MeasurementModelRow.cpp
 * @brief MeasurementModel's row: whether an observation enters the update
 *        (isUsableMeasurement), its Jacobian row and predicted observable
 *        (buildJacobianRow), and the product coverage checks they and the
 *        pre-pass use.
 *
 * buildJacobianRow is the observation equation of the single-receiver model:
 * the a-priori model value plus one term per estimated or applied quantity
 * (position, zenith wet delay, receiver and phase clock, troposphere
 * gradients, inter-system bias, slant ionosphere, receiver and satellite code
 * bias, the network's satellite phase bias and clock, the SSR satellite bias,
 * the ambiguity and the network's phase colour).  The coverage checks decide
 * whether a .BIA product or the SSR stream already corrects a signal.
 */

#include "measurement/MeasurementModel.hpp"
#include "common/PppTypes.hpp"
#include "common/SignalBand.hpp"
#include "products/SsrCorrectionStore.hpp"
#include "filter/StateKeySchema.hpp"
#include <cmath>

namespace GnssProcessor {

    // ===================================================================
    // Product coverage checks
    // ===================================================================

    bool MeasurementModel::ifCombHasFullBia(const std::string& satellite,
                                             const std::string& if_key) const {
        const size_t us = if_key.find('_');
        if (us == std::string::npos || us == 0 || us + 1 >= if_key.size())
            return true; // not an IF key: treated as covered
        const std::string sig1 = if_key.substr(0, us);
        const std::string sig2 = if_key.substr(us + 1);
        return ppp_model_.hasBiasEntry(satellite, sig1)
            && ppp_model_.hasBiasEntry(satellite, sig2);
    }

    bool MeasurementModel::ssrCoversCodeSignal(const std::string& satellite,
                                               const std::string& signal,
                                               const GnssTime& obs_time) const {
        // Matches the ssr_bias_term gate in buildJacobianRow.  The network producer
        // estimates satellite biases as states (importing them would make those
        // states unobservable), so it never imports SSR code biases and this
        // returns false.  In every other mode this is a presence check against the
        // live store: true iff a non-trivial ssr_bias_term will be applied to this
        // (sat, raw signal) row, in which case an estimated sat-bias state for the
        // same row would be redundant.  With an empty store (post-processing), false.
        if (config_.getProcessingMode() == ProcessingMode::PPP_RTK_Network)
            return false;
        // With the staleness gate on, coverage uses the same limit as the apply gate
        // (max age × stale-hold factor).  Once the gate drops a bias the row is
        // uncorrected, so the sat-bias state and the no-BIA downweight apply again
        // (A5).
        if (config_.enableSsrBiasMaxAge())
            return ssrStore().hasCodeBias(satellite, signal, obs_time,
                                          config_.getSsrBiasMaxAge()
                                              * config_.getSsrBiasStaleHoldFactor());
        return ssrStore().hasCodeBias(satellite, signal);
    }

    bool MeasurementModel::biaCoversSignal(const std::string& satellite,
                                           const std::string& signal) const {
        if (!ppp_model_.hasBiasEntry(satellite, signal)) return false;
        if (!config_.getModelCorrections().bias_coverage_requires_apply) return true;
        // Producer exemption: the network's code rows are corrected by the
        // network-estimated shared code-bias states, not by the .BIA, so its
        // coverage semantics stay legacy under either flag value.
        if (config_.getProcessingMode() == ProcessingMode::PPP_RTK_Network) return true;
        return config_.getModelCorrections().enable_code_phase_bias;
    }

    bool MeasurementModel::ssrCoversPhaseSignal(const std::string& satellite,
                                                const std::string& signal,
                                                const GnssTime& obs_time) const {
        // Phase-channel counterpart of ssrCoversCodeSignal.  The network producer
        // estimates satellite phase biases as states and never imports them, so it
        // returns false.  In every other mode this is a presence check against the
        // live store, age-gated with the same limit as the apply site, so coverage
        // matches what buildJacobianRow applies to the row.
        if (config_.getProcessingMode() == ProcessingMode::PPP_RTK_Network)
            return false;
        if (config_.enableSsrBiasMaxAge())
            return ssrStore().getPhaseBias(satellite, signal, obs_time,
                                           config_.getSsrBiasMaxAge()
                                               * config_.getSsrBiasStaleHoldFactor())
                .has_value();
        return ssrStore().getPhaseBias(satellite, signal).has_value();
    }

    bool MeasurementModel::phaseBiasCoveredForAr(const std::string& satellite,
                                                 const std::string& signal,
                                                 const GnssTime& obs_time) const {
        const size_t us = signal.find('_');
        const bool is_if = (us != std::string::npos && us > 0 && us + 1 < signal.size());
        // Veto AR eligibility when the covering network phase bias declares
        // signal_integer=false (the anchor-free ensemble datum emits non-integer biases).
        // Only a bias present in the store (age-gated when the staleness gate is on) can
        // veto, and only with the toggle on, so an absent bias, a .BIA-only path and the
        // toggle off are byte-identical.  For an IF combination, both component biases
        // must be integer-grade (T3a).
        if (config_.networkRequireSignalIntegerForAr()) {
            const bool   gate    = config_.enableSsrBiasMaxAge();
            const double max_age = config_.getSsrBiasMaxAge() * config_.getSsrBiasStaleHoldFactor();
            auto nonInteger = [&](const std::string& sig) -> bool {
                auto pb = gate ? ssrStore().getPhaseBias(satellite, sig, obs_time, max_age)
                               : ssrStore().getPhaseBias(satellite, sig);
                return pb.has_value() && !pb->signal_integer;
            };
            if (is_if) {
                if (nonInteger(signal.substr(0, us)) || nonInteger(signal.substr(us + 1)))
                    return false;
            } else if (nonInteger(signal)) {
                return false;
            }
        }
        if (!config_.getModelCorrections().bias_coverage_requires_apply) {
            // Legacy: raw .BIA presence, exactly the historical AR gate.
            return is_if ? ifCombHasFullBia(satellite, signal)
                         : ppp_model_.hasBiasEntry(satellite, signal);
        }
        if (is_if) {
            const std::string sig1 = signal.substr(0, us);
            const std::string sig2 = signal.substr(us + 1);
            return (biaCoversSignal(satellite, sig1) && biaCoversSignal(satellite, sig2))
                || (ssrCoversPhaseSignal(satellite, sig1, obs_time)
                    && ssrCoversPhaseSignal(satellite, sig2, obs_time));
        }
        return biaCoversSignal(satellite, signal)
            || ssrCoversPhaseSignal(satellite, signal, obs_time);
    }

    bool MeasurementModel::ifCombHasFullRxAtx(const std::string& if_key,
                                               System system) const {
        const size_t us = if_key.find('_');
        if (us == std::string::npos || us == 0 || us + 1 >= if_key.size())
            return true;
        const std::string sig1 = if_key.substr(0, us);
        const std::string sig2 = if_key.substr(us + 1);
        return ppp_model_.hasReceiverAtxEntry(sig1, system)
            && ppp_model_.hasReceiverAtxEntry(sig2, system);
    }

    // ===================================================================
    // Measurement filtering
    // ===================================================================

    bool MeasurementModel::isUsableMeasurement(const PppObservation& obs,
        const std::string& ref_type,
        const std::set<std::string>& excluded,
        const std::string& epoch_prefix) const {
        if (ref_type.empty()) return false;

        if (obs.system == System::GLONASS && config_.enableBetweenSatelliteSd()
            && !config_.enableGlonassBssd()) {
            return false;
        }

        // When BSSD is active, exclude code/phase/Doppler measurements on
        // signals not tracked by the pivot satellite.  Such measurements
        // cannot be same-signal differenced and would be rejected by
        // BssdTransform's purity rule anyway; filtering here is an upstream
        // optimization.  Doppler is included: under BSSD it is SD-transformed
        // alongside code/phase, which cancels the receiver clock rate and the
        // ISBR in same-system pairs.
        if (!bssd_pivot_signals_.empty() && ref_type.size() >= 2
            && (ref_type[0] == 'C' || ref_type[0] == 'L' || ref_type[0] == 'D')) {
            const std::string cons = PppUtils::getSystemString(obs.satellite);
            auto cit = bssd_pivot_signals_.find(cons);
            if (cit != bssd_pivot_signals_.end()
                && !cit->second.count(ref_type)) return false;
        }

        const MeasurementType mt = classifyMeasurement(ref_type);

        if (mt != MeasurementType::Code && mt != MeasurementType::Phase &&
            !(mt == MeasurementType::Doppler && config_.estimateVelocity()))
            return false;

        const bool is_uncombined = config_.isUncombinedEnabled();
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
            log_manager_.getLogger("measurement_model")->debug(
                "{}Excluding previously rejected measurement: {}", epoch_prefix, excl_key);
            return false;
        }

        return true;
    }

    // ===================================================================
    // Jacobian + innovation assembly
    // ===================================================================

    bool MeasurementModel::buildJacobianRow(const PppObservation& obs,
        const std::string& ref_type,
        const Eigen::VectorXd& x,
        size_t state_size,
        Eigen::VectorXd& jacobian,
        double& predicted) const {

        const MeasurementType mtype = classifyMeasurement(ref_type);
        const bool is_phase = (mtype == MeasurementType::Phase);
        const bool is_doppler = (mtype == MeasurementType::Doppler);
        const bool is_uncombined = config_.isUncombinedEnabled();
        const double mapping_wet = obs.mf_w;

        jacobian = Eigen::VectorXd::Zero(state_size);

        // Doppler measurements
        if (is_doppler) {
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
            // rate counterpart of the ISB partial in the code/phase branch: the
            // total observed rate on a non-GPS Doppler is (global clock rate +
            // ISBR_sys).
            const std::string sys_name = StateInitializer::systemToIsbName(obs.system);
            if (!sys_name.empty()) {
                const size_t isbr_idx = state_manager_.getSystemTimeRateOffsetIndex(sys_name);
                if (isbr_idx < state_size) {
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

        // Position partial: -LOS
        jacobian.segment<3>(state_manager_.getPositionIndex()) = -obs.los;

        // Troposphere wet delay
        jacobian(state_manager_.getTropoIndex()) = mapping_wet;

        // Receiver clock.  Under BSSD the clock cancels in SD and has no
        // state, so the Jacobian write is skipped and the clock contribution
        // is zero (BssdTransform would zero this column anyway).
        double rx_clk = 0.0;
        if (state_manager_.hasClock()) {
            jacobian(state_manager_.getClockIndex()) = 1.0;
            rx_clk = x(state_manager_.getClockIndex());
        }

        // Phase clock offset
        double phase_clk_value = 0.0;
        if (is_phase && state_manager_.hasPhaseClock()) {
            jacobian(state_manager_.getPhaseClockIndex()) = 1.0;
            phase_clk_value = x(state_manager_.getPhaseClockIndex());
        }

        // Tropospheric gradients
        double tropo_grad_term = 0.0;
        if (state_manager_.hasTropoGradients()) {
            const double el_rad = obs.elevation * Constants::PI / 180.0;
            const double az_rad = obs.azimuth * Constants::PI / 180.0;
            const double sin_el = std::sin(el_rad);
            const double tan_el = std::tan(el_rad);
            const double mf_grad = 1.0 / (sin_el * tan_el + 0.0032);
            const double cos_az = std::cos(az_rad);
            const double sin_az = std::sin(az_rad);

            jacobian(state_manager_.getTropoGradientNorthIndex()) = mf_grad * cos_az;
            jacobian(state_manager_.getTropoGradientEastIndex())  = mf_grad * sin_az;

            tropo_grad_term = mf_grad * (x(state_manager_.getTropoGradientNorthIndex()) * cos_az
                                        + x(state_manager_.getTropoGradientEastIndex()) * sin_az);
        }

        // Inter-system time offset
        const std::string sys_name = StateInitializer::systemToIsbName(obs.system);
        double time_offset_value = 0.0;
        if (!sys_name.empty()) {
            const size_t isb_idx = state_manager_.getSystemTimeOffsetIndex(sys_name);
            if (isb_idx < state_size) {
                jacobian(isb_idx) = 1.0;
                time_offset_value = x(isb_idx);
            }
        }

        // Per-satellite ionosphere
        double iono_term = 0.0;
        if (is_uncombined) {
            const int iono_idx = state_manager_.getIonosphereIndex(obs.satellite);
            if (iono_idx >= 0 && iono_idx < static_cast<int>(state_size)) {
                const double freq = ppp_utils_.getFrequency(ref_type, obs.system, obs.satellite);
                if (freq > 0.0) {
                    const double f_ref = Constants::GPS_L1_FREQ;
                    const double alpha = (f_ref * f_ref) / (freq * freq);
                    const double iono_coeff = (is_phase ? -1.0 : 1.0) * alpha;
                    jacobian(iono_idx) = iono_coeff;
                    iono_term = iono_coeff * x(iono_idx);
                }
            }
        }

        // Receiver bias: DCB / IFB (code only, uncombined)
        double code_bias_term = 0.0;
        if (config_.estimateIfb() && is_uncombined && !is_phase) {
            const auto bias = state_initializer_.resolveReceiverBias(ref_type, obs.system, obs.satellite);
            if (std::isfinite(bias.coefficient) && !bias.state_key.empty()) {
                const int cb_idx = state_manager_.getCodeBiasIndex(bias.state_key);
                if (cb_idx >= 0 && cb_idx < static_cast<int>(state_size)) {
                    jacobian(cb_idx) = bias.coefficient;
                    code_bias_term = bias.coefficient * x(cb_idx);
                }
            }
        }

        // Per-satellite signal bias (code-only: phase ambiguity absorbs sat phase bias)
        double sat_bias_term = 0.0;
        if (config_.estimateSatBiases()) {
            const bool is_if = (ref_type.find('_') != std::string::npos);
            const bool applicable = is_uncombined
                ? (!is_if && !is_phase)
                : (is_if && !is_phase);
            if (applicable) {
                const std::string sb_key = obs.satellite + "|" + ref_type;
                const int sb_idx = state_manager_.getSatBiasIndex(sb_key);
                if (sb_idx >= 0 && sb_idx < static_cast<int>(state_size)) {
                    jacobian(sb_idx) = 1.0;
                    sat_bias_term = x(sb_idx);
                }
            }
        }

        // Per-satellite signal phase bias (network producer only).  On the
        // single-receiver user side the satellite phase bias is imported as a
        // known SSR correction (below) or absorbed into the float ambiguity; the
        // network estimates it as an explicit per-(sat,signal) state on
        // undifferenced phase rows (Odijk Eq. 4; it carries the network pivot-
        // receiver ambiguity lump).  On a single row it is collinear with the
        // per-receiver float ambiguity; it is separable only across stations
        // (W4).  This code only reads an existing column, which
        // ensureNetworkSharedStates creates on the network producer; gated on
        // estimateSatPhaseBiases() (default off), so it is inert in every other
        // mode.
        double sat_phase_bias_term = 0.0;
        if (config_.estimateSatPhaseBiases() && is_phase && is_uncombined
            && ref_type.find('_') == std::string::npos) {
            const std::string spb_key = StateKeys::makeSatPhaseBiasKey(obs.satellite, ref_type);
            const int spb_idx = state_manager_.getSatPhaseBiasIndex(spb_key);
            if (spb_idx >= 0 && spb_idx < static_cast<int>(state_size)) {
                jacobian(spb_idx) = 1.0;
                sat_phase_bias_term = x(spb_idx);
                // The receiver phase-bias split state δ_{r,σ}, one per
                // (receiver, system, signal).  It appears on exactly the phase
                // rows whose (sat,σ) has an ungated shared phase-bias column
                // (spb_idx >= 0 here: the D2-graph vertices; a gated (sat,σ)
                // keeps the per-station lumping and gets no RPB).  It enters
                // with +1, the same sign as the ambiguity column it splits
                // (Odijk §4/§6: the phase row is …+δ_{r,σ}−δ^s_σ+z^s_r), so the
                // receiver fractional phase bias gets its own estimable state
                // instead of lumping into the emitted satellite phase bias.  It
                // is folded into sat_phase_bias_term (both are +1
                // phase-bias-family predicted terms), so the predicted assembly
                // below is byte-identical when the toggle is off.
                if (config_.networkPhaseDatumEq34()) {
                    const std::string rpb_key = StateKeys::makeReceiverPhaseBiasKey(
                        PppUtils::getSystemString(obs.satellite), ref_type);
                    const int rpb_idx = state_manager_.getReceiverPhaseBiasIndex(rpb_key);
                    if (rpb_idx >= 0 && rpb_idx < static_cast<int>(state_size)) {
                        jacobian(rpb_idx) = 1.0;
                        sat_phase_bias_term += x(rpb_idx);
                    }
                }
            }
        }

        // Per-satellite clock (network producer only).  The orbit is held fixed
        // at the product while the network estimates a per-satellite clock
        // correction Δδtˢ (metres) consistent with the chosen S-system datum.  The
        // observable carries the satellite clock as −c·δtˢ and obs.modeled has
        // already removed the a-priori product clock, so the residual-clock state
        // enters with a −1 Jacobian on both code and phase rows: one clock per
        // satellite, shared by all signals.  Like the bias states, this only reads
        // an existing column, which ensureNetworkSharedStates creates; gated on
        // estimateSatClocks() (default off), so it is inert in every other mode.
        // Single-station PPP would be rank-deficient here (rx-clock ↔ common
        // sat-clock), which is why it is network-only.
        double sat_clock_term = 0.0;
        if (config_.estimateSatClocks() && is_uncombined) {
            const std::string sc_key = StateKeys::makeSatClockKey(obs.satellite);
            const int sc_idx = state_manager_.getSatClockIndex(sc_key);
            if (sc_idx >= 0 && sc_idx < static_cast<int>(state_size)) {
                jacobian(sc_idx) = -1.0;
                sat_clock_term = -x(sc_idx);
            }
        }

        // SSR satellite biases (PPP-RTK): a known correction added to the
        // predicted observable (no state, no Jacobian).  getCodeBias /
        // getPhaseBias return the per-(sat, signal) value in metres; combined
        // iono-free keys such as "C1W_C5X" return 0/nullopt and contribute
        // nothing, since SSR biases are per raw signal.  No-op in
        // post-processing (empty store).
        //
        // Sign convention: the decoded bias is the satellite signal bias present
        // in the observation, so it is added to the clean model.  This convention
        // can only be confirmed empirically against the provider's stream: float
        // ambiguity fractions that cluster near 0.5 instead of 0 mean the
        // corresponding sign constant must be flipped (and the code rebuilt).
        //
        // Network producer: disabled.  The network estimates the satellite
        // code/phase biases as states (above), so importing the same quantity
        // here would double-count it and make those bias states unobservable.
        double ssr_bias_term = 0.0;
        if (config_.getProcessingMode() != ProcessingMode::PPP_RTK_Network) {
            constexpr double kSsrCodeBiasSign  = +1.0;
            constexpr double kSsrPhaseBiasSign = +1.0;
            const auto& ssr_store = ssrStore();
            const bool   gate    = config_.enableSsrBiasMaxAge();  // reject stale biases by obs-vs-record age (A5)
            // Stale hold (A5): the apply limit is hold_factor × max_age.  The last
            // record stays applied across staleness gaps below that limit, so a
            // provider interval near max_age produces no model step (and no
            // freshness-flip ambiguity reset; RealTimeEngine keys its flip detector
            // to the same limit).  A held bias is applied at full weight; the
            // network URA/staleness channel (consume_ura, ssr_max_age_s) is a
            // separate knob and does not cover this hold zone.
            const double max_age = config_.getSsrBiasMaxAge()
                                 * config_.getSsrBiasStaleHoldFactor();
            if (is_phase) {
                auto pb = gate ? ssr_store.getPhaseBias(obs.satellite, ref_type, obs.time, max_age)
                               : ssr_store.getPhaseBias(obs.satellite, ref_type);
                if (pb) ssr_bias_term = kSsrPhaseBiasSign * pb->bias_m;
            } else {
                ssr_bias_term = kSsrCodeBiasSign
                    * (gate ? ssr_store.getCodeBias(obs.satellite, ref_type, obs.time, max_age)
                            : ssr_store.getCodeBias(obs.satellite, ref_type));
            }
        }

        // Assemble predicted observable
        const double modeled = obs.modeled.at(ref_type);
        const double zwd = x(state_manager_.getTropoIndex());
        const double tropo_wet_term = mapping_wet * zwd;

        const Eigen::Vector3d pos_current = x.segment<3>(state_manager_.getPositionIndex());
        const double range_correction = -obs.los.dot(pos_current - obs.rx_pos_linearization);

        predicted = modeled + range_correction + rx_clk + phase_clk_value
            + tropo_wet_term + tropo_grad_term + time_offset_value
            + iono_term + code_bias_term + sat_bias_term + sat_phase_bias_term
            + sat_clock_term + ssr_bias_term;

        // Ambiguity (phase only)
        double amb_term = 0.0;
        if (is_phase) {
            const bool nlwl = (config_.getStateAmbiguityBasis()
                               == StateAmbiguityBasis::NlWl);
            if (!nlwl) {
                const std::string amb_key = obs.satellite + "_" + ref_type;
                const int amb_idx = state_manager_.getAmbiguityIndex(amb_key);
                if (amb_idx < 0) return false;
                jacobian(amb_idx) = 1.0;
                amb_term = x(amb_idx);
                predicted += amb_term;
                // The line of sight's phase-colour state (network.phase_colour_state,
                // card LGM): +1 on every phase row of the satellite, both bands, and on
                // no code row.  Its linearization point is 0 (never written back).
                if (config_.getNetworkConfig().phase_colour_state) {
                    const int pc_idx = state_manager_.getPhaseColourIndex(obs.satellite);
                    if (pc_idx >= 0 && pc_idx < static_cast<int>(state_size)) {
                        jacobian(pc_idx) = 1.0;
                        predicted += x(pc_idx);
                    }
                }
            } else {
                // NlWl basis: state holds NL_ref (cycles, the L_ref carrier
                // ambiguity) and WL_(ref, b) = N_a - N_b in canonical
                // ascending-band form for each non-ref band b.  The phase
                // observation φ_b in meters is
                //     φ_b = ρ + λ_b · N_b
                // where N_b is reconstructed from state as
                //     N_b = NL_ref                                  (b == ref)
                //     N_b = NL_ref - sign · WL_<a>_<z>              (b != ref)
                // with sign = +1 when ref_band is the lower of the pair
                // and sign = -1 when ref_band is the higher one.
                if (ref_type.size() < 2) return false;
                auto rb_it = nlwl_ref_band_.find(obs.satellite);
                if (rb_it == nlwl_ref_band_.end()) return false;
                const char ref_band = rb_it->second;
                const char band = ref_type[1];
                const double lambda_b = ppp_utils_.getWavelength(
                    obs.system, ref_type, obs.satellite);
                if (!std::isfinite(lambda_b) || lambda_b <= 0.0) return false;

                const std::string nl_key = StateKeys::makeNlAmbiguityKey(
                    obs.satellite, ref_band);
                const int nl_idx = state_manager_.getAmbiguityIndex(nl_key);
                if (nl_idx < 0) return false;

                jacobian(nl_idx) = lambda_b;
                if (band == ref_band) {
                    amb_term = lambda_b * x(nl_idx);
                } else {
                    const std::string wl_key = StateKeys::makeWlAmbiguityKey(
                        obs.satellite, ref_band, band);
                    const int wl_idx = state_manager_.getAmbiguityIndex(wl_key);
                    if (wl_idx < 0) return false;
                    const double wl_sign = (ref_band < band) ? +1.0 : -1.0;
                    jacobian(wl_idx) = -lambda_b * wl_sign;
                    amb_term = lambda_b * (x(nl_idx) - wl_sign * x(wl_idx));
                }
                predicted += amb_term;
            }
        }

        // PHASE-DECOMP diagnostic (trace, "measurement_model"): the phase-row
        // counterpart of CODE-DECOMP below.  It prints the zero-difference
        // predicted phase and the terms that carry a clock, ionosphere, bias or
        // ambiguity, so that a wandering float or a product datum step can be
        // attributed to one channel.  It is not a complete decomposition: the
        // a-priori model, the range correction, the zenith wet delay, the
        // troposphere gradients, the inter-system bias and the phase-colour term
        // are in predicted and are not printed.  ssr_pbias is the applied
        // network/SSR satellite phase-bias term (0 when the store has no entry
        // for this (sat, signal); the closed-loop datum attribution uses this
        // term); sat_pbias is the network producer's estimated per-(sat,signal)
        // phase-bias state, plus the receiver phase-bias split state when that
        // is armed (inert elsewhere); sat_clk is the producer's residual
        // satellite-clock state; amb is the float-ambiguity contribution in
        // metres (NL/WL basis included).
        if (is_phase && !obs.satellite.empty()) {
            if (auto lg = log_manager_.getLogger("measurement_model");
                lg->should_log(spdlog::level::trace)) {
                const double observed_val =
                    obs.observed.count(ref_type) ? obs.observed.at(ref_type) : 0.0;
                lg->trace("PHASE-DECOMP {} {} tow={:.1f} observed={:.3f} predicted={:.3f} "
                          "zd_innov={:+.3f}m | rx_clk={:+.3f} phase_clk={:+.3f} "
                          "iono={:+.3f} ssr_pbias={:+.3f} sat_pbias={:+.3f} "
                          "sat_clk={:+.3f} amb={:+.3f}",
                          obs.satellite, ref_type, obs.time.tow, observed_val, predicted,
                          observed_val - predicted, rx_clk, phase_clk_value, iono_term,
                          ssr_bias_term, sat_phase_bias_term, sat_clock_term, amb_term);
            }
        }

        // CODE-DECOMP diagnostic (trace, "measurement_model"): for any code
        // signal, prints the zero-difference predicted observable and the clock,
        // ionosphere and bias terms in it, so that a residual of hundreds of
        // metres or kilometres can be attributed.  It is not a complete
        // decomposition: the a-priori model, the range correction, the zenith
        // wet delay, the troposphere gradients, the inter-system bias and the
        // network satellite-clock term (on every network producer code row) are
        // in predicted and are not printed.  rx_clk is the receiver-clock state
        // (0 under BSSD, where the between-sat SD removes the physical clock, so
        // zd_innov here still contains it, equally on every signal, and it cancels only after
        // differencing); iono is the per-sat slant-iono term (+alpha*I on code);
        // ssr_cbias is the SSR code bias matched by the exact RINEX obs code (0
        // if the provider's set lacks this code); sat_bias is the estimated
        // per-(sat,signal) state; bia_cbias prints code_bias_term, the receiver
        // DCB/IFB state term.  A .BIA code bias (enable_code_phase_bias) is
        // applied in PppModel and is already part of obs.modeled.
        if (!is_phase && !obs.satellite.empty()) {
            if (auto lg = log_manager_.getLogger("measurement_model");
                lg->should_log(spdlog::level::trace)) {
                const double observed_val =
                    obs.observed.count(ref_type) ? obs.observed.at(ref_type) : 0.0;
                lg->trace("CODE-DECOMP {} {} tow={:.1f} observed={:.3f} predicted={:.3f} "
                          "zd_innov={:+.3f}m | rx_clk={:+.3f} iono={:+.3f} ssr_cbias={:+.3f} "
                          "sat_bias={:+.3f} bia_cbias={:+.3f}",
                          obs.satellite, ref_type, obs.time.tow, observed_val, predicted,
                          observed_val - predicted, rx_clk, iono_term, ssr_bias_term,
                          sat_bias_term, code_bias_term);
            }
        }

        return true;
    }

} // namespace GnssProcessor
