/**
 * @file NetworkPhaseDatum.cpp
 * @brief Prioritized-datum spectrum-based drop-and-heal (see the header).
 *
 * Decomposition INC-8 (2026-07-17) — body relocated VERBATIM from
 * ApplicationNetwork.cpp; former lambda captures are explicit parameters
 * (capture→param map in the INC-8 cut spec).
 */

#include "filter/NetworkPhaseDatum.hpp"

#include "filter/StateKeySchema.hpp"    // StateKeys::parseSatPhaseBiasKey

#include <algorithm>
#include <map>
#include <utility>

namespace GnssProcessor {

int healNearNullPhaseBias(NetworkEstimator& est,
                          const std::set<std::string>& shared_phase_keys,
                          bool phase_datum_prioritized,
                          bool within_epoch_armed,
                          std::vector<NetworkEstimator::SharedPrior>& cap_priors) {
    if (!phase_datum_prioritized || est.numShared() == 0 || shared_phase_keys.empty())
        return 0;
    const Eigen::MatrixXd& Nss = est.reducedSharedNormals();
    Eigen::SelfAdjointEigenSolver<Eigen::MatrixXd> es(Nss, Eigen::ComputeEigenvectors);
    if (es.info() != Eigen::Success) return 0;
    const Eigen::VectorXd& ev = es.eigenvalues();   // ascending
    const double smax = ev.size() ? ev(ev.size() - 1) : 0.0;
    const double cutoff = NetworkEstimator::kNullSpaceTol * std::max(smax, 1.0);
    std::map<int, std::pair<std::string, std::string>> pbcol;
    for (const auto& pk : shared_phase_keys) {
        const auto p = StateKeys::parseSatPhaseBiasKey(pk);
        if (p) pbcol[static_cast<int>(
            est.phaseBiasColumn(p->satellite, p->signal))] = {p->satellite, p->signal};
    }
    // Every phase-bias column meaningfully spanning a near-null direction
    // (|component| > 0.3) is weakly/un-observable → heal it.  A clock-gauge
    // direction has ~zero phase-bias weight, so its minor PB participants
    // stay below 0.3 and are left alone.
    constexpr double kSpanFrac = 0.3;
    std::set<int> heal_cols;
    for (Eigen::Index j = 0; j < ev.size(); ++j) {
        if (ev(j) >= cutoff) continue;                 // observable direction
        const Eigen::VectorXd v = es.eigenvectors().col(j).cwiseAbs();
        for (const auto& [c, ss] : pbcol)
            if (c < v.size() && v(c) > kSpanFrac) heal_cols.insert(c);
    }
    if (heal_cols.empty()) return 0;
    std::vector<NetworkEstimator::SharedPrior> hp;
    hp.reserve(heal_cols.size());
    for (int c : heal_cols) {
        NetworkEstimator::SharedPrior p;
        p.kind   = SharedParamKind::PhaseBias;
        p.sat    = pbcol[c].first;
        p.signal = pbcol[c].second;
        p.value  = 0.0;
        p.info   = 1.0;   // σ = 1 m
        hp.push_back(p);
    }
    est.addSharedPrior(hp);
    if (within_epoch_armed)
        cap_priors.insert(cap_priors.end(), hp.begin(), hp.end());
    return static_cast<int>(hp.size());
}

} // namespace GnssProcessor
