#pragma once
/**
 * @file NetworkLedgerEmit.hpp
 * @brief Card SID phase 5a: the EMITTER's per-family bookkeeping read against the
 *        arc ledger, and the producer's declaration of its signal roster on the
 *        wire (@c network.ledger_emit, default OFF).
 *
 * ### The two things this observes
 *
 * **E1, the emitter against the ledger.**  Two containers hold a lifecycle the
 * ledger already owns, in their own key space, and neither can see the other:
 *
 *  - card PBH's @c pb_hold carries a GENERATION COUNTER per (satellite, signal),
 *    and a change of it is the declaration a consumer re-floats on.  The ledger
 *    carries an ADMITTED generation per (station, satellite, signal), which
 *    advances at an arc birth.  A new hold generation whose arcs are the same
 *    generation as before is a hold restart with no arc birth behind it; an arc
 *    birth with no new hold generation is a generation the consumer never learns
 *    about.  Both are counted, both are evented with identity.
 *  - the emitter's per-family baselines decide a FIRST SIGHTING with
 *    @c find() == end() (@c NetworkProductEmitter::isPhaseFirstSighting and its
 *    twins).  A family whose arcs the ledger already knows is a RETURN, and the
 *    audit of 2026-09-22 named exactly that confusion in the anchor election.
 *
 * **E2, the declaration.**  The producer states on the wire, per constellation
 * and RINEX band digit, which phase and code signals it models, which codes ride
 * as card SCC companions and which signal the S-basis pins
 * (@c products/SignalDeclaration.hpp holds the grammar).  Nothing on the
 * producer reads it back: it exists so a consumer can tell "the network carries
 * no family for this signal" from "the record is stale", which is the split the
 * freshness flip cannot make today (audit Part C).
 *
 * ### OBSERVATORY
 *
 * Nothing here writes a solve quantity.  Armed, the only difference in
 * @c products.txt is the @c N records themselves; filter them out and the file
 * is byte-identical (standing invariant 6).  The run-end table is the ON-path
 * telemetry (standing invariant 4): every class carries its denominator, and a
 * zero against a zero denominator is reported as dead wiring in those words.
 *
 * ### Two call sites, because the two halves need different epochs
 *
 * @ref networkLedgerEmitCapture runs at the EMIT site, before
 * @c NetworkProductEmitter::emitToStore overwrites the baselines, and records
 * what the emitter believed plus this epoch's hold generations and the
 * declaration.  @ref networkLedgerEmitFold runs after the roster's audit pass,
 * where the ledger's ADMITTED machine has been advanced for this epoch, and does
 * the comparison.  Splitting them is not a convenience: the first half reads
 * state that is about to be destroyed and the second needs state that does not
 * exist yet.
 */

#include "filter/ArcLedger.hpp"
#include "products/SignalDeclaration.hpp"

#include <map>
#include <memory>
#include <string>
#include <vector>

namespace spdlog { class logger; }

namespace GnssProcessor {

    class GnssConfig;
    class NetworkProductEmitter;
    class NetworkRunTraces;
    struct NetStation;               // filter/NetworkRunTypes.hpp
    struct NetworkRunContext;
    struct NetworkSolutionEpoch;
    struct SyncedEpoch;

    /// @brief Card SID phase 5 (E1): one emitted family's state at the emit site.
    struct LedgerEmitFamily {
        std::string   satellite;
        std::string   signal;
        std::uint32_t hold_generation{ 0 };   ///< card PBH's generation, 0 when the hold is not armed.
        bool          phase_first{ false };   ///< the emitter treated the phase bias as a first sighting.
        bool          has_phase{ false };     ///< the epoch carried a phase bias for this key.
        bool          code_first{ false };    ///< the emitter treated the code bias as a first sighting.
        bool          has_code{ false };      ///< the epoch carried a code bias for this key.
    };

    /**
     * @brief Run-scope state of the phase-5a observatory.
     *
     * Owned by @c NetworkRunContext (@c ctx.ledger_emit), which is where carried
     * state belongs on this path.  Deliberately NOT snapshotted and NOT part of
     * the warm-resume fingerprint, for the reason the roster and the ledger give:
     * nothing here is read by the solve, so a resumed run re-seats it.
     */
    struct LedgerEmitState {
        bool armed{ false };            ///< network.ledger_emit.
        bool declare_wire{ true };      ///< E2: write the declaration (always true while armed).

