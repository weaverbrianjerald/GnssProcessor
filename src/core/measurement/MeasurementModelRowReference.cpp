/**
 * @file MeasurementModelRowReference.cpp
 * @brief The code and phase row builder as it was written before the observation equation
 *        became a term list, kept as the reference the term list is checked against
 *        (processing.obs_equation_check).
 *
 * buildJacobianRowReference is the row builder unchanged apart from its two decomposition
 * traces, which the live builder keeps: it writes the Jacobian row and the predicted
 * observable term by term, in the order of the sum.  checkRowAgainstReference runs it beside
 * the term-list builder and compares the two bit for bit.  Neither is called with the check
 * off.
 *
 * Temporary: this file is deleted when the term-list builder has been shown equal on the
 * field and simulated golden runs and the network no longer builds its rows from a
 * StateManager.
 */

#include "measurement/MeasurementModel.hpp"
#include "measurement/ObservationEquationCheck.hpp"
#include "common/PppTypes.hpp"
#include "common/PppUtils.hpp"
#include "common/SignalBand.hpp"
#include "products/SsrCorrectionStore.hpp"
#include "filter/StateKeySchema.hpp"
#include <cmath>

namespace GnssProcessor {

    bool MeasurementModel::buildJacobianRowReference(const PppObservation& obs,
        const std::string& ref_type,
        const Eigen::VectorXd& x,
        size_t state_size,
        Eigen::VectorXd& jacobian,
        double& predicted) const {

        const MeasurementType mtype = classifyMeasurement(ref_type);
        const bool is_phase = (mtype == MeasurementType::Phase);
        const bool is_uncombined = config_.isUncombinedEnabled();
        const double mapping_wet = obs.mf_w;

        jacobian = Eigen::VectorXd::Zero(state_size);


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
            const auto bias = state_initializer_.resolveReceiverBias(ref_type, obs.system, obs.satellite, clockRefs());
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
                // The line of sight's phase-colour state (network.phase_colour_state):
                // +1 on every phase row of the satellite, both bands, and on
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

        return true;
    }

    void MeasurementModel::checkRowAgainstReference(const PppObservation& obs,
        const std::string& ref_type,
        const Eigen::VectorXd& x,
        size_t state_size,
        const ObservationEquation& eq,
        const Eigen::VectorXd& jacobian,
        double predicted) const {

        obsEquationCheckNoteFrequencies(log_manager_);

        Eigen::VectorXd ref_jacobian;
        double ref_predicted = 0.0;
        const bool ref_usable = buildJacobianRowReference(obs, ref_type, x, state_size,
                                                          ref_jacobian, ref_predicted);
        const bool jacobian_equal  = eq.usable && ref_usable && sameBits(jacobian, ref_jacobian);
        const bool predicted_equal = eq.usable && ref_usable && sameBits(predicted, ref_predicted);
        obsEquationCheckRecord(eq.usable, ref_usable, eq.fault, jacobian_equal, predicted_equal);

        // The first few disagreements are named in the log; the counters carry the rest.
        constexpr long long kLogged = 20;
        const ObsEquationCheckCounts c = obsEquationCheckCounts();
        const bool usable_differs = eq.usable != ref_usable;
        const bool differs = usable_differs
            || (eq.usable && (!jacobian_equal || !predicted_equal));
        if (!differs || c.usable_mismatch + c.jacobian_mismatch + c.predicted_mismatch > kLogged)
            return;
        auto lg = log_manager_.getLogger("measurement_model");
        if (usable_differs) {
            lg->warn("obs_equation_check {} {} tow={:.1f}: term list {} the row, reference {} it (fault {})",
                     obs.satellite, ref_type, obs.time.tow, eq.usable ? "accepted" : "refused",
                     ref_usable ? "accepted" : "refused", static_cast<int>(eq.fault));
            return;
        }
        Eigen::Index first = -1;
        for (Eigen::Index i = 0; i < jacobian.size() && first < 0; ++i)
            if (!sameBits(jacobian(i), ref_jacobian(i))) first = i;
        lg->warn("obs_equation_check {} {} tow={:.1f}: predicted {:.17g} against {:.17g}; first "
                 "Jacobian difference at column {} ({:.17g} against {:.17g})",
                 obs.satellite, ref_type, obs.time.tow, predicted, ref_predicted, first,
                 first >= 0 ? jacobian(first) : 0.0, first >= 0 ? ref_jacobian(first) : 0.0);
    }

} // namespace GnssProcessor
