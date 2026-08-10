/**
 * @file RealTimeEngineSsrDiag.cpp
 * @brief Consumer-side SSR observability seams for RealTimeEngine::processEpoch.
 *
 * Pure diagnostics: neither seam mutates estimator state.  Both run BEFORE
 * the broadcast-grade drop so satellites the precise solution discards
 * still appear in the census and the trace.
 */

#include "realtime/RealTimeEngine.hpp"
#include "math/TimeUtils.hpp"
#include "products/SsrCorrectionStore.hpp"
#include <limits>
#include <fmt/format.h>

namespace GnssProcessor {

/**
 * @brief One-line per-epoch SSR coverage summary (are corrections flowing AND applying?).
 *
 * Relocated verbatim from the pre-cut RealTimeEngine::processEpoch
 * (frozen lines 1130-1144); see scripts/rt_seam_verify.py.
 */
void RealTimeEngine::logSsrCoverage(RtEpochContext& ctx) {
    auto log = log_manager_.getLogger("rt_engine");
    const std::string& epoch_prefix = ctx.epoch_prefix;
    const GnssTime& current_time = ctx.current_time;
    const std::vector<PppObservation>& modeled_obs = ctx.modeled_obs;

    if (config_.realtimeUsesLiveOrbitClockSource()) {
        std::vector<std::string> sats;
        sats.reserve(modeled_obs.size());
        size_t n_cm = 0;
        for (const auto& o : modeled_obs) {
            sats.push_back(o.satellite);
            if (!o.broadcast_only) ++n_cm;
        }
        const auto cov = ssrStore().coverageFor(sats, current_time);
        log->info("{}SSR coverage: {} sats | clk-applied {}/{} | in-store orb {}/{} "
                  "clk {}/{} cbias {}/{} pbias {}/{}",
                  epoch_prefix, cov.total, n_cm, cov.total,
                  cov.orbit, cov.total, cov.clock, cov.total,
                  cov.code_bias, cov.total, cov.phase_bias, cov.total);
    }
}

/**
 * @brief L10 consumer-side SSR-application trace (network.trace_ssr_consumer).
 *
 * Relocated verbatim from the pre-cut RealTimeEngine::processEpoch
 * (frozen lines 1156-1225); see scripts/rt_seam_verify.py.
 */
void RealTimeEngine::traceSsrConsumer(RtEpochContext& ctx) {
    const GnssTime& current_time = ctx.current_time;
    const std::vector<PppObservation>& modeled_obs = ctx.modeled_obs;

    if (ssr_consumer_trace_.is_open()) {
        const auto& store = ssrStore();
        const double now_s = TimeUtils::continuousGpsSeconds(current_time);
        // RTA-9(b): mirror the apply-site windows — with the clock stale-hold
        // rung armed, the clock/URA lookups accept the same widened window the
        // apply gate uses (the recomputed r_add_var then grows with the true dt
        // exactly like the URA channel at the apply site; the rung's fallback-q²
        // term for URA-less sats is NOT folded in here — clk_applied + blank
        // ura_m identifies those rows).
        const double ssr_max_age = SsrCorrectionStore::clockHoldWindow(
            config_.getSsrMaxAge(), config_.getSsrClockStaleHoldFactor());
        const bool   consume_ura = config_.networkConsumeUra();
        const double ura_q2 = config_.networkUraStalenessQ2();
        const bool   bias_age_on = config_.enableSsrBiasMaxAge();
        const double bias_limit = config_.getSsrBiasMaxAge() * config_.getSsrBiasStaleHoldFactor();
        for (const auto& obs : modeled_obs) {
            const std::string& sat = obs.satellite;
            auto& o = ssr_consumer_trace_;
            o << current_time.week << ',' << fmt::format("{:.3f}", current_time.tow) << ','
              << sat << ',';
            const auto ep = store.getSsrEpoch(sat);
            double ssr_dt = 0.0; bool have_ep = false;
            if (ep) { ssr_dt = now_s - TimeUtils::continuousGpsSeconds(*ep); have_ep = true;
                      o << fmt::format("{:.3f}", ssr_dt); }
            o << ',';
            o << (obs.broadcast_only ? 0 : 1) << ',';                       // clk_applied (TRUE signal)
            const auto oc = store.getOrbitClock(sat, current_time, ssr_max_age);
            if (oc && oc->has_clock) o << fmt::format("{:.4f}", oc->c0);
            o << ',';
            const auto ura = store.getUra(sat, current_time, ssr_max_age);
            if (ura) o << fmt::format("{:.4f}", *ura);
            o << ',';
            double add_var = 0.0;   // recomputed R-inflation (0 when consume_ura off / no URA)
            if (consume_ura && ura) {
                add_var = (*ura) * (*ura);
                if (ura_q2 > 0.0 && have_ep && ssr_dt > 0.0)
                    add_var += ura_q2 * ssr_dt * ssr_dt * ssr_dt / 3.0;   // == MeasurementModel apply site
            }
            o << fmt::format("{:.6e}", add_var) << ',';
            const auto pbt = store.getPhaseBiasTime(sat);
            if (pbt) o << fmt::format("{:.3f}", now_s - TimeUtils::continuousGpsSeconds(*pbt));
            o << ',';
            int n_pb = 0;
            int pb_disc_max = std::numeric_limits<int>::min();
            int cb_disc_max = std::numeric_limits<int>::min();
            for (const auto& kv : obs.modeled) {              // mirror the A5 tracked-signal filter
                const std::string& code = kv.first;
                if (code.empty() || code.find('_') != std::string::npos) continue;
                if (code[0] == 'L') {
                    auto pb = bias_age_on ? store.getPhaseBias(sat, code, current_time, bias_limit)
                                          : store.getPhaseBias(sat, code);
                    if (pb) { ++n_pb; pb_disc_max = std::max(pb_disc_max, pb->discontinuity_counter); }
                } else if (code[0] == 'C') {
                    auto d = store.getCodeBiasDiscontinuity(
                        sat, code, current_time,
                        bias_age_on ? bias_limit : std::numeric_limits<double>::max());
                    if (d) cb_disc_max = std::max(cb_disc_max, *d);
                }
            }
            o << n_pb << ',';
            const auto cd = store.getClockDiscontinuity(sat);
            if (cd) o << *cd;                                              // blank on nullopt
            o << ',';
            if (pb_disc_max != std::numeric_limits<int>::min()) o << pb_disc_max;
            o << ',';
            if (cb_disc_max != std::numeric_limits<int>::min()) o << cb_disc_max;
            o << ',';
            o << (n_pb > 0 ? 1 : 0) << '\n';                              // pb_fresh
        }
    }
}

} // namespace GnssProcessor
