/**
 * @file RealTimeEngineSsrDiag.cpp
 * @brief Consumer-side SSR observability seams for RealTimeEngine::processEpoch.
 *
 * Pure diagnostics: neither seam mutates estimator state.  Both run BEFORE
 * the broadcast-grade drop so satellites the precise solution discards
 * still appear in the census and the trace.
 */

#include "realtime/RealTimeEngine.hpp"
#include "filter/StateKeySchema.hpp"   // Card CST: StateKeys::makeAmbiguityKey
#include "math/TimeUtils.hpp"
#include "products/SsrCorrectionStore.hpp"
#include <algorithm>
#include <limits>
#include <map>
#include <string>
#include <utility>
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

// ---------------------------------------------------------------------------
// Card CST: per-satellite readout of the closed-loop consumer's own epoch
// ---------------------------------------------------------------------------
//
// NEW CODE (not a relocation), and deliberately the LAST thing the epoch does.
// The two seams above run BEFORE the broadcast-grade drop because they are about
// what ARRIVED; this one runs at the processEpoch tail, after the update, the
// constraints and AR, because it is about what the consumer DID — the states it
// reads have to be the committed ones, or "a declared reset shows as variance
// re-inflation" is a half-epoch out.
//
// EV6 (docs/refound/EV6_hour6_decouple_event_2026-09-15.md §9 item 1) named the
// gap this closes: closed_loop_trace.csv carries position and n_sats only, so a
// consumer-side float re-convergence could not be separated from the slant-iono
// half of the RF-0b applied pair, and an undeclared wire step could not be seen
// landing at the consumer at all.
//
// MEASURE-ONLY.  Every read is a const lookup into state the epoch has already
// committed; nothing is written back, so the consumer's solve is byte-identical
// with the knob armed.

void RealTimeEngine::noteConsumerSatEvent_(const std::string& key, const char* action) {
    if (!consumer_sat_trace_) return;   // unarmed: not even a map write
    auto& slot = consumer_sat_events_[key];
    if (slot.empty()) slot = action;
    else { slot += ';'; slot += action; }
}

