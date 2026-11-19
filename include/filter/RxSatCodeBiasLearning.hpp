#pragma once

/**
 * @file RxSatCodeBiasLearning.hpp
 * @brief When a receiver-satellite code bias counts as learned, and the store a run leaves.
 *
 * The family's states share a free common part: the same shift of every state of one
 * constellation and signal is the receiver code bias of that signal, which the rest of the model
 * owns and the data cannot separate from it.  A single state therefore says nothing by itself.
 * What the data can know is a state against the others of its group (one constellation, one
 * code signal): the deviation of the state from the mean of the group.  Everything here reads
 * the filter covariance through that one quantity.
 *
 * ### The quadratic form
 *
 * For a state i and a set S of n states, the deviation is x_i minus the mean of the x_k over S,
 * the vector v = e_i - (1/n) 1_S, and its variance is v' P v
 * (@ref deviationVariance).  A common shift of all states leaves it unchanged, and so does any
 * variance of the common part in P.
 *
 * ### Learned
 *
 * A state is LEARNED when the variance of its deviation from the mean of all states of its
 * group is at or below (0.5 * prior sigma)^2, and the group has at least two states
 * (@ref isLearnedDeviation, @ref learnedKeys).  The test needs no reference value: an untouched
 * state has the full prior variance and fails it; a state whose differences to the others are
 * well determined passes it however large the free common part is.
 *
 * ### The store a run leaves
 *
 * Per group, the learned states are written (@ref buildRxSatCodeBiasStore): the sigma is the
 * standard deviation of the state's deviation from the mean of the learned set (again a
 * @ref deviationVariance).  A group with fewer than two learned states writes nothing, since the
 * deviation from the mean of one state is zero.  Entries of the previous store that the run never
 * saw are carried over with the value they had.  The values are then re-centred: one shift per
 * group, the mean over every entry of the group in the file being written (written now and
 * carried over), is subtracted from all of them, so the file holds zero mean per group and a
 * written entry keeps its distance to a carried-over one.
 *
 * Plain value types and free functions over a covariance matrix; nothing here owns state.
 */

#include "filter/RxSatCodeBiasStore.hpp"
#include "filter/StateManager.hpp"

#include <Eigen/Dense>

#include <cstddef>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace GnssProcessor {

/**
 * @brief The variance of a state minus the mean of a set of states: v' P v with
 *        v = e_i - (1/n) 1_set.
 *
 * Evaluated without forming v: P_ii - (2/n) sum_k P_ik + (1/n^2) sum_kl P_kl over the set.
 * State @p i may or may not belong to the set.  An empty set gives P_ii.
 *
 * @param P   The covariance matrix.
 * @param i   The state.
 * @param set The states whose mean is subtracted (indices into @p P).
 * @return The variance (m²); not clamped (round-off can leave a tiny negative value).
 */
double deviationVariance(const Eigen::MatrixXd& P, Eigen::Index i,
                         const std::vector<Eigen::Index>& set);

/**
 * @brief The learned test on one deviation variance.
 * @param deviation_variance_m2 The variance of the state minus the group mean (m²).
 * @param prior_sigma_m         The family's prior sigma, @c filter.initial_std.rx_sat_code_bias (m).
 * @return True when the variance is at or below (0.5 * prior sigma)².
 */
bool isLearnedDeviation(double deviation_variance_m2, double prior_sigma_m);

/**
 * @brief The states of the family, grouped by constellation and code signal.
 *
 * The group key is the text "<constellation letter>|<code signal>" ("G|C1W"); a state key that
 * is no receiver-satellite code-bias key is left out.  Each group lists its state keys in key order.
 *
 * @param states The family's key-to-index map.
 * @return Group key to the (state key, index) pairs of the group.
 */
std::map<std::string, std::vector<std::pair<std::string, Eigen::Index>>>
rxSatCodeBiasGroups(const std::map<std::string, std::size_t>& states);

