#pragma once
#include <cstddef>
/**
 * @file EpochAdmission.hpp
 * @brief The epoch admission of the network producer: whether a synced epoch is processed,
 *        skipped or ends the run.
 *
 * Two sites give the one verdict, because the steps between them run on every synced epoch
 * that is not fast-forwarded:
 *   1. networkFastForward, first in Application::networkProcessEpoch_: the start-offset
 *      fast-forward (network.start_offset_hours).  It precedes the event injector, so a
 *      fast-forwarded epoch cannot consume an event, and the signal roster.
 *   2. networkAdmitEpoch, after the epoch is built and the signal roster has advanced
 *      ctx.arc_ledger: the one-epoch clears (the RHS attribution arm, the trip hand-off, the
 *      datum-graph spectrum), the staged broadcast feeds, then the max-duration stop
 *      (processing.max_duration_s), the decimation skip (T2e) and the processed-epoch count.
 * A single function would have to own the injector, the canonical-pair screen, the epoch's
 * construction and the roster, whose order against both gates is load-bearing.
 *
 * Both bodies are the frozen driver text (main 7141240), moved by scripts/nom_ep2_cut.py and
 * checked by scripts/nom_ep2_verify.py.
 */

namespace GnssProcessor {

struct NetworkRun;
struct NetworkEpoch;
struct SyncedEpoch;

/// The admission's verdict on one synced epoch.
enum class EpochAdmission {
    Process,   ///< The epoch is processed (and counted in NetworkServiceCounters::n_loop_epochs).
    Skip,      ///< The epoch is not processed: fast-forwarded or decimated out; the run goes on.
    Stop       ///< The run ends here: the max-duration limit was reached (nothing of this epoch is processed).
};

/**
 * @brief The start-offset fast-forward (network.start_offset_hours).
 *
 * Anchors on the first synced epoch and skips every epoch until the elapsed data span reaches
 * the offset; the solve then cold-starts.  Writes NetworkCarryState::start_anchor_s once.  The
 * span and the offset are compared on the millisecond grid (TimeUtils::offsetReached), so an
 * offset that lies on an epoch starts the run on that epoch, and an offset between two epochs
 * (a half-epoch back-off) starts it on the next one.
 *
 * @param run    The run (network/driver/Run.hpp): reads ctx.knobs.start_offset_s, writes
 *               ctx.carry.start_anchor_s.
 * @param se_raw The source's epoch, before the injector (only its time tag is read).
 * @return Skip while inside the offset, else Process (always Process with the knob unset).
 */
EpochAdmission networkFastForward(NetworkRun& run, const SyncedEpoch& se_raw);

/**
 * @brief The per-epoch admission: the epoch top's one-epoch clears and feeds, the max-duration
 *        stop, the decimation skip and the processed-epoch count.
 *
 * In order: arms the estimator's shared-information recorder for the RHS attribution; clears
 * the trip hand-off and, under the datum-graph census, its spectral hand-off; feeds the staged
 * broadcast navigation records this epoch has reached (brdc_rts) and the native broadcast clock
 * a priori; returns Stop once processing.max_duration_s of data has elapsed; returns
 * Skip for an epoch decimation removes; else counts the epoch and returns Process.
 *
 * @param run The run: writes the estimator's recorder arm, ctx.carry (rhs_trip_pending,
 *            dg_spec), the broadcast store and cursor (setup), ctx.counters (the max-duration
 *            anchor, decim_counter, n_loop_epochs); logs the stop.
 * @param ep  The epoch, read only (its time tag).
 * @return Process, Skip or Stop (see EpochAdmission).
 */
EpochAdmission networkAdmitEpoch(NetworkRun& run, const NetworkEpoch& ep);

/// @brief Run-scope counters of EpochAdmission, a base of NetworkServiceCounters (network/state/RunContext.hpp).
struct EpochAdmissionCounters {
    double max_dur_anchor_s = 0.0;
    bool   max_dur_anchored = false;
    std::size_t decim_counter = 0;   // counts post-offset raw epochs for epoch_decimation_n (T2e)
};

} // namespace GnssProcessor
