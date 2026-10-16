#pragma once
/**
 * @file NetworkRunTypes.hpp
 * @brief @internal Run-loop record types for Application::runNetwork: the
 *        per-station bundle, the PASS-B assembly slot, the carry PODs (D2, card 1)
 *        and the other run-loop-local helper types that the network seams share.
 *
 * Support header for the network run loop, not a public API; each type carries its
 * own documentation.  The types came from the anonymous namespace of
 * ApplicationNetwork.cpp (INC-2; see docs/network_decomposition_plan.md).  The
 * former file-static helper @ref sharedCarryKey is @c inline with external linkage.
 */

#include "filter/StateManager.hpp"
#include "network/solve/NetworkEstimator.hpp"      // SharedParamKind
#include "network/carry/NetworkCouplingExact.hpp"  // CouplingExactCounters (StationAssembly, card CTU)
#include "network/assembly/NetworkParamInventory.hpp" // StationParamSlot (StationAssembly, card NOM N1)
#include "network/assembly/NetworkContributions.hpp" // RowTag, GimRowsOutcome (StationAssembly, card NOM N2a)
#include "network/qc/NetworkWithinEpochCulprits.hpp" // WithinEpochCulprits (EpochWorkspace::we_culprits, card WCF)
#include "model/PppModel.hpp"
#include "measurement/MeasurementModel.hpp"
#include "common/PppState.hpp"
#include "slip/MwSlipDetector.hpp"
#include "slip/TurboEditResolver.hpp"
#include "slip/IfPhaseSlipDetector.hpp"

#include <Eigen/Dense>

#include <algorithm>
#include <array>
#include <cmath>          // the invariant step z (std::abs / std::isfinite)
#include <cstdint>        // the freeing-cause bitmask (RelevelEpochState::free_cause)
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

namespace GnssProcessor {

/// Per-station runtime bundle: an independent single-receiver StateManager,
/// PppModel and MeasurementModel.  The single-receiver invariants hold: the network
/// couples stations only through the shared satellite border and never aliases
/// their core indices.  Coordinates are held fixed at @c ecef.
struct NetStation {
    std::string                       name;
    Eigen::Vector3d                   ecef{Eigen::Vector3d::Zero()};
    bool                              is_pivot{false};
    std::unique_ptr<StateManager>     sm;
    std::unique_ptr<PppModel>         model;
    std::unique_ptr<MeasurementModel> mm;
    PppState                          pstate;
    // MW cycle-slip arc state, persistent per station.  Built when
    // network.phase_leveling and (network.cycle_slip.turbo_edit or
    // network.cycle_slip.mw_detect) are on, for non-pivot stations and also for the
    // pivot under network.phase_datum_prioritized or
    // network.cycle_slip.datum_station_detect.  Null otherwise, and no MW detection
    // runs.  The turbo_edit resolver (MW+GF integer repair, for 1 s
    // data) is built only for turbo_edit; under mw_detect the same detector drives a
    // detect-then-refloat with no resolver (a null turbo_edit selects the refloat
    // branch in PASS A).
    std::unique_ptr<MwSlipDetector>   mw_slip;

    /// SD pre-reduce EWL constraint (docs/SPEC_sd_ewl_prereduce.md): per-station
    /// eligibility tallies for `Application::networkAppendSdEwlRows_`.  They are held
    /// on the station rather than in a shared accumulator because the row build runs
    /// inside the parallel produce phase; the run-end readout sums them over stations
    /// serially.  Standing invariant #4: a zero `rows_built` with every other counter
    /// also zero means the wiring never ran, which is different from the feature
    /// having no effect.
    struct SdEwlCounters {
        long long sat_seen{ 0 };                ///< (station, epoch, satellite) visits on a constellation that has an EWL tier; the denominator for every skip counter below.
        long long rows_built{ 0 };              ///< SD EWL rows appended to this station's block.
        long long rows_capped{ 0 };             ///< eligible rows dropped by max_rows_per_station.
        long long skip_no_detector{ 0 };        ///< st.mw_slip == nullptr (no MW detector at this station, e.g. the clock-datum station on the legacy path, or phase_leveling or the detector off); counted once per station-epoch.
        long long skip_no_pivot{ 0 };           ///< fewer than two resolved arcs at this station for a constellation ⇒ no between-satellite difference to form.
        long long skip_immature{ 0 };           ///< an MW arc exists on a candidate signal pair, but its lagged Welford count is below min_arc_epochs.
        long long skip_no_arc{ 0 };             ///< the detector holds no MW arc on any candidate (band1, band2) signal pair, i.e. the L2/L5 MW does not exist at all.  Distinct from skip_immature by construction: here the pair was never formed.
        long long skip_frame_recon{ 0 };        ///< the arc is stored SD (bia_corrected) but its group MW pivot arc was absent, so arcZdMean could not rebuild the ZD frame (valid=false at count>0).
        long long skip_mixed_frame{ 0 };        ///< the two arcs disagree on bia_corrected (satellite-bias cancellation needs a single frame).
        long long skip_missing_amb_col{ 0 };    ///< no (band1, band2) phase-signal pair has both ambiguities in this station's local block this epoch (band untracked, or the arc not estimated).
        long long skip_missing_shared_col{ 0 }; ///< a satellite PB/CB shared column is unregistered this epoch.
        long long skip_missing_code{ 0 };       ///< the arc did not report the code channels its MW consumed, so the satellite code-bias column cannot be named without guessing.
        long long skip_bad_lambda{ 0 };         ///< non-finite / non-positive wavelength or frequency.
        long long skip_big_z{ 0 };              ///< |z_SD| exceeded max_abs_z_m.
        double    sum_var_m2{ 0.0 };            ///< sum of the built rows' pre-whitening SD variances (m^2), for the mean-R readout.
        // --- increment 2b (network.sd_ewl.fix_enable); all zero on the float-only path.
        long long rows_fixed{ 0 };              ///< rows that folded a fixed integer (their four local ambiguity coefficients are absent, not merely small).
        long long rows_float{ 0 };              ///< rows that took the increment-1 float path.  rows_fixed + rows_float == rows_built.
        long long groups_fixed{ 0 };            ///< whitened all-fixed groups emitted (a fixed and a float group never share one R; see Application.hpp).
        long long fix_pair_mismatch{ 0 };       ///< an integer was published, but the row settled on an arc of a different tracking-channel pair, so the integer belongs to another bias frame and the row went float.
        long long fix_datum_only{ 0 };          ///< arcs demoted to the float group because their whole fixed group held only datum values (frame choices, no rounding decision).  A hard constraint with no integer information is not worth a removed degree of freedom, and admitting it would make the feature impossible to disarm through its own gates.
        long long fix_lone_arc{ 0 };            ///< exactly one arc of a constellation had an integer, with nothing to difference it against, so it was demoted to the float group.  It keeps the fix sigma floor, which can only deweight.

        void operator+=(const SdEwlCounters& o) {
            sat_seen += o.sat_seen;
            rows_built += o.rows_built;
            rows_capped += o.rows_capped;
            skip_no_detector += o.skip_no_detector;
            skip_no_pivot += o.skip_no_pivot;
            skip_immature += o.skip_immature;
            skip_no_arc += o.skip_no_arc;
            skip_frame_recon += o.skip_frame_recon;
            skip_mixed_frame += o.skip_mixed_frame;
            skip_missing_amb_col += o.skip_missing_amb_col;
            skip_missing_shared_col += o.skip_missing_shared_col;
            skip_missing_code += o.skip_missing_code;
            skip_bad_lambda += o.skip_bad_lambda;
            skip_big_z += o.skip_big_z;
            sum_var_m2 += o.sum_var_m2;
            rows_fixed += o.rows_fixed;
            rows_float += o.rows_float;
            groups_fixed += o.groups_fixed;
            fix_pair_mismatch += o.fix_pair_mismatch;
            fix_datum_only += o.fix_datum_only;
            fix_lone_arc += o.fix_lone_arc;
        }
    };
    SdEwlCounters sd_ewl_ctr;   ///< accumulated across this run for this station.
    std::unique_ptr<TurboEditResolver> turbo_edit;
    // IF-phase triple-difference slip detection: persistent per-station
    // between-satellite, between-epoch IF-phase OMC state.  Built when
    // network.cycle_slip.if_phase_td is on, for non-pivot stations and, under
    // network.phase_datum_prioritized, for the pivot too; null otherwise (no detection
    // runs).  Station-local, so parallel-safe with no shared write.
    std::unique_ptr<IfPhaseSlipDetector> if_slip;
    // IF-phase slip thresholding: per-signal-pair EWMA of the elevation-normalized
    // robust noise scale for this station, with a clean-sample counter that drives the
    // shrinkage from the network pool toward the per-station value.  Persists across
    // epochs; keyed by the IF pair "sig1_sig2" (e.g. "L1W_L2W").  See the PASS-A
    // IF-phase block.
    struct IfPairScale {
        double ewma_scale{0.0};  ///< EWMA of 1.4826·median(|z|), z = residual·sin(el) (m, zenith-equiv).
        long   count{0};         ///< Clean samples folded in (shrinkage weight vs the network pool).
    };
    std::map<std::string, IfPairScale> if_pair_scale;
    // Prioritized phase-bias anchor (network.phase_datum_prioritized) and eq34 chain
    // (network.phase_datum_eq34): the "sat_signal" ambiguity keys this station
    // gauge-pins this epoch, each mapped to its integer pin target (m).  Each such
    // ambiguity gets the tight-prior (1e10) pin folded into this station's own local
    // block.  Prioritized: the target is 0, which drives the absolute ambiguity to 0 so
    // that the phase bias absorbs this receiver's lump.  eq34: one entry per
    // spanning-tree edge (r, s) rooted at this station, with target 0 on first sighting
    // and λ·round(carried float) on a re-pin (pin-to-round, so the emitted-bias frame
    // step is an integer number of cycles, usually 0).  Cleared and rebuilt every epoch
    // by the serial election (PASS A tail).  Empty on the legacy single-pivot path,
    // where the pivot pins the whole shared set via is_pivot.  A station gauges some
    // (sat, signal) pairs and is an ordinary carry-eligible station for the rest:
    // membership is per (sat, signal), never a station-level role.
    std::map<std::string, double> pinned_amb_targets;
    // eq34 only (card 3): the "<sys>_RPB_<σ>" receiver-phase-bias keys for which this
    // station is the datum receiver r1 this epoch, each folded as a 1e10 pin on the
    // RPB column in this station's own block (δ_{r1,σ} = 0, the receiver half of the
    // single-receiver-chain gauge).  Cleared and rebuilt every epoch by the chain
    // election.  Empty except at an eq34 component's datum receiver, and always empty
    // when network.phase_datum_eq34 is off.
    std::set<std::string> pinned_rpb_keys;
    // Per-pin σ multiplier for the few entries of @ref pinned_amb_targets that had to
    // be pinned on a host younger than the eligibility threshold (eq34 reattach
    // hygiene, docs/SPEC_eq34_reattach_hygiene.md, INC-3).  The fold divides the 1e10
    // hard-pin weight by scale²: the pin still fixes the gauge, but the datum row is
    // soft enough for the data to pull the cold host's ambiguity during the ramp
    // instead of re-levelling the whole component onto its code-seeded value.  An
    // absent key means scale 1 and the legacy 1e10 weight, so a run with the feature
    // off never reads this map and the fold is byte-identical.  Cleared and rebuilt
    // with the pins.
    std::map<std::string, double> pinned_amb_sigma_scale;
    // Seed-round, fold-deferred path (docs/SPEC_eq34_seed_round.md §3.2, card 3):
    // the wavelength of each elected pin whose ambiguity state did not exist at
    // election time (a cold start, or a new arc that PASS B's pre-pass allocates), so
    // the election had no value to round.  The fold computes target = λ·round(piv_x/λ)
    // from this map and reports the realized target back through
    // StationAssembly::pin_seed_targets.  Empty unless
    // network.phase_datum_eq34_seed_round is on; cleared and rebuilt with the pins.
    std::map<std::string, double> pinned_amb_seed_lambda;

    // --- flicker/gap signal integrity (docs/SPEC_flicker_gap_integrity.md, INC-2) ---
    // Per-satellite σ multiplier for this station's phase rows while that line of
    // sight is unvetted: not @c Stable because it re-entered, flickered, degraded or is
    // new, which places it inside the structural window where no time-difference
    // detector can operate (MW needs Welford history, IF-TD needs three consecutive
    // epochs, GF is barred by policy at 30 s).  The split at the fold divides the row's
    // information by scale².  Code rows are untouched: they are slip-immune, and they
    // are what re-anchors the float.  The key is the satellite rather than the
    // ambiguity key because the demotion is a property of the line of sight: across a
    // loss of lock the slip magnitude is unbounded on every band at once, so demoting
    // one band while trusting the other would treat an absence of evidence as evidence
    // of no slip.  An absent key means scale 1 and the legacy 1/σ² weight, so a run
    // with the feature off never reads this map and the assembly is byte-identical.
    // Cleared and rebuilt each epoch by @ref networkSignalIntegrity before the parallel
    // PASS-B produce reads it (station-local, so thread-safe).
    std::map<std::string, double> phase_sigma_scale;
    /// Per-satellite sigma scale for this station's code rows while that line of sight
    /// is declared gross (card GL, docs/SPEC_event_harness.md §24.18).  The code-row
    /// counterpart of @ref phase_sigma_scale above, under the same contract: the fold
    /// divides the row's information by scale^2 (sigma multiplied by scale); phase rows
    /// are untouched, because they have their own responses (the erase and pending
    /// reseed that the declaration already performed); an absent key means scale 1, the
    /// legacy 1/sigma^2 weight.  Cleared and rebuilt by
    /// @ref networkGrossLosPublishScales, serially at the end of the previous epoch's QC
    /// block, before the parallel PASS-B produce reads it (station-local, so
    /// thread-safe).  Never populated when network.gross_los_screen is off, so such a
    /// run never reads this map and the assembly is byte-identical.
    std::map<std::string, double> gl_code_sigma_scale;
    /// Code rows this station folded at the deweighted sigma over the run (card GL).
    /// Accumulated inside the parallel produce phase, so it lives per station; summed
    /// into the run-end report at teardown, like @c si_rows_demoted.
    long long gl_rows_deweighted{ 0 };
    /// Phase rows this station folded at a demoted weight over the run (the row
    /// telemetry of signal integrity INC-2).  Accumulated inside the parallel produce
    /// phase, so it lives per station; summed into the run-end report at teardown, like
    /// @c sd_ewl_ctr.
    long long si_rows_demoted{ 0 };
    /// Code rows of an extra-band signal (one whose band is neither band of the
    /// constellation's ionosphere-free pin pair) that this station folded at the scaled
    /// sigma over the run (`network.extra_band_code_sigma_scale`, card L5P).
    /// Accumulated inside the parallel produce phase, so it lives per station and is
    /// summed into the run-end report at teardown, like @c gl_rows_deweighted.
    long long eb_rows_deweighted{ 0 };
    /// Code rows for which the scale could not be decided, because the row's
    /// constellation has no resolvable ionosphere-free pin pair in the BIA product
    /// (card L5P).  Those rows keep their unscaled weight; a nonzero count means the
    /// knob is partly inert, and the run-end line says so (standing invariant #4).
    long long eb_rows_no_pin{ 0 };
    /// The receiver IFB columns network.estimate_rx_extra_band_code_delay estimates at
    /// this station (card L5B): the counts and edge state of
    /// network/assembly/NetworkParamInventory.hpp, written by the station's own split and inject
    /// inside the parallel produce phase and by the serial harvest, summed by the run-end
    /// report.  Untouched unless the knob is effective.
    RxIfbStationState l5b;

    // --- second code channel, card SCC (docs/refound/SCC_second_code_channel_2026-09-21.md)
    /// This epoch's companion cells at this station, as "<satellite>|<signal>" keys
    /// (the shape of @c StateKeys::makeSatBiasKey).  Republished serially from the
    /// PASS-A observations before the parallel PASS-B produce reads it (station-local,
    /// so thread-safe).  It lets @ref Application::networkSplitStation_ tell a
    /// companion row from a fallback row: both route through the receiver inter-signal
    /// state, and only the companion arrived beside its canonical code.  Empty on every
    /// unarmed posture, so the split never reads it and the assembly is byte-identical.
    std::set<std::string> scc_cells;
    /// The previous epoch's @ref scc_cells, so that the serial sweep can diff the two
    /// and emit the opening and closing edges (standing invariant #7; card SCC, R7).
    /// Not cleared between epochs: a station absent this epoch keeps its set, and its
    /// cells close when it returns without them.
    std::set<std::string> scc_cells_prev;
    /// Receiver inter-signal state keys this station held at the end of the previous
    /// epoch (card SCC, R7).  A key that is absent here and present now, at a station
    /// that has held it before, is a reseed: the state cold-started instead of being
    /// restored by the receiver code-bias carry.  A reseed is the one way card SCC can
    /// silently determine a receiver bias from a single epoch of code.
    std::set<std::string> scc_isc_prev;
    /// Receiver inter-signal state keys this station has held at any time in the run
    /// (card SCC, R7).  The reseed test needs "held before", not "held last epoch": a
    /// state that legitimately disappears for an hour and returns is still a reseed.
    std::set<std::string> scc_isc_seen;
    /// Companion code rows this station folded over the run (card SCC, R7).
    /// Accumulated inside the parallel produce phase, so it lives per station and is
    /// summed into the run-end report at teardown, like @c eb_rows_deweighted.
    long long scc_rows{ 0 };
    /// Code rows this station routed through the receiver inter-signal state that were
    /// not companions, i.e. a band-1 code that arrived without its latched canonical
    /// code beside it (the fallback case that R2 covers; card SCC, R7).  On a lineage
    /// whose canonical latch is decided this reads 0; a nonzero count means some
    /// station's canonical code is regularly missing, which points at the latch.
    long long scc_fallback_rows{ 0 };