        /// This epoch's capture, drained by the fold.  Cleared at every capture.
        std::vector<LedgerEmitFamily> epoch;
        bool epoch_valid{ false };      ///< a capture ran for the epoch the fold is about to read.

        /// "SAT|SIGNAL" -> the hold generation this producer last emitted, so a
        /// CHANGE is what the fold tests rather than a nonzero value.
        std::map<std::string, std::uint32_t> prev_hold_gen;

        /// (station, satellite, signal) -> the ADMITTED generation the fold saw at
        /// the previous epoch.  An increase is an admitted BIRTH, which is the one
        /// ledger edge the emitter's generation is supposed to track.  Kept here
        /// rather than derived from the record's state because a birth and a
        /// Reentered-to-Tracking step leave the same state behind.
        std::map<ArcLedger::Key, int> prev_adm_gen;
        bool prev_adm_gen_valid{ false };

        /// Per-CLASS event budget, spent over the whole RUN (see
        /// @ref kLedgerEmitEventCap).  Kept here rather than in the fold so the
        /// cap bounds the run and not each epoch.
        long long ev_gen_no_birth{ 0 };
        long long ev_birth_no_gen{ 0 };
        long long ev_first_return{ 0 };

        /// (satellite, signal) -> admitted-ALIVE arcs of the family at the
        /// previous fold.  A family that goes from zero to some is a family-level
        /// return, which is a different statement from one more station joining a
        /// family the wire already carries.
        std::map<std::string, long long> prev_family_alive;

        /// The declaration as of the last epoch it was BUILT, and the wire form
        /// last WRITTEN, so the writer can emit only on a change.
        NetworkSignalDeclaration decl;
        std::string              decl_wire;      ///< '\n'-joined records last written ("" = never).
        bool                     decl_pending{ false };  ///< the build changed it and the writer has not run.
    };

    /// @brief Run-end counters of the phase-5a observatory (invariant 4).
    struct NetworkLedgerEmitCounters {
        long long epochs{ 0 };            ///< emit epochs the capture ran (denominator).
        long long families{ 0 };          ///< emitted (sat, signal) families examined, summed over epochs.

        // --- E1a: the hold generation against the ledger's admitted births ----
        long long gen_pop{ 0 };           ///< families carrying a hold generation (denominator of E1a).
        long long gen_changed{ 0 };       ///< of those, whose hold generation changed at this epoch.
        long long gen_with_birth{ 0 };    ///< a hold generation change WITH an admitted arc birth (agreement).
        long long gen_no_birth{ 0 };      ///< a hold generation change with NO admitted arc birth.
        long long birth_no_gen{ 0 };      ///< an admitted arc birth with NO hold generation change.
        /// Of those, the sharp case: the FAMILY itself had no admitted-alive arc
        /// at the previous epoch, so the wire's family returned and the consumer
        /// was told nothing.  The remainder is one more STATION joining a family
        /// the wire already carries, which is not a re-levelling and is expected.
        long long birth_no_gen_family{ 0 };
        long long gen_no_arcs{ 0 };       ///< families the ledger has no arc for at all (not comparable).

        // --- E1b: the emitter's first sighting against the ledger -------------
        long long first_phase{ 0 };       ///< phase-bias first sightings seen (denominator).
        long long first_phase_true{ 0 };  ///< of those, with no ledger record of the family: a true first sighting.
        long long first_phase_return{ 0 };///< of those, whose family already carries an admitted generation.
        /// Of those returns, the ones whose arcs were ALL admitted for the FIRST
        /// TIME at this very epoch: the ledger and the emitter saw the family in
        /// the same epoch, so "return" is an artifact of the two clocks meeting,
        /// not a re-coupling edge.  Dominated by the run's first epoch.
        long long first_phase_return_new{ 0 };
        long long first_code{ 0 };
        long long first_code_true{ 0 };
        long long first_code_return{ 0 };

        long long events{ 0 };            ///< ARC rows written for the classes above.
        long long events_suppressed{ 0 }; ///< candidate rows the per-class cap withheld.

