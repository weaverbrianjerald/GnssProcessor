#pragma once
/**
 * @file PassASlipScan.hpp
 * @brief The PASS A slip scan of one network epoch as named stages: the parallel station
 *        scan with its MW and IF-phase detectors, the serial folds, the second code and
 *        second phase bookkeeping, the shared raw observables, and the MW census.
 *
 * `Application::networkPassASlipScan_` (ApplicationNetworkAssembly.cpp) builds a
 * @ref PassAView once per epoch and calls the stages in order. A stage reads the view,
 * binds the names it uses at its head, and keeps the body of the scan it was cut from.
 * The serial response drains that follow are in PassASlipDrains.hpp.
 *
 * Threading: passAScanStations and the stages it calls (passAScanMw, passAMwDetectOnly,
 * passAMwAdmit, passAMwEvidence) run inside an OpenMP region and write only
 * index-addressed per-station slots. Every other stage is serial and station-ordered.
 */

#include "network/slip/SlipQc.hpp"
#include "network/slip/CommonBandScope.hpp"
#include "network/driver/Epoch.hpp"
#include "network/driver/RunTypes.hpp"
#include "network/diag/RunTraces.hpp"
#include "network/state/Entities.hpp"
#include "network/state/RunContext.hpp"
#include "slip/MwSlipTypes.hpp"

#include <functional>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace spdlog { class logger; }

