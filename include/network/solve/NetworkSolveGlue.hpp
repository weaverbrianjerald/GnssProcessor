#pragma once
/**
 * @file NetworkSolveGlue.hpp
 * @brief The network producer's solve for one epoch: the datum rows, the null-space gate, the
 *        estimator solve, the within-epoch RAIM tier and the sigma0 ceiling, as a fixed list of
 *        stages (card NOM3 step SG).
 *
 * networkSolveGlue calls the stages below in this order and hands each the values the earlier
 * ones produced.  The order is load-bearing: the estimator accumulates the folds in this order
 * (its increments are not bit-symmetric, card KP), and every later stage reads what an earlier
 * one wrote.  Four stages can end the epoch, five ways; each way is a row of kSolveSkips.
 *
 *  #   stage                         home                              hands on              exit
 *  1   networkCensusSigma0           filter/NetworkPrefitCensus        ctx.carry H sigma0
 *  2   networkCodeBiasDatum          filter/NetworkCodeBiasDatum       est rows, ws capture
 *  3   networkClockDatum             filter/NetworkClockDatum          SolveDatum            DatumFailed
 *  4   networkPreGatePriors          filter/NetworkSolveGlue           est priors, ws
 *  5   networkEstimabilityDump       filter/NetworkRankGate            traces
 *  6   networkKaulaMargin            filter/NetworkBiasAdmission       log, once
 *  7   networkSbasisCompletion       filter/NetworkSbasisCompletion    est rows, ws
 *  8   networkRankGate               filter/NetworkRankGate            est heal priors, ws   GateWithheld
 *  9   networkClockModelWarm         filter/NetworkSolveGlue           clk_model_warm
 * 10   networkPostGatePriors         filter/NetworkSolveGlue           est priors, ws
 * 11   networkPrimarySolve           filter/NetworkSolveGlue           x_s                   SolveFailed
 * 12   networkClockAnchorGuardEpoch  filter/NetworkClockAnchor         x_s, ws
 * 13   networkWithinEpochAdapt       filter/NetworkRaimWithinEpoch     WithinEpochOutcome    WeRestoreFailed
 * 14   networkSigma0Ceiling          filter/NetworkRaimWithinEpoch     Sigma0CeilingDecision Sigma0Withheld
 *
 * Order constraints a new stage must respect:
 *  - stage 1 precedes every H line of the epoch (the gate and sigma0 exits read its value);
 *  - stage 2 clears this epoch's datum scratch even when the W6 rows are not built, and its
 *    first-N log reads the estimability budget before stage 5 spends it;
 *  - everything the gate must see is folded before stage 8: the W6 rows, the GFC birth rows and
 *    the KP prior (stage 2), the clock rows (stage 3), the SH and pin-bridge priors (stage 4),
 *    the SBC rows (stage 7); the gate, the dump and the heal read one tolerance (DG2);
 *  - stage 6 takes lambda_max of the reduced system after the KP prior and the clock rows and
 *    before the SBC rows; it equals the gate's only on an epoch where SBC folds nothing and no
 *    clock-clone view is formed (the gate then judges the clone-marginalized view);
 *  - after the gate, in this order: the ephemeris re-level, the handover S-transform, the D2
 *    carry prior, the clock-oscillator prior, the cold clock anchor (stage 10);
 *  - stage 12 runs after the solve and before T2, so a prior fault is refloated before T2
 *    attributes it to data;
 *  - stage 14's recorder edge precedes the epoch's SB_FRAMEDIAG consumer; its withhold tail
 *    dumps the recorder, then erases the freed carries;
 *  - the carve-out is decided in stage 14 and applied in NetworkSolPrep.
 * A new pre-gate or post-gate fold goes into networkPreGatePriors / networkPostGatePriors at
 * its place in that order; a new T2 response class goes to filter/NetworkRaimWithinEpoch; a new
 * way to end the epoch is a SolveSkip row.  No stage has its own section timer: the whole solve
 * is SB_SOLVEGLUE.
 *
 * One access path per object: the solve writes ws, est_index_of and dd_build, and inside the
 * solve a stage reaches them only through those parameters, never through ep.ws,
 * ep.assembly.est_index_of or ep.dd_build.  The one place the two paths meet is EP1's DD build:
 * the within-epoch replay calls networkBuildCarryDdPriors(run, ep, dd_build), which
 * reads the culprit sets as ep.ws.we_culprits while stage 13 adapts the sets through its ws
 * parameter.  They are one object because the spine passes ep.ws and every stage receives the
 * solve's own ws by reference (card WCF; never a copy).
 *
 * Each stage takes the seam form (NetworkRun& run, const NetworkEpoch& ep[, kept...][,
 * outputs...]) and opens with a generated preamble (scripts/nom_sg_cut.py) that binds the
 * names its body reads; the body below it is the frozen text of the solve before the cut
 * (scripts/nom_sg_verify.py proves each line's place).
 */