        // --- E2: the declaration ---------------------------------------------
        long long decl_builds{ 0 };       ///< epochs the declaration was built (denominator).
        long long decl_changes{ 0 };      ///< of those, that changed it.
        long long decl_records{ 0 };      ///< N records written to the wire.
        long long decl_rows{ 0 };         ///< (constellation, band) rows in the current declaration.
        long long decl_cells{ 0 };        ///< station cells the build read, summed over epochs (denominator).
        long long decl_cells_latch{ 0 };  ///< of those, answered by the station LATCH because the roster cell had no modelled role yet.
        long long decl_empty{ 0 };        ///< epochs whose declaration named no modelled signal at all (never written).
        double    decl_bytes{ 0.0 };      ///< bytes the N records added to products.txt.
        double    capture_ms{ 0.0 };      ///< wall time inside the capture, summed.
        double    fold_ms{ 0.0 };         ///< wall time inside the fold, summed.
    };

    /// @brief Per-class event cap, so a pathological run cannot flood the ledger.
    inline constexpr long long kLedgerEmitEventCap = 300;

    /**
     * @brief Arm the phase-5a observatory from config (once, at run start).
     *
     * @param ctx     Run context (@c ctx.ledger_emit).
     * @param config_ The run's config.
     * @param log     Producer logger (one arming line when armed).
     */
    void networkLedgerEmitArm(NetworkRunContext& ctx, const GnssConfig& config_,
                              const std::shared_ptr<spdlog::logger>& log);

    /**
     * @brief CAPTURE half (E1 + E2): what the emitter believed, and the declaration.
     *
     * Called from @c networkEmitStore between @c networkPbHold (which stamps the
     * hold generations into @p sol) and @c NetworkProductEmitter::emitToStore
     * (which overwrites the per-family baselines).  Reads only; @p sol is const.
     *
     * @param ctx     Run context.
     * @param emitter The product emitter, read ONLY through its first-sighting
     *                predicates.  Never mutated.
     * @param sol     The epoch about to be emitted.
     */
    void networkLedgerEmitCapture(NetworkRunContext& ctx,
                                  const NetworkProductEmitter& emitter,
                                  const NetworkSolutionEpoch& sol);

    /**
     * @brief FOLD half (E1): compare the capture with the ledger, count, event.
     *
     * Called from the driver after @c networkSignalRosterAudit, which is where
     * the ledger's ADMITTED machine has been advanced for this epoch.  A capture
     * that never ran (a withheld epoch) leaves every counter alone.
     *
     * @param ctx    Run context.
     * @param traces Trace bundle (the @c ARC event rows).
     * @param se     This epoch (its time stamps the rows).
     */
    void networkLedgerEmitFold(NetworkRunContext& ctx, NetworkRunTraces& traces,
                               const SyncedEpoch& se);

    /**
     * @brief E2: build this epoch's declaration from the roster.  PURE over the roster.
     *
     * Aggregates the per-station cells into one statement per (constellation,
     * band digit): the modelled phases and codes the producer's stations carry,
     * ordered by the number of stations that model them (descending, lexical tie
     * break: the network's own preference order, which is what a consumer keyed
     * to the declaration walks), the card SCC companions, and the S-basis datum
     * designation where the epoch's pins agreed on one.
     *
     * ⚠ THE FIRST-EPOCH FALL-BACK.  The roster is built UPSTREAM of
     * @c computeModels, so at a constellation's first epoch its cells carry no
     * modelled role at all and a declaration taken from them alone would state
     * that the network models NOTHING: a false statement the consumer would act
     * on, and it would be the declaration the consumer's own latch is decided
     * against.  A cell the roster cannot answer therefore falls back to that
     * station's own canonical LATCH, which is the same source the roster
     * reproduces one epoch later and the same fall-back card SID phase 2's
     * tracker takes.  The fall-backs are counted
     * (@ref NetworkLedgerEmitCounters::decl_cells_latch), and a declaration that
     * still names no modelled signal is returned with @c present false so
     * nothing reaches the wire or the consumer store.
     *
     * @param ctx      Run context (@c ctx.roster and the counters).
     * @param stations The PRODUCER roster, index-aligned with @c ctx.roster.by_station.
     * @return The declaration; @c present false when it names no modelled signal.
     */
    NetworkSignalDeclaration networkLedgerEmitBuildDeclaration(
        NetworkRunContext& ctx, const std::vector<NetStation>& stations);

    /**
     * @brief Run-end readout: the table of every class, or nothing when disarmed.
     *
     * @param ctx Run context.
     * @param log Producer logger.
     */
    void networkLedgerEmitRunEnd(NetworkRunContext& ctx,
                                 const std::shared_ptr<spdlog::logger>& log);

} // namespace GnssProcessor