namespace GnssProcessor {

class GnssConfig;
struct SyncedEpoch;

/// The admission predicate of the MW detect branch: whether a pair is in the configured scope.
using PassAMwPairOk = std::function<bool(const std::string&, const std::string&, const std::string&)>;

/**
 * @brief Handles on the run and the epoch that every PASS A stage reads.
 *
 * References and copies only; owns nothing. The members are the names the scan used as
 * locals, so a stage body keeps its original spelling after the bindings at its head.
 */
struct PassAView {
    const SyncedEpoch& se;                        ///< the synced epoch
    NetworkRunContext& ctx;                       ///< run context: knobs, counters, carry, entities
    NetworkRunTraces& traces;                   ///< trace streams and the event writer
    const GnssConfig& config_;                   ///< run configuration
    const std::shared_ptr<spdlog::logger>& log;  ///< the main log
    double dt;                                   ///< epoch step (s)
    double pl_now_s;                             ///< phase-leveling clock (s)
    bool carry_warm;                            ///< the carry is past its warm-up
    std::vector<NetStation>& stations;          ///< the station roster
    std::vector<std::vector<PppObservation>>& modeled;  ///< per-station modelled rows (written by the scan)
    std::vector<bool>& present;                  ///< station present this epoch (written by the fold)
    const std::set<std::string>& shared_phase_keys;  ///< legacy-datum shared phase-bias keys
    std::set<std::pair<int, std::string>>& carry_slipped_ambs;  ///< raw-detector slip funnel (written)
    SlipQcChannel& qc;                           ///< slip-QC channel (raw observables, history, counters)
    EpochEvents::Slip& slip_ev;                 ///< the epoch's slip events (written)
    int n_sta_total;                            ///< station count
    bool parallel_on;                           ///< the scan runs under OpenMP
    int n_threads;                              ///< OpenMP thread count
    bool phase_datum_prioritized;               ///< prioritized phase datum (no special pivot)
    bool carry_invariant;                        ///< invariant-carry vehicle
    bool trace_flag_rates;                       ///< flag-rate tier counters armed
    bool ar_suspect_armed;                       ///< the AR suspect-corner mask is armed
    net::ArcTable& arc_rows;                    ///< the leveling carry
    FlagRate& fr_if_slip;                       ///< IF-phase flag-rate counter
    const std::set<std::string>* cbadm_admit;     ///< run-latched admitted code keys (null when unarmed)
    int i_lo;                                   ///< first station of the scan (0 prioritized, else 1)
    PassABuffers& pa;                           ///< per-station buffers the scan fills and the drains read
    bool sid_screens;                           ///< the drop-reason writers are armed
    std::vector<std::map<std::string, DropReason>>& pa_drops;  ///< per-station drop reasons (armed only)
};

/**
 * @brief Per-station PASS A tail, parallel over stations when enabled.
 *        Models each station, allocates its shared states, and runs its MW and IF-phase
 *        slip detectors. Everything written is station-local or an index-addressed slot, so
 *        the loop is safe under OpenMP; present[] and exceptions are folded serially by the
 *        stages that follow.
 */
void passAScanStations(const PassAView& v);

/**
 * @brief Rolls one station's MW arcs forward and collects either TurboEdit integer repairs
 *        or, under mw_detect, the admitted refloat candidates. A detection failure costs only
 *        this station's MW action for the epoch.
 */
void passAScanMw(const PassAView& v, int i, NetStation& st);

/**
 * @brief The detect-only MW branch: fixes the admission knobs and the pair scope, admits
 *        the flagged pairs, then records the common-band evidence from the same detector.
 */
void passAMwDetectOnly(const PassAView& v, int i, NetStation& st,
                       const std::vector<MwSlipResult>& mwr);

/**
 * @brief Admits each MW flag of one station as a refloat candidate, or counts why it was
 *        not. Writes the station's index-addressed census and candidate slots only.
 */
void passAMwAdmit(const PassAView& v, int i, const std::vector<MwSlipResult>& mwr,
                  const PassAMwPairOk& mw_pair_ok, const double frac_max, const bool ev_on,
                  const double ev_ratio, const int ev_narc);

/**
 * @brief Collects one station's common-band scope evidence: every arc the detector
 *        processed, every flag, and the receiver's own loss-of-lock bits. Collected whether
 *        or not the scope is armed, so the census sizes the mechanism before it is.
 */
void passAMwEvidence(const PassAView& v, int i, NetStation& st,
                     const std::vector<MwSlipResult>& mwr, const PassAMwPairOk& mw_pair_ok);

/**
 * @brief Commits present[] from the scan's per-station flags and logs a station whose
 *        geometry threw, in station order.
 */
void passAFoldPresent(const PassAView& v);

/**
 * @brief Folds the per-station drop reasons (elevation mask, no model) into the arc
 *        ledger, in station order. A station that threw is left out: its whole epoch is
 *        missing, which is an outage and not a per-satellite screen.
 */
void passAFoldDropReasons(const PassAView& v);

/**
 * @brief Second-code-channel bookkeeping for the epoch: republishes each station's
 *        companion cell set for PASS B, counts the cells, both cell edges and the receiver
 *        inter-signal states. Serial and station-ordered; skipped entirely when off.
 */
void passASecondCode(const PassAView& v);

/**
 * @brief Second-phase-channel bookkeeping for the epoch: republishes each station's
 *        second-phase cell set for PASS B, counts cells, edges and arcs, and advances the
 *        between-channel witness. Serial and station-ordered; skipped entirely when off.
 */
void passASecondPhase(const PassAView& v);

/**
 * @brief Per-observation part of the second-phase bookkeeping: counts the fallback
 *        occurrences against the station's latched phase and advances the epoch-to-epoch
 *        witness of (latched minus second) phase, per band.
 */
void passASecondPhaseWitness(const PassAView& v, int i, NetStation& st, const PppObservation& ob,
                             NetworkServiceCounters::SecondPhase& C,
                             const std::vector<std::string>& allow, const long long now_ep);

/**
 * @brief Computes the shared raw slip observables once per epoch, after present[] is final
 *        and before the drains and the observability pre-filter, then qualifies them and
 *        advances the CS-GF authority. Skipped when no consumer asked for them.
 */
void passARawObservables(const PassAView& v);

/**
 * @brief One CS-GF level-authority recursion step per dual-band line of sight from this
 *        epoch's qualified verdicts. Serial and station-ordered, so deterministic.
 */
void passAIonoCsAdvance(const PassAView& v);

/**
 * @brief MW detection for the legacy datum station, which the parallel scan skips.
 *        Detector-mode candidates only; the pivot's ambiguities are gauge-pinned, so no
 *        integer repair applies. Does nothing unless datum_detect_on.
 */
void passADatumStationMwDetect(const PassAView& v, const bool datum_detect_on);

/**
 * @brief Reduces the per-station MW admission slots into the run counters, in station
 *        order, and stamps the AR search's suspect corners. Runs unconditionally: these
 *        counters are the mechanism's knob-independent telemetry.
 */
void passAFoldMwAdmission(const PassAView& v);

/**
 * @brief Resume slip warm-up: for the first epochs after a warm resume the MW/LLI drain
 *        runs diagnostic-only, so mw_carry_warm is cleared and the retirement is logged.
 */
void passAResumeSlipWarmup(const PassAView& v, bool& mw_carry_warm);

/**
 * @brief Forms the common-band scope verdict for the epoch, before either drain, and adds
 *        its tallies to the run counters. The verdict is formed armed or not.
 */
void passACommonBandVerdict(const PassAView& v, CommonBandEpoch& common_band);

} // namespace GnssProcessor
