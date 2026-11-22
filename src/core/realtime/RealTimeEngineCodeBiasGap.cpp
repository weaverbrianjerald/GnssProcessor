/**
 * @file RealTimeEngineCodeBiasGap.cpp
 * @brief Engine side of the consumer's response to a code signal whose satellite code bias is
 *        missing from the stream (processing.ssr_code_bias_gap_response; realtime/CodeBiasGap.hpp).
 *
 * Three seams and the run-end line:
 *  - observeCodeBiasGaps_ (with the datum-break reactions, before the update): asks the store
 *    whether each code signal of a modelled satellite has its bias, with the freshness test of the
 *    apply site (addSsrBias), and records the edges;
 *  - addCodeBiasGapExclusions_ (inside runMeasurementUpdate, mode full): leaves the bias-less
 *    code rows out of every pass of the update;
 *  - revalidateCodeBiasGaps_ (after the update, before the integer search): tests the holds of
 *    the satellites in their re-validation window, re-floats a failed one, and bars the unheld
 *    ambiguities of the barred satellites from the search (mode full).
 *
 * Every seam's first statement is its arming gate, so with the key off nothing is measured or
 * written and the output is byte-identical.
 */

#include "realtime/RealTimeEngine.hpp"
#include "common/Constants.hpp"
#include "common/GnssIdentity.hpp"
#include "filter/StateKeySchema.hpp"
#include <cmath>
#include <iterator>
#include <optional>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace GnssProcessor {

namespace {

/// Variance below which an ambiguity counts as held (m^2).  The hold variance is 1e-7 m^2 by
/// default; a float's variance stays above 1e-4 m^2 (a hundredth of a cycle squared and more).
constexpr double kHeldVarianceM2 = 1e-6;
/// A held ambiguity sits on a whole cycle; anything further from one is not a hold.
constexpr double kHeldIntegerTolCyc = 0.01;

}  // namespace

void RealTimeEngine::observeCodeBiasGaps_(RtEpochContext& ctx) {
    const CodeBiasGap::Mode mode = codeBiasGapModeFromString(config_.getSsrCodeBiasGapResponse());
    if (mode == CodeBiasGap::Mode::Off) return;
    if (!code_bias_gap_configured_) {
        code_bias_gap_.configure({ mode, config_.getSsrCodeBiasGapRevalidateEp(),
                                   config_.getSsrCodeBiasGapWindowEp() });
        code_bias_gap_configured_ = true;
        log_manager_.getLogger("rt_engine")->info(
            "[RT] Code-bias gap response ARMED (processing.ssr_code_bias_gap_response {}): a code "
            "signal whose stream code bias is missing {}; after the bias returns, every hold of the "
            "satellite in the next {} epochs is tested over {} clean epochs against its corrected "
            "Melbourne-Wubbena and re-floated when it is more than {:.1f} cycle off.",
            codeBiasGapModeName(mode),
            mode == CodeBiasGap::Mode::Full
                ? "is left out of the update and bars a new fix of its satellite"
                : "stays in the update (revalidate: the open edge writes nothing)",
            code_bias_gap_.config().window_ep, code_bias_gap_.config().revalidate_ep,
            CodeBiasGap::kReleaseCycles);
    }

    const SsrCorrectionStore& ssr = ssrStore();
    const bool   gate    = config_.enableSsrBiasMaxAge();
    const double max_age = config_.getSsrBiasMaxAge() * config_.getSsrBiasStaleHoldFactor();
    auto hasCode = [&](const std::string& sat, const std::string& code, const GnssTime& t) {
        return gate ? ssr.hasCodeBias(sat, code, t, max_age) : ssr.hasCodeBias(sat, code);
    };
    auto hasPhase = [&](const std::string& sat, const std::string& phase, const GnssTime& t) {
        return (gate ? ssr.getPhaseBias(sat, phase, t, max_age) : ssr.getPhaseBias(sat, phase))
            .has_value();
    };

    for (const auto& obs : ctx.modeled_obs) {
        std::vector<std::pair<std::string, bool>> codes;
        bool on_wire = false;   // the stream corrects this satellite this epoch
        for (const auto& kv : obs.observed) {
            const std::string& sig = kv.first;
            if (sig.size() < 2 || isIonoFreePairLabel(sig)) continue;
            const ObsType type = obsTypeOf(sig);
            if (type == ObsType::Code) {
                const bool has = hasCode(obs.satellite, sig, obs.time);
                codes.emplace_back(sig, has);
                on_wire = on_wire || has;
            } else if (type == ObsType::Phase) {
                on_wire = on_wire || hasPhase(obs.satellite, sig, obs.time);
            }
        }
        // A satellite the stream does not correct at all is the unrecorded-satellite rule's
        // domain, not a gap: its signals keep their state.
        if (!on_wire) continue;
        for (const auto& [sig, has] : codes) code_bias_gap_.observe(obs.satellite, sig, has);
    }
    flushCodeBiasGapEvents_();
}