#include "network/solve/NetworkEstimator.hpp"   // NetworkEstimator (+ PerConstSigma0)
#include "network/solve/NetworkSbasis.hpp"      // NetworkSbasis::Plan
#include "network/driver/NetworkRunTypes.hpp"    // EpochAction / NetStation / StationAssembly / EpochWorkspace
#include "network/driver/NetworkWlRunTypes.hpp"  // (transitively) hoisted run-loop types
#include "network/carry/NetworkInvariantCarryDriver.hpp"
#include "network/driver/NetworkSetupResult.hpp"    // NetworkSetupResult (emitter / product_writer / stations / ...)
#include "rinex/MultiRinexSync.hpp"      // SyncedEpoch
#include "products/SsrCorrectionStore.hpp"   // NetworkProducerStatus (the exits' H line)

#include <Eigen/Core>

#include <array>
#include <cstdint>
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
struct NetworkRunContext;
struct NetworkRunTraces;
struct NetworkRun;              // network/driver/NetworkRun.hpp (card NOM3 step EP1)
struct NetworkEpoch;            // network/driver/NetworkEpoch.hpp (card NOM3 step EP1)
struct EpochEvents;             // network/driver/NetworkEpoch.hpp
struct EpochDdBuild;            // network/driver/NetworkEpoch.hpp (card NOM3 step EP1)

/// What the post-fit tiers hand the epoch stash: seeded by the solve (the within-epoch tier's
/// refloats), extended by networkPostFitRaim and networkClockQc, read by networkStashStatus.
/// With prep.sol, the only epoch members a seam writes after the solve (card NOM3 step EP1).
struct NetworkPostFitHandoff {
    std::set<std::string> clock_refloat_sats;      ///< Satellites the stash refloats: the T2 and RAIM culprits, then the clock NIS and monitor culprits.
    std::set<std::string> retro_flag_sats;         ///< The retro-bump feed (networkRetroUraBookkeeping, inside networkClockQc).
    int                   n_arc_refloat_epoch{0};  ///< Isolated-arc refloats this epoch (the epoch-status row).
};

/**
 * @brief The 17 loop-body locals SB_SOLVEGLUE produces that downstream epoch seams
 *        consume, plus the epoch disposition.  On a SkipEpoch return the value fields
 *        are default (the caller ends the epoch before reading them).
 */
struct NetworkSolveGlueResult {
    Eigen::VectorXd                                  x_s;
    NetworkSbasis::Plan                              plan;
    std::set<char>                                   changed_consts;
    bool                                             datum_changed{false};
    bool                                             clk_model_warm{false};
    NetworkPostFitHandoff                            handoff;   ///< The post-fit hand-off (clock_refloat_sats, retro_flag_sats, n_arc_refloat_epoch), which RAIM and clock QC extend (card NOM3 step EP1).
    bool                                             retro_feed{false};
    int                                              n_adapt{0};
    double                                           sigma0_adapt{0.0};
    bool                                             perconst_active{false};
    std::set<char>                                   perconst_publish;
    std::set<char>                                   perconst_dec_withhold;
    std::map<char, NetworkEstimator::PerConstSigma0> perconst_stats;
    bool                                             we_t2_opp{false};
    bool                                             we_t2_flag{false};
    EpochAction                                      action{EpochAction::Proceed};
    /// The landed within-epoch replay's culprits (card FCF), copied from the epoch
    /// workspace's culprit sets (@ref EpochWorkspace::we_culprits) before the T2 block clears
    /// them; empty without a replay.  Read by the fixed-cycle ledger only (its evidence,
    /// networkArLatticeEvidence).
    WithinEpochCulprits                              we_landed;
};