    // --- second phase channel, card SID phase 6a (docs/refound/SID_signal_identity_2026-09-22.md)
    /// This epoch's second-phase cells at this station, as
    /// @c StateKeys::makeSatPhaseBiasKey keys.  Republished serially from the PASS-A
    /// observations before the parallel PASS-B produce reads it (station-local, so
    /// thread-safe).  It lets @ref Application::networkSplitStation_ tell a
    /// second-phase row from the latched one; nothing else distinguishes them, because
    /// card SID phase 6a builds no receiver state that a row could be keyed on.  Empty
    /// on every unarmed posture, so the split never reads it and the assembly is
    /// byte-identical.
    std::set<std::string> spc_cells;
    /// The previous epoch's @ref spc_cells, so that the serial sweep can diff the two
    /// and emit the opening and closing edges (standing invariant #7; card SID phase
    /// 6a, R7).  Not cleared between epochs: a station absent this epoch keeps its set,
    /// and its cells close when it returns without them.
    std::set<std::string> spc_cells_prev;
    /// Every second-phase cell this station has opened during the run (card SID phase
    /// 6a, R7).  The run-end "arcs born" count is its size summed over stations; a
    /// re-opening after a dropout is an edge, not a birth.
    std::set<std::string> spc_cells_seen;
    /// Second-phase rows this station folded over the run (card SID phase 6a, R7).
    /// Accumulated inside the parallel produce phase, so it lives per station and is
    /// summed into the run-end report at teardown, like @c scc_rows.
    long long spc_rows{ 0 };
    /// Between-channel witness memory (card SID phase 6a, R8), keyed
    /// "<satellite>|<second signal>" -> (latched minus second phase, in cycles; the
    /// loop epoch of the measurement).  Differencing this quantity from one epoch to
    /// the next turns a one-channel slip into an integer step, while a slip common to
    /// both channels cancels.  Station-local and written only by the serial sweep.
    /// Empty unless second-phase rows are active
    /// (@c GnssConfig::networkSecondPhaseRowsActive).
    std::map<std::string, std::pair<double, long long>> spc_witness;
};

/// One SD extra-wide-lane integer decision for a (station, constellation, satellite),
/// as published by @ref networkSdEwlAr (increment 2a: Route B plus the A2 DD-to-SD
/// datum) and consumed by @c Application::networkAppendSdEwlRows_ (increment 2b).
///
/// @c n_cyc is in the r0-anchored frame: it equals the true undifferenced EWL integer
/// plus a per-station constant, which cancels in the between-satellite difference,
/// and a per-satellite constant set by the component reference station.  Only the SD
/// difference @c n_cyc(s) − @c n_cyc(pivot) is meaningful, never the value itself.
///
/// @c sig1 and @c sig2 record the network-common channel pair the integer was decided
/// on.  The row builder must fold on that exact pair: the satellite bias combination
/// @f$B_{EWL}^s@f$ is per signal, so a row built on a different tracking channel would
/// assert an integer derived from a different bias frame.
struct SdEwlFix {
    long long   n_cyc{ 0 };        ///< propagated SD-frame integer, in cycles of lambda_EWL.
    std::string sig1, sig2;        ///< the network-common phase pair it was decided on.
    double      frac_cyc{ 0.0 };   ///< |fractional residue| of the DD that produced it (0 for the link or reference satellite, whose value is a datum choice, not a measurement).
    double      ffr{ 1.0 };        ///< rounding success probability of that DD (1 for the link/reference satellite).
    bool        usable{ false };   ///< passed the frac and FFR gates and was not vetoed by a flip this epoch.
    /// True when this value came from a rounded double difference rather than from a
    /// frame choice.  Datum values (the reference station's own satellites, each
    /// station's link satellite, and a satellite framed on first sighting) carry no
    /// rounding decision and are ungated by construction: no FFR or |frac| setting can
    /// reject them.  A whitened group built only from datum values would assert a hard
    /// constraint with no integer information behind it while removing a real local
    /// degree of freedom, and it would make the feature impossible to disarm through
    /// its own gates.  The row builder therefore requires at least one measured integer
    /// per fixed group.
    bool        measured{ false };
};

/// What the D2 fold does with one carried border entry this epoch (card PRM phase 1b,
/// section 4.4).  Decided once per epoch, before PASS B, by
/// @ref networkCarryDecide, and read by every consumer of the decision: the exact
/// coupling update's rows (through @c cxu_epoch), the fold itself and the run-end
/// tally.  The card's seven names map as: Anchored and Loose (a satellite clock under
/// the Q3 anchor), Reinit, Reref and Parked as named; the card's Skipped is split into
/// @c Held, @c OnOscillator, @c Screened and @c Retired; the card's Departed is the
/// decision's @c departed flag (a foldable entry whose column left the layout, which the
/// estimator marginalizes); @c Folded (a bias at its kind's process noise, or a clock with
/// the anchor disarmed) and @c Seeded (READMIT) are the two the card did not list.
enum class CarryDisposition : int {
    Undecided = 0,  ///< not decided this epoch (a fresh stash or restore entry)
    Folded,         ///< folded at its kind's process noise over the gap
    Anchored,       ///< a satellite clock folded at the Q3 anchor's rate (card Q3)
    Loose,          ///< a satellite clock the armed anchor does not hold: the legacy rate
    Reinit,         ///< folded at the clock monitor's reinit variance, decorrelated (inflate_q)
    Seeded,         ///< a parked pre-gap realization seeding its re-registered column (READMIT)
    Reref,          ///< marginalized this epoch: its phase-bias frame was re-referenced
    Held,           ///< folded by another prior: the SH inject (VtecSh)
    OnOscillator,   ///< a satellite clock carried by the clock oscillator model instead
    Parked,         ///< a departed satellite's realization, not folded (column absent or past the seed horizon)
    Screened,       ///< not folded: the effective variance came out non-positive
    Retired,        ///< not folded: a clock-pivot handover retired its datum (decided by the fold)
};

/// Number of @ref CarryDisposition values (array sizing of the run-end tally).
inline constexpr int kCarryDispositions = 12;

/// Short lower-case name of a disposition, for the run-end table and the manifest.
inline const char* carryDispositionName(CarryDisposition d) {
    switch (d) {
        case CarryDisposition::Undecided:    return "undecided";
        case CarryDisposition::Folded:       return "folded";
        case CarryDisposition::Anchored:     return "anchored";
        case CarryDisposition::Loose:        return "loose";
        case CarryDisposition::Reinit:       return "reinit";
        case CarryDisposition::Seeded:       return "seeded";
        case CarryDisposition::Reref:        return "reref";
        case CarryDisposition::Held:         return "held";
        case CarryDisposition::OnOscillator: return "on_oscillator";
        case CarryDisposition::Parked:       return "parked";
        case CarryDisposition::Screened:     return "screened";
        case CarryDisposition::Retired:      return "retired";
    }
    return "?";
}

/**
 * @brief One carried border entry's fold decision for this epoch (card PRM phase 1b).
 *
 * Holds the route and the process-noise increment, not the folded variance: a consumer
 * forms the variance from the carried variance it holds with
 * @ref carryFoldVarEff, so a re-expression of the carry between the decision and a
 * consumer (the clock-pivot handover S-transform adds @f$v_i^2\sigma_q^2@f$ to a clock's
 * variance after the coupling rows are built) changes the base and not the decision.
 * Every member has a default, because the stash and the warm resume default-construct
 * @ref CarryPrior; the decision pass overwrites every entry, parked ones included,
 * before any reader runs.
 */
struct CarryFoldDecision {
    CarryDisposition disposition{CarryDisposition::Undecided};
    /// The increment over the carried variance before any floor (m^2): q·gap of the kind,
    /// or for a clock under the armed anchor its own increment (cta_q·dt_sat anchored,
    /// the legacy q over the gap loose).  0 for the dispositions that do not fold.
    double q_applied_m2{0.0};
    double seed_gap_s{0.0};     ///< Seeded: the time since the park (s); 0 otherwise
    bool   anchored{false};     ///< the anchor holds this clock (its floor applies)
    bool   decoupled{false};    ///< the anchor found an anchored clock's carry stale this epoch
    bool   departed{false};     ///< a foldable entry whose shared column is absent from this epoch's layout
    bool   has_seam{false};     ///< card CTU G5: a switch-on seam's level noise applies (dense fold only)
    double seam_q_m2{0.0};      ///< that noise (m^2)
    /// The interval the increment covers (s) and its rate (m^2/s), q_applied_m2 = rate·dt
    /// before card PFQ's term.  Set for a folding disposition.
    double dt_applied_s{0.0};
    double rate_m2ps{0.0};
    /// Card PFQ (network.clock_pivot_frame_q): the pivot's rate this clock's term uses
    /// (m^2/s), 0 when the clock takes no term (the knob off, the pivot itself, a reinit, a
    /// non-folding disposition, another kind).  The term r_p·dt_applied_s is inside
    /// q_applied_m2; the dense fold and the exact update add r_p·min(dt_a, dt_b) between
    /// two such clocks of a constellation.
    double pfq_rate_m2ps{0.0};
    double pfq_add_m2{0.0};     ///< card PFQ: the diagonal term inside q_applied_m2 (m^2), 0 without
    double var_decided_m2{0.0};   ///< the carried variance when decided
    double value_decided_m{0.0};  ///< the carried value when decided
};

/// One shared parameter carried across epochs (D2): its value and posterior
/// variance, re-keyed by (kind, sat, signal) identity so it survives layout
/// changes (a satellite blinking in/out).
struct CarryPrior {
    SharedParamKind kind;
    std::string     sat;
    std::string     signal;
    int             sh_n{-1};    ///< VtecSh degree (VtecSh only)
    int             sh_m{-1};    ///< VtecSh order  (VtecSh only)
    char            sh_trig{0};  ///< VtecSh 'C'/'S' (VtecSh only)
    int             sh_layer{0}; ///< VtecSh shell index (VtecSh only; 0 = legacy layer)
    double          value;
    double          var;
    bool            inflate_q{false};  ///< set when the clock monitor flagged this satellite: the next epoch folds it with the large reinit variance instead of the stiff process noise (a soft large-q reinitialization that lets a frozen clock move toward the data while keeping a weak anchor).
    /// Continuous-GPS second at which this entry's shared column left the layout
    /// (0 = live; READMIT continuity, the E2 seed-first re-admission,
    /// network.readmit_seed_horizon_s; SPEC_synthetic_minimum_network §18.4,
    /// SPEC_amb_iono_split_overcount §7 item FIRST).  A parked entry is the satellite's
    /// pre-gap realization, held for the seed horizon.  It is never folded while its
    /// column is absent, it is walked by every frame re-base that walks @c prev_carry,
    /// and it is excluded from the snapshot.  On re-registration it seeds the column's
    /// D2 prior at @c var + q·gap, so the Q3 anchor folds the column anchored over the
    /// true gap instead of tripping `gap` into a code-only loose re-draw, which the
    /// anchor's own re-coupling would then freeze into a tail.
    double          t_park_s{0.0};
    /// This epoch's fold decision (card PRM phase 1b): written by
    /// @ref networkCarryDecide before PASS B, read by the coupling rows and the fold.  It
    /// lives from the decision to the stash, which rebuilds the entries; a parked entry is
    /// copied whole by the stash and re-decided at the next epoch's pass.
    CarryFoldDecision fold{};
};

/// Re-admission continuity constants (READMIT).  @c kReadmitMinGapEp: the
/// re-admission detector's minimum absence in loop epochs (a single withheld epoch
/// skips the stash for every satellite and must not read as a network-wide
/// re-admission).  @c kReadmitVerifyWindowS (s): a Q3 nis/abs trip this soon after a
/// clock seed is attributed to the seed, meaning the data rejected it (the
/// mark-if-moved rule of invariant #7).  @c kReadmitSeedK: the standardized-residual
/// threshold of the stash-side seed-vs-solve check (telemetry only).
inline constexpr long long kReadmitMinGapEp      = 3;
inline constexpr double    kReadmitVerifyWindowS = 300.0;
inline constexpr double    kReadmitSeedK         = 6.0;

/// Identity key of a dense-carried shared parameter (C1).  Mirrors the internal key
/// construction of NetworkEstimator so that the stash and the fold agree.  SH-VTEC
/// never enters the dense block (its carry is the diagonal pre-gate fold), so the
/// (n, m, trig) family needs no key here.
inline std::string sharedCarryKey(SharedParamKind kind, const std::string& sat,
                                  const std::string& signal) {
    switch (kind) {
        case SharedParamKind::Clock:     return sat + "|CLK";
        case SharedParamKind::CodeBias:  return sat + "|CB|" + signal;
        case SharedParamKind::PhaseBias: return sat + "|PB|" + signal;
        default:                         return sat;  // unreachable for the dense carry
    }
}

/// Dense posterior covariance block over the D2-carried Clock, PhaseBias and CodeBias
/// entries of @c prev_carry (C1, network.shared_carry_full_cov).  Stashed beside
/// @c prev_carry at emit and folded the next epoch as one information block
/// Λ = (P_prev + Q)⁻¹ (@ref NetworkEstimator::addSharedPriorDense), which is the
/// information-form Kalman time update on the shared border.  Rows are looked up by
/// the same (kind, sat, signal) identity key as the estimator layout, so the block
/// survives cross-epoch layout changes and every existing @c prev_carry value
/// mutation (the ephemeris-cutover re-level) without re-indexing.  @c prev_carry
/// remains the single source of truth for carried values and flags; this block is a
/// passive covariance side-car.  When it is empty, the fold falls back per key to the
/// legacy diagonal path (toggle off, warm resume from the v1 snapshot, or a
/// stash-side non-finite screen).
struct SharedCarryCov {
    std::map<std::string, std::size_t> row;  ///< identity key → row/col in P
    Eigen::MatrixXd P;                       ///< dense posterior covariance (m²)
    void clear() { row.clear(); P.resize(0, 0); }
};

/// One non-negligible entry of a carried local column's regression @f$M_r@f$ on the
/// shared border (card 1, network.carry_conditional_coupling).  The coupling makes the
/// between-epoch carry conditional (a pseudo-observation row over @f$(x_s\mid x_r)@f$)
/// rather than an independent marginal.  The entry holds the shared parameter's
/// identity (kind plus sat/signal, or SH n/m/trig), so that it re-maps across the
/// epoch's changing shared-column layout; the coefficient @f$M_{rj}@f$; last epoch's
/// solved value @f$\hat s_{\text{prev},j}=x_s(j)@f$ (the coupling-row RHS term); and
/// last epoch's datum-frame marginal variance @f$P_{\text{prev},jj}@f$, used to
/// marginalize the term into the row variance when the shared column has departed
/// this epoch.  Datum-pinned and folded-gauge columns are carried with their
/// coefficient but with @c p_prev = 0, because they are exactly known (the datum
/// re-applies them each epoch).  Keeping them preserves the row's exact gauge
/// invariance (card 1e); without them every coupling row observes the clock gauge,
/// and the W5 null-space gate withholds the rows every epoch.
struct CoupledShared {
    SharedParamKind kind{SharedParamKind::Clock};  ///< shared parameter kind
    std::string sat;      ///< satellite id (Clock/PhaseBias/CodeBias)
    std::string signal;   ///< signal id (PhaseBias/CodeBias)
    int  n{0};            ///< SH degree (VtecSh)
    int  m{0};            ///< SH order  (VtecSh)
    char trig{'C'};       ///< SH cos/sin (VtecSh)
    int  sh_layer{0};     ///< SH shell index (VtecSh; 0 = legacy layer)
    double coeff{0.0};    ///< @f$M_{rj}@f$ (regression coeff of the local on this shared col)
    double s_prev{0.0};   ///< @f$\hat s_{\text{prev},j}=x_s(j)@f$ last epoch (correction frame)
    double p_prev{0.0};   ///< @f$P_{\text{prev},jj}@f$ last-epoch datum-frame variance (departed-col marginalization)
};

/// Relative sparsification threshold for a carried column's regression row: an
/// entry @f$M_{rj}@f$ is stashed only when @f$|M_{rj}| > @f$ @c kCouplingSparsifyRelThresh
/// @f$\cdot\max_k|M_{rk}|@f$ (Card 1).  1e-8 keeps only the numerically meaningful
/// couplings; the dropped near-zero entries contribute negligibly to the row.
inline constexpr double kCouplingSparsifyRelThresh = 1e-8;

/// Pre-whitening ingredients of one conditional coupling row: one column of a station's
/// whitened local block (F3; @ref PlBlockRaw), retained so that the block can be
/// re-whitened if a within-epoch RAIM refloat later drops some of its columns.
/// @c shared holds the present shared terms, kept as ordinary @f$A_s@f$ coefficients as
/// in card 1; the departed terms are already marginalized into @c var_eff.
struct CplRowRaw {
    Eigen::Index local_col{0};                            ///< carried local (A_r) column this row targets (+1).
    std::vector<std::pair<Eigen::Index, double>> shared;  ///< present shared terms: (shared col, M_rj).
    double rhs{0.0};                                      ///< (r̂_prev − r_lin) + Σ_present M_rj·ŝ_prev,j.
    double var_eff{0.0};                                  ///< pure conditional variance (plus process noise); this row's own diagonal.
    /// The row's contribution tag (card NOM N2a): its kind and the identity of its local
    /// column, carried with the ingredients so that a re-whiten keeps the tags aligned.
    /// Default @c Unset (caught by the tag check).
    RowTag tag{};
};

/// One station's jointly whitened phase-leveling carry block (F3,
/// network.phase_leveling_local_block, SPEC_amb_iono_split_overcount): the
/// pre-whitening ingredients, retained per station so that the block can be whitened
/// at build time and re-whitened by the within-epoch RAIM refloat with the refloated
/// columns marginalized (their rows and columns deleted from Σ before the factor,
/// never conditioned on; the decoupling edge of standing invariant #7).  @c rows holds
/// the per-column scalar-path ingredients: the same +1 local coefficient, the present
/// shared terms (κ-shrunk per card 1d, gauge-restored per card 1f), the RHS, and
/// σ² = var_eff + Σ_departed M²P_prev + Σ_present M²κqΔt, which the card 1 scalar row
/// would have carried.  Here @c var_eff is that σ², i.e. the diagonal of Σ; the
/// departed-column marginalization stays per row and diagonal, as on the scalar path.
/// @c Sigma is the n×n joint prior covariance over those columns: that diagonal, and off
/// the diagonal the harvested conditional cross covariances (N_rr⁻¹)_{c_i c_j} (scaled
/// by the VCE factor), or 0 where the pair was harvested at different epochs.  Σ is then
/// block-diagonal by harvest stamp, each block a principal sub-matrix of one posterior,
/// so it is PD by construction.
/// @c row0 is the block's first row index within the station's coupling block (block
/// row k sits at A_s/A_r row @c cpl_row_start + row0 + k).  Empty unless the knob
/// built a block for this station this epoch.
struct PlBlockRaw {
    std::vector<CplRowRaw> rows;    ///< per-column ingredients (the departed terms are already folded into @c var_eff)
    Eigen::MatrixXd        Sigma;   ///< n×n joint prior covariance (m²)
    Eigen::Index           row0{0}; ///< offset of the block's first row within the coupling block
};

/// One station-epoch's harvested conditional cross-covariance block over its carried
/// ambiguity columns (F3, network.phase_leveling_local_block):
/// C(a,b) = (N_rr⁻¹)_{c_a c_b} = n_{c_a}(c_b), the N_rr⁻¹e_c solve of column a read
/// at column b.  @ref NetworkNormalEquations::stationRegression already performs that
/// solve per column for @c cond_var and @c row, so the cross terms are a gather, not
/// a second solve (LocalCarry::var is the diagonal of C).  Row a is NaN when column
/// a's harvest failed.  Shared by pointer among the station's @ref LocalCarry entries
/// stashed at that harvest, each holding its own index.  "Same harvest" is pointer
/// equality, so a pair whose two entries point at different blocks (one column
/// re-harvested later than the other, or a warm-resume-restored entry with no block)
/// has no valid cross term; it is zeroed and counted at inject.  Released when the
/// last entry referencing it is re-stashed or pruned.  Not snapshotted, by design:
/// the first inject after a resume degrades to the diagonal block for one epoch
/// (visible as pair-miss counts), and the first harvest after the resume rebuilds it.
struct PlHarvestBlock {
    Eigen::MatrixXd C;        ///< n×n conditional cross covariance (m²); NaN rows = failed harvests
    double          stamp_s{0.0}; ///< continuous-GPS seconds of the harvest (diagnostic)
};

/// One carried-forward ambiguity (phase leveling, increment 1): the float ambiguity's
/// absolute value, its posterior variance, and the continuous-GPS second at which it
/// was last seen (for the inter-epoch gap inflation and the stale-carry drop).  Keyed
/// in @c local_carry by (station index, satellite, signal), so it survives a
/// satellite leaving and re-entering.
struct LocalCarry {
    double value_abs;     ///< Carried absolute ambiguity estimate (m).
    double var;           ///< Last-solve posterior variance (m²): the marginal (conditionalLocalVarianceDiag) on the legacy diagonal carry, or, under network.carry_conditional_coupling, the pure conditional @f$(N_{rr}^{-1})_{rr}@f$.
    double last_seen_s;   ///< Continuous-GPS seconds when last stashed (gap inflation).
    std::vector<CoupledShared> coupling{};  ///< The regression on the free shared border (card 1).  Empty on the legacy diagonal path, and for a warm-resume-restored entry unless network.resume_coupling_restore rebuilt it from a v3 snapshot's QL/QZ record (§18.12); empty on a v1/v2 file or with the knob off.  An empty coupling injects as a plain diagonal row (a one-epoch degradation).
    std::shared_ptr<const PlHarvestBlock> blk{};  ///< The harvest cross-covariance block that holds this entry's off-diagonals (F3, network.phase_leveling_local_block).  Null when the knob is off, on the ZWD carry, and for a warm-resume-restored entry; with no block, every pair with this entry is zeroed and counted at inject.
    int blk_idx{-1};                              ///< This entry's row and column in @c blk->C (−1 = none; F3).
    double var_first{0.0};                        ///< The arc's first-stash conditional variance (m²; 0 = unknown): set once at birth and kept across re-stashes (an erase-and-rebirth starts it over; a re-level keeps it).  The unfloored σ-scale denominator of network.phase_datum_handover_max_var_ratio; snapshotted only when that knob is armed.
    /// Restored-arc sentinel for @ref birth_ep (SPEC_event_harness §15.4).  A
    /// warm-resume-restored entry is stamped with this value instead of 0, because 0
    /// means "never stashed" to the harvest's first-stash-wins rule: a restored arc
    /// left at 0 would be stamped as born at the resume epoch by the first harvest
    /// after the resume, and every age gate would then see a network of newborns for
    /// @c min_age epochs.  A negative stamp is unreachable by the harvest
    /// (@c n_loop_epochs is incremented before the harvest, so a genuine first stash
    /// is ≥ 1), survives the `!= 0` first-stash test, and reads as mature at every age
    /// reader (all of which test `> 0`).
    static constexpr long long kBirthEpRestored = -1;
    /// Restored-arc sentinel for @ref birth_s: the seconds counterpart of
    /// @ref kBirthEpRestored, under the same rule (a real stamp is a continuous-GPS
    /// second, so it is always ≫ 0).
    static constexpr double    kBirthSRestored  = -1.0;
    long long birth_ep{0};                        ///< Loop-epoch index (n_loop_epochs) of the arc's first stash: set once at birth, kept across re-stashes, restarted by an erase-and-rebirth.  0 = never stashed, @ref kBirthEpRestored (−1) = warm-resume-restored with its true birth not snapshotted.  Both read as mature (every reader tests `> 0`).  The harvest's first-stash-wins rule tests `!= 0`, so the −1 sentinel survives it, while a 0 is replaced by the current epoch at the next stash (@ref localCarryStampBirth).  The age gates read it through @ref localCarryYoung.  Not snapshotted: a restored entry is sentinel-stamped and reads as mature for its whole life (until an erase-and-rebirth), which is the documented restored-entry contract.
    double birth_s{0.0};                          ///< Continuous-GPS seconds (week*604800 + tow) of the arc's first stash: the seconds counterpart of @c birth_ep, set once at birth from the harvest's @c pl_now_s, kept across re-stashes, and restarted by an erase-and-rebirth (a T4 attribution erase deletes the entry, and the next harvest re-creates it with a level that already contains any earlier slip).  Read by the sim-only slip oracle (@c network.cycle_slip.oracle_relevel) as the hard lower edge of its truth-slip window, so a repair never re-applies a jump that the re-created level already absorbed.  Not snapshotted (like @c birth_ep): a warm-resume-restored entry carries @ref kBirthSRestored (−1), which never wins the window's `max` and so leaves the window at its @c oracle_window_s latency bound for the arc's whole restored life.  That is acceptable for a sim-only diagnostic and errs in the conservative direction.
    /// Continuous-GPS seconds of the arc's last re-level (introduced as the age clock of
    /// the retired young-arc variance inflation, @c network.phase_leveling_young_inflate_var_m2;
    /// SPEC_amb_iono_split_overcount §6.17–§6.18).  The harvest sets it to its
    /// @c pl_now_s (@ref localCarryStampRelevel) on two events, both of which produce a
    /// level whose value is of code quality while its harvested variance is
    /// phase-stiff (conditioned on the station's mature arcs):
    ///   * the entry is created (no previous entry): a first stash, or the re-stash
    ///     after a T4 attribution erase or a gap prune deleted the entry;
    ///   * the arc was freed this epoch by the slip response
    ///     (@c network.cycle_slip.relevel_free_epoch's latched @c free_ambs set, the
    ///     same set the PASS-B inject consulted), so this harvest re-levelled it from
    ///     one epoch of code.
    /// The float vehicle's drop (@c ambFloatDrop in NetworkAmbFloat.cpp) also sets it,
    /// because a drop doubts the level.  It is preserved across every ordinary
    /// re-stash, so the age keeps growing while the arc matures.  Read by the AR suspect
    /// ledger, the arc life stamp and the amb_state trace.  Not snapshotted (like
    /// @c birth_s and @c birth_ep): a warm-resume-restored entry reads 0 (unstamped).
    /// Unlike
    /// @c birth_s and @c birth_ep it needs no restore sentinel: it is stamped only on
    /// those events, never on an ordinary re-stash, so a restored entry's 0 survives
    /// every harvest until the arc is actually re-levelled.
    double relevel_s{0.0};
    /// The birth prior in force for this arc (m²), which a drop restores (RF-1
    /// §3.3(b), §7.2).  Set at the arc's first stash to the larger of @ref var_first
    /// and the elevation-mapped floor built from network.amb_float_birth_var_m2
    /// (@c ambFloatBirthFloor), kept across every re-stash and every re-level like
    /// @ref var_first, and restarted only by an erase-and-rebirth (which deletes the
    /// entry).
    ///
    /// It is a separate field from @ref var_first by design.  @c var_first is the
    /// arc's own first-stash conditional variance, a measurement of what the model
    /// reported, and it is the denominator of
    /// @c network.phase_datum_handover_max_var_ratio, which must keep reading the
    /// unfloored quantity.  @c var_birth is what the arc is worth at birth: the same
    /// number floored by the posture's code-noise scale, because two routes hand back a
    /// phase-stiff variance for a value of code quality (a band-scoped free determined
    /// against its sibling band's still-stiff carry, and the SH tie row that separates
    /// a new arc's ambiguity from its iono at the tie's optimistic precision; design
    /// §1.4).  Flooring @c var_first in place would silently move the handover ratio's
    /// denominator.
    ///
    /// Read by the drop (@c ambFloatDrop in NetworkAmbFloat.cpp, RF-1 step 4), which
    /// runs only under the float vehicle and restores the largest of var_birth,
    /// var_first and the floor.  With the default floor of 0.0 it equals @c var_first
    /// exactly.  Not snapshotted: design §6.1 names it as a field that must join the
    /// warm-resume record when seams come back, or a restored arc cannot be dropped
    /// correctly.
    double var_birth{0.0};
    /// Number of epochs in which this arc's carried prior was actually folded (RF-1
    /// §7.4): the abscissa of the D1 variance-trajectory fit (@c log var = a − b·log n
    /// over the first 30 folds) and the bucket key of the D4 σ̂₀(pl)-by-arc-age read.
    ///
    /// It cannot be derived from @ref birth_ep: an arc's age counts every epoch since
    /// its first stash, while the fold count skips the epochs in which the carry did
    /// not act (the free-epoch latch, the gap cliff, a pending reseed, a non-finite
    /// @c x_lin), which are the epochs RF-1 studies.  Incremented at the harvest when
    /// PASS B recorded a fold for the column this epoch (StationAssembly::amb_state_fold),
    /// preserved across every re-stash, and restarted only by an erase-and-rebirth
    /// (which deletes the entry).
    ///
    /// Trace support only: maintained while @c network.trace_amb_state is armed and
    /// read by nothing on the solve path, so an unarmed run never touches it.
    long long n_folds{0};
    /// Loop-epoch index (@c n_loop_epochs) of this arc's last drop (RF-1 §7.2): the
    /// re-coupling edge's own clock, distinct from @ref birth_ep.  0 = never dropped.
    /// Written by the float vehicle's drop (@c ambFloatDrop in NetworkAmbFloat.cpp),
    /// which also stamps @ref relevel_s, and carried forward unchanged by every
    /// re-stash.  Nothing in the producer reads it (the unit tests do): the drop's
    /// trace rows are written at the drop itself, and a repeated drop within one epoch
    /// is made idempotent by the drop's variance test.
    long long dropped_ep{0};
    /// Per-arc phase of the walk metric (RF-3a A2,
    /// docs/refound/RF-3_code_row_qc_2026-09-22.md §4).  A drop sets it to 1.  The next
    /// harvest of the arc records @c |value_abs(after) − value_abs(before)| as the
    /// change that the first fold after the drop absorbed and advances it to 2; the
    /// harvest after that records the same quantity as the residual walk and clears it.
    /// Measure-only, and maintained in both states of network.amb_float_drop_var_m2 so
    /// that a run with the drop floor off reports the walk it suffers: a stiff restored
    /// prior lets a large level error through a few metres per epoch over tens of
    /// epochs, while a floored one takes the correction at once.  Not snapshotted, like
    /// every other RF-1 stamp: a restored entry reads 0, which means no drop pending
    /// (the conservative direction).
    int drop_walk_ph{0};
    /// Card SCL2 X2 (network.carry_coupling_shared_history, filter/NetworkSharedHistory):
    /// this row's coefficients on the shared-history columns of the compression stamped
    /// @ref shx_stash_s (m; history column j has the prior N(0, 1)).  Valid only when
    /// @ref shx_stash_s equals @ref last_seen_s (the row was stashed at that compression);
    /// a re-stash without history terms leaves them stale and ignored.  @ref var then
    /// excludes the kept share (the squared norm of @ref shx_coef): the row's marginal variance is
    /// @c var plus that norm (what the snapshot writes).  Empty off and for a restored
    /// entry.
    std::vector<double> shx_coef{};
    double shx_stash_s{0.0};   ///< Card SCL2 X2: the compression (harvest second) @ref shx_coef belongs to (0 = none)
};

/// The route through which the PASS-B inject folded a carried ambiguity column this
/// epoch (RF-1 §7.4): the `route` column of @c network_amb_state.csv.
///
/// It is recorded at the inject rather than inferred at the harvest from the knob
/// posture, because the two can disagree on a single station-epoch:
/// @c CouplingRowBuilder::buildLocalBlock returns false on a non-SPD factor after its
/// ridge retry, and the caller falls back to per-column rows.  Under the float_kf
/// vehicle that fallback changes the vehicle for one station-epoch (design risk #3),
/// and this column makes it visible per arc rather than only as a run total.
enum class AmbFoldRoute {
    None,           ///< No prior was folded for this column this epoch (skipped: freed / stale-gap / pending reseed / no carry / non-finite / gauge-pinned).
    Diag,           ///< A plain diagonal local prior row (Λ, η): the legacy per-column route.
    Block,          ///< A row of the station-wide jointly-whitened local block (F3 / the float_kf vehicle's structural fold).
    CouplingScalar  ///< A card 1 conditional coupling row: +1 on the local column, M_r on the free shared border (the RF-0 route).
};

/// One row of @c network_amb_state.csv, the per-arc carry-state trace (RF-1 §7.4).
///
/// The fields follow the design's pre-registered header, in its order, so that a
/// reader (scripts/amb_state_read.py) can parse by name and a grader can reproduce
/// the pre-registered reads D1–D4 without re-deriving what a column means.  Rows are
/// buffered per epoch in @c NetworkCarryState and drained serially by
/// @ref networkAmbStateTrace, as @c sd_ewl_trace is, so the file order does not
/// depend on the thread count and the four erase sites (which run in different seams,
/// some after the harvest) can all contribute to the same epoch's block.
struct AmbStateRow {
    long long   week{0};            ///< GPS week of the epoch.
    double      tow{0.0};           ///< GPS time of week (s); the join key of every trace CSV in the run.
    std::string station;            ///< Station name.
    std::string sat;                ///< Satellite (e.g. G07).
    std::string signal;             ///< RINEX signal (e.g. L1C).
    double      value_abs_m{0.0};   ///< The carried absolute float ambiguity after this epoch's harvest (m).  On an erase row, the value the entry held when it was deleted.
    double      var_m2{0.0};        ///< The carried posterior variance after this epoch's harvest (m²); the conditional variance under card 1.
    double      var_first_m2{0.0};  ///< @ref LocalCarry::var_first: the arc's first-stash variance, unfloored.
    double      var_birth_m2{0.0};  ///< @ref LocalCarry::var_birth: the floored birth prior that the drop restores (RF-1 step 4).
    double      fold_var_m2{0.0};   ///< The variance the fold used this epoch (0 when @ref folded is false).
    long long   n_folds{0};         ///< @ref LocalCarry::n_folds after this epoch (the D1 abscissa).
    long long   age_ep{0};          ///< Loop epochs since the arc's first stash (−1 = warm-resume-restored; 0 = born this epoch, or an unstamped entry on an erase or drop row).
    long long   age_since_relevel_ep{0}; ///< Epochs since @ref LocalCarry::relevel_s (−1 = unstamped, read as mature; erase rows always carry −1 and drop rows 0).
    bool        folded{false};      ///< Did a carried prior actually act on this column this epoch?
    AmbFoldRoute route{AmbFoldRoute::None}; ///< …and through which route (@ref AmbFoldRoute).
    std::string event;              ///< "" | birth | drop:<cause> | erase:<cause> | stransform.
};

/// One carried-ambiguity entry as a warm resume restores it from a snapshot's
/// `L` / `Z` row.  This is the one place where the restored-arc age convention is
/// applied.
///
/// The snapshot does not serialize the arc's birth, by design (see
/// @ref LocalCarry::birth_ep): the contract is that a restored entry reads as mature,
/// because its true birth is at least as old as the snapshot, and refusing it would
/// refuse the whole network for @c min_age epochs after every resume.  Leaving the
/// fields at their 0 default does not deliver that contract: the harvest's
/// first-stash-wins rule reads 0 as "never stamped" and re-stamps it with the current
/// loop epoch, so the first harvest after a resume would mark every restored arc as
/// newborn (SPEC_event_harness §15.4).  The sentinel distinguishes "restored" from
/// "never stashed" without touching the snapshot format or the warm-resume
/// fingerprint.
///
/// @param value_abs   Carried absolute estimate (m), the snapshot's field.
/// @param var         Carried posterior variance (m²), the snapshot's field.
/// @param last_seen_s Continuous-GPS seconds of the stash that wrote the row.
/// @param var_first   First-stash variance if the row carried one (0 = absent).
/// @return A @ref LocalCarry with both birth stamps at their restored sentinels.
inline LocalCarry makeRestoredLocalCarry(double value_abs, double var,
                                        double last_seen_s,
                                        double var_first = 0.0) {
    LocalCarry lc{value_abs, var, last_seen_s};
    lc.var_first = var_first;
    lc.birth_ep  = LocalCarry::kBirthEpRestored;
    lc.birth_s   = LocalCarry::kBirthSRestored;
    return lc;
}

/// True when this carried entry is younger than the maturity bar.  One definition
/// with two readers, both in NetworkPhaseDatum: the held S-basis age gate
/// `network.phase_anchor_min_age_ep` and the cold anchor-fallback candidacy bar
/// `network.phase_datum_candidacy_min_age_ep`.
///
/// The restored-arc age convention: an entry whose @c birth_ep is not positive has no
/// usable age, whether it is 0 (never stashed, or restored without the sentinel) or
/// @ref LocalCarry::kBirthEpRestored (warm-resume-restored), and it is never young.
/// The conservative direction is to let the variance ceiling decide rather than
/// refuse a carry that the ledger says has converged.
///
/// @param lc          The station's carried entry for the arc.
/// @param now_ep      This epoch's @c n_loop_epochs.
/// @param min_age_ep  The maturity bar in loop epochs; ≤ 0 disables the test.
/// @return True only when the arc has a positive birth stamp and fewer than
///         @p min_age_ep loop epochs have elapsed since it.
inline bool localCarryYoung(const LocalCarry& lc, long long now_ep,
                            int min_age_ep) {
    return min_age_ep > 0 && lc.birth_ep > 0
           && (now_ep - lc.birth_ep) < static_cast<long long>(min_age_ep);
}

/// Stamp @p fresh with the arc's birth under the first-stash-wins rule; the one
/// definition of the rule that the phase-leveling harvest applies at every stash.
///
/// The "already stamped" test is `!= 0`, not `> 0` (SPEC_event_harness §15.4).  Both
/// non-zero states mean that the arc's birth is settled: a positive stamp is a real
/// first stash, and @ref LocalCarry::kBirthEpRestored marks a warm-resume entry that
/// is older than this run, with its true age not snapshotted, and therefore mature.
/// A `> 0` test cannot tell the sentinel from an unstashed 0, and would let the first
/// harvest after a resume re-stamp every restored arc as born at the resume epoch.
///
/// An erase-and-rebirth (a T4 attribution erase, a gap prune, a re-admission) needs
/// no clause here: it deletes the entry, so @p prev is null and the arc is stamped
/// with the current epoch, which is the correct birth for a re-born arc.
///
/// @param fresh   The entry about to be stashed (its birth fields are written).
/// @param prev    The station's previous entry for the same key, or null when
///                there is none (a first stash, or a re-stash after an erase).
/// @param now_ep  This epoch's @c n_loop_epochs (≥ 1 at any harvest).
/// @param now_s   This harvest's continuous-GPS seconds (@c pl_now_s).
inline void localCarryStampBirth(LocalCarry& fresh, const LocalCarry* prev,
                                 long long now_ep, double now_s) {
    fresh.birth_ep = (prev && prev->birth_ep != 0) ? prev->birth_ep : now_ep;
    fresh.birth_s  = (prev && prev->birth_s != 0.0) ? prev->birth_s : now_s;
}

/// The stamp rule of @ref LocalCarry::relevel_s, the arc's re-level clock; the one
/// definition that the phase-leveling harvest applies at every stash.
///
/// Two events, and only these two, stamp the arc with the harvest's time: the entry is
/// created (a first stash, or the re-stash after a T4 attribution erase or a gap prune
/// deleted the entry), or the arc was freed this epoch, so this harvest re-levelled it
/// from one epoch of code.  Every other re-stash keeps the previous stamp, so the age
/// keeps growing while the arc matures.  A re-level does not reset @c birth_s
/// (@ref localCarryStampBirth), and an ordinary re-stash does not reset this.
///
/// @param prev_relevel_s  The previous entry's stamp (0 when there was no entry).
/// @param created         True when no previous entry existed: a first stash, or the
///                        re-stash after a T4 erase / gap prune deleted the entry.
/// @param freed           True when the arc was in this epoch's latched free set (the
///                        same @c relevel.free_ambs the PASS-B inject consulted).
/// @param now_s           This harvest's continuous-GPS seconds (@c pl_now_s).
/// @return The stamp to store: @p now_s on either event, otherwise @p prev_relevel_s
///         unchanged.
inline double localCarryStampRelevel(double prev_relevel_s, bool created, bool freed,
                                     double now_s) {
    return (created || freed) ? now_s : prev_relevel_s;
}

/// One confident MW wide-lane integer that the MW drain defers to the IF-TD driver
/// (the only scope that holds the pooled IF delta and scale) for the float 2×2 delta
/// bridge; collected in @ref RelevelEpochState::mw_cands (SPEC_slip_relevel_walk §6).
struct MwBridgeCand {
    int         sta{-1};          ///< Station index.
    std::string sat, sig1, sig2;  ///< Satellite + the MW pair's phase signals.
    int         dn_wl{0};         ///< Confident ZD wide-lane integer (cycles).
    double      sigma_wl_cyc{0};  ///< WL confidence (cycles; MW frac, floored).
    double      lambda_wl{0};     ///< WL wavelength (m).
    bool        consumed{false};  ///< Set when the IF-TD driver bridged it.
};
/// One pended slip awaiting its MW wide-lane integer (bridge v2, pend-and-replace;
/// SPEC_slip_relevel_walk §8.2).  Created at the T1 flag epoch with the pre-slip
/// carry capture (the PASS-A carry still holds the previous harvest) and the
/// measured δIF.  The interim epochs run the v1 free re-level.  When the confident
/// MW integer arrives (in the same epoch via the candidate index, or one or two
/// epochs later via the MW drain), the 2×2 system is solved and the carry is
/// replaced with pre_slip + shift, gated by the kernel caps and by corroboration
/// with the free level.  The corroboration test rests on the §8.1 audit: correct
/// bridges err by at most 0.5 cycles against truth and wrong ones by 6–113 cycles,
/// while the interim free level sits within about 2 cycles of truth, so agreement
/// with it separates the two populations.
struct RelevelPend {
    double pre1{0}, pre_var1{0};   ///< Pre-slip carry (band 1): value_abs (m), var (m²).
    double pre2{0}, pre_var2{0};   ///< Pre-slip carry (band 2).
    double dif_m{0};               ///< T1-measured IF step, common-mode removed (m).
    double sigma_if_m{0};          ///< Its 1σ (m).
    double lambda_if{0};           ///< (1,1) IF wavelength (m).
    std::string sig1, sig2;        ///< The pair's phase signals.
    double tow_s{0};               ///< Continuous-GPS-s of the flag (expiry base).
    bool   has_dn{false};          ///< The MW integer has arrived.
    int    dn_wl{0};               ///< Confident ZD wide-lane integer (cycles).
    double sigma_wl{0};            ///< Its 1σ (cycles, floored).
    double lambda_wl{0};           ///< Wide-lane wavelength (m).  v3 also sets it
                                   ///  (without has_dn) at pend creation when the
                                   ///  GF row is captured: the kernel derives the
                                   ///  per-band frequencies from (λ_WL, λ_IF)
                                   ///  alone, so the E1+E4 route needs it too.
    // --- Bridge v3, E4: the GF-phase TD row on the same pair -----------------
    // d_gf = λ1·ΔN1 − λ2·ΔN2 + (α2−α1)·δI + ε (geometry/clock-free, undifferenced).
    // Its σ is empirical (the station's own clean-step EWMA gf_step_scale, which
    // includes the iono-TD content), never a model σ.  Together with δIF it closes
    // the (1,2) plane at 0.15–0.3 cycles without waiting for MW: whenever
    // σ_gf ≲ 0.75·σ_IF the GF row is worth as much as a correct WL integer, without
    // the risk of a wrong integer.
    bool   has_gf{false};
    double dgf_m{0};               ///< T1-measured ΔGF across the slip epoch (m).
    double sigma_gf_m{0};          ///< Its 1σ (m; empirical, floored).
};

/// Per-epoch state of the slip re-level response (SPEC_slip_relevel_walk §6;
/// network.cycle_slip.relevel_free_epoch, relevel_delta_bridge).
///
/// A detector's pending-reseed mark is consumed (arc re-seeded, flag cleared) by the
/// PASS-B measurement pre-pass (MeasurementModel::ensureRequiredStates) before the
/// inject-skip check at the leveling-carry fold reads it.  Without this state, the
/// intended "re-float from data this epoch rather than clamp to the stale carry"
/// does not happen for detector-caught slips: the pre-slip level is injected at
/// converged weight, and the level channels random-walk.  @c free_ambs is the latched
/// set of marks that the inject site consults instead of the consumed flag;
/// @c mw_cands defers the MW drain's confident WL integers to the IF-TD driver for
/// the float 2×2 delta bridge; @c free_ambs and @c bridged feed the harvest-side VCE
/// and prior-residual census guard, so that a freed or bridged arc's slip-scale
/// innovation is never ingested as a clean row.
///
/// Lifetime: one epoch.  Filled by the PASS-A serial drains, read by the PASS-B
/// inject and the post-solve harvest, cleared at the next epoch's PASS-A entry.  Not
/// snapshotted, by design (forward-only response policy; an epoch after a resume
/// degrades to the pre-fix path, the conservative direction).
struct RelevelEpochState {
    /// Freeing-cause bits (§7.6(c)).  A bitmask because one (station, amb key) can be
    /// freed by several detectors in the same epoch (an LLI assertion corroborated by an
    /// MW step marks twice).  The T4 bits name the RAIM erase causes (the injected-free
    /// drain also tags kCauseT4Attrib); the RAIM path itself never touches
    /// @c free_ambs (§7.6, confound (1)).
    enum : std::uint8_t {
        kCauseMw        = 1,   ///< MW wide-lane integer-jump drain.
        kCauseLli       = 2,   ///< Receiver LLI loss-of-lock drain.
        kCauseIfTd      = 4,   ///< IF-phase time-difference pool drain.
        kCauseT4Attrib  = 8,   ///< RAIM prominence-attribution erase (T4).
        kCauseT4Consens = 16,  ///< RAIM per-satellite consensus erase (T3).
        kCauseWindup    = 32,  ///< Wind-up accumulator restart response (§10.4).
        kCauseGap       = 64,  ///< Gap re-entry response (card G, §G3-W1): the arc came back across a tracking break that, by the raw level or the receiver's LLI, it did not survive.
        kCauseGfJump    = 128, ///< Arc-scoped geometry-free jump trigger (card ARL; network/slip/NetworkGfArcJump.hpp): the arc's GF step departed from its last clean step.
    };
    /// (station idx, amb key) marked pending-reseed by a detector this epoch.  The
    /// inject site skips these, which gives each marked arc its free epoch.
    std::set<std::pair<int, std::string>> free_ambs;
    /// Cause tag parallel to @c free_ambs (§7.6(c); the same one-epoch lifetime).
    /// Read downstream by the classifiers of a freeing (for example the second-phase
    /// census, the float vehicle and the signal roster).
    std::map<std::pair<int, std::string>, std::uint8_t> free_cause;
    /// Deferred MW candidates for the IF-TD-driver delta bridge.
    std::vector<MwBridgeCand> mw_cands;
    /// Arcs whose carry was delta-bridged this epoch (station, sat, signal), for the
    /// harvest-side statistics guard (excluded from LS-VCE, relabelled in prires).
    std::set<std::tuple<int, std::string, std::string>> bridged;

