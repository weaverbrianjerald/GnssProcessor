/**
 * @file RealTimeEngineArObserve.cpp
 * @brief The engine side of the consumer's observe-mode integer search.
 *
 * The arithmetic (SD formation, the two searches, the conditioning, the rows) is
 * realtime/ArObserve; this file decides which ambiguity states may enter it, applies the
 * chain gate, hands the search a copy of the state, and keeps the outcome and the run
 * totals.  Nothing here writes to the filter, the state manager, the resolver or the SSR
 * store: with the mode armed and the AR mode 0 every pre-existing output is bit-identical.
 */

#include "realtime/RealTimeEngine.hpp"

#include "common/GnssIdentity.hpp"            // systemOfSat
#include "common/PppUtils.hpp"                // getSystemString
#include "filter/AmbiguityResolverSetup.hpp"  // makeAmbiguityResolverConfig
#include "filter/StateKeySchema.hpp"          // parseAmbiguityKey
#include "products/SsrCorrectionStore.hpp"

#include <algorithm>
#include <cmath>
#include <map>
#include <string>
#include <vector>

namespace GnssProcessor {

namespace {
/// The least 3D position sigma (m) the search waits for.  The resolver's own bar defaults to
/// 0.05 m, which a float with free slant ionosphere can take hours to reach, yet the loader
/// already caps that bar at 0.20 m for a direct per-band search because a looser float lets the
/// search run away.  The observer is held to the same ceiling, not to the tighter default.
constexpr double kPositionSigmaBarFloorM = 0.20;
} // namespace

void RealTimeEngine::observeIntegerSearch_(RtEpochContext& ctx) {
    if (!config_.arObserve()) return;   // off: nothing is read, allocated or written (byte-identical)

    // The epoch's buffer starts empty (the first admission seam also clears it, so an epoch
    // that never gets here leaves nothing behind either).
    ar_observe_epoch_ = ArObserveEpoch{};
    ar_observe_epoch_.time = ctx.current_time;
    if (!ctx.update_applied || !state_manager_ || !ppp_model_) return;

    auto log = log_manager_.getLogger("rt_engine");
    const AmbiguityResolverConfig ar = makeAmbiguityResolverConfig(config_);
    const bool gate = config_.networkRequireSignalIntegerForAr();
    const WireChainContext& wire = wire_chain_;

    if (!ar_observe_logged_) {
        ar_observe_logged_ = true;
        std::string systems;
        for (const auto& s : ar.ar_active_systems) systems += (systems.empty() ? "" : "+") + s;
        if (systems.empty()) systems = "all";
        log->info("[RT] AR observe armed: search '{}', determined bar {:.3f} cyc, gates ratio "
                  ">= {:.1f} (<= {:.1e}), bootstrap P_s >= {:.4f}, |float - fixed| <= {:.3f} cyc, "
                  "partial AR {}, elevation mask {:.1f} deg, position sigma bar {:.3f} m, "
                  "constellations {}; chain gate {} (wire amb_datum {}).  Measure-only: nothing is "
                  "written back to the filter.",
                  config_.arObserveSearch(), config_.arObserveDeterminedSigmaCyc(),
                  ar.ratio_threshold, ar.ratio_upper_bound, ar.min_success_rate,
                  ar.nl_max_per_pair_frac, ar.enable_partial_ar ? "on" : "off",
                  ar.elevation_mask_ar, std::max(ar.max_position_sigma_m, kPositionSigmaBarFloorM),
                  systems, gate ? "on" : "off", wire.amb_datum);
    }

    // --- The state the search reads (copies; nothing below writes to the manager) -------
    const Eigen::VectorXd x = state_manager_->getStateVector();
    const Eigen::MatrixXd P = state_manager_->getCovariance();
    std::map<std::string, double> sat_elev;
    for (const auto& obs : ctx.modeled_obs) sat_elev[obs.satellite] = obs.elevation;

    // --- Candidates: the ambiguity states the resolver would pair (the exclusions of
    //     AmbiguityResolver::buildDdPairs), without the iono-free keys and GLONASS ---------
    const SsrCorrectionStore& store = ssrStore();
    const bool   aged    = config_.enableSsrBiasMaxAge();
    const double max_age = config_.getSsrBiasMaxAge() * config_.getSsrBiasStaleHoldFactor();
    std::vector<ArCandidate> candidates;
    long long n_cand = 0, n_eligible = 0;
    for (const auto& [key, idx] : state_manager_->getAmbiguities()) {
        const auto parsed = StateKeys::parseAmbiguityKey(key);
        if (!parsed) continue;
        const std::string& sat    = parsed->satellite;
        const std::string& signal = parsed->signal;
        if (signal.find('_') != std::string::npos) continue;          // iono-free combination
        const System sys = systemOfSat(sat);
        if (sys == System::GLONASS) continue;                         // FDMA: not a common wavelength
        const Eigen::Index i = static_cast<Eigen::Index>(idx);
        if (i >= x.size() || i >= P.rows()) continue;
        if (x(i) == 0.0) continue;                                    // uninitialized seed
        if (P(i, i) == 0.0) continue;                                 // frozen pivot
        const auto eit = sat_elev.find(sat);
        const double elevation = (eit != sat_elev.end()) ? eit->second : 0.0;
        if (elevation < ar.elevation_mask_ar) continue;
        if (state_manager_->isAmbiguityUnresolvable(key)) continue;
        if (!ar.ar_active_systems.empty()) {
            const std::string name = PppUtils::getSystemString(sat);
            if (std::find(ar.ar_active_systems.begin(), ar.ar_active_systems.end(), name)
                == ar.ar_active_systems.end())
                continue;
        }
        const double lambda = ppp_model_->pppUtils().getWavelength(sys, signal, sat);
        if (!(std::isfinite(lambda) && lambda > 0.0)) continue;

        // The ambiguity reaches the gate.
        ++n_cand;
        ArCandidate c;
        c.sat           = sat;
        c.signal        = signal;
        c.state_idx     = static_cast<int>(i);
        c.elevation_deg = elevation;
        c.lambda_m      = lambda;
        const auto cit = wire.cmp.find(sat + "|" + signal);
        if (cit != wire.cmp.end()) c.cmp = cit->second;
        const std::optional<SsrPhaseBiasEntry> pb =
            aged ? store.getPhaseBias(sat, signal, ctx.current_time, max_age)
                 : store.getPhaseBias(sat, signal);
        if (pb) c.hold_generation = pb->hold_generation;
        // The chain gate: the satellite phase bias must be integer-grade, the wire must
        // declare the single-receiver chain datum, and the bias must name its component.
        if (gate && !(wire.amb_datum == kWireAmbDatumChain && c.cmp != 0
                      && pb && pb->signal_integer))
            continue;
        ++n_eligible;
        candidates.push_back(std::move(c));
    }

    // A gate that admits nothing because of what the wire declares is a property of the wire, not
    // of an epoch; say so once rather than leave a run of empty epochs unexplained.
    if (gate && n_cand > 0 && n_eligible == 0 && !ar_observe_gate_warned_
        && (wire.amb_datum != kWireAmbDatumChain || wire.cmp.empty())) {
        ar_observe_gate_warned_ = true;
        log->warn("[RT] AR observe: network.require_signal_integer_for_ar is on and none of the "
                  "{} ambiguity states is eligible: the wire declares amb_datum {} (the gate needs "
                  "{}) and {} phase-bias component(s) (cmp=) are known.  An untokenized or golden "
                  "wire admits nothing, by design.",
                  n_cand, wire.amb_datum, kWireAmbDatumChain, wire.cmp.size());
    }

    // --- The search ---------------------------------------------------------------------
    ArObserveOptions opt;
    opt.mode                      = arSearchModeOf(config_.arObserveSearch());
    opt.gates.min_success_rate     = ar.min_success_rate;
    opt.gates.ratio_threshold      = ar.ratio_threshold;
    opt.gates.ratio_upper_bound    = ar.ratio_upper_bound;
    opt.gates.nl_max_per_pair_frac = ar.nl_max_per_pair_frac;
    opt.gates.enable_partial_ar    = ar.enable_partial_ar;
    opt.determined_sigma_cyc       = config_.arObserveDeterminedSigmaCyc();
    opt.split_by_component         = gate;
    opt.position_index             = static_cast<int>(state_manager_->getPositionIndex());
    opt.max_position_sigma_m       = std::max(ar.max_position_sigma_m, kPositionSigmaBarFloorM);
    const GnssTime stamp = ctx.current_time;
    ar_observe_epoch_ = observeIntegerSearch(x, P, candidates, opt);
    ar_observe_epoch_.time = stamp;
    accumulateArObserve(ar_observe_counters_, ar_observe_epoch_, n_cand, n_eligible);
}

void RealTimeEngine::logArObserveSummary(const char* tag) const {
    if (!config_.arObserve()) return;   // off: never printed (byte-identical)
    log_manager_.getLogger("rt_engine")->info(
        "{}", formatArObserveSummary(ar_observe_counters_, tag, config_.arObserveSearch(),
                                     config_.arObserveDeterminedSigmaCyc(),
                                     config_.networkRequireSignalIntegerForAr()));
}

} // namespace GnssProcessor
