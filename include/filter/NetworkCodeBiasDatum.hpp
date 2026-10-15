#pragma once
/**
 * @file NetworkCodeBiasDatum.hpp
 * @brief Stage 2 of the network solve (card NOM3 step SG): the W6 satellite code-bias datum.
 *        The per-epoch clears of the datum scratch, the two data rosters (RF-0f, B5D), the rows
 *        NetworkSbasis::buildCodeBiasDatumRows builds, their fold, telemetry and replay capture,
 *        the card GFC first-sighting family prior, the card KP prior and the first-N summary.
 *
 * The row builder's Doxygen holds the datum design (IF pin and per-(constellation, signal)
 * sum-to-zero, card-2 time-linking); this file holds the epoch's use of it.
 */

#include "filter/NetworkSbasis.hpp"     // NetworkSbasis::CodeBiasDatumRows
#include "filter/NetworkRunTypes.hpp"   // NetStation, EpochWorkspace

#include <set>
#include <string>
#include <vector>

namespace GnssProcessor {

class GnssConfig;
struct NetworkRun;
struct NetworkEpoch;

/**
 * @brief Stage 2: the W6 satellite code-bias datum.  Clears this epoch's datum scratch (even
 *        when no code-bias key is registered), builds and folds the rows, records their
 *        telemetry, folds the GFC birth rows and the KP prior, and captures all of them for the
 *        within-epoch replay.
 * @param run The run: the estimator is folded; ctx carry (the this_epoch_* hand-offs and the
 *            builder's carry maps) and ctx counters are written.
 * @param ep  The epoch, read: pass_a (present), columns (registered_code_keys, n_cb).
 * @param ws  [in/out] The replay capture (cap_H, cap_z, cap_row_kinds, cap_birth_*,
 *            cap_priors).
 */
void networkCodeBiasDatum(NetworkRun& run, const NetworkEpoch& ep, EpochWorkspace& ws);

/**
 * @brief The (satellite, signal) inter-signal corrections some present station's data determine
 *        this epoch (RF-0f): a satellite's signal is bridged when one present station carries it
 *        and a same-band companion in its own code-bias state map.
 * @param stations The network's stations (read: sm).
 * @param present  Which stations are present this epoch (read).
 * @param config_  The configuration (read: network.sat_isc_state).
 * @return Keys "sat|signal"; empty unless network.sat_isc_state.
 */
std::set<std::string> networkIscBridgedRoster(const std::vector<NetStation>& stations,
                                              const std::vector<bool>& present,
                                              const GnssConfig& config_);

/**
 * @brief The (satellite, extra signal, pin pair) code-bias columns the data determine this
 *        epoch (B5D): one present station carrying code on both pin bands and on the extra band.
 * @param stations The network's stations (read: sm).
 * @param present  Which stations are present this epoch (read).
 * @param config_  The configuration (read: network.sat_gf_extra_band_free).
 * @return Keys "sat|signal|b1b2"; empty unless network.sat_gf_extra_band_free.
 */
std::set<std::string> networkExtraBandRoster(const std::vector<NetStation>& stations,
                                             const std::vector<bool>& present,
                                             const GnssConfig& config_);

/**
 * @brief The on-path telemetry of the folded W6 rows (RF-0b, RF-0c, card GAL), the held
 *        extra-band keys (B5D) and the emit site's per-key ISC verdicts (RF-0f).
 * @param run         The run: ctx counters (n_w6_*, n_sat_gf_*, n_isc_*, n_extra_band_*,
 *                    n_s2z_reseed_*, prm_resp) and ctx carry (this_epoch_extra_band_held,
 *                    this_epoch_isc_obs, this_epoch_isc_pinned) are written.
 * @param ep          The epoch, read: columns (registered_code_keys).
 * @param cbd         The rows this epoch folded (read).
 * @param isc_bridged The bridging roster (read).
 */
void networkCodeBiasDatumRecord(NetworkRun& run, const NetworkEpoch& ep,
                                const NetworkSbasis::CodeBiasDatumRows& cbd,
                                const std::set<std::string>& isc_bridged);

/**
 * @brief The first-N W6 summary line, gated on the estimability budget before the dump spends it.
 * @param run The run, read: the log, the estimator, the budget.
 * @param ep  The epoch, read: columns (n_cb).
 * @param cbd The rows this epoch folded (read).
 */
void networkCodeBiasDatumFirstNLog(NetworkRun& run, const NetworkEpoch& ep,
                                   const NetworkSbasis::CodeBiasDatumRows& cbd);

} // namespace GnssProcessor
