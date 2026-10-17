/**
 * @file MeasurementModelVariance.cpp
 * @brief The variance of one row (measurement/RowRules.hpp): the stochastic-model
 *        variance plus the consumer's network-correction terms.
 *
 * The composition, in the order rowVariance applies it:
 *  1. the stochastic-model variance (obs.variances, from PppModel);
 *  2. the combined-correction decision (closed_loop.consume_combined_sigma):
 *     with a fresh network correction carrying a combined sigma, that sigma
 *     (age-inflated) replaces steps 3 and 4;
 *  3. the network URA and the ZOH clock-staleness term, with the held-clock
 *     fallback (consume_ura, ssr_clock_stale_hold_factor);
 *  4. the network phase-bias sigma on phase rows (consume_phase_bias_sigma);
 *  5. the combined-correction variance when step 2 chose it;
 *  6. the no-BIA code downweight, a factor on the sum of 1 to 5.
 * None of steps 2 to 5 applies to the network producer's own rows.
 */

#include "measurement/RowRules.hpp"
#include "config/GnssConfig.hpp"
#include "products/SsrCorrectionStore.hpp"
#include "math/TimeUtils.hpp"
#include <cmath>

namespace GnssProcessor {

    double rowVariance(const RowProducts& products, const PppObservation& obs,
                       const std::string& ref_type) {
        const GnssConfig& config = products.config;
        double variance = obs.variances.at(ref_type);

        // When the network product carries a per-key combined-correction σ
        // and closed_loop.consume_combined_sigma is on, that σ (age-inflated)
        // goes into this row's variance in place of the individual URA +
        // phase-bias-σ RSS below.  For a fresh, reconverging satellite the
        // combined σ is at the mm level where the individual clock/bias σ
        // are at the cm level, so the row keeps its weight instead of being
        // deweighted 10–100× (Zhang 2021 / Psychas 2024b).  Using both would
        // double-count the shared clock variance and bring that deweighting
        // back, so an active combined row suppresses the two blocks below.
        //
        // The fallback is per key: an absent or too-stale token fails the
        // freshness gate, combined_active stays false, and the URA path
        // (which owns staleness) handles the row, so mixed streams stay
        // safe.  The product's rho is not applied: the measurement R is
        // diagonal, with no off-diagonal code/phase slot.  With the knob off,
        // on rows without network corrections and on producer rows, R is
        // byte-identical.
        bool   combined_active = false;
        double combined_var    = 0.0;
        if (config.networkConsumeCombinedSigma()
            && config.getProcessingMode() != ProcessingMode::PPP_RTK_Network
            && ref_type.size() >= 2) {
            const auto&  ssr      = products.ssr;
            const double clk_hold = config.getSsrClockStaleHoldFactor();
            const double window   = SsrCorrectionStore::clockHoldWindow(
                config.getSsrMaxAge(), clk_hold);
            // The combined σ applies only to a fresh network correction.  A
            // stale-held clock (content age in (max_age, hold_window],
            // RTA-9(b)) keeps the v1 URA path below, so its ZOH clock-staleness
            // growth (q²·dt³/3) is retained: combined_var carries no staleness
            // term when the producer advertised no agek, and a held clock is
            // never consumed at full weight.  agek covers latency within the
            // fresh window; beyond it the conservative v1 staleness model sets
            // the weighting.  With clk_hold ≤ 1 (default) every accepted clock
            // is Fresh, so the common case is unchanged.
            bool clock_fresh = true;
            if (clk_hold > 1.0) {
                if (auto ep = ssr.getSsrEpoch(obs.satellite)) {
                    const double age = TimeUtils::continuousGpsSeconds(obs.time)
                                     - TimeUtils::continuousGpsSeconds(*ep);
                    clock_fresh = SsrCorrectionStore::classifyClockAge(
                                      age, config.getSsrMaxAge(), clk_hold)
                                  == SsrCorrectionStore::ClockAgeRung::Fresh;
                }
            }
            double sig_c = 0.0;
            if (ref_type[0] == 'L') {          // phase row: combined phase σ (sigc)
                const bool   pb_gate    = config.enableSsrBiasMaxAge();
                const double pb_max_age = config.getSsrBiasMaxAge()
                                        * config.getSsrBiasStaleHoldFactor();
                auto pb = pb_gate
                    ? ssr.getPhaseBias(obs.satellite, ref_type, obs.time, pb_max_age)
                    : ssr.getPhaseBias(obs.satellite, ref_type);
                if (pb && pb->combined_sigma_m > 0.0
                       && std::isfinite(pb->combined_sigma_m))
                    sig_c = pb->combined_sigma_m;
            } else if (ref_type[0] == 'C') {   // code row: combined code σ (sigcc, per-sat)
                if (auto oc = ssr.getOrbitClock(obs.satellite, obs.time, window);
                    oc && oc->has_clock && oc->combined_code_sigma_m > 0.0
                       && std::isfinite(oc->combined_code_sigma_m))
                    sig_c = oc->combined_code_sigma_m;
            }
            if (sig_c > 0.0 && clock_fresh) {
                // Age inflation σc²·(1 + agek·Δt_age²): Δt_age is the correction
                // content age (obs.time − SSR content epoch), the same dt the URA
                // ZOH term uses.  agek 0 (producer advertised none) ⇒ plain σc².
                double       infl = 1.0;
                const double agek = ssr.combinedSigmaAgeCoeff();
                if (agek > 0.0) {
                    if (auto ep = ssr.getSsrEpoch(obs.satellite)) {
                        const double dt = TimeUtils::continuousGpsSeconds(obs.time)
                                        - TimeUtils::continuousGpsSeconds(*ep);
                        if (dt > 0.0) infl = 1.0 + agek * dt * dt;
                    }
                }
                combined_var    = sig_c * sig_c * infl;
                combined_active = true;
            }
        }

        // Inflate R by the network URA on rows carrying this satellite's SSR
        // corrections (an independent clock/bias error variance), plus the ZOH
        // clock-staleness term q²·dt³/3 (B1).  Off by default and never applied
        // to the producer's own rows, so with it off R is byte-identical.
        //
        // The clock stale-hold rung extends this block rather than adding a
        // second inflation path (RTA-9(b)).  With
        // processing.ssr_clock_stale_hold_factor > 1: (1) the URA freshness
        // window widens to the same clockHoldWindow that the clock apply gate
        // uses (a URA as stale as the held clock still qualifies, and the
        // q²·dt³/3 term then grows with the true dt); (2) when the URA channel
        // does not deliver the staleness term for a stale-held clock
        // (consume_ura off, no URA record for the sat, or ura_staleness_q2
        // explicitly 0), the same q²·dt³/3 form is applied with a fallback q²,
        // so a held clock is never consumed at full weight.  Factor 0 (default)
        // leaves the branch unchanged.
        {
            const double clk_hold = config.getSsrClockStaleHoldFactor();
            const bool consumer_rows =
                config.getProcessingMode() != ProcessingMode::PPP_RTK_Network;
            if ((config.networkConsumeUra() || clk_hold > 1.0) && consumer_rows
                && !combined_active) {   // combined σ supersedes URA on this row (M4)
                const auto& ssr = products.ssr;
                const double max_age = config.getSsrMaxAge();
                // URA freshness is gated on the same window as the
                // clock/orbit overlay.  getUra's own default is a looser 60 s,
                // so a 60 s old URA could qualify a clock the store rejects at
                // 30 s; clockHoldWindow widens both together.
                const double window =
                    SsrCorrectionStore::clockHoldWindow(max_age, clk_hold);
                // Content-epoch age: the dt over which the clock polynomial
                // extrapolates and with which the ZOH variance grows.
                double dt = 0.0;
                bool   have_dt = false;
                if (auto ep = ssr.getSsrEpoch(obs.satellite)) {
                    dt = TimeUtils::continuousGpsSeconds(obs.time)
                       - TimeUtils::continuousGpsSeconds(*ep);
                    have_dt = true;
                }
                const double q2 = config.networkUraStalenessQ2();
                double add_var = 0.0;
                bool staleness_folded = false;   // ZOH term already delivered by the URA channel
                if (config.networkConsumeUra()) {
                    if (auto ura = ssr.getUra(obs.satellite, obs.time, window)) {
                        add_var += (*ura) * (*ura);
                        if (q2 > 0.0 && have_dt && dt > 0.0) {
                            add_var += q2 * dt * dt * dt / 3.0;  // ZOH extrapolation var
                            staleness_folded = true;
                        }
                    }
                }
                // Fallback rung (RTA-9(b)): a stale-held clock (age in
                // (max_age, window]) whose staleness the URA channel did not
                // fold in gets the same q²·dt³/3 growth.  The fallback
                // q² = 1.5e-7 m²/s³ is the network.clock_model_q2 default (the
                // random-walk-FM density of the producer's own clock model),
                // so the held-clock variance grows at the rate the product's
                // clock is modelled to wander.  Only rows whose satellite
                // carries an applied (held) SSR clock qualify.
                if (clk_hold > 1.0 && !staleness_folded && have_dt
                    && SsrCorrectionStore::classifyClockAge(dt, max_age, clk_hold)
                           == SsrCorrectionStore::ClockAgeRung::StaleHeld) {
                    if (auto oc = ssr.getOrbitClock(obs.satellite, obs.time, window);
                        oc && oc->has_clock) {
                        constexpr double kSsrClockHoldFallbackQ2 = 1.5e-7;  // m²/s³
                        const double q2_eff = q2 > 0.0 ? q2 : kSsrClockHoldFallbackQ2;
                        add_var += q2_eff * dt * dt * dt / 3.0;
                    }
                }
                if (add_var > 0.0) variance += add_var;
            }
        }

        // Inflate a phase row's measurement variance by the network phase-bias
        // correction σ² (B1b; defect-2 lever B, Tienstra §6.6): the phase-bias
        // block of Qŷ_B1 that the clock-only URA channel omits.  Without it the
        // consumer applies phase-bias corrections as if exact; with it a
        // wandering bias (the defect-2 driver) is down-weighted.
        // Per-(sat,signal) via getPhaseBias; IF rows return nullopt (per-signal
        // biases only) and are untouched.  Producer rows are excluded.  σ is 0
        // for RTCM/IGS-SSR biases and when emit_phase_bias_sigma is off, so with
        // the knob off (or on a non-network stream) R is byte-identical.
        if (config.networkConsumePhaseBiasSigma() && !combined_active
            && ref_type.size() >= 2 && ref_type[0] == 'L'
            && config.getProcessingMode() != ProcessingMode::PPP_RTK_Network) {
            // Use the same freshness decision as the SSR apply site in
            // buildJacobianRow, so R is inflated by the σ of the bias that was
            // applied, not of one the apply path has already aged out (or, at
            // the hold boundary, the reverse) (T3e).  gate/max_age are
            // recomputed here because they are out of scope in this function.
            // With the A5 gate off, the plain overload is used and the result
            // is byte-identical.
            const bool   pb_gate    = config.enableSsrBiasMaxAge();
            const double pb_max_age = config.getSsrBiasMaxAge()
                                    * config.getSsrBiasStaleHoldFactor();
            auto pb = pb_gate
                ? products.ssr.getPhaseBias(obs.satellite, ref_type, obs.time, pb_max_age)
                : products.ssr.getPhaseBias(obs.satellite, ref_type);
            if (pb && pb->sigma_m > 0.0 && std::isfinite(pb->sigma_m)) {
                variance += pb->sigma_m * pb->sigma_m;
            }
        }

        // Apply the combined-correction variance for this row; the URA and B1b
        // additions above were suppressed by !combined_active (M4, contract v2).
        if (combined_active)
            variance += combined_var;

        // Downweight code on signals without BIA coverage
        if (!config.estimateSatBiases()
            && config.noBiaCodeDownweight() > 1.0
            && ref_type.size() >= 2 && ref_type[0] == 'C') {
            const bool is_if = (ref_type.find('_') != std::string::npos);
            // A signal the live SSR stream corrects is not missing
            // bias coverage (ssr_bias_term applies to the row), so it
            // is not downweighted as if uncorrected.  (Raw signals
            // only; IF rows get ssr_bias_term = 0, so the IF branch
            // keeps the .BIA check alone.)
            const bool missing_bia = is_if
                ? !ifCombHasFullBia(products, obs.satellite, ref_type)
                : (config.isUncombinedEnabled()
                   && !biaCoversSignal(products, obs.satellite, ref_type)
                   && !ssrCoversCodeSignal(products, obs.satellite, ref_type, obs.time));
            if (missing_bia)
                variance *= config.noBiaCodeDownweight();
        }

        return variance;
    }

} // namespace GnssProcessor