/**
 * @brief The keys of the LEARNED states.
 *
 * For every group of at least two states, a state is learned when @ref isLearnedDeviation holds
 * for its @ref deviationVariance against the mean of all states of the group.  A group of one
 * state has no learned state.
 *
 * @param P              The filter covariance.
 * @param states         The family's key-to-index map.
 * @param prior_sigma_m  The family's prior sigma (m).
 * @return The learned keys.
 */
std::set<std::string> learnedKeys(const Eigen::MatrixXd& P,
                                  const std::map<std::string, std::size_t>& states,
                                  double prior_sigma_m);

/**
 * @brief The per-epoch check of the learned test: marks the states that pass it for the first time.
 *
 * Called once per epoch after the measurement update.  For each state that passes the test and
 * has no mark, @c StateManager::markRxSatCodeBiasLearned counts and logs the event with the
 * satellite and the signal.  A state that failed after a reset gets its mark again when it
 * passes again.  Does nothing when the family is empty.
 *
 * @param states        The state manager.
 * @param prior_sigma_m The family's prior sigma (m).
 * @return The number of states newly marked.
 */
std::size_t noteLearnedRxSatCodeBiases(StateManager& states, double prior_sigma_m);

/**
 * @brief The counts of one @ref buildRxSatCodeBiasStore.
 */
struct RxSatCodeBiasBuildCounts {
    std::size_t written{ 0 };        ///< Entries made from learned states of the run.
    std::size_t carried_over{ 0 };   ///< Entries of the previous store the run never saw, copied unchanged.
};

/**
 * @brief The store a run leaves, from its final state vector and covariance.
 *
 * Per group with at least two learned states (see the file banner): an entry for each learned
 * state with sigma = the standard deviation of its deviation from the mean of the learned set
 * (at least @ref kRxSatCodeBiasMinStoredSigmaM) and the epoch of the end of the run.  Every entry
 * of @p previous whose key is not a state of the run is carried over with its sigma and epoch; an
 * entry whose key is a state of the run but which is not written (not learned) is dropped.
 *
 * The values: for each group that has an entry written by the run, one shift, the mean of the
 * values over ALL entries of that group in the file (the states' values x_i and the carried-over
 * values), is subtracted from every one of them.  The written values are therefore x_i minus that
 * mean, the carried-over ones their old value minus it, and the difference between a written and
 * a carried-over entry is unchanged by the re-centring.  A group without a written entry keeps
 * its carried-over values as they are.
 *
 * @param x             The final state vector.
 * @param P             The final covariance.
 * @param states        The family's key-to-index map.
 * @param prior_sigma_m The family's prior sigma (m).
 * @param previous      The store the run started from (usable one), or null for none.
 * @param identity      The identity the new store belongs to.
 * @param end_time      The epoch of the last update.
 * @param counts        Out, optional: the counts of written and carried-over entries.
 * @return The store.
 */
RxSatCodeBiasStore buildRxSatCodeBiasStore(const Eigen::VectorXd& x, const Eigen::MatrixXd& P,
                                           const std::map<std::string, std::size_t>& states,
                                           double prior_sigma_m,
                                           const RxSatCodeBiasStore* previous,
                                           const RxSatCodeBiasIdentity& identity,
                                           const GnssTime& end_time,
                                           RxSatCodeBiasBuildCounts* counts = nullptr);

/**
 * @brief The run-end line of the family.
 *
 * "receiver-satellite code bias: states=N learned_now=L | born=B (from the store S) learned=E
 * reset=R (satellites M) written=W carried_over=C store_ignored=I".
 *
 * @param counters    The run's counters.
 * @param n_states    The number of states the family holds at the end.
 * @param learned_now The number of states that pass the learned test at the end.
 * @return The line.
 */
std::string rxSatCodeBiasRunEndLine(const StateManager::RxSatCodeBiasCounters& counters,
                                    std::size_t n_states, std::size_t learned_now);

} // namespace GnssProcessor
