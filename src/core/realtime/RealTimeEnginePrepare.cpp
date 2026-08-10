/**
 * @file RealTimeEnginePrepare.cpp
 * @brief Measurement-set conditioning seams for RealTimeEngine::processEpoch.
 *
 * Everything that reshapes the epoch's observation set or the filter's
 * starting point between computeModels and the KF predict.
 *
 * seedClockFromWls and regaugeBssd are MUTUALLY EXCLUSIVE by construction
 * (hasClock() vs !hasClock()): under BSSD the filter carries no receiver
 * clock state to seed, which is exactly why the re-gauge exists.  Both
 * self-gate, so the driver calls both unconditionally.
 */

#include "realtime/RealTimeEngine.hpp"
#include "math/TimeUtils.hpp"
#include "math/WlsEstimator.hpp"
#include <algorithm>
#include <cmath>

namespace GnssProcessor {

/**
 * @brief Drop broadcast-grade satellites from the precise SSR PPP-RTK solution.
 *
 * Relocated verbatim from the pre-cut RealTimeEngine::processEpoch
 * (frozen lines 1235-1244); see scripts/rt_seam_verify.py.
 */
void RealTimeEngine::dropBroadcastGrade(RtEpochContext& ctx) {
    auto log = log_manager_.getLogger("rt_engine");
    const std::string& epoch_prefix = ctx.epoch_prefix;
    std::vector<PppObservation>& modeled_obs = ctx.modeled_obs;

    if (config_.getCorrectionsReadyGate().drop_broadcast_grade) {
        size_t dropped = 0;
        for (auto it = modeled_obs.begin(); it != modeled_obs.end(); ) {
            if (it->broadcast_only) { it = modeled_obs.erase(it); ++dropped; }
            else ++it;
        }
        if (dropped > 0)
            log->debug("{}Dropped {} broadcast-grade sat(s) (no SSR clock) from the precise solution",
                       epoch_prefix, dropped);
    }
}

/**
 * @brief WLS receiver-clock seed. Self-gated on hasClock() -- mutually exclusive with regaugeBssd().
 *
 * Relocated verbatim from the pre-cut RealTimeEngine::processEpoch
 * (frozen lines 1249-1271); see scripts/rt_seam_verify.py.
 */
void RealTimeEngine::seedClockFromWls(RtEpochContext& ctx) {
    auto log = log_manager_.getLogger("rt_engine");
    const std::string& epoch_prefix = ctx.epoch_prefix;
    const GnssTime& current_time = ctx.current_time;
    const std::vector<PppObservation>& modeled_obs = ctx.modeled_obs;
    const bool gap_detected = ctx.gap_detected;

    if (state_manager_->hasClock()) {
        const auto wls_result = wls_estimator_->estimate(current_time, modeled_obs);
        if (wls_result.success() && wls_result.state.size() >= 4) {
            const double wls_clock = wls_result.state(3);
            const Eigen::VectorXd sv = state_manager_->getStateVector();
            const size_t clk_idx = state_manager_->getClockIndex();
            const double kf_clock = (sv.size() > static_cast<Eigen::Index>(clk_idx))
                                    ? sv(clk_idx) : 0.0;

            constexpr double kJumpThresh = 0.00005 * 299792458.0; // 0.5 ms
            const double delta = wls_clock - kf_clock;
            const bool should_seed = (std::abs(delta) >= kJumpThresh)
                                     || gap_detected || first_epoch_;
            if (should_seed && sv.size() > static_cast<Eigen::Index>(clk_idx)) {
                Eigen::VectorXd sv2 = sv;
                sv2(clk_idx) = wls_clock;
                state_manager_->setStateVector(sv2);
                ppp_state_.clock_bias = wls_clock;
                log->info("{}Seeded clock: wls={:.1f} kf={:.1f} d={:.1f}",
                          epoch_prefix, wls_clock, kf_clock, delta);
            }
        }
    }
}

/**
 * @brief BSSD code/phase re-gauge: remove the bulk receiver clock. Self-gated on !hasClock() -- mutually exclusive with seedClockFromWls().
 *
 * Relocated verbatim from the pre-cut RealTimeEngine::processEpoch
 * (frozen lines 1300-1333); see scripts/rt_seam_verify.py.
 */
void RealTimeEngine::regaugeBssd(RtEpochContext& ctx) {
    auto log = log_manager_.getLogger("rt_engine");
    const std::string& epoch_prefix = ctx.epoch_prefix;
    std::vector<PppObservation>& modeled_obs = ctx.modeled_obs;

    if (config_.enableBetweenSatelliteSd() && !state_manager_->hasClock()) {
        std::vector<double> code_innov;
        code_innov.reserve(modeled_obs.size());
        for (const auto& o : modeled_obs) {
            for (const auto& [code, val] : o.observed) {
                if (code.empty() || code[0] != 'C') continue;      // code only
                const auto mit = o.modeled.find(code);
                if (mit == o.modeled.end()) continue;
                const double innov = val - mit->second;
                if (std::isfinite(innov)) code_innov.push_back(innov);
            }
        }
        if (!code_innov.empty()) {
            const size_t mid = code_innov.size() / 2;
            std::nth_element(code_innov.begin(), code_innov.begin() + mid, code_innov.end());
            const double common_clock_m = code_innov[mid];
            if (std::isfinite(common_clock_m)) {
                size_t n_reduced = 0;
                for (auto& o : modeled_obs) {
                    for (auto& kv : o.observed) {
                        const std::string& code = kv.first;
                        if (code.empty()) continue;
                        const char c0 = code[0];
                        if (c0 != 'C' && c0 != 'L') continue;      // code + phase
                        kv.second -= common_clock_m;               // common-mode: SD-invariant
                        ++n_reduced;
                    }
                }
                log->debug("{}BSSD re-gauge: removed common rx clock {:.3f} m "
                           "(median of {} code innovations) from {} observables",
                           epoch_prefix, common_clock_m, code_innov.size(), n_reduced);
            }
        }
    }
}

} // namespace GnssProcessor