/// The clock datum of this epoch (stage 3): the S-basis plan and whether the clock pivot moved.
struct SolveDatum {
    NetworkSbasis::Plan plan;                 ///< Pins or constraint rows, the declared dimension, the pivots (networkClockDatum).
    bool                datum_changed{false}; ///< A clock-pivot handover was signalled this epoch (networkClockDatum).
    std::set<char>      changed_consts;       ///< The constellations whose pivot changed, for the scoped response (networkClockDatum).
};

/// What the within-epoch tier T2 hands the sigma0 ceiling and the result (stage 13).
struct WithinEpochOutcome {
    int       n_adapt{0};             ///< Re-solves that landed.
    double    sigma0_adapt{0.0};      ///< The final within-epoch sigma0; 0 when the tier did not assess (A4).
    long long prm_free_landed{0};     ///< The landed replay's withheld carried priors (card PRM), counted on a kept epoch.
    bool      era_live{false};        ///< The previous loop epoch was an A4-withheld decision (the era continues).
    bool      escalated{false};       ///< The withhold-era budget escalation was in effect this epoch.
    std::map<char, std::vector<double>> sigma0_pool_c;  ///< The last assessment's rows per constellation (the carve-out; permuted by stage 14).
    bool      t2_opp{false};          ///< The B8/T2 flag-rate opportunity, committed at the emit site.
    bool      t2_flag{false};         ///< The first-assess sigma0 exceeded the trigger.
    WithinEpochCulprits landed;       ///< The landed replay's culprits, for the fixed-cycle ledger (card FCF).
};

/// The sigma0 ceiling's per-constellation decision (stage 14), applied by NetworkSolPrep.
struct Sigma0CeilingDecision {
    bool active{false};                                     ///< A publishable subset exists.
    std::set<char> publish;                                 ///< Constellations kept in the emission.
    std::set<char> dec_withhold;                            ///< Constellations the decision withheld.
    std::map<char, NetworkEstimator::PerConstSigma0> stats; ///< sigma0_c and rows, for the log and the events.
};

/// The five ways the solve ends an epoch early (card NOM3 step SG).  Enumerators in the order
/// the stages can reach them; kSolveSkips has one row per enumerator, in this order.
enum class SolveSkip : std::uint8_t { DatumFailed, GateWithheld, SolveFailed, WeRestoreFailed, Sigma0Withheld };

/// The values an exit's row and tail read, from the stages that ran before it.  Plain values and
/// non-owning pointers to the orchestrator's own objects; empty (all null or zero) at DatumFailed.
struct SolveSkipFacts {
    const NetworkSbasis::Plan* plan{nullptr};       ///< Stage 3's plan: the trace row's declared dimension, the sigma0 tail's recorder dump.
    bool   datum_changed{false};                    ///< The status row's datum_changed column.
    double sigma0_adapt{0.0};                       ///< The T2 sigma0 (status row; the sigma0 H line without the census feed); 0.0 before T2.
    int    n_adapt{0};                              ///< Re-solves landed (status row).
    const std::vector<int>* est_index_of{nullptr};  ///< The solve's station-slot map (the sigma0 tail's recorder dump).
    /**
     * @brief The facts after stage 3 (GateWithheld, SolveFailed).
     * @param d The orchestrator's datum (read; its plan's address is kept).
     * @return The facts.
     */
    static SolveSkipFacts of(const SolveDatum& d);
    /**
     * @brief The facts after stage 13 (WeRestoreFailed, Sigma0Withheld).
     * @param d            The orchestrator's datum (read; its plan's address is kept).
     * @param we           The within-epoch outcome (read).
     * @param est_index_of The solve's station-slot map (read; its address is kept).
     * @return The facts.
     */
    static SolveSkipFacts of(const SolveDatum& d, const WithinEpochOutcome& we,
                             const std::vector<int>& est_index_of);
};

