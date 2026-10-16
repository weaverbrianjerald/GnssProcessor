#pragma once
/**
 * @file NetworkClockDatum.hpp
 * @brief Stage 3 of the network solve (card NOM3 step SG): the clock datum.  The declaration
 *        the election reads, the HLT-aware sticky election (NetworkSbasis::buildSharedDatum),
 *        the constraint-realized clock rows and the pivot-handover verdict.
 */

#include "filter/NetworkSbasis.hpp"     // SbasisDeclaration
#include "filter/NetworkRunTypes.hpp"   // EpochAction

namespace GnssProcessor {

class GnssConfig;
struct NetworkRunContext;
struct NetworkRun;
struct NetworkEpoch;
struct SolveDatum;   // filter/NetworkSolveGlue.hpp

/**
 * @brief The clock datum's declaration the epoch's election reads
 *        (@ref NetworkSbasis::buildSharedDatum): the reference receiver, the self-described
 *        code-bias and phase-bias frames, and the clock gauge (broadcast-anchored,
 *        sum-to-zero, or a configured pivot satellite).
 *
 * One implementation, read by the election in @ref networkClockDatum and by the handover
 * preview before PASS B (@ref networkHandoverStransformBeforeRows, card PRM phase 2), so the
 * two elect from the same declaration.
 * @param ctx              the run context (the knobs).
 * @param config_          the configuration (the network block).
 * @param clk_anchor_sigma the resolved broadcast anchor sigma (m).
 * @return the declaration.
 */
SbasisDeclaration networkClockDatumDeclaration(const NetworkRunContext& ctx,
                                               const GnssConfig& config_,
                                               double clk_anchor_sigma);

/**
 * @brief Stage 3: elects the clock datum (HLT-aware, sticky), folds the constraint-realized
 *        clock rows and decides the pivot-handover verdict.
 * @param run   The run: the estimator is folded; ctx counters (hlt_*) are written.
 * @param ep    The epoch, read: se, columns (registered_clock_sats).
 * @param datum [out] The plan, datum_changed and changed_consts (SolveDatum).
 * @return SkipEpoch when the election throws (exit DatumFailed), else Proceed.
 */
EpochAction networkClockDatum(NetworkRun& run, const NetworkEpoch& ep, SolveDatum& datum);

} // namespace GnssProcessor
