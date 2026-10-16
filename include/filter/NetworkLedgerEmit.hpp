#pragma once
/**
 * @file NetworkLedgerEmit.hpp
 * @brief The emitter's per-family bookkeeping read against the arc ledger, and the
 *        producer's declaration of its signal roster on the wire
 *        (@c network.ledger_emit, off by default; card SID phase 5a).
 *
 * ### What this observes
 *
 * **E1, the emitter against the ledger.**  Two containers track a lifecycle that the
 * ledger also tracks, each in its own key space, and neither reads the other:
 *
 *  - The @c pb_hold of card PBH keeps a generation counter per (satellite, signal);
 *    a change of that counter is the declaration on which a consumer refloats.  The
 *    ledger keeps an admitted generation per (station, satellite, signal), which
 *    advances at an arc birth.  A new hold generation with no arc birth behind it is
 *    a hold restart; an arc birth with no new hold generation is a generation the
 *    consumer is never told about.  Both cases are counted and written as events
 *    with their identity.
 *  - The emitter's per-family baselines classify a first sighting as
 *    @c find() == end() (@c NetworkProductEmitter::isPhaseFirstSighting and
 *    @c isCodeFirstSighting).  A family whose arcs the ledger already holds is a
 *    return, not a first sighting; the anchor election confuses the two (SID audit).
 *
 * **E2, the declaration.**  The producer states on the wire, per constellation and
 * RINEX band digit, which phase and code signals it models, which codes are carried
 * as card SCC companions, and which signal the S-basis pins (grammar in
 * @c products/SignalDeclaration.hpp).  Nothing on the producer reads it back.  It lets
 * a consumer distinguish "the network carries no family for this signal" from "the
 * record is stale", which the freshness flip cannot distinguish (audit Part C).
 *
 * ### Observatory
 *
 * Nothing here writes a solve quantity.  When armed, the only difference in
 * @c products.txt is the @c N records; with those removed the file is byte-identical
 * (standing invariant 6).  The run-end table is the ON-path telemetry (standing
 * invariant 4): every class is printed with its denominator, and a zero over a zero
 * denominator is reported as dead wiring.
 *
 * ### Two call sites
 *
 * The two halves need state from different points of the epoch.
 * @ref networkLedgerEmitCapture runs at the emit site, before
 * @c NetworkProductEmitter::emitToStore overwrites the baselines, and records the
 * emitter's first-sighting answers and this epoch's hold generations.
 * @ref networkLedgerEmitFold runs after the roster's audit pass, once the ledger's
 * admitted machine has advanced for this epoch, and does the comparison.  The capture
 * reads state that emitToStore is about to overwrite; the fold needs state that does
 * not exist until the audit pass has run.
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

    /// @brief One emitted family's state at the emit site (E1, card SID phase 5).
    struct LedgerEmitFamily {
        std::string   satellite;
        std::string   signal;
        std::uint32_t hold_generation{ 0 };   ///< hold generation of card PBH, 0 when the hold is not armed.
        bool          phase_first{ false };   ///< the emitter treated the phase bias as a first sighting.
        bool          has_phase{ false };     ///< the epoch carried a phase bias for this key.
        bool          code_first{ false };    ///< the emitter treated the code bias as a first sighting.
        bool          has_code{ false };      ///< the epoch carried a code bias for this key.
    };

    /**
     * @brief Run-scope state of the phase-5a observatory.
     *
     * Owned by @c NetworkRunContext (@c ctx.ledger_emit), where carried state belongs
     * on this path.  Not snapshotted and not part of the warm-resume fingerprint, for
     * the same reason as the roster and the ledger: the solve reads nothing here, so a
     * resumed run starts it fresh.
     */
    struct LedgerEmitState {
        bool armed{ false };            ///< network.ledger_emit.
        bool declare_wire{ true };      ///< write the declaration (E2).  Not read; the declaration is always written while armed.

        /// This epoch's capture, consumed by the fold.  Cleared at every capture.
        std::vector<LedgerEmitFamily> epoch;
        bool epoch_valid{ false };      ///< a capture ran for the epoch the fold is about to read.

        /// "SAT|SIGNAL" -> the hold generation this producer last emitted, so the fold
        /// tests for a change rather than for a nonzero value.
        std::map<std::string, std::uint32_t> prev_hold_gen;

        /// (station, satellite, signal) -> the admitted generation the fold saw at the
        /// previous epoch.  An increase is an admitted birth, the ledger edge the
        /// emitter's generation should track.  Stored rather than derived from the
        /// record's state because a birth and a Reentered-to-Tracking step leave the
        /// same state.
        std::map<ArcLedger::Key, int> prev_adm_gen;
        bool prev_adm_gen_valid{ false };

        /// Per-class event budget, spent over the whole run (see
        /// @ref kLedgerEmitEventCap).  Stored here rather than in the fold so the cap
        /// bounds the run and not each epoch.
        long long ev_gen_no_birth{ 0 };
        long long ev_birth_no_gen{ 0 };
        long long ev_first_return{ 0 };

        /// "SAT|SIGNAL" -> admitted-alive arcs of the family at the previous fold.  A
        /// family that goes from zero to some is a family-level return, which differs
        /// from one more station joining a family the wire already carries.
        std::map<std::string, long long> prev_family_alive;

        /// The declaration as of the last epoch it changed; the writer emits only on a
        /// change.
        NetworkSignalDeclaration decl;
        std::string              decl_wire;      ///< '\n'-joined records last written ("" = never).  Not written or read; the change test compares @c decl.
        bool                     decl_pending{ false };  ///< the build changed it and the writer has not run.
    };

    /// @brief Run-end counters of the phase-5a observatory (invariant 4).
    struct NetworkLedgerEmitCounters {
        long long epochs{ 0 };            ///< emit epochs the capture ran (denominator).
        long long families{ 0 };          ///< emitted (sat, signal) families examined, summed over epochs.

        // --- E1a: the hold generation against the ledger's admitted births ----
        long long gen_pop{ 0 };           ///< families carrying a hold generation (denominator of E1a).
        long long gen_changed{ 0 };       ///< of those, with ledger arcs and a prior emission, whose hold generation changed at this epoch.
        long long gen_with_birth{ 0 };    ///< a hold generation change with an admitted arc birth (agreement).
        long long gen_no_birth{ 0 };      ///< a hold generation change with no admitted arc birth.
        long long birth_no_gen{ 0 };      ///< an admitted arc birth with no hold generation change.
        /// Of those, the family itself had no admitted-alive arc at the previous epoch:
        /// the family returned on the wire and the consumer was not told.  The
        /// remainder are one more station joining a family the wire already carries,
        /// which is not a re-levelling and is expected.
        long long birth_no_gen_family{ 0 };
        long long gen_no_arcs{ 0 };       ///< families the ledger has no arc for at all (not comparable).

        // --- E1b: the emitter's first sighting against the ledger -------------
        long long first_phase{ 0 };       ///< phase-bias first sightings seen (denominator).
        long long first_phase_true{ 0 };  ///< of those, with no admitted arc of the family in the ledger: a true first sighting.
        long long first_phase_return{ 0 };///< of those, whose family already carries an admitted generation.
        /// Of those returns, the ones whose arcs were all admitted for the first time
        /// at this epoch: the ledger and the emitter saw the family in the same epoch,
        /// so the "return" comes from the two bookkeepings meeting and is not a
        /// re-coupling edge.  Mostly the run's first epoch.
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
        long long decl_cells_latch{ 0 };  ///< of those, answered by the station latch because the roster cell had no modelled role yet.
        long long decl_empty{ 0 };        ///< epochs whose declaration named no modelled signal at all (never written).
        double    decl_bytes{ 0.0 };      ///< bytes the N records added to products.txt.
        double    capture_ms{ 0.0 };      ///< wall time inside the capture, summed.
        double    fold_ms{ 0.0 };         ///< wall time inside the fold, summed.
    };

    /// @brief Per-class event cap, which bounds the ARC rows a run can write.
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
     * @brief Capture half (E1): the emitter's first-sighting answers and the hold
     *        generations of the epoch about to be emitted.
     *
     * Called from @c networkEmitStore between @c networkPbHold (which stamps the hold
     * generations into @p sol) and @c NetworkProductEmitter::emitToStore (which
     * overwrites the per-family baselines).  Reads only; @p sol is const.  The E2
     * declaration is built by a separate call at the same site
     * (@ref networkLedgerEmitBuildDeclaration).
     *
     * @param ctx     Run context.
     * @param emitter The product emitter, read only through its first-sighting
     *                predicates.  Never mutated.
     * @param sol     The epoch about to be emitted.
     */
    void networkLedgerEmitCapture(NetworkRunContext& ctx,
                                  const NetworkProductEmitter& emitter,
                                  const NetworkSolutionEpoch& sol);

    struct NetworkRun;              // app/NetworkRun.hpp (card NOM3 step EP1)
    struct NetworkEpoch;            // app/NetworkEpoch.hpp (card NOM3 step EP1)

    /**
     * @brief Fold half (E1): compare the capture with the ledger, count, write events.
     *
     * Called from the driver after @c networkSignalRosterAudit, where the ledger's
     * admitted machine has advanced for this epoch.  When no capture ran (a withheld
     * epoch), every counter is left unchanged.
     *
     * Epoch form (card NOM3 step EP1); the explicit form and its forwarding adapter
     * retired in card NOM3 step EP2 (the unit tests call this form through
     * tests/support/NetworkTestRun.hpp).
     *
     * @param run The run (app/NetworkRun.hpp). Written through non-const bindings: run.ctx (ctx),
     *            run.traces (traces).
     * @param ep The epoch (app/NetworkEpoch.hpp), read only. Bindings: ep.se (se).
     *
     * The former parameters, by binding:
     *   - @p ctx = run.ctx: Run context.
     *   - @p traces = run.traces: Trace bundle (the @c ARC event rows).
     *   - @p se = ep.se: This epoch (its time stamps the rows).
     */
    void networkLedgerEmitFold(NetworkRun& run, const NetworkEpoch& ep);

    /**
     * @brief E2: build this epoch's declaration from the roster.
     *
     * Reads the roster and, for the fall-back below, each station model's latch;
     * writes only the E2 counters.  Aggregates the per-station cells into one
     * statement per (constellation, band digit): the modelled phases and codes the
     * producer's stations carry, ordered by the number of stations that model them
     * (descending, lexical tie break; this is the network's preference order, which a
     * consumer keyed to the declaration walks), the card SCC companions, and the
     * S-basis datum designation when all of the epoch's pins agreed on one signal.  A
     * row that names no modelled phase or code is dropped.
     *
     * First-epoch fall-back.  The roster is built upstream of @c computeModels, so at
     * a constellation's first epoch its cells carry no modelled role.  A declaration
     * taken from those cells alone would state that the network models nothing, a
     * false statement that the consumer would act on and against which its own latch
     * would be decided.  A cell the roster cannot answer therefore falls back to that
     * station's canonical latch, which is the source the roster reproduces one epoch
     * later and the same fall-back the phase-2 tracker of card SID uses.  The
     * fall-backs are counted (@ref NetworkLedgerEmitCounters::decl_cells_latch), and a
     * declaration that still names no modelled signal is returned with @c present
     * false, so nothing reaches the wire or the consumer store.
     *
     * @param ctx      Run context (@c ctx.roster and the counters).
     * @param stations The producer roster, index-aligned with @c ctx.roster.by_station.
     * @return The declaration; @c present false when it names no modelled signal or
     *         the roster is not armed.
     */
    NetworkSignalDeclaration networkLedgerEmitBuildDeclaration(
        NetworkRunContext& ctx, const std::vector<NetStation>& stations);

    /**
     * @brief Run-end readout: the table of every class, or nothing when disarmed.
     *
     * @param ctx Run context.
     * @param log Producer logger.
     */
    void networkLedgerEmitRunEnd(const NetworkRunContext& ctx,
                                 const std::shared_ptr<spdlog::logger>& log);

    // ---- Run-end reports (card NOM step TD1b; generated by scripts/nom_td_cut.py) ----

    struct NetworkRunEndView;

    /**
     * @brief Run-end report: the ledger-driven emission (SID phase 5a).
     *
     * The table's row for the ledger-driven emission (SID phase 5a); the report is the
     * overload above.
     *
     * Gate: none of its own: the row calls the report above on every run, as the teardown
     * did, and that report applies its own.
     *
     * @param v The run at its end (filter/NetworkRunEnd.hpp).
     * @return 0 (this report does not set the exit code).
     */
    int networkLedgerEmitRunEnd(const NetworkRunEndView& v);

    // ---- End of the run-end reports ----

} // namespace GnssProcessor