void RealTimeEngine::addCodeBiasGapExclusions_(std::set<std::string>& excluded) {
    if (!code_bias_gap_.armed()) return;
    const auto keys = code_bias_gap_.excludedRowKeys();
    if (keys.empty()) return;
    const std::set<std::string>& pivots = state_manager_->getFrozenPivotSatellites();
    uint64_t n_excl = 0, n_pivot = 0;
    for (const auto& key : keys) {
        const std::string sat = key.substr(0, key.find('_'));
        if (pivots.count(sat)) { ++n_pivot; continue; }   // its row stands in every single difference
        excluded.insert(key);
        ++n_excl;
    }
    code_bias_gap_.noteRowsExcluded(n_excl);
    code_bias_gap_.notePivotKept(n_pivot);
}

void RealTimeEngine::revalidateCodeBiasGaps_(RtEpochContext& ctx) {
    if (!code_bias_gap_.armed()) return;

    const std::vector<std::string> window_sats = code_bias_gap_.windowSatellites();
    const bool full = code_bias_gap_.config().mode == CodeBiasGap::Mode::Full;
    if (window_sats.empty() && !full) return;

    const SsrCorrectionStore& ssr = ssrStore();
    const bool   gate    = config_.enableSsrBiasMaxAge();
    const double max_age = config_.getSsrBiasMaxAge() * config_.getSsrBiasStaleHoldFactor();
    const PppUtils& utils = ppp_model_->pppUtils();
    const Eigen::VectorXd x = state_manager_->getStateVector();
    auto log = log_manager_.getLogger("rt_engine");

    // --- Test the holds of the satellites in their window ---
    if (!window_sats.empty()) {
        // d = held wide lane - corrected Melbourne-Wubbena, for every held satellite with clean
        // code this epoch; the per-system median then removes the receiver's wide-lane bias and
        // the frame's pivot.
        std::map<std::string, double> d;
        for (const auto& obs : ctx.modeled_obs) {
            const std::string& sat = obs.satellite;
            // One phase signal per band (the lexically smallest with its code), two lowest bands.
            std::map<char, std::string> band_sig;
            for (const auto& kv : obs.observed) {
                const std::string& sig = kv.first;
                if (sig.size() < 2 || isIonoFreePairLabel(sig) || obsTypeOf(sig) != ObsType::Phase)
                    continue;
                if (!obs.observed.count("C" + sig.substr(1))) continue;
                const auto it = band_sig.find(sig[1]);
                if (it == band_sig.end() || sig < it->second) band_sig[sig[1]] = sig;
            }
            if (band_sig.size() < 2) continue;
            const std::string s1 = band_sig.begin()->second;
            const std::string s2 = std::next(band_sig.begin())->second;
            const std::string c1 = "C" + s1.substr(1), c2 = "C" + s2.substr(1);

            // Clean code: both code biases and both phase biases present (the apply site's test).
            if (code_bias_gap_.satInGap(sat)) continue;
            const auto pb1 = gate ? ssr.getPhaseBias(sat, s1, obs.time, max_age) : ssr.getPhaseBias(sat, s1);
            const auto pb2 = gate ? ssr.getPhaseBias(sat, s2, obs.time, max_age) : ssr.getPhaseBias(sat, s2);
            const bool cb_ok = gate ? (ssr.hasCodeBias(sat, c1, obs.time, max_age)
                                       && ssr.hasCodeBias(sat, c2, obs.time, max_age))
                                    : (ssr.hasCodeBias(sat, c1) && ssr.hasCodeBias(sat, c2));
            if (!pb1 || !pb2 || !cb_ok) continue;
            const double cb1 = gate ? ssr.getCodeBias(sat, c1, obs.time, max_age) : ssr.getCodeBias(sat, c1);
            const double cb2 = gate ? ssr.getCodeBias(sat, c2, obs.time, max_age) : ssr.getCodeBias(sat, c2);

            // Held integers: both ambiguities at the hold variance on a whole cycle, or the frozen
            // pivot (value 0, variance 0), whose wide lane is the frame's zero.
            const double lam1 = utils.getWavelength(obs.system, s1, sat);
            const double lam2 = utils.getWavelength(obs.system, s2, sat);
            if (!(lam1 > 0.0) || !(lam2 > 0.0)) continue;
            const int i1 = state_manager_->getAmbiguityIndex(StateKeys::makeAmbiguityKey(sat, s1));
            const int i2 = state_manager_->getAmbiguityIndex(StateKeys::makeAmbiguityKey(sat, s2));
            if (i1 < 0 || i2 < 0 || i1 >= x.size() || i2 >= x.size()) continue;
            const double v1 = state_manager_->getStateVariance(i1);
            const double v2 = state_manager_->getStateVariance(i2);
            if (!(v1 < kHeldVarianceM2) || !(v2 < kHeldVarianceM2)) continue;
            const double n1 = x(i1) / lam1, n2 = x(i2) / lam2;
            if (std::abs(n1 - std::round(n1)) > kHeldIntegerTolCyc
                || std::abs(n2 - std::round(n2)) > kHeldIntegerTolCyc) continue;

            const double f1 = Constants::SPEED_OF_LIGHT / lam1;
            const double f2 = Constants::SPEED_OF_LIGHT / lam2;
            if (!(std::abs(f1 - f2) > 1.0)) continue;
            const double mw = CodeBiasGap::correctedMelbourneWubbena(
                f1, f2, obs.observed.at(s1), obs.observed.at(s2), obs.observed.at(c1),
                obs.observed.at(c2), pb1->bias_m, pb2->bias_m, cb1, cb2);
            d[sat] = (std::round(n1) - std::round(n2)) - mw;
        }
        const std::map<std::string, double> off = CodeBiasGap::medianRemoved(d);

        std::vector<std::string> release;
        for (const auto& sat : window_sats) {
            const auto it = off.find(sat);
            const auto verdict = code_bias_gap_.sample(
                sat, it == off.end() ? std::nullopt : std::optional<double>(it->second));
            if (verdict && *verdict == CodeBiasGap::EventKind::Released) release.push_back(sat);
        }
        if (!release.empty()) {
            const double reset_std = config_.getAmbiguityStd();
            for (const auto& sat : release) {
                int n_keys = 0;
                std::vector<std::string> keys;
                for (const auto& [key, idx] : state_manager_->getAmbiguities()) {
                    (void)idx;
                    const auto parts = StateKeys::parseAmbiguityKey(key);
                    if (parts && parts->satellite == sat) keys.push_back(key);
                }
                for (const auto& key : keys)
                    if (state_manager_->resetAmbiguity(key, reset_std)) ++n_keys;
                noteAmbLevelDisturbed_(sat);
                log->info("[RT] Code-bias gap: re-floated {} ({} ambiguity key(s) at std {:.3f} m)",
                          sat, n_keys, reset_std);
            }
            if (ambiguity_resolver_)
                ambiguity_resolver_->forgetHeldPairs(std::set<std::string>(release.begin(), release.end()));
        }
        flushCodeBiasGapEvents_();
    }

    // --- Bar the unheld ambiguities of the barred satellites from the integer search ---
    if (!full) return;
    uint64_t n_barred = 0;
    std::set<std::string> seen;
    const Eigen::VectorXd x2 = state_manager_->getStateVector();
    for (const auto& obs : ctx.modeled_obs) {
        const std::string& sat = obs.satellite;
        if (!seen.insert(sat).second || !code_bias_gap_.barred(sat)) continue;
        bool any = false;
        for (const auto& kv : obs.observed) {
            const std::string& sig = kv.first;
            if (sig.size() < 2 || isIonoFreePairLabel(sig) || obsTypeOf(sig) != ObsType::Phase) continue;
            const std::string key = StateKeys::makeAmbiguityKey(sat, sig);
            const int idx = state_manager_->getAmbiguityIndex(key);
            if (idx < 0 || idx >= x2.size()) continue;
            if (state_manager_->getStateVariance(idx) < kHeldVarianceM2) continue;   // a hold taken on clean code stays
            state_manager_->markAmbiguityUnresolvable(key);
            any = true;
        }
        if (any) ++n_barred;
    }
    code_bias_gap_.noteBarred(n_barred);
}

