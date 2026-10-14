#pragma once

/**
 * @file CycleSlipEpochWork.hpp
 * @brief The working value one @c CycleSlipManager::detectAndRepair call
 *        threads through its sections.
 *
 * @c detectAndRepair is a short ordered driver over private section members
 * (the table in CycleSlipManager.hpp).  The sets those sections hand to one
 * another were function-scope locals of a single 1,200-line function; they are
 * now the members of @c SlipEpochWork, which lives for one call.  Each section
 * opens with an alias preamble (for example @c const auto& te = w.te;) naming
 * exactly the members it reads (@c const) and writes, so its body reads the
 * names it always had.
 *
 * Internal to the slip layer: included by CycleSlipManager.cpp and the
 * CycleSlip*.cpp section files only.
 */

#include "slip/CycleSlipManager.hpp"
#include <map>
#include <memory>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace GnssProcessor {

/**
 * @brief One pivot-affected culprit whose integer repair waits until the
 *        pivot frame is settled: in place (S12) or after a forced handover
 *        (S13).  Queued by the per-satellite adjudication (S11).
 */
struct DeferredIntegerRepair {
    std::string sat_id;             ///< Satellite, e.g. "G05".
    std::string signal;             ///< Culprit signal, e.g. "L2W".
    int         dN_carrier_cycles;  ///< Integer slip in this signal's cycles.
};

/**
 * @brief Per-epoch working value of @c CycleSlipManager::detectAndRepair.
 *
 * Built by the driver from its four arguments and the module logger, filled and
 * read by the sections in order (S0 to S17, the section list of card UOM's
 * organization map), discarded when the call returns.  It owns nothing that
 * outlives the epoch: what must persist lives in the manager (@c last_mw_slips_,
 * @c epoch_reset_sats_, @c epoch_repairs_, the soft-LLI bridge, the detectors).
 * The members are grouped by the section that first writes them.
 */
struct SlipEpochWork {
    /**
     * @brief Bind the call's inputs.
     * @param epoch_prefix_in  Log prefix of this epoch.
     * @param modeled_obs_in   Modeled observations of this epoch.
     * @param state_manager_in Filter state (written by repairs and resets).
     * @param meas_model_in    Measurement model: the BSSD pivot map; its
     *                         @c PivotManager is written through a
     *                         @c const_cast by S13.
     * @param log_in           The "cycle_slip_manager" module logger.
     */
    SlipEpochWork(const std::string& epoch_prefix_in,
                  const std::vector<PppObservation>& modeled_obs_in,
                  StateManager& state_manager_in,
                  const MeasurementModel& meas_model_in,
                  std::shared_ptr<spdlog::logger> log_in)
        : epoch_prefix(epoch_prefix_in),
          modeled_obs(modeled_obs_in),
          state_manager(state_manager_in),
          meas_model(meas_model_in),
          log(std::move(log_in)) {}

    // --- The call's inputs ----------------------------------------------------
    const std::string& epoch_prefix;                 ///< Log prefix.
    const std::vector<PppObservation>& modeled_obs;  ///< Modeled observations.
    StateManager& state_manager;                     ///< Filter state.
    const MeasurementModel& meas_model;              ///< BSSD pivot source.
    std::shared_ptr<spdlog::logger> log;             ///< "cycle_slip_manager" logger.

    // --- S0: the epoch frame --------------------------------------------------
    /// The epoch's result: counters and state_modified, written by every
    /// section; S17 copies the smoother accumulators in and returns it.
    CycleSlipEpochResult result;
    /// Continuous GPS seconds of this epoch (for soft-LLI hold bookkeeping);
    /// 0 when no observation carries a satellite.
    double epoch_now_s{ 0.0 };
    /// BSSD pivot predicate, shared by the gap-hold pre-pass (S1) and the
    /// WL-settle pending-hold registration (S5).
    SoftLliBridge::IsBssdPivotFn is_bssd_pivot;

    // --- S2: auxiliary detectors ----------------------------------------------
    /// GRAPHIC / EWL slip events, applied as BSSD-aware resets by S16.
    std::vector<SlipEvent> aux_events;

    // --- S3: working sets of the MW, TurboEdit and soft-LLI paths --------------
    /// Satellites already reset or repaired this epoch; S15 and S16 skip them.
    std::set<std::string> reset_sats;
    // Satellites whose soft-LLI-held ambiguities were integer-repaired by the
    // bridge this epoch (snapshot+dN·λ restore in the pre-pass below).  The MW
    // per-sat reset loop skips these, since their slip is already handled.
    std::set<std::string> soft_lli_repaired;

