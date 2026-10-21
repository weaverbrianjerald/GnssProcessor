#pragma once
/**
 * @file WarmResumeCounters.hpp
 * @brief Run-scope counters of WarmResume, a base of NetworkServiceCounters (network/state/RunContext.hpp).
 *
 * The writing file declares its functions in RunContext.hpp, so the counters have a header of their own.
 */


namespace GnssProcessor {

/// @brief Run-scope counters of WarmResume, a base of NetworkServiceCounters (network/state/RunContext.hpp).
struct WarmResumeCounters {
    long long cxu_resume_stash = 0;                  ///< the exact update's stash stamp rebuilt from a restored dense border
    // Resume frame restore (network.resume_frame_restore): the identities of the
    // re-coupling edge, i.e. what the seam reconnected, so a 0 under an armed knob reads
    // as inert.
    long long n_resume_frame_anchors     = 0;  ///< phase-anchor incumbents restored
    long long n_resume_frame_targets     = 0;  ///< sticky pin targets restored
    long long n_resume_frame_cta         = 0;  ///< clock-anchor per-sat states restored
    long long n_resume_frame_cta_anchored = 0; ///< ...of which anchored
    long long n_resume_frame_clk_keys    = 0;  ///< clock emit re-base held-frame per-sat keys restored
    long long n_resume_frame_ledger      = 0;  ///< handover-ledger entries primed from restored leveling rows
    // Resume datum restore (network.resume_datum_restore): the re-coupling edge's
    // identities for the two datum memories.  0 restored under an armed knob means
    // inert.  The first-post-resume-epoch event counts that say whether the datum
    // continued (n_resume_datum_first_reref, _first_reattach, _first_cb_disc) are declared
    // with their writers (ChainDatum.hpp, SolPrep.hpp) and counted on every warm resume,
    // armed or not, so the control leg of an A/B reports them from the same instrument.
    long long n_resume_datum_eq34_rx      = 0;  ///< sticky datum receivers restored (EQD)
    long long n_resume_datum_eq34_edges   = 0;  ///< receiver parent edges restored (EQP)
    long long n_resume_datum_sat_roots    = 0;  ///< satellite component roots restored (EQS)
    long long n_resume_datum_prev_targets = 0;  ///< last-epoch pin targets restored (EQT)
    long long n_resume_datum_prev_el      = 0;  ///< longevity elevations restored (EQE)
    long long n_resume_datum_retained     = 0;  ///< gap-retained tree edges restored (EQR)
    long long n_resume_datum_cb_prev      = 0;  ///< code-bias previous values restored (CBP)
    long long n_resume_datum_cb_seen      = 0;  ///< code-bias ever-seen satellites restored (CBS)
    long long n_resume_datum_cb_pair      = 0;  ///< code-bias IF-pin reference pairs restored (CBI); 0 here with CBP > 0 means the IF-pin half re-seeds anyway
    // Resume emit-layer restore (network.resume_emit_restore): the re-coupling edge's
    // identities for the emit re-base.  0 under an armed knob means inert: the snapshot
    // carried no ER rows (a writer without network.state_snapshot_emit_capture), this run
    // builds no emit re-base, or no warm resume happened, and the run-end line says which.
    long long n_resume_emit_rebase_keys = 0; ///< emission-frame keys restored into NetworkEmitRebase (ER)
    // Resume station-layer restore (network.resume_station_restore): the re-coupling
    // edge's identities for the two station memories.  0 under an armed knob means
    // inert: the snapshot carried no RC / SD rows (a writer without
    // network.state_snapshot_station_capture), the run arms neither carry, a sub-switch
    // withheld that half, or no warm resume happened; the run-end line says which.
    long long n_resume_station_rxcb  = 0; ///< rx-CB carry entries restored (RC)
    long long n_resume_station_shdev = 0; ///< entries of the per-LOS SH deviation carry restored (SD)
    // Resume canonical-signal restore (network.resume_signal_canon_restore).  0 stations
    // under an armed knob means inert: the snapshot carried no CN rows (a writer without
    // network.state_snapshot_signal_capture) or no warm resume happened, and the
    // run-end line says which.  The disagree count (PppModel::canonicalAuditDisagreements,
    // summed in the run-end line) measures the problem directly: it is the number of
    // restored entries that this run's first epoch would have latched differently, i.e.
    // the flips the restore prevents, so a nonzero value confirms the restore acted, and
    // a zero one says the seam epoch happened to agree.
    long long n_resume_canon_stations = 0; ///< stations whose canonical latch was restored (CN)
    long long n_resume_canon_entries  = 0; ///< canonical entries restored across those stations
    // Resume rx-CB linearization-point restore (network.resume_rxcb_lin_restore): the
    // re-coupling edge's identities for the frame against which the rx-CB inject gate
    // judges the restored carry.  0 entries under an armed knob means inert: the snapshot
    // carried no RL rows (a writer without network.state_snapshot_rxcb_lin_capture), or
    // no warm resume happened.
    //
    // The uncovered tally is scoped to the keys for which the RL rows carry a reference.
    // The rows pair 1:1 with the RC rows, so that is exactly the set with an rx-CB carry,
    // i.e. the set whose x_lin the inject gate reads.  A code-bias family with no carry
    // (GPS_IFB5 beside the carried GPS_DCB on a GPS L1/L2/L5 station) takes the
    // one-epoch seed and is not a miss, so it is not counted.  A nonzero tally
    // therefore means a carried key whose restored reference could not be applied (a
    // non-finite value, or a state index this run does not hold), which is a defect.
    // `_uncovered` itself is never written: the tally is kept per station in
    // NetworkCarryState::rxcb_lin_sta_counts and summed by the run-end readout.
    long long n_resume_rxcb_lin_stations  = 0; ///< stations handed an RL override map
    long long n_resume_rxcb_lin_entries   = 0; ///< RL entries installed across them
    long long n_resume_rxcb_lin_uncovered = 0; ///< carried code-bias keys whose restored reference could not be applied (expected 0); never written, see above
    // Coupling-regressor restore (network.resume_coupling_restore): both edges of the
    // seam.  rows/zrows/terms/dict are the re-coupling edge's identities; seed_reinit is
    // the decoupling edge (columns the write epoch was re-drawing, diverted on purpose);
    // n_resume_cpl_seam_present / n_resume_cpl_seam_departed (StationBuildCounters) are
    // what the first fold bound to.  Rows > 0 with seam_present == 0 indicates a
    // miswire, not a clean run.
    long long n_resume_cpl_rows        = 0;  ///< L leveling rows whose regression was rebuilt
    long long n_resume_cpl_zrows       = 0;  ///< Z (ZWD) rows whose regression was rebuilt
    long long n_resume_cpl_terms       = 0;  ///< regressor entries restored across both
    long long n_resume_cpl_dict        = 0;  ///< shared-column dictionary entries read
    long long n_resume_cpl_orphan      = 0;  ///< QL/QZ rows with no live carry to attach to
    long long n_resume_cpl_seed_reinit = 0;  ///< SatSignalState::tu_reinit seeds planted (loose anchor / eph cutover)
    // First-post-resume-harvest observatory (measure-only, byte-identical): the restored
    // M against the epoch's own freshly derived M_d for the same key.  This is the only
    // direct evidence that the restored row was frame-valid, and it needs no second
    // leg.  It also separates "the regressors were stale" from "the residual is the
    // diagonal border" if the trips do not reach 0.
    long long n_resume_cpl_census_rows      = 0;  ///< rows compared against their own fresh harvest
    long long n_resume_cpl_census_terms     = 0;  ///< identities matched in both vectors
    long long n_resume_cpl_census_unmatched = 0;  ///< restored identities the fresh harvest did not produce
    // The dense covariance side-car of the shared carry across the resume seam.
    long long n_resume_cpl_cov_keys    = 0;  ///< dense-carry keys restored from the snapshot's QK/QP records
    long long n_resume_cpl_cov_dropped = 0;  ///< QK keys with no restored carry row (row and column dropped)
    /// Carried prior rows (prev_carry) present at a warm resume with a dense (full-cov)
    /// carry.  The marker that separates "no warm resume happened" from "the border
    /// came back complete": n_resume_cpl_diag_border is 0 in both cases, so the run-end
    /// readout needs its own denominator.
    long long n_resume_dense_carry_keys = 0;
    /// Clock-anchor decouples (nis + abs + gap + datum + ext) accumulated over the first
    /// post-resume epoch, latched when the census retires.  A restored carry injected as
    /// a plain stiff prior on a border with no cross-covariance out-votes the anchored
    /// clock priors and trips them at the first fold, so "trips at a continuous run's
    /// level" is the check on the border.
    /// Structurally 0 on a lineage that does not arm network.clock_temporal_anchor; the
    /// run-end line says which case it is.
    long long n_resume_first_fold_cta_trips = 0;
};

} // namespace GnssProcessor