/// One exit, declared: what networkSolveSkip writes for it.  Every exit counts n_skipped, writes
/// one network_epoch_status row with `disposition` and advances prev_week / prev_tow last.
struct SolveSkipRow {
    SolveSkip   skip;               ///< The exit.
    const char* disposition;        ///< The status row's disposition (the frozen literal).
    const char* site;               ///< The stage that detects it (documentation).
    bool        col_datum_changed;  ///< Status row: datum_changed.
    bool        col_t2_sigma0;      ///< Status row: sigma0 = sigma0_adapt, n_adapt, sigma0_first (when assessed).
    bool        col_t2_meters;      ///< Status row: n_we_refloat_sats, we_clk_move_max_m / _sat (when assessed).
    int         trace_flag;         ///< network trace row with this withheld flag; -1 = no row.
    bool        h_line;             ///< Product heartbeat (H with no E line), or the pre-open buffer.
    NetworkProducerStatus::State h_state;   ///< Its state token when h_line (Unknown otherwise).
    void (*tail)(NetworkRun&, const NetworkEpoch&, const SolveSkipFacts&);  ///< Run after the H line, before the time stamp; nullptr = none.
};
/// The exits, one row per SolveSkip enumerator in its order (defined in NetworkSolveGlue.cpp).
extern const std::array<SolveSkipRow, 5> kSolveSkips;

/**
 * @brief Ends the epoch through @p skip's row of kSolveSkips: n_skipped, the trace row, the
 *        status row, the H line, the tail, then prev_week / prev_tow.
 * @param run  The run: ctx counters (n_skipped, prev_week, prev_tow), the traces and the product
 *             writer are written.
 * @param ep   The epoch, read: se, columns (n_present, n_clock, n_pb), assembly (total_pins).
 * @param skip The exit taken.
 * @param f    The facts the row reads (read).
 * @return A default result whose action is SkipEpoch.
 */
NetworkSolveGlueResult networkSolveSkip(NetworkRun& run, const NetworkEpoch& ep, SolveSkip skip,
                                        const SolveSkipFacts& f);

/**
 * @brief Stage 4: the soft priors the gate must see (SH-VTEC, the open pin bridges) and the
 *        datum verdict the next election's bridge lifecycle reads (ctx.carry.last_datum_changed).
 * @param run   The run: the estimator is folded; ctx carry is written.
 * @param ep    The epoch, read: clock (dt).
 * @param datum The clock datum (read: datum_changed).
 * @param ws    [in/out] The replay capture the priors are recorded in.
 */
void networkPreGatePriors(NetworkRun& run, const NetworkEpoch& ep, const SolveDatum& datum,
                          EpochWorkspace& ws);

/**
 * @brief Stage 9: the two-state clock model is past its warm-up (its predictions fold and its
 *        update runs).  Mirrors carry_warm.
 * @param run The run, read: the knobs (clk_model_on, pl_warmup) and n_emitted.
 * @return True when the model is warm.
 */
bool networkClockModelWarm(NetworkRun& run);

/**
 * @brief Stage 10: the folds after the gate, in their load-bearing order: the ephemeris
 *        re-level, the handover S-transform, the D2 carry prior, the clock-oscillator prior and
 *        the cold clock anchor (networkFoldColdClockAnchor).
 * @param run            The run: the estimator is folded; ctx carry is re-based.
 * @param ep             The epoch, read: se, columns, clock.
 * @param datum          The clock datum (read: plan, datum_changed, changed_consts).
 * @param clk_model_warm Stage 9's verdict.
 * @param ws             [in/out] The replay capture the priors are recorded in.
 * @param events The epoch's events; writes handover and solve (through the transform and the D2 inject).
 */
void networkPostGatePriors(NetworkRun& run, const NetworkEpoch& ep, const SolveDatum& datum,
                           bool clk_model_warm, EpochWorkspace& ws, EpochEvents& events);

/**
 * @brief Stage 11: the shared solve and the card-1d checksum.
 * @param run   The run: the estimator solves; n_cpl_solve_dump is written.
 * @param ep    The epoch, read: se.
 * @param datum The clock datum (read: plan).
 * @param x_s   [out] The shared solution.
 * @return SkipEpoch when the solve throws (exit SolveFailed), else Proceed.
 */