    // TurboEdit (MW+GF joint repair) resolution for this epoch.  A member of
    // the epoch work because it is consumed in two places: the MW per-satellite
    // loop (where it supplies per-signal integers for the dual-frequency case
    // cross-consistency cannot split) and the GF-only pass after the loop (the
    // MW dN1==dN2 blind spot).  resolve() also rolls the GF jump arcs forward,
    // so it runs every epoch, including epochs with no MW slips.
    TurboEditResolver::EpochResolution te;
    std::map<std::string, std::set<std::string>>  te_culprits_by_sat; // sat -> culprit signals
    std::map<std::string, std::map<std::string, int>> te_slips_by_sat; // sat -> signal -> dN
    // sat -> signals the resolver adjudicated as needing no state action.  Three
    // sources share one caller behaviour (suppress the MW mandate, touch
    // nothing):
    //  - code-event signals (MW flag with an affirmatively stable GF; the
    //    event is in the code observable);
    //  - gap-bridge moot signals (the across-gap slip is already absorbed by
    //    the prune-and-reseed on the signal's return);
    //  - clean_signals (dN == 0 from a confident joint solve; the documented
    //    contract is that converged precision is kept.  For repair satellites
    //    the culprit branch consumes them first, so they reach this set only
    //    for clean-without-repair adjudications, such as the clean partner of
    //    a bridged moot pair).
    // Disjoint from te.reset_sats by resolver contract (conflicting
    // evidence withdraws the adjudications).
    std::map<std::string, std::set<std::string>> te_suppress_by_sat;

    // --- S4: MW / LLI detection -----------------------------------------------
    /// The MW detector's results: one entry per mandated ambiguity key (an LLI
    /// entry has empty @c sig1 / @c sig2).  Empty when the MW detector is off.
    std::vector<MwSlipResult> slip_results;

    // --- S7: MW results grouped per satellite ---------------------------------
    /// Slipped pairs per satellite, deduplicated by (sig1, sig2).
    std::map<std::string, std::vector<MwSlipResult>> slips_by_sat;
    /// Slipped and clean pairs per satellite (clean ones synthesized from the
    /// detector's mature arcs with a zero slip), deduplicated by (sig1, sig2).
    std::map<std::string, std::vector<MwSlipResult>> all_pairs_by_sat;

    // --- S8: mandates (only when the MW detector reported anything) -----------
    /// Every distinct mandated ambiguity key, grouped by satellite.
    std::map<std::string, std::set<std::string>> reset_keys_by_sat;
    // Satellites carrying a receiver-declared loss of lock this
    // epoch (signal-less reset entries).  LLI is authoritative:
    // integer continuity is broken arbitrarily, so culprit repairs
    // do not override the mandated resets.  This applies in
    // particular to TurboEdit's GF-only resolution, whose equal-slip
    // premise cannot be checked when the LLI branch suppressed all
    // MW pair evidence.  The real-time engine also treats LLI
    // entries as unconditional resets.
    std::set<std::string> lli_sats;
    // Ambiguity keys reset during this epoch's slip phase (full
    // resets, targeted culprit resets, residual resets).  A reset
    // reseeds from the current observations, in the post-slip frame,
    // so the clean-peer pass of the in-place pivot handling below
    // does not apply an additional -dN_pivot shift to such a key (the
    // fresh seed is already consistent with the slipped pivot's
    // carrier).
    std::set<std::string> epoch_reset_amb_keys;

    // --- S9: pivot culprits ---------------------------------------------------
    /// (constellation name, signal) pairs whose BSSD pivot is itself a
    /// culprit: every satellite's repair on such a signal is deferred.
    std::set<std::pair<std::string, std::string>> pivot_affected_cons_sig;

    // --- S10: the deferred queues (filled by S11, drained by S12 and S13) -----
    // Deferred state for pivot-affected (sat, signal) culprits.
    // Populated by the per-sat loop below; drained after the
    // loop by the force-handover + post-handover repair block.
    std::vector<DeferredIntegerRepair> deferred_integer_repairs;
    // Pivot-affected sats whose slip integer was inconsistent
    // across pairs: no integer to apply, so they are reset after
    // the handover.
    std::vector<std::pair<std::string, std::string>> deferred_resets;
};

} // namespace GnssProcessor
