#pragma once
/**
 * @file StateSnapshotCounters.hpp
 * @brief Run-scope counters of StateSnapshot, a base of NetworkServiceCounters (network/state/RunContext.hpp).
 *
 * The writing file declares its functions in RunContext.hpp, so the counters have a header of their own.
 */

#include <cstddef>

namespace GnssProcessor {

/// @brief Run-scope counters of StateSnapshot, a base of NetworkServiceCounters (network/state/RunContext.hpp).
struct StateSnapshotCounters {
    /// The closed-loop consumer's snapshot written beside each network history copy
    /// (`<state_snapshot_path>.consumer.epNNNN`).  A seed
    /// with `closed_loop.state_snapshot` armed and `cl_snap_written == 0` has no
    /// consumer warm start to offer a leg, however clean the log reads.
    long long cl_snap_written = 0;       ///< consumer snapshots written with the network copy's tow
    long long cl_snap_skipped = 0;       ///< history epochs where the consumer had not processed that tow (nothing written)

    double prev_tow = -1.0;
    int    prev_week = -1;
    int    iod = 0;
    size_t n_emitted = 0, n_skipped = 0, n_gate_withheld = 0, n_runaway_epochs = 0;
    // Operator health-record state (network.health_interval_s > 0).
    double last_health_write_s = -1.0;   // content time of the last health row (s)
    double last_emit_content_s = -1.0;   // content time of the last emitted epoch (s)
    size_t n_sigma0_withheld = 0;   // epochs skipped specifically by the σ̂0 withhold ceiling (subset of n_skipped)
    long long n_snap_parked_excluded  = 0;  ///< parked entries excluded from written snapshots

    // State-snapshot writer telemetry; the path stays in NetworkCarryState.
    // The carry blob is about 99 % of
    // the snapshot, so its size and shape are the only readout that shows
    // network.state_snapshot_carry_diagonal took effect: a diagonal run otherwise looks
    // like a dense one in every log line and counter.
    long long snap_writes             = 0;      // snapshots written this run
    long long snap_carry_bytes_last   = 0;      // bytes of the last serialized carry blob
    bool      snap_carry_diagonal     = false;  // that blob was diagonal-only (v3)
    // Coupling-regressor capture (network.state_snapshot_coupling_capture): the
    // QS/QL/QZ payload is about 37x the rest of the file, so the counts of rows, terms
    // and shared columns are the only readout that shows the capture took effect.
    // rows_dropped reports a writer-side numerical problem that is otherwise invisible:
    // a row with a non-finite field is omitted whole, never in part, because a partial
    // coefficient vector breaks the gauge identity.
    long long snap_cpl_dict_last     = 0;   // shared columns in the last snapshot's QS dictionary
    long long snap_cpl_rows_last     = 0;   // QL rows in the last snapshot
    long long snap_cpl_zrows_last    = 0;   // QZ rows in the last snapshot
    long long snap_cpl_terms_last    = 0;   // regressor entries in the last snapshot
    long long snap_cpl_rows_dropped  = 0;   // rows omitted whole for a non-finite/negative field, run total
    long long snap_datum_rows_last   = 0;   // EQ*/CB* datum rows in the last snapshot; 0 under an armed capture on an eq34/time-linked-CB run means the capture is inert
    long long snap_emit_rows_last    = 0;   // ER emit-layer rows in the last snapshot; 0 under an armed capture with the re-base running means the capture is inert
    long long snap_station_rows_last = 0;   // RC/SD station-layer rows in the last snapshot; 0 under an armed capture on a run that arms carry_rx_code_bias or the SH deviation carry means the capture is inert
    long long snap_canon_rows_last   = 0;   // CN canonical-signal rows in the last snapshot; 0 under an armed capture means no station had decided a latch yet, which on a network run past epoch 1 means the capture is inert
    long long snap_rxcb_lin_rows_last = 0;  // RL code-bias linearization-point rows in the last snapshot; 0 under an armed capture means the capture is inert: no RC row has a finite code-bias state to pair with (for example estimate_ifb is off)
};

} // namespace GnssProcessor
