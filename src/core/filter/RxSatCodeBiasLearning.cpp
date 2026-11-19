/**
 * @file RxSatCodeBiasLearning.cpp
 * @brief The learned test, the per-epoch check and the store a run leaves (see
 *        filter/RxSatCodeBiasLearning.hpp).
 */

#include "filter/RxSatCodeBiasLearning.hpp"

#include "common/GnssIdentity.hpp"
#include "filter/StateKeySchema.hpp"

#include <algorithm>
#include <cmath>
#include <fmt/format.h>

namespace GnssProcessor {

namespace {

/// The key of the group a state belongs to ("G|C1W"), or empty for a key that is none of the family's.
std::string groupKeyOf(const std::string& state_key) {
    const auto parts = StateKeys::parseRxSatCodeBiasKey(state_key);
    if (!parts) return std::string();
    return std::string(1, letterOfSat(parts->satellite)) + "|" + parts->signal;
}

/// The indices of a group's states.
std::vector<Eigen::Index> indicesOf(const std::vector<std::pair<std::string, Eigen::Index>>& group) {
    std::vector<Eigen::Index> out;
    out.reserve(group.size());
    for (const auto& member : group) out.push_back(member.second);
    return out;
}

} // namespace

double deviationVariance(const Eigen::MatrixXd& P, Eigen::Index i,
                         const std::vector<Eigen::Index>& set) {
    if (set.empty()) return P(i, i);
    const double n = static_cast<double>(set.size());
    double row_sum = 0.0;
    double block_sum = 0.0;
    for (const Eigen::Index k : set) {
        row_sum += P(i, k);
        for (const Eigen::Index l : set) block_sum += P(k, l);
    }
    return P(i, i) - 2.0 * row_sum / n + block_sum / (n * n);
}

bool isLearnedDeviation(double deviation_variance_m2, double prior_sigma_m) {
    const double bound = 0.5 * prior_sigma_m;
    return deviation_variance_m2 <= bound * bound;
}

std::map<std::string, std::vector<std::pair<std::string, Eigen::Index>>>
rxSatCodeBiasGroups(const std::map<std::string, std::size_t>& states) {
    std::map<std::string, std::vector<std::pair<std::string, Eigen::Index>>> groups;
    for (const auto& [key, index] : states) {
        const std::string group = groupKeyOf(key);
        if (group.empty()) continue;
        groups[group].emplace_back(key, static_cast<Eigen::Index>(index));
    }
    return groups;
}

std::set<std::string> learnedKeys(const Eigen::MatrixXd& P,
                                  const std::map<std::string, std::size_t>& states,
                                  double prior_sigma_m) {
    std::set<std::string> learned;
    for (const auto& [group_key, group] : rxSatCodeBiasGroups(states)) {
        if (group.size() < 2) continue;
        const std::vector<Eigen::Index> all = indicesOf(group);
        for (const auto& [key, index] : group)
            if (isLearnedDeviation(deviationVariance(P, index, all), prior_sigma_m))
                learned.insert(key);
    }
    return learned;
}

std::size_t noteLearnedRxSatCodeBiases(StateManager& states, double prior_sigma_m) {
    const auto& family = states.getRxSatCodeBiasStates();
    if (family.empty()) return 0;
    const Eigen::MatrixXd P = states.getCovariance();
    std::size_t n_new = 0;
    for (const std::string& key : learnedKeys(P, family, prior_sigma_m))
        if (states.markRxSatCodeBiasLearned(key)) ++n_new;
    return n_new;
}

RxSatCodeBiasStore buildRxSatCodeBiasStore(const Eigen::VectorXd& x, const Eigen::MatrixXd& P,
                                           const std::map<std::string, std::size_t>& states,
                                           double prior_sigma_m,
                                           const RxSatCodeBiasStore* previous,
                                           const RxSatCodeBiasIdentity& identity,
                                           const GnssTime& end_time,
                                           RxSatCodeBiasBuildCounts* counts) {
    RxSatCodeBiasStore store;
    store.identity = identity;
    RxSatCodeBiasBuildCounts n;

    // The entries of this run, at the value of the state (before any re-centring), and the
    // groups that have any.  A group with fewer than two learned states writes nothing.
    std::set<std::string> written_groups;
    const std::set<std::string> learned = learnedKeys(P, states, prior_sigma_m);
    for (const auto& [group_key, group] : rxSatCodeBiasGroups(states)) {
        std::vector<std::pair<std::string, Eigen::Index>> members;
        for (const auto& member : group)
            if (learned.count(member.first)) members.push_back(member);
        if (members.size() < 2) continue;   // the deviation from the mean of one state is zero

        const std::vector<Eigen::Index> set = indicesOf(members);
        for (const auto& [key, index] : members) {
            const auto parts = StateKeys::parseRxSatCodeBiasKey(key);
            RxSatCodeBiasEntry e;
            e.satellite = parts->satellite;
            e.signal    = parts->signal;
            e.value_m   = x(index);
            e.sigma_m   = std::max(std::sqrt(std::max(deviationVariance(P, index, set), 0.0)),
                                   kRxSatCodeBiasMinStoredSigmaM);
            e.updated   = end_time;
            store.entries[key] = e;
            ++n.written;
        }
        written_groups.insert(group_key);
    }

    // The entries the run never saw are carried over with the value they had.
    if (previous != nullptr) {
        for (const auto& [key, e] : previous->entries) {
            if (states.count(key)) continue;   // the run saw it: written above or dropped
            store.entries[key] = e;
            ++n.carried_over;
        }
    }

    // One shift per group, the mean over every entry of the group in this file (written and
    // carried over), so the zero-mean convention holds for the file and a written entry keeps
    // its distance to a carried-over one.
    for (const std::string& group_key : written_groups) {
        double sum = 0.0;
        std::size_t count = 0;
        for (const auto& [key, e] : store.entries)
            if (groupKeyOf(key) == group_key) { sum += e.value_m; ++count; }
        const double mean = sum / static_cast<double>(count);
        for (auto& [key, e] : store.entries)
            if (groupKeyOf(key) == group_key) e.value_m -= mean;
    }
    if (counts != nullptr) *counts = n;
    return store;
}

std::string rxSatCodeBiasRunEndLine(const StateManager::RxSatCodeBiasCounters& c,
                                    std::size_t n_states, std::size_t learned_now) {
    return fmt::format("receiver-satellite code bias: states={} learned_now={} | born={} (from the "
                       "store {}) learned={} reset={} (satellites {}) written={} carried_over={} "
                       "store_ignored={}",
                       n_states, learned_now, c.born, c.born_from_store, c.learned, c.reset,
                       c.reset_satellites, c.written, c.carried_over, c.store_ignored);
}

} // namespace GnssProcessor
