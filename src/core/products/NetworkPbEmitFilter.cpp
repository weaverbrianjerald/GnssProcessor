/**
 * @file NetworkPbEmitFilter.cpp
 * @brief Implementation of the emission-frame phase-bias continuity filter.
 */

#include "products/NetworkPbEmitFilter.hpp"

#include <algorithm>

namespace GnssProcessor {

double NetworkPbEmitFilter::stepSigma(const std::string& constellation) const {
    // Weak seed prior of kPriorRows pseudo-samples whose robust floor is the seed
    // variance (log per pseudo-sample = log(seed²·kChi2GeoMean)), so the estimate
    // is always defined, regularised toward the seed, and cannot collapse to 0.
    const double seed_var = cfg_.meas_sigma_m * cfg_.meas_sigma_m;
    double sum_log = kPriorRows * std::log(seed_var * kChi2GeoMean);
    double n       = kPriorRows;
    const auto it = step_.find(constellation);
    if (it != step_.end()) { sum_log += it->second.sum_log_e2; n += it->second.n_eff; }
    const double geo = std::exp(sum_log / n);          // geometric mean of ê²
    double s = std::sqrt(geo / kChi2GeoMean);          // recover the innovation σ
    const double floor = kSigmaFloorFrac * cfg_.meas_sigma_m;
    return std::max(s, floor);
}

bool NetworkPbEmitFilter::stateForTest(const std::string& sat, const std::string& signal,
                                       double& mean, double& var) const {
    const auto it = state_.find(sat + "|" + signal);
    if (it == state_.end()) return false;
    mean = it->second.mean;
    var  = it->second.var;
    return true;
}

void NetworkPbEmitFilter::filterEpoch(std::vector<Entry>& entries, double epoch_time_s) {
    if (entries.empty()) return;
    ++counters_.epochs;

    // Advance the trailing window on the σ̂_step accumulators BEFORE reading them,
    // so this epoch's gate uses the estimate from prior epochs only (a row never
    // gates itself).  Decayed here, fed at the bottom (smoothed rows only).
    if (phi_ < 1.0)
        for (auto& kv : step_) { kv.second.sum_log_e2 *= phi_; kv.second.n_eff *= phi_; }

    for (Entry& e : entries) {
        const std::string key  = e.sat + "|" + e.signal;
        const std::string cst  = bucket(e.sat);
        // σ̂_step doubles as the self-tuning measurement noise R (§ header): the
        // filter needs no hand R and adapts to the measured per-epoch noise,
        // sidestepping the datum-dependent level entanglement of the solve σ.
        const double sstep = stepSigma(cst);
        const double R     = sstep * sstep;

        const auto sit = state_.find(key);
        if (sit == state_.end()) {                       // --- first sighting: seed, pass through
            state_[key] = KeyState{e.value_m, R, epoch_time_s};
            e.post_sigma_m = sstep;
            e.reset_cause  = Cause::kInit;
            ++counters_.inits;
            continue;
        }

        KeyState& st = sit->second;
        const double gap   = epoch_time_s - st.last_seen_s;
        const double Ppred = st.var + cfg_.q_m2_s * std::max(0.0, gap);   // random-walk predict
        const double innov = e.value_m - st.mean;

        // Reset precedence: a reference-path slip and a stale gap are structural
        // (checked first); the calibrated innovation gate is the noise test.  A
        // reset EMITS THE RAW VALUE (e.value_m unchanged) so the genuine step
        // survives to the emitter's jump test — never laundered (invariant #1).
        Cause cause = Cause::kSmooth;
        if (e.slipped)                                    cause = Cause::kRefPathSlip;
        else if (gap > cfg_.max_gap_s)                    cause = Cause::kGap;
        else if (std::abs(innov) > cfg_.jump_k * sstep)   cause = Cause::kInnovation;

        if (cause != Cause::kSmooth) {
            st.mean        = e.value_m;                   // re-seed on the fresh value
            st.var         = R;
            st.last_seen_s = epoch_time_s;
            e.post_sigma_m = sstep;
            e.reset_cause  = cause;
            switch (cause) {
                case Cause::kRefPathSlip: ++counters_.resets_refpath;    break;
                case Cause::kGap:         ++counters_.resets_gap;        break;
                case Cause::kInnovation:  ++counters_.resets_innovation; break;
                default:                                                  break;
            }
            // The contaminated innovation is NOT fed to σ̂_step (clean-gate, like
            // the M2 monitor) so a genuine event never inflates the noise floor.
            continue;
        }

        // --- Smooth: emit the posterior mean, shrink P, feed the clean innovation.
        const double S = Ppred + R;
        const double K = Ppred / S;
        st.mean        = st.mean + K * innov;
        st.var         = Ppred * (1.0 - K);              // = Ppred·R/S
        st.last_seen_s = epoch_time_s;
        e.value_m      = st.mean;                        // the smoothed value the emitter carries
        e.post_sigma_m = std::sqrt(st.var);
        e.reset_cause  = Cause::kSmooth;
        ++counters_.smoothed;
        counters_.post_sigma_sum += e.post_sigma_m;

        StepAccum& a = step_[cst];
        const double e2 = innov * innov;
        a.sum_log_e2 += std::log(e2 > kTinyE2 ? e2 : kTinyE2);
        a.n_eff      += 1.0;
    }
}

}  // namespace GnssProcessor
