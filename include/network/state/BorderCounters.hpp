#pragma once
/**
 * @file BorderCounters.hpp
 * @brief Run-scope counters of Border, a base of NetworkServiceCounters (network/state/RunContext.hpp).
 *
 * The writing file declares its functions in RunContext.hpp, so the counters have a header of their own.
 */

#include "network/carry/CouplingExact.hpp"
#include "network/driver/RunTypes.hpp"

namespace GnssProcessor {

/// @brief Run-scope counters of Border, a base of NetworkServiceCounters (network/state/RunContext.hpp).
struct BorderCounters {
    long long sts_rows_retained = 0;     ///< carry rows folded that the baseline skip would have dropped
    long long cta_gf_keys = 0;           ///< (sat,epoch) clock priors whose information was gauge-downdated.  0 on an armed gauge-free run means the downdate never reached a group of >= 2 carried clocks: the step is inert and the anchor still constrains the gauge.
    long long cta_gf_inert = 0;          ///< epochs where the gauge-free step was armed but downdated nothing (every constellation contributed < 2 carried clocks, or UᵀΛU was singular)
    long long n_resume_cpl_diag_border   = 0;  ///< border keys (C1) that folded diagonally at the first post-resume fold (the known one-epoch coupling asymmetry; see networkInjectD2CarryPrior in BorderPrior.cpp)
    // Readmit continuity (E2), both edges (standing invariant #7): parked is the
    // decoupling edge (what left, and when); seeded is the re-coupling edge (what came
    // back and was re-validated: by the Q3 guard for the clock, by the stash-side seed
    // check for every kind); expired / dropped are what was not reconnected, and why.
    long long n_readmit_parked        = 0;  ///< entries of the D2 Clock/CodeBias carry parked at a satellite's departure
    long long n_readmit_seeded_clk    = 0;  ///< clock columns seeded from a parked entry on re-registration
    long long n_readmit_seeded_cb     = 0;  ///< code-bias columns seeded
    long long n_readmit_seeded_anch   = 0;  ///< ...of the clock seeds, folded anchored over the gap (instead of loose)
    long long n_readmit_expired       = 0;  ///< parked entries dropped: the gap exceeded the horizon
    long long n_readmit_dropped_datum = 0;  ///< parked entries dropped: a clock-datum change retired their frame
    long long n_readmit_dropped_stale = 0;  ///< parked entries dropped: the column returned but the entry was not consumed (skip-stash / pinned)
    long long n_readmit_events        = 0;  ///< re-admissions detected (clock column back after >= kReadmitMinGapEp loop epochs)
    long long n_readmit_seed_check    = 0;  ///< stash-side seed-vs-solve checks run (all seeded kinds)
    long long n_readmit_seed_outlier  = 0;  ///< ...of which |e|/sqrt(S) > kReadmitSeedK
    long long n_readmit_marg_windows  = 0;  ///< coupling marginalization windows opened
    // Parked seeds the D2 fold applied, met by the epoch-tail snapshots of the carry LS-VCE
    // monitor and the prior-residual census (network.readmit_seed_mirrors).  Off, the
    // snapshots skip every parked entry, so each count is a row the mirror did not take;
    // on, it is a row the mirror took.  Counted only where the mirror is armed for the kind.
    long long n_readmit_mirror_seed_vce_cb    = 0;  ///< code-bias seeds met by the monitor's snapshot
    long long n_readmit_mirror_seed_census_cb = 0;  ///< code-bias seeds met by the census snapshot
    long long n_readmit_mirror_seed_census_clk = 0; ///< clock seeds met by the census snapshot
    long long cxu_inverse_split = 0,      ///< network.parallel_coupling_prepare: inverses split over column blocks
              cxu_inverse_declined = 0;   ///< network.parallel_coupling_prepare: inverses that took the whole solve
    long long cxu_epochs = 0,              ///< epochs the exact coupling update was armed and its inputs were attempted
              cxu_epochs_built = 0,        ///< of those, with the factor of P_SS + Q_S built (the rows took the exact route)
              cxu_epochs_unfactored = 0,   ///< of those, where P_SS + Q_S did not factor (the rows took the coupling-carry route)
              cxu_epochs_nostash = 0;      ///< of those, with no valid stashed carry covariance (nothing built)
    long long cxu_keys_s = 0,              ///< carried keys the rows treated as an S column (folded and present), summed over epochs
              cxu_keys_d[kCouplingExactRoutes]{};   ///< carried keys re-attributed as D columns, summed over epochs, indexed by CouplingExactDRoute
    long long cxu_q_negative = 0;          ///< row inflations below 0 (a VCE factor < 1), clamped to 0
    long long cxu_fold_handover_epochs = 0,   ///< dense-fold epochs with the exact update built and a clock-datum change (pivot handover) this epoch
              cxu_fold_diag_fallback = 0;     ///< epochs the dense carried-covariance fold failed and fell back to the diagonal carry while the exact update was armed
    long long cxu_fold_reexpressed_in_s = 0;   ///< keys in S folded on a variance a handover re-expressed after the rows
    long long cxu_fold_retired_in_s = 0;       ///< keys in S the fold retired at a handover (the rows carried them)
    long long cxu_pivot_missing = 0;       ///< built epochs with a clock constellation and no recorded pivot
    long long prm_disp[4][kCarryDispositions]{};   ///< key-epochs by (SharedParamKind, disposition the fold applied); see CarryDispositionCounters
    long long prm_departed[4]{};                   ///< per SharedParamKind: foldable key-epochs whose column had left the layout (the estimator marginalizes them)
    long long prm_reexpressed = 0,                 ///< keys folded on a carried value or variance that differs from the decided one (a handover re-expressed it after the decision)
              prm_reexpressed_epochs = 0,          ///< epochs with at least one such key
              prm_undecided_at_fold = 0;           ///< entries the fold met with no decision this epoch (0 unless a path skips the decision pass)
    double    prm_reexpressed_max_dvar = 0.0;      ///< largest |carried variance - decided variance| among the re-expressed keys (m^2)
};

} // namespace GnssProcessor