EpochAction networkPrimarySolve(NetworkRun& run, const NetworkEpoch& ep, const SolveDatum& datum,
                                Eigen::VectorXd& x_s);

/**
 * @brief The datum build, the solve and the within-epoch RAIM adaptation of this epoch.
 * @see the file banner for the EpochAction and result-struct contract.  The within-epoch
 *      tier's arming and the resolved clock anchor sigma are read from @c ctx.knobs
 *      (within_epoch_armed, clk_anchor_sigma; card NOM3 step EP0).
 *
 * Epoch form (card NOM3 step EP1; generated by scripts/nom_ep1_cut.py): the run and the epoch
 * replace the explicit parameters; the definition binds each former parameter in its preamble,
 * under its former name.
 *
 * @param run The run: the estimator is folded and solved (and rebuilt by each T2 replay); ctx
 *            carry and counters, the traces, the setup's writer and consumer are written as
 *            before. Written through non-const bindings: run.ctx (ctx), run.traces (traces),
 *            run.setup (setup), run.engines.est (est). Read: run.config (config_), run.log
 *            (log).
 * @param ep The epoch, read: se, clock, pass_a, columns, assembly (asm_res, total_pins).
 *           Bindings: ep.se (se), ep.assembly.asm_res (asm_res), ep.pass_a.present (present),
 *           ep.pass_a.shared_phase_keys (shared_phase_keys), ep.columns.registered_clock_sats
 *           (registered_clock_sats), ep.columns.registered_code_keys (registered_code_keys),
 *           ep.columns.col_desc (col_desc), ep.columns.n_clock (n_clock), ep.columns.n_pb
 *           (n_pb), ep.columns.n_cb (n_cb), ep.columns.n_present (n_present),
 *           ep.assembly.total_pins (total_pins), ep.clock.carry_warm (carry_warm), ep.clock.dt
 *           (dt), ep.clock.pl_now_s (pl_now_s).
 * @param ws [in/out] The capture channel the replay re-folds, the clock maps, and the
 *     within-epoch culprit sets the T2 block adapts and clears (card WCF).
 * @param est_index_of [in/out] ep.assembly.est_index_of, rebuilt by each replay.
 * @param dd_build [in/out] ep.dd_build, rewritten by each replay's DD rebuild.
 *
 * The former parameters, by binding:
 *   - @p ctx = run.ctx.
 *   - @p config_ = run.config.
 *   - @p log = run.log.
 *   - @p traces = run.traces.
 *   - @p setup = run.setup.
 *   - @p est = run.engines.est.
 *   - @p ws = the output parameter itself (ep.ws at the spine line).
 *   - @p se = ep.se.
 *   - @p asm_res = ep.assembly.asm_res.
 *   - @p est_index_of = the output parameter itself (ep.assembly.est_index_of at the spine
 *     line).
 *   - @p present = ep.pass_a.present.
 *   - @p shared_phase_keys = ep.pass_a.shared_phase_keys.
 *   - @p registered_clock_sats = ep.columns.registered_clock_sats.
 *   - @p registered_code_keys = ep.columns.registered_code_keys.
 *   - @p col_desc = ep.columns.col_desc.
 *   - @p n_clock = ep.columns.n_clock.
 *   - @p n_pb = ep.columns.n_pb.
 *   - @p n_cb = ep.columns.n_cb.
 *   - @p n_present = ep.columns.n_present.
 *   - @p total_pins = ep.assembly.total_pins.
 *   - @p carry_warm = ep.clock.carry_warm.
 *   - @p dt = ep.clock.dt.
 *   - @p pl_now_s = ep.clock.pl_now_s.
 *   - @p buildCarryDdPriors = (removed: direct call in the T2 replay).
 *
 * @return The solve's result; action is SkipEpoch at the five exits (bookkeeping done here).
 * @param events The epoch's events; written: handover, code_datum, solve, qc (rhs_trip_pending).
 */
NetworkSolveGlueResult networkSolveGlue(NetworkRun& run, const NetworkEpoch& ep, EpochWorkspace& ws,
                                        std::vector<int>& est_index_of, EpochDdBuild& dd_build, EpochEvents& events);

} // namespace GnssProcessor
