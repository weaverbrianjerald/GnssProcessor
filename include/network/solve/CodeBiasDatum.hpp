#pragma once
/**
 * @file CodeBiasDatum.hpp
 * @brief Stage 2 of the network solve: the W6 satellite code-bias datum.
 *        The per-epoch clears of the datum scratch, the two data rosters (RF-0f, B5D), the rows
 *        NetworkSbasis::buildCodeBiasDatumRows builds, their fold, telemetry and replay capture,
 *        the first-sighting family prior, the physical-bound prior and the first-N summary.
 *
 * The row builder's Doxygen holds the datum design (IF pin and per-(constellation, signal)
 * sum-to-zero, time-linking); this file holds the epoch's use of it.
 */

#include "network/solve/Sbasis.hpp"     // NetworkSbasis::CodeBiasDatumRows
#include "network/driver/RunTypes.hpp"   // NetStation, EpochWorkspace

#include <set>
#include <string>
#include <vector>
#include <map>
#include <cstddef>

namespace GnssProcessor {

struct EpochEvents;   // network/driver/Epoch.hpp

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
 * @param events The epoch's events; writes code_datum.
 */
void networkCodeBiasDatum(NetworkRun& run, const NetworkEpoch& ep, EpochWorkspace& ws, EpochEvents& events);

/**
 * @brief The (satellite, signal) inter-signal corrections some present station's data determine
 *        this epoch (RF-0f): a satellite's signal is bridged when one present station carries it
 *        and a same-band companion in its own code-bias state map.
 * @param stations The network's stations (read: params).
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
 * @param stations The network's stations (read: params).
 * @param present  Which stations are present this epoch (read).
 * @param config_  The configuration (read: network.sat_gf_extra_band_free).
 * @return Keys "sat|signal|b1b2"; empty unless network.sat_gf_extra_band_free.
 */
std::set<std::string> networkExtraBandRoster(const std::vector<NetStation>& stations,
                                             const std::vector<bool>& present,
                                             const GnssConfig& config_);

/**
 * @brief The on-path telemetry of the folded W6 rows (RF-0b, RF-0c), the held
 *        extra-band keys (B5D) and the emit site's per-key ISC verdicts (RF-0f).
 * @param run         The run: ctx counters (n_w6_*, n_sat_gf_*, n_isc_*, n_extra_band_*,
 *                    n_s2z_reseed_*, prm_resp) and ctx carry (this_epoch_extra_band_held,
 *                    this_epoch_isc_obs, this_epoch_isc_pinned) are written.
 * @param ep          The epoch, read: columns (registered_code_keys).
 * @param cbd         The rows this epoch folded (read).
 * @param isc_bridged The bridging roster (read).
 * @param events The epoch's events; writes code_datum (the realized pairs, the held extra-band keys).
 */
void networkCodeBiasDatumRecord(NetworkRun& run, const NetworkEpoch& ep,
                                const NetworkSbasis::CodeBiasDatumRows& cbd,
                                const std::set<std::string>& isc_bridged, EpochEvents& events);

/**
 * @brief The first-N W6 summary line, gated on the estimability budget before the dump spends it.
 * @param run The run, read: the log, the estimator, the budget.
 * @param ep  The epoch, read: columns (n_cb).
 * @param cbd The rows this epoch folded (read).
 */
void networkCodeBiasDatumFirstNLog(NetworkRun& run, const NetworkEpoch& ep,
                                   const NetworkSbasis::CodeBiasDatumRows& cbd);

/// @brief Run-scope counters of CodeBiasDatum, a base of NetworkServiceCounters (network/state/RunContext.hpp).
struct CodeBiasDatumCounters {
    // Epochs on which the satellite code-bias datum was folded with no ionosphere
    // constraint behind it (RF-0b, network.w6_iono_constraint = none), and the
    // cumulative count of sum-to-zero (constellation, signal) rows in those epochs,
    // which are the datum in that posture.  Standing invariant #4: an armed "none" run
    // with n_w6_iono_none_epochs == 0 never reached the W6 block at all (no registered
    // code columns), and one with n_w6_iono_none_s2z_rows == 0 folded the IF pin
    // against nothing; in both cases the satellite code biases have no datum at all,
    // which a clean-looking log would not reveal.
    size_t n_w6_iono_none_epochs = 0, n_w6_iono_none_s2z_rows = 0;
    size_t n_sat_gf_tgd = 0, n_sat_gf_tgd0 = 0, n_sat_gf_fallback = 0;
    // The per-signal extension (RF-0d).  `extra` counts the rows beyond the
    // ionosphere-free pair (which make a sparse channel's level a datum), `isc0` how
    // many of those asserted a zero inter-signal correction the navigation message
    // never stated, `sameband` the columns deliberately left estimable, and
    // `s2z_suppressed` the sum-to-zero rows dropped because the datum already pins
    // every column of that channel.
    size_t n_sat_gf_extra = 0, n_sat_gf_isc0 = 0;
    size_t n_sat_gf_sameband = 0, n_sat_gf_s2z_suppressed = 0;
    /// The RF-0c / RF-0d geometry-free datum counters split per constellation letter,
    /// accumulated from @c NetworkSbasis::CodeBiasDatumRows::sat_gf_by_const beside the
    /// global counters (R3 iii).  Read only by the Galileo run-end line.
    std::map<std::string, NetworkSbasis::SatGfConstCounts> sat_gf_by_const;
};

} // namespace GnssProcessor