void RealTimeEngine::flushCodeBiasGapEvents_() {
    const auto& events = code_bias_gap_.events();
    if (events.empty()) return;
    auto log = log_manager_.getLogger("rt_engine");
    for (const auto& e : events) {
        const char* name = codeBiasGapEventName(e.kind);
        switch (e.kind) {
        case CodeBiasGap::EventKind::Open:
            log->info("[RT] Code-bias gap OPEN: {} {} has no code bias on the stream{}",
                      e.sat, e.signal,
                      code_bias_gap_.config().mode == CodeBiasGap::Mode::Full
                          ? " (row left out, no new fix of the satellite)" : "");
            break;
        case CodeBiasGap::EventKind::Close:
            log->info("[RT] Code-bias gap CLOSE: {} {} has its code bias again after {} epoch(s); "
                      "re-validation window opens", e.sat, e.signal, e.n);
            break;
        case CodeBiasGap::EventKind::Validated:
            log->info("[RT] Code-bias gap: hold of {} VALIDATED (mean offset {:+.3f} cycle over {} "
                      "epochs)", e.sat, e.mean_cyc, e.n);
            break;
        case CodeBiasGap::EventKind::Released:
            log->info("[RT] Code-bias gap: hold of {} RELEASED (mean offset {:+.3f} cycle over {} "
                      "epochs, beyond {:.1f})", e.sat, e.mean_cyc, e.n, CodeBiasGap::kReleaseCycles);
            break;
        case CodeBiasGap::EventKind::Expired:
            log->info("[RT] Code-bias gap: window of {} ended without a verdict", e.sat);
            break;
        }
        noteConsumerSatEvent_(e.sat + "|" + (e.signal.empty() ? std::string("CLK") : e.signal), name);   // satellite-level events on the ev_clk column
    }
    code_bias_gap_.clearEvents();
}

void RealTimeEngine::logCodeBiasGapSummary(const char* tag) const {
    if (!code_bias_gap_.armed()) return;   // silent unless armed (byte-identical)
    const auto& c = code_bias_gap_.counters();
    log_manager_.getLogger("rt_engine")->info(
        "[RT] Code-bias gap response ({}, {}): gaps opened {} / closed {} ({} signal-epochs without "
        "a code bias); code rows left out {}, pivot rows kept {}; satellite-epochs barred from a new "
        "fix {}; re-validation windows {}: validated {} / released {} / expired {} ({} held epochs "
        "tested); closure {}",
        tag, codeBiasGapModeName(code_bias_gap_.config().mode), c.opened, c.closed, c.gap_epochs,
        c.rows_excluded, c.pivot_kept, c.barred_epochs, c.windows, c.validated, c.released,
        c.expired, c.tested_epochs, code_bias_gap_.closureHolds() ? "HOLDS" : "BROKEN");
}

}  // namespace GnssProcessor
