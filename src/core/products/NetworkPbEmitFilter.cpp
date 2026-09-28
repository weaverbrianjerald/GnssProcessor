/**
 * @file NetworkPbEmitFilter.cpp
 * @brief Implementation of the emission-frame phase-bias continuity filter.
 */

#include "products/NetworkPbEmitFilter.hpp"

#include <algorithm>

namespace GnssProcessor {

double NetworkPbEmitFilter::stepSigma(const std::string& constellation) const {
    // Weak seed prior of kPriorRows pseudo-samples, each with log ê² =
    // log(seed²·kChi2GeoMean), so that alone they imply σ̂ = seed.  The estimate
    // is always defined, regularised toward the seed, and cannot collapse to 0.
    const double seed_var = cfg_.meas_sigma_m * cfg_.meas_sigma_m;
    double sum_log = kPriorRows * std::log(seed_var * kChi2GeoMean);
    double n       = kPriorRows;
    const auto it = step_.find(constellation);
    if (it != step_.end()) { sum_log += it->second.sum_log_e2; n += it->second.n_eff; }
    const double geo = std::exp(sum_log / n);          // geometric mean of ê²
    double s = std::sqrt(geo / kChi2GeoMean);          // recover the innovation σ
    // v1 floors at a fraction of the seed (the censored feed can only shrink σ̂, so
    // it needs a structural-collapse guard).  v2 feeds every valid-prior row, so the
    // estimate cannot collapse; the floor is only a numerical guard on a degenerate
    // all-zeros window, and the seed keeps its role as the weak prior above.
    const double floor = cfg_.gate_v2 ? kSigmaFloorAbsM
                                      : kSigmaFloorFrac * cfg_.meas_sigma_m;
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

    // Decay the σ̂_step accumulators before any is read, so this epoch's gate uses
    // the estimate from prior epochs only (a row never gates itself).  Rows are fed
    // after their own gate decision (v1: smoothed rows only; v2: smoothed +
    // innovation-gated, see feedStep).
    if (phi_ < 1.0)
        for (auto& kv : step_) { kv.second.sum_log_e2 *= phi_; kv.second.n_eff *= phi_; }

    // Fold one innovation into a constellation's σ̂_step accumulator (χ²₁ geo-mean
    // of ê²) and count it (fed_rows = n_fed_all_rows telemetry).  v1 feeds smoothed
    // rows only; v2 also feeds kInnovation rows (the censoring-free feed below).
    auto feedStep = [this](const std::string& cst, double innov) {
        StepAccum& a = step_[cst];
        const double e2 = innov * innov;
        a.sum_log_e2 += std::log(e2 > kTinyE2 ? e2 : kTinyE2);
        a.n_eff      += 1.0;
        ++counters_.fed_rows;
    };

    for (Entry& e : entries) {
        const std::string key  = e.sat + "|" + e.signal;
        const std::string cst  = bucket(e.sat);
        // σ̂_step doubles as the self-tuning measurement noise R (see the header):
        // the filter needs no hand-set R and adapts to the measured per-epoch
        // noise, avoiding the datum-dependent level entanglement of the solve σ.
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
        const double S     = Ppred + R;                  // predicted innovation var (KF gain below; v2 gate scale)

        // Reset precedence: a reference-path slip, then a stale gap (both
        // structural), then the calibrated innovation gate (the noise test).  A
        // reset emits the raw value (e.value_m unchanged), so the genuine step
        // survives to the emitter's jump test and is not smoothed away
        // (invariant #1).  Gate scale: v1's k·σ̂_step, or v2's model-predicted
        // k·√(Ppred+R); dt enters via Ppred's q·gap term, so one k transfers
        // across cadences.
        Cause cause = Cause::kSmooth;
        if (e.slipped)                 cause = Cause::kRefPathSlip;
        else if (gap > cfg_.max_gap_s) cause = Cause::kGap;
        else {
            const double gate = cfg_.gate_v2 ? cfg_.jump_k * std::sqrt(S)
                                             : cfg_.jump_k * sstep;
            if (std::abs(innov) > gate) cause = Cause::kInnovation;
        }

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
            // Feed policy at a reset.  v1 (clean gate, like the M2 monitor):
            // withhold every reset row.  That censoring can collapse the gate: when
            // the true step sits at gate scale the withheld rows bias σ̂_step low,
            // tightening the gate until more rows are rejected.  v2 (censoring-free):
            // a kInnovation row still carries a valid prior and a legitimate
            // per-epoch step sample, so it is fed; the χ²₁ geo-mean is tail-safe to
            // the rare genuine event, and the estimator sees the gate-scale steps
            // that v1 withholds.  kRefPathSlip (exogenous, flagged) and kGap (no
            // per-epoch prior) stay out of the noise estimate.
            if (cfg_.gate_v2 && cause == Cause::kInnovation)
                feedStep(cst, innov);
            continue;
        }

        // --- Smooth: emit the posterior mean, shrink P, feed the clean innovation.
        const double K = Ppred / S;
        st.mean        = st.mean + K * innov;
        st.var         = Ppred * (1.0 - K);              // = Ppred·R/S
        st.last_seen_s = epoch_time_s;
        e.value_m      = st.mean;                        // the smoothed value the emitter carries
        e.post_sigma_m = std::sqrt(st.var);
        e.reset_cause  = Cause::kSmooth;
        ++counters_.smoothed;
        counters_.post_sigma_sum += e.post_sigma_m;

        feedStep(cst, innov);
    }
}

}  // namespace GnssProcessor