    void clear() {
        free_ambs.clear(); free_cause.clear(); mw_cands.clear(); bridged.clear();
    }
};

/// One present, carry-eligible (non-pivot) ambiguity this epoch, recorded in
/// PASS B for the post-solve harvest/stash: its A_r column, identity, and the
/// linearization point used this epoch (so the absolute value is
/// @c x_lin + x_r(col) and a carried prior targets @c value_abs − x_lin).
/// One present phase-colour column of a station-epoch (card LGM section 4.1),
/// recorded in PASS B for the post-solve harvest and the trace.
struct PlPc {
    std::string  sat;          ///< Satellite id (the state's key).
    Eigen::Index col{-1};      ///< A_r column of the state (= h2local[state index]).
    double       x_lin{0.0};   ///< Linearization point (the StateManager value; stays 0).
    double       elev_deg{std::numeric_limits<double>::quiet_NaN()};  ///< Elevation (deg) recorded at allocation.
    bool         carried{false};///< A carried prior was folded this epoch (else a birth row).
};

struct PlAmb {
    Eigen::Index col;     ///< A_r column of this ambiguity (= h2local[amb_idx]).
    std::string  sat;     ///< RINEX satellite id.
    std::string  signal;  ///< RINEX signal / IF combo.
    double       x_lin;   ///< This epoch's ambiguity linearization point (state value).
    std::string  amb_key; ///< Full ambiguity key (clears a one-shot IF-phase refloat mark post-stash).
    /// This LOS's elevation (degrees) this epoch, or −1 when the station's modeled
    /// observation set did not carry it (RF-1 step 4).  Recorded at the inject only
    /// when @c network.amb_float_birth_var_m2 is armed (otherwise the map walk is
    /// skipped), and read by the harvest to map the birth-prior floor as the code row
    /// weights are mapped: σ²(e) = σ²_zen / sin^{2p}(e).  A flat zenith floor would be
    /// optimistic at the low elevations that hold the metre-plus tail.
    double       elev_deg{-1.0};
};

/// ---------------------------------------------------------------------------
/// Two-edge anchor handover (docs/SPEC_pivot_visibility_gauge_defect.md §5.1)
/// ---------------------------------------------------------------------------
/// The phase-bias gauge is one constraint per (sat, signal): "station r*'s ambiguity
/// for this key = 0".  When r* changes (the clock pivot stopped seeing the
/// satellite, or the anchor election moved), re-pinning the successor at 0 realizes
/// a different frame, and the whole family steps by the inter-station ambiguity
/// difference, which the satellite clock absorbs.  A legacy pivot hands over in one
/// large step; the prioritized election splits it into several smaller steps.  Both
/// are one-edge events: the gauge changes with no S-transform.
///
/// The two-edge handover pins the incoming anchor at its own value in the outgoing
/// frame, so the realization is continuous by construction.  That value comes from
/// @ref AnchorHandoverLedger, harvested after the solve one epoch earlier.
///
/// The ledger exists because `getStateVector()(amb_idx)` is `x_lin`, a linearization
/// frame, not an estimate.  The network solve is never written back into the
/// per-station StateManager (`setStateVector` runs once, at setup), and an `x_lin`
/// shift on a local column is an exact solve no-op, so pinning to it would carry no
/// information about where the arc sits.  The absolute estimate is
/// `x_lin + back-substituted Δx`, which exists only after the solve; hence the
/// one-epoch deferral, the same shape as `carry_innov_pending`.
struct AnchorHandoverLedger {
    /// (station index, ambiguity key "sat_signal") -> that station's absolute
    /// ambiguity estimate from the previous epoch's solve, in that epoch's realized
    /// frame.  Rebuilt wholesale each epoch: a one-epoch lifetime bounds what a
    /// decoupled gauge may carry (invariant #7).  value = the absolute estimate;
    /// var = its conditional variance (m²).  The variance is needed because the pin
    /// is a hard constraint of weight ~1e10, so pinning an ill-determined estimate is
    /// worse than re-zeroing: a target taken from an unvalidated arc produces a large
    /// residual spike one epoch later.  Invariant #7's re-coupling edge must
    /// re-validate what it reconnects, and the variance makes that check possible.
    struct Entry {
        double value{0.0};
        double var{-1.0};   ///< < 0 = unavailable, treated as unusable.
        double var_first{0.0};  ///< the arc's first-stash conditional variance (m²; 0 = unknown, which the ratio ceiling refuses); the denominator of network.phase_datum_handover_max_var_ratio.
    };
    std::map<std::pair<int, std::string>, Entry> value_abs;
    /// True once a harvest has run, so the election can tell "armed but the
    /// first epoch has no history yet" from "not armed".
    bool primed{false};
};

/// One entry of the held S-basis (RF-2 item 1): which station's ambiguity realizes a
/// (satellite, signal) phase-bias datum, at what constant value, and since when.
///
/// The anchor station's ambiguity for each pinned (satellite, signal) is the S-basis,
/// held at its value at election rather than carried as a float state.  Under RF-1
/// every ambiguity is a live state, including the one that is the datum; a datum
/// ambiguity that converges, is dropped and is re-flagged by T4 re-realizes the
/// phase bias each time, and the consumer has to re-converge through every such
/// move.  The level vehicle's frozen zero-q anchor supplies this property
/// implicitly; the held basis supplies it by design.
///
/// @c value is frozen at election.  For a continuous handover it is the successor's
/// carried level in the outgoing frame, so the family does not step; for a cold one
/// it is 0, so the family steps by exactly @c prev_value, which item 2 declares.  It
/// is never re-derived during a tenure, because that re-derivation is the churn the
/// held basis removes.
struct PhaseBasisEntry {
    std::string station;        ///< The station whose ambiguity holds the datum.
    double      value{0.0};     ///< The constant the column is pinned at (m), frozen at election.
    long long   elected_ep{0};  ///< Loop epoch of the election that set it (tenure clock).
    bool        cold{false};    ///< True when the election could not inherit a level (value = 0 and the family stepped).
    double      prev_value{0.0};///< The outgoing basis value this one replaced (m): item 2's Δ.
    std::string prev_station;   ///< …and the station that held it ("" = first sighting).
};

/// One anchor handover, reported out of the election for the caller's
/// @c trace_events rows.  Observational only: emitting it never changes which
/// station is elected or what target is pinned.  Both edges are in one record by
/// design, because a handover with only its incoming half logged is the silent
/// re-referencing that invariant #7 forbids.
struct AnchorHandoverEvent {
    std::string amb;            ///< "sat_signal" (the ambiguity key).
    std::string from;           ///< Outgoing anchor station; empty = first pin (no prior frame).
    std::string to;             ///< Incoming anchor station.
    double      target_m{0.0};  ///< Pin target applied to the incoming anchor (m).
    bool        continuous{false}; ///< true = frame preserved; false = cold re-realization.
    double      var{-1.0};        ///< the successor's ledger conditional variance (m²; < 0 = no ledger entry); traced when the ratio ceiling is armed.
    double      var_first{0.0};   ///< the successor arc's first-stash variance (m²; 0 = unknown).
    /// Cold anchor fallback (network.phase_datum_candidacy_usable, SPEC_event_harness
    /// §15): how many higher-ranked candidates the usable-first pass skipped before
    /// this successor, and the first one it skipped.  0 and empty when the knob is off
    /// (the pass is not entered).  Recorded so that a cold-handover census can be
    /// re-run from the trace alone instead of from a second run.
    int         cand_skipped{0};
    std::string cand_first_unusable{};
};

/// One present station ZWD column this epoch (B9): its A_r column and the ZWD
/// linearization point, for the post-solve harvest/stash (mirrors @ref PlAmb).
struct PlZwd {
    Eigen::Index col;     ///< A_r column of the ZWD state (= h2local[tropo_idx]).
    double       x_lin;   ///< This epoch's ZWD linearization point (state value).
};

/// The carried receiver clock's one-step @b (bias, drift) prediction (RX-CLK-in-block,
/// @c network.carry_rx_clock_in_block).
///
/// Shared by both edges by design.  PASS B needs it to build the injected prior (mean
/// and @c var_eff); the post-solve harvest needs the same prediction to form the
/// innovation that its clock-jump gate tests.  Two separate computations could drift
/// apart without notice, and the injected prior would then be tested against a
/// prediction other than the one that was injected, so the algebra lives here once.
struct RxClkPred {
    double mean{0.0};  ///< value_abs + rate*gap  (m)
    double var{0.0};   ///< var + q*gap_ep + rate_var*gap^2  (m^2)
};

/// Propagate a carried receiver clock over @p gap_s seconds (RX-CLK-in-block).
///
/// The state is (bias, drift).  The bias is a random walk of @p q_m2 per epoch.  The
/// gap is supplied in epochs (@p gap_ep) as well as in seconds because the knob's
/// unit is per epoch, while the drift's contribution grows with the square of the gap
/// in seconds.  The drift's own uncertainty enters as @f$\sigma_d^2\,\Delta t^2@f$.
/// The (bias, drift) cross covariance is not carried, so the returned variance omits
/// the term @f$2\,\Delta t\,\mathrm{cov}(b, d)@f$: it is an upper bound (a prior no
/// stiffer than the information supports) when that covariance is zero or negative,
/// and an underestimate when it is positive.
///
/// An ISB column calls this with @p rate = @p rate_var = 0: a pure random walk.
///
/// @param value_abs Last harvested absolute value (m): x_lin + x_r, never a correction.
/// @param var       Its harvested conditional variance (m^2).
/// @param rate      Carried drift (m/s); 0 when unknown or not applicable.
/// @param rate_var  Its variance (m^2/s^2); the large @c carry_rx_clock_rate_var0 until
///                  the harvest has folded one increment.
/// @param gap_s     Elapsed time since @c value_abs was harvested (s).
/// @param gap_ep    The same gap expressed in epochs (>= 1), for the per-epoch q.
/// @param q_m2      Bias random-walk variance per epoch (m^2).
/// @return The predicted mean and variance.
inline RxClkPred rxClockPredict(double value_abs, double var, double rate,
                                double rate_var, double gap_s, double gap_ep,
                                double q_m2) {
    RxClkPred p;
    p.mean = value_abs + rate * gap_s;
    p.var  = var + q_m2 * gap_ep + rate_var * gap_s * gap_s;
    return p;
}

/// Scalar Kalman update of the carried drift from one harvested increment
/// (RX-CLK-in-block).
///
/// @param rate      [in,out] the drift estimate (m/s).
/// @param rate_var  [in,out] its variance (m^2/s^2).  A value < 0 on entry means
///                  "never updated"; the caller must then pass the configured
///                  @c carry_rx_clock_rate_var0 instead, because this function
///                  assumes a finished prior.
/// @param obs_rate  the increment's implied rate, (c_k - c_{k-1}) / dt (m/s).
/// @param r_rate    that increment's own noise variance, (var_k + var_{k-1} +
///                  q*gap_ep) / dt^2 (m^2/s^2).
/// @param rate_q_ep the drift's process noise over this gap (m^2/s^2).
/// @return true when the update was applied (a non-degenerate denominator and a finite
///         observation); false leaves both arguments untouched.
inline bool rxClockRateUpdate(double& rate, double& rate_var, double obs_rate,
                              double r_rate, double rate_q_ep) {
    if (!std::isfinite(obs_rate) || !std::isfinite(r_rate) || r_rate < 0.0) return false;
    const double var_pred = rate_var + rate_q_ep;
    const double denom = var_pred + r_rate;
    if (!(denom > 0.0) || !std::isfinite(denom)) return false;
    const double k = var_pred / denom;
    rate     += k * (obs_rate - rate);
    rate_var  = (1.0 - k) * var_pred;
    return true;
}

/// One of a station's carried receiver-clock-family local columns as PASS B recorded
/// it (RX-CLK-in-block, @c network.carry_rx_clock_in_block): the primary receiver
/// clock (@c kind empty) or one per-system ISB (@c kind = the system name).  The
/// record is written for every present column, folded or not, so that the post-solve
/// harvest can stash the new absolute value even in epochs where the carry could not
/// act (the @c pl_zwd_present pattern).
struct PlRxClk {
    Eigen::Index col;      ///< A_r column of the state (= h2local[state index]).
    double       x_lin;    ///< This epoch's linearization point.  For the clock it is constant over the run: the network producer never writes its solution back into the station StateManager, so the whole time-varying clock lives in x_r.
    std::string  kind;     ///< "" = the primary receiver clock (the one with the carried drift); otherwise the ISB's system name ("Galileo", ...), which takes a pure random walk.
};

/// PASS B produce-phase result for one station: the assembled local Jacobian split
/// plus the disposition flags that the serial consume phase needs.  Each slot is
/// written by exactly one thread (indexed, never push_back) and consumed serially in
/// station order, so est.addStation sees identical inputs in an identical order
/// regardless of thread count (the determinism guarantee).
struct StationAssembly {
    bool             ok{false};            ///< Produced a usable H/A_s/A_r block.
    bool             empty_innov{false};   ///< computeMeasurementUpdate yielded no rows.
    bool             state_mismatch{false};///< H column count != state size.
    bool             threw{false};         ///< computeMeasurementUpdate / splitStation threw.
    Eigen::MatrixXd  A_s, A_r;             ///< Shared-border + local-nuisance Jacobian.
    Eigen::VectorXd  w, y;                 ///< Per-row information weights + innovations.
    int              np{0};                ///< Pivot ambiguity pins folded in (pivot only).
    std::map<int,int> h2local;             ///< H-column -> A_r-column map (back-substitute the slant-iono nuisance for the viz trace).
    /// One contribution tag per row of @c A_s / @c A_r, in row order (card NOM N2a,
    /// network/assembly/NetworkContributions.hpp): every site that appends rows appends their tags in
    /// lockstep, so the row's kind, satellite and signal travel with it.  Every reader that
    /// asks what a row is reads its tag (card NOM step N2b, filter/NetworkRowClassify).
    std::vector<RowTag> row_tags;
    /// The split's data rows: rows [0, n_data_rows) of the block are the measurement
    /// update's (@c em.H.rows()), everything after them is appended (card NOM N2a; the tag
    /// check reads it).
    Eigen::Index n_data_rows{0};
    /// One tag per entry of @c carry_cols (the post-PD diagonal channel), in entry order.  An
    /// entry the coalesce built from two diagonal priors on one column keeps the tag of the
    /// entry the sort placed first; the merged entries' tags are in @c carry_merged.
    std::vector<RowTag> carry_tags;
    std::vector<RowTag> carry_merged;   ///< Tags of the post-PD entries merged into another entry of the same column.
    /// One tag per entry of @c prior_cols (the pre-PD channel); @c prior_merged as above.
    std::vector<RowTag> prior_tags;
    std::vector<RowTag> prior_merged;   ///< Tags of the pre-PD entries merged into another entry of the same column.
    /// The serial consume folded @c carry_cols through @c addStationPrior (the entries count
    /// as folded contributions only then).
    bool carry_prior_folded{false};
    /// What the GIM ionosphere row builder did on this station-epoch (card NOM section 16,
    /// suspect 4: it was called and silently inert on the golden).
    GimRowsOutcome gim_rows{GimRowsOutcome::NotCalled};
    /// SD-EWL rows the builder reported this station-epoch (the change of the station's
    /// @c sd_ewl_ctr.rows_built over the call; card NOM N2a: the tag check compares it with
    /// the @c SdEwl tags).
    long long sd_ewl_rows{0};
    /// The station's parameter inventory of this epoch (card NOM N1): every state column
    /// classified, with the disposition networkSplitStation_ gave it.  Written by this
    /// station's produce iteration only, drained serially after PASS B
    /// (networkParamInventoryDrain).  Trace class: nothing in the solve reads it.
    StationParamSlot param_inv;
    std::string      err;                  ///< Diagnostic (mismatch detail or exception text).
    // Phase-leveling carry (increment 1): built in PASS B, consumed serially.
    std::vector<Eigen::Index> carry_cols;  ///< A_r columns receiving a carried prior (ascending, for determinism).
    Eigen::VectorXd           carry_dNrr;  ///< Per-column information increment Λ = 1/var_eff.
    Eigen::VectorXd           carry_dur;   ///< Per-column RHS increment η = Λ·(value_abs − x_lin).
    // The station's own datum rows (card RF-PD-fix, network.station_prior_before_pd),
    // held apart from the carry above so that they can be handed to addStation and
    // folded into N_rr/u_r before its positive-definiteness test.  Otherwise the test
    // judges a station without the only row that gauges its receiver DCB, and drops it
    // for lacking that row (docs/refound/RF-PD_station_rejection_2026-09-15.md).  A row
    // lands in exactly one of the two vectors: with the knob off, the RXCB-DATUM rows
    // stay in carry_* at their existing position in the coalesce (byte-identical); with
    // it on they move here and are withheld from the later addStationPrior call, which
    // does not sum.  Empty means nothing to fold early.
    std::vector<Eigen::Index> prior_cols;  ///< A_r columns receiving a pre-PD datum row (ascending).
    Eigen::VectorXd           prior_dNrr;  ///< Per-column information increment Λ = 1/σ².
    Eigen::VectorXd           prior_dur;   ///< Per-column RHS increment η = Λ·(0 − x_lin).
    // Conditional coupling rows (card 1, network.carry_conditional_coupling).  The
    // appended rows live in A_s/A_r (not in carry_cols), so the within-epoch RAIM
    // refloat must know which rows to withhold.  cpl_row_start = the first A_s/A_r row
    // index of the coupling block (−1 when none); cpl_row_local_cols = the carried local
    // column each coupling row targets, in append order (so that a refloated column's
    // row can be zeroed).
    Eigen::Index              cpl_row_start{-1};
    std::vector<Eigen::Index> cpl_row_local_cols;
    // Coupling-term counts of this station-epoch's injected rows (card 1 and 1d
    // observability): layout-present terms, departed (marginalized) terms, and present
    // terms κ-shrunk by the border time update.  Summed serially into run totals.
    long long                 cpl_terms_present{0};
    long long                 cpl_terms_departed{0};
    long long                 cpl_terms_shrunk{0};
    /// (constellation, kind) families whose coefficient sum was held at its harvest
    /// value against a truncated column, and families with truncated mass but no present
    /// recipient to carry it, which leaves the row gauge-dirty for that family this
    /// epoch (card 1f, network.carry_coupling_gauge_project).  Both stay 0 with the
    /// card off, so a nonzero pair shows that the wiring ran.
    long long                 cpl_gauge_held{0};
    long long                 cpl_gauge_orphan{0};
    /// Orphan attribution: per (constellation, SharedParamKind) family, the orphan
    /// count and the summed |truncated mass| left gauge-dirty.  There is no recipient
    /// fallback, by decision: the coefficient sums are realized per constellation
    /// because the datum is, so holding an orphaned family's mass against the row's
    /// other constellation family would move a different datum's gauge.  The residue
    /// stays counted, attributed and unfixed by construction; a fallback needs its own
    /// derivation card.
    std::map<std::pair<char,int>, std::pair<long long,double>> cpl_orphan_fam;
    /// Coupling terms diverted to the departed route by the widened (kind, sat, signal)
    /// probe specifically, i.e. terms that the legacy satellite-wide probe would have
    /// folded as present (card 1g, network.carry_coupling_reinit_sources).  This is the
    /// card's own denominator: the run-total departed count moves for many reasons and
    /// cannot distinguish armed-and-working from armed-and-inert.
    long long                 cpl_reinit_diverted{0};
    /// Coupling terms diverted to the departed route this station-epoch because their
    /// satellite is inside its post-re-admission marginalization window (READMIT
    /// re-coupling edge, network.readmit_coupling_marg_ep).
    long long                 cpl_readmit_diverted{0};
    /// Card CTU (network.carry_coupling_exact_update): this station-epoch's exact-update
    /// row and term counts, accumulated in the parallel produce phase and folded into
    /// the run totals serially afterwards.  All zero when the card is off.
    CouplingExactCounters     cxu{};
    /// Inject-site free-epoch skips taken this station-epoch: the arc had a live carry
    /// that the latched set suppressed (SPEC_slip_relevel_walk).
    long long                 relevel_free_skips{0};
    /// Carried leveling priors this station skipped because their gap exceeded
    /// @c phase_leveling_max_gap_s (SPEC_withhold_era_release §8).  This skip, not the
    /// post-harvest prune, is the "cliff" mechanism: the entry is not erased, only not
    /// injected, so the arc solves free and is re-stashed from one epoch of code.
    /// Accumulated per station in the parallel produce phase and folded into the run
    /// counters serially afterwards, like @c relevel_free_skips.
    long long                 inject_stale_skips{0};
    /// Card PRM phase 3a (network/assembly/NetworkResponses.hpp): the carried-instance responses
    /// taken in the parallel produce phase, one per @c ResponseSite, accumulated per
    /// station and folded into @c NetworkServiceCounters::prm_resp serially afterwards,
    /// like @c relevel_free_skips.  Count-only: no decision reads them.
    long long                 prm_amb_free_latch{0};          ///< AmbFreeEpochLatch: arcs with a carried entry that the latched free set kept out of the fold, latched by anything but a re-entry response alone (a slip detector, a T4 bit, an unknown bit, or no recorded cause) (leveling vehicle only; under float_kf the arc folds at birth strength).
    long long                 prm_amb_free_latch_reentry{0};  ///< AmbFreeEpochLatchReentry: as @c prm_amb_free_latch, for an arc whose recorded latch cause is non-zero and has no bit outside @c RelevelEpochState::kCauseGap | @c kCauseWindup (gap re-entry, wind-up restart).
    long long                 prm_amb_pending_reseed_skip{0}; ///< AmbPendingReseedSkip: arcs with a carried entry not folded for a pending-reseed mark at the inject (either vehicle).
    long long                 prm_amb_gap_skip{0};            ///< AmbGapSkip: carried arcs past @c phase_leveling_max_gap_s not folded (leveling vehicle only); the subset of @c inject_stale_skips that skipped.
    long long                 prm_rxcb_gap_skip{0};           ///< RxcbGapSkip: carried receiver code biases past the 900 s horizon not folded, on either route (RXCB-IN-BLOCK, or the diagonal fold); the gate's variance guard is not counted.
    long long                 prm_rxcb_jump_skip{0};          ///< RxcbJumpSkip: diagonal-route carried receiver code biases withheld by the inject's innovation test (the instances of @c pl_cb_innov_skips).
    long long                 prm_iono_gap_skip{0};           ///< IonoGapSkip: carried slant ionospheres past their horizon not folded: the I-in-block refusal past @c iono_blk_max_gap_s (the instances of @c pl_blk_iono_gap_drop).
    long long                 prm_rxclk_gap_skip{0};          ///< RxclkGapSkip: carried receiver clocks (and ISBs) past @c rxclk_max_gap_s refused by the RX-CLK-in-block walk (the instances of @c pl_blk_rxclk_gap_drop).
    long long                 prm_zwd_gap_skip{0};            ///< ZwdGapSkip: a carried wet delay past @c phase_leveling_max_gap_s not folded, counted once at the diagonal B9 gate, which an in-block (B9-in-block) gap refusal always reaches with the same entry and the same test.
    long long                 prm_rpb_gap_skip{0};            ///< RpbGapSkip: carried eq34 receiver phase biases past @c phase_leveling_max_gap_s whose local time-linking prior was not built (a negative gap is not counted).
    /// Sim-only oracle ambiguity pin (network/slip/NetworkOraclePin.hpp): this station-epoch's
    /// pin accounting, accumulated in the parallel produce phase and folded into the run
    /// counters serially afterwards, like @c relevel_free_skips.  All four are 0 on every
    /// run with no `oracle_pin` event armed (the table pointer is null and the block is
    /// unreachable).  Closure: `opin_offered == opin_rows + opin_gauge_skipped +
    /// opin_no_column`.
    long long                 opin_offered{0};        ///< Ambiguity columns of this station found in the pin table.
    long long                 opin_rows{0};           ///< …folded as a diagonal pin prior row.
    long long                 opin_gauge_skipped{0};  ///< …skipped: the arc is gauge-pinned (an anchor), whose 1e10 datum pin owns the family's frame.
    long long                 opin_no_column{0};      ///< …skipped: the column is not in this epoch's A_r (or its x_lin / the row is non-finite).
    /// Per folded (sat, signal) ambiguity column of this station-epoch, the variance
    /// the fold used and the route it took (RF-1 §7.4, @c network.trace_amb_state).
    /// Filled only when the trace is armed; otherwise the map is never touched, and an
    /// unarmed run pays one bool test per column.  Read by the serial harvest, which
    /// owns the row write.  Recorded in the parallel produce phase into this station's
    /// own object, consumed serially afterwards.
    ///
    /// A column absent from this map was not folded this epoch: the harvest writes its
    /// row with @c folded = 0 and names the cause in @c event.  That is the population
    /// that D2 and D3 count, so the absence is itself the record.
    std::map<std::pair<std::string, std::string>, std::pair<double, AmbFoldRoute>>
        amb_state_fold;
    /// This station-epoch's jointly whitened leveling-carry block ingredients (F3,
    /// network.phase_leveling_local_block, SPEC_amb_iono_split_overcount).  Empty when
    /// the knob is off, when the station had no injectable carried column, or when the
    /// block's factorization fell back to per-column rows.  Read by the within-epoch
    /// RAIM refloat, which re-whitens the surviving columns (the whitening mixes the
    /// block's rows, so a refloat cannot zero a single output row).  All F3 counts below
    /// are produce-phase local and summed serially.
    PlBlockRaw                pl_blk;
    long long                 pl_blk_blocks{0};        ///< Blocks whitened (0 or 1 per station-epoch; F3).
    long long                 pl_blk_rows{0};          ///< Whitened rows emitted (F3).
    long long                 pl_blk_marg_cols{0};     ///< Carried entries of this station not injected this epoch (departed / stale / freed / reseed / non-finite), marginalized out of the block (F3).
    long long                 pl_blk_pair_missing{0};  ///< (i, j) pairs zeroed because the two columns were harvested at different epochs, or one carries no harvest block (F3).
    long long                 pl_blk_col_refused{0};   ///< Block columns dropped by the scalar-row screen (non-finite RHS or σ² ≤ 0; F3).
    long long                 pl_blk_fallback{0};      ///< Block factorization failed after the ridge retry, so this station-epoch emitted per-column rows instead (F3).
    /// Dimension of the block that failed to factorize this station-epoch, 0 when none
    /// did (RF-1 step 3, design risk #3).  Under the @c float_kf vehicle a fallback is
    /// not the degradation of an optional knob but a change of vehicle for one
    /// station-epoch, so the serial scatter logs it at WARN with the station's identity
    /// and this dimension, and the run-end bar fails the run above 1 % of attempted
    /// station-epochs.  Produce-phase local, like every other @c pl_blk_* count.
    long long                 pl_blk_fallback_dim{0};
    // The station's receiver-clock (and ISB) columns carried inside the same F3 block
    // (RX-CLK-in-block, network.carry_rx_clock_in_block).  As for the iono carry and
    // unlike the ZWD carry, there is no diagonal fallback row, by design: a diagonal
    // prior on the most station-common direction of all is the n_LOS over-count that
    // falsified the diagonal ZWD carry.  `pl_blk_rxclk_not_folded` therefore counts
    // columns whose carry did not act at all, not columns degraded to a weaker row.
    long long                 pl_blk_rxclk_cols{0};          ///< Receiver-clock-family columns admitted into the block build this station-epoch (they passed the scalar screen and became rows of Sigma).  A block that then failed to factorize still counts here; read it against @c pl_blk_rxclk_not_folded.
    long long                 pl_blk_rxclk_isb_cols{0};      ///< …of which are ISB columns (network.carry_rx_clock_isb).  0 is expected on a GPS-only or BSSD posture, which allocates no per-system time-offset state.
    long long                 pl_blk_rxclk_not_folded{0};    ///< Clock-family columns pushed but not folded: the block was non-SPD this station-epoch and there is no diagonal row to fall back to, so their carry did not act.
    long long                 pl_blk_rxclk_missing_cross{0}; ///< (clock-family, other) pairs zeroed for a missing harvest cross term: a subset of @c pl_blk_pair_missing, split out so that the clock carry's own coupling can be read separately.
    long long                 pl_blk_rxclk_gap_drop{0};      ///< Clock-family carries refused at inject because the sighting gap exceeded network.carry_rx_clock_max_gap_s (invariant #1's absent-station edge; the harvest prune then erases them).
    /// The receiver-clock-family columns present this station-epoch, in push order: the
    /// post-solve harvest's record of what to stash (RX-CLK-in-block).  Written for
    /// every present column whether or not its carry was folded.
    std::vector<PlRxClk>      pl_rxclk;
    /// The A_r columns of the clock-family entries pushed into this station-epoch's
    /// block (RX-CLK-in-block).  Read by the post-fit RAIM pool census to recognise the
    /// whitened clock block rows (@c n_raim_pool_rxclk_rows).  Empty with the knob off,
    /// on every station-epoch that pushed none, and on a non-SPD block (no whitened row
    /// exists).
    std::vector<Eigen::Index> pl_rxclk_blk_cols;
    // The station's receiver code-bias columns carried inside the same local block
    // (RXCB-IN-BLOCK, network.carry_rx_code_bias_in_block).  As for the receiver clock
    // there is no diagonal fallback row: the diagonal rx-CB fold at the conditional
    // variance re-asserts the shared border every epoch (the information along each
    // satellite's level doubles per epoch, card CTU section 14), so a column whose block
    // fails to factorize carries nothing that epoch and is counted.
    long long                 pl_blk_rxcb_cols{0};           ///< Receiver code-bias columns admitted into the block build this station-epoch (they passed the scalar screen and became rows of Sigma).  A block that then failed to factorize still counts here; read it against @c pl_blk_rxcb_not_folded.
    long long                 pl_blk_rxcb_not_folded{0};     ///< Receiver code-bias columns pushed but not folded: the block was non-SPD this station-epoch and there is no diagonal row to fall back to.
    long long                 pl_blk_rxcb_missing_cross{0};  ///< (receiver code-bias, other) pairs zeroed for a missing harvest cross term: a subset of @c pl_blk_pair_missing.
    long long                 pl_rxcb_restored_skip{0};      ///< carried receiver code-bias entries without a harvest-block row (restored from a diagonal-carry snapshot), left out of the block.
    /// The A_r columns of the receiver code-bias entries pushed into this station-epoch's
    /// block (RXCB-IN-BLOCK).  Empty with the knob off, on a station-epoch that pushed
    /// none, and on a non-SPD block.
    std::vector<Eigen::Index> pl_rxcb_blk_cols;
    /// The receiver code-bias columns of @ref pl_rxcb_blk_cols that the block builder's
    /// scalar screen refused (@c CouplingRowBuilder::buildLocalBlock; counted in
    /// @c pl_blk_col_refused): pushed, but never rows of the block.  Read only by the
    /// fold count of card L5B, which counts a column as folded only when it is absent here.
    std::vector<Eigen::Index> rxcb_refused;
    long long                 pl_blk_hist[5]{0, 0, 0, 0, 0}; ///< Block dimension histogram <10 / 10–24 / 25–49 / 50–74 / ≥75 (F3).
    long long                 pl_blk_max_dim{0};       ///< Largest block dimension this station-epoch (F3).
    // The ZWD column carried inside the F3 block (B9-in-block, network.carry_zwd_in_block).
    long long                 pl_blk_zwd_cols{0};           ///< ZWD column admitted into the block build this station-epoch (0 or 1): it passed the scalar screen and became a row of Σ.  A block that then failed to factorize still counts here; read it against @c pl_blk_fallback and @c pl_blk_zwd_fallback_diag, which say how many of those admissions did not end up folded.
    long long                 pl_blk_zwd_fallback_diag{0};  ///< ZWD fell back to the B9 diagonal row this station-epoch (no block, or the block's factorization failed).
    long long                 pl_blk_zwd_missing_cross{0};  ///< (ambiguity, ZWD) pairs zeroed for a missing harvest cross term: a subset of pl_blk_pair_missing, split out so that the ZWD's own coupling can be read separately.
    /// True when this station-epoch's ZWD folded through the whitened block
    /// (B9-in-block), so that @c pl_zwd.col names one of the block's local columns and
    /// the block row whose @c cpl_row_local_cols entry equals it is the ZWD row.  The
    /// post-fit RAIM pool reads this to count how many pool-admitted rows are that row.
    bool                      pl_zwd_in_block{false};
    // The station's per-LOS slant-iono columns carried inside the same F3 block
    // (I-in-block, network.carry_iono_in_block).  There is no diagonal fallback row for
    // them, by design: a diagonal per-LOS iono prior is the falsified QUEUE-8(b)
    // vehicle.  `pl_blk_iono_not_folded` therefore counts columns whose carry did not
    // act at all, not columns degraded to an older row.
    long long                 pl_blk_iono_cols{0};          ///< Per-LOS iono columns admitted into the block build this station-epoch (they passed the scalar screen and became rows of Σ).  A block that then failed to factorize still counts here; read it against @c pl_blk_iono_not_folded.
    long long                 pl_blk_iono_not_folded{0};    ///< Iono columns pushed but not folded: the block was non-SPD (or never built) this station-epoch, and there is no diagonal row to fall back to, so their carry did not act.  The counterpart of the ZWD carry's @c pl_blk_zwd_fallback_diag.
    long long                 pl_blk_iono_missing_cross{0}; ///< (iono, other) pairs zeroed for a missing harvest cross term: a subset of @c pl_blk_pair_missing, split out so that the iono carry's own coupling can be read separately.
    long long                 pl_blk_iono_gap_drop{0};      ///< Carried LOS entries refused at inject because the sighting gap exceeded network.carry_iono_in_block_max_gap_s (invariant #1's absent-satellite edge; the harvest prune then erases them).
    /// The A_r columns of the iono entries pushed into this station-epoch's block, in
    /// push order (I-in-block).  Read by the post-fit RAIM pool census to recognise the
    /// whitened iono block rows (@c n_raim_pool_iono_rows); empty with the knob off and
    /// on every station-epoch that pushed none.
    std::vector<Eigen::Index> pl_iono_blk_cols;
    std::vector<PlAmb>        pl_amb;       ///< Present non-pivot ambiguities, for the post-solve stash.  A carry-eligible subset: filled only when the phase-leveling ambiguity carry is the live vehicle (`carry_warm && !carry_invariant`), because it doubles as that carry's inject/harvest record.  QC tiers must not key on it; see `qc_amb`.
    /// Census of the ambiguity columns that are present and unpinned, and therefore
    /// checkable, this epoch, independent of the carry vehicle (RAIM pool re-host,
    /// docs/SPEC_raim_pool_rehost.md).  The same (col, sat, signal, x_lin, key) tuple as
    /// @c pl_amb, built by the same walk, but without the two carry gates that leave
    /// @c pl_amb empty under carry_invariant (`legacy_local_inject`'s carry_warm, and
    /// `do_amb_carry`'s `!carry_invariant`).
    ///
    /// "Which columns get a carried prior injected" is a carry question; "which columns
    /// a post-fit residual test can screen" is an observability question.  Hosting the
    /// second on the first would leave every post-fit RAIM tier structurally inert under
    /// @c network.carry_invariant.
    ///
    /// Filled when @c ctx.knobs.raim_ci_on is set or the continuous phase-datum handover
    /// is active; empty otherwise.  @c pl_amb keeps its contents either way, so every
    /// existing @c pl_amb reader (the leveling harvest, the carry-VCE row feed, the
    /// sticky tree, the within-epoch tier) is untouched, and a run with both off is
    /// byte-identical.
    std::vector<PlAmb>        qc_amb;
    bool                      pl_zwd_present{false};  ///< This station had a carry-eligible ZWD column this epoch (B9).
    bool                      pl_zwd_injected{false}; ///< A ZWD carry prior was injected this epoch (the "carried" flag of network_zwd.csv, distinct from carry-eligible; L7).
    PlZwd                     pl_zwd{};               ///< The ZWD present-record for the post-solve stash (B9).
    /// Present unpinned RPB columns recorded for the post-solve stash, as (rpb_key, A_r
    /// column, linearization point) (eq34 RPB time-linking, card 3).  Empty unless
    /// network.phase_datum_eq34 is on with phase_datum_eq34_rpb_q_m2s > 0.
    std::vector<std::tuple<std::string, Eigen::Index, double>> pl_rpb;
    /// Present per-LOS iono columns recorded for the post-solve harvest, as (sat, A_r
    /// column, linearization point) (I-in-block).  The harvest appends them to the
    /// block's columns.  Same tuple shape as pl_rpb.  Empty unless
    /// network.carry_iono_in_block is on.
    std::vector<std::tuple<std::string, Eigen::Index, double>> pl_iono;
    /// rx-CB carry (docs/SPEC_rx_cb_carry.md): present receiver code-bias columns
    /// (state key, A_r col, x_lin) recorded for the post-solve harvest.  Filled only
    /// under network.carry_rx_code_bias; same tuple shape as pl_rpb.
    std::vector<std::tuple<std::string, Eigen::Index, double>> pl_cb;
    // Card LGM (network.phase_colour_state): the present phase-colour columns and the
    // edges of their priors this station-epoch (produce-phase locals, summed serially).
    std::vector<PlPc>         pl_pc;        ///< Present columns, for the harvest and the trace.
    std::vector<Eigen::Index> pc_planned;   ///< Columns that entered PASS B with a live carry.
    std::vector<Eigen::Index> pc_folded;    ///< Of those, the columns whose carried prior folded.
    std::vector<Eigen::Index> pc_refused;   ///< Columns the block builder's scalar screen refused.
    long long pc_birth_rows{0};     ///< Birth prior rows (no live carry).
    long long pc_backstop_rows{0};  ///< Birth rows for planned carried columns that did not fold.
    long long pc_block_folds{0};    ///< Carried columns folded through the whitened block.
    long long pc_fallback_folds{0}; ///< Carried columns folded by the per-column fallback.
    int pl_cb_datum_rows{0};   ///< Zero-mean DCB prior rows folded this station-epoch (RXCB-DATUM; summed serially).
        int pl_cb_injected{0};     ///< rx-CB priors folded this epoch (produce-phase local; summed serially).
    int pl_cb_innov_skips{0};  ///< rx-CB priors withheld by the innovation gate (receiver-reset response).
    /// Per-row position partial @f$\partial y_i/\partial\delta p@f$ for the station
    /// congruence screen (docs/SPEC_redundancy_census.md CG-INC-0), expressed in the
    /// station's local ENU frame: @c m_data × 3, row-aligned with the first @c m_data
    /// rows of @c A_s / @c A_r / @c w / @c y.
    ///
    /// The network holds every station coordinate fixed, so @c networkSplitStation_
    /// drops the three position columns of @c H (they would break @c A_r's
    /// linear-in-observation invariant; see the A6 guard there), and no other structure
    /// in the run retains the geometry of a row.  A wrong held coordinate is therefore
    /// invisible to every other armed tier: median polish removes the station common
    /// effect, per-row w-tests see only what is left after absorption, and the
    /// (station, satellite) block test pools one line of sight while a coordinate error
    /// is spread coherently across all of them.  This block is the state that the
    /// congruence screen needs to test for such an error.
    ///
    /// It is the Jacobian, not a raw line-of-sight vector, by design: it is copied from
    /// @c em.H (whose position partial is @f$-\hat u@f$ per
    /// @c MeasurementModel::buildJacobianRow) and rotated by the ENU→ECEF matrix at the
    /// station's held coordinate.  Taking the partial the measurement model built,
    /// rather than re-deriving a sign per row type, makes the screen's convention
    /// correct by construction for every current and future row class:
    /// @c em.innovations and @c em.H are one linearization, so @f$y \approx H\,\delta x@f$
    /// holds row by row whatever the observable is.  A row whose observable carries no
    /// geometry (an SH↔slant tie, a pin, a coupling row) has an exactly-zero partial
    /// here or lies past @c rows(): every append site in the assembly appends past the
    /// data rows, so only rows @f$[0,\text{rows}())@f$ can carry a non-zero triple.
    ///
    /// Empty unless @c network.congruence_screen is armed: an unarmed run allocates
    /// nothing and executes no extra floating-point operation, which the card's
    /// byte-identity A/B checks.
    Eigen::MatrixXd           row_pos_enu;
    /// Pre-formatted `network_sd_ewl.csv` rows for this station-epoch (SD pre-reduce
    /// EWL, docs/SPEC_sd_ewl_prereduce.md, @c network.sd_ewl.trace): one per satellite
    /// visit, carrying the elected signal pair, the arc outcome, and the row's z/σ.
    /// Buffered here rather than written in place because the row build runs in the
    /// parallel produce phase; PASS B's serial consume drains them in station order, so
    /// the file is deterministic for any thread count.  Empty unless the trace is armed
    /// (log-only; it changes no solve).
    std::vector<std::string>  sd_ewl_trace;
    /// Chain-pin audit (card 3, docs/SPEC_synthetic_minimum_network.md §14 T1, measured
    /// record §12.8): per (constellation, signal) fold tallies for this station-epoch's
    /// elected datum pins, keyed `"<cons>|<signal>"` (the election's @c dr_key), with
    /// value {folded, miss_state, miss_row} counting @c foldAmb / @c foldRpb outcomes.
    /// Those lambdas return early without a message when the elected ambiguity or RPB
    /// has no state index or no live phase row this epoch, so an armed chain datum that
    /// places fewer pins than the election declared would otherwise look like a working
    /// one (standing invariant #4); these tallies supply the missing count.  Written in
    /// the parallel produce phase (one writer per slot) and summed serially after the
    /// consume, so run totals do not depend on the thread count.  Empty unless
    /// @c network.phase_datum_eq34 is on.
    std::map<std::string, std::array<int, 3>> pin_audit;
    /// Fold-deferred realized targets of the seed-round path (card 3,
    /// docs/SPEC_eq34_seed_round.md §3.4): (ambiguity key, realized target m, pin
    /// innovation y m) for every pin whose target the fold computed because the state
    /// was absent at election time.  Drained serially after the consume into
    /// @c ctx.carry.eq34_prev_target for accepted stations.  Without this feedback the
    /// next election would read a provisional 0, compute a spurious km-scale pin move
    /// every epoch, and permanently re-reference the (sat, σ).  |y| ≤ λ/2 by
    /// construction of std::round; the consume asserts it into the run counter.  Empty
    /// unless @c network.phase_datum_eq34_seed_round is on.
    std::vector<std::tuple<std::string, double, double>> pin_seed_targets;
};

/// Whiten one station's phase-leveling carry block over its kept columns, i.e. those
/// whose @c local_col is not in @p skip (the within-epoch RAIM refloat drop-set; empty
/// for the primary build) (F3, network.phase_leveling_local_block).  The dropped
/// columns are marginalized (their rows and columns are deleted from Σ before the
/// factor, never conditioned on), which is exact: the kept columns' joint prior is the
/// corresponding principal sub-block of Σ.  Factor Σ_kept = LLᵀ (one ridge retry at
/// 1e-12·max|diag|), then emit the unit-information rows L⁻¹·[I | M | y]: @p Hr
/// (m×ncol_r, the dense lower-triangular L⁻¹ placed on the block's local columns), @p Hs
/// (m×nshared, L⁻¹ times the per-column present-shared regressor rows) and @p z (L⁻¹y).
/// @p kept receives the indices into @c blk.rows of the surviving rows, in order (output
/// row k belongs to @c blk.rows[kept[k]]).
/// Returns false when @c blk.Sigma is not n×n or the factor is non-SPD after the retry
/// (the caller falls back to per-column rows, or drops the block for a refloat
/// re-solve); m = 0 returns true with empty outputs.  Header-inline because its two
/// callers (the builder's primary build and the run-TU refloat re-whiten) must share
/// one codegen, so that the re-whitened survivors are the rows the primary build would
/// have emitted for that column set.
inline bool whitenLocalBlock(const PlBlockRaw& blk,
                             const std::set<Eigen::Index>& skip,
                             Eigen::Index ncol_r, Eigen::Index nshared,
                             Eigen::MatrixXd& Hr, Eigen::MatrixXd& Hs,
                             Eigen::VectorXd& z, std::vector<Eigen::Index>& kept) {
    kept.clear();
    const Eigen::Index n = static_cast<Eigen::Index>(blk.rows.size());
    for (Eigen::Index k = 0; k < n; ++k)
        if (skip.find(blk.rows[static_cast<std::size_t>(k)].local_col) == skip.end())
            kept.push_back(k);
    const Eigen::Index m = static_cast<Eigen::Index>(kept.size());
    Hr = Eigen::MatrixXd::Zero(m, ncol_r);
    Hs = Eigen::MatrixXd::Zero(m, nshared);
    z.resize(m);
    if (m == 0) return true;
    if (blk.Sigma.rows() != n || blk.Sigma.cols() != n) return false;
    Eigen::MatrixXd S(m, m);
    for (Eigen::Index a = 0; a < m; ++a)
        for (Eigen::Index b = 0; b < m; ++b)
            S(a, b) = blk.Sigma(kept[static_cast<std::size_t>(a)],
                                kept[static_cast<std::size_t>(b)]);
    for (Eigen::Index k = 0; k < m; ++k) {
        const CplRowRaw& r = blk.rows[static_cast<std::size_t>(kept[static_cast<std::size_t>(k)])];
        if (r.local_col >= 0 && r.local_col < ncol_r) Hr(k, r.local_col) = 1.0;
        for (const auto& sc : r.shared)
            if (sc.first >= 0 && sc.first < nshared) Hs(k, sc.first) = sc.second;
        z(k) = r.rhs;
    }
    S = (0.5 * (S + S.transpose())).eval();   // clean numerical asymmetry before LLT
    Eigen::LLT<Eigen::MatrixXd> llt(S);
    if (llt.info() != Eigen::Success) {
        const double dmax = S.diagonal().cwiseAbs().maxCoeff();
        S.diagonal().array() += (dmax > 0.0 ? 1e-12 * dmax : 1e-12);
        llt.compute(S);
        if (llt.info() != Eigen::Success) return false;   // caller falls back / drops
    }
    Hr = llt.matrixL().solve(Hr);
    Hs = llt.matrixL().solve(Hs);
    z  = llt.matrixL().solve(z);
    return true;
}

/// Two-state (offset, drift) stochastic clock model for a carried satellite clock: the
/// Zucca–Tavella oscillator model, x = [offset Δδtˢ (m), drift (m/s)], driven by white-FM
/// (@c q1, m²/s) and random-walk-FM (@c q2, m²/s³) noise whose diffusion coefficients come
/// from the clock's Allan / Hadamard deviation.  Over a step @c dt the transition is
/// Φ = [[1, dt], [0, 1]] and the process-noise covariance is
///   Q(dt) = [[q1·dt + q2·dt³/3,  q2·dt²/2], [q2·dt²/2,  q2·dt]].
/// Carried across epochs in the driver (the @c clock_osc map).  When network.clock_model is
/// on it replaces the scalar random-walk clock carry (var_eff = var + q_clock·dt).  Its
/// tight, drift-following prediction turns a self-consistent lock-in into a large
/// normalized innovation (a detectable dynamics fault) rather than a magnitude that no
/// test sees, and its predicted variance sizes the detection band per satellite and epoch
/// instead of a hand-tuned metre threshold.
struct ClockOsc {
    double x0{0.0};    ///< clock offset Δδtˢ (m)
    double x1{0.0};    ///< clock drift (m/s)
    double p00{0.0};   ///< cov(offset, offset)
    double p01{0.0};   ///< cov(offset, drift)
    double p11{0.0};   ///< cov(drift, drift)
    double t_last{0.0};///< Continuous-GPS seconds of this satellite's last update (0 = never; P0-3).  The prediction and the drift finite difference use the true per-satellite elapsed time, so a satellite that disappears (eclipse) and returns predicts over its real gap, with Q inflated over the true dt rather than one epoch, instead of from a state that is hours old and falsely tight.
    bool   init{false};///< false until the first stash seeds it
};

/// One-step predicted ClockOsc (the carry-prior mean and variance): Φ·x and Φ·P·Φᵀ + Q(dt).
struct ClockOscPred { double x0, x1, p00, p01, p11; };

/// Per-satellite state of the guarded temporal clock anchor (card Q3,
/// docs/SPEC_clock_temporal_anchor.md).  Two states: ANCHORED (the clock's D2 carry
/// folds at @c network.clock_anchor_q) and LOOSE (it folds at the legacy
/// @c filter.process_noise.sat_clock_q, the behaviour without the anchor, which is
/// what bounds a decoupled satellite; standing invariant #7).
///
/// A satellite starts LOOSE and earns the anchor by staying in band for
/// @c network.clock_anchor_recouple_epochs consecutive epochs, the re-coupling
/// re-validation of invariant #7.  Every transition is an event with an identity.
struct ClockAnchorSat {
    double t_last{0.0};        ///< continuous-GPS seconds of this satellite's last fold (0 = never).  The anchor inflates over `pl_now_s − t_last`, never over the global epoch dt: every skip path (gate withhold, σ̂₀ withhold, solve failure) advances prev_tow while the carry survives, so the global dt understates a stale carry's age without bound.  The same rule as the clock model's P0-3 / R11 handling.
    double last_value{0.0};    ///< prior mean folded this epoch (the guard's μ)
    double last_var_eff{0.0};  ///< prior variance folded this epoch (the guard's P⁻; marginal, so it is identical on the diagonal and dense D2 branches)
    int    in_band{0};         ///< consecutive in-band epochs accumulated while LOOSE
    bool   anchored{false};    ///< false = LOOSE (born LOOSE; re-validation required)
    bool   ever_anchored{false};  ///< the guard has anchored this satellite before (measure-only: separates a re-coupling from the first engagement in card PRM phase 3a's response count)
    bool   folded{false};      ///< a clock prior is folded for this satellite this epoch and is still standing (guard eligibility; cleared when the guard withdraws it)
    bool   folded_anchored{false};  ///< …and it was the ANCHORED variance
    bool   stale_pending{false};    ///< the fold found the carry older than the staleness bound and fell back to the LOOSE variance; the guard owns the state transition, event and mark, so every decoupling edge goes through one path
};

/// Run state of the guarded temporal clock anchor (card Q3).  Lives in
/// NetworkCarryState.  Not snapshotted, by design (an exemption from the three-place
/// rule): a warm resume is an invariant #1 event (the F1 rule), so every satellite must
/// re-earn the anchor from LOOSE after a resume rather than inherit a stiffness whose
/// evidence was not carried across.
struct ClockAnchorState {
    std::map<std::string, ClockAnchorSat> sat;
    /// Satellites decoupled this epoch (the guard's output for the epoch), consumed by
    /// the wire-mark queue and the event/counter tier.
    std::set<std::string> tripped;
};

/// Per-satellite window sums of the level innovation instrument (card CTU section 14),
/// reset at every window end.
struct ClockLevelInnovWin {
    int    k{0};          ///< epochs accumulated
    double s_nu{0.0};     ///< sum of the innovation nu (m)
    double s_var{0.0};    ///< sum of its predicted variance P + R (m^2)
    double s_r{0.0};      ///< sum of the code part R = s0^2 Q (m^2)
    double s_p{0.0};      ///< sum of the prior contrast variance P (m^2)
    int    kab{0};        ///< epochs with both split halves
    double s_nua{0.0};    ///< sum of the even-station half's innovation (m)
    double s_nub{0.0};    ///< sum of the odd-station half's innovation (m)
};

/// Predict a ClockOsc forward by @p dt with white-FM @p q1 and random-walk-FM @p q2.
inline ClockOscPred clockOscPredict(const ClockOsc& s, double dt, double q1, double q2) {
    const double q11 = q1 * dt + q2 * dt * dt * dt / 3.0;
    const double q12 = q2 * dt * dt / 2.0;
    const double q22 = q2 * dt;
    ClockOscPred p;
    p.x0  = s.x0 + s.x1 * dt;
    p.x1  = s.x1;
    p.p00 = s.p00 + 2.0 * dt * s.p01 + dt * dt * s.p11 + q11;
    p.p01 = s.p01 + dt * s.p11 + q12;
    p.p11 = s.p11 + q22;
    return p;
}

/// Measurement update of @p s from prediction @p pr with a network-solved offset @p z
/// (variance @p r).  H = [1 0].  The update uses the raw innovation z − x0 (the carry
/// follows the raw clock).  Returns S, the innovation variance, so that the caller can
/// form the normalized innovation innov/√S (the dynamics-fault statistic), where innov is
/// the caller's z − predicted with the per-constellation median innovation removed.
/// @note Retained for reference: the clock-model carry sets the oscillator state directly
///       from the joint-solve posterior (the B3 data-incest fix), so this scalar KF update
///       is not wired.
[[maybe_unused]] inline double clockOscUpdate(ClockOsc& s, const ClockOscPred& pr, double z, double r) {
    const double S  = pr.p00 + r;
    const double dz = z - pr.x0;                 // raw innovation (carry follows the raw clock)
    const double K0 = (S > 0.0) ? pr.p00 / S : 0.0;
    const double K1 = (S > 0.0) ? pr.p01 / S : 0.0;
    s.x0  = pr.x0 + K0 * dz;
    s.x1  = pr.x1 + K1 * dz;
    s.p00 = (1.0 - K0) * pr.p00;
    s.p01 = (1.0 - K0) * pr.p01;
    s.p11 = pr.p11 - K1 * pr.p01;
    s.init = true;
    return S;
}

// Per-(station, sat) SH representativeness-error Gauss–Markov deviation carry: the
// per-LOS iono temporal-accumulation channel of the per-epoch network producer
// (Iono-2, T5 v2).  The SH surface cannot represent per-LOS slant-iono structure, and
// re-applying the pin as white noise forces that misfit dispersively into the float
// ambiguities (the NL-AR blocker).  With rep_error_tau_s > 0 the pin constrains
// (ι − Σ_l SH_l·c_l) to a tracked deviation d (GM: τ = rep_error_tau_s, steady-state
// σ = representation_sigma_tecu) instead of to 0.  Units (v2): dev and var are carried
// in vertical TECU (converted by the epoch's layer-0 mapping M₁(el)·tecu_to_L1m at
// consumption and harvest), so the carried level and its steady state are
// elevation-stationary; a carry in slant metres would not re-scale the level as the
// elevation changes.  var_ss is the constant representation_sigma_tecu², kept per
// entry so that the aging pre-pass needs no config re-read.  Threading contract: the
// map is aged (and slip-erased via the C3c funnel) once per epoch in a serial
// pre-pass and measurement-updated in the serial post-solve harvest via
// shDevExactUpdate(); the parallel assembly only reads it (find, never insert), so it
// is race-free by construction.  t_s = 0 marks a never-harvested (cold) entry.  With
// the carry off, the output is byte-identical.  (The map instance @c sh_dev_carry
// lives in NetworkCarryState.)
struct ShDevCarry { double dev{0.0}; double var{0.0}; double var_ss{0.0}; double t_s{0.0}; };

/// Exact scalar posterior update of one GM deviation d from the solved pin
/// functional (Iono-2).  It replaces the T5 v1 scalar filter, falsified as circular:
/// v1 re-consumed the pin-regularized residual as an independent floor-noise
/// observation, which collapses P to floor² after one update.
///
/// Model: prior d ~ N(d_pred, P_pred); pin row u − d = ε, ε ~ N(0, f²), with u the
/// functional (ι − Σ SH·c) whose data-only likelihood is N(u₀, U).  The assembly folds
/// the Schur-eliminated row u = d_pred + ε(f² + P_pred); the solve returns the
/// posterior û (= the pin residual) and V̂ = Var_post(u) = U·a/(U+a) with
/// a = P_pred + f².  Exact posterior of d:
///     d̂  = d_pred + (P_pred/a)·(û − d_pred)          (gain from the predicted P)
///     P⁺ = P_pred·(U + f²)/(U + a),  U = V̂·a/(a − V̂)
/// Limits: U→∞ (uninformative epoch) ⇒ P⁺ = P_pred, d̂ → d_pred; U→0 (data pins u) ⇒
/// P⁺ = P_pred·f²/a, the classic scalar KF update against a floor-noise observation.
/// V̂ ≥ a (numerically no information) ⇒ P⁺ = P_pred.  All inputs must be in one
/// consistent unit frame (the caller converts slant ↔ vertical).
///
/// @param d_pred   GM-aged prior deviation.
/// @param p_pred   GM-aged prior variance (> 0).
/// @param u_hat    Solved posterior mean of the pin functional.
/// @param v_hat    Solved posterior variance of the pin functional (≥ 0).
/// @param floor2   Pin-row floor variance f² (> 0).
/// @return {d̂, P⁺}.
inline std::pair<double, double> shDevExactUpdate(double d_pred, double p_pred,
                                                  double u_hat, double v_hat,
                                                  double floor2) {
    const double a = p_pred + floor2;
    if (!(a > 0.0)) return {d_pred, p_pred};
    const double d_post = d_pred + (p_pred / a) * (u_hat - d_pred);
    if (!(v_hat > 0.0))                       // data pins u exactly (or degenerate)
        return {d_post, p_pred * floor2 / a};
    if (v_hat >= a) return {d_post, p_pred};  // no information beyond the row itself
    const double U = v_hat * a / (a - v_hat); // recovered data-only variance
    return {d_post, p_pred * (U + floor2) / (U + a)};
}

/// @brief The gap horizon of the receiver code-bias carry (s; docs/SPEC_rx_cb_carry.md
///        section 3): a carried receiver code bias unseen longer than this is not folded
///        (@ref rxCbInjectGate's @c max_gap_s on both routes), is not judged by the block
///        harvest's step test, and is pruned at the harvest.
///
/// A constant, not a knob.  The one definition read by every one of those sites and by
/// the parameter registry (@c ParamSpec::gap_horizon_s of the receiver code bias, card PRM
/// phase 3b); before phase 3b it was two function-local constants and six literals.
inline constexpr double kRxCbMaxGapS = 900.0;

/// Inject-time decision for one carried receiver code-bias prior (rx-CB carry,
/// docs/SPEC_rx_cb_carry.md).  Read-only with respect to the carry map: the produce
/// phase is parallel, so a breach is answered by withholding the fold, and the serial
/// post-solve harvest then overwrites the stale entry with the fresh posterior (an
/// implicit, race-free drop).  The innovation gate is the invariant #1 event response
/// for receiver resets and firmware bias jumps: |value_abs − x_lin| beyond
/// k·√(var_eff + guard²) means that the carried level and the station filter's own
/// estimate disagree at hardware-jump scale.
///
/// The innovation test is frame-dependent.  @p innov_m is @c value_abs − x_lin(cb),
/// and x_lin is a frozen linearization point (seeded once by
/// @c StateInitializer::initializeCodeBiases, never written back from the solve), so
/// the statistic measures how far the carried value sits from an arbitrary frame, not
/// whether anything happened.  @p innov_test = false disables it and leaves the
/// frame-free gap and variance sanity checks; the in-block route passes false, and its
/// block harvest's step test owns the event response there.
enum class RxCbInjectAction { Fold, SkipGap, SkipInnov };
inline RxCbInjectAction rxCbInjectGate(double gap_s, double max_gap_s,
                                       double var, double q_m2s, double innov_m,
                                       double k_sigma, double guard_var_m2,
                                       double& var_eff_out, bool innov_test = true) {
    if (gap_s > max_gap_s) return RxCbInjectAction::SkipGap;
    const double var_eff = var + q_m2s * (gap_s > 0.0 ? gap_s : 0.0);
    // NaN via self-compare, runaway via a bound.
    if (!(var_eff > 0.0) || var_eff != var_eff || !(var_eff < 1e30))
        return RxCbInjectAction::SkipGap;
    var_eff_out = var_eff;
    if (innov_test
        && innov_m * innov_m > k_sigma * k_sigma * (var_eff + guard_var_m2))
        return RxCbInjectAction::SkipInnov;
    return RxCbInjectAction::Fold;
}

/// The D5-only slip response (Iono-2 v3k, erase-policy split): keep the accumulated
/// deviation level and re-tap the confidence to the GM steady state.  Applied when a
/// (station, sat) LOS is in the C3c funnel without a raw-detector confirmation this
/// epoch.  Most D5 re-taps (97 % measured) show no raw observable step: they are
/// iono-representation tension, the signal the deviation carry exists to absorb, and
/// erasing on them caps the mean deviation lifetime at about 0.7·τ.  Raw-confirmed
/// slips still erase (invariant #1 unchanged); see the PASS-B pre-pass.  Idempotent
/// under the subsequent GM aging step (λ²·σ_ss² + (1−λ²)·σ_ss² = σ_ss²).
inline void shDevD5Retap(ShDevCarry& dc) { dc.var = dc.var_ss; }

/// Iono-2 v3 result of one de-mean pass over the deviation carry
/// (docs/SPEC_iono2_mechB_v3.md; see @c shDevDemean).
struct ShDevDemeanStats {
    bool   applied{false};  ///< Global mean removed this epoch (n_used ≥ min_global_los).
    int    n_used{0};       ///< Warm entries that participated.
    double g_tecu{0.0};     ///< The removed global weighted mean (signed, vertical TECU).
    int    n_sta{0};        ///< Stations whose per-station mean was also removed.
};

/// Contract-enforcing de-mean of the carried deviation priors, which prevents a
/// ratchet (Iono-2 v3).  A component of the deviation field that is common in vertical
/// TECU across LOS has the row signature M(el)·tecu_to_m, identical to the a00
/// column's, so the data cannot separate that common deviation from a00, and the
/// per-epoch harvest, age and re-assert cycle ratchets it.  The per-station mean is
/// the receiver-DCB lump one level down.  Removing both from the carried prior leaves
/// them to a00 and the receiver states, where each epoch's data re-derives them; the
/// carry keeps only the per-LOS structure its contract names.
///
/// Serial context only (the PASS-B pre-pass, after slip-erase and GM aging).
/// Information weights w = 1/max(var, ε) over warm entries (t_s > 0); the global
/// weighted mean is removed when n ≥ @p min_global_los, then each station's weighted
/// mean of the residual when that station has ≥ @p min_station_los warm LOS.
/// Variances are left unchanged (conservative: the prior loses an estimated mean and
/// keeps its spread).  Cold entries are untouched.  Key layout "STATION|SAT" (the
/// sh_dev_carry contract).
///
/// @param carry            The deviation-carry map (mutated in place).
/// @param min_global_los   Minimum warm LOS for the global de-mean (spec: 30).
/// @param min_station_los  Minimum warm LOS per station for its mean (spec: 4).
/// @return Per-epoch stats for the run-end counters.
inline ShDevDemeanStats shDevDemean(std::map<std::string, ShDevCarry>& carry,
                                    std::size_t min_global_los,
                                    std::size_t min_station_los) {
    ShDevDemeanStats st;
    constexpr double kEps = 1e-12;
    double sw = 0.0, swd = 0.0;
    for (const auto& kv : carry) {
        if (kv.second.t_s <= 0.0) continue;   // cold: not yet a prior
        const double w = 1.0 / std::max(kv.second.var, kEps);
        sw += w;
        swd += w * kv.second.dev;
        ++st.n_used;
    }
    if (st.n_used < static_cast<int>(min_global_los) || !(sw > 0.0)) return st;
    st.g_tecu = swd / sw;
    st.applied = true;
    // Per-station accumulation of the residual (single pass: subtract g inline).
    std::map<std::string, std::pair<double, double>> sta;  // station -> {Σw, Σw·dev}
    std::map<std::string, std::size_t> sta_n;
    for (auto& kv : carry) {
        if (kv.second.t_s <= 0.0) continue;
        kv.second.dev -= st.g_tecu;
        const std::string s = kv.first.substr(0, kv.first.find('|'));
        const double w = 1.0 / std::max(kv.second.var, kEps);
        auto& acc = sta[s];
        acc.first += w;
        acc.second += w * kv.second.dev;
        ++sta_n[s];
    }
    for (auto& kv : carry) {
        if (kv.second.t_s <= 0.0) continue;
        const std::string s = kv.first.substr(0, kv.first.find('|'));
        const auto itn = sta_n.find(s);
        if (itn == sta_n.end() || itn->second < min_station_los) continue;
        const auto& acc = sta.at(s);
        if (acc.first > 0.0) kv.second.dev -= acc.second / acc.first;
    }
    for (const auto& kv : sta_n)
        if (kv.second >= min_station_los) ++st.n_sta;
    return st;
}

// One network_epoch_status.csv row per processed epoch (L1).  Fields not yet in
// scope at a given disposition site are left blank (std::nullopt), not zero: the
// pyviz timeline viewers (L5) distinguish "unavailable" from a real 0.  The writer
// body is NetworkRunTraces::emitEpochStatus (decomposition INC-4).
struct EpochStatusRow {
    std::optional<double> sigma0;
    std::optional<int>    n_adapt;
    std::optional<int>    n_present;
    std::optional<int>    n_clock_sats;
    std::optional<double> clock_median_m;
    std::optional<double> clock_std_m;
    std::string           worst_sat;        // empty string => blank
    std::optional<double> worst_dev_m;
    std::optional<int>    n_refloat_sats;
    std::optional<int>    n_retro_applied;
    std::optional<int>    datum_changed;
    std::optional<int>    n_arc_refloat;   ///< Isolated-arc RAIM refloats this epoch (the churn metric; distinct from n_refloat_sats, which is the clock-refloat set; T2d).
    // --- per-epoch carry churn (SPEC_day129_frame_guards P1-1) -----------------
    // The card 4 run-end log line reports run totals only, which cannot show whether
    // the churn spiked at a given epoch.  These are the same quantities differenced
    // per epoch (NetworkInstallCarry).  Appended columns; existing positions are
    // preserved (as for T2d).
    std::optional<long long> n_d5_flags;
    std::optional<long long> n_d5_retaps;
    std::optional<long long> n_chord_births;
    std::optional<long long> n_slip_refloats;
    // --- measure-only post-fit variance factor on the emit path (P1-4) --------
    // Present only under network.trace_postfit_sigma0.  sigma0 (above) is the
    // within-epoch RAIM's statistic and is 0.0000 whenever that RAIM is off.
    std::optional<double> sigma0_pf;        ///< robust σ̂0 over all post-fit residual rows
    std::optional<double> sigma0_pf_code;   ///< …code rows only
    std::optional<double> sigma0_pf_phase;  ///< …phase rows only
    std::optional<long long> n_pf_rows;     ///< rows in the pooled statistic (0 ⇒ inert)
    // --- redundancy census (docs/SPEC_redundancy_census.md INC-1/INC-2) -------
    // Present only on an epoch the census swept (strided, escalated or trip-forced).
    // A non-sweep epoch leaves all four blank rather than repeating the last sweep's
    // numbers: the timeline viewers must be able to see where the sweeps were, and a
    // carried-forward value would read as a per-epoch measurement that was never
    // taken.
    std::optional<long long> n_redun_rows;         ///< data rows swept: the denominator (1461 of 3300 and 1461 of 40000 are different networks)
    std::optional<long long> n_uncheckable_rows;   ///< of those, rows with r_i < redundancy_eps
    std::optional<double>    uncheckable_max_s;    ///< largest centred standardized prefit deviation among those rows (0 ⇒ swept, none carried a finite metric)
    std::optional<long long> n_unobservable_local; ///< local columns below the relative N_rr pivot floor (INC-2)
    // --- prefit census (docs/SPEC_prefit_census_observability.md INC-1) -------
    // Per data class {obs_code, obs_phase, obs_other, iono_sh_row}: the enumerator
    // order of NetRowClass, which is also the trace column order.  The statistic is
    // the common-mode-removed standardized prefit deviation
    // s = |y − median_type(y)|·√w (m ≡ 0 for iono_sh_row, which carries no receiver
    // clock), so a whole-station receiver-clock step cancels to ~0 and a single
    // parked cell does not.  Present only under network.trace_prefit_census and on
    // an epoch that classified rows; blank otherwise (the "unavailable is not 0"
    // convention).
    struct PrefitCensusStats {
        double    max_s[4]{};      ///< largest s in the class this epoch
        double    p99_s[4]{};      ///< 99th percentile of s in the class
        long long n_gt_10[4]{};    ///< rows with s > 10   (the storm-sea shoulder)
        long long n_gt_1000[4]{};  ///< rows with s > 1000 (the fault regime)
    };
    std::optional<PrefitCensusStats> prefit_census;
    // --- measurement-admission census (docs/SPEC_measurement_admission.md INC-3)
    // The absolute, product-referenced |z| population, which the prefit census
    // cannot provide: its statistic subtracts the filter state and therefore reads ~0
    // on a cell that was wrong from birth.  The two columns exist in the schema only
    // when network.adm_trace or network.adm_screen is armed, and are blank on an epoch
    // the screen did not run (the "unavailable is not 0" convention: a genuine 0.0
    // worst-|z| and a skipped epoch mean different things).
    std::optional<double> adm_sta_worst_z;  ///< median over screened stations of each station's worst |z| (m): the typical level across the network
    std::optional<double> adm_net_worst_z;  ///< network maximum |z| (m): the extreme; its identity goes to the ADMISSION events
    // --- PD-safe invariant-DD fold (docs/SPEC_pd_safe_dd_fold.md INC-0) -------
    // The per-epoch view of the fold's PD screen, so that the branch these columns
    // observe (`M := 0` on a non-PD Pᶻ+B) is visible epoch by epoch.  Always on and
    // solve-neutral: they are reads of a factorization the fold already computed.
    // Blank on an epoch in which the invariant carry did not fold, per the
    // "unavailable is not 0" convention: zero degenerate blocks and "the carry never
    // ran" mean different things.
    std::optional<long long> ddfold_degen_blocks;  ///< blocks that folded inertly this epoch (PD screen tripped ⇒ M=0)
    std::optional<long long> ddfold_degen_rows;    ///< Σ K over those blocks: the carried dimension that took no data
    std::optional<double>    ddfold_min_pivot_rel; ///< worst relative pivot d_min/d_max this epoch (≤ 0 ⇒ indefinite)
    // --- prior-residual census (docs/SPEC_prior_residual_census.md) -----------
    // The per-epoch view of the carried-prior census.  In the schema only when
    // network.prior_residual_census is armed (as for adm: with the census off, the
    // schema and output stay structurally byte-identical), and blank on an epoch the
    // census did not run.  The per-(family × label) detail is in
    // network_prior_residual.csv; these five are the timeline channel, and
    // prires_rows is the denominator without which the reported count means nothing.
    std::optional<long long> prires_rows;        ///< carried priors censused this epoch, all families and labels
    std::optional<long long> prires_rows_resp;   ///< of those, RESPONDED (an independent mechanism had already fired): the positive control's population
    std::optional<long long> prires_rows_frame;  ///< of those, FRAME (datum handover / reref / inflate-q / hold): the invariant #7 edges
    std::optional<long long> prires_n_reported;  ///< of those, |t| above network.prior_residual_report_sigma (a trace threshold; nothing acted)
    std::optional<double>    prires_max_t;       ///< the worst |t| this epoch
    // --- constraint-aware global σ̂₀ (docs/SPEC_prior_residual_census.md §14) ----
    // A schema flag separate from the five above, by design: the census's own §9.1
    // A/B arms compare network_epoch_status.csv across builds, so a census-only run
    // must keep the schema it has without this rider.  Present only when
    // network.prior_residual_global_sigma0 is armed.
    std::optional<double>    s0g_sigma0;         ///< σ̂₀_global = sqrt((Σw·v² + Σw·ê²)/(Σr_data + Σr_prior)); blank when Σr ≤ 0 (never 0)
    std::optional<double>    s0g_tension;        ///< Σw·ê² / Σw·v²: the weighted misfit carried by the priors relative to that of the data (the headline number).
    std::optional<int>       s0g_flags;          ///< in-band degradation bitmask (@c NetworkPriorResidual::GlobalFlag): 1 APPROX(r≡1), 2 NO_DATA, 4 NO_PRIOR, 8 DEGENERATE, 16 RNEG_DROP
    // --- within-epoch tier observability, R0 (SPEC_pivot_visibility_gauge_defect
    // §5.07.5).  Present in the schema only when network.raim_within_epoch is armed
    // (as for adm and prires: with the tier off, the output stays structurally
    // byte-identical), blank on an epoch the tier did not assess.  sigma0_first is the
    // pre-adaptation σ̂₀, the null sample that QUEUE item 4 needs on disk (a trip
    // event otherwise carries it only in its detail span).  n_we_refloat_sats is the
    // tier's own whole-satellite refloat count (n_refloat_sats above counts the
    // clock-couple set, empty unless clock_monitor_couple_raim).  we_clk_move_* is the
    // §5.07.2 impulse-channel meter: how far the within-epoch re-solves moved a
    // satellite clock away from the primary solve (0.0 = tier assessed, no re-solve).
    std::optional<double> sigma0_first;
    std::optional<int>    n_we_refloat_sats;
    std::optional<double> we_clk_move_max_m;
    std::string           we_clk_move_max_sat;   // empty = blank
};

/// Disposition of a relocated epoch-loop phase, consumed at the runNetwork call site
/// (decomposition INC-7, plan §Control flow).  SkipEpoch stands for an epoch-loop
/// `continue`; the site-specific bookkeeping (n_skipped, trace row, epoch-status row,
/// the prev_week/prev_tow advance) stays inside the relocated body.  StopRun is
/// reserved for a phase that would `break` out of the loop; none uses it, and the
/// max-duration break stays in the prologue.
enum class EpochAction { Proceed, SkipEpoch, StopRun };

/// @internal Per-epoch workspace of Application::runNetwork (decomposition INC-4).
/// Constructed fresh at the top of every epoch iteration and never hoisted for
/// reuse: fresh construction is the per-epoch reset.  Its scope is the RAIM-replay
/// capture channel, the two clock maps and the T2 culprit sets at the end; other per-epoch
/// state joins as its consuming blocks move out of the run-loop TU (INC-7/9).
///
/// RAIM-replay capture channel (network.raim_within_epoch): the within-epoch re-solve
/// (resolve_dropping) replays the epoch over a fresh beginEpoch(), so every
/// shared-border input folded outside the per-station assembly must be captured when
/// armed and re-applied verbatim.  addSharedPrior is additive, so the captured prior
/// sets concatenate into one replayable vector.  The writers capture only when the tier
/// is armed (NetworkKnobs::within_epoch_armed, which networkSolveGlue reads); otherwise
/// the channel stays empty.  Writer sites (a new prior fold elsewhere must mirror into
/// this channel):
///   - cap_H / cap_z / cap_constraint_weight / cap_has_constraint / cap_row_kinds: the W6
///     satellite code-bias datum constraint rows and their kinds (SB_SOLVEGLUE, W6
///     block; the kinds name the replay's re-fold, card NOM step N2b).
///   - cap_birth_H / cap_birth_z / cap_birth_weight: card GFC step 2's first-sighting
///     family prior rows (SB_SOLVEGLUE, network.code_family_birth_prior_sigma_m), soft
///     priors captured apart from the gauge rows above and re-applied at their own weight
///     1/sigma^2 under ContributionKind::CbFamilyBirth, through the helper the primary fold
///     uses (NetworkSbasis::foldFamilyBirthRows).  rows() == 0 means nothing to re-apply.
///   - cap_sbc_H / cap_sbc_z: the SBC S-basis completion gauge rows
///     (SB_SOLVEGLUE, network.sbasis_complete block; re-applied at
///     NetworkSbasis::kSbasisCompleteWeight, a compile-time constant, so no weight
///     field travels).  rows() == 0 means nothing to re-apply.
///   - cap_priors / cap_prior_kinds: the SH-VTEC pre-gate prior
///     (networkInjectShVtecPrior_), the eq34 reattach-hygiene pin-bridge priors
///     (network.phase_datum_time_linked), the drop-and-heal soft anchors
///     (NetworkPhaseDatum healNearNullPhaseBias, gate-failure path), the card KP
///     code-bias bound prior (NetworkSolveGlue, network.cb_kaula_prior), the D2 diagonal
///     carry prior and dense-fallback leftovers (networkInjectD2CarryPrior_), the
///     two-state clock-model prior (networkInjectClockOscPrior_), and the a-priori
///     satellite-clock Tikhonov anchor.  Each writer appends its priors through
///     @ref networkCapturePriors with the kind its fold named, so cap_prior_kinds holds
///     one kind per entry of cap_priors and the replay's single re-fold of them is
///     recorded per kind (card NOM step N2b).
///   - cap_dense_keys / cap_dense_cov: the C1 dense shared-carry fold
///     (networkInjectD2CarryPrior_, dense-success branch only; its non-PD fallback
///     routes through cap_priors like every diagonal prior).  On a card SCL clone epoch
///     they hold the marginal of the joint fold over the epoch's own columns, which is
///     the fold without the clones (the prior card Q3's phase statistic, card PVN and
///     card SMH read), and the replay folds cap_ccl_* instead.
///   - cap_ccl_on / cap_ccl_keys / cap_ccl_cov / cap_ccl_walk / cap_ccl_idle: card SCL's
///     joint fold (the stand-in block, the clones' walk factors) and the idle clones'
///     isolated priors (filter/NetworkClockClone); empty and false when off.
///   - cap_shx: card SCL2 X2's history priors (filter/NetworkSharedHistory); empty off.
/// The Q3 clock-anchor guard (NetworkClockAnchor) also edits the capture: before its
/// re-solve it removes the withdrawn satellites' clock priors from cap_priors (and their
/// kinds from cap_prior_kinds, in step) and
/// marginalizes them out of cap_dense_keys / cap_dense_cov (and, on a card SCL clone
/// epoch, removes their walk factors from cap_ccl_walk).
struct EpochWorkspace {
    Eigen::MatrixXd cap_H;
    Eigen::VectorXd cap_z;
    double cap_constraint_weight = 0.0;
    bool   cap_has_constraint = false;
    std::vector<ContributionKind> cap_row_kinds;   ///< One kind per row of cap_H (the W6 builder's row kinds; card NOM N2b)
    Eigen::MatrixXd cap_birth_H;          ///< Card GFC step 2's first-sighting family prior rows (see writer-site list)
    Eigen::VectorXd cap_birth_z;          ///< Their targets
    double          cap_birth_weight = 0.0;  ///< Their weight, 1/sigma^2
    Eigen::MatrixXd cap_sbc_H;   ///< SBC completion rows (see writer-site list)
    Eigen::VectorXd cap_sbc_z;   ///< SBC completion targets
    std::vector<NetworkEstimator::SharedPrior> cap_priors;
    std::vector<ContributionKind> cap_prior_kinds;   ///< One kind per entry of cap_priors: the kind its fold site named (card NOM N2b; see @ref networkCapturePriors)
    std::vector<NetworkEstimator::SharedPrior> cap_dense_keys;
    Eigen::MatrixXd cap_dense_cov;
    bool cap_ccl_on = false;                                       ///< card SCL: the joint fold below was captured (the replay folds it in place of cap_dense_*)
    std::vector<NetworkEstimator::SharedPrior> cap_ccl_keys;       ///< card SCL: the stand-in block's keys (the cloned clocks as their clones)
    Eigen::MatrixXd cap_ccl_cov;                                   ///< card SCL: the stand-in block's covariance
    std::vector<NetworkEstimator::CloneWalk> cap_ccl_walk;         ///< card SCL: the walk factors
    Eigen::MatrixXd cap_ccl_walkQ;                                 ///< card PFQ: the walks' joint covariance (empty: the per-walk form)
    std::vector<NetworkEstimator::SharedPrior> cap_ccl_idle;       ///< card SCL: the idle clones' isolated priors
    std::vector<NetworkEstimator::SharedPrior> cap_shx;            ///< card SCL2 X2: the history columns' N(0, 1) priors (filter/NetworkSharedHistory)
    /// Two-state clock-model predictions stashed by the prior fold
    /// (networkInjectClockOscPrior_), consumed by the post-solve normalized-
    /// innovation test (T5) later in the same epoch.
    std::map<std::string, ClockOscPred> clk_pred;
    /// Per-satellite folded a-priori clock-anchor information (Λ diagonal), populated
    /// only when the anchor-info trace is open and consumed by the B7 anchor-fraction
    /// diagnostic after the solve (B7, P1-4).
    std::map<std::string, double> clk_prior_info;
    /// The within-epoch RAIM tier's culprit sets (card WCF; the hand-off is described in
    /// network/qc/NetworkWithinEpochCulprits.hpp).  Written only by the T2 block of
    /// networkSolveGlue, which clears it when it starts and when it ends; read by the
    /// DD-carry rebuild of each of the block's re-solves (the driver's rebuild callback
    /// reads this member of the same workspace).  Empty at every other time, so the
    /// epoch's primary build withholds nothing.
    WithinEpochCulprits we_culprits;
};

/**
 * @brief Append a fold's priors to the RAIM-replay capture with the kind the fold named
 *        (card NOM step N2b).
 *
 * Appends @p priors to @p cap_priors and one @p kind per prior to @p cap_prior_kinds, so
 * the two stay in step (the replay names the kinds to its single re-fold of the captured
 * priors through @c NetworkEstimator::contributionPriorKinds).  The writers are listed in
 * the @ref EpochWorkspace contract.
 *
 * @param cap_priors      The capture (@c EpochWorkspace::cap_priors).
 * @param cap_prior_kinds Its kinds (@c EpochWorkspace::cap_prior_kinds).
 * @param priors          The priors the fold folded.
 * @param kind            The kind the fold site named for them.
 */
inline void networkCapturePriors(std::vector<NetworkEstimator::SharedPrior>& cap_priors,
                                 std::vector<ContributionKind>& cap_prior_kinds,
                                 const std::vector<NetworkEstimator::SharedPrior>& priors,
                                 ContributionKind kind) {
    cap_priors.insert(cap_priors.end(), priors.begin(), priors.end());
    cap_prior_kinds.insert(cap_prior_kinds.end(), priors.size(), kind);
}

/// Epoch-scratch candidate census consumed by every prioritized phase-bias datum
/// realization (eq34 chain, anchor election).  Built once per epoch by
/// @ref networkBuildPhaseDatumCandidates (network/solve/NetworkPhaseDatum.hpp) and passed
/// by const reference to whichever election the mode dispatch picks; nothing
/// downstream of the election reads it.
///
/// The member names are the run-TU local names they replaced, so the bodies
/// relocated at decomposition INC-8b alias them without renaming.
///
/// `union_keys` is post-gate: the network.phase_datum_min_stations coverage gate
/// erases from it in place inside the builder.  There is no pre-gate copy, and none
/// may be added: a pre-gate census would re-admit every one-tracker (sat, signal)
/// into shared_phase_keys, which the card 3 D3c RPB allocation reads directly (the
/// non-PD ejection, one-station re-level and split-leak class that the gate exists
/// to prevent).
struct PhaseDatumCandidates {
    /// Per station: satellite -> elevation (deg), from the pruned observations.
    std::vector<std::map<std::string, double>>      sat_el;
    /// Per station: "sat_signal" -> "sat|signal|PB" (the per-(sat, signal)
    /// phase-bias states this station allocated in PASS A: the anchorable keys).
    std::vector<std::map<std::string, std::string>> usable_keys;
    /// constellation -> station index -> # satellites tracked at >= pref_elev.
    std::map<char, std::map<std::size_t, int>>      cover;
    /// constellation -> candidate station indices (can anchor >= 1 of its keys).
    std::map<char, std::set<std::size_t>>           cand_set;
    /// "sat_signal" -> "sat|signal|PB", coverage-gated union over present stations.
    std::map<std::string, std::string>              union_keys;
    /// The "sat_signal" keys that the coverage gate excluded this epoch, i.e. exactly
    /// what was erased from @ref union_keys above (card EV7 §6).  Kept because the drop
    /// edge (network/solve/NetworkPhaseGateDrop.hpp) is the difference between this set and
    /// the previous epoch's anchor incumbents.  Empty when the gate is off
    /// (network.phase_datum_min_stations = 1) and whenever it excluded nothing.  No
    /// election decision reads it; only the drop-edge step at the end of the anchor
    /// election does.  The hygiene pass's own erasures from @ref union_keys are not in
    /// it, by design (a different mechanism with its own bridge lifecycle).
    std::set<std::string>                           gated_keys;
};

} // namespace GnssProcessor
