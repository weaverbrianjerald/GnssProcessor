#pragma once
/**
 * @file PassASlipScanCounters.hpp
 * @brief Run-scope counters of PassASlipScan, a base of NetworkServiceCounters (network/state/RunContext.hpp).
 *
 * Declared apart from the owner header, which includes RunContext.hpp.
 */

#include <map>
#include <string>

namespace GnssProcessor {

/// @brief Run-scope counters of PassASlipScan, a base of NetworkServiceCounters (network/state/RunContext.hpp).
struct PassASlipScanCounters {
    long long n_resume_slip_warmup_epochs = 0; ///< epochs the MW/LLI drain ran diagnostic-only post-resume
    /// MW detect-only admission census (network.cycle_slip.mw_evidence_refloat).
    /// Reduced serially from PassABuffers::sta_mw_admit after the parallel scan, and
    /// collected independently of the knob, on purpose.  legacy: admitted by the
    /// integer gate.  evidence: admitted by the strength rule alone with the knob armed.
    /// would_admit: the same population with the knob off, i.e. exactly what arming
    /// would change (an off run that reports 0 here says the knob is inert on that
    /// lineage, with no run needed).  drop_loose / drop_unknown: the two refused
    /// populations, a flag with a known integer whose rounding residual exceeded
    /// turbo_edit_frac_max, and a flag with no usable integer at all.
    long long n_mw_admit_legacy = 0, n_mw_evidence_admitted = 0,
              n_mw_evidence_would_admit = 0, n_mw_drop_loose = 0,
              n_mw_drop_unknown = 0;
    /// Detector rows the network did not form under `primary`, summed over stations
    /// (network.cycle_slip.mw_detect_pairs).  Zero under the default `all` and
    /// on a two-band lineage; a `primary` run reporting zero on a three-band lineage is
    /// dead wiring.
    long long n_mw_pair_scope_skipped = 0;
    /// Common-band scope census (network.cycle_slip.common_band_scope).
    /// Reduced per epoch from CommonBandEpoch and, like the MW
    /// admission census above, collected independently of the knob, on purpose.
    /// verdicts: (station, satellite) pairs that had at least one MW pair fire, i.e. the
    /// population a band scope could act on.  narrowed: of those, the ones positive
    /// exoneration restricted.  bands_kept: bands a narrowed verdict spared
    /// (Σ|exonerated|), the headline number, because under the all-band union every live
    /// band would otherwise have been freed.  all_no_exoneration / all_inconsistent: the
    /// two fallbacks that return the unscoped response (nothing exonerated; every band
    /// exonerated although a pair fired).  iftd_veto: silent pairs refused as
    /// exonerators because the IF-TD detector flagged them (the equal-count trap).
    /// immature_pairs: silent pairs refused for arc age.  would_narrow: the knob-off count
    /// of verdicts that would have narrowed, i.e. exactly what arming changes; an off run
    /// reporting 0 here says the knob is inert on that lineage, with no armed run needed.
    /// weak_silence: exonerating silences whose detection threshold stood above 6
    /// wide-lane cycles, a gate too loose to have excluded the slip sizes the scope
    /// responds to.  Reported, never acted on; it measures how much narrowing rests on a
    /// weak silence.
    long long n_cb_verdicts = 0, n_cb_narrowed = 0, n_cb_all_no_exoneration = 0,
              n_cb_all_inconsistent = 0, n_cb_bands_kept = 0,
              n_cb_iftd_veto = 0, n_cb_immature_pairs = 0,
              n_cb_weak_silence = 0, n_cb_anchor_colocate = 0,
              n_cb_would_narrow = 0;
    /// Silent pairs refused as exonerators because one of their bands carried a receiver
    /// LLI at the previous epoch, so the detector withheld that pair's sample and kept
    /// the arc (the count runs straight through the flagged epoch while the lane saw
    /// nothing).  Its own counter, not folded into n_cb_immature_pairs:
    /// the two refusals have different causes, and this one sizes the population the
    /// spurious `keep` verdicts came from.
    long long n_cb_lli_shadow = 0;
    /// Verdicts formed from the receiver's LLI bits alone, at an epoch where every MW
    /// sample of the satellite was withheld so the normal rule could not run, and how
    /// many of those restrict a drain.  Independent of the knob: an off run reports
    /// what arming would scope.  A run whose LLI drain
    /// fires with lli_verdicts 0 is dead wiring.
    long long n_cb_lli_verdicts = 0, n_cb_lli_narrowed = 0;
    /// Of those, the ones refused because the station anchors the satellite (also
    /// counted in @ref n_cb_anchor_colocate).  Its own counter because the co-locates of
    /// the two paths must be distinguishable: attributing a split-reference
    /// regression to the LLI path relies on this split.
    long long n_cb_lli_anchor_colocate = 0;
    /// The reference-arc clause (network.cycle_slip.common_band_ref_arc): silent MW
    /// pairs refused as exonerators because their IF-TD sample sits on the chain's
    /// reference arc, whose double difference is identically 0, so the pooled test could
    /// not have flagged it and the silence is the absence of a test, not evidence of
    /// continuity.  Counted apart from @ref n_cb_iftd_veto on purpose: a veto is
    /// positive evidence the pair moved, this is a structural dead zone, and only the
    /// second carries the clause's named cost (a real single-band slip on a reference
    /// arc also stops narrowing).  Nonzero only with the clause armed, so an armed leg
    /// whose event sits on a known reference arc and reports 0 here is dead wiring;
    /// the by-pair map attributes the refusals to the chains whose reference the arc is.
    long long n_cb_iftd_ref_arc = 0;
    std::map<std::string, long long> n_cb_iftd_ref_arc_by_pair;
};

} // namespace GnssProcessor