void RealTimeEngine::traceConsumerSat_(RtEpochContext& ctx) {
    if (!consumer_sat_trace_) return;

    const GnssTime& current_time = ctx.current_time;
    const std::vector<PppObservation>& modeled_obs = ctx.modeled_obs;

    // The BIAS-routed store is what the measurement model reads for code/phase
    // bias; the CLOCK-routed one is what the geometry/corrections read for the
    // satellite clock.  Under the closed-loop harness's default profile
    // (consume_clock == consume_bias == true) they are the same object, but a
    // clock-on/bias-off profile splits them, and a row that mixed the two would
    // report a correction the consumer never applied.
    const SsrCorrectionStore& bias_store = ssrStore();
    const SsrCorrectionStore& clk_store =
        clock_store_override_ ? *clock_store_override_ : bias_store;

    // Mirror the apply-site staleness windows exactly (the same construction the
    // L10 trace above uses), so an age-gated-out correction reads BLANK here and
    // the row says "the consumer applied nothing", never "it applied zero".
    const double ssr_max_age = SsrCorrectionStore::clockHoldWindow(
        config_.getSsrMaxAge(), config_.getSsrClockStaleHoldFactor());
    const bool   bias_age_on = config_.enableSsrBiasMaxAge();
    const double bias_limit  = config_.getSsrBiasMaxAge() * config_.getSsrBiasStaleHoldFactor();

    // Row-aligned (sat, signal) -> pre-fit innovation / post-fit residual from the
    // COMMITTED update.  meas_update is per-epoch transport (RtEpochContext) and
    // dies with the epoch — which is exactly why this harvest lives in the engine
    // and not in the network seam that writes the file.  An epoch whose update
    // never committed leaves both maps empty and every residual column blank.
    std::map<std::string, double> innov_of, resid_of;
    if (ctx.update_applied) {
        const auto& mu = ctx.meas_update;
        const auto n_lbl = static_cast<Eigen::Index>(
            std::min(mu.satellite_ids.size(), mu.signal_types.size()));
        for (Eigen::Index i = 0; i < n_lbl; ++i) {
            const std::size_t u = static_cast<std::size_t>(i);
            // BSSD / DD rows are labelled "<sat_i>-<sat_p>" and are NOT a
            // single-satellite quantity; skip them rather than attribute a
            // differenced residual to one satellite.  (This consumer is
            // undifferenced on the RF-0hv posture, so nothing is skipped there.)
            const std::string& sat = mu.satellite_ids[u];
            if (sat.find('-') != std::string::npos) continue;
            const std::string k = sat + "|" + mu.signal_types[u];
            if (i < mu.innovations.size()) innov_of[k] = mu.innovations(i);
            if (i < mu.residuals.size())   resid_of[k] = mu.residuals(i);
        }
    }

    const Eigen::VectorXd x = state_manager_->getStateVector();
    auto stateAt = [&](int idx, double& v, double& var) {
        if (idx < 0 || idx >= static_cast<int>(x.size())) return false;
        v   = x(idx);
        var = state_manager_->getStateVariance(idx);
        return true;
    };

    for (const auto& obs : modeled_obs) {
        const std::string& sat = obs.satellite;
        if (sat.empty()) continue;

        // n_sig first: the row-level count of RAW phase signals this satellite
        // contributes, so a reader can tell a one-band epoch from a two-band one
        // without grouping the whole file.
        int n_sig = 0;
        for (const auto& kv : obs.modeled) {
            const std::string& c = kv.first;
            if (c.size() >= 2 && c[0] == 'L' && c.find('_') == std::string::npos) ++n_sig;
        }
        if (n_sig == 0) continue;

        // Per-SATELLITE quantities, resolved once and repeated on the sat's rows.
        const auto oc = clk_store.getOrbitClock(sat, current_time, ssr_max_age);
        const bool   have_clk = static_cast<bool>(oc) && oc->has_clock;
        const double clk_m    = have_clk ? oc->c0 : 0.0;
        const auto   clk_disc = clk_store.getClockDiscontinuity(sat);
        const auto   ev_clk   = consumer_sat_events_.find(sat + "|CLK");

        double iono_v = 0.0, iono_var = 0.0;
        const bool have_iono =
            stateAt(state_manager_->getIonosphereIndex(sat), iono_v, iono_var);

        for (const auto& kv : obs.modeled) {
            const std::string& sig = kv.first;
            if (sig.size() < 2 || sig[0] != 'L') continue;              // phase only
            if (sig.find('_') != std::string::npos) continue;            // raw signal only

            ConsumerSatRow r;
            r.sat = sat;
            r.sig = sig;
            r.elev_deg = obs.elevation;
            r.n_sig = n_sig;

            // The band+attribute-paired CODE signal ("L1W" -> "C1W"), but only
            // when the epoch actually carries it — the satellite code bias and
            // the slant ionosphere are the RF-0b pair a consumer must apply
            // TOGETHER, so they belong on one row.
            std::string code_sig = "C" + sig.substr(1);
            if (obs.modeled.find(code_sig) == obs.modeled.end()) code_sig.clear();
            r.code_sig = code_sig;

            r.have_clk = have_clk; r.clk_m = clk_m;
            if (clk_disc) { r.have_clk_disc = true; r.clk_disc = *clk_disc; }

            // The phase bias the model applied THIS epoch — the model's own read
            // (MeasurementModel ssr_bias_term: gated when the staleness gate is
            // on, ungated otherwise).
            const auto pb = bias_age_on
                ? bias_store.getPhaseBias(sat, sig, current_time, bias_limit)
                : bias_store.getPhaseBias(sat, sig);
            if (pb) {
                r.have_pb = true; r.pb_m = pb->bias_m;
                r.have_pb_disc = true; r.pb_disc = pb->discontinuity_counter;
            }

            if (!code_sig.empty()) {
                const bool cb_present = bias_age_on
                    ? bias_store.hasCodeBias(sat, code_sig, current_time, bias_limit)
                    : bias_store.hasCodeBias(sat, code_sig);
                if (cb_present) {
                    r.have_cb = true;
                    r.cb_m = bias_age_on
                        ? bias_store.getCodeBias(sat, code_sig, current_time, bias_limit)
                        : bias_store.getCodeBias(sat, code_sig);
                }
                const auto cbd = bias_store.getCodeBiasDiscontinuity(
                    sat, code_sig, current_time,
                    bias_age_on ? bias_limit : std::numeric_limits<double>::max());
                if (cbd) { r.have_cb_disc = true; r.cb_disc = *cbd; }
            }

            // What the consumer DID with each declaration this epoch — invariant
            // #7's re-coupling edge, read from the consumer's side.
            {
                const auto it = consumer_sat_events_.find(sat + "|" + sig);
                if (it != consumer_sat_events_.end()) r.ev_pb = it->second;
            }
            if (!code_sig.empty()) {
                const auto it = consumer_sat_events_.find(sat + "|" + code_sig);
                if (it != consumer_sat_events_.end()) r.ev_cb = it->second;
            }
            if (ev_clk != consumer_sat_events_.end()) r.ev_clk = ev_clk->second;

            {
                const auto it = innov_of.find(sat + "|" + sig);
                if (it != innov_of.end()) { r.have_innov_phase = true;
                                            r.innov_phase_m = it->second; }
            }
            {
                const auto it = resid_of.find(sat + "|" + sig);
                if (it != resid_of.end()) { r.have_res_phase = true;
                                            r.res_phase_m = it->second; }
            }
            if (!code_sig.empty()) {
                const auto it = innov_of.find(sat + "|" + code_sig);
                if (it != innov_of.end()) { r.have_innov_code = true;
                                            r.innov_code_m = it->second; }
                const auto jt = resid_of.find(sat + "|" + code_sig);
                if (jt != resid_of.end()) { r.have_res_code = true;
                                            r.res_code_m = jt->second; }
            }

            r.have_amb = stateAt(state_manager_->getAmbiguityIndex(
                                     StateKeys::makeAmbiguityKey(sat, sig)),
                                 r.amb_m, r.amb_var);
            r.have_iono = have_iono; r.iono_m = iono_v; r.iono_var = iono_var;

            consumer_sat_rows_.push_back(std::move(r));
        }
    }
}

} // namespace GnssProcessor
