#pragma once
/**
 * @file HandoverStransformCounters.hpp
 * @brief Run-scope counters of HandoverStransform, a base of NetworkServiceCounters (network/state/RunContext.hpp).
 *
 * Declared apart from the owner header, which includes RunContext.hpp.
 */

#include "network/state/CarrySites.hpp"
#include <map>

namespace GnssProcessor {

/// @brief Run-scope counters of HandoverStransform, a base of NetworkServiceCounters (network/state/RunContext.hpp).
struct HandoverStransformCounters {
    // Handover S-transform telemetry (queue item 1(b); invariant #4): these are the
    // mechanism's on-path counters, and transforms applied with sts_rows_retained == 0 mean
    // the fold release is mis-wired however clean the log looks.
    long long sts_edges = 0;             ///< pivot-switch (constellation,epoch) edges the transform handled
    long long sts_applied = 0;           ///< carried rows whose mean/var the transform re-expressed
    long long sts_noop_no_carry = 0;     ///< edges skipped: the incoming pivot had no carried clock level
    long long sts_pre_edges = 0;         ///< edges re-expressed before the coupling rows
    long long sts_pre_redetected = 0;    ///< re-detections after a skipped epoch, released without a second transform
    long long sts_pre_mismatch = 0;      ///< changed constellations the solve glue elected differently from the preview (the transform ran after PASS B instead)
    long long sts_pre_undone = 0;        ///< previewed transforms undone after a skipped epoch, the next election having chosen another pivot
    long long sts_selfcheck_fail = 0;    ///< post-transform |x_q| or re-derived c0 above tolerance (§8.4 INC-3)
    double    sts_max_abs_c0 = 0.0;      ///< largest |c0| applied over the run (m)
    double    sts_last_c0 = 0.0;         ///< the last c0 applied (m)
    // Dense handover S-transform (network.handover_stransform_dense; invariant #4: armed with 0 dense edges
    // on a run with a handover means the kernel is mis-wired).
    long long sts_dense_edges = 0;          ///< edges the dense kernel re-expressed (S P S^T)
    long long sts_dense_fallback = 0;       ///< edges the dense kernel could not apply (the diagonal kernel ran)
    long long sts_dense_rows = 0;           ///< carried clocks with a block row re-expressed
    long long sts_dense_parked = 0;         ///< carried clocks without a block row moved by the diagonal rule
    long long sts_dense_p_added = 0;        ///< outgoing pivots appended to the block and the carry
    long long sts_dense_selfcheck_fail = 0; ///< dense edges whose self-check failed
    double    sts_dense_var_saved = 0.0;    ///< summed (diagonal kernel's variance - dense variance) over the re-expressed clocks (m^2)
    double    sts_dense_contrast_maxdev = 0.0; ///< largest |change| of a between-clock contrast variance over the run (m^2; rounding)
    /// The baseline that increment 0 named as missing: clock-pivot handovers per
    /// constellation over the run.  Counted whenever pivotHandoverSignalled raises the
    /// edge, transform armed or not, so an off run states the population an on run
    /// acts on.
    std::map<char, long long> pivot_handovers;
    long long prm_ref_calls[kCarryMechanisms]{};
};

} // namespace GnssProcessor
